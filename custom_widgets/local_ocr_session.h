#ifndef LOCAL_OCR_SESSION_H
#define LOCAL_OCR_SESSION_H

#include "local_ocr_executor.h"
#include "local_ocr_preflight.h"

namespace LocalOcrSession {
enum class Action { Start,
                    Continue };
struct Request {
    QString libraryRoot;
    qulonglong comicInfoId = 0;
    QString sourcePath;
    int perEnd = 3;
    QString applicationPath;
    QString dataRoot; // Dedicated local directory outside library and runtime.
    LocalMetadata::OcrOptions options;
};
struct Outcome {
    LocalMetadata::Result metadata;
    std::optional<LocalOcrLibrary::Binding> library;
    QString sourceFingerprint;
    QString jobId;
    std::optional<OcrJobs::State> state;
    bool complete = false;
    bool restored = false;
    int cachedPages = 0;
    int recoveredUnrecordedPages = 0;
    int repeatedInputPages = 0;
    qint64 cacheReadMs = 0;
    QString error;
};
// Owns the Store and cooperative session lock entirely on the calling worker
// thread. Continue never enqueues a different identity. Only cleanly Paused jobs
// resume; Running/Failed/Interrupted need separate recovery and are not reset.
// The runner must return after process-tree cleanup, as executeClaimed requires.
Outcome run(const Request &request, Action action, const LocalMetadata::Cancellation &cancel,
            const LocalMetadata::Progress &preparation = { }, const LocalMetadata::Progress &recognition = { },
            const LocalOcrExecutor::Runner &runner = { });
}
#endif
