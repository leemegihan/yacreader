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
    void croppedCreditOcr();
    void preprocessingPreservesSource();
    void regionCoordinatesAndCandidatePrefill();
    void spacedColophonAndPublisher();
    void dialogueAndLabelBoundaries();
    void tsvConfidenceAndLanguageSelection();
    void automaticQueryUsesOnlyStrongCredits();
    void realMultilingualColophon();
    void neuralResponseAndReview();
    void realNeuralOcr();
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
    options.language = "kor+eng";
    QImage image(QCoreApplication::applicationDirPath() + QStringLiteral("/ocr-colophon-kor.png"));
    QVERIFY(!image.isNull());
    const auto reading = LocalMetadata::recognizePage(image, options, std::make_shared<std::atomic_bool>(false));
    QVERIFY2(reading.error.isEmpty(), qPrintable(reading.error));
    QVERIFY(reading.reviewRequired);
    QString compact = reading.text;
    compact.remove(QRegularExpression(QStringLiteral("\\s+")));
    QVERIFY2(compact.contains(QStringLiteral("푸른하늘")), qPrintable(reading.text));
    QVERIFY2(compact.contains(QStringLiteral("홍길동")), qPrintable(reading.text));
    auto cancelled = std::make_shared<std::atomic_bool>(true);
    QVERIFY(LocalMetadata::recognizePage(image, options, cancelled).text.isEmpty());
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
        QTextStream(stdout) << QString::fromUtf8(tsvFor({ "Title: Test Book", "Author: Alice Example" }));
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
