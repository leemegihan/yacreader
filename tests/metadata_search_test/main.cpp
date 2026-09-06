#include "metadata_tokens.h"
#include "query_parser.h"
#include "search_query.h"

#include <QSqlError>
#include <QTest>
#include <QUuid>

#include <algorithm>
#include <stdexcept>

class MetadataSearchTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void exactTokens_data();
    void exactTokens();
    void compoundsAndLegacySearch();
    void quotedValuesRoundTrip();
    void invalidQueriesFail();
    void facetsAreNotTruncated();
    void tokenBoundariesAgree();

private:
    QList<int> matches(const QString &filter);
    void add(int id, const QString &writer, const QString &tags);
    QSqlDatabase db;
    QString connection;
};

void MetadataSearchTest::init()
{
    connection = QUuid::createUuid().toString();
    db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setConnectOptions("QSQLITE_ENABLE_REGEXP");
    db.setDatabaseName(":memory:");
    QVERIFY(db.open());
    QSqlQuery q(db);
    QVERIFY(q.exec("CREATE TABLE comic_info (id INTEGER PRIMARY KEY, writer TEXT, tags TEXT, "
                   "number TEXT, title TEXT, numPages INTEGER, hash TEXT, read INTEGER, currentPage INTEGER, "
                   "rating INTEGER, hasBeenOpened INTEGER, date TEXT, added INTEGER, type INTEGER, "
                   "lastTimeOpened INTEGER, series TEXT, volume TEXT, storyArc TEXT)"));
    QVERIFY(q.exec("CREATE TABLE comic (id INTEGER PRIMARY KEY, comicInfoId INTEGER, parentId INTEGER, fileName TEXT, path TEXT)"));
    QVERIFY(q.exec("CREATE TABLE folder (id INTEGER PRIMARY KEY, parentId INTEGER, name TEXT)"));
    QVERIFY(q.exec("INSERT INTO folder VALUES (1, 1, 'root'), (2, 1, 'Comics')"));
    add(1, "Ann", "Action, Fantasy");
    add(2, "Anna", "Live Action; Fantasy");
    add(3, " ANN ; Bob ", "Action\nHorror");
    add(4, "山田太郎、Élodie", "冒険；ＳＦ，100% Pure\rA_B");
    add(5, QString(), QString());
    add(6, "Other", "Act; Action Hero; AxB; 1000 Pure");
}

void MetadataSearchTest::cleanup()
{
    db.close();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
}

void MetadataSearchTest::add(int id, const QString &writer, const QString &tags)
{
    QSqlQuery q(db);
    q.prepare("INSERT INTO comic_info (id, writer, tags) VALUES (?, ?, ?)");
    q.addBindValue(id);
    q.addBindValue(writer);
    q.addBindValue(tags);
    QVERIFY2(q.exec(), qPrintable(q.lastError().text()));
    q.prepare("INSERT INTO comic VALUES (?, ?, 2, 'comic.cbz', '/comic.cbz')");
    q.addBindValue(id);
    q.addBindValue(id);
    QVERIFY(q.exec());
}

QList<int> MetadataSearchTest::matches(const QString &filter)
{
    auto q = comicsSearchQuery(db, filter);
    if (q.lastError().isValid()) {
        QTest::qFail(qPrintable(q.lastError().text()), __FILE__, __LINE__);
        return { };
    }
    QList<int> ids;
    while (q.next())
        ids.append(q.value(4).toInt());
    std::sort(ids.begin(), ids.end());
    return ids;
}

void MetadataSearchTest::exactTokens_data()
{
    QTest::addColumn<QString>("filter");
    QTest::addColumn<QList<int>>("expected");
    QTest::newRow("author-boundary") << "author:Ann" << QList<int> { 1, 3 };
    QTest::newRow("tag-boundary") << "tag:Action" << QList<int> { 1, 3 };
    QTest::newRow("unicode-case") << "author:élodie" << QList<int> { 4 };
    QTest::newRow("japanese-separator") << "author:山田太郎" << QList<int> { 4 };
    QTest::newRow("fullwidth-separators") << "tag:ＳＦ" << QList<int> { 4 };
    QTest::newRow("literal-percent") << "tag:\"100% Pure\"" << QList<int> { 4 };
    QTest::newRow("literal-underscore") << "tag:A_B" << QList<int> { 4 };
    QTest::newRow("unknown") << "tag:Missing" << QList<int> { };
    QTest::newRow("empty") << "tag:\"\"" << QList<int> { };
    QTest::newRow("separator-is-not-a-token") << "tag:\"Action, Fantasy\"" << QList<int> { };
    QTest::newRow("negation-includes-null") << "NOT tag:Action" << QList<int> { 2, 4, 5, 6 };
    QTest::newRow("case-insensitive-field") << "AuThOr==ann" << QList<int> { 1, 3 };
}

void MetadataSearchTest::exactTokens()
{
    QFETCH(QString, filter);
    QFETCH(QList<int>, expected);
    QCOMPARE(matches(filter), expected);
}

void MetadataSearchTest::compoundsAndLegacySearch()
{
    QCOMPARE(matches("author:Ann AND tag:Fantasy"), QList<int> { 1 });
    QCOMPARE(matches("tag:Action tag:Horror"), QList<int> { 3 });
    QCOMPARE(matches("(tag:Fantasy OR tag:Horror) NOT author:Anna"), (QList<int> { 1, 3 }));
    QCOMPARE(matches("writer:Ann"), (QList<int> { 1, 2, 3 }));
    QCOMPARE(matches("writer==Ann"), QList<int> { 1 });
    QCOMPARE(matches("tags:Action"), (QList<int> { 1, 2, 3, 6 }));
    auto folders = foldersSearchQuery(db, "tag:Action AND author:Ann");
    QVERIFY2(!folders.lastError().isValid(), qPrintable(folders.lastError().text()));
    QVERIFY(folders.next());
    QCOMPARE(folders.value(0).toInt(), 2);
    QVERIFY(!folders.next());
}

void MetadataSearchTest::quotedValuesRoundTrip()
{
    const QStringList values { "A \"Quoted\" Author", "C:\\Authors\\Name", "O'Brien", "A.*[B](C)+?", "NOT", "ends\\", "x\" OR tag:Action OR author:\"y" };
    int id = 10;
    for (const auto &value : values) {
        add(id, value, "Test");
        QString escaped = value;
        escaped.replace("\\", "\\\\");
        escaped.replace("\"", "\\\"");
        QCOMPARE(matches(QStringLiteral("author:\"%1\"").arg(escaped)), QList<int> { id });
        ++id;
    }
}

void MetadataSearchTest::invalidQueriesFail()
{
    QueryParser parser;
    QVERIFY_EXCEPTION_THROWN(parser.parse("tag:Action AND author:\"unfinished"), std::invalid_argument);
    QVERIFY_EXCEPTION_THROWN(parser.parse("tag>Action"), std::invalid_argument);
    QVERIFY_EXCEPTION_THROWN(parser.parse("(tag:Action"), std::invalid_argument);
}

void MetadataSearchTest::facetsAreNotTruncated()
{
    QVERIFY(db.transaction());
    for (int i = 10; i < 620; ++i)
        add(i, "Bulk", "Many");
    QVERIFY(db.commit());
    QCOMPARE(matches("tag:Many").size(), 610);
    QCOMPARE(matches("author:Bulk AND tag:Many").size(), 610);
    QCOMPARE(matches("tags:Many").size(), 500);
}

void MetadataSearchTest::tokenBoundariesAgree()
{
    const QString raw = "\tAnn\u00a0,ANN; Bob\n山田太郎、Élodie；100% Pure，A_B\r";
    const auto tokens = MetadataTokens::split(raw);
    QCOMPARE(tokens.size(), 7);
    for (const auto &token : tokens) {
        QRegularExpression re(MetadataTokens::exactPattern(token));
        QVERIFY2(re.isValid(), qPrintable(re.errorString()));
        QVERIFY(re.match(raw).hasMatch());
    }
    QVERIFY(!QRegularExpression(MetadataTokens::exactPattern("An")).match(raw).hasMatch());
}

QTEST_GUILESS_MAIN(MetadataSearchTest)
#include "main.moc"
