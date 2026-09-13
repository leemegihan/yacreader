#include "local_ocr_session.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QUuid>

namespace LocalOcrSession {
namespace {
QString tr(const char *text)
{
    return QCoreApplication::translate("LocalOcrSession", text);
}
// Refuse redirected ancestors before creating private job/cache directories.
QString directPath(const QString &path)
{
    if (!QDir::isAbsolutePath(path))
        return { };
    auto clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
    auto part = clean;
    while (!part.isEmpty()) {
        const QFileInfo info(part);
        if (info.isSymLink() || info.isJunction())
            return { };
        const auto parent = info.absolutePath();
        if (parent == part)
            break;
        part = parent;
    }
    return clean;
}
bool contains(const QString &parent, const QString &child)
{
    return parent.compare(child, Qt::CaseInsensitive) == 0 || child.startsWith(parent.endsWith(u'/') ? parent : parent + u'/', Qt::CaseInsensitive);
}
}
Outcome run(const Request &request, Action action, const LocalMetadata::Cancellation &cancel,
            const LocalMetadata::Progress &preparation, const LocalMetadata::Progress &recognition,
            const LocalOcrExecutor::Runner &runner)
{
    Outcome out;
    const auto flag = cancel ? cancel : std::make_shared<std::atomic_bool>(false);
    const auto fail = [&](const QString &message) {
        out.error = message;
        out.metadata.error = message;
        return out;
    };
    if (flag->load())
        return fail(tr("Cancelled before opening saved OCR work."));
    const auto root = directPath(request.dataRoot);
    const auto library = directPath(request.libraryRoot);
    const auto runtime = directPath(QFileInfo(request.applicationPath).absolutePath());
    if (root.isEmpty() || library.isEmpty() || runtime.isEmpty() || contains(library, root) || contains(root, library) || contains(runtime, root) || contains(root, runtime))
        return fail(tr("OCR work data must use a separate local directory outside the library and application."));
    const auto database = QDir(root).filePath(QStringLiteral("ocr-jobs.sqlite"));
    if (action == Action::Continue && !QFileInfo::exists(database))
        return fail(tr("No saved OCR work exists for these settings. Use Read pages to start."));
    if (!QDir().mkpath(root))
        return fail(tr("Could not create the private OCR work directory."));
    QLockFile lock(QDir(root).filePath(QStringLiteral("selected-session.lock")));
    lock.setStaleLockTime(0); // Long-running OCR must not become stale merely by age.
    if (!lock.tryLock(0))
        return fail(tr("Another saved OCR session is still active. Wait for its cleanup."));
    QString error;
    const auto prepared = LocalOcrPreflight::read(request.libraryRoot, request.comicInfoId, request.sourcePath,
                                                  request.perEnd, request.applicationPath, request.options, &error, flag, preparation);
    if (!prepared)
        return fail(error);
    out.library = prepared->library;
    out.sourceFingerprint = prepared->source.fingerprint;
    out.metadata = prepared->source.source;
    out.jobId = OcrJobs::jobId(prepared->spec);
    if (out.jobId.isEmpty())
        return fail(tr("Could not identify the selected OCR work."));
    if (flag->load())
        return fail(tr("Cancelled before opening saved OCR work."));
    OcrJobs::Store store; // Constructed, used and destroyed on this worker thread.
    if (!store.open(database))
        return fail(store.lastError());
    if (action == Action::Start && !store.enqueue(prepared->spec))
        return fail(store.lastError());
    const auto job = store.get(out.jobId);
    if (!job)
        return fail(action == Action::Continue ? tr("No saved work matches the current library, pages and OCR settings. Previous work is preserved.") : store.lastError());
    out.state = job->state;
    const auto cache = QDir(root).filePath(QStringLiteral("pages"));
    if (job->state == OcrJobs::State::PageReview || job->state == OcrJobs::State::FilenameReview) {
        const auto review = LocalOcrExecutor::restoreReview(*job, prepared->library, prepared->source, prepared->runtime, cache, &error, flag);
        if (!review)
            return fail(error);
        out.metadata = review->metadata;
        out.complete = true;
        out.restored = true;
        out.cachedPages = review->evidence.size();
        out.cacheReadMs = review->restoreMs;
        return out;
    }
    if (job->state == OcrJobs::State::Paused) {
        if (action != Action::Continue)
            return fail(tr("This work is paused. Use Open saved result / Continue to read only unfinished pages."));
        if (!store.resume(out.jobId))
            return fail(store.lastError());
    } else if (job->state != OcrJobs::State::Queued) {
        return fail(tr("Saved work is running or requires failure recovery. Its results were preserved; it was not restarted.") + (job->error.isEmpty() ? QString() : u'\n' + job->error));
    }
    if (!QDir().mkpath(cache))
        return fail(tr("Could not create the private OCR page cache."));
    const auto now = [] { return QDateTime::currentMSecsSinceEpoch(); };
    const auto lease = store.claimWhenCurrent(out.jobId, QUuid::createUuid().toString(), 300000, now);
    if (!lease)
        return fail(store.lastError());
    const auto result = LocalOcrExecutor::executeClaimed(store, *lease, prepared->source, prepared->runtime, cache, flag, recognition, now, runner);
    out.metadata = result.metadata;
    out.state = result.state;
    out.cachedPages = result.cachedPages;
    out.recoveredUnrecordedPages = result.recoveredUnrecordedPages;
    out.repeatedInputPages = result.repeatedInputPages;
    out.cacheReadMs = result.cacheReadMs;
    out.complete = result.stateSaved && result.batch.status == LocalMetadata::RecognitionStatus::Complete;
    if (result.stateSaved && result.state == OcrJobs::State::Paused && result.batch.status == LocalMetadata::RecognitionStatus::Cancelled && result.batch.validPages.size() == out.metadata.pages.size()) {
        for (int i = 0; i < result.batch.validPages.size(); ++i)
            if (!result.batch.validPages.at(i))
                out.pendingPages.append(out.metadata.pages.at(i).number);
    }
    out.error = result.stateError.isEmpty() ? result.batch.error : result.stateError;
    if (!out.error.isEmpty())
        out.metadata.error = out.error;
    return out;
}
}
