#ifndef OCR_JOB_STORE_H
#define OCR_JOB_STORE_H

#include <QJsonObject>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include <functional>
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
    QJsonObject settingsSnapshot;
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
    // Production entry points sample a nonthrowing clock after the write lock.
    // Fixed-time overloads preserve deterministic logical-time callers.
    std::optional<Lease> claimWhenCurrent(const QString &id, const QString &owner, qint64 durationMs, const std::function<qint64()> &clock);
    bool heartbeatWhenCurrent(const Lease &lease, qint64 durationMs, const std::function<qint64()> &clock);
    bool finishWhenCurrent(const Lease &lease, bool hasPageCandidates, const std::function<qint64()> &clock);
    bool failWhenCurrent(const Lease &lease, const QString &message, const std::function<qint64()> &clock);
    // Worker cancellation must not pause a successor that owns a newer token.
    bool pauseWhenCurrent(const Lease &lease, const std::function<qint64()> &clock);
    bool recordPage(const Lease &lease, const PageReceipt &page, qint64 nowMs);
    // Production clocks are sampled after acquiring the DB write transaction.
    // The callback must not throw; the fixed-time overload supports logical tests.
    bool recordPageWhenCurrent(const Lease &lease, const PageReceipt &page, const std::function<qint64()> &clock);
    bool finish(const Lease &lease, bool hasPageCandidates, qint64 nowMs);
    bool fail(const Lease &lease, const QString &message, qint64 nowMs);
    bool pause(const QString &id);
    bool cancel(const QString &id);
    bool resume(const QString &id);

    // Fence expired owners into Interrupted, never automatically make jobs runnable.
    // Before explicitly resuming, the executor must confirm the old OS worker exited.
    // Database leases alone do not provide physical GPU process exclusivity.
    bool interruptExpired(qint64 nowMs);
    bool interruptExpiredWhenCurrent(const std::function<qint64()> &clock);

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
// Returns empty for unsupported/incomplete snapshots. This checks structure, not
// the installed files: the executor must remeasure the environment before reuse.
QString settingsFingerprint(const QJsonObject &snapshot);
QString stateName(State state);
}

#endif
