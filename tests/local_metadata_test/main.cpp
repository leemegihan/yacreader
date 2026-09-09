#include "comic_db.h"
#include "comic_image_folder.h"
#include "initial_comic_info_extractor.h"
#include "library_creator.h"
#include "library_maintenance_lock.h"
#include "local_metadata.h"
#include "local_ocr_cache.h"
#include "ocr_page_view.h"
#include "yacreader_archive_inspector_dialog.h"
#include "yacreader_global.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <cstdlib>

namespace {
QByteArray tsvFor(const QStringList &lines, int confidence = 90)
{
    QByteArray result = "level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext\n";
    int number = 0;
    for (const auto &line : lines)
        result += QString("5\t1\t1\t1\t%1\t1\t0\t0\t100\t20\t%2\t%3\n").arg(++number).arg(confidence).arg(line).toUtf8();
    return result;
}
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

int localOcrProbe(const QStringList &args);

class LocalMetadataTest : public QObject
{
    Q_OBJECT
private slots:
    void localProbePreservesInputs();
    void localPageProbeSamplingIsBounded();
    void isolatedSettingsPaths();
    void sampling();
    void folderAndArchiveUseSamePageOrder();
    void collectionFolderIsNotAComic();
    void labelledCandidatesAreNotTranslators();
    void ocrProcessAndCancellation();
    void realOcr();
    void croppedCreditOcr();
    void preprocessingPreservesSource();
    void regionCoordinatesAndCandidatePrefill();
    void spacedColophonAndPublisher();
    void dialogueAndLabelBoundaries();
    void tsvConfidenceAndLanguageSelection();
    void automaticQueryUsesOnlyStrongCredits();
    void realMultilingualColophon();
    void neuralCacheCompetingWriters();
    void neuralCacheIdentity();
    void neuralCachePreservesReviewAndDevices();
    void neuralCacheRejectsCorruption();
    void neuralResponseAndReview();
    void neuralAlternativesRequireReview();
    void neuralCjkDisagreementRequiresReview();
    void coverTitlesAndCombinedEvidence();
    void coverLayoutRejectsUnrelatedText();
    void nameHintsExcludeLibraryRoots();
    void filenameHintsHandleDecorations();
    void filenameFallbackRequiresExplicitReview();
    void filenameFallbackDistinguishesErrorsAndPageEvidence();
    void neuralCandidatesOpenReviewBeforeSearch();
    void conflictingCandidatesRequireSelection();
    void creditSuffixAndCompoundLabels();
    void creditGeometryAndDialogue();
    void bulletSeparatedCredits();
    void joinedAuthorRequiresPublishingLayout();
    void joinedCircleBannerNeedsReview();
    void publisherHintsRespectReviewBoundary();
    void alternatePairedLabelRequiresAdjacentNames();
    void bracketedInlineCredits();
    void sharedAuthorCircleNeedsReview();
    void realNeuralWorkReuse();
    void localNeuralDevice();
    void realNeuralOcr_data();
    void realNeuralOcr();
    void folderScanAndMetadataPreservation();
    void rootImageFolderSurvivesRescan();
    void closingAndSwitchingCancelWorkers();
    void missingSourceDoesNotCreateDatabase();
};

void LocalMetadataTest::isolatedSettingsPaths()
{
    const bool wasSet = qEnvironmentVariableIsSet("YACREADER_DATA_DIR");
    const auto previous = qEnvironmentVariable("YACREADER_DATA_DIR");
    const auto setRoot = [](const QString &value) {
#ifdef Q_OS_WIN
        // qputenv is an ANSI CRT API on Windows. UTF-8 bytes can fail under
        // a Korean system locale; preserve Unicode in the wide environment.
        return _wputenv_s(L"YACREADER_DATA_DIR", value.toStdWString().c_str()) == 0;
#else
        return qputenv("YACREADER_DATA_DIR", value.toUtf8());
#endif
    };
    const auto restore = qScopeGuard([&] {
        if (wasSet)
            setRoot(previous);
        else
            qunsetenv("YACREADER_DATA_DIR");
    });
    QTemporaryDir directory;
    const auto isolated = directory.filePath(QStringLiteral("한글 漫画"));
    QVERIFY(setRoot(isolated));
    QCOMPARE(YACReader::getSettingsPath(), QDir(isolated).filePath(QCoreApplication::applicationName()));
    QCOMPARE(YACReader::getCommonSettingsPath(), QDir(isolated).filePath("shared"));
    QCOMPARE(YACReader::getPluginsPath(), QDir(isolated).filePath("shared/plugins"));
    QVERIFY(qunsetenv("YACREADER_DATA_DIR"));
    QCOMPARE(YACReader::getSettingsPath(), QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
}

void LocalMetadataTest::localProbePreservesInputs()
{
    QTemporaryDir directory;
    const QString readingPath = directory.filePath("reading.json");
    const QString manifestPath = directory.filePath("manifest.json");
    const QString outputPath = directory.filePath("candidates.json");
    QJsonArray lines;
    for (const auto &pair : { qMakePair(QString("작가"), 80), qMakePair(QString("홍길동"), 110) })
        lines.append(QJsonObject { { "text", pair.first }, { "language", "kor" }, { "confidence", 95 }, { "box", QJsonArray { 10, pair.second, 150, pair.second + 20 } } });
    const auto bytes = QJsonDocument(QJsonObject { { "version", 1 }, { "engine", "paddle-regions" }, { "language", "kor" }, { "lines", lines } }).toJson();
    QFile reading(readingPath);
    QVERIFY(reading.open(QIODevice::WriteOnly));
    QCOMPARE(reading.write(bytes), bytes.size());
    reading.close();
    QFile manifest(manifestPath);
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(QJsonDocument(QJsonObject { { "version", 1 }, { "pages", QJsonArray { QJsonObject { { "number", 10 }, { "size", QJsonArray { 200, 200 } }, { "result", readingPath } } } } }).toJson());
    manifest.close();
    QCOMPARE(localOcrProbe({ "probe", "--local-ocr-candidates", manifestPath, outputPath }), 0);
    QFile output(outputPath);
    QVERIFY(output.open(QIODevice::ReadOnly));
    const auto result = QJsonDocument::fromJson(output.readAll()).object();
    const auto candidates = result.value("candidates").toArray();
    QCOMPARE(candidates.size(), 1);
    QCOMPARE(candidates.first().toObject().value("field").toString(), QString("author"));
    QCOMPARE(candidates.first().toObject().value("value").toString(), QString("홍길동"));
    output.close();
    QCOMPARE(localOcrProbe({ "probe", "--local-ocr-candidates", manifestPath, outputPath }), 2);
    QCOMPARE(localOcrProbe({ "probe", "--local-ocr-candidates", readingPath, directory.filePath("invalid.json") }), 2);
    QVERIFY(!QFileInfo::exists(directory.filePath("invalid.json")));
    QCOMPARE(localOcrProbe({ "probe", "--local-ocr-pages", directory.path(), directory.filePath("unsafe") }), 2);
    QVERIFY(!QFileInfo::exists(directory.filePath("unsafe")));
    QVERIFY(reading.open(QIODevice::ReadOnly));
    QCOMPARE(reading.readAll(), bytes);
}

void LocalMetadataTest::localPageProbeSamplingIsBounded()
{
    QTemporaryDir directory;
    const auto sourceRoot = directory.filePath("sources");
    QVERIFY(QDir().mkdir(sourceRoot));
    const auto folder = QDir(sourceRoot).filePath("images");
    QVERIFY(QDir().mkdir(folder));
    QList<QPair<QString, QByteArray>> files;
    for (int i = 8; i >= 1; --i) {
        QImage image(12, 16, QImage::Format_RGB32);
        image.fill(qRgb(i * 20, 0, 0));
        QBuffer buffer;
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "PNG"));
        const auto name = QString::number(i) + ".png";
        files.append(qMakePair(name, buffer.data()));
        QFile file(QDir(folder).filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(buffer.data()), qint64(buffer.data().size()));
    }
    const auto archive = QDir(sourceRoot).filePath("sample.cbz");
    QVERIFY(writeZip(archive, files));
    const QVector<QVector<int>> expected { { 1, 8 }, { 1, 2, 3, 6, 7, 8 }, { 1, 2, 3, 4, 5, 6, 7, 8 } };
    const QVector<int> counts { 1, 3, 6 };
    for (const auto &source : { folder, archive }) {
        for (int i = 0; i < counts.size(); ++i) {
            const auto output = directory.filePath(QString("pages-%1-%2").arg(source == folder ? "folder" : "archive").arg(counts[i]));
            QStringList args { "probe", "--local-ocr-pages", source, output };
            if (counts[i] != 3)
                args.append(QString::number(counts[i])); // Default remains three.
            QCOMPARE(localOcrProbe(args), 0);
            QFile manifest(QDir(output).filePath("pages.json"));
            QVERIFY(manifest.open(QIODevice::ReadOnly));
            const auto object = QJsonDocument::fromJson(manifest.readAll()).object();
            QCOMPARE(object.value("pageCount").toInt(), 8);
            QCOMPARE(object.value("perEnd").toInt(), counts[i]);
            QVector<int> numbers;
            for (const auto &page : object.value("pages").toArray())
                numbers.append(page.toObject().value("number").toInt());
            QCOMPARE(numbers, expected[i]); // A short work never repeats pages.
            QCOMPARE(localOcrProbe(args), 2); // Existing output is preserved.
        }
    }
    for (const auto &count : QStringList { "0", "7", "-1", "invalid", "1.5" }) {
        const auto output = directory.filePath("rejected");
        QCOMPARE(localOcrProbe({ "probe", "--local-ocr-pages", archive, output, count }), 2);
        QVERIFY(!QFileInfo::exists(output));
    }
    QCOMPARE(localOcrProbe({ "probe", "--local-ocr-pages", folder, QDir(folder).filePath("rejected"), "6" }), 2);
    QVERIFY(!QFileInfo::exists(QDir(folder).filePath("rejected")));
    QCOMPARE(localOcrProbe({ "probe", "--local-ocr-candidates", archive, directory.filePath("rejected"), "6" }), 2);
    for (const auto &item : files) {
        QFile source(QDir(folder).filePath(item.first));
        QVERIFY(source.open(QIODevice::ReadOnly));
        QCOMPARE(source.readAll(), item.second);
    }
}

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
        if (item.field == LocalMetadata::Suggestion::Author)
            QVERIFY(!item.value.contains("Publisher"));
    }
    QCOMPARE(imageEvidence, 7); // Five author/title credits and two publisher/circle hints.
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

void LocalMetadataTest::preprocessingPreservesSource()
{
    QImage image(80, 40, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    image.setPixelColor(10, 10, Qt::black);
    const auto original = image.copy();
    LocalMetadata::OcrOptions options;
    const auto prepared = LocalMetadata::prepareOcrImage(image, options);
    QCOMPARE(image, original);
    QCOMPARE(prepared.size(), QSize(200, 120));
    QCOMPARE(prepared.pixelColor(0, 0), QColor(Qt::white));
    QCOMPARE(prepared.pixelColor(30, 30), QColor(Qt::white));
    options.rotation = 90;
    QCOMPARE(LocalMetadata::prepareOcrImage(image, options).size(), QSize(120, 200));
    options.invert = true;
    const auto inverted = LocalMetadata::prepareOcrImage(image, options);
    QCOMPARE(inverted.pixelColor(40, 40), QColor(Qt::black));
}

void LocalMetadataTest::regionCoordinatesAndCandidatePrefill()
{
    OcrPageView view;
    QImage page(2600, 3800, QImage::Format_RGB32);
    page.fill(Qt::white);
    view.setPage(page);
    view.show();
    QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, QPoint(50, 100));
    QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, QPoint(249, 199));
    QCOMPARE(view.selectedRegion(), QRect(200, 400, 800, 400));
    view.setPage(page);
    QVERIFY(view.selectedRegion().isEmpty());

    YACReaderArchiveInspectorDialog dialog;
    LocalMetadata::Result result;
    LocalMetadata::Page credit;
    credit.number = 20;
    credit.text = "Author: Alice Example\nTitle: Test Book";
    result.pages = { credit };
    result.suggestions = LocalMetadata::suggest(result.pages, "/downloads/[Wrong Person] Wrong Title.cbz");
    dialog.showResult(result);
    QCOMPARE(dialog.titleEdit->text(), QString("Test Book"));
    QCOMPARE(dialog.authorEdit->text(), QString("Alice Example"));
    dialog.titleEdit->setText("My correction");
    dialog.showResult(result);
    QCOMPARE(dialog.titleEdit->text(), QString("My correction"));
    dialog.authorEdit->clear();
    credit.text += "\nAuthor: Bob Example";
    result.pages = { credit };
    result.suggestions = LocalMetadata::suggest(result.pages, "/downloads/Work.cbz");
    dialog.showResult(result);
    QVERIFY(dialog.authorEdit->text().isEmpty());
}

void LocalMetadataTest::croppedCreditOcr()
{
    auto options = LocalMetadata::defaultOcrOptions();
    if (options.executable.isEmpty() && qEnvironmentVariableIsEmpty("YACREADER_REQUIRE_OCR"))
        QSKIP("OCR runtime not installed.");
    options.language = "eng";
    options.segmentation = 6;
    // A small credit on a large illustrated page. Only the chosen rectangle is
    // sent to OCR, with automatic upscaling, white border and optional inversion.
    QImage page(2800, 3800, QImage::Format_RGB32);
    page.fill(QColor(65, 65, 65));
    const QRect region(80, 3450, 1000, 140);
    QPainter painter(&page);
    painter.fillRect(region, Qt::black);
    painter.setPen(Qt::white);
    QFont font("Arial");
    font.setPixelSize(40);
    painter.setFont(font);
    painter.drawText(region.adjusted(25, 0, -10, 0), Qt::AlignVCenter, "Author: Alice Example");
    painter.end();
    options.invert = true;
    QString error;
    const auto text = LocalMetadata::recognize(page.copy(region), options, std::make_shared<std::atomic_bool>(false), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(text.contains("Alice Example", Qt::CaseInsensitive), qPrintable(text));
    // The fast model remains independently usable alongside the precise model.
    if (!options.dataPath.isEmpty()) {
        options.dataPath = QDir(QFileInfo(options.executable).absolutePath()).filePath("tessdata");
        const auto fastText = LocalMetadata::recognize(page.copy(region), options, std::make_shared<std::atomic_bool>(false), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(fastText.contains("Alice Example", Qt::CaseInsensitive), qPrintable(fastText));
    }
}

void LocalMetadataTest::spacedColophonAndPublisher()
{
    LocalMetadata::Page page;
    page.number = 87;
    page.reading = LocalMetadata::parseTsv(tsvFor({ "あ と が き", "奥 付", "タ イ ト ル", "青 い 空 の 旅 3", "発 行 日", "2025 年 12 月 31 日", "発 行 者", "見 本 太 郎", "Special Thanks: Wrong Person", "連 絡 先", "example@example.test", "印 刷 所", "Example Press" }), "jpn+eng");
    page.text = page.reading.text;
    page.kind = LocalMetadata::classifyPage(page.text, page.number);
    QCOMPARE(page.kind, LocalMetadata::PageKind::Colophon);
    const auto suggestions = LocalMetadata::suggest({ page }, "/downloads/(edition) [example artist] Translated title.zip");
    int titles = 0, publishers = 0, authors = 0;
    bool nameHint = false;
    for (const auto &item : suggestions) {
        if (item.page == 0) {
            nameHint = nameHint || item.value == "example artist";
            continue;
        }
        QVERIFY(item.labelled);
        QCOMPARE(item.confidence, 90.0);
        if (item.field == LocalMetadata::Suggestion::Title) {
            ++titles;
            QCOMPARE(item.value, QString("青い空の旅 3"));
        }
        if (item.field == LocalMetadata::Suggestion::Publisher) {
            ++publishers;
            QCOMPARE(item.value, QString("見本太郎"));
        }
        if (item.field == LocalMetadata::Suggestion::Author)
            ++authors;
        QVERIFY(!item.value.contains("Wrong Person"));
        QVERIFY(!item.value.contains("Example Press"));
    }
    QCOMPARE(titles, 1);
    QCOMPARE(publishers, 1);
    QCOMPARE(authors, 0);
    QVERIFY(nameHint);
    YACReaderArchiveInspectorDialog dialog;
    LocalMetadata::Result result;
    result.pages = { page };
    result.pageCount = 88;
    result.suggestions = suggestions;
    dialog.showResult(result);
    QCOMPARE(dialog.titleEdit->text(), QString("青い空の旅 3"));
    QCOMPARE(dialog.publisherEdit->text(), QString("見本太郎"));
    QVERIFY(dialog.authorEdit->text().isEmpty());
}

void LocalMetadataTest::dialogueAndLabelBoundaries()
{
    LocalMetadata::Page page;
    page.number = 1;
    page.text = "여기 놔둘게.\n꿈에 그리던 집\n44 A\nこんにちは\nタイトル\n発行者\n印刷所\nWrong Press";
    for (const auto &suggestion : LocalMetadata::suggest({ page }, "/downloads/Book.zip"))
        QCOMPARE(suggestion.page, 0);
    QCOMPARE(LocalMetadata::classifyPage("hello there\n안녕하세요", 1), LocalMetadata::PageKind::Unknown);
    QCOMPARE(LocalMetadata::classifyPage("あ と が き\nThanks for reading", 20), LocalMetadata::PageKind::Afterword);
    QCOMPARE(LocalMetadata::classifyPage("Title: Test Book\nAuthor: Alice", 2), LocalMetadata::PageKind::TitlePage);
    QCOMPARE(LocalMetadata::classifyPage("제목\n푸른 하늘\n작가\n홍길동\n2025", 20), LocalMetadata::PageKind::Colophon);
    QCOMPARE(LocalMetadata::classifyPage("작가\n홍길동", 20), LocalMetadata::PageKind::Unknown);
    QCOMPARE(LocalMetadata::normalizeOcrText("タ イ ト ル\n한국어 제목\nTest Book"), QString("タイトル\n한국어 제목\nTest Book"));
}

void LocalMetadataTest::tsvConfidenceAndLanguageSelection()
{
    auto japanese = LocalMetadata::parseTsv(tsvFor({ "奥 付", "タ イ ト ル", "青 い 空", "発 行 者", "見 本 太 郎" }, 91), "jpn+eng");
    auto wrongKorean = LocalMetadata::parseTsv(tsvFor({ "zx", "a 4" }, 35), "kor+eng");
    auto selected = LocalMetadata::chooseReading({ wrongKorean, japanese });
    QCOMPARE(selected.language, QString("jpn+eng"));
    QVERIFY(!selected.uncertainLanguage);
    const auto alternateLayout = LocalMetadata::parseTsv(tsvFor({ "奥付", "タイトル", "育い空", "発行者", "見本太郎" }, 90), "jpn+eng");
    QVERIFY(!LocalMetadata::chooseReading({ japanese, alternateLayout, wrongKorean }).uncertainLanguage);
    auto korean = LocalMetadata::parseTsv(tsvFor({ "제목: 푸른 하늘", "작가: 홍길동" }, 92), "kor+eng");
    auto wrongJapanese = LocalMetadata::parseTsv(tsvFor({ "あ", "xx" }, 30), "jpn+eng");
    QCOMPARE(LocalMetadata::chooseReading({ wrongJapanese, korean }).language, QString("kor+eng"));
    QCOMPARE(korean.confidence, 92.0);
    QVERIFY(!LocalMetadata::parseTsv("bad response", "eng").error.isEmpty());
    auto unknown = LocalMetadata::parseTsv(tsvFor({ "abc" }, -1), "eng");
    QCOMPARE(unknown.confidence, -1.0);
    QVERIFY(LocalMetadata::chooseReading({ unknown }).uncertainLanguage);
    LocalMetadata::Reading missing;
    missing.language = "kor+eng";
    missing.error = "missing model";
    selected = LocalMetadata::chooseReading({ missing, japanese });
    QVERIFY(!selected.text.isEmpty());
    QVERIFY(!selected.error.isEmpty());
    QVERIFY(selected.uncertainLanguage);
}

void LocalMetadataTest::automaticQueryUsesOnlyStrongCredits()
{
    YACReaderArchiveInspectorDialog dialog;
    QSignalSpy requested(&dialog, &YACReaderArchiveInspectorDialog::titleSearchRequested);
    LocalMetadata::Page page;
    page.number = 20;
    page.reading = LocalMetadata::parseTsv(tsvFor({ "奥付", "タイトル", "青い空", "発行者", "見本太郎" }), "jpn+eng");
    page.text = page.reading.text;
    page.kind = LocalMetadata::PageKind::Colophon;
    LocalMetadata::Result result;
    result.pages = { page };
    result.pageCount = 22;
    result.suggestions = LocalMetadata::suggest(result.pages, "/downloads/(edition) [example artist] Book.zip");
    dialog.showResult(result);
    dialog.autoSearch->setChecked(false);
    dialog.requestSearch(true);
    QCOMPARE(requested.count(), 0);
    dialog.autoSearch->setChecked(true);
    dialog.requestSearch(true);
    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.first().at(2).toString(), QString("青い空"));
    QVERIFY(requested.first().at(3).toString().isEmpty());
    QVERIFY(requested.first().at(5).toStringList().contains("example artist"));
    QCOMPARE(requested.first().at(6).toStringList(), QStringList { "見本太郎" });
    dialog.requestSearch(true);
    QCOMPARE(requested.count(), 1);
    dialog.automaticSearchDone = false;
    dialog.result.pages[0].reading.uncertainLanguage = true;
    dialog.requestSearch(true);
    QCOMPARE(requested.count(), 1);
}

void LocalMetadataTest::realMultilingualColophon()
{
    auto options = LocalMetadata::defaultOcrOptions();
    if (qEnvironmentVariableIsEmpty("YACREADER_REQUIRE_OCR"))
        QSKIP("The packaged Windows job requires both language models and CJK fonts.");
    options.language = "auto";
#ifdef Q_OS_WIN
    // Exercise Unicode model staging before the expensive full app build too.
    // jpn's embedded configuration also loads the jpn_vert sublanguage.
    QTemporaryDir modelDirectory;
    QVERIFY(modelDirectory.isValid());
    const auto unicodeModels = modelDirectory.filePath("언어 모델");
    QVERIFY(QDir().mkpath(unicodeModels));
    for (const auto &name : { "eng", "jpn", "jpn_vert", "kor" }) {
        const auto file = QString::fromLatin1(name) + ".traineddata";
        QVERIFY(QFile::copy(QDir(options.dataPath).filePath(file), QDir(unicodeModels).filePath(file)));
    }
    options.dataPath = unicodeModels;
#endif
    QString fixtureFamily;
    const auto fixtureDirectory = qEnvironmentVariable("YACREADER_TEST_CJK_FIXTURES");
    const auto fixtureFont = qEnvironmentVariable("YACREADER_TEST_CJK_FONT");
    if (fixtureDirectory.isEmpty() && !fixtureFont.isEmpty()) {
        const int fontId = QFontDatabase::addApplicationFont(fixtureFont);
        QVERIFY2(fontId >= 0, "Could not load the pinned CJK fixture font.");
        const auto families = QFontDatabase::applicationFontFamilies(fontId);
        QVERIFY(!families.isEmpty());
        fixtureFamily = families.first();
    }
    for (const bool korean : { false, true }) {
        QImage image;
        if (!fixtureDirectory.isEmpty()) {
            // Installed-path checks use exactly the same input pixels. Native
            // Windows and offscreen font rasterizers otherwise change the test.
            QVERIFY(image.load(QDir(fixtureDirectory).filePath(korean ? "ocr-colophon-kor.png" : "ocr-colophon-jpn.png")));
        } else {
            image = QImage(1600, 900, QImage::Format_RGB32);
            image.fill(Qt::white);
            QPainter painter(&image);
            QFont font(fixtureFamily.isEmpty() ? (korean ? "Malgun Gothic" : "Yu Gothic") : fixtureFamily);
            font.setPixelSize(56);
            painter.setFont(font);
            painter.setPen(Qt::black);
            const QStringList lines = korean ? QStringList { "제목", "푸른 하늘", "작가", "홍길동", "발행일", "2025" }
                                             : QStringList { "奥付", "タイトル", "青い空", "発行者", "見本太郎", "発行日" };
            int y = 100;
            for (const auto &line : lines) {
                for (const auto ch : line)
                    QVERIFY2(QFontMetrics(font).inFontUcs4(ch.unicode()), "The fixture font lacks a required CJK glyph; do not OCR missing-glyph boxes.");
                painter.drawText(100, y, line);
                y += 120;
            }
        }
        const auto fixturePath = QCoreApplication::applicationDirPath() + (korean ? "/ocr-colophon-kor" : "/ocr-colophon-jpn");
        QVERIFY(image.save(fixturePath + ".png"));
        const auto reading = LocalMetadata::recognizePage(image, options, std::make_shared<std::atomic_bool>(false));
        qInfo() << "Colophon language/confidence/score:" << reading.language << reading.confidence << reading.score;
        QFile recognized(fixturePath + ".txt");
        QVERIFY(recognized.open(QIODevice::WriteOnly));
        recognized.write(reading.text.toUtf8());
        QVERIFY2(reading.error.isEmpty(), qPrintable(reading.error));
        QCOMPARE(reading.language, korean ? QString("kor+eng") : QString("jpn+eng"));
        LocalMetadata::Page page;
        page.number = 20;
        page.text = reading.text;
        page.reading = reading;
        QCOMPARE(LocalMetadata::classifyPage(page.text, page.number), LocalMetadata::PageKind::Colophon);
        QStringList titles, credits;
        for (const auto &candidate : LocalMetadata::suggest({ page }, "/downloads/Fixture.zip")) {
            if (candidate.page != 20 || !candidate.labelled)
                continue;
            if (candidate.field == LocalMetadata::Suggestion::Title)
                titles.append(candidate.value);
            if (candidate.field == (korean ? LocalMetadata::Suggestion::Author : LocalMetadata::Suggestion::Publisher))
                credits.append(candidate.value);
        }
        QCOMPARE(titles.size(), 1);
        QCOMPARE(credits.size(), 1);
        // Measure the identification fields this feature actually uses.
        // The full UTF-8 diagnostic retains omissions in unrelated fields,
        // including publication-date labels. Do not claim perfect-page OCR.
        auto compact = [](QString text) {
            text.remove(QRegularExpression(QStringLiteral("\\s+")));
            return text;
        };
        const auto actual = compact(titles.first() + "\n" + credits.first());
        const auto expected = compact(korean ? QString("푸른 하늘\n홍길동") : QString("青い空\n見本太郎"));
        QVector<int> distance(actual.size() + 1);
        for (int j = 0; j <= actual.size(); ++j)
            distance[j] = j;
        for (int i = 1; i <= expected.size(); ++i) {
            int previous = distance[0];
            distance[0] = i;
            for (int j = 1; j <= actual.size(); ++j) {
                const int saved = distance[j];
                distance[j] = qMin(qMin(distance[j] + 1, distance[j - 1] + 1), previous + (expected.at(i - 1) != actual.at(j - 1)));
                previous = saved;
            }
        }
        qInfo() << "Identification character errors:" << distance.last();
        QVERIFY2(distance.last() <= 1, "Title/credit OCR exceeds the one-character error bound; inspect the UTF-8 diagnostic.");
        QVERIFY(reading.confidence > 0);
    }
}

namespace {
LocalOcrCache::Identity cacheIdentity()
{
    return { QString(64, u'a'), QSize(200, 200), QString(64, u'b'), QString(64, u'c'),
             QString(64, u'd'), QString(64, u'e'), QStringLiteral("kor"), 8, QStringLiteral("gpu:0"), QStringLiteral("gpu:0") };
}
QByteArray cacheResponse(const QString &device = QStringLiteral("gpu:0"))
{
    const QJsonArray lines { QJsonObject { { "text", "저자: 가상작가" }, { "confidence", 94 }, { "language", "kor" }, { "box", QJsonArray { 10, 10, 180, 30 } } } };
    return QJsonDocument(QJsonObject { { "version", 1 }, { "engine", "paddle-regions" }, { "language", "kor" }, { "device", device }, { "lines", lines } }).toJson();
}
}

int cacheWriterProbe(const QStringList &args)
{
    if (args.size() != 7)
        return 2;
    QFile ready(args[3]);
    if (!ready.open(QIODevice::WriteOnly | QIODevice::NewOnly) || ready.write("ready") != 5)
        return 2;
    ready.close();
    QElapsedTimer timer;
    timer.start();
    while (!QFileInfo::exists(args[4]) && timer.elapsed() < 10000)
        QThread::msleep(10);
    if (!QFileInfo::exists(args[4]))
        return 2;
    const auto bytes = cacheResponse() + (args[6] == "second" ? QByteArray("\n") : QByteArray());
    QString error;
    const bool saved = LocalOcrCache::save(args[2], cacheIdentity(), bytes, &error);
    QFile result(args[5]);
    const QByteArray outcome = saved ? "saved" : "refused";
    return result.open(QIODevice::WriteOnly | QIODevice::NewOnly) && result.write(outcome) == outcome.size() ? 0 : 3;
}

void LocalMetadataTest::neuralCacheCompetingWriters()
{
    QTemporaryDir dir;
    const auto root = dir.filePath(QStringLiteral("cache"));
    QVERIFY(QDir().mkdir(root));
    const auto gate = dir.filePath(QStringLiteral("gate"));
    const auto ready1 = dir.filePath(QStringLiteral("ready-1"));
    const auto ready2 = dir.filePath(QStringLiteral("ready-2"));
    const auto result1 = dir.filePath(QStringLiteral("result-1"));
    const auto result2 = dir.filePath(QStringLiteral("result-2"));
    QProcess first, second;
#ifdef Q_OS_WIN
    for (auto *process : { &first, &second })
        process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= 0x08000000; });
#endif
    first.start(QCoreApplication::applicationFilePath(), { "--cache-writer", root, ready1, gate, result1, "first" });
    second.start(QCoreApplication::applicationFilePath(), { "--cache-writer", root, ready2, gate, result2, "second" });
    QVERIFY(first.waitForStarted());
    QVERIFY(second.waitForStarted());
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(ready1) && QFileInfo::exists(ready2), 10000);
    QFile release(gate);
    QVERIFY(release.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(release.write("go"), 2);
    release.close();
    QVERIFY(first.state() == QProcess::NotRunning || first.waitForFinished(15000));
    QVERIFY(second.state() == QProcess::NotRunning || second.waitForFinished(15000));
    QCOMPARE(first.exitCode(), 0);
    QCOMPARE(second.exitCode(), 0);
    QFile a(result1), b(result2);
    QVERIFY(a.open(QIODevice::ReadOnly));
    QVERIFY(b.open(QIODevice::ReadOnly));
    const auto resultA = a.readAll(), resultB = b.readAll();
    QVERIFY((resultA == "saved" && resultB == "refused") || (resultB == "saved" && resultA == "refused"));
    QString error;
    const auto cached = LocalOcrCache::load(root, cacheIdentity(), &error);
    QVERIFY2(cached.has_value(), qPrintable(error));
    QCOMPARE(cached->rawResult, cacheResponse() + (resultB == "saved" ? QByteArray("\n") : QByteArray()));
    QCOMPARE(QDir(root).entryList({ "*.lock" }, QDir::Files).size(), 0);
}

void LocalMetadataTest::neuralCacheIdentity()
{
    const auto original = cacheIdentity();
    const auto key = LocalOcrCache::key(original);
    QCOMPARE(key.size(), 64);
    for (int change = 0; change < 11; ++change) {
        auto changed = original;
        switch (change) {
        case 0:
            changed.imageSha256 = QString(64, u'f');
            break;
        case 1:
            changed.preparedSize = QSize(300, 200);
            break;
        case 2:
            changed.preprocessingFingerprint = QString(64, u'f');
            break;
        case 3:
            changed.workerSha256 = QString(64, u'f');
            break;
        case 4:
            changed.modelManifestSha256 = QString(64, u'f');
            break;
        case 5:
            changed.packageManifestSha256 = QString(64, u'f');
            break;
        case 6:
            changed.language = QStringLiteral("jpn");
            break;
        case 7:
            changed.cpuThreads = 4;
            break;
        case 8:
            changed.actualDevice = QStringLiteral("cpu");
            break;
        case 9:
            changed.requestedDevice = QStringLiteral("cpu");
            changed.actualDevice = QStringLiteral("cpu");
            break;
        case 10:
            changed.cpuThreads = 16;
            break;
        }
        QVERIFY(!LocalOcrCache::key(changed).isEmpty());
        QVERIFY(LocalOcrCache::key(changed) != key);
    }
    auto invalid = original;
    invalid.imageSha256 = QStringLiteral("unknown");
    QVERIFY(LocalOcrCache::key(invalid).isEmpty());
    invalid = original;
    invalid.requestedDevice = QStringLiteral("cpu"); // CPU request cannot claim GPU execution.
    QVERIFY(LocalOcrCache::key(invalid).isEmpty());
    invalid = original;
    invalid.preparedSize = QSize(100000, 100000);
    QVERIFY(LocalOcrCache::key(invalid).isEmpty());
}

void LocalMetadataTest::neuralCachePreservesReviewAndDevices()
{
    QTemporaryDir dir;
    const auto identity = cacheIdentity();
    const auto bytes = cacheResponse();
    QString error;
    QVERIFY2(LocalOcrCache::save(dir.path(), identity, bytes, &error), qPrintable(error));
    const auto cached = LocalOcrCache::load(dir.path(), identity, &error);
    QVERIFY2(cached.has_value(), qPrintable(error));
    QCOMPARE(cached->rawResult, bytes);
    QCOMPARE(cached->reading.text, QStringLiteral("저자: 가상작가"));
    QVERIFY(cached->reading.reviewRequired);
    QCOMPARE(cached->reading.device, QStringLiteral("gpu:0"));
    QCOMPARE(cached->reading.lines.first().bounds, QRect(10, 10, 170, 20));
    QVERIFY(LocalOcrCache::save(dir.path(), identity, bytes, &error));

    auto cpuFallback = identity;
    cpuFallback.actualDevice = QStringLiteral("cpu");
    QVERIFY(!LocalOcrCache::load(dir.path(), cpuFallback, &error));
    QVERIFY(!LocalOcrCache::save(dir.path(), cpuFallback, bytes, &error));
    QVERIFY(LocalOcrCache::save(dir.path(), cpuFallback, cacheResponse(QStringLiteral("cpu")), &error));
    QCOMPARE(LocalOcrCache::load(dir.path(), cpuFallback, &error)->reading.device, QStringLiteral("cpu"));
    auto cpuRequest = cpuFallback;
    cpuRequest.requestedDevice = QStringLiteral("cpu");
    QVERIFY(!LocalOcrCache::load(dir.path(), cpuRequest, &error));
    // Valid empty-page OCR is preserved; it is not an engine error.
    auto emptyPage = identity;
    emptyPage.imageSha256 = QString(64, u'f');
    auto empty = QJsonDocument::fromJson(bytes).object();
    empty["lines"] = QJsonArray();
    QVERIFY(LocalOcrCache::save(dir.path(), emptyPage, QJsonDocument(empty).toJson(), &error));
    const auto blank = LocalOcrCache::load(dir.path(), emptyPage, &error);
    QVERIFY(blank && blank->reading.text.isEmpty() && blank->reading.error.isEmpty());
}

void LocalMetadataTest::neuralCacheRejectsCorruption()
{
    QTemporaryDir dir;
    const auto identity = cacheIdentity();
    const auto bytes = cacheResponse();
    QString error;
    QVERIFY(!LocalOcrCache::save(dir.path(), identity, "{}", &error));
    auto failed = QJsonDocument::fromJson(bytes).object();
    failed["error"] = "Synthetic OCR failure";
    QVERIFY(!LocalOcrCache::save(dir.path(), identity, QJsonDocument(failed).toJson(), &error));
    failed = QJsonDocument::fromJson(bytes).object();
    failed["lines"] = QJsonArray { QJsonObject { { "text", "invalid" }, { "confidence", 99 }, { "language", "kor" }, { "box", QJsonArray { 0, 0, 999, 999 } } } };
    QVERIFY(!LocalOcrCache::save(dir.path(), identity, QJsonDocument(failed).toJson(), &error));
    QVERIFY(!LocalOcrCache::save(QStringLiteral("relative-cache"), identity, bytes, &error));
    QVERIFY(LocalOcrCache::save(dir.path(), identity, bytes, &error));
    QVERIFY(!LocalOcrCache::save(dir.path(), identity, bytes + '\n', &error)); // Do not replace original bytes.
    const QString filename = dir.filePath(LocalOcrCache::key(identity) + QStringLiteral(".json"));
    QFile file(filename);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto envelope = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    envelope["resultSha256"] = QString(64, u'0');
    const auto damaged = QJsonDocument(envelope).toJson();
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(damaged), damaged.size());
    file.close();
    QVERIFY(!LocalOcrCache::load(dir.path(), identity, &error));
    QVERIFY(!LocalOcrCache::save(dir.path(), identity, bytes, &error));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), damaged); // Preserve damaged evidence for explicit recovery.
}

void LocalMetadataTest::neuralResponseAndReview()
{
    QJsonArray lines;
    for (const auto &pair : { qMakePair(QString("제목"), 10), qMakePair(QString("푸른 하늘"), 40), qMakePair(QString("작가"), 80), qMakePair(QString("홍길동"), 110) })
        lines.append(QJsonObject { { "text", pair.first }, { "language", "kor" }, { "confidence", 95 }, { "box", QJsonArray { 10, pair.second, 150, pair.second + 20 } } });
    QJsonObject response { { "version", 1 }, { "engine", "paddle-regions" }, { "language", "kor" }, { "lines", lines } };
    const auto reading = LocalMetadata::parseNeuralReading(QJsonDocument(response).toJson(), QSize(200, 200));
    QVERIFY(reading.error.isEmpty());
    QVERIFY(reading.reviewRequired);
    QCOMPARE(reading.lines.size(), 4);
    LocalMetadata::Page page;
    page.number = 10;
    page.text = reading.text;
    page.reading = reading;
    page.kind = LocalMetadata::PageKind::Colophon;
    LocalMetadata::Result result;
    result.pages = { page };
    result.suggestions = LocalMetadata::suggest(result.pages, QString());
    QCOMPARE(result.suggestions.size(), 2);
    YACReaderArchiveInspectorDialog dialog;
    QSignalSpy requests(&dialog, &YACReaderArchiveInspectorDialog::titleSearchRequested);
    dialog.showResult(result);
    QVERIFY(dialog.titleEdit->text().isEmpty());
    QVERIFY(dialog.authorEdit->text().isEmpty());
    dialog.requestSearch(true);
    QCOMPARE(requests.count(), 0);
    // Do not attach a label to a distant, unrelated detected text region.
    page.reading.lines[1].bounds.moveLeft(500);
    QCOMPARE(LocalMetadata::suggest({ page }, QString()).size(), 1);
    lines[0] = QJsonObject { { "text", "제목" }, { "confidence", 101 }, { "language", "kor" }, { "box", QJsonArray { 0, 0, 10, 10 } } };
    response["lines"] = lines;
    QVERIFY(!LocalMetadata::parseNeuralReading(QJsonDocument(response).toJson(), QSize(200, 200)).error.isEmpty());
    QVERIFY(!LocalMetadata::parseNeuralReading("{}", QSize(200, 200)).error.isEmpty());
}

void LocalMetadataTest::neuralAlternativesRequireReview()
{
    const QJsonArray box { 10, 10, 180, 30 };
    const QJsonObject alternate { { "text", "[Author]青木そら" }, { "language", "jpn" }, { "confidence", 93 }, { "box", box } };
    QJsonObject line { { "text", "[Author]" }, { "language", "kor" }, { "confidence", 97 }, { "box", box }, { "alternatives", QJsonArray { alternate } } };
    QJsonObject response { { "version", 1 }, { "engine", "paddle-regions" }, { "language", "auto" }, { "lines", QJsonArray { line } } };
    auto read = [&] { return LocalMetadata::parseNeuralReading(QJsonDocument(response).toJson(), QSize(200, 200)); };
    const auto reading = read();
    QVERIFY(reading.error.isEmpty());
    QCOMPARE(reading.text, QString("[Author]"));
    QCOMPARE(reading.confidence, 97.0);
    QCOMPARE(reading.lines.size(), 1);
    QCOMPARE(reading.alternatives.size(), 1);
    QCOMPARE(reading.alternatives.first().text, QString("[Author]青木そら"));
    LocalMetadata::Page page;
    page.number = 1;
    page.text = reading.text;
    page.reading = reading;
    LocalMetadata::Result result;
    result.pages = { page };
    result.suggestions = LocalMetadata::suggest(result.pages, QString());
    QCOMPARE(result.suggestions.size(), 1);
    const auto candidate = result.suggestions.first();
    QCOMPARE(candidate.field, LocalMetadata::Suggestion::Author);
    QCOMPARE(candidate.value, QString("青木そら"));
    QVERIFY(!candidate.labelled);
    QVERIFY(!candidate.evidence.first().labelled);
    YACReaderArchiveInspectorDialog dialog;
    QSignalSpy requests(&dialog, &YACReaderArchiveInspectorDialog::titleSearchRequested);
    dialog.showResult(result);
    QVERIFY(dialog.authorEdit->text().isEmpty());
    QVERIFY(dialog.pageText->toPlainText().contains("[Author]青木そら"));
    dialog.requestSearch(true);
    QCOMPARE(requests.count(), 0);
    // Isolate the unlabelled-evidence gate with a title that would otherwise
    // satisfy automatic lookup, even without the neural review-required flag.
    auto titlePage = page;
    titlePage.kind = LocalMetadata::PageKind::Colophon;
    titlePage.reading.reviewRequired = false;
    titlePage.reading.alternatives.first().text = "[Title]雨の図書館";
    LocalMetadata::Result titleResult;
    titleResult.pages = { titlePage };
    titleResult.suggestions = LocalMetadata::suggest(titleResult.pages, QString());
    YACReaderArchiveInspectorDialog titleDialog;
    QSignalSpy titleRequests(&titleDialog, &YACReaderArchiveInspectorDialog::titleSearchRequested);
    titleDialog.autoSearch->setChecked(true);
    titleDialog.showResult(titleResult);
    QVERIFY(titleDialog.titleEdit->text().isEmpty());
    titleDialog.requestSearch(true);
    QCOMPARE(titleRequests.count(), 0);
    titleDialog.requestSearch(false); // Explicit action opens review, not immediate lookup.
    QCOMPARE(titleRequests.count(), 1);
    QCOMPARE(titleRequests.first().at(2).toString(), QString("雨の図書館"));
    QVERIFY(!titleRequests.first().at(7).toBool());
    QVERIFY(titleRequests.first().at(8).toString().contains("미확정"));
    // An incomplete alternative must not borrow the primary reading's next line.
    page.reading.alternatives.first().text = "[Author]";
    page.text += "\nWrong Person";
    QVERIFY(LocalMetadata::suggest({ page }, QString()).isEmpty());
    for (const auto &bad : { QJsonValue(QJsonValue::Null), QJsonValue(QJsonObject()), QJsonValue(QJsonArray { alternate, alternate, alternate }) }) {
        line["alternatives"] = bad;
        response["lines"] = QJsonArray { line };
        QVERIFY(!read().error.isEmpty());
    }
    for (const auto &field : { QString("box"), QString("language"), QString("confidence") }) {
        auto bad = alternate;
        bad[field] = field == "box" ? QJsonValue(QJsonArray { 11, 10, 180, 30 }) : field == "language" ? QJsonValue(QString("kor"))
                                                                                                       : QJsonValue(84);
        line["alternatives"] = QJsonArray { bad };
        response["lines"] = QJsonArray { line };
        QVERIFY(!read().error.isEmpty());
    }
    line.remove("alternatives");
    response["lines"] = QJsonArray { line };
    QVERIFY(read().error.isEmpty());
    QVERIFY(read().alternatives.isEmpty());
}

void LocalMetadataTest::neuralCjkDisagreementRequiresReview()
{
    const QJsonArray box { 10, 10, 180, 30 };
    const QJsonObject alternate { { "text", "제목: 가상의 책" }, { "language", "kor" }, { "confidence", 94 }, { "box", box } };
    const QJsonObject line { { "text", "雨" }, { "language", "jpn" }, { "confidence", 96 }, { "box", box }, { "alternatives", QJsonArray { alternate } } };
    const QJsonObject response { { "version", 1 }, { "engine", "paddle-regions" }, { "language", "auto" }, { "lines", QJsonArray { line } } };
    const auto reading = LocalMetadata::parseNeuralReading(QJsonDocument(response).toJson(), QSize(200, 200));
    QVERIFY(reading.error.isEmpty());
    QCOMPARE(reading.text, QString("雨"));
    QCOMPARE(reading.confidence, 96.0);
    QCOMPARE(reading.lines.size(), 1);
    QCOMPARE(reading.alternatives.size(), 1);
    LocalMetadata::Page page;
    page.number = 20;
    page.kind = LocalMetadata::PageKind::Colophon;
    page.text = reading.text;
    page.reading = reading;
    page.reading.reviewRequired = false; // Exercise the unlabelled gate independently.
    LocalMetadata::Result result;
    result.pages = { page };
    result.suggestions = LocalMetadata::suggest(result.pages, QString());
    QCOMPARE(result.suggestions.size(), 1);
    const auto candidate = result.suggestions.first();
    QCOMPARE(candidate.field, LocalMetadata::Suggestion::Title);
    QCOMPARE(candidate.value, QString("가상의 책"));
    QVERIFY(!candidate.labelled);
    QVERIFY(!candidate.evidence.first().labelled);
    QVERIFY(candidate.reason.contains("다르게 읽은"));
    YACReaderArchiveInspectorDialog dialog;
    QSignalSpy requests(&dialog, &YACReaderArchiveInspectorDialog::titleSearchRequested);
    dialog.autoSearch->setChecked(true);
    dialog.showResult(result);
    QVERIFY(dialog.titleEdit->text().isEmpty());
    QVERIFY(dialog.pageText->toPlainText().contains("雨"));
    QVERIFY(dialog.pageText->toPlainText().contains("제목: 가상의 책"));
    dialog.requestSearch(true);
    QCOMPARE(requests.count(), 0);
    dialog.requestSearch(false);
    QCOMPARE(requests.count(), 1);
    QCOMPARE(requests.first().at(2).toString(), QString("가상의 책"));
    QVERIFY(!requests.first().at(7).toBool());
    QVERIFY(requests.first().at(8).toString().contains("미확정"));
}

void LocalMetadataTest::realNeuralOcr()
{
    if (qEnvironmentVariableIsEmpty("YACREADER_REQUIRE_NEURAL_OCR"))
        QSKIP("The neural Windows package job requires this test.");
    auto options = LocalMetadata::defaultOcrOptions();
    options.neural = true;
    QFETCH(QString, language);
    QFETCH(QString, fixture);
    QFETCH(QString, title);
    QFETCH(QString, credit);
    options.language = language;
    QImage image(QCoreApplication::applicationDirPath() + "/ocr-colophon-" + fixture + ".png");
    QVERIFY(!image.isNull());
    const auto reading = LocalMetadata::recognizePage(image, options, std::make_shared<std::atomic_bool>(false));
    QVERIFY2(reading.error.isEmpty(), qPrintable(reading.error));
    QVERIFY(reading.reviewRequired);
    QString compact = reading.text;
    compact.remove(QRegularExpression(QStringLiteral("\\s+")));
    QVERIFY2(compact.contains(title), qPrintable(reading.text));
    QVERIFY2(compact.contains(credit), qPrintable(reading.text));
    auto cancelled = std::make_shared<std::atomic_bool>(true);
    QVERIFY(LocalMetadata::recognizePage(image, options, cancelled).text.isEmpty());
}

void LocalMetadataTest::realNeuralOcr_data()
{
    QTest::addColumn<QString>("language");
    QTest::addColumn<QString>("fixture");
    QTest::addColumn<QString>("title");
    QTest::addColumn<QString>("credit");
    QTest::newRow("Korean") << QString("kor+eng") << QString("kor") << QString("푸른하늘") << QString("홍길동");
    QTest::newRow("Japanese") << QString("jpn+eng") << QString("jpn") << QString("青い空") << QString("見本太郎");
    QTest::newRow("Auto-Korean") << QString("auto") << QString("kor") << QString("푸른하늘") << QString("홍길동");
    QTest::newRow("Auto-Japanese") << QString("auto") << QString("jpn") << QString("青い空") << QString("見本太郎");
}

namespace {
LocalMetadata::Page detectedCover()
{
    LocalMetadata::Page page;
    page.number = 1;
    page.reading.reviewRequired = true;
    // Deliberately reverse detector order. Original boxes define Japanese
    // right-to-left columns even when OCR returns the left column first.
    page.reading.lines = { { "図書館", 92, QRect(680, 70, 100, 340) }, { "星降る夜の", 94, QRect(830, 50, 90, 550) }, { "著者：青木そら", 95, QRect(60, 1300, 270, 35) } };
    for (const auto &line : page.reading.lines)
        page.text += line.text + QLatin1Char('\n');
    return page;
}
LocalMetadata::Page detectedColophon()
{
    LocalMetadata::Page page;
    page.number = 8;
    page.kind = LocalMetadata::PageKind::Colophon;
    page.reading.reviewRequired = true;
    page.reading.lines = { { "タイトル：星降る夜の図書館", 96, QRect(100, 500, 400, 30) }, { "著者：青木そら", 97, QRect(100, 550, 240, 30) }, { "発行所：月のアトリエ", 95, QRect(100, 600, 280, 30) }, { "翻訳：橋本ゆき", 95, QRect(100, 650, 240, 30) }, { "奥付", 95, QRect(100, 450, 100, 35) } };
    for (const auto &line : page.reading.lines)
        page.text += line.text + QLatin1Char('\n');
    return page;
}
}

void LocalMetadataTest::coverTitlesAndCombinedEvidence()
{
    const auto cover = detectedCover();
    const auto colophon = detectedColophon();
    auto candidates = LocalMetadata::suggest({ cover }, QString());
    QCOMPARE(candidates.size(), 2);
    QCOMPARE(candidates.last().value, QString("星降る夜の図書館"));
    QVERIFY(!candidates.last().labelled);
    QCOMPARE(candidates.last().confidence, 92.0);
    auto slanted = cover;
    slanted.reading.lines.last().bounds = QRect(60, 1300, 270, 110);
    QCOMPARE(LocalMetadata::suggest({ slanted }, QString()).last().value, QString("星降る夜の図書館"));
    candidates = LocalMetadata::suggest({ cover, colophon }, "/TestLibrary/02_JP_Archive.cbz");
    QCOMPARE(candidates.size(), 3);
    for (const auto &candidate : candidates) {
        if (candidate.field == LocalMetadata::Suggestion::Publisher) {
            QCOMPARE(candidate.value, QString("月のアトリエ"));
            QCOMPARE(candidate.evidence.size(), 1);
        } else {
            QCOMPARE(candidate.evidence.size(), 2);
            QVERIFY(candidate.labelled);
            QCOMPARE(candidate.page, 8);
            QVERIFY(LocalMetadata::suggestionSource(candidate).contains("1, 8"));
        }
    }
    YACReaderArchiveInspectorDialog dialog;
    LocalMetadata::Result result;
    result.pages = { cover, colophon };
    result.suggestions = candidates;
    dialog.showResult(result);
    QCOMPARE(dialog.candidateList->count(), 3);
    QVERIFY(dialog.titleEdit->text().isEmpty());
    QVERIFY(dialog.authorEdit->text().isEmpty());

    auto horizontal = cover;
    horizontal.text = "비 오는 날의\n우체국\n글·그림 김하늘";
    horizontal.reading.lines = { { "비 오는 날의", 92, QRect(100, 70, 600, 90) }, { "우체국", 93, QRect(150, 180, 400, 90) }, { "글·그림 김하늘", 95, QRect(100, 1300, 300, 30) } };
    QCOMPARE(LocalMetadata::suggest({ horizontal }, QString()).last().value, QString("비 오는 날의 우체국"));
}

void LocalMetadataTest::coverLayoutRejectsUnrelatedText()
{
    auto noTitle = [](const LocalMetadata::Page &page) {
        for (const auto &candidate : LocalMetadata::suggest({ page }, QString()))
            if (candidate.field == LocalMetadata::Suggestion::Title)
                return false;
        return true;
    };
    auto page = detectedCover();
    page.number = 2;
    QVERIFY(noTitle(page));
    page = detectedCover();
    for (auto &line : page.reading.lines)
        line.bounds = QRect(); // Crop re-read must not mix coordinate systems.
    QVERIFY(noTitle(page));
    page = detectedCover();
    page.reading.lines[0].bounds.moveTop(850); // Unrelated column.
    QVERIFY(noTitle(page));
    page = detectedCover();
    page.error = "OCR failed";
    QVERIFY(noTitle(page));
    page = detectedCover();
    page.reading.lines.removeLast(); // No detected author credit.
    QVERIFY(noTitle(page));
    page = detectedCover();
    page.reading.lines[0].confidence = 20;
    page.reading.lines[1].confidence = 20;
    QVERIFY(noTitle(page));
    page = detectedCover();
    page.reading.lines[0].text = "ここで待とう。";
    page.reading.lines[1].text = "明日は晴れる？";
    QVERIFY(noTitle(page));
}

void LocalMetadataTest::creditSuffixAndCompoundLabels()
{
    LocalMetadata::Page page;
    page.number = 1;
    page.reading.lines = { { "비 오는", 96, QRect(100, 80, 400, 90) }, { "날의", 96, QRect(100, 190, 400, 90) }, { "작은", 96, QRect(100, 300, 400, 90) }, { "우체국", 96, QRect(100, 410, 400, 90) }, { "김하늘", 94, QRect(100, 1200, 120, 30) }, { "지음", 95, QRect(100, 1240, 80, 30) } };
    for (const auto &line : page.reading.lines)
        page.text += line.text + QLatin1Char('\n');
    auto candidates = LocalMetadata::suggest({ page }, QString());
    QCOMPARE(candidates.size(), 2);
    QCOMPARE(candidates.first().field, LocalMetadata::Suggestion::Author);
    QCOMPARE(candidates.first().value, QString("김하늘"));
    QCOMPARE(candidates.last().value, QString("비 오는 날의 작은 우체국"));
    QVERIFY(!candidates.last().labelled);

    page.reading.lines.insert(4, { "푸른", 96, QRect(100, 1160, 90, 30) });
    page.text.clear();
    for (const auto &line : page.reading.lines)
        page.text += line.text + QLatin1Char('\n');
    QCOMPARE(LocalMetadata::suggest({ page }, QString()).first().value, QString("푸른 김하늘"));

    page.number = 30;
    page.reading.lines.clear();
    page.text = "奥付\n誌名\n雨の図書館\n発行／著者\n月の工房 / 高橋あおい\n翻訳\nWrong Person";
    candidates = LocalMetadata::suggest({ page }, QString());
    QCOMPARE(candidates.size(), 3);
    QCOMPARE(candidates[0].value, QString("雨の図書館"));
    QCOMPARE(candidates[1].field, LocalMetadata::Suggestion::Publisher);
    QCOMPARE(candidates[1].value, QString("月の工房"));
    QCOMPARE(candidates[2].field, LocalMetadata::Suggestion::Author);
    QCOMPARE(candidates[2].value, QString("高橋あおい"));
    page.text = "発行／著者\n月の工房 /";
    candidates = LocalMetadata::suggest({ page }, QString());
    QCOMPARE(candidates.size(), 1);
    QCOMPARE(candidates.first().field, LocalMetadata::Suggestion::Publisher);
    page.text = "発行／著者\n/";
    QVERIFY(LocalMetadata::suggest({ page }, QString()).isEmpty());
    page.text = "김하늘 지음";
    QCOMPARE(LocalMetadata::suggest({ page }, QString()).first().value, QString("김하늘"));
}

void LocalMetadataTest::bracketedInlineCredits()
{
    LocalMetadata::Page page;
    page.number = 20;
    page.text = "[Circle]月の工房\n[Author] Alice Example\n[Translator] Wrong Person";
    const auto candidates = LocalMetadata::suggest({ page }, QString());
    QCOMPARE(candidates.size(), 2);
    QCOMPARE(candidates[0].field, LocalMetadata::Suggestion::Publisher);
    QCOMPARE(candidates[0].value, QString("月の工房"));
    QCOMPARE(candidates[1].field, LocalMetadata::Suggestion::Author);
    QCOMPARE(candidates[1].value, QString("Alice Example"));
    for (const auto &candidate : candidates)
        QVERIFY(candidate.labelled);
    page.text = "［作者・サークル名］青木そら";
    const auto ambiguous = LocalMetadata::suggest({ page }, QString());
    QCOMPARE(ambiguous.size(), 2);
    for (const auto &candidate : ambiguous)
        QVERIFY(!candidate.labelled);
    for (const auto &text : QStringList { "[Circle]\nTwitter", "[Circle]   \nTwitter", "[Translator] Wrong Person", "Unknown [Author] Wrong Person", "[Author] ここで待とう。", "[Unknown] Author: Wrong Person", "[Author Alice Example" }) {
        page.text = text;
        QVERIFY2(LocalMetadata::suggest({ page }, QString()).isEmpty(), qPrintable(text));
    }
}

void LocalMetadataTest::bulletSeparatedCredits()
{
    LocalMetadata::Page page;
    page.number = 20;
    page.text = "発行者●青木そら\n発行所◉月の工房\n印刷所●架空印刷会社\n発行日●2025年1月1日";
    QCOMPARE(LocalMetadata::classifyPage(page.text, page.number), LocalMetadata::PageKind::Colophon);
    const auto candidates = LocalMetadata::suggest({ page }, QString());
    QCOMPARE(candidates.size(), 2);
    QCOMPARE(candidates[0].value, QString("青木そら"));
    QCOMPARE(candidates[1].value, QString("月の工房"));
    for (const auto &candidate : candidates) {
        QCOMPARE(candidate.field, LocalMetadata::Suggestion::Publisher);
        QVERIFY(candidate.labelled);
    }
    page.text = "著者●青木そら\nTitle: A Circle ● In The Rain";
    const auto titled = LocalMetadata::suggest({ page }, QString());
    QCOMPARE(titled.size(), 2);
    QCOMPARE(titled[0].field, LocalMetadata::Suggestion::Author);
    QCOMPARE(titled[0].value, QString("青木そら"));
    QCOMPARE(titled[1].value, QString("A Circle ● In The Rain"));
    page.text = "作者・サークル名●青木そら";
    const auto ambiguous = LocalMetadata::suggest({ page }, QString());
    QCOMPARE(ambiguous.size(), 2);
    for (const auto &candidate : ambiguous)
        QVERIFY(!candidate.labelled);
    for (const auto &text : QStringList { "ここでは著者●青木そら", "著者●ここで待とう。", "著者青木そら", "著者●\n翻訳●Wrong Person", "翻訳●Wrong Person", "Unknown●著者: Wrong Person" }) {
        page.text = text;
        QVERIFY2(LocalMetadata::suggest({ page }, QString()).isEmpty(), qPrintable(text));
    }
}

void LocalMetadataTest::joinedAuthorRequiresPublishingLayout()
{
    LocalMetadata::Page base;
    base.number = 20;
    base.reading.lines = { { "著者ツキノソラAT", 91, QRect(100, 100, 300, 40) }, { "発行者●青木そら", 96, QRect(100, 145, 280, 40) }, { "印刷所●架空印刷会社", 96, QRect(100, 190, 360, 40) } };
    auto suggestions = [](LocalMetadata::Page page) {
        page.text.clear();
        for (const auto &line : page.reading.lines)
            page.text += line.text + QLatin1Char('\n');
        return LocalMetadata::suggest({ page }, QString());
    };
    auto authors = [&](const LocalMetadata::Page &page) {
        auto values = suggestions(page);
        values.erase(std::remove_if(values.begin(), values.end(), [](const LocalMetadata::Suggestion &value) { return value.field != LocalMetadata::Suggestion::Author; }), values.end());
        return values;
    };
    const auto candidates = authors(base);
    QCOMPARE(candidates.size(), 1);
    QCOMPARE(candidates.first().value, QString("ツキノソラAT"));
    QVERIFY(!candidates.first().labelled);
    QVERIFY(!candidates.first().evidence.first().labelled);
    QVERIFY(candidates.first().reason.contains("구분자"));
    LocalMetadata::Result result;
    result.pages = { base };
    result.suggestions = suggestions(base);
    YACReaderArchiveInspectorDialog dialog;
    QSignalSpy requests(&dialog, &YACReaderArchiveInspectorDialog::titleSearchRequested);
    dialog.showResult(result);
    QVERIFY(dialog.authorEdit->text().isEmpty());
    dialog.requestSearch(true);
    QCOMPARE(requests.count(), 0);
    dialog.requestSearch(false);
    QCOMPARE(requests.count(), 1);
    QCOMPARE(requests.first().at(3).toString(), QString("ツキノソラAT"));
    QVERIFY(!requests.first().at(7).toBool());
    // No role boundary relaxation in plain text, even with colophon labels.
    auto plain = base;
    plain.text = "著者ツキノソラAT\n発行者●青木そら\n印刷所●架空印刷会社";
    plain.reading.lines.clear();
    const auto plainValues = LocalMetadata::suggest({ plain }, QString());
    QCOMPARE(plainValues.size(), 1);
    QCOMPARE(plainValues.first().field, LocalMetadata::Suggestion::Publisher);
    for (int index = 0; index < 3; ++index) {
        auto missing = base;
        missing.reading.lines[index].bounds = QRect();
        QVERIFY(authors(missing).isEmpty());
        auto low = base;
        low.reading.lines[index].confidence = 84;
        QVERIFY(authors(low).isEmpty());
    }
    for (int index = 1; index < 3; ++index) {
        auto missing = base;
        missing.reading.lines.removeAt(index);
        QVERIFY(authors(missing).isEmpty());
        for (const auto &bounds : { QRect(600, 145, 300, 40), QRect(100, 900, 300, 40), QRect(100, 145, 30, 100), QRect(100, 145, 300, 10), QRect(100, 100, 300, 40) }) {
            auto unrelated = base;
            unrelated.reading.lines[index].bounds = bounds;
            QVERIFY(authors(unrelated).isEmpty());
        }
    }
    for (const auto &text : QStringList { "著者はここで待とう", "著者ツキノソラです", "著者ツキノソラ。", "著者ABC", "著者サークル名", "作者ツキノソラ", "翻訳ツキノソラ", "ここでは著者ツキノソラ", "著者ツキノソラ@example.com" }) {
        auto unrelated = base;
        unrelated.reading.lines.first().text = text;
        QVERIFY2(authors(unrelated).isEmpty(), qPrintable(text));
    }
    auto sameRole = base;
    sameRole.reading.lines[2].text = "発行所●月の工房";
    QVERIFY(authors(sameRole).isEmpty());
    auto independent = base;
    independent.reading.lines.append({ "著者: ツキノソラAT", 95, QRect(100, 900, 300, 40) });
    const auto supported = authors(independent);
    QCOMPARE(supported.size(), 1);
    QVERIFY(supported.first().labelled);
    // The tentative author must not make a large cover line a title candidate.
    auto cover = base;
    cover.number = 1;
    cover.reading.lines.prepend({ "雨の図書館", 96, QRect(100, 10, 600, 80) });
    for (const auto &candidate : suggestions(cover))
        QVERIFY(candidate.field != LocalMetadata::Suggestion::Title);
}

void LocalMetadataTest::joinedCircleBannerNeedsReview()
{
    LocalMetadata::Page base;
    base.number = 2;
    base.reading.lines = { { "サークル架空工房・月野ソラ", 91, QRect(100, 40, 650, 70), "jpn" } };
    auto suggestions = [](LocalMetadata::Page page) {
        page.text.clear();
        for (const auto &line : page.reading.lines)
            page.text += line.text + QLatin1Char('\n');
        return LocalMetadata::suggest({ page }, QString());
    };
    const auto candidates = suggestions(base);
    QCOMPARE(candidates.size(), 1);
    QCOMPARE(candidates.first().field, LocalMetadata::Suggestion::Publisher);
    QCOMPARE(candidates.first().value, QString("架空工房・月野ソラ"));
    QVERIFY(!candidates.first().labelled);
    QVERIFY(!candidates.first().evidence.first().labelled);
    QVERIFY(candidates.first().reason.contains("나누지"));
    LocalMetadata::Result result;
    result.pages = { base };
    result.suggestions = candidates;
    YACReaderArchiveInspectorDialog dialog;
    QSignalSpy requests(&dialog, &YACReaderArchiveInspectorDialog::titleSearchRequested);
    dialog.showResult(result);
    QVERIFY(dialog.titleEdit->text().isEmpty());
    QVERIFY(dialog.authorEdit->text().isEmpty());
    QVERIFY(dialog.publisherEdit->text().isEmpty());
    dialog.autoSearch->setChecked(true);
    dialog.requestSearch(true);
    dialog.requestSearch(false); // A publisher alone is not a title/author query.
    QCOMPARE(requests.count(), 0);
    QCOMPARE(dialog.candidateList->count(), 1);
    for (const auto &box : { QRect(), QRect(100, 500, 650, 70), QRect(100, 40, 200, 70), QRect(100, -1, 650, 70) }) {
        auto unrelated = base;
        unrelated.reading.lines.first().bounds = box;
        QVERIFY(suggestions(unrelated).isEmpty());
    }
    for (const auto &text : QStringList { "サークル活動を楽しもう", "ここはサークル架空工房・月野ソラ", "サークル架空工房・月野ソラ。", "サークル架空工房・月野ソラ・別の名前", "サークル架空工房・", "サークル架空工房・https://example.test", "ただ自分としては、この本の作者として、皆さん" }) {
        auto unrelated = base;
        unrelated.reading.lines.first().text = text;
        QVERIFY2(suggestions(unrelated).isEmpty(), text.toUtf8().toHex().constData());
    }
    auto low = base;
    low.reading.lines.first().confidence = 84;
    QVERIFY(suggestions(low).isEmpty());
    auto body = base;
    body.number = 4;
    QVERIFY(suggestions(body).isEmpty());
    auto failed = base;
    failed.error = "failed";
    QVERIFY(suggestions(failed).isEmpty());
    auto uncertain = base;
    uncertain.reading.uncertainLanguage = true;
    QVERIFY(suggestions(uncertain).isEmpty());
    auto plain = base;
    plain.text = base.reading.lines.first().text;
    plain.reading.lines.clear();
    QVERIFY(LocalMetadata::suggest({ plain }, QString()).isEmpty());
    auto explicitCredit = base;
    explicitCredit.reading.lines.first().text = "サークル: 架空工房・月野ソラ";
    const auto explicitValues = suggestions(explicitCredit);
    QCOMPARE(explicitValues.size(), 1);
    QVERIFY(explicitValues.first().labelled); // Existing explicit parsing is unchanged.
    auto cover = base;
    cover.number = 1;
    cover.reading.lines.append({ "雨の図書館", 96, QRect(100, 400, 900, 180) });
    QCOMPARE(suggestions(cover).size(), 1); // No title or author inferred from the banner.
}

void LocalMetadataTest::publisherHintsRespectReviewBoundary()
{
    LocalMetadata::Page titlePage;
    titlePage.number = 20;
    titlePage.kind = LocalMetadata::PageKind::Colophon;
    titlePage.reading = LocalMetadata::parseTsv(tsvFor({ "作品名: 雨の図書館", "発行日: 2026年1月1日" }), "jpn+eng");
    titlePage.text = titlePage.reading.text;
    LocalMetadata::Page banner;
    banner.number = 2;
    banner.reading.lines = { { "サークル架空工房・月野ソラ", 91, QRect(100, 40, 650, 70), "jpn" } };
    banner.text = banner.reading.lines.first().text;
    LocalMetadata::Result result;
    result.pages = { titlePage, banner };
    result.suggestions = LocalMetadata::suggest(result.pages, QString());
    for (bool automatic : { true, false }) {
        YACReaderArchiveInspectorDialog dialog;
        QSignalSpy requests(&dialog, &YACReaderArchiveInspectorDialog::titleSearchRequested);
        dialog.showResult(result);
        QCOMPARE(dialog.titleEdit->text(), QString("雨の図書館"));
        dialog.autoSearch->setChecked(true);
        dialog.requestSearch(automatic);
        QCOMPARE(requests.count(), 1);
        QCOMPARE(requests.first().at(7).toBool(), automatic);
        QCOMPARE(requests.first().at(6).toStringList(), automatic ? QStringList() : QStringList { "架空工房・月野ソラ" });
    }
    // Explicit labels from a neural/uncertain page still need review. A filled
    // publisher edit must not reintroduce them into the automatic request.
    for (bool neural : { false, true }) {
        auto review = result;
        review.pages[1].kind = LocalMetadata::PageKind::Colophon;
        review.pages[1].reading.reviewRequired = neural;
        review.pages[1].reading.uncertainLanguage = !neural;
        review.suggestions.last().labelled = true;
        review.suggestions.last().evidence.first().labelled = true;
        YACReaderArchiveInspectorDialog dialog;
        QSignalSpy requests(&dialog, &YACReaderArchiveInspectorDialog::titleSearchRequested);
        dialog.showResult(review);
        dialog.publisherEdit->setText("架空工房・月野ソラ");
        dialog.autoSearch->setChecked(true);
        dialog.requestSearch(true);
        QCOMPARE(requests.count(), 1);
        QVERIFY(requests.first().at(6).toStringList().isEmpty());
    }
}

void LocalMetadataTest::alternatePairedLabelRequiresAdjacentNames()
{
    LocalMetadata::Page base;
    base.number = 20;
    base.reading.reviewRequired = true;
    base.reading.lines = { { "奥付", 99, QRect(230, 10, 80, 40), "jpn" }, { "/", 99, QRect(150, 100, 260, 40), "kor" }, { "架空工房／月野ソラ", 96, QRect(80, 148, 400, 45), "jpn" } };
    base.reading.alternatives = { { "発行/著者", 94, QRect(150, 100, 260, 40), "jpn" } };
    auto suggestions = [](LocalMetadata::Page page) {
        page.text.clear();
        for (const auto &line : page.reading.lines)
            page.text += line.text + QLatin1Char('\n');
        return LocalMetadata::suggest({ page }, QString());
    };
    const auto values = suggestions(base);
    QCOMPARE(values.size(), 2);
    QCOMPARE(values[0].field, LocalMetadata::Suggestion::Publisher);
    QCOMPARE(values[0].value, QString("架空工房"));
    QCOMPARE(values[1].field, LocalMetadata::Suggestion::Author);
    QCOMPARE(values[1].value, QString("月野ソラ"));
    for (const auto &value : values) {
        QVERIFY(!value.labelled);
        QVERIFY(!value.evidence.first().labelled);
        QCOMPARE(value.confidence, 94.0);
    }
    LocalMetadata::Result result;
    result.pages = { base };
    result.suggestions = values;
    YACReaderArchiveInspectorDialog dialog;
    QSignalSpy requests(&dialog, &YACReaderArchiveInspectorDialog::titleSearchRequested);
    dialog.showResult(result);
    QVERIFY(dialog.authorEdit->text().isEmpty());
    QVERIFY(dialog.publisherEdit->text().isEmpty());
    dialog.autoSearch->setChecked(true);
    dialog.requestSearch(true);
    QCOMPARE(requests.count(), 0);
    dialog.requestSearch(false);
    QCOMPARE(requests.count(), 1);
    QCOMPARE(requests.first().at(3).toString(), QString("月野ソラ"));
    QVERIFY(!requests.first().at(7).toBool());
    auto noContext = base;
    noContext.reading.lines.removeFirst();
    QVERIFY(suggestions(noContext).isEmpty());
    auto intervening = base;
    intervening.reading.lines.insert(2, { "別の行", 96, QRect(80, 142, 400, 40), "jpn" });
    QVERIFY(suggestions(intervening).isEmpty());
    for (const auto &bounds : { QRect(), QRect(80, 300, 400, 45), QRect(80, 100, 400, 45), QRect(800, 148, 400, 45), QRect(80, 148, 400, 10), QRect(80, 148, 40, 100) }) {
        auto unrelated = base;
        unrelated.reading.lines.last().bounds = bounds;
        QVERIFY(suggestions(unrelated).isEmpty());
    }
    for (const auto &name : QStringList { "架空工房", "架空工房/", "/月野ソラ", "架空工房/月野ソラ/別の名前", "架空工房/https://example.test", "架空工房/月野ソラです。" }) {
        auto unrelated = base;
        unrelated.reading.lines.last().text = name;
        QVERIFY(suggestions(unrelated).isEmpty());
    }
    for (const auto &role : QStringList { "著者", "発行", "作者・サークル名", "翻訳" }) {
        auto unrelated = base;
        unrelated.reading.alternatives.first().text = role;
        QVERIFY(suggestions(unrelated).isEmpty());
    }
    auto lowLabel = base;
    lowLabel.reading.alternatives.first().confidence = 84;
    QVERIFY(suggestions(lowLabel).isEmpty());
    auto lowName = base;
    lowName.reading.lines.last().confidence = 84;
    QVERIFY(suggestions(lowName).isEmpty());
    auto otherLanguage = base;
    otherLanguage.reading.lines.last().language = "kor";
    QVERIFY(suggestions(otherLanguage).isEmpty());
    auto wrongBox = base;
    wrongBox.reading.alternatives.first().bounds.translate(10, 0);
    QVERIFY(suggestions(wrongBox).isEmpty());
    auto failed = base;
    failed.error = "failed";
    QVERIFY(suggestions(failed).isEmpty());
    auto plain = base;
    plain.text = "奥付\n/\n架空工房／月野ソラ";
    plain.reading.lines.clear();
    QVERIFY(LocalMetadata::suggest({ plain }, QString()).isEmpty());
}

void LocalMetadataTest::sharedAuthorCircleNeedsReview()
{
    LocalMetadata::Page page;
    page.number = 1;
    page.reading.lines = { { "雨の図書館", 96, QRect(10, 10, 400, 90) }, { "作者・サークル名:青木そら", 96, QRect(10, 300, 300, 30) } };
    page.text = "雨の図書館\n作者・サークル名:青木そら";
    const auto candidates = LocalMetadata::suggest({ page }, QString());
    QCOMPARE(candidates.size(), 2); // No cover-title inference from an ambiguous role.
    QCOMPARE(candidates[0].field, LocalMetadata::Suggestion::Author);
    QCOMPARE(candidates[1].field, LocalMetadata::Suggestion::Publisher);
    for (const auto &candidate : candidates) {
        QCOMPARE(candidate.value, QString("青木そら"));
        QVERIFY(!candidate.labelled);
        QVERIFY(!candidate.evidence.first().labelled);
    }
    LocalMetadata::Result result;
    result.pages = { page };
    result.suggestions = candidates;
    YACReaderArchiveInspectorDialog dialog;
    QSignalSpy requests(&dialog, &YACReaderArchiveInspectorDialog::titleSearchRequested);
    dialog.showResult(result);
    QVERIFY(dialog.authorEdit->text().isEmpty());
    dialog.requestSearch(true);
    QCOMPARE(requests.count(), 0);
    page.reading.lines.clear();
    page.text = "発行サークル:月の工房";
    const auto publisher = LocalMetadata::suggest({ page }, QString());
    QCOMPARE(publisher.size(), 1);
    QCOMPARE(publisher.first().field, LocalMetadata::Suggestion::Publisher);
    page.text = "作者・サークル名:青木そら / 月の工房";
    QVERIFY(LocalMetadata::suggest({ page }, QString()).isEmpty());
    page.text = "作者・サークル名:ここで待とう。";
    QVERIFY(LocalMetadata::suggest({ page }, QString()).isEmpty());
}

void LocalMetadataTest::creditGeometryAndDialogue()
{
    LocalMetadata::Page page;
    page.number = 2;
    for (const auto &text : QStringList { "그림\n실례할게요", "작가: 여기서 기다릴까요?", "著者\n翻訳\nWrong Person", "발행/저자\n알 수 없음" }) {
        page.text = text;
        QVERIFY2(LocalMetadata::suggest({ page }, QString()).isEmpty(), qPrintable(text));
    }
    page.text = "著者\nOther Bubble";
    page.reading.lines = { { "著者", 90, QRect(100, 100, 80, 30) }, { "Other Bubble", 95, QRect(700, 500, 220, 30) } };
    QVERIFY(LocalMetadata::suggest({ page }, QString()).isEmpty());
    page.text = "그림\n홍길동";
    page.reading.lines.clear();
    QVERIFY(LocalMetadata::suggest({ page }, QString()).isEmpty());
}

void LocalMetadataTest::localNeuralDevice()
{
    const auto expected = qEnvironmentVariable("YACREADER_EXPECT_NEURAL_DEVICE");
    if (expected.isEmpty())
        QSKIP("Explicit local CPU-fallback / physical-GPU verification only.");
    QVERIFY(expected == "cpu" || expected == "gpu:0");
    auto options = LocalMetadata::defaultOcrOptions();
    options.neural = true;
    options.gpu = true;
    options.language = "auto";
    QVector<QImage> images;
    for (const auto &language : { "kor", "jpn" }) {
        images.append(QImage(QCoreApplication::applicationDirPath() + QString("/ocr-colophon-%1.png").arg(language)));
        QVERIFY(!images.last().isNull());
    }
    const auto readings = LocalMetadata::recognizePages(images, options, std::make_shared<std::atomic_bool>(false));
    QCOMPARE(readings.size(), 2);
    for (int i = 0; i < readings.size(); ++i) {
        const auto &reading = readings.at(i);
        QVERIFY2(reading.error.isEmpty(), qPrintable(reading.error));
        QCOMPARE(reading.device, expected);
        QCOMPARE(reading.warning.isEmpty(), expected == "gpu:0");
        QString text = reading.text;
        text.remove(QRegularExpression(QStringLiteral("\\s+")));
        QVERIFY2(text.contains(i ? QString("見本太郎") : QString("홍길동")), qPrintable(reading.text));
        QVERIFY(reading.reviewRequired);
        qInfo("Local requested GPU: actual=%s page=%d reading=%lld ms initialization=%lld ms",
              qPrintable(reading.device), i + 1, reading.elapsedMs, reading.initializationMs);
    }
}

void LocalMetadataTest::realNeuralWorkReuse()
{
    if (qEnvironmentVariableIsEmpty("YACREADER_REQUIRE_NEURAL_OCR"))
        QSKIP("The neural Windows package job requires this test.");
    auto options = LocalMetadata::defaultOcrOptions();
    options.neural = true;
    options.language = "auto";
    QVector<QImage> images;
    for (int i = 0; i < 6; ++i) {
        images.append(QImage(QCoreApplication::applicationDirPath() + (i % 2 ? "/ocr-colophon-jpn.png" : "/ocr-colophon-kor.png")));
        QVERIFY(!images.last().isNull());
    }
    int lastCompleted = -1;
    QElapsedTimer timer;
    timer.start();
    const auto readings = LocalMetadata::recognizePages(images, options, std::make_shared<std::atomic_bool>(false),
                                                        [&](int completed, int total, const QString &stage) {
                                                            QCOMPARE(total, 6);
                                                            QVERIFY(completed >= lastCompleted);
                                                            QVERIFY(!stage.isEmpty());
                                                            lastCompleted = completed;
                                                        });
    const auto batchMs = timer.elapsed();
    QCOMPARE(readings.size(), 6);
    QCOMPARE(lastCompleted, 6);
    for (int i = 0; i < readings.size(); ++i) {
        const auto &reading = readings.at(i);
        QVERIFY2(reading.error.isEmpty(), qPrintable(reading.error));
        QVERIFY(reading.reviewRequired);
        QCOMPARE(reading.device, QString("cpu"));
        QVERIFY(reading.elapsedMs > 0);
        if (i == 0)
            QVERIFY(reading.initializationMs > 0);
        else
            QCOMPARE(reading.initializationMs, 0);
        QString text = reading.text;
        text.remove(QRegularExpression(QStringLiteral("\\s+")));
        QVERIFY2(text.contains(i % 2 ? QString("見本太郎") : QString("홍길동")), qPrintable(reading.text));
    }
    // Measure the old per-page process pattern with identical pixels/models.
    // Benchmark only on the staged runtime; installed verification checks reuse.
    if (!qEnvironmentVariableIsEmpty("YACREADER_BENCHMARK_NEURAL_OCR")) {
        timer.restart();
        for (const auto &image : images) {
            const auto reading = LocalMetadata::recognizePage(image, options, std::make_shared<std::atomic_bool>(false));
            QVERIFY2(reading.error.isEmpty(), qPrintable(reading.error));
        }
        qInfo("OCR six pages, auto: separate processes=%lld ms; shared process=%lld ms", timer.elapsed(), batchMs);
    }
    qInfo("OCR shared six-page job: %lld ms", batchMs);
}

void LocalMetadataTest::nameHintsExcludeLibraryRoots()
{
    QVERIFY(LocalMetadata::suggest({ }, "/TestLibrary/02_JP_Archive.cbz").isEmpty());
    QVERIFY(LocalMetadata::suggest({ }, QString()).isEmpty());
    const auto rootHints = LocalMetadata::suggest({ }, "/My personal collection/Evening.cbz", "/My personal collection");
    QCOMPARE(rootHints.size(), 1);
    QCOMPARE(rootHints.first().field, LocalMetadata::Suggestion::Title);
    const auto authorHints = LocalMetadata::suggest({ }, "/Library/青木そら/Evening.cbz", "/Library");
    QCOMPARE(authorHints.last().value, QString("青木そら"));
    QCOMPARE(authorHints.last().page, 0);
    const auto bracket = LocalMetadata::suggest({ }, "/Library/[Alice Example] Evening.cbz", "/Library");
    QCOMPARE(bracket.first().value, QString("Alice Example"));
    const auto languageMarker = LocalMetadata::suggest({ }, "/downloads/[Japanese] Evening.cbz");
    QCOMPARE(languageMarker.size(), 1);
    QCOMPARE(languageMarker.first().field, LocalMetadata::Suggestion::Title);
    for (const auto &marker : QStringList { "미번", "미번역", "번역", "한글", "한글판", "히토미펌", "Repost", "Reupload", "転載" }) {
        const auto tagged = LocalMetadata::suggest({ }, QString("/library/[%1] Example Book.cbz").arg(marker), "/library");
        QCOMPARE(tagged.size(), 1);
        QCOMPARE(tagged.first().field, LocalMetadata::Suggestion::Title);
        QCOMPARE(tagged.first().value, QString("Example Book"));
        QVERIFY(!tagged.first().labelled);
        const auto parent = LocalMetadata::suggest({ }, QString("/library/%1/Example Book.cbz").arg(marker), "/library");
        QCOMPARE(parent.size(), 1);
        QCOMPARE(parent.first().field, LocalMetadata::Suggestion::Title);
    }
    const auto creator = LocalMetadata::suggest({ }, "/library/[Alice Example] Example Book.cbz", "/library");
    QCOMPARE(creator.size(), 2);
    QCOMPARE(creator.first().field, LocalMetadata::Suggestion::Author);
    QCOMPARE(creator.first().value, QString("Alice Example"));
    QVERIFY(!creator.first().labelled);
    QTemporaryDir temporary;
    QVERIFY(QDir().mkpath(temporary.filePath("My collection/01_KO_Folder")));
    QVERIFY(LocalMetadata::suggest({ }, temporary.filePath("My collection/01_KO_Folder"), temporary.filePath("My collection")).isEmpty());
    const auto nativeHints = LocalMetadata::suggest({ }, QDir::toNativeSeparators(temporary.filePath("My collection/Evening.cbz")), temporary.filePath("My collection/./"));
    QCOMPARE(nativeHints.size(), 1);
    QCOMPARE(nativeHints.first().field, LocalMetadata::Suggestion::Title);
#ifdef Q_OS_WIN
    const auto caseHints = LocalMetadata::suggest({ }, temporary.filePath("My collection/Evening.cbz"), temporary.filePath("MY COLLECTION"));
    QCOMPARE(caseHints.size(), 1);
#endif
}

void LocalMetadataTest::filenameHintsHandleDecorations()
{
    const QString root = QDir::tempPath() + "/library";
    struct Example {
        QString file;
        QString title;
        QString author;
    };
    const QList<Example> examples {
        { "[AI번역] [Alice Example] Evening Garden.cbz", "Evening Garden", "Alice Example" },
        { "[번역][풀컬러] Evening Garden 2.zip", "Evening Garden 2", "" },
        { "(edition) [translated] (digital) [Alice Example] Evening Garden.cbz", "Evening Garden", "Alice Example" },
        { "[Category One][Category Two] Evening Garden.zip", "Evening Garden", "" },
        { "[Alice Example][Alice Example] Evening Garden.zip", "Evening Garden", "Alice Example" },
        { "[translated]", "", "" },
        { "[translation] [fullcolor]", "", "" },
        { "[Alice Example]", "", "Alice Example" },
    };
    for (const auto &example : examples) {
        const auto candidates = LocalMetadata::suggest({ }, root + "/" + example.file, root);
        QStringList titles, authors;
        for (const auto &candidate : candidates) {
            QCOMPARE(candidate.page, 0);
            QVERIFY(!candidate.labelled);
            QVERIFY(candidate.evidence.isEmpty());
            (candidate.field == LocalMetadata::Suggestion::Title ? titles : authors).append(candidate.value);
        }
        QCOMPARE(titles, example.title.isEmpty() ? QStringList() : QStringList { example.title });
        QCOMPARE(authors, example.author.isEmpty() ? QStringList() : QStringList { example.author });
    }
    QTemporaryDir temporary;
    const QString folder = temporary.filePath("[AI번역][Alice Example] Evening Garden.v2");
    QVERIFY(QDir().mkdir(folder));
    const auto folderHints = LocalMetadata::suggest({ }, folder, temporary.path());
    QCOMPARE(folderHints.size(), 2);
    QCOMPARE(folderHints.last().value, QString("Evening Garden.v2"));
}

void LocalMetadataTest::filenameFallbackRequiresExplicitReview()
{
    LocalMetadata::Page page;
    page.number = 1;
    page.reading.reviewRequired = true;
    page.reading.engine = "PaddleOCR";
    LocalMetadata::Result result;
    result.pageCount = 20;
    result.pages = { page };
    result.suggestions = LocalMetadata::suggest(result.pages, "/library/[translated][Alice Example] Evening Garden.cbz", "/library");
    YACReaderArchiveInspectorDialog dialog;
    QCOMPARE(dialog.pageLimit->maximum(), 3);
    QCOMPARE(dialog.pageLimit->value(), 3);
    dialog.showResult(result, true);
    QVERIFY(dialog.filenameFallback);
    QVERIFY(dialog.statusLabel->text().contains("파일명"));
    QVERIFY(dialog.searchButton->text().contains("파일명"));
    QVERIFY(dialog.titleEdit->text().isEmpty());
    QVERIFY(dialog.authorEdit->text().isEmpty());
    QSignalSpy requests(&dialog, &YACReaderArchiveInspectorDialog::titleSearchRequested);
    dialog.requestSearch(true);
    QCOMPARE(requests.count(), 0);
    QVERIFY(!dialog.automaticSearchDone);
    dialog.requestSearch(false);
    QCOMPARE(requests.count(), 1);
    QCOMPARE(requests.first().at(2).toString(), QString("Evening Garden"));
    QVERIFY(requests.first().at(3).toString().isEmpty());
    QCOMPARE(requests.first().at(5).toStringList(), QStringList { "Alice Example" });
    QVERIFY(!requests.first().at(7).toBool()); // Review only, no network or database write.
    QVERIFY(requests.first().at(8).toString().contains("힌트"));
}

void LocalMetadataTest::filenameFallbackDistinguishesErrorsAndPageEvidence()
{
    LocalMetadata::Page blank;
    blank.number = 1;
    LocalMetadata::Result result;
    result.pages = { blank };
    result.suggestions = LocalMetadata::suggest(result.pages, "/library/Evening Garden.cbz", "/library");
    YACReaderArchiveInspectorDialog dialog;
    dialog.showResult(result);
    QVERIFY(!dialog.filenameFallback); // Preview is not completed OCR.
    auto error = result;
    error.pages[0].error = "Unreadable image";
    dialog.showResult(error, true);
    QVERIFY(!dialog.filenameFallback);
    error = result;
    error.pages[0].reading.error = "Missing model";
    dialog.showResult(error, true);
    QVERIFY(!dialog.filenameFallback);
    error = result;
    error.error = "Archive failed";
    dialog.showResult(error, true);
    QVERIFY(!dialog.filenameFallback);
    auto publisher = result;
    publisher.pages[0].text = "サークル: Example Studio";
    publisher.suggestions = LocalMetadata::suggest(publisher.pages, "/library/Evening Garden.cbz", "/library");
    dialog.showResult(publisher, true);
    QVERIFY(!dialog.filenameFallback); // Even tentative page identity evidence is retained.
    dialog.showResult(result, true);
    QVERIFY(dialog.filenameFallback);
    dialog.titleEdit->setText("User Selected Title");
    QSignalSpy requests(&dialog, &YACReaderArchiveInspectorDialog::titleSearchRequested);
    dialog.requestSearch(false);
    QCOMPARE(requests.count(), 1);
    QCOMPARE(requests.first().at(2).toString(), QString("User Selected Title"));
    QVERIFY(!requests.first().at(7).toBool());
    dialog.showResult({ }, true);
    QVERIFY(!dialog.filenameFallback);
}

void LocalMetadataTest::neuralCandidatesOpenReviewBeforeSearch()
{
    YACReaderArchiveInspectorDialog dialog;
    LocalMetadata::Result result;
    result.pages = { detectedCover(), detectedColophon() };
    result.pageCount = 8;
    result.suggestions = LocalMetadata::suggest(result.pages, QString());
    dialog.showResult(result);
    QSignalSpy requests(&dialog, &YACReaderArchiveInspectorDialog::titleSearchRequested);
    dialog.autoSearch->setChecked(true);
    dialog.requestSearch(true);
    QCOMPARE(requests.count(), 0);
    dialog.requestSearch(false);
    QCOMPARE(requests.count(), 1);
    QCOMPARE(requests.first().at(2).toString(), QString("星降る夜の図書館"));
    QCOMPARE(requests.first().at(3).toString(), QString("青木そら"));
    QVERIFY(!requests.first().at(7).toBool()); // Open review; no HTTP request.
    QVERIFY(requests.first().at(8).toString().contains("1, 8"));
    QCOMPARE(requests.first().at(6).toStringList(), QStringList { "月のアトリエ" });
}

void LocalMetadataTest::conflictingCandidatesRequireSelection()
{
    YACReaderArchiveInspectorDialog dialog;
    LocalMetadata::Result result;
    auto other = detectedColophon();
    other.number = 7;
    other.text.replace("青木そら", "青木うみ");
    other.reading.lines[1].text = "著者：青木うみ";
    result.pages = { detectedCover(), detectedColophon(), other };
    result.suggestions = LocalMetadata::suggest(result.pages, QString());
    dialog.showResult(result);
    QSignalSpy requests(&dialog, &YACReaderArchiveInspectorDialog::titleSearchRequested);
    dialog.requestSearch(false);
    QCOMPARE(requests.count(), 0);
    QVERIFY(dialog.statusLabel->text().contains("서로 다른"));
    dialog.authorEdit->setText("青木そら");
    dialog.titleEdit->setText("星降る夜の図書館");
    dialog.requestSearch(false);
    QCOMPARE(requests.count(), 1);
    QVERIFY(!requests.first().at(7).toBool()); // The neural publisher hint still needs review.
    result.suggestions.erase(std::remove_if(result.suggestions.begin(), result.suggestions.end(), [](const LocalMetadata::Suggestion &candidate) { return candidate.field == LocalMetadata::Suggestion::Publisher; }), result.suggestions.end());
    YACReaderArchiveInspectorDialog identitiesOnly;
    QSignalSpy directRequests(&identitiesOnly, &YACReaderArchiveInspectorDialog::titleSearchRequested);
    identitiesOnly.showResult(result);
    identitiesOnly.authorEdit->setText("青木そら");
    identitiesOnly.titleEdit->setText("星降る夜の図書館");
    identitiesOnly.requestSearch(false);
    QCOMPARE(directRequests.count(), 1);
    QVERIFY(directRequests.first().at(7).toBool()); // Explicit identity values alone can be queried.
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

// Explicit local diagnostics: no library database or external lookup.
int localOcrProbe(const QStringList &args)
{
    if (args.size() != 4 && args.size() != 5)
        return 2;
    int perEnd = 3;
    if (args.size() == 5) {
        bool valid = false;
        perEnd = args.at(4).toInt(&valid);
        if (args.at(1) != "--local-ocr-pages" || !valid || perEnd < 1 || perEnd > 6)
            return 2;
    }
    const QFileInfo input(args.at(2)), output(args.at(3));
    if (!input.exists() || output.exists() || !output.absoluteDir().exists())
        return 2;
    if (args.at(1) == "--local-ocr-pages") {
        const QString sourceRoot = input.isDir() ? input.canonicalFilePath() : input.absoluteDir().canonicalPath();
        const QString relative = QDir(sourceRoot).relativeFilePath(output.absoluteDir().canonicalPath());
        if (relative != ".." && !relative.startsWith("../") && !QDir::isAbsolutePath(relative))
            return 2;
        const auto result = LocalMetadata::readPages(input.canonicalFilePath(), perEnd, std::make_shared<std::atomic_bool>(false));
        if (!result.error.isEmpty() || result.pages.isEmpty() || !QDir().mkdir(output.absoluteFilePath()))
            return 3;
        QJsonArray pages;
        int index = 0;
        for (const auto &page : result.pages) {
            const auto image = LocalMetadata::prepareOcrImage(page.image, LocalMetadata::OcrOptions());
            const QString name = QString("page-%1.png").arg(index++);
            if (image.isNull() || !image.save(QDir(output.absoluteFilePath()).filePath(name)))
                return 3;
            pages.append(QJsonObject { { "number", page.number }, { "sourceEntry", page.name }, { "image", name }, { "size", QJsonArray { image.width(), image.height() } } });
        }
        QSaveFile destination(QDir(output.absoluteFilePath()).filePath("pages.json"));
        const auto bytes = QJsonDocument(QJsonObject { { "version", 1 }, { "pageCount", result.pageCount }, { "perEnd", perEnd }, { "pages", pages } }).toJson();
        return destination.open(QIODevice::WriteOnly) && destination.write(bytes) == bytes.size() && destination.commit() ? 0 : 3;
    }
    const bool filenameMode = args.at(1) == "--local-ocr-fallback";
    if ((args.at(1) != "--local-ocr-candidates" && !filenameMode) || !input.isFile())
        return 2;
    QFile manifest(input.absoluteFilePath());
    if (!manifest.open(QIODevice::ReadOnly) || manifest.size() > 1024 * 1024)
        return 2;
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(manifest.readAll(), &parseError);
    const auto entries = document.object().value("pages").toArray();
    if (parseError.error != QJsonParseError::NoError || document.object().value("version").toInt() != 1 || entries.isEmpty() || entries.size() > 12)
        return 2;
    QVector<LocalMetadata::Page> pages;
    for (const auto &entry : entries) {
        const auto object = entry.toObject();
        const auto dimensions = object.value("size").toArray();
        const int width = dimensions.size() == 2 ? dimensions.at(0).toInt() : 0;
        const int height = dimensions.size() == 2 ? dimensions.at(1).toInt() : 0;
        if (width <= 0 || height <= 0 || qint64(width) * height > 17000000 || object.value("number").toInt() < 1)
            return 2;
        QFile readingFile(object.value("result").toString());
        if (!readingFile.open(QIODevice::ReadOnly) || readingFile.size() > 4 * 1024 * 1024)
            return 2;
        LocalMetadata::Page page;
        page.number = object.value("number").toInt();
        page.reading = LocalMetadata::parseNeuralReading(readingFile.readAll(), QSize(width, height));
        if (!page.reading.error.isEmpty())
            return 3;
        page.text = page.reading.text;
        page.image = QImage(width, height, QImage::Format_Mono);
        page.image.fill(0);
        page.kind = LocalMetadata::classifyPage(page.text, page.number);
        pages.append(page);
    }
    const QString sourcePath = filenameMode ? document.object().value("sourcePath").toString() : QString();
    if (filenameMode && sourcePath.trimmed().isEmpty())
        return 2;
    const auto suggestions = LocalMetadata::suggest(pages, sourcePath, filenameMode ? document.object().value("libraryRoot").toString() : QString());
    bool pageCandidate = false;
    QJsonArray candidates;
    for (const auto &item : suggestions) {
        pageCandidate = pageCandidate || item.page > 0;
        QJsonArray evidence;
        for (const auto &source : item.evidence)
            evidence.append(QJsonObject { { "page", source.page }, { "labelled", source.labelled }, { "confidence", source.confidence } });
        candidates.append(QJsonObject { { "field", item.field == LocalMetadata::Suggestion::Title ? "title" : item.field == LocalMetadata::Suggestion::Author ? "author"
                                                                                                                                                              : "publisher" },
                                        { "value", item.value },
                                        { "labelled", item.labelled },
                                        { "reason", item.reason },
                                        { "evidence", evidence } });
    }
    QSaveFile destination(output.absoluteFilePath());
    const auto bytes = QJsonDocument(QJsonObject { { "version", 1 }, { "pathHintsUsed", filenameMode }, { "filenameFallback", filenameMode && !pageCandidate }, { "candidates", candidates } }).toJson();
    return destination.open(QIODevice::WriteOnly) && destination.write(bytes) == bytes.size() && destination.commit() ? 0 : 3;
}

// Offline cache round-trip for an explicitly supplied small manifest. It never
// opens original media, executes OCR, requests a catalog or writes a library DB.
int localOcrCacheProbe(const QStringList &args)
{
    if (args.size() != 4)
        return 2;
    const QFileInfo input(args[2]), output(args[3]);
    const QString cacheRoot = output.absoluteFilePath() + QStringLiteral(".cache");
    if (!input.isAbsolute() || !input.isFile() || !output.isAbsolute() || output.exists() || QFileInfo(cacheRoot).exists() || !output.dir().exists())
        return 2;
    QFile file(input.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
        return 2;
    const auto document = QJsonDocument::fromJson(file.readAll());
    const auto object = document.object();
    const auto pages = object.value("pages").toArray();
    if (!document.isObject() || object.value("version").toInt() != 1 || pages.isEmpty() || pages.size() > 6)
        return 2;
    struct Page {
        LocalOcrCache::Identity identity;
        QByteArray bytes;
    };
    QVector<Page> validated;
    for (const auto &value : pages) {
        const auto page = value.toObject();
        const auto identity = page.value("identity").toObject();
        LocalOcrCache::Identity i;
        i.imageSha256 = identity.value("imageSha256").toString();
        i.preparedSize = QSize(identity.value("width").toInt(), identity.value("height").toInt());
        i.preprocessingFingerprint = identity.value("preprocessingFingerprint").toString();
        i.workerSha256 = identity.value("workerSha256").toString();
        i.modelManifestSha256 = identity.value("modelManifestSha256").toString();
        i.packageManifestSha256 = identity.value("packageManifestSha256").toString();
        i.language = identity.value("language").toString();
        i.cpuThreads = identity.value("cpuThreads").toInt();
        i.requestedDevice = identity.value("requestedDevice").toString();
        i.actualDevice = identity.value("actualDevice").toString();
        if (LocalOcrCache::key(i).isEmpty() || !QFileInfo(page.value("result").toString()).isAbsolute())
            return 2;
        QFile result(page.value("result").toString());
        if (!result.open(QIODevice::ReadOnly) || result.size() > 4 * 1024 * 1024)
            return 2;
        const auto bytes = result.readAll();
        const auto reading = LocalMetadata::parseNeuralReading(bytes, i.preparedSize);
        if (!reading.error.isEmpty() || reading.device != i.actualDevice || reading.language != i.language)
            return 3;
        validated.append({ i, bytes });
    }
    if (!QDir().mkdir(cacheRoot))
        return 3;
    QJsonArray reports;
    QString error;
    for (const auto &page : validated) {
        if (!LocalOcrCache::save(cacheRoot, page.identity, page.bytes, &error))
            return 3;
        const auto cached = LocalOcrCache::load(cacheRoot, page.identity, &error);
        if (!cached || cached->rawResult != page.bytes || !cached->reading.reviewRequired || !LocalOcrCache::save(cacheRoot, page.identity, page.bytes, &error))
            return 3;
        reports.append(QJsonObject { { "key", LocalOcrCache::key(page.identity) },
                                     { "resultSha256", cached->resultSha256 },
                                     { "actualDevice", cached->reading.device },
                                     { "reviewRequired", cached->reading.reviewRequired },
                                     { "byteIdentical", true } });
    }
    const auto bytes = QJsonDocument(QJsonObject { { "version", 1 }, { "pages", reports }, { "ocrExecuted", false } }).toJson();
    QSaveFile destination(output.absoluteFilePath());
    return destination.open(QIODevice::WriteOnly) && destination.write(bytes) == bytes.size() && destination.commit() ? 0 : 3;
}

int main(int argc, char **argv)
{
    if (argc > 2 && QByteArray(argv[1]) == "page.png") {
        if (qEnvironmentVariable("YACREADER_FAKE_OCR_MODE") == "timeout")
            QThread::sleep(10);
        QTextStream(stdout) << QString::fromUtf8(tsvFor({ "Title: Test Book", "Author: Alice Example" }));
        return 0;
    }
#ifdef Q_OS_WIN
    // The offscreen plugin does not discover Windows system fonts itself.
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", qEnvironmentVariable("SystemRoot").toUtf8() + "/Fonts");
#endif
    QApplication app(argc, argv);
    if (app.arguments().value(1) == "--cache-writer")
        return cacheWriterProbe(app.arguments());
    if (app.arguments().value(1) == "--local-ocr-cache")
        return localOcrCacheProbe(app.arguments());
    if (app.arguments().value(1).startsWith("--local-ocr-"))
        return localOcrProbe(app.arguments());
    LocalMetadataTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "main.moc"
