#include "local_ocr_recovery.h"

#include <QCoreApplication>

namespace LocalOcrRecovery {
using namespace LocalMetadata;
namespace {
QString tr(const char *text)
{
    return QCoreApplication::translate("LocalMetadata", text);
}

void normalize(RecognitionBatch &batch, int count)
{
    if (batch.evidence.isEmpty())
        batch.evidence.resize(count); // Legacy synthetic runners may omit evidence.
    if (batch.readings.size() != count || batch.validPages.size() != count || batch.evidence.size() != count) {
        batch.evidence.fill(std::nullopt, count);
        batch.readings.resize(count);
        batch.validPages.fill(false, count);
        if (batch.status != RecognitionStatus::CleanupFailed && batch.status != RecognitionStatus::Cancelled && batch.status != RecognitionStatus::DeliveryFailed)
            batch.status = RecognitionStatus::Failed;
        batch.error = tr("Invalid OCR page completion record.");
    }
    bool allValid = true;
    for (int i = 0; i < count; ++i) {
        const auto &reading = batch.readings.at(i);
        batch.validPages[i] = batch.validPages.at(i) && reading.error.isEmpty() && (reading.device == QStringLiteral("cpu") || reading.device == QStringLiteral("gpu:0"));
        if (batch.evidence.at(i) && (batch.evidence.at(i)->selectedIndex != i || batch.evidence.at(i)->actualDevice != reading.device || !validNeuralEvidence(*batch.evidence.at(i))))
            batch.validPages[i] = false;
        if (!batch.validPages.at(i))
            batch.evidence[i].reset();
        allValid = allValid && batch.validPages.at(i);
    }
    if (batch.status == RecognitionStatus::Complete && (!allValid || !batch.error.isEmpty())) {
        batch.status = RecognitionStatus::Failed;
        if (batch.error.isEmpty())
            batch.error = tr("Some OCR pages did not complete.");
    }
    if (batch.status != RecognitionStatus::Complete && batch.error.isEmpty())
        batch.error = tr("OCR did not complete.");
}
}

RecognitionBatch recognize(const QVector<QImage> &images, const OcrOptions &options,
                           const Cancellation &cancel, const Progress &progress, const Attempt &attempt, const PageSink &sink)
{
    QString deliveryError;
    QVector<bool> delivered(images.size(), false);
    const auto makeSink = [&](const QVector<int> &indexes, bool cpuOnly) -> PageSink {
        if (!sink)
            return { };
        return [&, indexes, cpuOnly](const NeuralPageEvidence &page, QString *error) {
            if (cancel && cancel->load())
                return false;
            if (deliveryError.isEmpty()) {
                if (page.selectedIndex < 0 || page.selectedIndex >= indexes.size() || !validNeuralEvidence(page) || (cpuOnly && (page.requestedDevice != QStringLiteral("cpu") || page.actualDevice != QStringLiteral("cpu")))) {
                    deliveryError = tr("Invalid OCR page delivery.");
                } else {
                    auto mapped = page;
                    mapped.selectedIndex = indexes.at(page.selectedIndex);
                    if (delivered.at(mapped.selectedIndex)) {
                        deliveryError = tr("Duplicate OCR page delivery.");
                    } else {
                        QString reason;
                        bool accepted = false;
                        try {
                            accepted = sink(mapped, &reason);
                        } catch (...) {
                            reason = tr("OCR page receiver raised an exception.");
                        }
                        if (accepted)
                            delivered[mapped.selectedIndex] = true;
                        else
                            deliveryError = reason.isEmpty() ? tr("OCR page delivery failed.") : reason.left(1500);
                    }
                }
            }
            if (error)
                *error = deliveryError;
            return deliveryError.isEmpty();
        };
    };
    const auto applyDeliveryFailure = [&](RecognitionBatch &outcome) {
        if (!deliveryError.isEmpty() && outcome.status != RecognitionStatus::CleanupFailed) {
            outcome.status = RecognitionStatus::DeliveryFailed;
            outcome.error = deliveryError;
        }
    };
    QVector<int> originalIndexes;
    for (int i = 0; i < images.size(); ++i)
        originalIndexes.append(i);
    auto batch = attempt(images, options, cancel, progress, makeSink(originalIndexes, !options.gpu));
    applyDeliveryFailure(batch);
    normalize(batch, images.size());
    if (cancel && cancel->load() && batch.status != RecognitionStatus::CleanupFailed && batch.status != RecognitionStatus::DeliveryFailed) {
        batch.status = RecognitionStatus::Cancelled;
        batch.error = tr("Cancelled");
    }
    // Cleanup failure and cancellation never authorize another process.
    if (!batch.gpuStarted || batch.status != RecognitionStatus::Failed)
        return batch;
    QVector<int> indexes;
    QVector<QImage> remaining;
    for (int i = 0; i < images.size(); ++i) {
        if (!batch.validPages.at(i)) {
            indexes.append(i);
            remaining.append(images.at(i));
        }
    }
    // A nonzero exit after all outputs is still an execution failure. Preserve
    // evidence; do not repeat the whole work merely to conceal that failure.
    if (remaining.isEmpty())
        return batch;
    const int retained = images.size() - remaining.size();
    const auto initialError = batch.error;
    auto fallback = options;
    fallback.gpu = false;
    const Progress retryProgress = [&](int completed, int, const QString &stage) {
        if (progress)
            progress(retained + qBound(0, completed, int(remaining.size())), images.size(), stage);
    };
    retryProgress(0, remaining.size(), tr("GPU execution failed; retrying unfinished pages on CPU."));
    // Progress can synchronously request cancellation.
    if (cancel && cancel->load()) {
        batch.status = RecognitionStatus::Cancelled;
        batch.error = tr("Cancelled");
        return batch;
    }
    auto retried = attempt(remaining, fallback, cancel, retryProgress, makeSink(indexes, true));
    applyDeliveryFailure(retried);
    normalize(retried, remaining.size());
    if (retried.gpuStarted) {
        retried.status = RecognitionStatus::Failed;
        retried.error = tr("CPU retry unexpectedly started a GPU worker.");
        retried.validPages.fill(false);
    }
    for (int i = 0; i < indexes.size(); ++i) {
        // A CPU request must not accept a response labelled as GPU.
        if (retried.validPages.at(i) && retried.readings.at(i).device != QStringLiteral("cpu")) {
            retried.validPages[i] = false;
            retried.status = RecognitionStatus::Failed;
            retried.error = tr("CPU retry returned an unexpected device.");
        }
        auto &reading = retried.readings[i];
        const auto warning = tr("GPU execution failed; this page was retried on CPU.");
        reading.warning = reading.warning.isEmpty() ? warning : reading.warning + u'\n' + warning;
        batch.readings[indexes.at(i)] = reading;
        batch.validPages[indexes.at(i)] = retried.validPages.at(i);
        batch.evidence[indexes.at(i)] = retried.validPages.at(i) ? retried.evidence.at(i) : std::nullopt;
        if (batch.evidence.at(indexes.at(i)))
            batch.evidence[indexes.at(i)]->selectedIndex = indexes.at(i);
    }
    batch.attemptErrors.append(initialError);
    batch.attemptErrors.append(retried.attemptErrors);
    batch.status = retried.status;
    batch.error = retried.error;
    if (cancel && cancel->load() && batch.status != RecognitionStatus::CleanupFailed && batch.status != RecognitionStatus::DeliveryFailed) {
        batch.status = RecognitionStatus::Cancelled;
        batch.error = tr("Cancelled");
    }
    return batch;
}

QVector<Reading> legacyReadings(const RecognitionBatch &batch)
{
    auto readings = batch.readings;
    // The old vector API has no session status. Preserve text/device but attach
    // failure to every otherwise-successful page so callers cannot report success.
    if (batch.status != RecognitionStatus::Complete)
        for (auto &reading : readings)
            if (reading.error.isEmpty())
                reading.error = batch.error.isEmpty() ? tr("OCR did not complete.") : batch.error;
    return readings;
}
}
