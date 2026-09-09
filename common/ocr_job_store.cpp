#include "ocr_job_store.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QUuid>
#include <QVariant>

#include <limits>

namespace OcrJobs {
namespace {
constexpr int applicationId = 0x594f4352;
constexpr int schemaVersion = 1;

bool fingerprint(const QString &value)
{
    if (value.size() != 64)
        return false;
    for (const QChar c : value)
        if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f')))
            return false;
    return true;
}

bool validTime(qint64 now, qint64 duration)
{
    return now >= 0 && duration > 0 && duration <= 3600000 && now <= std::numeric_limits<qint64>::max() - duration;
}

QByteArray serialize(const Spec &s)
{
    QJsonArray pages;
    for (int page : s.pages)
        pages.append(page);
    return QJsonDocument(QJsonObject {
                                 { "libraryGeneration", s.libraryGeneration },
                                 { "comicId", s.comicId },
                                 { "sourceSnapshot", s.sourceSnapshot },
                                 { "sourceContext", s.sourceContext },
                                 { "settingsFingerprint", s.settingsFingerprint },
                                 { "totalPages", s.totalPages },
                                 { "pages", pages } })
            .toJson(QJsonDocument::Compact);
}

Spec deserialize(const QByteArray &bytes)
{
    const auto o = QJsonDocument::fromJson(bytes).object();
    Spec s;
    s.libraryGeneration = o["libraryGeneration"].toString();
    s.comicId = o["comicId"].toString();
    s.sourceSnapshot = o["sourceSnapshot"].toString();
    s.sourceContext = o["sourceContext"].toObject();
    s.settingsFingerprint = o["settingsFingerprint"].toString();
    s.totalPages = o["totalPages"].toInt();
    for (const auto page : o["pages"].toArray())
        s.pages.append(page.toInt());
    return s;
}

bool validSpec(const Spec &s)
{
    if (s.libraryGeneration.trimmed().isEmpty() || s.comicId.trimmed().isEmpty() || !fingerprint(s.sourceSnapshot) || !fingerprint(s.settingsFingerprint) || s.totalPages <= 0 || s.pages.isEmpty() || s.pages.size() > 6)
        return false;
    for (const QString &key : { QStringLiteral("path"), QStringLiteral("libraryRoot"), QStringLiteral("sourceKind") })
        if (s.sourceContext[key].toString().trimmed().isEmpty())
            return false;
    int previous = 0;
    for (int page : s.pages) {
        if (page <= previous || page > s.totalPages || (page > 3 && page <= s.totalPages - 3))
            return false;
        previous = page;
    }
    return serialize(s).size() <= 65536;
}
}

QString stateName(State state)
{
    switch (state) {
    case State::Queued:
        return QStringLiteral("queued");
    case State::Running:
        return QStringLiteral("running");
    case State::Paused:
        return QStringLiteral("paused");
    case State::Cancelled:
        return QStringLiteral("cancelled");
    case State::Failed:
        return QStringLiteral("failed");
    case State::Interrupted:
        return QStringLiteral("interrupted");
    case State::PageReview:
        return QStringLiteral("page_review");
    case State::FilenameReview:
        return QStringLiteral("filename_review");
    }
    return { };
}

Store::Store()
    : thread(QThread::currentThread())
{
}

Store::~Store()
{
    if (db.isValid()) {
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }
}

bool Store::ready()
{
    error.clear();
    if (QThread::currentThread() != thread) {
        error = QStringLiteral("Use the job store only on its owning thread.");
        return false;
    }
    if (!opened || !db.isOpen()) {
        error = QStringLiteral("Job store is not open.");
        return false;
    }
    return true;
}

bool Store::execute(const QString &sql)
{
    QSqlQuery q(db);
    if (q.exec(sql))
        return true;
    error = q.lastError().text();
    return false;
}

bool Store::begin()
{
    return execute(QStringLiteral("BEGIN IMMEDIATE"));
}
bool Store::commit()
{
    if (execute(QStringLiteral("COMMIT")))
        return true;
    return rollback(error);
}
bool Store::rollback(const QString &message)
{
    QSqlQuery q(db);
    q.exec(QStringLiteral("ROLLBACK"));
    error = message;
    return false;
}

bool Store::open(const QString &path)
{
    error.clear();
    if (QThread::currentThread() != thread || db.isValid()) {
        error = QStringLiteral("Create a new store on this thread before opening.");
        return false;
    }
    const QFileInfo file(path);
    if (!file.isAbsolute() || file.fileName() != QStringLiteral("ocr-jobs.sqlite") || file.isSymLink() || !file.dir().exists()) {
        error = QStringLiteral("Use an absolute ocr-jobs.sqlite path in an existing private data directory.");
        return false;
    }
    const bool existed = file.exists();
    connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
    db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    db.setDatabaseName(path);
    db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!db.open()) {
        error = db.lastError().text();
        return false;
    }
    // Inspect ownership before enabling WAL or changing an existing database.
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("PRAGMA application_id")) || !q.next())
        return rollback(QStringLiteral("Cannot read job store identity."));
    const int id = q.value(0).toInt();
    q.finish();
    if ((existed && id != applicationId) || (!existed && id != 0 && id != applicationId)) {
        error = QStringLiteral("Refusing a database not owned by the OCR job store.");
        db.close();
        return false;
    }
    if (!q.exec(QStringLiteral("PRAGMA user_version")) || !q.next())
        return rollback(QStringLiteral("Cannot read job store schema."));
    const int version = q.value(0).toInt();
    q.finish();
    if ((id == applicationId && version != schemaVersion) || (id == 0 && version != 0)) {
        error = QStringLiteral("Unsupported OCR job store schema.");
        db.close();
        return false;
    }
    if (!execute(QStringLiteral("PRAGMA foreign_keys=ON")) || !begin())
        return false;
    if (id == 0) {
        if (!execute(QStringLiteral(
                    "CREATE TABLE jobs (id TEXT PRIMARY KEY, spec BLOB NOT NULL, state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 7), "
                    "attempts INTEGER NOT NULL DEFAULT 0, token TEXT NOT NULL DEFAULT '', owner TEXT NOT NULL DEFAULT '', "
                    "lease_start INTEGER NOT NULL DEFAULT 0, expires INTEGER NOT NULL DEFAULT 0, error TEXT NOT NULL DEFAULT '')")) ||
            !execute(QStringLiteral(
                    "CREATE TABLE pages (job_id TEXT NOT NULL REFERENCES jobs(id), page INTEGER NOT NULL, cache_key TEXT NOT NULL, "
                    "result_sha256 TEXT NOT NULL, actual_device TEXT NOT NULL, PRIMARY KEY(job_id,page))")) ||
            !execute(QStringLiteral("PRAGMA application_id=%1").arg(applicationId)) || !execute(QStringLiteral("PRAGMA user_version=%1").arg(schemaVersion)))
            return rollback(error);
    }
    if (!commit())
        return false;
    if (!q.exec(QStringLiteral("PRAGMA journal_mode=WAL")) || !q.next() || q.value(0).toString() != QStringLiteral("wal")) {
        error = QStringLiteral("WAL is required for the private OCR job store.");
        db.close();
        return false;
    }
    q.finish();
    opened = execute(QStringLiteral("PRAGMA synchronous=FULL"));
    return opened;
}

std::optional<QString> Store::enqueue(const Spec &spec)
{
    if (!ready())
        return { };
    if (!validSpec(spec)) {
        error = QStringLiteral("Invalid job identity, settings or bounded page plan.");
        return { };
    }
    const auto bytes = serialize(spec);
    const QString id = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO jobs(id,spec,state) VALUES(?,?,?)"));
    q.addBindValue(id);
    q.addBindValue(bytes);
    q.addBindValue(int(State::Queued));
    if (!q.exec()) {
        error = q.lastError().text();
        return { };
    }
    return id;
}

std::optional<Job> Store::get(const QString &id)
{
    if (!ready())
        return { };
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT spec,state,attempts,error FROM jobs WHERE id=?"));
    q.addBindValue(id);
    if (!q.exec()) {
        error = q.lastError().text();
        return { };
    }
    if (!q.next()) {
        error = QStringLiteral("Job does not exist.");
        return { };
    }
    Job job;
    job.id = id;
    const auto specBytes = q.value(0).toByteArray();
    job.spec = deserialize(specBytes);
    const int state = q.value(1).toInt();
    if (!validSpec(job.spec) || serialize(job.spec) != specBytes || QString::fromLatin1(QCryptographicHash::hash(specBytes, QCryptographicHash::Sha256).toHex()) != id || state < int(State::Queued) || state > int(State::FilenameReview)) {
        error = QStringLiteral("Corrupt job identity or state.");
        return { };
    }
    job.state = State(state);
    job.attempts = q.value(2).toInt();
    job.error = q.value(3).toString();
    q.finish();
    q.prepare(QStringLiteral("SELECT page,cache_key,result_sha256,actual_device FROM pages WHERE job_id=? ORDER BY page"));
    q.addBindValue(id);
    if (!q.exec()) {
        error = q.lastError().text();
        return { };
    }
    while (q.next()) {
        const PageReceipt page { q.value(0).toInt(), q.value(1).toString(), q.value(2).toString(), q.value(3).toString() };
        if (!job.spec.pages.contains(page.page) || !fingerprint(page.cacheKey) || !fingerprint(page.resultSha256) || (page.actualDevice != QStringLiteral("cpu") && page.actualDevice != QStringLiteral("gpu:0"))) {
            error = QStringLiteral("Corrupt page receipt.");
            return { };
        }
        job.pages.append(page);
    }
    return job;
}

QVector<Job> Store::list()
{
    if (!ready())
        return { };
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT id FROM jobs ORDER BY rowid"))) {
        error = q.lastError().text();
        return { };
    }
    QStringList ids;
    while (q.next())
        ids.append(q.value(0).toString());
    q.finish();
    QVector<Job> result;
    for (const auto &id : ids) {
        const auto job = get(id);
        if (!job)
            return { };
        result.append(*job);
    }
    return result;
}

std::optional<Lease> Store::claim(const QString &id, const QString &owner, qint64 now, qint64 duration)
{
    if (!ready())
        return { };
    if (owner.trimmed().isEmpty() || owner.size() > 256 || !validTime(now, duration)) {
        error = QStringLiteral("Invalid owner or lease duration.");
        return { };
    }
    Lease lease { id, QUuid::createUuid().toString(QUuid::WithoutBraces), owner };
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE jobs SET state=?,attempts=attempts+1,token=?,owner=?,lease_start=?,expires=?,error='' WHERE id=? AND state=?"));
    q.addBindValue(int(State::Running));
    q.addBindValue(lease.token);
    q.addBindValue(owner);
    q.addBindValue(now);
    q.addBindValue(now + duration);
    q.addBindValue(id);
    q.addBindValue(int(State::Queued));
    if (!q.exec() || q.numRowsAffected() != 1) {
        error = q.lastError().isValid() ? q.lastError().text() : QStringLiteral("Job is not queued.");
        return { };
    }
    return lease;
}

bool Store::fence(const Lease &lease, qint64 now)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT 1 FROM jobs WHERE id=? AND state=? AND token=? AND owner=? AND lease_start<=? AND expires>?"));
    q.addBindValue(lease.jobId);
    q.addBindValue(int(State::Running));
    q.addBindValue(lease.token);
    q.addBindValue(lease.owner);
    q.addBindValue(now);
    q.addBindValue(now);
    if (q.exec() && q.next())
        return true;
    error = q.lastError().isValid() ? q.lastError().text() : QStringLiteral("Expired or superseded job lease.");
    return false;
}

bool Store::heartbeat(const Lease &lease, qint64 now, qint64 duration)
{
    if (!ready())
        return false;
    if (!validTime(now, duration)) {
        error = QStringLiteral("Invalid lease duration.");
        return false;
    }
    if (!begin())
        return false;
    if (!fence(lease, now))
        return rollback(error);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE jobs SET expires=MAX(expires,?) WHERE id=?"));
    q.addBindValue(now + duration);
    q.addBindValue(lease.jobId);
    if (!q.exec())
        return rollback(q.lastError().text());
    return commit();
}

bool Store::recordPage(const Lease &lease, const PageReceipt &page, qint64 now)
{
    if (!ready())
        return false;
    if (!fingerprint(page.cacheKey) || !fingerprint(page.resultSha256) || (page.actualDevice != QStringLiteral("cpu") && page.actualDevice != QStringLiteral("gpu:0"))) {
        error = QStringLiteral("Invalid validated-page receipt.");
        return false;
    }
    if (!begin())
        return false;
    if (!fence(lease, now))
        return rollback(error);
    const auto job = get(lease.jobId);
    if (!job || !job->spec.pages.contains(page.page))
        return rollback(QStringLiteral("Page is outside the selected plan."));
    for (const auto &saved : job->pages) {
        if (saved.page == page.page) {
            if (saved.cacheKey != page.cacheKey || saved.resultSha256 != page.resultSha256 || saved.actualDevice != page.actualDevice)
                return rollback(QStringLiteral("Refusing to replace committed page evidence."));
            return commit();
        }
    }
    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT INTO pages(job_id,page,cache_key,result_sha256,actual_device) VALUES(?,?,?,?,?)"));
    q.addBindValue(lease.jobId);
    q.addBindValue(page.page);
    q.addBindValue(page.cacheKey);
    q.addBindValue(page.resultSha256);
    q.addBindValue(page.actualDevice);
    if (!q.exec())
        return rollback(q.lastError().text());
    return commit();
}

bool Store::setFinished(const Lease &lease, State state, const QString &message)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE jobs SET state=?,token='',owner='',expires=0,error=? WHERE id=?"));
    q.addBindValue(int(state));
    q.addBindValue(message.isNull() ? QStringLiteral("") : message);
    q.addBindValue(lease.jobId);
    if (!q.exec())
        return rollback(q.lastError().text());
    return commit();
}

bool Store::finish(const Lease &lease, bool hasPageCandidates, qint64 now)
{
    if (!ready() || !begin())
        return false;
    if (!fence(lease, now))
        return rollback(error);
    const auto job = get(lease.jobId);
    if (!job || job->pages.size() != job->spec.pages.size())
        return rollback(QStringLiteral("Incomplete work cannot enter candidate review."));
    return setFinished(lease, hasPageCandidates ? State::PageReview : State::FilenameReview, { });
}

bool Store::fail(const Lease &lease, const QString &message, qint64 now)
{
    if (!ready())
        return false;
    if (message.trimmed().isEmpty() || message.size() > 16384) {
        error = QStringLiteral("A bounded failure message is required.");
        return false;
    }
    if (!begin())
        return false;
    if (!fence(lease, now))
        return rollback(error);
    return setFinished(lease, State::Failed, message);
}

bool Store::transition(const QString &id, State target)
{
    if (!ready() || !begin())
        return false;
    const auto job = get(id);
    if (!job)
        return rollback(error);
    const State from = job->state;
    const bool resumable = from == State::Paused || from == State::Cancelled || from == State::Failed || from == State::Interrupted;
    const bool allowed = target == State::Queued ? resumable
            : target == State::Paused            ? (from == State::Queued || from == State::Running)
                                                 : (from == State::Queued || from == State::Running || resumable);
    if (!allowed)
        return rollback(QStringLiteral("This job state does not allow that transition."));
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE jobs SET state=?,token='',owner='',expires=0 WHERE id=?"));
    q.addBindValue(int(target));
    q.addBindValue(id);
    if (!q.exec())
        return rollback(q.lastError().text());
    return commit();
}

bool Store::pause(const QString &id)
{
    return transition(id, State::Paused);
}
bool Store::cancel(const QString &id)
{
    return transition(id, State::Cancelled);
}
bool Store::resume(const QString &id)
{
    return transition(id, State::Queued);
}

bool Store::interruptExpired(qint64 now)
{
    if (!ready())
        return false;
    if (now < 0) {
        error = QStringLiteral("Invalid recovery time.");
        return false;
    }
    QSqlQuery q(db);
    // A backwards wall-clock jump is also interrupted rather than extending ownership.
    q.prepare(QStringLiteral("UPDATE jobs SET state=?,token='',owner='',expires=0,error=? WHERE state=? AND (expires<=? OR lease_start>?)"));
    q.addBindValue(int(State::Interrupted));
    q.addBindValue(QStringLiteral("Worker lease ended; verify worker exit before explicit resume."));
    q.addBindValue(int(State::Running));
    q.addBindValue(now);
    q.addBindValue(now);
    if (q.exec())
        return true;
    error = q.lastError().text();
    return false;
}
}
