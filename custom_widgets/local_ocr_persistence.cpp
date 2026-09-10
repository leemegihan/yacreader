#include "local_ocr_persistence.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>

namespace LocalOcrPersistence {
std::optional<LocalOcrCache::Identity> identity(const LocalOcrRuntime::Measurement &measurement,
                                                const LocalMetadata::NeuralPageEvidence &page, QString *error)
{
    if (error)
        error->clear();
    auto invalid = [&]() -> std::optional<LocalOcrCache::Identity> {
        if (error)
            *error = QStringLiteral("Page evidence does not match the complete neural settings.");
        return std::nullopt;
    };
    if (measurement.settingsFingerprint.isEmpty() || OcrJobs::settingsFingerprint(measurement.settingsSnapshot) != measurement.settingsFingerprint || !LocalMetadata::validNeuralEvidence(page))
        return invalid();
    const auto options = measurement.settingsSnapshot["options"].toObject();
    const auto environment = measurement.settingsSnapshot["environment"].toObject();
    if (!options["neural"].toBool() || environment["preprocessingRevision"].toString() != page.geometry.revision || (!options["gpu"].toBool() && page.requestedDevice != QStringLiteral("cpu")))
        return invalid();
    const auto runtimeValue = environment[page.requestedDevice == QStringLiteral("gpu:0") ? "gpu" : "cpu"];
    if (!runtimeValue.isObject())
        return invalid();
    const auto runtime = runtimeValue.toObject();
    const auto &m = page.geometry.inputToPrepared;
    const QJsonObject preprocessing {
        { "revision", page.geometry.revision }, { "inputWidth", page.geometry.inputSize.width() }, { "inputHeight", page.geometry.inputSize.height() }, { "preparedWidth", page.geometry.preparedSize.width() }, { "preparedHeight", page.geometry.preparedSize.height() }, { "matrix", QJsonArray { m.m11(), m.m12(), m.m13(), m.m21(), m.m22(), m.m23(), m.m31(), m.m32(), m.m33() } }, { "rotation", options["rotation"] }, { "invert", options["invert"] }
    };
    const auto preparationHash = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(preprocessing).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    const auto language = options["language"].toString();
    LocalOcrCache::Identity result { page.imageSha256, page.geometry.preparedSize, preparationHash,
                                     runtime["workerSha256"].toString(), runtime["modelManifestSha256"].toString(), runtime["packageManifestSha256"].toString(),
                                     language == QStringLiteral("auto") ? QStringLiteral("auto") : language.startsWith(QStringLiteral("kor")) ? QStringLiteral("kor")
                                                                                                                                              : QStringLiteral("jpn"),
                                     options["cpuThreads"].toInt(), page.requestedDevice, page.actualDevice };
    if (LocalOcrCache::key(result).isEmpty() || LocalMetadata::parseNeuralReading(page.rawResult, page.geometry.preparedSize).language != result.language)
        return invalid();
    return result;
}

Result recordPage(OcrJobs::Store &store, const OcrJobs::Lease &lease,
                  const LocalOcrRuntime::Measurement &measurement, const LocalMetadata::NeuralPageEvidence &page,
                  const QString &cacheRoot, const LocalMetadata::Cancellation &cancel, const Clock &clock)
{
    Result result;
    auto cancelled = [&] {
        if (cancel && cancel->load()) {
            result.error = QStringLiteral("Page persistence cancelled.");
            return true;
        }
        return false;
    };
    if (cancelled())
        return result;
    const auto key = identity(measurement, page, &result.error);
    if (!key)
        return result;
    const auto job = store.get(lease.jobId);
    if (!job) {
        result.error = store.lastError();
        if (result.error.isEmpty())
            result.error = QStringLiteral("OCR job was not found.");
        return result;
    }
    if (job->state != OcrJobs::State::Running || job->spec.settingsSnapshot != measurement.settingsSnapshot || job->spec.settingsFingerprint != measurement.settingsFingerprint || page.selectedIndex >= job->spec.pages.size()) {
        result.error = QStringLiteral("Page is outside the active job settings or selected plan.");
        return result;
    }
    result.cacheKey = LocalOcrCache::key(*key);
    const OcrJobs::PageReceipt receipt { job->spec.pages.at(page.selectedIndex), result.cacheKey, page.resultSha256, page.actualDevice };
    for (const auto &saved : job->pages)
        if (saved.page == receipt.page && (saved.cacheKey != receipt.cacheKey || saved.resultSha256 != receipt.resultSha256 || saved.actualDevice != receipt.actualDevice)) {
            result.error = QStringLiteral("Refusing to replace committed page evidence.");
            return result;
        }
    if (cancelled() || !LocalOcrCache::save(cacheRoot, *key, page.rawResult, &result.error))
        return result;
    result.cacheSaved = true;
    if (cancelled())
        return result;
    // Sample after cache I/O AND the DB write lock, including any SQLite wait.
    result.receiptSaved = store.recordPageWhenCurrent(lease, receipt, [&] {
        const qint64 now = clock ? clock() : QDateTime::currentMSecsSinceEpoch();
        return cancel && cancel->load() ? qint64(-1) : now;
    });
    if (!result.receiptSaved && !cancelled())
        result.error = store.lastError();
    return result;
}
}