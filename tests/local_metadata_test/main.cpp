#include "comic_db.h"
#include "comic_image_folder.h"
#include "initial_comic_info_extractor.h"
#include "library_creator.h"
#include "library_maintenance_lock.h"
#include "local_metadata.h"
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
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QThread>
#include <QUuid>

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
    void neuralResponseAndReview();
    void coverTitlesAndCombinedEvidence();
    void coverLayoutRejectsUnrelatedText();
    void nameHintsExcludeLibraryRoots();
    void neuralCandidatesOpenReviewBeforeSearch();
    void conflictingCandidatesRequireSelection();
    void creditSuffixAndCompoundLabels();
    void creditGeometryAndDialogue();
    void bulletSeparatedCredits();
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
    QVERIFY(requests.first().at(7).toBool()); // Explicit values can be queried.
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
    if (args.size() != 4)
        return 2;
    const QFileInfo input(args.at(2)), output(args.at(3));
    if (!input.exists() || output.exists() || !output.absoluteDir().exists())
        return 2;
    if (args.at(1) == "--local-ocr-pages") {
        const QString sourceRoot = input.isDir() ? input.canonicalFilePath() : input.absoluteDir().canonicalPath();
        const QString relative = QDir(sourceRoot).relativeFilePath(output.absoluteDir().canonicalPath());
        if (relative != ".." && !relative.startsWith("../") && !QDir::isAbsolutePath(relative))
            return 2;
        const auto result = LocalMetadata::readPages(input.canonicalFilePath(), 3, std::make_shared<std::atomic_bool>(false));
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
        const auto bytes = QJsonDocument(QJsonObject { { "version", 1 }, { "pageCount", result.pageCount }, { "pages", pages } }).toJson();
        return destination.open(QIODevice::WriteOnly) && destination.write(bytes) == bytes.size() && destination.commit() ? 0 : 3;
    }
    if (args.at(1) != "--local-ocr-candidates" || !input.isFile())
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
    QJsonArray candidates;
    for (const auto &item : LocalMetadata::suggest(pages, QString())) {
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
    const auto bytes = QJsonDocument(QJsonObject { { "version", 1 }, { "pathHintsUsed", false }, { "candidates", candidates } }).toJson();
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
    if (app.arguments().value(1).startsWith("--local-ocr-"))
        return localOcrProbe(app.arguments());
    LocalMetadataTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "main.moc"
