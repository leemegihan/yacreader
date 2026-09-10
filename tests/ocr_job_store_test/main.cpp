#include "ocr_job_store.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <cstdlib>
#include <thread>

using namespace OcrJobs;

namespace {
QJsonObject snapshot()
{
    // Paths refer only to a synthetic namespace; this store never opens them.
    const QString root = QDir::temp().absoluteFilePath(QStringLiteral("synthetic-ocr-snapshot"));
    const QJsonObject runtime {
        { "executable", root + "/python.exe" }, { "dataPath", root + "/models" }, { "executableSha256", QString(64, u'1') }, { "workerSha256", QString(64, u'2') }, { "modelManifestSha256", QString(64, u'3') }, { "packageManifestSha256", QString(64, u'4') }
    };
    return {
        { "version", 1 },
        { "options", QJsonObject { { "neural", true }, { "gpu", true }, { "cpuThreads", 8 }, { "executable", "" }, { "dataPath", "" }, { "language", "auto" }, { "vertical", false }, { "segmentation", 11 }, { "rotation", 0 }, { "invert", false }, { "adaptiveThreshold", false }, { "timeoutMs", 90000 } } },
        { "environment", QJsonObject { { "platform", "windows-x64" }, { "applicationSha256", QString(64, u'5') }, { "preprocessingRevision", "synthetic-preprocess-1" }, { "cpu", runtime }, { "gpu", runtime } } }
    };
}

Spec sample()
{
    Spec s;
    s.libraryGeneration = QStringLiteral("synthetic-generation-1");
    s.comicId = QStringLiteral("42");
    s.sourceSnapshot = QString(64, u'a');
    s.settingsSnapshot = snapshot();
    s.settingsFingerprint = settingsFingerprint(s.settingsSnapshot);
    s.sourceContext = { { "path", "synthetic/book.cbz" }, { "libraryRoot", "synthetic" }, { "sourceKind", "archive" } };
    s.totalPages = 10;
    s.pages = { 1, 2, 3, 8, 9, 10 };
    return s;
}
PageReceipt receipt(int page)
{
    return { page, QString(64, u'c'), QString(64, u'd'), QStringLiteral("gpu:0") };
}
bool write(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::NewOnly) && f.write(bytes) == bytes.size();
}
QByteArray read(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
QString database(const QTemporaryDir &dir)
{
    return dir.filePath(QStringLiteral("ocr-jobs.sqlite"));
}
void start(QProcess &p, const QStringList &arguments)
{
#ifdef Q_OS_WIN
    p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= 0x08000000; });
#endif
    p.start(QCoreApplication::applicationFilePath(), arguments);
}
bool waitFile(const QString &path)
{
    QElapsedTimer timer;
    timer.start();
    while (!QFile::exists(path) && timer.elapsed() < 10000)
        QThread::msleep(10);
    return QFile::exists(path);
}

int child(const QStringList &args)
{
    Store store;
    if (!store.open(args.value(2)))
        return 80;
    const QString mode = args.value(1);
    if (mode == QStringLiteral("--child-claim")) {
        if (!write(args.value(4), "ready") || !waitFile(args.value(5)))
            return 81;
        const auto lease = store.claim(args.value(3), args.value(4), 1000, 1000);
        return write(args.value(6), lease ? lease->token.toUtf8() : QByteArray("not-claimed")) ? 0 : 82;
    }
    if (mode == QStringLiteral("--child-stale")) {
        const Lease lease { args.value(3), args.value(4), args.value(5) };
        return store.recordPage(lease, receipt(8), 3001) ? 83 : 0;
    }
    if (mode == QStringLiteral("--child-crash")) {
        const auto lease = store.claim(args.value(3), QStringLiteral("crashed-worker"), 1000, 1000);
        if (!lease)
            return 84;
        for (int page : { 1, 2, 3 })
            if (!store.recordPage(*lease, receipt(page), 1001))
                return 85;
        if (!write(args.value(4), lease->token.toUtf8()))
            return 86;
        // Deliberately die inside an uncommitted transaction on another connection.
        // Previously committed receipts must survive; page eight must not.
        auto raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("crash-injector"));
        raw.setDatabaseName(args.value(2));
        if (!raw.open())
            return 87;
        QSqlQuery q(raw);
        if (!q.exec(QStringLiteral("BEGIN IMMEDIATE")))
            return 88;
        q.prepare(QStringLiteral("INSERT INTO pages VALUES(?,8,?,?,?)"));
        q.addBindValue(args.value(3));
        q.addBindValue(QString(64, u'c'));
        q.addBindValue(QString(64, u'd'));
        q.addBindValue(QStringLiteral("gpu:0"));
        if (!q.exec())
            return 89;
        std::_Exit(77);
    }
    return 90;
}
}

class OcrJobStoreTest : public QObject
{
    Q_OBJECT
private slots:
    void settingsSnapshotsSurviveRestart();
    void settingsRejectIncompleteAndChangedEvidence();
    void legacySchemaIsPreserved();
    void duplicateRegistrationAndContext();
    void boundedPlanValidation();
    void pauseAndResumePersist();
    void completionRequiresEveryPage();
    void failureAndCancellationAreNotReview();
    void leaseFencingAndClockChanges();
    void receiptsAreImmutable();
    void rejectsForeignAndFutureDatabases();
    void corruptRecordsAreRejected();
    void lockedWritesDoNotAdvanceState();
    void currentClocksAfterContention_data();
    void currentClocksAfterContention();
    void wrongThreadIsRejected();
    void competingProcesses();
    void abruptExitAndStaleProcess();
};

void OcrJobStoreTest::settingsSnapshotsSurviveRestart()
{
    QTemporaryDir dir;
    QString id;
    auto expected = sample();
    auto options = expected.settingsSnapshot["options"].toObject();
    options["language"] = "jpn+eng";
    options["cpuThreads"] = 16;
    options["rotation"] = 270;
    options["invert"] = true;
    options["adaptiveThreshold"] = true;
    options["vertical"] = true;
    options["segmentation"] = 5;
    options["timeoutMs"] = 123456;
    expected.settingsSnapshot["options"] = options;
    expected.settingsFingerprint = settingsFingerprint(expected.settingsSnapshot);
    QVERIFY(!expected.settingsFingerprint.isEmpty());
    {
        Store store;
        QVERIFY(store.open(database(dir)));
        const auto added = store.enqueue(expected);
        QVERIFY(added);
        id = *added;
        QVERIFY(store.pause(id));
    }
    Store reopened;
    QVERIFY(reopened.open(database(dir)));
    auto job = reopened.get(id);
    QVERIFY(job);
    QCOMPARE(job->spec.settingsSnapshot, expected.settingsSnapshot);
    QCOMPARE(job->spec.settingsFingerprint, expected.settingsFingerprint);
    QCOMPARE(settingsFingerprint(job->spec.settingsSnapshot), expected.settingsFingerprint);
    QVERIFY(reopened.resume(id));
    QVERIFY(reopened.claim(id, QStringLiteral("recovered"), 1000, 1000));
    QCOMPARE(reopened.get(id)->spec.settingsSnapshot, expected.settingsSnapshot);

    // Explicitly absent GPU remains reproducible CPU-fallback intent.
    auto environment = expected.settingsSnapshot["environment"].toObject();
    environment["gpu"] = QJsonValue(QJsonValue::Null);
    expected.settingsSnapshot["environment"] = environment;
    expected.settingsFingerprint = settingsFingerprint(expected.settingsSnapshot);
    QVERIFY(!expected.settingsFingerprint.isEmpty());
    const auto fallback = reopened.enqueue(expected);
    QVERIFY(fallback && *fallback != id);

    // Tesseract snapshots resolve their actual executable and trained-data path.
    options["neural"] = false;
    options["gpu"] = false;
    options["language"] = "jpn_vert+eng";
    auto cpu = environment["cpu"].toObject();
    cpu["workerSha256"] = QJsonValue(QJsonValue::Null);
    environment["cpu"] = cpu;
    options["executable"] = cpu["executable"];
    options["dataPath"] = cpu["dataPath"];
    expected.settingsSnapshot["options"] = options;
    expected.settingsSnapshot["environment"] = environment;
    expected.settingsFingerprint = settingsFingerprint(expected.settingsSnapshot);
    QVERIFY(!expected.settingsFingerprint.isEmpty());
    QVERIFY(reopened.enqueue(expected));
}

void OcrJobStoreTest::settingsRejectIncompleteAndChangedEvidence()
{
    QTemporaryDir dir;
    Store store;
    QVERIFY(store.open(database(dir)));
    const auto original = sample();
    QVERIFY(store.enqueue(original));
    auto reject = [&](const QJsonObject &value) {
        auto changed = original;
        changed.settingsSnapshot = value;
        changed.settingsFingerprint = settingsFingerprint(value);
        return changed.settingsFingerprint.isEmpty() && !store.enqueue(changed);
    };
    for (const QString &section : { QStringLiteral("options"), QStringLiteral("environment") }) {
        const auto object = original.settingsSnapshot[section].toObject();
        for (const QString &key : object.keys()) {
            auto missing = object;
            missing.remove(key);
            auto value = original.settingsSnapshot;
            value[section] = missing;
            QVERIFY2(reject(value), qPrintable(section + "/" + key));
        }
    }
    for (const auto badValue : { QJsonValue("8"), QJsonValue(8.5), QJsonValue(0), QJsonValue(17), QJsonValue(true) }) {
        auto value = snapshot();
        auto options = value["options"].toObject();
        options["cpuThreads"] = badValue;
        value["options"] = options;
        QVERIFY(reject(value));
    }
    auto value = snapshot();
    value["version"] = 2;
    QVERIFY(reject(value));
    value = snapshot();
    value["unexpected"] = true;
    QVERIFY(reject(value));
    const QJsonObject invalidOptions { { "gpu", 1 }, { "rotation", 45 }, { "timeoutMs", 0 }, { "segmentation", 12 }, { "language", "jpn" }, { "executable", "relative.exe" } };
    for (auto entry = invalidOptions.begin(); entry != invalidOptions.end(); ++entry) {
        value = snapshot();
        auto options = value["options"].toObject();
        options[entry.key()] = entry.value();
        value["options"] = options;
        QVERIFY(reject(value));
    }
    for (const QString &key : snapshot()["environment"].toObject()["cpu"].toObject().keys()) {
        value = snapshot();
        auto environment = value["environment"].toObject();
        auto cpu = environment["cpu"].toObject();
        cpu.remove(key);
        environment["cpu"] = cpu;
        value["environment"] = environment;
        QVERIFY2(reject(value), qPrintable(key));
    }
    auto incomplete = original;
    incomplete.settingsSnapshot = { };
    QVERIFY(!store.enqueue(incomplete)); // A legacy fingerprint alone cannot restore options.

    for (const QString &key : { QStringLiteral("executableSha256"), QStringLiteral("workerSha256"), QStringLiteral("modelManifestSha256"), QStringLiteral("packageManifestSha256") }) {
        value = snapshot();
        auto environment = value["environment"].toObject();
        auto cpu = environment["cpu"].toObject();
        cpu[key] = QString(64, u'9');
        environment["cpu"] = cpu;
        value["environment"] = environment;
        auto changed = original;
        changed.settingsSnapshot = value;
        QVERIFY(!store.enqueue(changed)); // Valid new environment with a stale fingerprint.
        changed.settingsFingerprint = settingsFingerprint(value);
        QVERIFY(changed.settingsFingerprint != original.settingsFingerprint);
        const auto added = store.enqueue(changed);
        QVERIFY(added);
        QVERIFY(*added != *store.enqueue(original));
        cpu[key] = "unverified";
        environment["cpu"] = cpu;
        value["environment"] = environment;
        QVERIFY(reject(value));
    }
    QCOMPARE(store.list().size(), 5);
}

void OcrJobStoreTest::legacySchemaIsPreserved()
{
    QTemporaryDir dir;
    const auto path = database(dir);
    {
        Store current;
        QVERIFY(current.open(path));
        QVERIFY(current.enqueue(sample()));
    }
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("legacy-snapshot-fixture"));
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("PRAGMA user_version=1")));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("legacy-snapshot-fixture"));
    const auto before = read(path);
    QVERIFY(!before.isEmpty());
    Store legacy;
    QVERIFY(!legacy.open(path));
    QVERIFY(legacy.lastError().contains(QStringLiteral("schema")));
    QCOMPARE(read(path), before);
    QVERIFY(!QFile::exists(path + QStringLiteral("-wal")));
}

void OcrJobStoreTest::duplicateRegistrationAndContext()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Store store;
    QVERIFY2(store.open(database(dir)), qPrintable(store.lastError()));
    const auto first = store.enqueue(sample());
    QVERIFY(first);
    QVERIFY(store.enqueue(sample()) == first);
    QCOMPARE(store.list().size(), 1);
    auto changed = sample();
    changed.libraryGeneration += u'2';
    QVERIFY(store.enqueue(changed) != first);
    changed = sample();
    changed.sourceContext["path"] = "synthetic/renamed.cbz";
    QVERIFY(store.enqueue(changed) != first);
    changed = sample();
    changed.sourceSnapshot = QString(64, u'e');
    QVERIFY(store.enqueue(changed) != first);
    changed = sample();
    auto options = changed.settingsSnapshot["options"].toObject();
    options["cpuThreads"] = 4;
    changed.settingsSnapshot["options"] = options;
    changed.settingsFingerprint = settingsFingerprint(changed.settingsSnapshot);
    const auto changedSettings = store.enqueue(changed);
    QVERIFY(changedSettings);
    QVERIFY(changedSettings != first);
    changed = sample();
    changed.totalPages = 11;
    changed.pages = { 1, 2, 3, 9, 10, 11 };
    QVERIFY(store.enqueue(changed) != first);
    QCOMPARE(store.list().size(), 6);
}

void OcrJobStoreTest::boundedPlanValidation()
{
    QTemporaryDir dir;
    Store store;
    QVERIFY(store.open(database(dir)));
    for (const QVector<int> &pages : { QVector<int> { }, { 0 }, { 1, 1 }, { 3, 2 }, { 4 }, { 11 }, { 1, 2, 3, 7, 8, 9, 10 } }) {
        auto s = sample();
        s.pages = pages;
        QVERIFY(!store.enqueue(s));
    }
    auto s = sample();
    s.totalPages = 2;
    s.pages = { 1, 2 };
    QVERIFY(store.enqueue(s));
    s.sourceContext.remove("libraryRoot");
    QVERIFY(!store.enqueue(s));
    s = sample();
    s.settingsFingerprint = QStringLiteral("unknown");
    QVERIFY(!store.enqueue(s));
    s = sample();
    s.sourceContext["extra"] = QString(70000, u'x');
    QVERIFY(!store.enqueue(s));
}

void OcrJobStoreTest::pauseAndResumePersist()
{
    QTemporaryDir dir;
    QString id;
    Lease old;
    {
        Store store;
        QVERIFY(store.open(database(dir)));
        const auto registered = store.enqueue(sample());
        QVERIFY(registered);
        id = *registered;
        const auto lease = store.claim(id, QStringLiteral("worker-1"), 1000, 1000);
        QVERIFY(lease);
        old = *lease;
        QVERIFY(store.recordPage(old, receipt(1), 1001));
        QVERIFY(store.pause(id));
        QVERIFY(!store.recordPage(old, receipt(2), 1002));
    }
    Store reopened;
    QVERIFY2(reopened.open(database(dir)), qPrintable(reopened.lastError()));
    const auto job = reopened.get(id);
    QVERIFY(job);
    QCOMPARE(job->state, State::Paused);
    QCOMPARE(job->pages.size(), 1);
    QVERIFY(!reopened.claim(id, QStringLiteral("worker-2"), 1100, 1000));
    QVERIFY(reopened.resume(id));
    const auto current = reopened.claim(id, QStringLiteral("worker-2"), 1100, 1000);
    QVERIFY(current);
    QVERIFY(current->token != old.token);
    QVERIFY(!reopened.heartbeat(old, 1101, 1000));
    QVERIFY(reopened.recordPage(*current, receipt(2), 1102));
    QCOMPARE(reopened.get(id)->attempts, 2);
    QCOMPARE(reopened.get(id)->pages.size(), 2);
}

void OcrJobStoreTest::completionRequiresEveryPage()
{
    QTemporaryDir dir;
    Store store;
    QVERIFY(store.open(database(dir)));
    for (bool hasCandidates : { false, true }) {
        auto s = sample();
        s.comicId = hasCandidates ? QStringLiteral("with-candidates") : QStringLiteral("without-candidates");
        const auto id = store.enqueue(s);
        QVERIFY(id);
        const auto lease = store.claim(*id, QStringLiteral("worker"), 1000, 1000);
        QVERIFY(lease);
        QVERIFY(!store.finish(*lease, hasCandidates, 1001));
        for (int page : s.pages)
            QVERIFY(store.recordPage(*lease, receipt(page), 1002));
        QVERIFY(store.finish(*lease, hasCandidates, 1003));
        QCOMPARE(store.get(*id)->state, hasCandidates ? State::PageReview : State::FilenameReview);
        QVERIFY(!store.pause(*id));
        QVERIFY(!store.resume(*id));
        QVERIFY(!store.cancel(*id));
        QVERIFY(!store.recordPage(*lease, receipt(1), 1004));
    }
}

void OcrJobStoreTest::failureAndCancellationAreNotReview()
{
    QTemporaryDir dir;
    Store store;
    QVERIFY(store.open(database(dir)));
    const auto id = store.enqueue(sample());
    QVERIFY(id);
    auto lease = store.claim(*id, QStringLiteral("worker"), 1000, 1000);
    QVERIFY(lease);
    QVERIFY(store.recordPage(*lease, receipt(1), 1001));
    QVERIFY(!store.fail(*lease, { }, 1002));
    QVERIFY(store.fail(*lease, QStringLiteral("Synthetic engine failure"), 1002));
    QCOMPARE(store.get(*id)->state, State::Failed);
    QVERIFY(store.interruptExpired(5000));
    QCOMPARE(store.get(*id)->state, State::Failed);
    QVERIFY(store.resume(*id));
    lease = store.claim(*id, QStringLiteral("worker"), 5000, 1000);
    QVERIFY(lease);
    QVERIFY(store.cancel(*id));
    QVERIFY(!store.finish(*lease, false, 5001));
    QVERIFY(store.interruptExpired(10000));
    QCOMPARE(store.get(*id)->state, State::Cancelled);
    QVERIFY(!store.claim(*id, QStringLiteral("worker"), 10000, 1000));
    QVERIFY(store.resume(*id));
    QCOMPARE(store.get(*id)->pages.size(), 1);
}

void OcrJobStoreTest::leaseFencingAndClockChanges()
{
    QTemporaryDir dir;
    Store store;
    QVERIFY(store.open(database(dir)));
    const auto id = store.enqueue(sample());
    QVERIFY(id);
    QVERIFY(!store.claim(*id, QStringLiteral(""), 1000, 1000));
    QVERIFY(!store.claim(*id, QStringLiteral("owner"), -1, 1000));
    QVERIFY(!store.claim(*id, QStringLiteral("owner"), 1000, 0));
    QVERIFY(!store.claim(*id, QStringLiteral("owner"), 1000, 3600001));
    auto lease = store.claim(*id, QStringLiteral("owner"), 1000, 1000);
    QVERIFY(lease);
    Lease forged = *lease;
    forged.owner = QStringLiteral("other");
    QVERIFY(!store.recordPage(forged, receipt(1), 1001));
    QVERIFY(store.heartbeat(*lease, 1500, 1000));
    QVERIFY(store.heartbeat(*lease, 1501, 1)); // Must not shorten the lease.
    QVERIFY(store.recordPage(*lease, receipt(1), 2400));
    QVERIFY(!store.heartbeat(*lease, 2500, 1000)); // Expiry is exclusive.
    QVERIFY(store.interruptExpired(2500));
    QCOMPARE(store.get(*id)->state, State::Interrupted);
    QVERIFY(!store.claim(*id, QStringLiteral("other"), 3000, 1000));
    QVERIFY(store.resume(*id));
    lease = store.claim(*id, QStringLiteral("other"), 3000, 1000);
    QVERIFY(lease);
    QVERIFY(!store.recordPage(*lease, receipt(2), 2999));
    QVERIFY(store.interruptExpired(2999)); // Clock moved behind acquisition.
    QCOMPARE(store.get(*id)->state, State::Interrupted);
}

void OcrJobStoreTest::receiptsAreImmutable()
{
    QTemporaryDir dir;
    Store store;
    QVERIFY(store.open(database(dir)));
    const auto id = store.enqueue(sample());
    QVERIFY(id);
    const auto lease = store.claim(*id, QStringLiteral("worker"), 1000, 1000);
    QVERIFY(lease);
    QVERIFY(store.recordPage(*lease, receipt(1), 1001));
    QVERIFY(store.recordPage(*lease, receipt(1), 1002));
    auto changed = receipt(1);
    changed.actualDevice = QStringLiteral("cpu");
    QVERIFY(!store.recordPage(*lease, changed, 1003));
    changed = receipt(2);
    changed.resultSha256 = QStringLiteral("not-a-hash");
    QVERIFY(!store.recordPage(*lease, changed, 1003));
    changed = receipt(2);
    changed.actualDevice = QStringLiteral("requested-gpu");
    QVERIFY(!store.recordPage(*lease, changed, 1003));
    QVERIFY(!store.recordPage(*lease, receipt(4), 1003));
    QCOMPARE(store.get(*id)->pages.size(), 1);
    QCOMPARE(store.get(*id)->pages.first().actualDevice, QStringLiteral("gpu:0"));
}

void OcrJobStoreTest::rejectsForeignAndFutureDatabases()
{
    QTemporaryDir dir;
    {
        Store wrongName;
        QVERIFY(!wrongName.open(dir.filePath(QStringLiteral("library.ydb"))));
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("library.ydb"))));
        Store relative;
        QVERIFY(!relative.open(QStringLiteral("ocr-jobs.sqlite")));
    }
    const auto path = database(dir);
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("foreign-fixture"));
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("CREATE TABLE private_library(value TEXT)")));
        QVERIFY(q.exec(QStringLiteral("INSERT INTO private_library VALUES('synthetic-only')")));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("foreign-fixture"));
    const auto before = read(path);
    {
        Store foreign;
        QVERIFY(!foreign.open(path));
        QVERIFY(!foreign.enqueue(sample()));
    }
    QCOMPARE(read(path), before);
    QVERIFY(!QFile::exists(path + QStringLiteral("-wal")));
    QTemporaryDir futureDir;
    {
        Store current;
        QVERIFY(current.open(database(futureDir)));
    }
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("future-fixture"));
        db.setDatabaseName(database(futureDir));
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("PRAGMA user_version=999")));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("future-fixture"));
    Store future;
    QVERIFY(!future.open(database(futureDir)));
    QVERIFY(!future.enqueue(sample()));
}

void OcrJobStoreTest::corruptRecordsAreRejected()
{
    QTemporaryDir dir;
    Store store;
    QVERIFY(store.open(database(dir)));
    const auto id = store.enqueue(sample());
    QVERIFY(id);
    auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("corrupt-fixture"));
    db.setDatabaseName(database(dir));
    QVERIFY(db.open());
    {
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("UPDATE jobs SET spec=X'7b7d'")));
    }
    QVERIFY(!store.get(*id));
    QVERIFY(!store.enqueue(sample())); // An existing damaged duplicate is not success.
    QVERIFY(!store.claim(*id, QStringLiteral("worker"), 1000, 1000));
    db.close();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(QStringLiteral("corrupt-fixture"));

    QTemporaryDir receiptDir;
    Store receipts;
    QVERIFY(receipts.open(database(receiptDir)));
    const auto goodId = receipts.enqueue(sample());
    QVERIFY(goodId);
    const auto lease = receipts.claim(*goodId, QStringLiteral("worker"), 1000, 1000);
    QVERIFY(lease);
    for (int page : sample().pages)
        QVERIFY(receipts.recordPage(*lease, receipt(page), 1001));
    db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("corrupt-receipt"));
    db.setDatabaseName(database(receiptDir));
    QVERIFY(db.open());
    {
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("UPDATE pages SET result_sha256='damaged' WHERE page=1")));
    }
    QVERIFY(!receipts.get(*goodId));
    QVERIFY(!receipts.finish(*lease, false, 1002));
    db.close();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(QStringLiteral("corrupt-receipt"));
}

void OcrJobStoreTest::lockedWritesDoNotAdvanceState()
{
    QTemporaryDir dir;
    Store store;
    QVERIFY(store.open(database(dir)));
    const auto id = store.enqueue(sample());
    QVERIFY(id);
    const auto lease = store.claim(*id, QStringLiteral("worker"), 1000, 1000);
    QVERIFY(lease);
    auto blocker = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("write-blocker"));
    blocker.setDatabaseName(database(dir));
    QVERIFY(blocker.open());
    {
        QSqlQuery q(blocker);
        QVERIFY(q.exec(QStringLiteral("BEGIN IMMEDIATE")));
        // WAL readers still see committed state while another connection owns the writer.
        QCOMPARE(store.get(*id)->state, State::Running);
        QVERIFY(!store.pause(*id));
        QVERIFY(!store.lastError().isEmpty());
        QCOMPARE(store.get(*id)->state, State::Running);
        QVERIFY(q.exec(QStringLiteral("ROLLBACK")));
    }
    QVERIFY(store.pause(*id));
    QVERIFY(!store.recordPage(*lease, receipt(1), 1001));
    QCOMPARE(store.get(*id)->state, State::Paused);
    blocker.close();
    blocker = QSqlDatabase();
    QSqlDatabase::removeDatabase(QStringLiteral("write-blocker"));
}

void OcrJobStoreTest::currentClocksAfterContention_data()
{
    QTest::addColumn<QString>("operation");
    for (const auto &name : { "claim", "heartbeat", "finish", "fail", "interrupt" })
        QTest::newRow(name) << QString::fromLatin1(name);
}

void OcrJobStoreTest::currentClocksAfterContention()
{
    QFETCH(QString, operation);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Store store;
    QVERIFY(store.open(database(dir)));
    const auto id = store.enqueue(sample());
    QVERIFY(id);
    std::optional<Lease> lease;
    if (operation != "claim") {
        lease = store.claim(*id, QStringLiteral("owner"), 1000, 100);
        QVERIFY(lease);
        for (const int page : sample().pages)
            QVERIFY(store.recordPage(*lease, receipt(page), 1001));
    }
    // Empty clocks must fail before acquiring a transaction or changing state.
    QVERIFY(!store.claimWhenCurrent(*id, QStringLiteral("owner"), 100, { }));
    QVERIFY(!store.heartbeatWhenCurrent(lease.value_or(Lease { }), 100, { }));
    QVERIFY(!store.finishWhenCurrent(lease.value_or(Lease { }), false, { }));
    QVERIFY(!store.failWhenCurrent(lease.value_or(Lease { }), QStringLiteral("failure"), { }));
    QVERIFY(!store.interruptExpiredWhenCurrent({ }));

    std::atomic<int> ready { 0 };
    std::atomic_bool proceed { false }, writerOk { false };
    std::atomic<qint64> now { operation == "interrupt" ? 999 : 1001 };
    const auto dbPath = database(dir);
    std::thread writer([&] {
        const auto connection = QStringLiteral("clock-blocker");
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(dbPath);
            const bool opened = db.open();
            QSqlQuery query(db);
            if (!opened || !query.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
                ready.store(-1);
            } else {
                ready.store(1);
                QElapsedTimer timer;
                timer.start();
                while (!proceed.load() && timer.elapsed() < 5000)
                    QThread::msleep(1);
                QThread::msleep(200);
                now.store(operation == "interrupt" ? 1001 : 1101);
                writerOk.store(query.exec(QStringLiteral("COMMIT")));
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
    });
    const auto join = qScopeGuard([&] {
        proceed.store(true);
        if (writer.joinable())
            writer.join();
    });
    QElapsedTimer timer;
    timer.start();
    while (ready.load() == 0 && timer.elapsed() < 5000)
        QThread::msleep(1);
    QCOMPARE(ready.load(), 1);
    int samples = 0;
    const auto clock = [&] { ++samples; return now.load(); };
    proceed.store(true);
    if (operation == "claim") {
        const auto claimed = store.claimWhenCurrent(*id, QStringLiteral("new-owner"), 100, clock);
        QVERIFY(claimed);
        // A pre-lock timestamp would create a lease already expired at 1102.
        QVERIFY(store.recordPage(*claimed, receipt(1), 1102));
    } else if (operation == "interrupt") {
        QVERIFY(store.interruptExpiredWhenCurrent(clock));
        // The old pre-lock time 999 would falsely look like a backwards clock jump.
        QCOMPARE(store.get(*id)->state, State::Running);
    } else {
        const bool changed = operation == "heartbeat" ? store.heartbeatWhenCurrent(*lease, 1000, clock)
                : operation == "finish"               ? store.finishWhenCurrent(*lease, false, clock)
                                                      : store.failWhenCurrent(*lease, QStringLiteral("failure"), clock);
        QVERIFY(!changed);
        QVERIFY(!store.lastError().isEmpty());
        QCOMPARE(store.get(*id)->state, State::Running);
        QCOMPARE(store.get(*id)->pages.size(), sample().pages.size());
        // A rejected write must leave no transaction behind.
        QVERIFY(store.interruptExpired(1101));
        QCOMPARE(store.get(*id)->state, State::Interrupted);
    }
    writer.join();
    QVERIFY(writerOk.load());
    QCOMPARE(samples, 1);
}

void OcrJobStoreTest::wrongThreadIsRejected()
{
    QTemporaryDir dir;
    Store store;
    QVERIFY(store.open(database(dir)));
    bool rejected = false;
    std::thread other([&] { rejected = !store.enqueue(sample()).has_value(); });
    other.join();
    QVERIFY(rejected);
    QVERIFY(store.enqueue(sample()));
}

void OcrJobStoreTest::competingProcesses()
{
    QTemporaryDir dir;
    Store store;
    QVERIFY(store.open(database(dir)));
    const auto id = store.enqueue(sample());
    QVERIFY(id);
    QProcess first, second;
    const auto gate = dir.filePath(QStringLiteral("gate"));
    const auto ready1 = dir.filePath(QStringLiteral("ready-1"));
    const auto ready2 = dir.filePath(QStringLiteral("ready-2"));
    const auto output1 = dir.filePath(QStringLiteral("claim-1"));
    const auto output2 = dir.filePath(QStringLiteral("claim-2"));
    start(first, { "--child-claim", database(dir), *id, ready1, gate, output1 });
    start(second, { "--child-claim", database(dir), *id, ready2, gate, output2 });
    QVERIFY(first.waitForStarted());
    QVERIFY(second.waitForStarted());
    QVERIFY(waitFile(ready1));
    QVERIFY(waitFile(ready2));
    QVERIFY(write(gate, "go"));
    QVERIFY(first.waitForFinished(15000));
    QVERIFY(second.waitForFinished(15000));
    QCOMPARE(first.exitCode(), 0);
    QCOMPARE(second.exitCode(), 0);
    const auto a = read(output1), b = read(output2);
    QVERIFY(!a.isEmpty() && !b.isEmpty());
    QVERIFY((a == "not-claimed") != (b == "not-claimed"));
    QCOMPARE(store.get(*id)->attempts, 1);
}

void OcrJobStoreTest::abruptExitAndStaleProcess()
{
    QTemporaryDir dir;
    QString id;
    {
        Store store;
        QVERIFY(store.open(database(dir)));
        const auto registered = store.enqueue(sample());
        QVERIFY(registered);
        id = *registered;
    }
    const auto tokenFile = dir.filePath(QStringLiteral("old-token"));
    QProcess crashed;
    start(crashed, { "--child-crash", database(dir), id, tokenFile });
    QVERIFY(crashed.waitForStarted());
    QVERIFY(crashed.waitForFinished(15000));
    QCOMPARE(crashed.exitCode(), 77);
    Store reopened;
    QVERIFY2(reopened.open(database(dir)), qPrintable(reopened.lastError()));
    auto job = reopened.get(id);
    QVERIFY(job);
    QCOMPARE(job->pages.size(), 3);
    QCOMPARE(job->state, State::Running);
    QVERIFY(reopened.interruptExpired(2000));
    QCOMPARE(reopened.get(id)->state, State::Interrupted);
    QVERIFY(reopened.resume(id)); // The child exit above has been confirmed.
    const auto lease = reopened.claim(id, QStringLiteral("replacement"), 3000, 1000);
    QVERIFY(lease);
    QProcess stale;
    start(stale, { "--child-stale", database(dir), id, QString::fromUtf8(read(tokenFile)), "crashed-worker" });
    QVERIFY(stale.waitForStarted());
    QVERIFY(stale.waitForFinished(15000));
    QCOMPARE(stale.exitCode(), 0);
    QVERIFY(!reopened.finish(*lease, false, 3001));
    for (int page : { 8, 9, 10 })
        QVERIFY(reopened.recordPage(*lease, receipt(page), 3002));
    QVERIFY(reopened.finish(*lease, false, 3003));
    QCOMPARE(reopened.get(id)->state, State::FilenameReview);
    QCOMPARE(reopened.get(id)->pages.size(), 6);
    auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("integrity"));
    db.setDatabaseName(database(dir));
    QVERIFY(db.open());
    {
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("PRAGMA integrity_check")));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("ok"));
    }
    db.close();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(QStringLiteral("integrity"));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1).startsWith(QStringLiteral("--child-")))
        return child(app.arguments());
    OcrJobStoreTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "main.moc"
