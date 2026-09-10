#ifndef LOCAL_OCR_RECOVERY_H
#define LOCAL_OCR_RECOVERY_H

#include "local_metadata.h"

namespace LocalOcrRecovery {
// A runner returns only after verifying that its worker tree exited.
// This injection point lets synthetic tests exercise retry policy without OCR.
using Attempt = std::function<LocalMetadata::RecognitionBatch(const QVector<QImage> &, const LocalMetadata::OcrOptions &, const LocalMetadata::Cancellation &, const LocalMetadata::Progress &, const LocalMetadata::PageSink &)>;
LocalMetadata::RecognitionBatch recognize(const QVector<QImage> &images, const LocalMetadata::OcrOptions &options,
                                          const LocalMetadata::Cancellation &cancel, const LocalMetadata::Progress &progress, const Attempt &attempt, const LocalMetadata::PageSink &sink = { });
QVector<LocalMetadata::Reading> legacyReadings(const LocalMetadata::RecognitionBatch &batch);
}
#endif
