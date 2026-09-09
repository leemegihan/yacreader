#ifndef OCR_JOB_STORE_H
#define OCR_JOB_STORE_H

#include <QJsonObject>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include <optional>

class QThread;

// Durable bookkeeping only: no OCR execution, cache lookup, catalog requests or
// library writes. Each instance belongs to the thread that constructed it.
namespace OcrJobs {
enum class State { Queued,
                   Running,
                   Paused,
                   Cancelled,
                   Failed,
                   Interrupted,
                   PageReview,
                   FilenameReview };

struct Spec {
    QString libraryGeneration;
    QString comicId;
    QString sourceSnapshot;
    // Include path, library root and source kind; changes create a new job.
    QJsonObject sourceContext;
    QString settingsFingerprint;
    int totalPages = 0;
    QVector<int> pages;
};

// A receipt references an already validated, durably stored page result.
// It is NOT an OCR cache entry. A caller must revalidate the referenced artifact
// before reuse; this store never treats an arbitrary JSON file as valid OCR.
struct PageReceipt {
    int page = 0;
    QString cacheKey;
    QString resultSha256;
    QString actualDevice;
};

struct Job {
    QString id;
    Spec spec;
    State state = State::Queued;
    int attempts = 0;
    QString error;
    QVector<PageReceipt> pages;
};

struct Lease {
    QString jobId;
    QString token;
    QString owner;
};

class Store
{
public:
    Store();
    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    // Explicit, absolute path named ocr-jobs.sqlite in a separate data directory.
    // Existing foreign databases and unknown schema versions are refused.
    bool open(const QString &databasePath);
    QString lastError() const { return error; }
    std::optional<QString> enqueue(const Spec &spec);
    std::optional<Job> get(const QString &id);
    QVector<Job> list();
    std::optional<Lease> claim(const QString &id, const QString &owner, qint64 nowMs, qint64 durationMs);
    bool heartbeat(const Lease &lease, qint64 nowMs, qint64 durationMs);
    bool recordPage(const Lease &lease, const PageReceipt &page, qint64 nowMs);
    bool finish(const Lease &lease, bool hasPageCandidates, qint64 nowMs);
    bool fail(const Lease &lease, const QString &message, qint64 nowMs);
    bool pause(const QString &id);
    bool cancel(const QString &id);
    bool resume(const QString &id);

    // Fence expired owners into Interrupted, never automatically make jobs runnable.
    // Before explicitly resuming, the executor must confirm the old OS worker exited.
    // Database leases alone do not provide physical GPU process exclusivity.
    bool interruptExpired(qint64 nowMs);

private:
    bool ready();
    bool execute(const QString &sql);
    bool begin();
    bool commit();
    bool rollback(const QString &message);
    bool fence(const Lease &lease, qint64 nowMs);
    bool transition(const QString &id, State target);
    bool setFinished(const Lease &lease, State state, const QString &message);
    bool opened = false;
    QSqlDatabase db;
    QString connection;
    QString error;
    QThread *thread;
};
QString stateName(State state);
}

#endif
