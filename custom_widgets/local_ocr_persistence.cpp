#include "local_ocr_persistence.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>

namespace LocalOcrPersistence {
namespace {
std::optional<LocalOcrCache::Identity> inputIdentity(const LocalOcrRuntime::Measurement &measurement,
                                                     const LocalMetadata::NeuralPageEvidence &page, QString *error)
{
    if (error)
        error->clear();
    auto invalid = [&]() -> std::optional<LocalOcrCache::Identity> {
        if (error)
            *error = QStringLiteral("Page evidence does not match the complete neural settings.");
        return std::nullopt;
    };
    if (measurement.settingsFingerprint.isEmpty() || OcrJobs::settingsFingerprint(measurement.settingsSnapshot) != measurement.settingsFingerprint || !LocalMetadata::validOcrGeometry(page.geometry))
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
    if (LocalOcrCache::key(result).isEmpty())
        return invalid();
    return result;
}
}

std::optional<LocalOcrCache::Identity> identity(const LocalOcrRuntime::Measurement &measurement,
                                                const LocalMetadata::NeuralPageEvidence &page, QString *error)
{
    const auto result = inputIdentity(measurement, page, error);
    if (!result)
        return std::nullopt;
    if (!LocalMetadata::validNeuralEvidence(page) || LocalMetadata::parseNeuralReading(page.rawResult, page.geometry.preparedSize).language != result->language) {
        if (error)
            *error = QStringLiteral("Invalid neural page response or language.");
        return std::nullopt;
    }
    return result;
}

std::optional<LocalMetadata::NeuralPageEvidence> restorePage(const OcrJobs::Job &job,
                                                             const LocalOcrRuntime::Measurement &measurement, int selectedIndex,
                                                             const LocalMetadata::OcrGeometry &geometry, const QString &imageSha256,
                                                             const QString &cacheRoot, QString *error, const LocalMetadata::Cancellation &cancel)
{
    if (error)
        error->clear();
    const auto invalid = [&](const QString &reason) -> std::optional<LocalMetadata::NeuralPageEvidence> {
        if (error)
            *error = reason;
        return std::nullopt;
    };
    if (cancel && cancel->load())
        return invalid(QStringLiteral("Cache recovery cancelled."));
    if (selectedIndex < 0 || selectedIndex >= job.spec.pages.size() || job.spec.settingsFingerprint != measurement.settingsFingerprint || job.spec.settingsSnapshot != measurement.settingsSnapshot)
        return invalid(QStringLiteral("Cached page is outside the revalidated settings or selected plan."));
    std::optional<OcrJobs::PageReceipt> receipt;
    for (const auto &saved : job.pages) {
        if (saved.page == job.spec.pages.at(selectedIndex)) {
            if (receipt)
                return invalid(QStringLiteral("Duplicate durable page receipt."));
            receipt = saved;
        }
    }
    if (!receipt)
        return invalid(QStringLiteral("No durable receipt for this selected page."));
    // A CPU response can have come from the CPU package or a GPU package that
    // selected CPU internally. Match the committed key, never guess its origin.
    std::optional<LocalOcrCache::Identity> matched;
    for (const QString requested : { "cpu", "gpu:0" }) {
        const LocalMetadata::NeuralPageEvidence input { selectedIndex, geometry, imageSha256, { }, { }, requested, receipt->actualDevice };
        const auto candidate = inputIdentity(measurement, input, nullptr);
        if (candidate && LocalOcrCache::key(*candidate) == receipt->cacheKey) {
            if (matched)
                return invalid(QStringLiteral("Ambiguous durable page identity."));
            matched = candidate;
        }
    }
    if (!matched)
        return invalid(QStringLiteral("Cached page does not match the prepared input, environment or device."));
    const auto loaded = LocalOcrCache::load(cacheRoot, *matched, error);
    if (!loaded)
        return std::nullopt;
    if (cancel && cancel->load())
        return invalid(QStringLiteral("Cache recovery cancelled."));
    if (loaded->resultSha256 != receipt->resultSha256)
        return invalid(QStringLiteral("Cached response differs from the durable receipt."));
    LocalMetadata::NeuralPageEvidence result { selectedIndex, geometry, imageSha256, loaded->rawResult,
                                               loaded->resultSha256, matched->requestedDevice, matched->actualDevice };
    if (!LocalMetadata::validNeuralEvidence(result))
        return invalid(QStringLiteral("Invalid recovered neural page evidence."));
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