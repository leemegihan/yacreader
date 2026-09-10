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
std::optional<QVector<NeuralPageEvidence>> selectedInputs(const OcrJobs::Job &job,
                                                          const LocalOcrSource::Snapshot &source,
                                                          const LocalOcrRuntime::Measurement &runtime,
                                                          const Cancellation &flag, QString *error)
{
    const auto invalid = [&](const QString &reason) -> std::optional<QVector<NeuralPageEvidence>> {
        if (error)
            *error = reason;
        return std::nullopt;
    };
    const int count = job.spec.pages.size();
    if (count < 1 || count > 6)
        return invalid(QStringLiteral("Invalid selected page count."));
    if (source.fingerprint.isEmpty() || source.fingerprint != hash(QJsonDocument(source.manifest).toJson(QJsonDocument::Compact)) || source.fingerprint != job.spec.sourceSnapshot || source.source.pageCount != job.spec.totalPages || source.source.pages.size() != count || !source.source.error.isEmpty() || source.manifest["path"] != job.spec.sourceContext["path"] || source.manifest["sourceKind"] != job.spec.sourceContext["sourceKind"] || runtime.settingsFingerprint.isEmpty() || runtime.settingsFingerprint != OcrJobs::settingsFingerprint(runtime.settingsSnapshot) || runtime.settingsFingerprint != job.spec.settingsFingerprint || runtime.settingsSnapshot != job.spec.settingsSnapshot)
        return invalid(QStringLiteral("Source or runtime changed; keep the old work for review."));
    const auto manifestPages = source.manifest["selectedPages"].toArray();
    const auto manifestNames = source.manifest["pageNames"].toArray();
    const int perEnd = source.manifest["perEnd"].toInt();
    if (perEnd < 1 || perEnd > 3 || manifestPages.size() != count || manifestNames.size() != source.source.pageNames.size())
        return invalid(QStringLiteral("Invalid selected source plan."));
    const auto indexes = sampleIndexes(source.source.pageCount, perEnd);
    if (indexes.size() != count)
        return invalid(QStringLiteral("Selected source plan differs from the job."));
    for (int i = 0; i < manifestNames.size(); ++i)
        if (manifestNames[i].toString() != source.source.pageNames.at(i))
            return invalid(QStringLiteral("Source page order changed."));
    const auto options = optionsFrom(runtime.settingsSnapshot["options"].toObject());
    if (!options.neural)
        return invalid(QStringLiteral("Durable page execution requires neural OCR."));
    QVector<NeuralPageEvidence> inputs;
    for (int i = 0; i < count; ++i) {
        if (flag->load())
            return invalid(QStringLiteral("Cancelled"));
        const auto &page = source.source.pages.at(i);
        const auto manifestPage = manifestPages[i].toObject();
        if (page.number != job.spec.pages.at(i) || page.number != indexes.at(i) + 1 || page.number != manifestPage["number"].toInt() || page.name != manifestPage["name"].toString() || page.sourceSha256 != manifestPage["rawSha256"].toString() || !page.error.isEmpty() || page.image.isNull())
            return invalid(QStringLiteral("Selected source page changed or is invalid."));
        const auto prepared = prepareOcrPage(page.image, options);
        QByteArray png;
        QBuffer buffer(&png);
        if (!validOcrGeometry(prepared.geometry) || !buffer.open(QIODevice::WriteOnly) || !prepared.image.save(&buffer, "PNG"))
            return invalid(QStringLiteral("Could not prepare selected OCR page."));
        inputs.append({ i, prepared.geometry, hash(png), { }, { }, { }, { } });
    }
    return inputs;
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
    QElapsedTimer cacheTimer;
    cacheTimer.start();
    QString inputError;
    const auto preparedInputs = selectedInputs(*job, source, runtime, flag, &inputError);
    if (!preparedInputs)
        return fail(inputError, flag->load() ? RecognitionStatus::Cancelled : RecognitionStatus::Failed);
    const auto &inputs = *preparedInputs;
    const auto options = optionsFrom(runtime.settingsSnapshot["options"].toObject());
    QVector<int> missing;
    QVector<QImage> images;
    for (int i = 0; i < count; ++i) {
        if (flag->load())
            return fail(QStringLiteral("Cancelled"), RecognitionStatus::Cancelled);
        const auto &page = source.source.pages.at(i);
        const auto &input = inputs.at(i);
        bool recorded = false;
        for (const auto &receipt : job->pages)
            recorded = recorded || receipt.page == page.number;
        if (recorded) {
            QString error;
            const auto restored = LocalOcrPersistence::restorePage(*job, runtime, i, input.geometry, input.imageSha256, cacheRoot, &error, flag);
            if (!restored)
                return fail(error, flag->load() ? RecognitionStatus::Cancelled : RecognitionStatus::Failed);
            out.batch.evidence[i] = *restored;
            out.batch.readings[i] = parseNeuralReading(restored->rawResult, restored->geometry.preparedSize);
            out.batch.validPages[i] = true;
            out.cacheHits[i] = true;
            ++out.cachedPages;
        } else {
            const auto found = LocalOcrPersistence::findUnrecordedPage(runtime, i, input.geometry, input.imageSha256, cacheRoot, flag);
            if (!found.error.isEmpty())
                return fail(found.error, flag->load() ? RecognitionStatus::Cancelled : RecognitionStatus::Failed);
            if (found.page) {
                const auto saved = LocalOcrPersistence::recordPage(store, lease, runtime, *found.page, cacheRoot, flag, now);
                if (!saved.receiptSaved)
                    return fail(saved.error, flag->load() ? RecognitionStatus::Cancelled : RecognitionStatus::DeliveryFailed);
                out.batch.evidence[i] = *found.page;
                out.batch.readings[i] = parseNeuralReading(found.page->rawResult, found.page->geometry.preparedSize);
                out.batch.validPages[i] = true;
                out.cacheHits[i] = true;
                ++out.cachedPages;
                ++out.recoveredUnrecordedPages;
            } else {
                missing.append(i);
                images.append(page.image);
            }
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
    QVector<std::optional<NeuralPageEvidence>> accepted(count);
    QString deliveryError;
    const PageSink persist = [&](const NeuralPageEvidence &page, QString *error) {
        const auto reject = [&](const QString &reason) {
            deliveryError = reason;
            if (error)
                *error = reason;
            return false;
        };
        if (!deliveryError.isEmpty())
            return reject(deliveryError);
        if (flag->load()) {
            if (error)
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
                if (error)
                    *error = saved.error;
                return false;
            }
            return reject(saved.error.isEmpty() ? QStringLiteral("Page persistence failed.") : saved.error);
        }
        accepted[mapped.selectedIndex] = mapped;
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
        if (out.batch.validPages.at(original) && accepted.at(original)) {
            const auto &saved = *accepted.at(original);
            const auto &completed = *out.batch.evidence.at(original);
            if (saved.rawResult != completed.rawResult || saved.resultSha256 != completed.resultSha256 || saved.requestedDevice != completed.requestedDevice || saved.actualDevice != completed.actualDevice) {
                out.batch.validPages[original] = false;
                deliveryError = QStringLiteral("Completion evidence differs from the accepted page receipt.");
            }
        }
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

std::optional<RestoredReview> restoreReview(const OcrJobs::Job &job, const LocalOcrLibrary::Binding &library,
                                            const LocalOcrSource::Snapshot &source, const LocalOcrRuntime::Measurement &runtime,
                                            const QString &cacheRoot, QString *error, const Cancellation &cancel)
{
    if (error)
        error->clear();
    const auto fail = [&](const QString &reason) -> std::optional<RestoredReview> {
        if (error)
            *error = reason;
        return std::nullopt;
    };
    const auto flag = cancel ? cancel : std::make_shared<std::atomic_bool>(false);
    if (flag->load())
        return fail(QStringLiteral("Review restoration cancelled."));
    if ((job.state != OcrJobs::State::PageReview && job.state != OcrJobs::State::FilenameReview) || !job.error.isEmpty())
        return fail(QStringLiteral("Only a successfully completed job can restore review."));
    if (library.generation.isEmpty() || library.generation != job.spec.libraryGeneration || library.comicId != job.spec.comicId || library.libraryRoot != job.spec.sourceContext["libraryRoot"].toString() || library.sourcePath != job.spec.sourceContext["path"].toString())
        return fail(QStringLiteral("Current library selection differs from the saved review."));
    QElapsedTimer timer;
    timer.start();
    const auto inputs = selectedInputs(job, source, runtime, flag, error);
    if (!inputs)
        return std::nullopt;
    if (job.pages.size() != inputs->size())
        return fail(QStringLiteral("Completed review requires exactly one receipt per selected page."));
    RestoredReview out;
    out.savedState = job.state;
    out.metadata = source.source;
    out.metadata.suggestions.clear();
    for (int i = 0; i < inputs->size(); ++i) {
        const auto &input = inputs->at(i);
        const auto saved = LocalOcrPersistence::restorePage(job, runtime, i, input.geometry, input.imageSha256, cacheRoot, error, flag);
        if (!saved)
            return std::nullopt; // Never expose a partly reconstructed successful review.
        out.evidence.append(*saved);
        auto &page = out.metadata.pages[i];
        page.reading = parseNeuralReading(saved->rawResult, saved->geometry.preparedSize);
        page.text = page.reading.text;
        page.error = page.reading.error;
        page.kind = classifyPage(page.text, page.number);
    }
    out.metadata.suggestions = suggest(out.metadata.pages, job.spec.sourceContext["path"].toString(), job.spec.sourceContext["libraryRoot"].toString());
    bool pageCandidates = false;
    for (const auto &candidate : out.metadata.suggestions)
        pageCandidates = pageCandidates || candidate.page > 0;
    if (pageCandidates != (job.state == OcrJobs::State::PageReview))
        return fail(QStringLiteral("Saved review classification differs from the validated evidence."));
    if (flag->load())
        return fail(QStringLiteral("Review restoration cancelled."));
    out.restoreMs = timer.elapsed();
    return out;
}
}
