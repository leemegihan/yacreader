#include "local_ocr_cache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>

namespace LocalOcrCache {
namespace {
constexpr qint64 maxResultSize = 4 * 1024 * 1024;
constexpr qint64 maxEntrySize = 6 * 1024 * 1024;

void failure(QString *error, const QString &message)
{
    if (error)
        *error = message;
}
bool hash(const QString &value)
{
    if (value.size() != 64)
        return false;
    for (const QChar c : value)
        if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f')))
            return false;
    return true;
}
QString digest(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
bool device(const QString &name)
{
    return name == QStringLiteral("cpu") || name == QStringLiteral("gpu:0");
}
QJsonObject metadata(const Identity &i)
{
    return { { "version", 1 }, { "imageSha256", i.imageSha256 }, { "width", i.preparedSize.width() }, { "height", i.preparedSize.height() }, { "preprocessingFingerprint", i.preprocessingFingerprint }, { "workerSha256", i.workerSha256 }, { "modelManifestSha256", i.modelManifestSha256 }, { "packageManifestSha256", i.packageManifestSha256 }, { "language", i.language }, { "cpuThreads", i.cpuThreads }, { "requestedDevice", i.requestedDevice }, { "actualDevice", i.actualDevice } };
}
bool valid(const Identity &i)
{
    return hash(i.imageSha256) && hash(i.preprocessingFingerprint) && hash(i.workerSha256) && hash(i.modelManifestSha256) && hash(i.packageManifestSha256) && i.preparedSize.width() > 0 && i.preparedSize.height() > 0 && qint64(i.preparedSize.width()) * i.preparedSize.height() <= 17000000 && (i.language == QStringLiteral("auto") || i.language == QStringLiteral("kor") || i.language == QStringLiteral("jpn")) && i.cpuThreads >= 1 && i.cpuThreads <= 16 && device(i.requestedDevice) && device(i.actualDevice) && (i.requestedDevice != QStringLiteral("cpu") || i.actualDevice == QStringLiteral("cpu"));
}
QString path(const QString &root, const Identity &i, QString *error)
{
    failure(error, { });
    const QFileInfo directory(root);
    if (!directory.isAbsolute() || !directory.isDir() || directory.isSymLink() || !valid(i)) {
        failure(error, QStringLiteral("Invalid private cache directory or complete OCR identity."));
        return { };
    }
    const QString result = QDir(directory.canonicalFilePath()).filePath(key(i) + QStringLiteral(".json"));
    if (QFileInfo(result).isSymLink()) {
        failure(error, QStringLiteral("Refusing a linked cache entry."));
        return { };
    }
    return result;
}
std::optional<Entry> validate(const Identity &i, const QByteArray &bytes, QString *error)
{
    if (bytes.isEmpty() || bytes.size() > maxResultSize) {
        failure(error, QStringLiteral("Invalid OCR result size."));
        return { };
    }
    const auto reading = LocalMetadata::parseNeuralReading(bytes, i.preparedSize);
    if (!reading.error.isEmpty() || reading.device != i.actualDevice || reading.language != i.language) {
        failure(error, QStringLiteral("OCR response, language or actual device does not match the cache identity."));
        return { };
    }
    return Entry { reading, bytes, digest(bytes) };
}
}

QString key(const Identity &i)
{
    return valid(i) ? digest(QJsonDocument(metadata(i)).toJson(QJsonDocument::Compact)) : QString();
}

std::optional<Entry> load(const QString &root, const Identity &i, QString *error)
{
    const auto filename = path(root, i, error);
    if (filename.isEmpty())
        return { };
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) {
        failure(error, QStringLiteral("OCR cache entry is missing or unreadable."));
        return { };
    }
    if (file.size() > maxEntrySize) {
        failure(error, QStringLiteral("OCR cache entry exceeds its size limit."));
        return { };
    }
    const auto document = QJsonDocument::fromJson(file.read(maxEntrySize + 1));
    const auto object = document.object();
    if (!document.isObject() || object.value("version").toInt() != 1 || object.value("identity").toObject() != metadata(i) || !object.value("result").isString()) {
        failure(error, QStringLiteral("Unsupported or mismatched OCR cache envelope."));
        return { };
    }
    const auto encoded = object.value("result").toString().toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.toBase64() != encoded || digest(decoded.decoded) != object.value("resultSha256").toString()) {
        failure(error, QStringLiteral("OCR cache result integrity check failed."));
        return { };
    }
    return validate(i, decoded.decoded, error);
}

bool save(const QString &root, const Identity &i, const QByteArray &result, QString *error)
{
    const auto filename = path(root, i, error);
    if (filename.isEmpty())
        return false;
    const auto entry = validate(i, result, error);
    if (!entry)
        return false;
    QLockFile lock(filename + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(5000)) {
        failure(error, QStringLiteral("OCR cache entry is locked."));
        return false;
    }
    if (QFileInfo::exists(filename)) {
        const auto existing = load(root, i, error);
        if (!existing)
            return false;
        if (existing->rawResult == result)
            return true;
        failure(error, QStringLiteral("Refusing to replace committed OCR evidence."));
        return false;
    }
    const auto bytes = QJsonDocument(QJsonObject { { "version", 1 }, { "identity", metadata(i) }, { "resultSha256", entry->resultSha256 }, { "result", QString::fromLatin1(result.toBase64()) } })
                               .toJson(QJsonDocument::Compact);
    QSaveFile output(filename);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
        failure(error, QStringLiteral("Could not atomically commit OCR cache entry."));
        return false;
    }
    return true;
}
}
