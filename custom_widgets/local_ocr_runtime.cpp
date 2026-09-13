#include "local_ocr_runtime.h"

#include "ocr_job_store.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

namespace LocalOcrRuntime {
namespace {
QString digest(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
class Inventory
{
public:
    LocalMetadata::Cancellation cancel;
    LocalMetadata::Progress progress;
    QElapsedTimer progressClock;
    QString stage;
    qint64 readBytes = 0;
    int completedFiles = 0;
    bool report(bool force = false)
    {
        if (!active())
            return false;
        if (progress && (force || !progressClock.isValid() || progressClock.elapsed() >= 250)) {
            progress(0, 0, QStringLiteral("%1 · 파일 %2개 · %3 MiB 읽음").arg(stage).arg(completedFiles).arg(readBytes / (1024 * 1024)));
            progressClock.start();
        }
        return active();
    }
    bool beginStage(const QString &text)
    {
        stage = text;
        return report(true);
    }
    QString error;
    int files = 0;
    qint64 bytes = 0;
    struct Stamp {
        QString path;
        bool directory;
        qint64 size;
        QDateTime modified;
    };
    QVector<Stamp> observed;
    bool active()
    {
        if (cancel && cancel->load())
            error = QStringLiteral("OCR runtime measurement cancelled.");
        return error.isEmpty();
    }
    bool reject(const QString &message)
    {
        error = message;
        return false;
    }
    bool ordinary(const QFileInfo &info)
    {
        return info.exists() && !info.isSymLink() && !info.isJunction();
    }
    QString fileHash(const QString &path)
    {
        if (!active())
            return { };
        const QFileInfo before(path);
        QFile file(path);
        if (!ordinary(before) || !before.isFile() || !file.open(QIODevice::ReadOnly)) {
            reject(QStringLiteral("Runtime file is missing, linked or unreadable."));
            return { };
        }
        if (++files > 100000 || before.size() < 0 || before.size() > 32LL * 1024 * 1024 * 1024 - bytes) {
            reject(QStringLiteral("Runtime inventory exceeds its file or byte bound."));
            return { };
        }
        bytes += before.size();
        const auto modified = before.lastModified();
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 read = 0;
        while (!file.atEnd()) {
            if (!active())
                return { };
            const auto block = file.read(1024 * 1024);
            if (block.isEmpty()) {
                reject(QStringLiteral("Runtime file could not be completely read."));
                return { };
            }
            read += block.size();
            readBytes += block.size();
            if (!report())
                return { };
            if (read > before.size()) {
                reject(QStringLiteral("Runtime file changed during measurement."));
                return { };
            }
            hash.addData(block);
        }
        const QFileInfo after(path);
        if (file.error() != QFileDevice::NoError || read != before.size() || !ordinary(after) || after.size() != before.size() || after.lastModified() != modified) {
            reject(QStringLiteral("Runtime file changed during measurement."));
            return { };
        }
        observed.append({ path, false, before.size(), modified });
        ++completedFiles;
        if (!report())
            return { };
        return QString::fromLatin1(hash.result().toHex());
    }
    bool stable()
    {
        for (const auto &before : observed) {
            if (!active())
                return false;
            const QFileInfo now(before.path);
            if (!ordinary(now) || now.isDir() != before.directory || now.size() != before.size || now.lastModified() != before.modified)
                return reject(QStringLiteral("Runtime changed before measurement completed."));
        }
        return true;
    }
    bool tree(const QString &root, QJsonArray &entries, const QString &relative = { }, int depth = 0, bool shared = false)
    {
        if (!active())
            return false;
        const QString path = relative.isEmpty() ? root : QDir(root).filePath(relative);
        const QFileInfo info(path);
        const QDir directory(path);
        if (depth > 48 || !ordinary(info) || !info.isDir() || !directory.isReadable())
            return reject(QStringLiteral("Runtime directory is missing, linked, unreadable or too deep."));
        observed.append({ path, true, info.size(), info.lastModified() });
        const auto children = directory.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
        for (const auto &child : children) {
            const QString name = child.fileName();
            if (shared && relative.isEmpty() && (name == QStringLiteral("ocr") || name == QStringLiteral("ocr-neural") || name == QStringLiteral("ocr-neural-gpu")))
                continue;
            if (!ordinary(child))
                return reject(QStringLiteral("Linked runtime entries are not supported."));
            const QString rel = relative.isEmpty() ? name : relative + u'/' + name;
            if (child.isDir()) {
                if (!tree(root, entries, rel, depth + 1, shared))
                    return false;
            } else if (child.isFile()) {
                const auto hash = fileHash(child.absoluteFilePath());
                if (hash.isEmpty())
                    return false;
                entries.append(QJsonObject { { "path", rel }, { "bytes", child.size() }, { "sha256", hash } });
            } else {
                return reject(QStringLiteral("Unsupported runtime filesystem entry."));
            }
        }
        return active();
    }
};
QJsonObject optionsObject(const LocalMetadata::OcrOptions &o)
{
    return { { "neural", o.neural }, { "gpu", o.gpu }, { "cpuThreads", o.cpuThreads }, { "executable", o.executable }, { "dataPath", o.dataPath }, { "language", o.language }, { "vertical", o.vertical }, { "segmentation", o.segmentation }, { "rotation", o.rotation }, { "invert", o.invert }, { "adaptiveThreshold", o.adaptiveThreshold }, { "timeoutMs", o.timeoutMs } };
}
}

std::optional<Measurement> measure(const QString &applicationPath, const LocalMetadata::OcrOptions &options,
                                   const LocalMetadata::Cancellation &cancel, QString *error, const LocalMetadata::Progress &progress)
{
    if (error)
        error->clear();
    Inventory inventory;
    inventory.cancel = cancel;
    inventory.progress = progress;
    auto failure = [&]() -> std::optional<Measurement> {
        if (error)
            *error = inventory.error.isEmpty() ? QStringLiteral("Invalid neural runtime settings or model inventory.") : inventory.error;
        return std::nullopt;
    };
    const QFileInfo application(applicationPath);
    if (!options.neural || !application.isAbsolute() || !inventory.ordinary(application))
        return failure();
    const QString root = application.absolutePath();
    const QString neural = QDir(root).filePath(QStringLiteral("ocr-neural"));
    const QString modelsRoot = QDir(neural).filePath(QStringLiteral("models"));
    if (!inventory.ordinary(QFileInfo(root)) || !inventory.ordinary(QFileInfo(neural)))
        return failure();
    if (!inventory.beginStage(QStringLiteral("OCR 실행 파일 확인 중")))
        return failure();
    const auto applicationHash = inventory.fileHash(applicationPath);
    const auto workerHash = inventory.fileHash(QDir(neural).filePath(QStringLiteral("worker.py")));
    if (applicationHash.isEmpty() || workerHash.isEmpty())
        return failure();
    QJsonArray models, shared, cpu;
    if (!inventory.beginStage(QStringLiteral("OCR 모델 확인 중")) || !inventory.tree(modelsRoot, models) || !inventory.beginStage(QStringLiteral("공용 실행 파일 확인 중")) || !inventory.tree(root, shared, { }, 0, true) || !inventory.beginStage(QStringLiteral("CPU 실행 환경 확인 중")) || !inventory.tree(QDir(neural).filePath(QStringLiteral("runtime")), cpu))
        return failure();
    QFile manifest(QDir(neural).filePath(QStringLiteral("models.json")));
    if (!inventory.ordinary(QFileInfo(manifest)) || !manifest.open(QIODevice::ReadOnly) || manifest.size() > 1024 * 1024)
        return failure();
    const auto manifestHash = inventory.fileHash(manifest.fileName());
    if (manifestHash.isEmpty())
        return failure();
    const auto declared = QJsonDocument::fromJson(manifest.read(1024 * 1024 + 1));
    if (!declared.isArray() || declared.array().size() != 9 || models.size() != 9)
        return failure();
    QSet<QString> required;
    for (const QString &model : { QStringLiteral("PP-OCRv5_mobile_det"), QStringLiteral("PP-OCRv5_server_rec"), QStringLiteral("korean_PP-OCRv5_mobile_rec") })
        for (const QString &file : { QStringLiteral("inference.json"), QStringLiteral("inference.pdiparams"), QStringLiteral("inference.yml") })
            required.insert(model + u'/' + file);
    QJsonObject actual;
    for (const auto item : models) {
        const auto object = item.toObject();
        const auto path = object["path"].toString();
        if (!required.remove(path))
            return failure();
        actual.insert(QStringLiteral("models/") + path, object["sha256"]);
    }
    if (!required.isEmpty())
        return failure();
    QSet<QString> seen;
    for (const auto item : declared.array()) {
        const auto entry = item.toObject();
        const auto path = entry["path"].toString();
        if (entry.size() != 2 || seen.contains(path) || !actual.contains(path) || actual[path] != entry["sha256"])
            return failure();
        seen.insert(path);
    }
    const auto modelsHash = digest(QJsonDocument(models).toJson(QJsonDocument::Compact));
    auto runtime = [&](const QString &path, const QJsonArray &files) {
        return QJsonObject { { "executable", QDir(path).filePath(QStringLiteral("python.exe")) }, { "dataPath", modelsRoot }, { "executableSha256", inventory.fileHash(QDir(path).filePath(QStringLiteral("python.exe"))) }, { "workerSha256", workerHash }, { "modelManifestSha256", modelsHash }, { "packageManifestSha256", digest(QJsonDocument(QJsonObject { { "runtime", files }, { "shared", shared } }).toJson(QJsonDocument::Compact)) } };
    };
    QJsonObject environment { { "platform", "windows-x64" }, { "applicationSha256", applicationHash }, { "preprocessingRevision", "decoded-gray-border-v1" }, { "cpu", runtime(QDir(neural).filePath(QStringLiteral("runtime")), cpu) }, { "gpu", QJsonValue::Null } };
    QJsonObject manifests { { "models", models }, { "shared", shared }, { "cpu", cpu }, { "gpu", QJsonValue::Null } };
    const QFileInfo gpuRoot(QDir(root).filePath(QStringLiteral("ocr-neural-gpu")));
    if (options.gpu && (gpuRoot.exists() || gpuRoot.isSymLink() || gpuRoot.isJunction())) {
        QJsonArray gpu;
        if (!inventory.beginStage(QStringLiteral("NVIDIA 실행 환경 확인 중")) || !inventory.ordinary(gpuRoot) || !inventory.tree(QDir(gpuRoot.absoluteFilePath()).filePath(QStringLiteral("runtime")), gpu))
            return failure();
        environment["gpu"] = runtime(QDir(gpuRoot.absoluteFilePath()).filePath(QStringLiteral("runtime")), gpu);
        manifests["gpu"] = gpu;
    }
    const QJsonObject snapshot { { "version", 1 }, { "options", optionsObject(options) }, { "environment", environment } };
    const auto fingerprint = OcrJobs::settingsFingerprint(snapshot);
    // No progress callback follows the final stability pass: callbacks can
    // cancel or mutate files, and must not invalidate an already checked stamp.
    if (!inventory.beginStage(QStringLiteral("실행 환경 변경 여부 최종 확인 중")) || !inventory.stable() || fingerprint.isEmpty())
        return failure();
    return Measurement { snapshot, fingerprint, manifests };
}
}