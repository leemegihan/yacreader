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
#include <QScopeGuard>
#include <QSet>
#include <QThread>
#include <QThreadPool>

#include <atomic>
#include <mutex>
#include <vector>

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
    std::atomic_bool *stopped = nullptr;
    std::atomic<qint64> *parallelBytes = nullptr;
    int maximumReaders = 16;
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
        if (stopped && stopped->load())
            return false;
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
            if (parallelBytes)
                parallelBytes->fetch_add(block.size());
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
    struct PendingFile {
        QString path;
        QString relative;
        qint64 size;
        QDateTime modified;
    };
    bool collect(const QString &root, std::vector<PendingFile> &pending, const QString &relative = { }, int depth = 0, bool shared = false)
    {
        if (!report())
            return false;
        const QString path = relative.isEmpty() ? root : QDir(root).filePath(relative);
        const QFileInfo info(path);
        const QDir directory(path);
        if (depth > 48 || !ordinary(info) || !info.isDir() || !directory.isReadable())
            return reject(QStringLiteral("Runtime directory is missing, linked, unreadable or too deep."));
        observed.append({ path, true, info.size(), info.lastModified() });
        const auto children = directory.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
        for (const auto &child : children) {
            if (!active())
                return false;
            const QString name = child.fileName();
            if (shared && relative.isEmpty() && (name == QStringLiteral("ocr") || name == QStringLiteral("ocr-neural") || name == QStringLiteral("ocr-neural-gpu")))
                continue;
            if (!ordinary(child))
                return reject(QStringLiteral("Linked runtime entries are not supported."));
            const QString rel = relative.isEmpty() ? name : relative + u'/' + name;
            if (child.isDir()) {
                if (!collect(root, pending, rel, depth + 1, shared))
                    return false;
            } else if (child.isFile()) {
                if (++files > 100000 || child.size() < 0 || child.size() > 32LL * 1024 * 1024 * 1024 - bytes)
                    return reject(QStringLiteral("Runtime inventory exceeds its file or byte bound."));
                bytes += child.size();
                pending.push_back({ child.absoluteFilePath(), rel, child.size(), child.lastModified() });
            } else {
                return reject(QStringLiteral("Unsupported runtime filesystem entry."));
            }
        }
        return active();
    }
    bool tree(const QString &root, QJsonArray &entries, const QString &relative = { }, int depth = 0, bool shared = false)
    {
        std::vector<PendingFile> pending;
        if (!collect(root, pending, relative, depth, shared))
            return false;
        if (pending.empty())
            return active();
        struct Result {
            QString hash;
            Stamp stamp;
        };
        std::vector<Result> results(pending.size());
        std::atomic_size_t next { 0 };
        std::atomic_bool stop { false };
        std::atomic<qint64> consumed { 0 };
        std::atomic_int completed { 0 };
        std::mutex failureMutex;
        QString workerError;
        // The pool belongs to this measurement. Joining also happens on error
        // or callback unwinding, before any referenced state is destroyed.
        QThreadPool pool;
        pool.setMaxThreadCount(qMin(qBound(1, maximumReaders, 16), qMax(1, QThread::idealThreadCount())));
        const auto join = qScopeGuard([&] { stop.store(true); pool.waitForDone(); });
        const int readerCount = qMin(pool.maxThreadCount(), int(pending.size()));
        for (int reader = 0; reader < readerCount; ++reader) {
            pool.start([&] {
                while (!stop.load()) {
                    const auto index = next.fetch_add(1);
                    if (index >= pending.size())
                        break;
                    const auto &file = pending[index];
                    Inventory leaf;
                    leaf.cancel = cancel;
                    leaf.stopped = &stop;
                    leaf.parallelBytes = &consumed;
                    const auto hash = leaf.fileHash(file.path);
                    if (hash.isEmpty() || leaf.observed.size() != 1 || leaf.observed[0].size != file.size || leaf.observed[0].modified != file.modified) {
                        std::lock_guard<std::mutex> lock(failureMutex);
                        if (!stop.load()) {
                            workerError = leaf.error.isEmpty() ? QStringLiteral("Runtime file changed during measurement.") : leaf.error;
                            stop.store(true);
                        }
                        break;
                    }
                    results[index] = { hash, leaf.observed[0] };
                    completed.fetch_add(1);
                }
            });
        }
        const auto initialBytes = readBytes;
        const auto initialFiles = completedFiles;
        do {
            readBytes = initialBytes + consumed.load();
            completedFiles = initialFiles + completed.load();
            if (!report())
                stop.store(true);
        } while (!pool.waitForDone(50));
        readBytes = initialBytes + consumed.load();
        completedFiles = initialFiles + completed.load();
        if (!active())
            return false;
        if (!workerError.isEmpty())
            return reject(workerError);
        if (completed.load() != int(pending.size()))
            return reject(QStringLiteral("Runtime inventory did not finish."));
        // Workers finish out of order; the manifest retains QDir traversal order.
        for (size_t i = 0; i < pending.size(); ++i) {
            observed.append(results[i].stamp);
            entries.append(QJsonObject { { "path", pending[i].relative }, { "bytes", pending[i].size }, { "sha256", results[i].hash } });
        }
        return report();
    }
};
QJsonObject optionsObject(const LocalMetadata::OcrOptions &o)
{
    return { { "neural", o.neural }, { "gpu", o.gpu }, { "cpuThreads", o.cpuThreads }, { "executable", o.executable }, { "dataPath", o.dataPath }, { "language", o.language }, { "vertical", o.vertical }, { "segmentation", o.segmentation }, { "rotation", o.rotation }, { "invert", o.invert }, { "adaptiveThreshold", o.adaptiveThreshold }, { "timeoutMs", o.timeoutMs } };
}
}

std::optional<Measurement> measure(const QString &applicationPath, const LocalMetadata::OcrOptions &options,
                                   const LocalMetadata::Cancellation &cancel, QString *error, const LocalMetadata::Progress &progress, int maximumReaders)
{
    if (error)
        error->clear();
    Inventory inventory;
    inventory.cancel = cancel;
    inventory.progress = progress;
    inventory.maximumReaders = maximumReaders;
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