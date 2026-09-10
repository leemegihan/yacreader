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
// Read-only recovery of a committed receipt. Caller must revalidate source/plan
// and supply newly prepared geometry/PNG hash plus a stopped-runtime measurement.
// Missing/corrupt/mismatched evidence fails without changing the job or cache.
// A restored page never implies successful batch completion or reviewed metadata.
std::optional<LocalMetadata::NeuralPageEvidence> restorePage(const OcrJobs::Job &job,
                                                             const LocalOcrRuntime::Measurement &measurement, int selectedIndex,
                                                             const LocalMetadata::OcrGeometry &geometry, const QString &imageSha256,
                                                             const QString &cacheRoot, QString *error, const LocalMetadata::Cancellation &cancel = { });
// Never finishes a job or hides batch failure. A late/cancelled result may leave
// immutable orphan cache data; expired leases are rejected at the DB write lock.
Result recordPage(OcrJobs::Store &store, const OcrJobs::Lease &lease,
                  const LocalOcrRuntime::Measurement &measurement, const LocalMetadata::NeuralPageEvidence &page,
                  const QString &cacheRoot, const LocalMetadata::Cancellation &cancel = { }, const Clock &clock = { });
}
#endif