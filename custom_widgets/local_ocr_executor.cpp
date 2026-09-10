#include "local_ocr_executor.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>

namespace LocalOcrExecutor {
using namespace LocalMetadata;
namespace {
QString hash(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
OcrOptions optionsFrom(const QJsonObject &value)
{
    OcrOptions options;
    options.neural = value["neural"].toBool();
    options.gpu = value["gpu"].toBool();
    options.cpuThreads = value["cpuThreads"].toInt();
    options.executable = value["executable"].toString();
    options.dataPath = value["dataPath"].toString();
    options.language = value["language"].toString();
    options.vertical = value["vertical"].toBool();
    options.segmentation = value["segmentation"].toInt();
    options.rotation = value["rotation"].toInt();
    options.invert = value["invert"].toBool();
    options.adaptiveThreshold = value["adaptiveThreshold"].toBool();
    options.timeoutMs = value["timeoutMs"].toInt();
    return options;
}
bool sameInput(const NeuralPageEvidence &a, const NeuralPageEvidence &b)
{
    return a.imageSha256 == b.imageSha256 && a.geometry.inputSize == b.geometry.inputSize && a.geometry.preparedSize == b.geometry.preparedSize && a.geometry.revision == b.geometry.revision && a.geometry.inputToPrepared == b.geometry.inputToPrepared;
}
}

Outcome executeClaimed(OcrJobs::Store &store, const OcrJobs::Lease &lease,
                       const LocalOcrSource::Snapshot &source, const LocalOcrRuntime::Measurement &runtime,
                       const QString &cacheRoot, const Cancellation &cancel, const Progress &progress,
                       const LocalOcrPersistence::Clock &clock, const Runner &runner)
{
    Outcome out;
    const auto flag = cancel ? cancel : std::make_shared<std::atomic_bool>(false);
    const LocalOcrPersistence::Clock now = clock ? clock : LocalOcrPersistence::Clock([] { return QDateTime::currentMSecsSinceEpoch(); });
    const auto job = store.get(lease.jobId);
    if (!job) {
        out.batch.error = store.lastError().isEmpty() ? QStringLiteral("OCR job not found.") : store.lastError();
        return out;
    }
    const int count = job->spec.pages.size();
    out.metadata = source.source;
    out.batch.readings.resize(count);
    out.batch.evidence.resize(count);
    out.batch.validPages.fill(false, count);
    out.cacheHits.fill(false, count);
    auto finish = [&]() {
        for (int i = 0; i < count && i < out.metadata.pages.size(); ++i) {
            auto &page = out.metadata.pages[i];
            page.reading = out.batch.validPages.at(i) ? out.batch.readings.at(i) : Reading { };
            if (!out.batch.validPages.at(i))
                page.reading.error = out.batch.readings.at(i).error.isEmpty() ? out.batch.error : out.batch.readings.at(i).error;
            page.text = page.reading.text;
            page.error = page.reading.error;
            page.kind = classifyPage(page.text, page.number);
        }
        out.metadata.suggestions = suggest(out.metadata.pages, job->spec.sourceContext["path"].toString(), job->spec.sourceContext["libraryRoot"].toString());
        if (out.batch.status == RecognitionStatus::Complete) {
            bool pageCandidates = false;
            for (const auto &suggestion : out.metadata.suggestions)
                pageCandidates = pageCandidates || suggestion.page > 0;
            out.stateSaved = store.finishWhenCurrent(lease, pageCandidates, now);
            if (!out.stateSaved) {
                out.batch.status = RecognitionStatus::DeliveryFailed;
                out.batch.error = store.lastError();
            }
        } else if (out.batch.status == RecognitionStatus::Cancelled) {
            out.stateSaved = store.pauseWhenCurrent(lease, now);
        } else {
            out.stateSaved = store.failWhenCurrent(lease, out.batch.error.left(16384), now);
        }
        if (!out.stateSaved)
            out.stateError = store.lastError();
        const auto current = store.get(lease.jobId);
        if (current)
            out.state = current->state;
        out.metadata.error = out.batch.error;
        return out;
    };
    const auto fail = [&](const QString &error, RecognitionStatus status = RecognitionStatus::Failed) {
        out.batch.error = error;
        out.batch.status = status;
        return finish();
    };
    if (flag->load())
        return fail(QStringLiteral("Cancelled"), RecognitionStatus::Cancelled);
    if (job->state != OcrJobs::State::Running || !store.heartbeatWhenCurrent(lease, 300000, now))
        return fail(store.lastError().isEmpty() ? QStringLiteral("OCR job is not owned by this executor.") : store.lastError());
    if (source.fingerprint.isEmpty() || source.fingerprint != hash(QJsonDocument(source.manifest).toJson(QJsonDocument::Compact)) || source.fingerprint != job->spec.sourceSnapshot || source.source.pageCount != job->spec.totalPages || source.source.pages.size() != count || !source.source.error.isEmpty() || source.manifest["path"] != job->spec.sourceContext["path"] || source.manifest["sourceKind"] != job->spec.sourceContext["sourceKind"] || runtime.settingsFingerprint.isEmpty() || runtime.settingsFingerprint != OcrJobs::settingsFingerprint(runtime.settingsSnapshot) || runtime.settingsFingerprint != job->spec.settingsFingerprint || runtime.settingsSnapshot != job->spec.settingsSnapshot)
        return fail(QStringLiteral("Source or runtime changed; keep the old work for review."));
    const auto manifestPages = source.manifest["selectedPages"].toArray();
    const auto manifestNames = source.manifest["pageNames"].toArray();
    const int perEnd = source.manifest["perEnd"].toInt();
    if (perEnd < 1 || perEnd > 3 || manifestPages.size() != count || manifestNames.size() != source.source.pageNames.size())
        return fail(QStringLiteral("Invalid selected source plan."));
    const auto indexes = sampleIndexes(source.source.pageCount, perEnd);
    if (indexes.size() != count)
        return fail(QStringLiteral("Selected source plan differs from the job."));
    for (int i = 0; i < manifestNames.size(); ++i)
        if (manifestNames[i].toString() != source.source.pageNames.at(i))
            return fail(QStringLiteral("Source page order changed."));
    const auto options = optionsFrom(runtime.settingsSnapshot["options"].toObject());
    if (!options.neural)
        return fail(QStringLiteral("Durable page execution requires neural OCR."));
    QVector<NeuralPageEvidence> inputs;
    QVector<int> missing;
    QVector<QImage> images;
    QElapsedTimer cacheTimer;
    cacheTimer.start();
    for (int i = 0; i < count; ++i) {
        if (flag->load())
            return fail(QStringLiteral("Cancelled"), RecognitionStatus::Cancelled);
        const auto &page = source.source.pages.at(i);
        const auto manifestPage = manifestPages[i].toObject();
        if (page.number != job->spec.pages.at(i) || page.number != indexes.at(i) + 1 || page.number != manifestPage["number"].toInt() || page.name != manifestPage["name"].toString() || page.sourceSha256 != manifestPage["rawSha256"].toString() || !page.error.isEmpty() || page.image.isNull())
            return fail(QStringLiteral("Selected source page changed or is invalid."));
        const auto prepared = prepareOcrPage(page.image, options);
        QByteArray png;
        QBuffer buffer(&png);
        if (!validOcrGeometry(prepared.geometry) || !buffer.open(QIODevice::WriteOnly) || !prepared.image.save(&buffer, "PNG"))
            return fail(QStringLiteral("Could not prepare selected OCR page."));
        inputs.append({ i, prepared.geometry, hash(png), { }, { }, { }, { } });
        bool recorded = false;
        for (const auto &receipt : job->pages)
            recorded = recorded || receipt.page == page.number;
        if (recorded) {
            QString error;
            const auto restored = LocalOcrPersistence::restorePage(*job, runtime, i, prepared.geometry, inputs.last().imageSha256, cacheRoot, &error, flag);
            if (!restored)
                return fail(error, flag->load() ? RecognitionStatus::Cancelled : RecognitionStatus::Failed);
            out.batch.evidence[i] = *restored;
            out.batch.readings[i] = parseNeuralReading(restored->rawResult, restored->geometry.preparedSize);
            out.batch.validPages[i] = true;
            out.cacheHits[i] = true;
            ++out.cachedPages;
        } else {
            missing.append(i);
            images.append(page.image);
        }
    }
    out.cacheReadMs = cacheTimer.elapsed();
    if (missing.isEmpty())
        return fail(QStringLiteral("Saved pages alone cannot prove successful execution; review the previous session."));
    QString ownershipError;
    const Progress pulse = [&](int completed, int, const QString &stage) {
        if (!flag->load() && !store.heartbeatWhenCurrent(lease, 300000, now)) {
            ownershipError = store.lastError();
            flag->store(true);
        }
        if (progress)
            progress(out.cachedPages + qBound(0, completed, int(missing.size())), count, stage);
    };
    QVector<bool> accepted(count, false);
    QString deliveryError;
    const PageSink persist = [&](const NeuralPageEvidence &page, QString *error) {
        const auto reject = [&](const QString &reason) {
            deliveryError = reason;
            *error = reason;
            return false;
        };
        if (!deliveryError.isEmpty())
            return reject(deliveryError);
        if (flag->load()) {
            *error = QStringLiteral("Cancelled");
            return false;
        }
        if (page.selectedIndex < 0 || page.selectedIndex >= missing.size() || !validNeuralEvidence(page)) {
            return reject(QStringLiteral("Invalid page from the selected OCR execution."));
        }
        auto mapped = page;
        mapped.selectedIndex = missing.at(page.selectedIndex);
        if (accepted.at(mapped.selectedIndex) || !sameInput(mapped, inputs.at(mapped.selectedIndex))) {
            return reject(QStringLiteral("Duplicate or changed prepared page delivery."));
        }
        const auto saved = LocalOcrPersistence::recordPage(store, lease, runtime, mapped, cacheRoot, flag, now);
        if (!saved.receiptSaved) {
            if (flag->load()) {
                *error = saved.error;
                return false;
            }
            return reject(saved.error.isEmpty() ? QStringLiteral("Page persistence failed.") : saved.error);
        }
        accepted[mapped.selectedIndex] = true;
        return true;
    };
    RecognitionBatch result;
    try {
        pulse(0, images.size(), QStringLiteral("Validated saved pages; reading unfinished pages."));
        if (flag->load())
            return fail(ownershipError.isEmpty() ? QStringLiteral("Cancelled") : ownershipError,
                        ownershipError.isEmpty() ? RecognitionStatus::Cancelled : RecognitionStatus::Failed);
        result = runner ? runner(images, options, flag, pulse, persist) : recognizePagesWithOutcome(images, options, flag, pulse, persist);
    } catch (...) {
        return fail(QStringLiteral("OCR execution callback failed; process cleanup must be verified."), RecognitionStatus::CleanupFailed);
    }
    if (result.readings.size() != missing.size() || result.validPages.size() != missing.size() || result.evidence.size() != missing.size())
        return fail(QStringLiteral("Invalid OCR completion record."), result.status == RecognitionStatus::CleanupFailed ? RecognitionStatus::CleanupFailed : RecognitionStatus::Failed);
    out.batch.status = result.status;
    out.batch.error = result.error;
    out.batch.gpuStarted = result.gpuStarted;
    out.batch.attemptErrors = result.attemptErrors;
    for (int i = 0; i < missing.size(); ++i) {
        const int original = missing.at(i);
        out.batch.readings[original] = result.readings.at(i);
        out.batch.evidence[original] = result.evidence.at(i);
        if (out.batch.evidence.at(original))
            out.batch.evidence[original]->selectedIndex = original;
        out.batch.validPages[original] = result.validPages.at(i) && out.batch.evidence.at(original).has_value() && validNeuralEvidence(*out.batch.evidence.at(original)) && sameInput(*out.batch.evidence.at(original), inputs.at(original));
        if (out.batch.validPages.at(original)) {
            // Candidate text comes from the validated raw response, never a
            // divergent presentation string supplied by a runner.
            const auto &page = *out.batch.evidence.at(original);
            out.batch.readings[original] = parseNeuralReading(page.rawResult, page.geometry.preparedSize);
            out.batch.readings[original].warning = result.readings.at(i).warning;
        }
        if (out.batch.status == RecognitionStatus::Complete && (!out.batch.validPages.at(original) || !accepted.at(original))) {
            out.batch.status = RecognitionStatus::DeliveryFailed;
            out.batch.error = QStringLiteral("A completed page was not durably accepted.");
        }
    }
    if (!deliveryError.isEmpty() && out.batch.status != RecognitionStatus::CleanupFailed) {
        out.batch.status = RecognitionStatus::DeliveryFailed;
        out.batch.error = deliveryError;
    }
    if (!ownershipError.isEmpty() && out.batch.status != RecognitionStatus::CleanupFailed) {
        out.batch.status = RecognitionStatus::Failed;
        out.batch.error = ownershipError;
    } else if (flag->load() && out.batch.status != RecognitionStatus::CleanupFailed && out.batch.status != RecognitionStatus::DeliveryFailed) {
        out.batch.status = RecognitionStatus::Cancelled;
        out.batch.error = QStringLiteral("Cancelled");
    }
    if (out.batch.status == RecognitionStatus::Complete && !out.batch.error.isEmpty())
        out.batch.status = RecognitionStatus::Failed;
    if (out.batch.status != RecognitionStatus::Complete && out.batch.error.isEmpty())
        out.batch.error = QStringLiteral("OCR execution did not complete.");
    return finish();
}
}
