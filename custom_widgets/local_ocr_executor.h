#ifndef LOCAL_OCR_EXECUTOR_H
#define LOCAL_OCR_EXECUTOR_H

#include "local_ocr_persistence.h"
#include "local_ocr_source.h"

namespace LocalOcrExecutor {
struct Outcome {
    LocalMetadata::RecognitionBatch batch;
    LocalMetadata::Result metadata;
    int cachedPages = 0;
    int recoveredUnrecordedPages = 0;
    QVector<bool> cacheHits; // Per selected page; device/timing may be from an older attempt.
    qint64 cacheReadMs = 0; // Preparation/cache recovery phase, separate from original OCR time.
    bool stateSaved = false;
    std::optional<OcrJobs::State> state;
    QString stateError;
};
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
