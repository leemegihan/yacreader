#include "comic_db.h"
#include "comic_image_folder.h"
#include "initial_comic_info_extractor.h"
#include "library_creator.h"
#include "library_maintenance_lock.h"
#include "local_metadata.h"
#include "yacreader_archive_inspector_dialog.h"
#include "yacreader_global.h"

#include <QApplication>
#include <QBuffer>
#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QProcess>
#include <QSettings>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QThread>
#include <QUuid>

namespace {
QImage sampleImage()
{
    QImage image(1400, 700, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setPen(Qt::black);
    QFont font("Arial");
    font.setPixelSize(64);
    painter.setFont(font);
    painter.drawText(80, 160, "Title: Test Book");
    painter.drawText(80, 350, "Author: Alice Example");
    painter.end();
    return image;
}

// Minimal uncompressed ZIP fixture: exercise the real archive backend without
// requiring a zip utility or including any user's comic pages in the repository.
bool writeZip(const QString &path, const QList<QPair<QString, QByteArray>> &files)
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    struct Entry {
        QByteArray name;
        quint32 size;
        quint32 crc;
        quint32 offset;
    };
    QList<Entry> entries;
    for (const auto &file : files) {
        quint32 crc = 0xffffffff;
        for (const auto ch : file.second) {
            crc ^= static_cast<unsigned char>(ch);
            for (int i = 0; i < 8; ++i)
                crc = (crc >> 1) ^ (0xedb88320 & (0 - (crc & 1)));
        }
        Entry entry { file.first.toUtf8(), quint32(file.second.size()), ~crc, quint32(bytes.size()) };
        stream << quint32(0x04034b50) << quint16(20) << quint16(0x800) << quint16(0) << quint16(0) << quint16(0)
               << entry.crc << entry.size << entry.size << quint16(entry.name.size()) << quint16(0);
        stream.writeRawData(entry.name.constData(), entry.name.size());
        stream.writeRawData(file.second.constData(), file.second.size());
        entries.append(entry);
    }
    const quint32 offset = bytes.size();
    for (const auto &entry : entries) {
        stream << quint32(0x02014b50) << quint16(20) << quint16(20) << quint16(0x800) << quint16(0) << quint16(0) << quint16(0)
               << entry.crc << entry.size << entry.size << quint16(entry.name.size()) << quint16(0) << quint16(0)
               << quint16(0) << quint16(0) << quint32(0) << entry.offset;
        stream.writeRawData(entry.name.constData(), entry.name.size());
    }
    const quint32 centralSize = bytes.size() - offset;
    stream << quint32(0x06054b50) << quint16(0) << quint16(0) << quint16(entries.size()) << quint16(entries.size())
           << centralSize << offset << quint16(0);
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
}

class LocalMetadataTest : public QObject
{
    Q_OBJECT
private slots:
    void sampling();
    void folderAndArchiveUseSamePageOrder();
    void collectionFolderIsNotAComic();
    void labelledCandidatesAreNotTranslators();
    void ocrProcessAndCancellation();
    void realOcr();
    void folderScanAndMetadataPreservation();
    void rootImageFolderSurvivesRescan();
    void closingAndSwitchingCancelWorkers();
    void missingSourceDoesNotCreateDatabase();
};

void LocalMetadataTest::sampling()
{
    QCOMPARE(LocalMetadata::sampleIndexes(0, 3), QVector<int>());
    QCOMPARE(LocalMetadata::sampleIndexes(1, 3), QVector<int>({ 0 }));
    QCOMPARE(LocalMetadata::sampleIndexes(5, 3), QVector<int>({ 0, 1, 2, 3, 4 }));
    QCOMPARE(LocalMetadata::sampleIndexes(10, 3), QVector<int>({ 0, 1, 2, 7, 8, 9 }));
    QCOMPARE(LocalMetadata::sampleIndexes(100, 999).size(), 12);
}

void LocalMetadataTest::folderAndArchiveUseSamePageOrder()
{
    QTemporaryDir temporary;
    const QString folder = temporary.filePath("한글 그림 폴더");
    QVERIFY(QDir().mkpath(folder));
    QByteArray png;
    QBuffer buffer(&png);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(sampleImage().save(&buffer, "PNG"));
    QList<QPair<QString, QByteArray>> files;
    for (const auto &name : QStringList { "10.png", "2.png", "1.png", "4.png", "3.png", "5.png", "6.png" }) {
        QFile file(QDir(folder).filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(png), png.size());
        files.append(QPair<QString, QByteArray> { name, png });
    }
    files.append(QPair<QString, QByteArray> { QStringLiteral("notes.txt"), QByteArrayLiteral("not a page") });
    QVERIFY(writeZip(temporary.filePath("作品.cbz"), files));
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    auto directoryResult = LocalMetadata::readPages(folder, 2, cancel);
    auto archiveResult = LocalMetadata::readPages(temporary.filePath("作品.cbz"), 2, cancel);
    QVERIFY2(archiveResult.error.isEmpty(), qPrintable(archiveResult.error));
    QCOMPARE(directoryResult.pageCount, 7);
    QCOMPARE(archiveResult.pageCount, directoryResult.pageCount);
    QCOMPARE(archiveResult.pages.size(), 4);
    for (int i = 0; i < 4; ++i) {
        QCOMPARE(archiveResult.pages.at(i).name, directoryResult.pages.at(i).name);
        QCOMPARE(archiveResult.pages.at(i).image, directoryResult.pages.at(i).image);
    }
    QCOMPARE(directoryResult.pages.last().name, QString("10.png"));
    YACReader::InitialComicInfoExtractor extractor(folder);
    extractor.extract();
    QVERIFY(extractor.hasValidCover());
    QCOMPARE(extractor.getNumPages(), 7);
    std::unique_ptr<Comic> viewer(FactoryComic::newComic(folder));
    QVERIFY(viewer != nullptr);
    ComicDB metadata;
    metadata.info.currentPage = 2;
    QVERIFY(viewer->load(folder, metadata));
    viewer->process();
    QCOMPARE(viewer->numPages(), 7U);
    QCOMPARE(viewer->getIndex(), 1U);
}

void LocalMetadataTest::collectionFolderIsNotAComic()
{
    QTemporaryDir temporary;
    QVERIFY(sampleImage().save(temporary.filePath("cover.png")));
    QVERIFY(QDir().mkpath(temporary.filePath(".yacreaderlibrary")));
    QVERIFY(ComicImageFolder::isComic(QFileInfo(temporary.path())));
    QVERIFY(QDir().mkpath(temporary.filePath("another-work")));
    QVERIFY(!ComicImageFolder::isComic(QFileInfo(temporary.path())));
    QVERIFY(QDir(temporary.path()).rmdir("another-work"));
    QVERIFY(writeZip(temporary.filePath("another.cbz"), { }));
    QVERIFY(!ComicImageFolder::isComic(QFileInfo(temporary.path())));
}

void LocalMetadataTest::labelledCandidatesAreNotTranslators()
{
    LocalMetadata::Page page;
    page.number = 21;
    page.text = "著者：山田 太郎\n作品名：青い空\n작가: 홍길동\n제목: 하늘\nAuthor: Alice Example\nTranslator: Wrong Person\n번역: 번역자\n発行：Publisher\nCircle: Some Group";
    const auto suggestions = LocalMetadata::suggest({ page }, "/downloads/[website] filename.cbz");
    int imageEvidence = 0;
    for (const auto &item : suggestions) {
        if (item.page > 0) {
            QCOMPARE(item.page, 21);
            ++imageEvidence;
        }
        QVERIFY(!item.value.contains("Wrong Person"));
        QVERIFY(!item.value.contains("번역자"));
        QVERIFY(!item.value.contains("Publisher"));
    }
    QCOMPARE(imageEvidence, 5);
    QVERIFY(suggestions.last().page == 0);
}

void LocalMetadataTest::ocrProcessAndCancellation()
{
    LocalMetadata::OcrOptions options;
    options.executable = QCoreApplication::applicationFilePath();
    options.language = "eng";
    auto flag = std::make_shared<std::atomic_bool>(false);
    QString error;
    const auto text = LocalMetadata::recognize(sampleImage(), options, flag, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(text.contains("Author: Alice Example"));
    flag->store(true);
    QVERIFY(LocalMetadata::recognize(sampleImage(), options, flag, &error).isEmpty());
    flag->store(false);
    qputenv("YACREADER_FAKE_OCR_MODE", "timeout");
    options.timeoutMs = 150;
    QElapsedTimer timer;
    timer.start();
    QVERIFY(LocalMetadata::recognize(sampleImage(), options, flag, &error).isEmpty());
    qunsetenv("YACREADER_FAKE_OCR_MODE");
    QVERIFY(!error.isEmpty());
    QVERIFY(timer.elapsed() < 6000);
    options.executable = "/missing-ocr-tool";
    QVERIFY(LocalMetadata::recognize(sampleImage(), options, flag, &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

void LocalMetadataTest::realOcr()
{
    auto options = LocalMetadata::defaultOcrOptions();
    if (!qEnvironmentVariableIsEmpty("YACREADER_TEST_OCR_DIR")) {
        const QDir directory(qEnvironmentVariable("YACREADER_TEST_OCR_DIR"));
        options.executable = directory.filePath("tesseract.exe");
        options.dataPath = directory.filePath("tessdata");
    }
    if (options.executable.isEmpty() && qEnvironmentVariableIsEmpty("YACREADER_REQUIRE_OCR"))
        QSKIP("OCR runtime not installed; packaged Windows job requires this test.");
    options.language = "eng";
    QString error;
    const auto fixture = sampleImage();
    int inkPixels = 0;
    for (int y = 0; y < fixture.height(); ++y) {
        for (int x = 0; x < fixture.width(); ++x) {
            if (qGray(fixture.pixel(x, y)) < 128)
                ++inkPixels;
        }
    }
    QVERIFY2(inkPixels > 1000, "The synthetic OCR fixture is blank; check the offscreen font directory.");
    const QString fixturePath = QCoreApplication::applicationDirPath() + "/ocr-fixture.png";
    QVERIFY(fixture.save(fixturePath));
    const auto text = LocalMetadata::recognize(fixture, options, std::make_shared<std::atomic_bool>(false), &error);
    if (text.isEmpty()) {
        QProcess diagnostic;
        QStringList arguments { fixturePath, "stdout", "-l", "eng", "--psm", "11" };
        if (!options.dataPath.isEmpty())
            arguments << "--tessdata-dir" << options.dataPath;
        diagnostic.start(options.executable, arguments);
        diagnostic.closeWriteChannel();
        diagnostic.waitForFinished(15000);
        qWarning() << "Direct OCR diagnostic:" << diagnostic.exitCode()
                   << diagnostic.readAllStandardOutput() << diagnostic.readAllStandardError();
    }
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(text.contains("Test Book", Qt::CaseInsensitive), qPrintable(text));
    QVERIFY2(text.contains("Alice Example", Qt::CaseInsensitive), qPrintable(text));
}

void LocalMetadataTest::folderScanAndMetadataPreservation()
{
    QTemporaryDir temporary;
    const QString root = temporary.filePath("library");
    QVERIFY(QDir().mkpath(root + "/Author/Work"));
    QVERIFY(sampleImage().save(root + "/Author/Work/1.png"));
    QVERIFY(sampleImage().save(root + "/Author/Work/2.png"));
    QSettings settings(temporary.filePath("settings.ini"), QSettings::IniFormat);
    LibraryCreator creator(&settings);
    QSignalSpy created(&creator, &LibraryCreator::created);
    creator.createLibrary(root, YACReader::LibraryPaths::libraryDataPath(root));
    creator.start();
    QVERIFY(creator.wait(30000));
    QTRY_COMPARE(created.count(), 1);
    const auto connection = QUuid::createUuid().toString();
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(YACReader::LibraryPaths::libraryDatabasePath(root));
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec("SELECT comicInfoId FROM comic WHERE path='/Author/Work'"));
        QVERIFY(query.next());
        const auto id = query.value(0).toULongLong();
        QVERIFY(query.exec(QString("UPDATE comic_info SET currentPage=1, title='Existing', writer='' WHERE id=%1").arg(id)));
        QString error;
        QVERIFY2(LocalMetadata::save(root, id, root + "/Author/Work", "New", "Alice", false, { }, &error), qPrintable(error));
        QVERIFY(query.exec(QString("SELECT title,writer,currentPage,numPages FROM comic_info WHERE id=%1").arg(id)));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), QString("Existing"));
        QCOMPARE(query.value(1).toString(), QString("Alice"));
        QCOMPARE(query.value(2).toInt(), 1);
        QCOMPARE(query.value(3).toInt(), 2);
        query.finish();
        QVERIFY(!LocalMetadata::save(root, id, root + "/Wrong", "Wrong", "Wrong", true, { }, &error));
        QVERIFY(LocalMetadata::save(root, id, root + "/Author/Work", "New", "", true, { }, &error));
        {
            LibraryMaintenanceLock maintenance(root);
            QVERIFY(maintenance.tryLock());
            QVERIFY(!LocalMetadata::save(root, id, root + "/Author/Work", "Wrong", "Wrong", true, { }, &error));
        }
        // Re-scan unchanged, then add a page: metadata and reading state survive.
        query.finish();
        creator.updateLibrary(root, YACReader::LibraryPaths::libraryDataPath(root));
        creator.start();
        QVERIFY(creator.wait(30000));
        QVERIFY(sampleImage().save(root + "/Author/Work/3.png"));
        creator.updateLibrary(root, YACReader::LibraryPaths::libraryDataPath(root));
        creator.start();
        QVERIFY(creator.wait(30000));
        QVERIFY(query.exec("SELECT ci.title,ci.writer,ci.currentPage,ci.numPages FROM comic c JOIN comic_info ci ON ci.id=c.comicInfoId WHERE c.path='/Author/Work'"));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), QString("New"));
        QCOMPARE(query.value(1).toString(), QString("Alice"));
        QCOMPARE(query.value(2).toInt(), 1);
        QCOMPARE(query.value(3).toInt(), 3);
        QVERIFY(!query.next());
    }
    QSqlDatabase::removeDatabase(connection);
}

void LocalMetadataTest::rootImageFolderSurvivesRescan()
{
    QTemporaryDir temporary;
    QVERIFY(sampleImage().save(temporary.filePath("1.png")));
    QSettings settings(temporary.filePath("settings.ini"), QSettings::IniFormat);
    LibraryCreator creator(&settings);
    creator.createLibrary(temporary.path(), YACReader::LibraryPaths::libraryDataPath(temporary.path()));
    creator.start();
    QVERIFY(creator.wait(30000));
    creator.updateLibrary(temporary.path(), YACReader::LibraryPaths::libraryDataPath(temporary.path()));
    creator.start();
    QVERIFY(creator.wait(30000));
    const auto connection = QUuid::createUuid().toString();
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(YACReader::LibraryPaths::libraryDatabasePath(temporary.path()));
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec("SELECT count(*) FROM comic WHERE path='/'"));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 1);
    }
    QSqlDatabase::removeDatabase(connection);
}

void LocalMetadataTest::closingAndSwitchingCancelWorkers()
{
    YACReaderArchiveInspectorDialog dialog;
    dialog.cancellation = std::make_shared<std::atomic_bool>(false);
    auto first = dialog.cancellation;
    dialog.reject();
    QVERIFY(first->load());
    dialog.cancellation = std::make_shared<std::atomic_bool>(false);
    auto second = dialog.cancellation;
    QTemporaryDir temporary;
    dialog.inspectComic(temporary.path(), 999);
    QVERIFY(second->load());
    QVERIFY(dialog.sourcePath.isEmpty());
    QVERIFY(dialog.titleEdit->text().isEmpty());
}

void LocalMetadataTest::missingSourceDoesNotCreateDatabase()
{
    QTemporaryDir directory;
    QString error;
    QVERIFY(!LocalMetadata::save(directory.path(), 999, QString(), "Title", "Author", false, { }, &error));
    QVERIFY(!QFileInfo::exists(YACReader::LibraryPaths::libraryDatabasePath(directory.path())));
}

int main(int argc, char **argv)
{
    if (argc > 2 && QByteArray(argv[1]) == "page.png") {
        if (qEnvironmentVariable("YACREADER_FAKE_OCR_MODE") == "timeout")
            QThread::sleep(10);
        QTextStream(stdout) << "Title: Test Book\nAuthor: Alice Example\n";
        return 0;
    }
#ifdef Q_OS_WIN
    // The offscreen plugin does not discover Windows system fonts itself.
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", qEnvironmentVariable("SystemRoot").toUtf8() + "/Fonts");
#endif
    QApplication app(argc, argv);
    LocalMetadataTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "main.moc"
