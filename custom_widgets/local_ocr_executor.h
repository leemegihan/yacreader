#ifndef LOCAL_OCR_EXECUTOR_H
#define LOCAL_OCR_EXECUTOR_H

#include "local_ocr_library.h"
#include "local_ocr_persistence.h"
#include "local_ocr_source.h"

namespace LocalOcrExecutor {
struct Outcome {
    LocalMetadata::RecognitionBatch batch;
    LocalMetadata::Result metadata;
    int cachedPages = 0;
    int recoveredUnrecordedPages = 0;
    int repeatedInputPages = 0; // Planned exact duplicates sharing a current inference, not old cache hits.
    QVector<bool> cacheHits; // Per selected page; device/timing may be from an older attempt.
    qint64 cacheReadMs = 0; // Preparation/cache recovery phase, separate from original OCR time.
    bool stateSaved = false;
    std::optional<OcrJobs::State> state;
    QString stateError;
};
struct RestoredReview {
    LocalMetadata::Result metadata;
    QVector<LocalMetadata::NeuralPageEvidence> evidence;
    OcrJobs::State savedState = OcrJobs::State::FilenameReview;
    qint64 restoreMs = 0; // Preparation/cache time; readings retain original OCR times.
};
// Read-only completed-review snapshot: no claim, OCR, orphan adoption or DB/cache
// writes. Caller supplies a freshly read selected job/library/source and measures
// a stopped runtime. Any mismatch returns no review, including partial results.
// This does not authorize metadata save or lock the library after the check.
std::optional<RestoredReview> restoreReview(const OcrJobs::Job &job, const LocalOcrLibrary::Binding &library,
                                            const LocalOcrSource::Snapshot &source, const LocalOcrRuntime::Measurement &runtime,
                                            const QString &cacheRoot, QString *error, const LocalMetadata::Cancellation &cancel = { });
// A runner returns only after its OS process tree is closed. Injection is for
// synthetic tests; normal callers leave it empty to use the neural pipeline.
using Runner = std::function<LocalMetadata::RecognitionBatch(const QVector<QImage> &,
                                                             const LocalMetadata::OcrOptions &, const LocalMetadata::Cancellation &,
                                                             const LocalMetadata::Progress &, const LocalMetadata::PageSink &)>;
// One already-claimed work on the Store's owning thread. Caller revalidates the
// current library generation, captures the source and measures a stopped runtime
// before claiming. No enqueue/resume, runtime measurement, UI or library writes.
// Missing/corrupt receipts do not authorize overwriting or a full-work OCR retry.
Outcome executeClaimed(OcrJobs::Store &store, const OcrJobs::Lease &lease,
                       const LocalOcrSource::Snapshot &source, const LocalOcrRuntime::Measurement &runtime,
                       const QString &cacheRoot, const LocalMetadata::Cancellation &cancel,
                       const LocalMetadata::Progress &progress = { }, const LocalOcrPersistence::Clock &clock = { },
                       const Runner &runner = { });
}
#endif
