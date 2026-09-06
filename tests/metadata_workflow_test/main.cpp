#include "yacreader_global.h"
#include "yacreader_metadata_browser.h"
#include "yacreader_metadata_lookup_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkReply>
#include <QPushButton>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

// No HTTP requests: exercise synchronous finished delivery during abort(),
// including a late signal after another comic has already been selected.
class PendingReply : public QNetworkReply
{
public:
    explicit PendingReply(QObject *parent)
        : QNetworkReply(parent)
    {
        open(QIODevice::ReadOnly);
    }

    void abort() override
    {
        aborted = true;
        setError(OperationCanceledError, QStringLiteral("Cancelled"));
        setFinished(true);
        emit finished();
    }

    void finishAgain() { emit finished(); }
    bool aborted = false;

protected:
    qint64 readData(char *, qint64) override { return -1; }
};

class MetadataWorkflowTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void switchingComicCancelsOldReply();
    void closingDialogCancelsReply();
    void savePreservesExistingMetadata();
    void overwritePreservesReadingState();
    void missingComicDoesNotReportSuccess();
    void savedSignalIdentifiesLibrary();
    void sameLibraryRefreshesFacets();
    void readOnlyLibraryCannotOpenLookup();
    void switchingLibraryClosesLookup();

private:
    PendingReply *attachReply(YACReaderMetadataLookupDialog &dialog);
    void selectCandidate(YACReaderMetadataLookupDialog &dialog);
    QTemporaryDir directory;
    QString connectionName;
    QSqlDatabase database;
};

void MetadataWorkflowTest::init()
{
    QVERIFY(directory.isValid());
    QVERIFY(QDir().mkpath(YACReader::LibraryPaths::libraryDataPath(directory.path())));
    connectionName = QUuid::createUuid().toString();
    database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(YACReader::LibraryPaths::libraryDatabasePath(directory.path()));
    QVERIFY(database.open());

    QSqlQuery query(database);
    QVERIFY(query.exec("DROP TABLE IF EXISTS comic"));
    QVERIFY(query.exec("DROP TABLE IF EXISTS comic_info"));
    QVERIFY(query.exec("CREATE TABLE comic_info (id INTEGER PRIMARY KEY, title TEXT, writer TEXT, tags TEXT, "
                       "genere TEXT, format TEXT, year INTEGER, synopsis TEXT, edited INTEGER, "
                       "lastTimeMetadataSet INTEGER, currentPage INTEGER, read BOOLEAN)"));
    QVERIFY(query.exec("CREATE TABLE comic (id INTEGER PRIMARY KEY, comicInfoId INTEGER, fileName TEXT)"));
    QVERIFY(query.exec("INSERT INTO comic_info VALUES (1, 'My title', 'My author', '', '', '', 0, '', 0, 0, 17, 0)"));
    QVERIFY(query.exec("INSERT INTO comic VALUES (1, 1, 'comic.cbz')"));
}

void MetadataWorkflowTest::cleanup()
{
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

PendingReply *MetadataWorkflowTest::attachReply(YACReaderMetadataLookupDialog &dialog)
{
    auto *reply = new PendingReply(&dialog);
    dialog.activeReply = reply;
    dialog.setBusy(true);
    connect(reply, &QNetworkReply::finished, &dialog, &YACReaderMetadataLookupDialog::processSearchReply);
    return reply;
}

void MetadataWorkflowTest::selectCandidate(YACReaderMetadataLookupDialog &dialog)
{
    dialog.setComic(directory.path(), 1, "comic.cbz", "My title", "My author", "");
    YACReaderMetadataLookupDialog::Candidate candidate;
    candidate.romajiTitle = QStringLiteral("Imported title");
    candidate.authors = { QStringLiteral("Imported author") };
    candidate.tags = { QStringLiteral("Adventure") };
    candidate.genres = { QStringLiteral("Fantasy") };
    candidate.year = 2020;
    dialog.candidates.append(candidate);
    dialog.resultsList->addItem(candidate.romajiTitle);
    dialog.resultsList->setCurrentRow(0);
}

void MetadataWorkflowTest::switchingComicCancelsOldReply()
{
    YACReaderMetadataLookupDialog dialog;
    dialog.setComic(directory.path(), 1, "first.cbz", "", "", "");
    auto *oldReply = attachReply(dialog);

    dialog.setComic(directory.path(), 2, "second.cbz", "", "", "");
    QVERIFY(oldReply->aborted);
    QVERIFY(dialog.activeReply == nullptr);
    QVERIFY(dialog.searchButton->isEnabled());
    QCOMPARE(dialog.searchEdit->text(), QStringLiteral("second"));
    QCOMPARE(dialog.resultsList->count(), 0);
    QVERIFY(!dialog.buttonBox->button(QDialogButtonBox::Apply)->isEnabled());

    auto *newReply = attachReply(dialog);
    oldReply->finishAgain();
    QVERIFY(dialog.activeReply == newReply);
    QVERIFY(!newReply->aborted);
    dialog.reject();
}

void MetadataWorkflowTest::closingDialogCancelsReply()
{
    YACReaderMetadataLookupDialog dialog;
    auto *reply = attachReply(dialog);
    dialog.reject();
    QVERIFY(reply->aborted);
    QVERIFY(dialog.activeReply == nullptr);
    QVERIFY(dialog.searchButton->isEnabled());
}

void MetadataWorkflowTest::savePreservesExistingMetadata()
{
    YACReaderMetadataLookupDialog dialog;
    selectCandidate(dialog);
    QString error;
    QVERIFY2(dialog.saveCandidate(dialog.candidates.first(), &error), qPrintable(error));

    QSqlQuery query(database);
    QVERIFY(query.exec("SELECT title, writer, tags, genere, year, currentPage FROM comic_info WHERE id = 1"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), QStringLiteral("My title"));
    QCOMPARE(query.value(1).toString(), QStringLiteral("My author"));
    QCOMPARE(query.value(2).toString(), QStringLiteral("Adventure"));
    QCOMPARE(query.value(3).toString(), QStringLiteral("Fantasy"));
    QCOMPARE(query.value(4).toInt(), 2020);
    QCOMPARE(query.value(5).toInt(), 17);
}

void MetadataWorkflowTest::overwritePreservesReadingState()
{
    YACReaderMetadataLookupDialog dialog;
    selectCandidate(dialog);
    dialog.overwriteExisting->setChecked(true);
    QString error;
    QVERIFY2(dialog.saveCandidate(dialog.candidates.first(), &error), qPrintable(error));

    QSqlQuery query(database);
    QVERIFY(query.exec("SELECT title, writer, currentPage, read FROM comic_info WHERE id = 1"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), QStringLiteral("Imported title"));
    QCOMPARE(query.value(1).toString(), QStringLiteral("Imported author"));
    QCOMPARE(query.value(2).toInt(), 17);
    QCOMPARE(query.value(3).toInt(), 0);
}

void MetadataWorkflowTest::missingComicDoesNotReportSuccess()
{
    YACReaderMetadataLookupDialog dialog;
    selectCandidate(dialog);
    dialog.currentComicInfoId = 999;
    QSignalSpy saved(&dialog, &YACReaderMetadataLookupDialog::metadataSaved);
    dialog.applySelectedCandidate();
    QCOMPARE(saved.count(), 0);
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
}

void MetadataWorkflowTest::savedSignalIdentifiesLibrary()
{
    YACReaderMetadataLookupDialog dialog;
    selectCandidate(dialog);
    QSignalSpy saved(&dialog, &YACReaderMetadataLookupDialog::metadataSaved);
    // A consumer may reconfigure the dialog when it closes. The saved identity
    // must still describe the row that was actually written.
    connect(&dialog, &QDialog::accepted, &dialog, [&dialog] {
        dialog.setComic("another-library", 999, "other.cbz", "", "", "");
    });
    dialog.applySelectedCandidate();
    QCOMPARE(saved.count(), 1);
    QCOMPARE(saved.first().at(0).toString(), directory.path());
    QCOMPARE(saved.first().at(1).toULongLong(), qulonglong(1));
}

void MetadataWorkflowTest::sameLibraryRefreshesFacets()
{
    YACReaderMetadataBrowser browser;
    browser.setLibraryPath(directory.path());
    auto *tabs = browser.findChild<QTabWidget *>();
    QVERIFY(tabs != nullptr);
    auto *authors = qobject_cast<QListWidget *>(tabs->widget(0));
    QVERIFY(authors != nullptr);
    QCOMPARE(authors->count(), 1);
    QCOMPARE(authors->item(0)->data(Qt::UserRole).toString(), QStringLiteral("My author"));

    QSqlQuery query(database);
    QVERIFY(query.exec("UPDATE comic_info SET writer = 'New author' WHERE id = 1"));
    browser.setLibraryPath(directory.path());
    QCOMPARE(authors->count(), 1);
    QCOMPARE(authors->item(0)->data(Qt::UserRole).toString(), QStringLiteral("New author"));

    QSignalSpy requested(&browser, &YACReaderMetadataBrowser::searchRequested);
    authors->itemClicked(authors->item(0));
    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.first().at(0).toString(), QStringLiteral("author:\"New author\""));
}

void MetadataWorkflowTest::readOnlyLibraryCannotOpenLookup()
{
    YACReaderMetadataBrowser browser;
    browser.setLibraryPath(directory.path(), true);
    auto *tabs = browser.findChild<QTabWidget *>();
    QVERIFY(tabs != nullptr);
    tabs->setCurrentIndex(2);
    auto *unidentified = qobject_cast<QListWidget *>(tabs->widget(2));
    QVERIFY(unidentified != nullptr);
    QCOMPARE(unidentified->count(), 1);
    unidentified->setCurrentRow(0);
    unidentified->itemDoubleClicked(unidentified->item(0));
    auto *lookup = browser.findChild<YACReaderMetadataLookupDialog *>();
    QVERIFY(lookup != nullptr);
    QVERIFY(!lookup->isVisible());
}

void MetadataWorkflowTest::switchingLibraryClosesLookup()
{
    YACReaderMetadataBrowser browser;
    browser.setLibraryPath(directory.path());
    auto *lookup = browser.findChild<YACReaderMetadataLookupDialog *>();
    QVERIFY(lookup != nullptr);
    auto *reply = attachReply(*lookup);
    QSignalSpy closed(lookup, &QDialog::finished);
    browser.setLibraryPath(QString());
    QCOMPARE(closed.count(), 1);
    QVERIFY(reply->aborted);
    QVERIFY(browser.libraryPath().isEmpty());
    auto *tabs = browser.findChild<QTabWidget *>();
    QVERIFY(tabs != nullptr);
    QCOMPARE(qobject_cast<QListWidget *>(tabs->widget(0))->count(), 0);
}

QTEST_MAIN(MetadataWorkflowTest)
#include "main.moc"
