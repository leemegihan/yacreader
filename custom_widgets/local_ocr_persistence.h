#ifndef LOCAL_OCR_PERSISTENCE_H
#define LOCAL_OCR_PERSISTENCE_H

#include "local_ocr_cache.h"
#include "local_ocr_runtime.h"
#include "ocr_job_store.h"

namespace LocalOcrPersistence {
using Clock = std::function<qint64()>;
struct Result {
    bool cacheSaved = false;
    bool receiptSaved = false;
    QString cacheKey;
    QString error;
};
// The executor must revalidate the source and measure a stopped runtime first.
// This helper binds those supplied settings/evidence, not source-file ownership.
std::optional<LocalOcrCache::Identity> identity(const LocalOcrRuntime::Measurement &measurement,
                                                const LocalMetadata::NeuralPageEvidence &page, QString *error);
// Never finishes a job or hides batch failure. A late/cancelled result may leave
// immutable orphan cache data; expired leases are rejected at the DB write lock.
Result recordPage(OcrJobs::Store &store, const OcrJobs::Lease &lease,
                  const LocalOcrRuntime::Measurement &measurement, const LocalMetadata::NeuralPageEvidence &page,
                  const QString &cacheRoot, const LocalMetadata::Cancellation &cancel = { }, const Clock &clock = { });
}
#endif