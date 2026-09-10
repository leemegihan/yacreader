#ifndef LOCAL_OCR_PREFLIGHT_H
#define LOCAL_OCR_PREFLIGHT_H

#include "local_ocr_library.h"
#include "local_ocr_runtime.h"
#include "local_ocr_source.h"
#include "ocr_job_store.h"

namespace LocalOcrPreflight {
struct Prepared {
    LocalOcrLibrary::Binding library;
    LocalOcrSource::Snapshot source;
    LocalOcrRuntime::Measurement runtime;
    OcrJobs::Spec spec;
};
// Read one selected work and an already stopped personal Windows runtime. No
// queue/cache/library writes, OCR, network or enumeration of other works.
// Library identity is checked before/after measurement and selected page capture.
// Progress counts preparation stages, not recognized pages.
// A returned snapshot is not a lock covering a later claim or metadata save.
std::optional<Prepared> read(const QString &libraryRoot, qulonglong comicInfoId,
                             const QString &sourcePath, int perEnd, const QString &applicationPath,
                             const LocalMetadata::OcrOptions &options, QString *error,
                             const LocalMetadata::Cancellation &cancel = { }, const LocalMetadata::Progress &progress = { });
}
#endif
