#include "comic_db.h"
#include "comic_image_folder.h"
#include "initial_comic_info_extractor.h"
#include "library_creator.h"
#include "library_maintenance_lock.h"
#include "local_metadata.h"
#include "local_ocr_cache.h"
#include "local_ocr_executor.h"
#include "local_ocr_library.h"
#include "local_ocr_persistence.h"
#include "local_ocr_preflight.h"
#include "local_ocr_process.h"
#include "local_ocr_recovery.h"
#include "local_ocr_runtime.h"
#include "local_ocr_session.h"
#include "local_ocr_source.h"
#include "ocr_job_store.h"
#include "ocr_page_view.h"
#include "yacreader_archive_inspector_dialog.h"
#include "yacreader_global.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLineF>
#include <QListWidget>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMap>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <thread>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
LocalOcrRuntime::Measurement persistenceSettings(const QString &root);
OcrJobs::Spec persistenceSpec(const QString &root, const LocalOcrRuntime::Measurement &settings);

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
    void inspectorPreferences();
    void isolatedLocalServerNames();
    void inspectorSessionNeural();
    void privateSelectedInspector();
    void sampling();
    void folderAndArchiveUseSamePageOrder();
    void sourceSnapshotValidation();
    void collectionFolderIsNotAComic();
    void labelledCandidatesAreNotTranslators();
    void neuralRecoveryWorkerFaults_data();
    void neuralRecoveryWorkerFaults();
    void recoveryRetainsCompletedPages();
    void recoveryPageSinkGuards();
    void recoveryFailureBoundaries_data();
    void recoveryFailureBoundaries();
    void recoveryCpuFailurePreservesEvidence();
    void recoveryLegacyRejectsSessionFailure();
    void recoveryCancellationFromProgress();
    void ocrProcessAndCancellation();
    void ocrReadinessSharing();
    void ocrProcessTreeCleanup_data();
    void ocrProcessTreeCleanup();
    void ocrResourceOwnership_data();
    void ocrResourceOwnership();
    void ocrResourceSameThread();
    void realOcr();
    void croppedCreditOcr();
    void preprocessingPreservesSource();
    void preparationGeometry_data();
    void preparationGeometry();
    void preparationEvidenceValidation();
    void runtimeSnapshotMeasuresFiles();
    void runtimeSnapshotDeployed();
    void cacheReceiptOrdering();
    void libraryBinding_data();
    void libraryBinding();
    void inspectorSavedReview_data();
    void inspectorSavedReview();
    void inspectorPreparationCancelled_data();
    void inspectorPreparationCancelled();
    void inspectorCompletedCancellation_data();
    void inspectorCompletedCancellation();
    void guardedMetadataSave_data();
    void guardedMetadataSave();
    void inspectorWorkerLifetime_data();
    void inspectorWorkerLifetime();
    void claimedExecutorNeural();
    void claimedExecutorWorker_data();
    void claimedExecutorWorker();
    void claimedExecutor_data();
    void claimedExecutor();
    void repeatedExecutorInputs_data();
    void repeatedExecutorInputs();
    void completedReview_data();
    void completedReview();
    void selectedSession_data();
    void selectedSession();
    void selectedPreflight_data();
    void selectedPreflight();
    void cacheReceiptRecovery();
    void cacheReceiptCrash();
    void cacheReceiptClockAfterContention();
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

void LocalMetadataTest::inspectorPreferences()
{
    const bool wasSet = qEnvironmentVariableIsSet("YACREADER_DATA_DIR");
    const auto previous = qEnvironmentVariable("YACREADER_DATA_DIR");
    const auto setRoot = [](const QString &value) {
#ifdef Q_OS_WIN
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
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(setRoot(temporary.filePath("profile-a")));
    const auto path = QDir(YACReader::getSettingsPath()).filePath("ocr-inspector.ini");
    {
        YACReaderArchiveInspectorDialog dialog;
        dialog.quality->setCurrentIndex(dialog.quality->findData(0));
        dialog.language->setCurrentIndex(dialog.language->findData("kor+eng"));
        dialog.performance->setCurrentIndex(dialog.performance->findData(16));
        dialog.pageLimit->setValue(2);
        dialog.autoSearch->setChecked(false);
        dialog.overwrite->setChecked(true);
        dialog.titleEdit->setText("A work-specific title");
        dialog.authorEdit->setText("A work-specific author");
        dialog.invert->setChecked(true);
        QVERIFY(!QFileInfo::exists(path)); // Programmatic initialization is not a user preference edit.
        QVERIFY(QMetaObject::invokeMethod(dialog.quality, "activated", Q_ARG(int, dialog.quality->currentIndex())));
    }
    QVERIFY(QFileInfo::exists(path));
    QSettings stored(path, QSettings::IniFormat);
    QCOMPARE(stored.allKeys().size(), 6);
    {
        YACReaderArchiveInspectorDialog reopened;
        QCOMPARE(reopened.quality->currentData().toInt(), 0);
        QCOMPARE(reopened.language->currentData().toString(), QString("kor+eng"));
        QCOMPARE(reopened.performance->currentData().toInt(), 16);
        QCOMPARE(reopened.pageLimit->value(), 2);
        QVERIFY(!reopened.autoSearch->isChecked());
        QVERIFY(!reopened.overwrite->isChecked());
        QVERIFY(!reopened.invert->isChecked());
        QVERIFY(reopened.titleEdit->text().isEmpty());
        QVERIFY(reopened.authorEdit->text().isEmpty());
        // Optional runtimes may be removed between launches; unavailable saved choices fall back.
        stored.setValue("quality", 2);
        stored.setValue("performance", -1);
        const int neural = reopened.quality->findData(2);
        if (neural >= 0)
            reopened.quality->removeItem(neural);
        const int gpu = reopened.performance->findData(-1);
        if (gpu >= 0)
            reopened.performance->removeItem(gpu);
        reopened.restorePreferences(stored);
        QCOMPARE(reopened.quality->currentData().toInt(), 1);
        QCOMPARE(reopened.performance->currentData().toInt(), 8);
        stored.setValue("language", "unsupported");
        stored.setValue("quality", 99);
        stored.setValue("performance", 64);
        stored.setValue("perEnd", 9);
        stored.setValue("autoSearch", "invalid");
        reopened.restorePreferences(stored);
        QCOMPARE(reopened.language->currentData().toString(), QString("auto"));
        QCOMPARE(reopened.quality->currentData().toInt(), 1);
        QCOMPARE(reopened.performance->currentData().toInt(), 8);
        QCOMPARE(reopened.pageLimit->value(), 3);
        QVERIFY(!reopened.autoSearch->isChecked());
        stored.setValue("quality", "invalid");
        reopened.restorePreferences(stored);
        QCOMPARE(reopened.quality->currentData().toInt(), 1);
    }
    QVERIFY(setRoot(temporary.filePath("profile-b")));
    YACReaderArchiveInspectorDialog separate;
    QCOMPARE(separate.quality->currentData().toInt(), 1);
    QCOMPARE(separate.language->currentData().toString(), QString("auto"));
    QCOMPARE(separate.performance->currentData().toInt(), 8);
    QCOMPARE(separate.pageLimit->value(), 3);
    QVERIFY(separate.autoSearch->isChecked());
    QVERIFY(!QFileInfo::exists(QDir(YACReader::getSettingsPath()).filePath("ocr-inspector.ini")));
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

void LocalMetadataTest::sourceSnapshotValidation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto folder = temporary.filePath("synthetic-pages");
    QVERIFY(QDir().mkdir(folder));
    QImage image(80, 40, QImage::Format_RGB32);
    image.fill(Qt::white);
    const auto encoded = [](const QImage &input) {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        if (buffer.open(QIODevice::WriteOnly))
            input.save(&buffer, "PNG");
        return bytes;
    };
    const auto png = encoded(image);
    QVERIFY(!png.isEmpty());
    QList<QPair<QString, QByteArray>> files;
    for (int i = 1; i <= 10; ++i) {
        const auto name = QStringLiteral("%1.png").arg(i);
        QFile file(QDir(folder).filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(png), png.size());
        files.append({ name, png });
    }
    const auto archive = temporary.filePath("synthetic.cbz");
    QVERIFY(writeZip(archive, files));
    QString error;
    const auto first = LocalOcrSource::read(folder, 3, { }, &error);
    QVERIFY2(first.has_value(), qPrintable(error));
    const auto repeated = LocalOcrSource::read(folder, 3, { }, &error);
    QVERIFY(repeated.has_value());
    QCOMPARE(first->fingerprint, repeated->fingerprint);
    QCOMPARE(first->source.pageNames.size(), 10);
    QCOMPARE(first->source.pages.size(), 6);
    QCOMPARE(first->source.pages[3].number, 8);
    const auto zipped = LocalOcrSource::read(archive, 3, { }, &error);
    QVERIFY2(zipped.has_value(), qPrintable(error));
    QCOMPARE(zipped->source.pageNames, first->source.pageNames);
    QCOMPARE(zipped->manifest["sourceKind"].toString(), QStringLiteral("archive"));
    QVERIFY(zipped->fingerprint != first->fingerprint); // Location and source kind belong to identity.
    for (int i = 0; i < 6; ++i)
        QCOMPARE(zipped->source.pages[i].sourceSha256, first->source.pages[i].sourceSha256);
    QVERIFY(!LocalOcrSource::read(folder, 4, { }, &error));
    QVERIFY(!error.isEmpty());
    auto cancel = std::make_shared<std::atomic_bool>(true);
    QVERIFY(!LocalOcrSource::read(folder, 3, cancel, &error));
    QVERIFY(!error.isEmpty());
    // Identical decoded pixels with different encoded metadata must invalidate
    // source identity even if their prepared OCR PNG could be reused.
    image.setText(QStringLiteral("Comment"), QStringLiteral("synthetic changed metadata"));
    const auto changedPng = encoded(image);
    QVERIFY(changedPng != png);
    QFile selected(QDir(folder).filePath("1.png"));
    QVERIFY(selected.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(selected.write(changedPng), changedPng.size());
    selected.close();
    const auto changed = LocalOcrSource::read(folder, 3, { }, &error);
    QVERIFY(changed.has_value());
    QVERIFY(changed->fingerprint != first->fingerprint);
    QCOMPARE(LocalMetadata::prepareOcrImage(changed->source.pages[0].image, { }), LocalMetadata::prepareOcrImage(first->source.pages[0].image, { }));
    QVERIFY(QFile::rename(QDir(folder).filePath("5.png"), QDir(folder).filePath("5-renamed.png")));
    const auto renamed = LocalOcrSource::read(folder, 3, { }, &error);
    QVERIFY(renamed.has_value());
    QVERIFY(renamed->fingerprint != changed->fingerprint); // Interior page names still affect the plan.
    files.append(files[0]);
    const auto duplicate = temporary.filePath("duplicate.cbz");
    QVERIFY(writeZip(duplicate, files));
    QVERIFY(!LocalOcrSource::read(duplicate, 3, { }, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(selected.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(selected.write("bad image"), qint64(9));
    selected.close();
    QVERIFY(!LocalOcrSource::read(folder, 3, { }, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!LocalOcrSource::read(temporary.path(), 3, { }, &error)); // Collection root.
    QVERIFY(!error.isEmpty());
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

namespace {
LocalMetadata::RecognitionBatch syntheticAttempt(int count, const QString &device)
{
    LocalMetadata::RecognitionBatch result;
    result.status = LocalMetadata::RecognitionStatus::Complete;
    result.gpuStarted = device == QStringLiteral("gpu:0");
    result.validPages.fill(true, count);
    for (int i = 0; i < count; ++i) {
        LocalMetadata::Reading reading;
        reading.device = device;
        reading.text = QStringLiteral("Synthetic page %1").arg(i);
        reading.reviewRequired = true;
        result.readings.append(reading);
    }
    return result;
}
QVector<QImage> recoveryImages()
{
    QVector<QImage> images;
    for (int i = 1; i <= 6; ++i) {
        QImage image(i, 1, QImage::Format_RGB32);
        image.fill(Qt::white);
        images.append(image);
    }
    return images;
}
}

void LocalMetadataTest::neuralRecoveryWorkerFaults_data()
{
    QTest::addColumn<QString>("mode");
    QTest::addColumn<QString>("action");
    for (const char *mode : { "partial-gap", "all-output-failure", "partial-cpu-failure", "cancel-after-page", "blank-partial", "invalid-partial" })
        QTest::newRow(mode) << QString::fromLatin1(mode) << QString();
    QTest::newRow("sink-reject-running-worker") << QStringLiteral("cancel-after-page") << QStringLiteral("reject");
    QTest::newRow("sink-exception-running-worker") << QStringLiteral("cancel-after-page") << QStringLiteral("throw");
    QTest::newRow("sink-cancel-running-worker") << QStringLiteral("cancel-after-page") << QStringLiteral("cancel");
    QTest::newRow("sink-reject-cpu-retry") << QStringLiteral("partial-gap") << QStringLiteral("reject-cpu");
    QTest::newRow("sink-reject-after-all-output") << QStringLiteral("all-output-failure") << QStringLiteral("reject");
    QTest::newRow("sink-reject-blank-page") << QStringLiteral("blank-partial") << QStringLiteral("reject");
}

void LocalMetadataTest::neuralRecoveryWorkerFaults()
{
    using namespace LocalMetadata;
    if (qEnvironmentVariable("YACREADER_SYNTHETIC_RECOVERY_WORKER") != QStringLiteral("1"))
        QSKIP("Explicit isolated synthetic worker failure injection only.");
    QFETCH(QString, mode);
    QFETCH(QString, action);
    QFile worker(QCoreApplication::applicationDirPath() + QStringLiteral("/ocr-neural/worker.py"));
    QVERIFY(worker.open(QIODevice::ReadOnly));
    QVERIFY(worker.readLine().startsWith("# YACReader synthetic recovery worker v1"));
    QVERIFY(QFile::exists(QCoreApplication::applicationDirPath() + QStringLiteral("/ocr-neural-gpu/runtime/python.exe")));
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto oldMode = qgetenv("YACREADER_FAULT_MODE");
    const auto oldTrace = qgetenv("YACREADER_FAULT_TRACE_BASE64");
    const auto restore = qScopeGuard([&] {
        if (oldMode.isNull())
            qunsetenv("YACREADER_FAULT_MODE");
        else
            qputenv("YACREADER_FAULT_MODE", oldMode);
        if (oldTrace.isNull())
            qunsetenv("YACREADER_FAULT_TRACE_BASE64");
        else
            qputenv("YACREADER_FAULT_TRACE_BASE64", oldTrace);
    });
    const QString tracePath = temporary.filePath(QStringLiteral("trace.jsonl"));
    QVERIFY(qputenv("YACREADER_FAULT_MODE", mode.toUtf8()));
    // qputenv uses the Windows narrow CRT environment. Carry Unicode paths
    // as ASCII bytes instead of passing UTF-8 to the active ANSI code page.
    QVERIFY(qputenv("YACREADER_FAULT_TRACE_BASE64", tracePath.toUtf8().toBase64()));
    auto images = recoveryImages();
    images.resize(2);
    OcrOptions options;
    options.neural = options.gpu = true;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    OcrJobs::Store store;
    QVERIFY(store.open(temporary.filePath("ocr-jobs.sqlite")));
    const auto settings = persistenceSettings(temporary.path());
    const auto jobId = store.enqueue(persistenceSpec(temporary.path(), settings));
    QVERIFY(jobId.has_value());
    const auto lease = store.claim(*jobId, QStringLiteral("synthetic-stream-owner"), 1000, 1000);
    QVERIFY(lease.has_value());
    const auto cache = temporary.filePath("cache");
    QVERIFY(QDir().mkdir(cache));
    QVector<int> persistedIndexes;
    QVector<NeuralPageEvidence> deliveries;
    bool deliveredBeforeCleanup = true;
    QElapsedTimer elapsed;
    elapsed.start();
    const auto result = recognizePagesWithOutcome(images, options, cancel, [&](int count, int, const QString &) {
                                                      if (action.isEmpty() && mode == QStringLiteral("cancel-after-page") && count > 0)
                                                          cancel->store(true); }, [&](const NeuralPageEvidence &page, QString *error) {
                                                      deliveries.append(page);
                                                      // The exact result file must still exist: this callback
                                                      // runs during collection, not after a batch returns.
                                                      QFile liveTrace(tracePath);
                                                      if (!liveTrace.open(QIODevice::ReadOnly))
                                                          deliveredBeforeCleanup = false;
                                                      QJsonObject liveCall;
                                                      while (!liveTrace.atEnd())
                                                          liveCall = QJsonDocument::fromJson(liveTrace.readLine()).object();
                                                      const int attemptIndex = page.requestedDevice == QStringLiteral("cpu") ? 0 : page.selectedIndex;
                                                      const auto outputs = liveCall["outputs"].toArray();
                                                      if (attemptIndex >= outputs.size() || !QFile::exists(outputs[attemptIndex].toString()))
                                                          deliveredBeforeCleanup = false;
                                                      if (action == QStringLiteral("throw"))
                                                          throw std::runtime_error("synthetic receiver exception");
                                                      if (action == QStringLiteral("reject") || (action == QStringLiteral("reject-cpu") && page.requestedDevice == QStringLiteral("cpu"))) {
                                                          *error = QStringLiteral("synthetic receipt rejection");
                                                          return false;
                                                      }
                                                      const auto saved = LocalOcrPersistence::recordPage(store, *lease, settings, page, cache, cancel, [] { return 1001; });
                                                      if (!saved.receiptSaved) {
                                                          *error = saved.error;
                                                          return false;
                                                      }
                                                      persistedIndexes.append(page.selectedIndex);
                                                      if (action == QStringLiteral("cancel"))
                                                          cancel->store(true);
                                                      return true; });
    QVERIFY(deliveredBeforeCleanup);
    if (mode == QStringLiteral("cancel-after-page"))
        QVERIFY(elapsed.elapsed() < 15000); // Worker intentionally sleeps for 30 seconds.

    QFile trace(tracePath);
    QVERIFY(trace.open(QIODevice::ReadOnly));
    QVector<QJsonObject> calls;
    while (!trace.atEnd()) {
        const auto line = trace.readLine().trimmed();
        if (!line.isEmpty())
            calls.append(QJsonDocument::fromJson(line).object());
    }
    const bool retry = (action.isEmpty() || action == QStringLiteral("reject-cpu")) && (mode == QStringLiteral("partial-gap") || mode == QStringLiteral("partial-cpu-failure") || mode == QStringLiteral("blank-partial") || mode == QStringLiteral("invalid-partial"));
    QCOMPARE(calls.size(), retry ? 2 : 1);
    QCOMPARE(calls[0]["device"].toString(), QStringLiteral("gpu:0"));
    const auto firstHashes = calls[0]["images"].toArray();
    QCOMPARE(firstHashes.size(), 2);
    QCOMPARE(result.readings.size(), 2);
    QVERIFY(result.gpuStarted);
    QCOMPARE(result.readings[1].device, QStringLiteral("gpu:0"));
    QCOMPARE(result.readings[1].text, mode == QStringLiteral("blank-partial") ? QString() : firstHashes[1].toString());
    QVERIFY(result.validPages[1]);
    if (retry) {
        QCOMPARE(calls[1]["device"].toString(), QStringLiteral("cpu"));
        const auto retriedHashes = calls[1]["images"].toArray();
        QCOMPARE(retriedHashes.size(), 1);
        QCOMPARE(retriedHashes[0], firstHashes[0]);
        QCOMPARE(result.readings[0].device, QStringLiteral("cpu"));
        QCOMPARE(result.readings[0].text, firstHashes[0].toString());
        QVERIFY(result.validPages[0]);
        QCOMPARE(result.attemptErrors.size(), 1);
    }
    QCOMPARE(result.evidence.size(), 2);
    for (int i = 0; i < 2; ++i) {
        QCOMPARE(result.evidence[i].has_value(), result.validPages[i]);
        if (!result.evidence[i])
            continue;
        const auto &evidence = *result.evidence[i];
        QCOMPARE(evidence.selectedIndex, i);
        QVERIFY(validNeuralEvidence(evidence));
        QCOMPARE(evidence.imageSha256, firstHashes[i].toString());
        QCOMPARE(evidence.actualDevice, result.readings[i].device);
        const int callIndex = retry && i == 0 ? 1 : 0;
        const int attemptIndex = retry && i == 0 ? 0 : i;
        const auto call = calls[callIndex];
        QCOMPARE(evidence.requestedDevice, call["device"].toString());
        const auto exact = QByteArray::fromBase64(call["responses"].toObject()[QString::number(attemptIndex)].toString().toLatin1());
        QCOMPARE(evidence.rawResult, exact);
        QVERIFY(!QFile::exists(call["outputs"].toArray()[attemptIndex].toString())); // Temp directory is gone.
    }
    const auto persistedJob = store.get(*jobId);
    QVERIFY(persistedJob.has_value());
    QCOMPARE(persistedJob->state, OcrJobs::State::Running); // Receipts never finish a batch.
    QCOMPARE(persistedJob->pages.size(), persistedIndexes.size());
    for (const int index : persistedIndexes) {
        QVERIFY(result.evidence[index].has_value());
        const auto &page = *result.evidence[index];
        QString error;
        const auto recovered = LocalOcrPersistence::restorePage(*persistedJob, settings, index, page.geometry, page.imageSha256, cache, &error);
        QVERIFY2(recovered.has_value(), qPrintable(error));
        QCOMPARE(recovered->rawResult, page.rawResult);
        QCOMPARE(recovered->requestedDevice, page.requestedDevice);
    }
    QVector<int> deliveredIndexes;
    for (const auto &page : deliveries) {
        deliveredIndexes.append(page.selectedIndex);
        QVERIFY(result.evidence[page.selectedIndex].has_value());
        QCOMPARE(page.rawResult, result.evidence[page.selectedIndex]->rawResult);
        QCOMPARE(page.imageSha256, result.evidence[page.selectedIndex]->imageSha256);
        QCOMPARE(page.requestedDevice, result.evidence[page.selectedIndex]->requestedDevice);
    }
    if (mode == QStringLiteral("all-output-failure"))
        QCOMPARE(deliveredIndexes, action.isEmpty() ? QVector<int>({ 0, 1 }) : QVector<int>({ 0 }));
    else
        QCOMPARE(deliveredIndexes, retry ? QVector<int>({ 1, 0 }) : QVector<int>({ 1 }));
    auto expectedPersisted = deliveredIndexes;
    if (action == QStringLiteral("reject") || action == QStringLiteral("throw") || action == QStringLiteral("reject-cpu"))
        expectedPersisted.removeLast();
    QCOMPARE(persistedIndexes, expectedPersisted);
    if (!action.isEmpty()) {
        QCOMPARE(result.status, action == QStringLiteral("cancel") ? RecognitionStatus::Cancelled : RecognitionStatus::DeliveryFailed);
        QVERIFY(!result.error.isEmpty());
    } else if (mode == QStringLiteral("partial-gap") || mode == QStringLiteral("blank-partial") || mode == QStringLiteral("invalid-partial")) {
        QCOMPARE(result.status, RecognitionStatus::Complete);
        QVERIFY(result.error.isEmpty());
    } else {
        QCOMPARE(result.status, mode == QStringLiteral("cancel-after-page") ? RecognitionStatus::Cancelled : RecognitionStatus::Failed);
        QVERIFY(!result.error.isEmpty());
        if (mode == QStringLiteral("all-output-failure"))
            QVERIFY(result.validPages[0]);
    }
}

void LocalMetadataTest::recoveryRetainsCompletedPages()
{
    using namespace LocalMetadata;
    const auto images = recoveryImages();
    auto gpu = syntheticAttempt(6, QStringLiteral("gpu:0"));
    gpu.status = RecognitionStatus::Failed;
    gpu.error = QStringLiteral("synthetic worker failed");
    gpu.validPages[1] = gpu.validPages[4] = false;
    gpu.readings[1].error = QStringLiteral("malformed page");
    gpu.readings[4] = { };
    gpu.readings[2].text.clear(); // Successful blank page is not missing.
    gpu.readings[0].elapsedMs = 123;
    auto cpu = syntheticAttempt(2, QStringLiteral("cpu"));
    cpu.readings[0].text = QStringLiteral("recovered second");
    cpu.readings[1].text = QStringLiteral("recovered fifth");
    OcrOptions options;
    options.neural = options.gpu = true;
    options.cpuThreads = 4;
    options.rotation = 90;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    const auto evidenceFor = [&](int index, const QImage &image, const Reading &reading, const QString &device) {
        const auto prepared = prepareOcrPage(image, options);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        prepared.image.save(&buffer, "PNG");
        QJsonArray lines;
        if (!reading.text.isEmpty())
            lines.append(QJsonObject { { "text", reading.text }, { "confidence", 99 }, { "box", QJsonArray { 1, 1, 2, 2 } }, { "language", "jpn" } });
        const auto raw = QJsonDocument(QJsonObject { { "version", 1 }, { "engine", "paddle-regions" }, { "device", device }, { "language", "auto" }, { "lines", lines } }).toJson();
        const auto hash = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
        return NeuralPageEvidence { index, prepared.geometry, hash(png), raw, hash(raw), device, device };
    };
    gpu.evidence.resize(6);
    for (int i : { 0, 2, 3, 5 })
        gpu.evidence[i] = evidenceFor(i, images[i], gpu.readings[i], QStringLiteral("gpu:0"));
    cpu.evidence = { evidenceFor(0, images[1], cpu.readings[0], QStringLiteral("cpu")), evidenceFor(1, images[4], cpu.readings[1], QStringLiteral("cpu")) };
    int calls = 0;
    QVector<int> retriedWidths, progressCounts, deliveredIndexes;
    bool allAccepted = true;
    QVector<OcrOptions> receivedOptions;
    auto result = LocalOcrRecovery::recognize(images, options, cancel, [&](int complete, int total, const QString &) {
                progressCounts.append(complete);
                QCOMPARE(total, 6); }, [&](const QVector<QImage> &input, const OcrOptions &received, const Cancellation &, const Progress &progress, const PageSink &sink) {
                receivedOptions.append(received);
                const auto &outcome = ++calls == 1 ? gpu : cpu;
                for (const auto &page : outcome.evidence) {
                    if (page) {
                        QString error;
                        allAccepted = sink(*page, &error) && allAccepted;
                    }
                }
                if (calls == 1)
                    return gpu;
                for (const auto &image : input)
                    retriedWidths.append(image.width());
                progress(1, 2, QStringLiteral("synthetic retry"));
                progress(2, 2, QStringLiteral("synthetic retry done"));
                return cpu; }, [&](const NeuralPageEvidence &page, QString *) {
                    deliveredIndexes.append(page.selectedIndex);
                    return true; });
    QVERIFY(allAccepted);
    QCOMPARE(deliveredIndexes, QVector<int>({ 0, 2, 3, 5, 1, 4 }));
    QCOMPARE(calls, 2);
    QCOMPARE(retriedWidths, QVector<int>({ 2, 5 }));
    QCOMPARE(receivedOptions.size(), 2);
    QVERIFY(receivedOptions.first().gpu);
    QVERIFY(!receivedOptions.last().gpu);
    QCOMPARE(receivedOptions.last().cpuThreads, 4);
    QCOMPARE(receivedOptions.last().rotation, 90);
    QCOMPARE(progressCounts, QVector<int>({ 4, 5, 6 }));
    QCOMPARE(result.status, RecognitionStatus::Complete);
    QVERIFY(result.error.isEmpty());
    QCOMPARE(result.attemptErrors, QStringList { gpu.error });
    for (int i : { 0, 2, 3, 5 }) {
        QCOMPARE(result.readings.at(i).text, gpu.readings.at(i).text);
        QCOMPARE(result.readings.at(i).device, QStringLiteral("gpu:0"));
        QVERIFY(result.evidence[i].has_value());
        QCOMPARE(result.evidence[i]->rawResult, gpu.evidence[i]->rawResult);
        QCOMPARE(result.evidence[i]->imageSha256, gpu.evidence[i]->imageSha256);
        QCOMPARE(result.evidence[i]->selectedIndex, i);
        QVERIFY(result.readings.at(i).warning.isEmpty());
        QVERIFY(result.validPages.at(i));
    }
    QCOMPARE(result.readings[0].elapsedMs, 123);
    QCOMPARE(result.readings[1].text, cpu.readings[0].text);
    QCOMPARE(result.readings[4].text, cpu.readings[1].text);
    QCOMPARE(result.readings[4].device, QStringLiteral("cpu"));
    QVERIFY(!result.readings[4].warning.isEmpty());
    for (int i = 0; i < 2; ++i) {
        const int original = i == 0 ? 1 : 4;
        QVERIFY(result.evidence[original].has_value());
        QCOMPARE(result.evidence[original]->selectedIndex, original);
        QCOMPARE(result.evidence[original]->rawResult, cpu.evidence[i]->rawResult);
        QCOMPARE(result.evidence[original]->imageSha256, cpu.evidence[i]->imageSha256);
        QCOMPARE(result.evidence[original]->requestedDevice, QStringLiteral("cpu"));
    }
}

void LocalMetadataTest::recoveryPageSinkGuards()
{
    using namespace LocalMetadata;
    OcrOptions options;
    options.neural = options.gpu = true;
    const auto prepared = prepareOcrPage(sampleImage(), options);
    QByteArray png;
    QBuffer buffer(&png);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(prepared.image.save(&buffer, "PNG"));
    const auto hash = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
    const QByteArray raw = R"({"version":1,"engine":"paddle-regions","device":"gpu:0","language":"auto","lines":[]})";
    const NeuralPageEvidence valid { 0, prepared.geometry, hash(png), raw, hash(raw), QStringLiteral("gpu:0"), QStringLiteral("gpu:0") };
    QVERIFY(validNeuralEvidence(valid));
    for (const QString mode : { "duplicate", "bad-index", "bad-hash", "cpu-device", "cancel-reject", "cleanup" }) {
        auto page = valid;
        auto selected = options;
        if (mode == QStringLiteral("bad-index"))
            page.selectedIndex = 1;
        if (mode == QStringLiteral("bad-hash"))
            page.resultSha256 = QString(64, u'0');
        if (mode == QStringLiteral("cpu-device"))
            selected.gpu = false;
        auto cancel = std::make_shared<std::atomic_bool>(false);
        int attempts = 0, callbacks = 0;
        bool firstAccepted = false, secondAccepted = true;
        const auto result = LocalOcrRecovery::recognize({ sampleImage() }, selected, cancel, { }, [&](const QVector<QImage> &, const OcrOptions &, const Cancellation &, const Progress &, const PageSink &sink) {
                    ++attempts;
                    QString error;
                    firstAccepted = sink(page, &error);
                    if (mode == QStringLiteral("duplicate"))
                        secondAccepted = sink(page, &error);
                    auto batch = syntheticAttempt(1, QStringLiteral("gpu:0"));
                    batch.status = mode == QStringLiteral("cleanup") ? RecognitionStatus::CleanupFailed : RecognitionStatus::Failed;
                    batch.error = QStringLiteral("synthetic execution failure");
                    batch.validPages[0] = false; // Would authorize a retry if delivery failure were lost.
                    return batch; }, [&](const NeuralPageEvidence &, QString *error) {
                    ++callbacks;
                    if (mode == QStringLiteral("cancel-reject"))
                        cancel->store(true);
                    if (mode == QStringLiteral("cancel-reject") || mode == QStringLiteral("cleanup")) {
                        *error = QStringLiteral("synthetic receipt rejection");
                        return false;
                    }
                    return true; });
        QCOMPARE(attempts, 1);
        QCOMPARE(result.status, mode == QStringLiteral("cleanup") ? RecognitionStatus::CleanupFailed : RecognitionStatus::DeliveryFailed);
        QCOMPARE(callbacks, mode == QStringLiteral("bad-index") || mode == QStringLiteral("bad-hash") || mode == QStringLiteral("cpu-device") ? 0 : 1);
        QCOMPARE(firstAccepted, mode == QStringLiteral("duplicate"));
        if (mode == QStringLiteral("duplicate"))
            QVERIFY(!secondAccepted);
        QVERIFY(!result.error.isEmpty());
    }
}

void LocalMetadataTest::recoveryFailureBoundaries_data()
{
    QTest::addColumn<int>("status");
    QTest::addColumn<bool>("gpuStarted");
    QTest::addColumn<bool>("allValid");
    QTest::newRow("exit-failed-after-all-output") << int(LocalMetadata::RecognitionStatus::Failed) << true << true;
    QTest::newRow("cancelled-with-partial-output") << int(LocalMetadata::RecognitionStatus::Cancelled) << true << false;
    QTest::newRow("delivery-failed") << int(LocalMetadata::RecognitionStatus::DeliveryFailed) << true << false;
    QTest::newRow("cleanup-failed") << int(LocalMetadata::RecognitionStatus::CleanupFailed) << true << false;
    QTest::newRow("cpu-failure-no-recursion") << int(LocalMetadata::RecognitionStatus::Failed) << false << false;
    QTest::newRow("normal-success") << int(LocalMetadata::RecognitionStatus::Complete) << true << true;
}

void LocalMetadataTest::recoveryFailureBoundaries()
{
    using namespace LocalMetadata;
    QFETCH(int, status);
    QFETCH(bool, gpuStarted);
    QFETCH(bool, allValid);
    auto first = syntheticAttempt(6, gpuStarted ? QStringLiteral("gpu:0") : QStringLiteral("cpu"));
    first.status = RecognitionStatus(status);
    first.gpuStarted = gpuStarted;
    if (first.status != RecognitionStatus::Complete)
        first.error = QStringLiteral("synthetic failure");
    first.validPages[4] = allValid;
    int calls = 0;
    const auto result = LocalOcrRecovery::recognize(recoveryImages(), { }, { }, { },
                                                    [&](const QVector<QImage> &, const OcrOptions &, const Cancellation &, const Progress &, const PageSink &) {
                                                        ++calls;
                                                        return first;
                                                    });
    QCOMPARE(calls, 1);
    QCOMPARE(result.status, first.status);
    QCOMPARE(result.error, first.error);
    QCOMPARE(result.readings[0].text, first.readings[0].text);
    QCOMPARE(result.validPages, first.validPages);
}

void LocalMetadataTest::recoveryCpuFailurePreservesEvidence()
{
    using namespace LocalMetadata;
    for (const bool unexpectedGpu : { false, true }) {
        auto first = syntheticAttempt(6, QStringLiteral("gpu:0"));
        first.status = RecognitionStatus::Failed;
        first.error = QStringLiteral("synthetic GPU failure");
        first.validPages[1] = first.validPages[4] = false;
        auto second = syntheticAttempt(2, unexpectedGpu ? QStringLiteral("gpu:0") : QStringLiteral("cpu"));
        second.gpuStarted = false;
        if (!unexpectedGpu) {
            second.status = RecognitionStatus::Failed;
            second.error = QStringLiteral("synthetic CPU failure after output");
        }
        int calls = 0;
        auto result = LocalOcrRecovery::recognize(recoveryImages(), { }, { }, { },
                                                  [&](const QVector<QImage> &, const OcrOptions &, const Cancellation &, const Progress &, const PageSink &) {
                                                      return ++calls == 1 ? first : second;
                                                  });
        QCOMPARE(calls, 2);
        QCOMPARE(result.status, RecognitionStatus::Failed);
        QVERIFY(!result.error.isEmpty());
        QCOMPARE(result.readings[0].text, first.readings[0].text);
        QCOMPARE(result.readings[0].device, QStringLiteral("gpu:0"));
        QCOMPARE(result.validPages[1], !unexpectedGpu);
        QCOMPARE(result.validPages[4], !unexpectedGpu);
        QCOMPARE(result.readings[1].text, second.readings[0].text);
    }
}

void LocalMetadataTest::recoveryLegacyRejectsSessionFailure()
{
    using namespace LocalMetadata;
    auto batch = syntheticAttempt(2, QStringLiteral("gpu:0"));
    batch.status = RecognitionStatus::Failed;
    batch.error = QStringLiteral("synthetic failure after all output");
    batch.readings[1].text.clear();
    auto result = LocalOcrRecovery::legacyReadings(batch);
    QCOMPARE(result[0].text, batch.readings[0].text);
    QVERIFY(result[1].text.isEmpty());
    for (const auto &reading : result) {
        QCOMPARE(reading.error, batch.error);
        QCOMPARE(reading.device, QStringLiteral("gpu:0"));
    }
    auto unsupported = recognizePagesWithOutcome({ sampleImage() }, { }, { }, { }, [](const NeuralPageEvidence &, QString *) { return true; });
    QCOMPARE(unsupported.status, RecognitionStatus::DeliveryFailed);
    QVERIFY(!unsupported.validPages[0]);
    // The outcome API retains valid data independent of the compatibility view.
    QVERIFY(batch.readings[0].error.isEmpty());
    QVERIFY(batch.validPages[1]);
}

void LocalMetadataTest::recoveryCancellationFromProgress()
{
    using namespace LocalMetadata;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    auto first = syntheticAttempt(6, QStringLiteral("gpu:0"));
    first.status = RecognitionStatus::Failed;
    first.error = QStringLiteral("synthetic failure");
    first.validPages[4] = false;
    int calls = 0;
    auto result = LocalOcrRecovery::recognize(recoveryImages(), { }, cancel, [&](int, int, const QString &) { cancel->store(true); }, [&](const QVector<QImage> &, const OcrOptions &, const Cancellation &, const Progress &, const PageSink &) {
                ++calls;
                return first; });
    QCOMPARE(calls, 1);
    QCOMPARE(result.status, RecognitionStatus::Cancelled);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(result.validPages[0]);
    QCOMPARE(result.readings[0].text, first.readings[0].text);
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

namespace {
// Existence alone is not a readable-file handshake: publication can become
// visible while the producer or another process still holds the file open.
QByteArray processTreeReady(QProcess &controller, const QString &path, QString *error)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000) {
        QFile ready(path);
        if (ready.open(QIODevice::ReadOnly)) {
            const auto bytes = ready.readAll();
            if (ready.error() == QFileDevice::NoError) {
                error->clear();
                return bytes;
            }
        }
        *error = ready.errorString();
        if (controller.state() == QProcess::NotRunning) {
            *error = QStringLiteral("Controller exited (%1): %2; %3")
                             .arg(controller.exitCode())
                             .arg(*error, QString::fromUtf8(controller.readAllStandardError()));
            return { };
        }
        QTest::qWait(10);
    }
    *error = QStringLiteral("Timed out reading worker readiness: %1").arg(*error);
    return { };
}
int processTreeHelper(const QStringList &args)
{
    if (args.value(1) == "--ocr-tree-exit")
        return 0;
    if (args.value(1) == "--ocr-tree-leaf") {
        QThread::sleep(60);
        return 0;
    }
    const QString root = args.value(2);
    const QString mode = args.value(3);
    if (args.value(1) == "--ocr-tree-worker") {
        QProcess leaf;
        leaf.setStandardOutputFile(QProcess::nullDevice());
        leaf.setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
        leaf.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) { a->flags |= CREATE_NO_WINDOW; });
#endif
        leaf.start(QCoreApplication::applicationFilePath(), { "--ocr-tree-leaf" });
        if (!leaf.waitForStarted(5000))
            return 41;
        QSaveFile ready(root + "/ready.json");
        const auto bytes = QJsonDocument(QJsonObject {
                                                 { "worker", QCoreApplication::applicationPid() }, { "leaf", leaf.processId() } })
                                   .toJson();
        if (!ready.open(QIODevice::WriteOnly) || ready.write(bytes) != bytes.size() || !ready.commit())
            return 42;
        if (mode == "worker-exit")
            std::_Exit(0);
        QThread::sleep(60);
        return 0;
    }
    LocalOcrProcess worker;
    QString error;
    if (!worker.startOcr(QCoreApplication::applicationFilePath(), { "--ocr-tree-worker", root, mode }, &error, args.value(4)))
        return 43;
    if (mode == "worker-exit" && !worker.waitForFinished(5000))
        return 46;
    QElapsedTimer timer;
    timer.start();
    while (!QFileInfo::exists(root + "/act")) {
        if (timer.elapsed() > 10000)
            return 44;
        QThread::msleep(10);
    }
    if (mode == "owner-crash")
        std::_Exit(73);
    if (mode == "scope")
        return 0;
    return worker.finishTree(&error) ? 0 : 45;
}
}

void LocalMetadataTest::ocrReadinessSharing()
{
#ifndef Q_OS_WIN
    QSKIP("Windows file sharing readiness regression");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QProcess controller;
    controller.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) { a->flags |= CREATE_NO_WINDOW; });
    controller.start(QCoreApplication::applicationFilePath(), { "--ocr-tree-leaf" });
    QVERIFY(controller.waitForStarted(5000));
    auto stop = qScopeGuard([&] { controller.kill(); controller.waitForFinished(5000); });
    const auto path = directory.filePath("ready.json");
    HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(file != INVALID_HANDLE_VALUE);
    auto close = qScopeGuard([&] { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); });
    const QByteArray expected("{\"ready\":true}");
    DWORD written = 0;
    QVERIFY(WriteFile(file, expected.constData(), DWORD(expected.size()), &written, nullptr));
    QCOMPARE(written, DWORD(expected.size()));
    QVERIFY(QFileInfo::exists(path));
    QFile immediate(path);
    QVERIFY(!immediate.open(QIODevice::ReadOnly)); // Deterministically recreate the local failure class.
    std::thread release([owned = file] { QThread::msleep(100); CloseHandle(owned); });
    file = INVALID_HANDLE_VALUE;
    auto join = qScopeGuard([&] { release.join(); });
    QString error;
    const auto bytes = processTreeReady(controller, path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(bytes, expected);
#endif
}

void LocalMetadataTest::ocrProcessTreeCleanup_data()
{
    QTest::addColumn<QString>("mode");
    for (const auto &mode : { "stop", "scope", "worker-exit", "owner-crash" })
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

void LocalMetadataTest::ocrProcessTreeCleanup()
{
#ifndef Q_OS_WIN
    QSKIP("Windows process-tree ownership test");
#else
    QFETCH(QString, mode);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    // Another process we own must survive this job's cleanup.
    QProcess unrelated;
    unrelated.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) { a->flags |= CREATE_NO_WINDOW; });
    unrelated.start(QCoreApplication::applicationFilePath(), { "--ocr-tree-leaf" });
    QVERIFY(unrelated.waitForStarted(5000));
    auto unrelatedCleanup = qScopeGuard([&] { unrelated.kill(); unrelated.waitForFinished(5000); });
    QProcess controller;
    controller.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) { a->flags |= CREATE_NO_WINDOW; });
    controller.start(QCoreApplication::applicationFilePath(), { "--ocr-tree-controller", directory.path(), mode });
    QVERIFY(controller.waitForStarted(5000));
    auto cleanup = qScopeGuard([&] {
        if (controller.state() != QProcess::NotRunning) {
            controller.kill();
            controller.waitForFinished(5000);
        }
    });
    QString readinessError;
    const auto readyBytes = processTreeReady(controller, directory.filePath("ready.json"), &readinessError);
    QVERIFY2(readinessError.isEmpty(), qPrintable(readinessError));
    const auto object = QJsonDocument::fromJson(readyBytes).object();
    QVERIFY(object["worker"].toInteger() > 0 && object["leaf"].toInteger() > 0);
    HANDLE leaf = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, object.value("leaf").toInteger());
    QVERIFY(leaf != nullptr);
    auto closeLeaf = qScopeGuard([&] { CloseHandle(leaf); });
    QCOMPARE(WaitForSingleObject(leaf, 0), DWORD(WAIT_TIMEOUT));
    HANDLE worker = nullptr;
    auto closeWorker = qScopeGuard([&] { if (worker) CloseHandle(worker); });
    if (mode != "worker-exit") {
        worker = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, object.value("worker").toInteger());
        QVERIFY(worker != nullptr);
        QCOMPARE(WaitForSingleObject(worker, 0), DWORD(WAIT_TIMEOUT));
    }
    QFile action(directory.filePath("act"));
    QVERIFY(action.open(QIODevice::WriteOnly));
    action.close();
    QVERIFY(controller.waitForFinished(10000));
    QCOMPARE(controller.exitCode(), mode == "owner-crash" ? 73 : 0);
    QCOMPARE(WaitForSingleObject(leaf, 5000), DWORD(WAIT_OBJECT_0));
    if (worker)
        QCOMPARE(WaitForSingleObject(worker, 5000), DWORD(WAIT_OBJECT_0));
    QVERIFY(!unrelated.waitForFinished(50));
    QCOMPARE(unrelated.state(), QProcess::Running);
#endif
}

void LocalMetadataTest::ocrResourceOwnership_data()
{
    QTest::addColumn<QString>("mode");
    QTest::newRow("normal-handoff") << QStringLiteral("stop");
    QTest::newRow("abandoned-live-descendants") << QStringLiteral("owner-crash");
}

void LocalMetadataTest::ocrResourceOwnership()
{
#ifndef Q_OS_WIN
    QSKIP("Windows named OCR resource ownership test");
#else
    QFETCH(QString, mode);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto key = QStringLiteral("test-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto name = QStringLiteral("Local\\YACReader.OCR.v1.") + key;
    QProcess controller;
    controller.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) { a->flags |= CREATE_NO_WINDOW; });
    controller.start(QCoreApplication::applicationFilePath(), { "--ocr-tree-controller", directory.path(), mode, key });
    QVERIFY(controller.waitForStarted(5000));
    auto stopController = qScopeGuard([&] { if (controller.state() != QProcess::NotRunning) { controller.kill(); controller.waitForFinished(5000); } });
    QElapsedTimer timer;
    QString readinessError;
    const auto readyBytes = processTreeReady(controller, directory.filePath("ready.json"), &readinessError);
    QVERIFY2(readinessError.isEmpty(), qPrintable(readinessError));
    const auto record = QJsonDocument::fromJson(readyBytes).object();
    QVERIFY(record["worker"].toInteger() > 0 && record["leaf"].toInteger() > 0);
    HANDLE leaf = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, record["leaf"].toInteger());
    QVERIFY(leaf != nullptr);
    auto closeLeaf = qScopeGuard([&] { CloseHandle(leaf); });
    HANDLE worker = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, record["worker"].toInteger());
    QVERIFY(worker != nullptr);
    auto closeWorker = qScopeGuard([&] { CloseHandle(worker); });
    // Keep kernel objects alive across owner exit. In the crash case this
    // intentionally prevents kill-on-last-close from doing the recovery for us.
    const auto jobName = name + QStringLiteral(".job");
    HANDLE job = OpenJobObjectW(JOB_OBJECT_QUERY, FALSE, reinterpret_cast<LPCWSTR>(jobName.utf16()));
    QVERIFY(job != nullptr);
    auto closeJob = qScopeGuard([&] { CloseHandle(job); });
    const auto mutexName = name + QStringLiteral(".mutex");
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, reinterpret_cast<LPCWSTR>(mutexName.utf16()));
    QVERIFY(mutex != nullptr);
    auto closeMutex = qScopeGuard([&] { CloseHandle(mutex); });
    {
        LocalOcrProcess contender;
        auto cancel = std::make_shared<std::atomic_bool>(false);
        std::thread cancellation([cancel] { QThread::msleep(100); cancel->store(true); });
        auto join = qScopeGuard([&] { cancellation.join(); });
        QString error;
        timer.restart();
        QVERIFY(!contender.startOcr(QCoreApplication::applicationFilePath(), { "--ocr-tree-leaf" }, &error, key, cancel));
        QVERIFY(!error.isEmpty());
        QVERIFY(timer.elapsed() < 2000);
    }
    QCOMPARE(WaitForSingleObject(leaf, 0), DWORD(WAIT_TIMEOUT));
    QCOMPARE(WaitForSingleObject(worker, 0), DWORD(WAIT_TIMEOUT));
    QFile act(directory.filePath("act"));
    QVERIFY(act.open(QIODevice::WriteOnly));
    act.close();
    QVERIFY(controller.waitForFinished(10000));
    QCOMPARE(controller.exitCode(), mode == QStringLiteral("owner-crash") ? 73 : 0);
    if (mode == QStringLiteral("owner-crash")) {
        QCOMPARE(WaitForSingleObject(leaf, 0), DWORD(WAIT_TIMEOUT));
        QCOMPARE(WaitForSingleObject(worker, 0), DWORD(WAIT_TIMEOUT));
    }
    LocalOcrProcess successor;
    QString error;
    QVERIFY2(successor.startOcr(QCoreApplication::applicationFilePath(), { "--ocr-tree-leaf" }, &error, key), qPrintable(error));
    // Both old handles must already be signalled at successful handoff.
    QCOMPARE(WaitForSingleObject(leaf, 0), DWORD(WAIT_OBJECT_0));
    QCOMPARE(WaitForSingleObject(worker, 0), DWORD(WAIT_OBJECT_0));
    QVERIFY(!successor.waitForFinished(50));
    QVERIFY2(successor.finishTree(&error), qPrintable(error));
#endif
}

void LocalMetadataTest::ocrResourceSameThread()
{
#ifndef Q_OS_WIN
    QSKIP("Windows recursive-mutex resource guard test");
#else
    const auto key = QStringLiteral("test-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    LocalOcrProcess first;
    QString error;
    QVERIFY2(first.startOcr(QCoreApplication::applicationFilePath(), { "--ocr-tree-leaf" }, &error, key), qPrintable(error));
    {
        LocalOcrProcess contender;
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(!contender.startOcr(QCoreApplication::applicationFilePath(), { "--ocr-tree-leaf" }, &error, key));
        QVERIFY(!error.isEmpty());
        QVERIFY(elapsed.elapsed() < 1000);
    }
    QVERIFY(!first.waitForFinished(50)); // Failed reentrant contender cannot kill it.
    QVERIFY(first.finishTree(&error));
    LocalOcrProcess successor;
    QVERIFY2(successor.startOcr(QCoreApplication::applicationFilePath(), { "--ocr-tree-leaf" }, &error, key), qPrintable(error));
    QVERIFY(first.finishTree(&error)); // Old object must not act on the reused named job.
    QVERIFY(!successor.waitForFinished(50));
    QVERIFY(successor.finishTree(&error));
    LocalOcrProcess invalid;
    QVERIFY(!invalid.startOcr(QCoreApplication::applicationFilePath(), { "--ocr-tree-leaf" }, &error, QStringLiteral("bad/key")));
    QCOMPARE(invalid.state(), QProcess::NotRunning);
    LocalOcrProcess completed;
    QVERIFY(completed.startOcr(QCoreApplication::applicationFilePath(), { "--ocr-tree-exit" }, &error, key));
    QVERIFY(completed.waitForFinished(5000));
    {
        LocalOcrProcess premature;
        QVERIFY(!premature.startOcr(QCoreApplication::applicationFilePath(), { "--ocr-tree-leaf" }, &error, key));
    }
    QVERIFY(completed.finishTree(&error));
    LocalOcrProcess afterCompletion;
    QVERIFY(afterCompletion.startOcr(QCoreApplication::applicationFilePath(), { "--ocr-tree-leaf" }, &error, key));
    QVERIFY(completed.finishTree(&error));
    QVERIFY(!afterCompletion.waitForFinished(50));
    QVERIFY(afterCompletion.finishTree(&error));
#endif
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

void LocalMetadataTest::preparationGeometry_data()
{
    QTest::addColumn<QSize>("inputSize");
    QTest::addColumn<int>("rotation");
    for (const QSize size : { QSize(80, 40), QSize(5201, 603), QSize(603, 5201), QSize(1999, 4), QSize(2000, 4) })
        for (const int rotation : { 0, 90, 180, 270 })
            QTest::newRow(qPrintable(QString("%1x%2-turn%3").arg(size.width()).arg(size.height()).arg(rotation))) << size << rotation;
}

void LocalMetadataTest::preparationGeometry()
{
    using namespace LocalMetadata;
    QFETCH(QSize, inputSize);
    QFETCH(int, rotation);
    QImage image(inputSize, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    image.setPixelColor(1, 1, Qt::black);
    const auto original = image.copy();
    OcrOptions options;
    options.rotation = rotation;
    const auto prepared = prepareOcrPage(image, options);
    QVERIFY(validOcrGeometry(prepared.geometry));
    QCOMPARE(image, original);
    QCOMPARE(prepared.image, prepareOcrImage(image, options));
    QCOMPARE(prepared.geometry.inputSize, inputSize);
    QCOMPARE(prepared.geometry.preparedSize, prepared.image.size());
    const QSize scaled = qMax(inputSize.width(), inputSize.height()) > 4000 ? inputSize.scaled(4000, 4000, Qt::KeepAspectRatio) : inputSize;
    const int factor = qMax(scaled.width(), scaled.height()) < 2000 ? 2 : 1;
    const auto rotatedSize = rotation == 90 || rotation == 270 ? scaled.transposed() : scaled;
    QCOMPARE(prepared.image.size(), rotatedSize * factor + QSize(40, 40));
    const QVector<QPointF> points { QPointF(), QPointF(inputSize.width(), 0), QPointF(0, inputSize.height()),
                                    QPointF(inputSize.width(), inputSize.height()), QPointF(1.25, 2.5) };
    for (const auto point : points) {
        const double x = point.x() * scaled.width() / inputSize.width();
        const double y = point.y() * scaled.height() / inputSize.height();
        QPointF expected(x, y);
        if (rotation == 90)
            expected = QPointF(scaled.height() - y, x);
        else if (rotation == 180)
            expected = QPointF(scaled.width() - x, scaled.height() - y);
        else if (rotation == 270)
            expected = QPointF(y, scaled.width() - x);
        expected = expected * factor + QPointF(20, 20);
        const auto actual = prepared.geometry.inputToPrepared.map(point);
        QVERIFY(QLineF(actual, expected).length() < 0.00001);
        QVERIFY(QLineF(prepared.geometry.inputToPrepared.inverted().map(actual), point).length() < 0.00001);
    }
    // A cropped input has a distinct decoded-page offset, never an EXIF claim.
    const QRect crop(QPoint(17, 23), inputSize);
    const QSize pageSize = inputSize + QSize(100, 100);
    const QRectF inputBox(0, 0, inputSize.width(), inputSize.height());
    const auto preparedBox = prepared.geometry.inputToPrepared.mapRect(inputBox);
    const auto mapped = mapOcrBoundsToPage(prepared.geometry, preparedBox, pageSize, crop);
    QVERIFY(mapped.has_value());
    QVERIFY(QLineF(mapped->topLeft(), QPointF(crop.topLeft())).length() < 0.00001);
    QVERIFY(QLineF(mapped->bottomRight(), QPointF(crop.x() + crop.width(), crop.y() + crop.height())).length() < 0.00001);
    QVERIFY(!mapOcrBoundsToPage(prepared.geometry, QRectF(0, 0, 10, 10), inputSize)); // Border only.
    QVERIFY(!mapOcrBoundsToPage(prepared.geometry, preparedBox, pageSize)); // Missing crop context.
    QVERIFY(!mapOcrBoundsToPage(prepared.geometry, preparedBox, inputSize, crop)); // Crop outside page.
    QVERIFY(!mapOcrBoundsToPage(prepared.geometry, QRectF(-1, 0, 2, 2), inputSize));
    options.invert = true;
    QCOMPARE(prepareOcrPage(image, options).geometry.inputToPrepared, prepared.geometry.inputToPrepared);
}

void LocalMetadataTest::preparationEvidenceValidation()
{
    using namespace LocalMetadata;
    const auto prepared = prepareOcrPage(sampleImage(), { });
    QByteArray png;
    QBuffer buffer(&png);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(prepared.image.save(&buffer, "PNG"));
    const auto hash = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
    const QByteArray raw = R"({"version":1,"engine":"paddle-regions","device":"cpu","language":"auto","lines":[]})";
    const NeuralPageEvidence good { 0, prepared.geometry, hash(png), raw, hash(raw), QStringLiteral("cpu"), QStringLiteral("cpu") };
    QVERIFY(validNeuralEvidence(good)); // A successful blank page is evidence.
    for (int corruption = 0; corruption < 10; ++corruption) {
        auto bad = good;
        switch (corruption) {
        case 0:
            bad.rawResult.append(' ');
            break; // Hash no longer matches exact bytes.
        case 1:
            bad.imageSha256 = QStringLiteral("missing");
            break;
        case 2:
            bad.selectedIndex = -1;
            break;
        case 3:
            bad.actualDevice = QStringLiteral("gpu:0");
            break;
        case 4:
            bad.geometry.inputToPrepared = QTransform::fromScale(0, 0);
            break;
        case 5:
            bad.geometry.revision = QStringLiteral("unknown");
            break;
        case 6:
            bad.rawResult = "invalid JSON";
            bad.resultSha256 = hash(bad.rawResult);
            break;
        case 8:
            bad.geometry.inputToPrepared = QTransform::fromTranslate(100000, 0);
            break;
        case 9:
            bad.geometry.inputToPrepared = QTransform::fromScale(std::numeric_limits<double>::infinity(), 1);
            break;
        case 7:
            bad.rawResult.replace("\"lines\":[]", "\"error\":\"failed\",\"lines\":[]");
            bad.resultSha256 = hash(bad.rawResult);
            break;
        }
        QVERIFY(!validNeuralEvidence(bad));
    }
    auto first = syntheticAttempt(1, QStringLiteral("cpu"));
    first.evidence = { good };
    first.evidence[0]->selectedIndex = 2;
    const auto result = LocalOcrRecovery::recognize({ sampleImage() }, { }, { }, { },
                                                    [&](const QVector<QImage> &, const OcrOptions &, const Cancellation &, const Progress &, const PageSink &) { return first; });
    QCOMPARE(result.status, RecognitionStatus::Failed);
    QVERIFY(!result.validPages[0]);
    QVERIFY(!result.evidence[0]);
    QVERIFY(!validOcrGeometry(prepareOcrPage({ }, { }).geometry));
}

void LocalMetadataTest::runtimeSnapshotMeasuresFiles()
{
    using namespace LocalMetadata;
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto write = [&](const QString &relative, const QByteArray &bytes) {
        const auto path = temporary.filePath(relative);
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            return false;
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    const auto hash = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
    QVERIFY(write("app.exe", "synthetic application"));
    QVERIFY(write("Qt6Core.dll", "synthetic shared dependency"));
    QVERIFY(write("imageformats/plugin.dll", "synthetic image plugin"));
    QVERIFY(write("ocr-neural/worker.py", "synthetic worker"));
    QVERIFY(write("ocr-neural/runtime/python.exe", "synthetic CPU interpreter"));
    QVERIFY(write("ocr-neural/runtime/Lib/package.py", "synthetic package"));
    QVERIFY(write("ocr-neural/runtime/Lib/__pycache__/package.pyc", "synthetic bytecode"));
    QJsonArray declaration;
    for (const QString &model : { QStringLiteral("PP-OCRv5_mobile_det"), QStringLiteral("PP-OCRv5_server_rec"), QStringLiteral("korean_PP-OCRv5_mobile_rec") })
        for (const QString &name : { QStringLiteral("inference.json"), QStringLiteral("inference.pdiparams"), QStringLiteral("inference.yml") }) {
            const QString relative = QStringLiteral("models/") + model + u'/' + name;
            const auto bytes = relative.toUtf8();
            QVERIFY(write(QStringLiteral("ocr-neural/") + relative, bytes));
            declaration.append(QJsonObject { { "path", relative }, { "sha256", hash(bytes) } });
        }
    const auto manifest = QJsonDocument(declaration).toJson();
    QVERIFY(write("ocr-neural/models.json", manifest));
    OcrOptions options;
    options.neural = options.gpu = true;
    options.cpuThreads = 4;
    options.rotation = 90;
    options.invert = true;
    QString error;
    const auto measure = [&] { return LocalOcrRuntime::measure(temporary.filePath("app.exe"), options, { }, &error); };
    auto cpuOptions = options;
    cpuOptions.gpu = false;
    const auto measureCpu = [&] { return LocalOcrRuntime::measure(temporary.filePath("app.exe"), cpuOptions, { }, &error); };
    const auto originalCpu = measureCpu();
    QVERIFY2(originalCpu.has_value(), qPrintable(error));
    QVERIFY(originalCpu->settingsSnapshot["environment"].toObject()["gpu"].isNull());
    QVERIFY(originalCpu->manifests["gpu"].isNull());
    QStringList stages;
    const auto reported = LocalOcrRuntime::measure(temporary.filePath("app.exe"), cpuOptions, { }, &error,
                                                   [&](int completed, int total, const QString &stage) {
                                                       QCOMPARE(completed, 0);
                                                       QCOMPARE(total, 0);
                                                       QVERIFY(!stage.contains(temporary.path()));
                                                       stages.append(stage);
                                                   });
    QVERIFY2(reported.has_value(), qPrintable(error));
    QCOMPARE(reported->settingsSnapshot, originalCpu->settingsSnapshot);
    QCOMPARE(reported->manifests, originalCpu->manifests);
    QVERIFY(stages.size() >= 5);
    QVERIFY(stages.last().contains(QStringLiteral("최종 확인")));
    const auto stopFromProgress = std::make_shared<std::atomic_bool>(false);
    QVERIFY(!LocalOcrRuntime::measure(temporary.filePath("app.exe"), cpuOptions, stopFromProgress, &error,
                                      [&](int, int, const QString &stage) {
                                          if (stage.contains(QStringLiteral("CPU 실행 환경")))
                                              stopFromProgress->store(true);
                                      }));
    QVERIFY(error.contains("cancelled"));
    bool changedAtFinalStage = false;
    QVERIFY(!LocalOcrRuntime::measure(temporary.filePath("app.exe"), cpuOptions, { }, &error,
                                      [&](int, int, const QString &stage) {
                                          if (stage.contains(QStringLiteral("최종 확인"))) {
                                              changedAtFinalStage = true;
                                              QVERIFY(write("Qt6Core.dll", "changed during progress callback"));
                                          }
                                      }));
    QVERIFY(changedAtFinalStage);
    QVERIFY(error.contains("changed"));
    QVERIFY(write("Qt6Core.dll", "synthetic shared dependency"));
    const auto original = measure();
    QVERIFY2(original.has_value(), qPrintable(error));
    QVERIFY(error.isEmpty());
    QCOMPARE(original->settingsFingerprint, OcrJobs::settingsFingerprint(original->settingsSnapshot));
    const auto environment = original->settingsSnapshot["environment"].toObject();
    QCOMPARE(environment["applicationSha256"].toString(), hash("synthetic application"));
    QVERIFY(environment["gpu"].isNull());
    QCOMPARE(original->manifests["models"].toArray().size(), 9);
    QCOMPARE(original->manifests["cpu"].toArray().size(), 3); // Bytecode is executable evidence too.
    QCOMPARE(original->settingsSnapshot["options"].toObject()["cpuThreads"].toInt(), 4);
    QCOMPARE(original->settingsSnapshot["options"].toObject()["rotation"].toInt(), 90);
    QVERIFY(original->settingsSnapshot["options"].toObject()["invert"].toBool());
    QCOMPARE(measure()->settingsFingerprint, original->settingsFingerprint);
    const QStringList changed { "app.exe", "Qt6Core.dll", "imageformats/plugin.dll", "ocr-neural/worker.py", "ocr-neural/runtime/python.exe", "ocr-neural/runtime/Lib/package.py", "ocr-neural/runtime/Lib/__pycache__/package.pyc" };
    for (const auto &path : changed) {
        QFile file(temporary.filePath(path));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto before = file.readAll();
        file.close();
        QVERIFY(write(path, before + " changed"));
        const auto current = measure();
        QVERIFY2(current.has_value(), qPrintable(error));
        QVERIFY(current->settingsFingerprint != original->settingsFingerprint);
        const auto currentCpu = measureCpu();
        QVERIFY2(currentCpu.has_value(), qPrintable(error));
        QVERIFY(currentCpu->settingsFingerprint != originalCpu->settingsFingerprint);
        QVERIFY(write(path, before));
        QCOMPARE(measure()->settingsFingerprint, original->settingsFingerprint);
        QCOMPARE(measureCpu()->settingsFingerprint, originalCpu->settingsFingerprint);
    }
    const auto modelPath = declaration[0].toObject()["path"].toString();
    QVERIFY(write(QStringLiteral("ocr-neural/") + modelPath, "changed model"));
    QVERIFY(!measure()); // A lockfile claim cannot substitute for the installed bytes.
    QVERIFY(!measureCpu());
    auto revised = declaration;
    revised[0] = QJsonObject { { "path", modelPath }, { "sha256", hash("changed model") } };
    QVERIFY(write("ocr-neural/models.json", QJsonDocument(revised).toJson()));
    const auto changedModel = measure();
    QVERIFY2(changedModel.has_value(), qPrintable(error));
    QVERIFY(changedModel->settingsFingerprint != original->settingsFingerprint);
    QVERIFY(write(QStringLiteral("ocr-neural/") + modelPath, modelPath.toUtf8()));
    QVERIFY(write("ocr-neural/models.json", manifest));
    auto duplicate = declaration;
    duplicate[1] = duplicate[0];
    QVERIFY(write("ocr-neural/models.json", QJsonDocument(duplicate).toJson()));
    QVERIFY(!measure());
    QVERIFY(write("ocr-neural/models.json", manifest));
    QVERIFY(QDir().mkpath(temporary.filePath("ocr-neural-gpu")));
    QVERIFY(!measure()); // Incomplete addon is not explicit absence for a GPU request.
    const auto incompleteAddonCpu = measureCpu();
    QVERIFY2(incompleteAddonCpu.has_value(), qPrintable(error));
    QCOMPARE(incompleteAddonCpu->settingsSnapshot, originalCpu->settingsSnapshot);
    QCOMPARE(incompleteAddonCpu->manifests, originalCpu->manifests);
    QVERIFY(write("ocr-neural-gpu/runtime/python.exe", "synthetic GPU interpreter"));
    const auto gpu = measure();
    QVERIFY2(gpu.has_value(), qPrintable(error));
    QVERIFY(gpu->settingsSnapshot["environment"].toObject()["gpu"].isObject());
    QVERIFY(gpu->settingsFingerprint != original->settingsFingerprint);
    QCOMPARE(measureCpu()->settingsFingerprint, originalCpu->settingsFingerprint);
    QVERIFY(write("ocr-neural-gpu/runtime/library.dll", "synthetic GPU library"));
    QVERIFY(measure()->settingsFingerprint != gpu->settingsFingerprint);
    const auto changedAddonCpu = measureCpu();
    QVERIFY2(changedAddonCpu.has_value(), qPrintable(error));
    QCOMPARE(changedAddonCpu->settingsSnapshot, originalCpu->settingsSnapshot);
    QCOMPARE(changedAddonCpu->manifests, originalCpu->manifests);
    // Multiple nested files, empty files and shared dependencies must retain
    // the exact serial manifest, irrespective of completion order.
    for (int i = 0; i < 64; ++i)
        QVERIFY(write(QStringLiteral("ocr-neural/runtime/Lib/batch%1/item%2.bin").arg(i % 4).arg(i), QByteArray(i * 1024, char(i))));
    const auto serial = LocalOcrRuntime::measure(temporary.filePath("app.exe"), options, { }, &error, { }, 1);
    QVERIFY2(serial.has_value(), qPrintable(error));
    QCOMPARE(serial->manifests["cpu"].toArray().size(), 67);
    auto *caller = QThread::currentThread();
    for (int readers : { -1, 4, 16, 999 }) {
        const auto parallel = LocalOcrRuntime::measure(temporary.filePath("app.exe"), options, { }, &error, [&](int, int, const QString &) { QCOMPARE(QThread::currentThread(), caller); }, readers);
        QVERIFY2(parallel.has_value(), qPrintable(error));
        QCOMPARE(parallel->settingsSnapshot, serial->settingsSnapshot);
        QCOMPARE(parallel->settingsFingerprint, serial->settingsFingerprint);
        QCOMPARE(parallel->manifests, serial->manifests);
    }
#ifdef Q_OS_WIN
    // A queued file can be enumerated but not read. The measurement must join
    // all readers and reject the partial manifest; retry works after release.
    const auto lockedPath = temporary.filePath("ocr-neural/runtime/Lib/batch0/item0.bin");
    HANDLE locked = CreateFileW(reinterpret_cast<LPCWSTR>(lockedPath.utf16()), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(locked != INVALID_HANDLE_VALUE);
    const auto release = qScopeGuard([&] { if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked); });
    QVERIFY(!measure());
    QVERIFY(error.contains("unreadable"));
    CloseHandle(locked);
    locked = INVALID_HANDLE_VALUE;
    const auto retried = measure();
    QVERIFY2(retried.has_value(), qPrintable(error));
    QCOMPARE(retried->manifests, serial->manifests);
#endif
    QVERIFY(QFile::remove(temporary.filePath("ocr-neural/runtime/python.exe")));
    QVERIFY(!measureCpu()); // The selected CPU runtime remains mandatory.
    QVERIFY(write("ocr-neural/runtime/python.exe", "synthetic CPU interpreter"));
    auto cancelled = std::make_shared<std::atomic_bool>(true);
    QVERIFY(!LocalOcrRuntime::measure(temporary.filePath("app.exe"), options, cancelled, &error));
    QVERIFY(error.contains("cancelled"));
    options.neural = false;
    QVERIFY(!measure()); // This adapter does not claim Tesseract support.
}

void LocalMetadataTest::runtimeSnapshotDeployed()
{
    const auto output = qEnvironmentVariable("YACREADER_RUNTIME_MEASUREMENT_OUTPUT");
    if (output.isEmpty())
        QSKIP("Explicit read-only inventory of an isolated deployed runtime only.");
    const QFileInfo target(output);
    QVERIFY(target.isAbsolute());
    QVERIFY(!target.exists());
    QVERIFY(!target.isSymLink());
    const auto parent = target.absoluteDir().canonicalPath();
    QVERIFY(!parent.isEmpty());
    const auto relative = QDir(QCoreApplication::applicationDirPath()).relativeFilePath(parent);
    QVERIFY(relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative));
    LocalMetadata::OcrOptions options;
    options.neural = options.gpu = true;
    QString error;
    const auto requestedReaders = qEnvironmentVariable("YACREADER_RUNTIME_READERS");
    bool validReaders = true;
    const int readers = requestedReaders.isEmpty() ? 16 : requestedReaders.toInt(&validReaders);
    QVERIFY(validReaders && readers >= 1 && readers <= 16);
    const auto measurement = LocalOcrRuntime::measure(QCoreApplication::applicationFilePath(), options, { }, &error, { }, readers);
    QVERIFY2(measurement.has_value(), qPrintable(error));
    const QJsonObject record { { "settingsSnapshot", measurement->settingsSnapshot }, { "settingsFingerprint", measurement->settingsFingerprint }, { "manifests", measurement->manifests }, { "ocrExecuted", false }, { "diagnosticApplication", true }, { "maximumReaders", readers } };
    QSaveFile file(output);
    file.setDirectWriteFallback(false);
    const auto bytes = QJsonDocument(record).toJson();
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
    QVERIFY(file.commit());
}

namespace {
LocalOcrRuntime::Measurement persistenceSettings(const QString &root)
{
    const QJsonObject cpu { { "executable", root + "/cpu/python.exe" }, { "dataPath", root + "/models" }, { "executableSha256", QString(64, u'1') }, { "workerSha256", QString(64, u'2') }, { "modelManifestSha256", QString(64, u'3') }, { "packageManifestSha256", QString(64, u'4') } };
    auto gpu = cpu;
    gpu["executable"] = root + "/gpu/python.exe";
    gpu["packageManifestSha256"] = QString(64, u'6');
    const QJsonObject snapshot {
        { "version", 1 },
        { "options", QJsonObject { { "neural", true }, { "gpu", true }, { "cpuThreads", 8 }, { "executable", "" }, { "dataPath", "" }, { "language", "auto" }, { "vertical", false }, { "segmentation", 11 }, { "rotation", 0 }, { "invert", false }, { "adaptiveThreshold", false }, { "timeoutMs", 90000 } } },
        { "environment", QJsonObject { { "platform", "windows-x64" }, { "applicationSha256", QString(64, u'7') }, { "preprocessingRevision", "decoded-gray-border-v1" }, { "cpu", cpu }, { "gpu", gpu } } }
    };
    // Synthetic supplied measurements; this test does not claim deployed files.
    return { snapshot, OcrJobs::settingsFingerprint(snapshot), { } };
}
LocalMetadata::NeuralPageEvidence persistencePage()
{
    QImage image(80, 40, QImage::Format_RGB32);
    image.fill(Qt::white);
    auto prepared = LocalMetadata::prepareOcrPage(image, { });
    // QCoreApplication and QApplication can supply different default PNG DPI.
    // This crash fixture must reproduce identical bytes across both processes.
    prepared.image.setDotsPerMeterX(3780);
    prepared.image.setDotsPerMeterY(3780);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    prepared.image.save(&buffer, "PNG");
    const auto hash = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
    const QByteArray raw = R"({"version":1,"engine":"paddle-regions","device":"cpu","language":"auto","lines":[]})";
    return { 0, prepared.geometry, hash(png), raw, hash(raw), QStringLiteral("cpu"), QStringLiteral("cpu") };
}
OcrJobs::Spec persistenceSpec(const QString &root, const LocalOcrRuntime::Measurement &settings)
{
    return { QStringLiteral("synthetic-generation"), QStringLiteral("42"), QString(64, u'a'), QJsonObject { { "path", root + "/synthetic.cbz" }, { "libraryRoot", root }, { "sourceKind", "archive" } }, settings.settingsSnapshot, settings.settingsFingerprint, 2, { 1, 2 } };
}
int persistenceLockHelper(const QStringList &args)
{
    if (args.size() != 5)
        return 90;
    auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("synthetic-locker"));
    db.setDatabaseName(args[2]);
    if (!db.open())
        return 91;
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("BEGIN IMMEDIATE")))
        return 92;
    QFile ready(args[3]);
    if (!ready.open(QIODevice::WriteOnly | QIODevice::NewOnly) || ready.write("locked") != 6)
        return 93;
    ready.close();
    QElapsedTimer timer;
    timer.start();
    while (!QFile::exists(args[4]) && timer.elapsed() < 10000)
        QThread::msleep(10);
    return QFile::exists(args[4]) && query.exec(QStringLiteral("COMMIT")) ? 0 : 94;
}

int persistenceCrashHelper(const QStringList &args)
{
    if (args.size() != 8)
        return 80;
    OcrJobs::Store store;
    if (!store.open(args[2]))
        return 81;
    const auto job = store.get(args[4]);
    if (!job)
        return 82;
    const LocalOcrRuntime::Measurement settings { job->spec.settingsSnapshot, job->spec.settingsFingerprint, { } };
    const auto page = persistencePage();
    if (page.imageSha256 != args[7])
        return 84; // Distinguish fixture byte drift from missing persisted data.
    LocalOcrPersistence::recordPage(store, { args[4], args[5], args[6] }, settings, page, args[3], { }, []() -> qint64 { std::_Exit(86); });
    return 83; // The clock seam is reached only after atomic cache publication.
}
}

void LocalMetadataTest::libraryBinding_data()
{
    QTest::addColumn<QString>("mode");
    for (const auto &mode : { "stable-and-metadata", "replace-database", "reuse-row", "changed-source", "ambiguous", "foreign-schema", "outside", "missing" })
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

void LocalMetadataTest::libraryBinding()
{
#ifndef Q_OS_WIN
    QSKIP("Personal Windows file identity binding");
#else
    QFETCH(QString, mode);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath("library");
    const auto source = root + QStringLiteral("/selected");
    const auto data = root + QStringLiteral("/.yacreaderlibrary");
    const auto path = data + QStringLiteral("/library.ydb");
    QVERIFY(QDir().mkpath(source));
    QVERIFY(QDir().mkdir(data));
    const auto sql = [&](const QString &statement) {
        const auto connection = QUuid::createUuid().toString();
        bool ok = false;
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(path);
            if (db.open()) {
                QSqlQuery query(db);
                ok = query.exec(statement);
            }
        }
        QSqlDatabase::removeDatabase(connection);
        return ok;
    };
    const auto fileHash = [&] {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return QByteArray();
        return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
    };
    QString error;
    if (mode == QStringLiteral("missing")) {
        QVERIFY(!LocalOcrLibrary::read(root, 42, source, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFile::exists(path));
        return;
    }
    QVERIFY(sql("CREATE TABLE comic(id INTEGER, comicInfoId INTEGER, path TEXT, title TEXT)"));
    QVERIFY(sql("INSERT INTO comic VALUES(1,42,'/selected','Initial')"));
    const auto before = fileHash();
    QVERIFY(!before.isEmpty());
    const auto original = LocalOcrLibrary::read(root, 42, source, &error);
    QVERIFY2(original.has_value(), qPrintable(error));
    QCOMPARE(original->generation.size(), 64);
    QCOMPARE(original->comicId, QStringLiteral("1"));
    QCOMPARE(original->comicInfoId, qulonglong(42));
    QCOMPARE(original->sourcePath, QFileInfo(source).canonicalFilePath());
    QCOMPARE(fileHash(), before); // Read binding cannot alter library contents.
    QCOMPARE(QDir(data).entryList(QDir::Files), QStringList({ QStringLiteral("library.ydb") }));
    if (mode == QStringLiteral("stable-and-metadata")) {
        const auto repeated = LocalOcrLibrary::read(root, 42, source, &error);
        QVERIFY2(repeated.has_value(), qPrintable(error));
        QCOMPARE(repeated->generation, original->generation);
        QVERIFY(sql("UPDATE comic SET title='Edited metadata'"));
        const auto changed = fileHash();
        QVERIFY(changed != before);
        const auto current = LocalOcrLibrary::read(root, 42, source, &error);
        QVERIFY(current.has_value());
        QCOMPARE(current->generation, original->generation);
        QCOMPARE(current->comicId, original->comicId);
        QCOMPARE(fileHash(), changed);
    } else if (mode == QStringLiteral("replace-database")) {
        QVERIFY(QFile::copy(path, data + QStringLiteral("/replacement.ydb")));
        QVERIFY(QFile::rename(path, data + QStringLiteral("/original.ydb")));
        QVERIFY(QFile::rename(data + QStringLiteral("/replacement.ydb"), path));
        const auto current = LocalOcrLibrary::read(root, 42, source, &error);
        QVERIFY2(current.has_value(), qPrintable(error));
        QVERIFY(current->generation != original->generation);
        QCOMPARE(fileHash(), before); // Equal bytes do not mean the same file generation.
    } else if (mode == QStringLiteral("reuse-row")) {
        QVERIFY(sql("UPDATE comic SET id=2"));
        const auto current = LocalOcrLibrary::read(root, 42, source, &error);
        QVERIFY(current.has_value());
        QCOMPARE(current->generation, original->generation);
        QCOMPARE(current->comicId, QStringLiteral("2"));
    } else {
        auto requested = source;
        if (mode == QStringLiteral("changed-source"))
            QVERIFY(sql("UPDATE comic SET path='/different'"));
        else if (mode == QStringLiteral("ambiguous"))
            QVERIFY(sql("INSERT INTO comic VALUES(2,42,'/selected','Duplicate')"));
        else if (mode == QStringLiteral("foreign-schema"))
            QVERIFY(sql("DROP TABLE comic"));
        else if (mode == QStringLiteral("outside")) {
            requested = temporary.filePath("outside");
            QVERIFY(QDir().mkdir(requested));
        }
        const auto preserved = fileHash();
        QVERIFY(!LocalOcrLibrary::read(root, 42, requested, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(fileHash(), preserved);
    }
#endif
}

void LocalMetadataTest::inspectorSavedReview_data()
{
    QTest::addColumn<QString>("mode");
    for (const auto &mode : { "restored-title", "fresh-title", "restored-empty", "failed-empty", "failed-title" })
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

void LocalMetadataTest::inspectorSavedReview()
{
    QFETCH(QString, mode);
    YACReaderArchiveInspectorDialog dialog;
    dialog.sourcePath = QStringLiteral("C:/synthetic/selected");
    dialog.titleEdit->setText(QStringLiteral("My reviewed title"));
    dialog.authorEdit->setText(QStringLiteral("My reviewed author"));
    dialog.autoSearch->setChecked(true);
    QSignalSpy requests(&dialog, &YACReaderArchiveInspectorDialog::titleSearchRequested);
    LocalOcrSession::Outcome output;
    output.complete = !mode.startsWith("failed");
    output.restored = mode.startsWith("restored");
    output.cachedPages = output.restored ? 1 : 0;
    output.metadata.pageCount = 1;
    LocalMetadata::Page page;
    page.number = 1;
    page.image = sampleImage();
    page.kind = LocalMetadata::PageKind::Colophon;
    page.reading.elapsedMs = 7890;
    page.reading.device = QStringLiteral("gpu:0");
    output.metadata.pages.append(page);
    if (mode.endsWith("title"))
        output.metadata.suggestions.append({ LocalMetadata::Suggestion::Title, QStringLiteral("Candidate title"), QStringLiteral("Synthetic labelled credit"), 1, true, 99, { { 1, true, 99 } } });
    if (!output.complete)
        output.error = output.metadata.error = QStringLiteral("Synthetic interrupted session");
    dialog.showSavedResult(output, 2000, 3);
    QCOMPARE(dialog.titleEdit->text(), QStringLiteral("My reviewed title"));
    QCOMPARE(dialog.authorEdit->text(), QStringLiteral("My reviewed author"));
    QCOMPARE(requests.count(), mode == "fresh-title" ? 1 : 0);
    QCOMPARE(dialog.filenameFallback, mode == "restored-empty");
    QCOMPARE(dialog.result.pages[0].reading.elapsedMs, qint64(7890));
    QCOMPARE(dialog.result.pages[0].reading.device, QStringLiteral("gpu:0"));
    if (output.restored)
        QVERIFY(dialog.statusLabel->text().contains(QStringLiteral("원래 OCR 기록")));
    if (!output.complete)
        QCOMPARE(dialog.statusLabel->text(), output.error);
}

void LocalMetadataTest::inspectorPreparationCancelled_data()
{
    QTest::addColumn<bool>("reviewed");
    QTest::newRow("preview") << false;
    QTest::newRow("existing-review") << true;
}

void LocalMetadataTest::inspectorPreparationCancelled()
{
    QFETCH(bool, reviewed);
    YACReaderArchiveInspectorDialog dialog;
    dialog.sourcePath = QStringLiteral("C:/synthetic/selected");
    LocalMetadata::Result previous;
    previous.pageCount = 2;
    for (int number : { 1, 2 }) {
        LocalMetadata::Page page;
        page.number = number;
        page.image = sampleImage();
        page.reading.elapsedMs = reviewed ? 7890 : 0;
        page.reading.device = reviewed ? QStringLiteral("gpu:0") : QString();
        previous.pages.append(page);
    }
    previous.suggestions.append({ LocalMetadata::Suggestion::Title, QStringLiteral("Previous candidate"), QStringLiteral("Synthetic evidence"), 1, true, 99, { { 1, true, 99 } } });
    dialog.showResult(previous, reviewed);
    dialog.pageList->setCurrentRow(1);
    dialog.titleEdit->setText(QStringLiteral("My reviewed title"));
    dialog.authorEdit->setText(QStringLiteral("My reviewed author"));
    if (reviewed)
        dialog.savedSelection = LocalOcrLibrary::Binding { };
    dialog.savedSourceFingerprint = reviewed ? QString(64, u'a') : QString();
    const auto fingerprint = dialog.savedSourceFingerprint;
    dialog.savedPerEnd = 1;
    const bool saveEnabled = dialog.saveButton->isEnabled();
    const bool fallback = dialog.filenameFallback;
    dialog.autoSearch->setChecked(true);
    QSignalSpy searches(&dialog, &YACReaderArchiveInspectorDialog::titleSearchRequested);
    QSignalSpy saves(&dialog, &YACReaderArchiveInspectorDialog::metadataSaved);
    dialog.cancellation = std::make_shared<std::atomic_bool>(true);
    LocalOcrSession::Outcome cancelled;
    cancelled.error = cancelled.metadata.error = QStringLiteral("OCR runtime measurement cancelled.");
    dialog.showSavedResult(cancelled, 1000, 3);
    QCOMPARE(dialog.pageList->count(), 2);
    QCOMPARE(dialog.pageList->currentRow(), 1);
    QCOMPARE(dialog.candidateList->count(), 1);
    QCOMPARE(dialog.result.pages.size(), 2);
    QCOMPARE(dialog.result.pages[1].image, previous.pages[1].image);
    QCOMPARE(dialog.result.pages[1].reading.elapsedMs, previous.pages[1].reading.elapsedMs);
    QCOMPARE(dialog.titleEdit->text(), QStringLiteral("My reviewed title"));
    QCOMPARE(dialog.authorEdit->text(), QStringLiteral("My reviewed author"));
    QCOMPARE(dialog.savedSelection.has_value(), reviewed);
    QCOMPARE(dialog.savedSourceFingerprint, fingerprint);
    QCOMPARE(dialog.savedPerEnd, 1);
    QCOMPARE(dialog.saveButton->isEnabled(), saveEnabled);
    QCOMPARE(dialog.filenameFallback, fallback);
    QCOMPARE(searches.count(), 0);
    QCOMPARE(saves.count(), 0);
    QVERIFY(dialog.statusLabel->text().contains(QStringLiteral("읽기 준비를 중지")));
    QVERIFY(!dialog.statusLabel->text().contains(QStringLiteral("runtime")));
}

void LocalMetadataTest::inspectorCompletedCancellation_data()
{
    QTest::addColumn<bool>("stop");
    QTest::newRow("ordinary-completion") << false;
    QTest::newRow("stop-before-ui-delivery") << true;
}

void LocalMetadataTest::inspectorCompletedCancellation()
{
    QFETCH(bool, stop);
    YACReaderArchiveInspectorDialog dialog;
    dialog.sourcePath = QStringLiteral("C:/synthetic/selected");
    dialog.autoSearch->setChecked(true);
    dialog.cancellation = std::make_shared<std::atomic_bool>(false);
    const auto flag = dialog.cancellation;
    QSignalSpy searches(&dialog, &YACReaderArchiveInspectorDialog::titleSearchRequested);
    QSignalSpy saves(&dialog, &YACReaderArchiveInspectorDialog::metadataSaved);
    auto output = std::make_shared<LocalOcrSession::Outcome>();
    bool delivered = false;
    auto *thread = dialog.createWorker(flag, [output] {
        output->complete = true;
        output->metadata.pageCount = 1;
        LocalMetadata::Page page;
        page.number = 1;
        page.image = sampleImage();
        page.text = QStringLiteral("Title: Completed candidate");
        output->metadata.pages.append(page);
        output->metadata.suggestions.append({ LocalMetadata::Suggestion::Title, QStringLiteral("Completed candidate"), QStringLiteral("Synthetic labelled title"), 1, true, 99, { { 1, true, 99 } } }); }, [&] {
        dialog.showSavedResult(*output, 1000, 3);
        delivered = true; }, true);
    QVERIFY(thread);
    dialog.setBusy(true);
    thread->start();
    // Do not process UI events until the worker is done and Stop has been
    // clicked. This deterministically covers the queued-completion race.
    QVERIFY(thread->wait(5000));
    QVERIFY(!delivered);
    if (stop)
        dialog.cancelButton->click();
    QCOMPARE(flag->load(), stop);
    QTRY_VERIFY_WITH_TIMEOUT(delivered, 5000);
    QVERIFY(dialog.activeWorker.isNull());
    QCOMPARE(dialog.pageList->count(), 1);
    QCOMPARE(dialog.candidateList->count(), 1);
    QCOMPARE(dialog.titleEdit->text(), QStringLiteral("Completed candidate"));
    QCOMPARE(dialog.result.pages[0].text, QStringLiteral("Title: Completed candidate"));
    QVERIFY(dialog.ocrButton->isEnabled());
    QVERIFY(dialog.searchButton->isEnabled());
    QCOMPARE(searches.count(), stop ? 0 : 1);
    QCOMPARE(saves.count(), 0);
    QCOMPARE(dialog.automaticSearchDone, !stop);
    if (stop) {
        dialog.requestSearch(true);
        QCOMPARE(searches.count(), 0);
        dialog.requestSearch(false); // Explicit review remains available.
        QCOMPARE(searches.count(), 1);
        QCOMPARE(saves.count(), 0);
    }
}

void LocalMetadataTest::guardedMetadataSave_data()
{
    QTest::addColumn<QString>("mode");
    for (const auto &mode : { "valid", "metadata-edit", "changed-page", "changed-plan", "changed-generation", "changed-row", "invalid-guard" })
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

void LocalMetadataTest::guardedMetadataSave()
{
#ifndef Q_OS_WIN
    QSKIP("Personal Windows saved-review identity guard");
#else
    QFETCH(QString, mode);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath("library");
    const auto source = root + "/Selected";
    QVERIFY(QDir().mkpath(source));
    QVERIFY(sampleImage().save(source + "/1.png"));
    QVERIFY(sampleImage().save(source + "/2.png"));
    QSettings settings(temporary.filePath("settings.ini"), QSettings::IniFormat);
    LibraryCreator creator(&settings);
    creator.createLibrary(root, YACReader::LibraryPaths::libraryDataPath(root));
    creator.start();
    QVERIFY(creator.wait(30000));
    const auto database = YACReader::LibraryPaths::libraryDatabasePath(root);
    const auto query = [&](const QString &statement) {
        QVariant result;
        const auto connection = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(database);
            if (db.open()) {
                QSqlQuery q(db);
                if (q.exec(statement))
                    result = q.next() ? q.value(0) : QVariant(true);
            }
        }
        QSqlDatabase::removeDatabase(connection);
        return result;
    };
    const auto id = query("SELECT comicInfoId FROM comic WHERE path='/Selected'").toULongLong();
    QVERIFY(id);
    QString error;
    const auto binding = LocalOcrLibrary::read(root, id, source, &error);
    QVERIFY2(binding.has_value(), qPrintable(error));
    const auto pages = LocalOcrSource::read(source, 3, { }, &error);
    QVERIFY2(pages.has_value(), qPrintable(error));
    LocalMetadata::SaveGuard guard { binding->generation, binding->comicId, pages->fingerprint, 3 };
    if (mode == "changed-generation")
        guard.libraryGeneration += QStringLiteral("changed");
    if (mode == "invalid-guard")
        guard.sourceFingerprint.clear();
    if (mode == "changed-page") {
        auto image = sampleImage();
        image.fill(Qt::black);
        QVERIFY(image.save(source + "/1.png"));
    }
    if (mode == "changed-plan")
        QVERIFY(sampleImage().save(source + "/3.png"));
    if (mode == "changed-row")
        QVERIFY(query("UPDATE comic SET id=id+100").isValid());
    if (mode == "metadata-edit")
        QVERIFY(query("UPDATE comic_info SET title='Edited before save'").isValid());
    const auto snapshot = [&] {
        QMap<QString, QByteArray> files;
        QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const auto path = it.next();
            QFile f(path);
            if (f.open(QIODevice::ReadOnly))
                files[path] = f.readAll();
        }
        return files;
    };
    const auto before = snapshot();
    const bool success = mode == "valid" || mode == "metadata-edit";
    QCOMPARE(LocalMetadata::save(root, id, source, "Reviewed title", "Reviewed author", true, { }, &error, &guard), success);
    QCOMPARE(error.isEmpty(), success);
    const auto after = snapshot();
    for (auto it = before.cbegin(); it != before.cend(); ++it)
        if (!success || it.key().startsWith(source + '/'))
            QCOMPARE(after.value(it.key()), it.value());
    if (!success)
        QCOMPARE(after.keys(), before.keys());
    if (success)
        QCOMPARE(query("SELECT title FROM comic_info WHERE id=" + QString::number(id)).toString(), QStringLiteral("Reviewed title"));
#endif
}

void LocalMetadataTest::inspectorWorkerLifetime_data()
{
    QTest::addColumn<QString>("mode");
    for (const auto &mode : { "cancel", "selection-change", "missing-selection", "close", "destroy" })
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

void LocalMetadataTest::inspectorWorkerLifetime()
{
    QFETCH(QString, mode);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto folder = temporary.filePath("selected");
    QVERIFY(QDir().mkdir(folder));
    QVERIFY(sampleImage().save(QDir(folder).filePath("new-selection.png"), "PNG"));
    if (mode == QStringLiteral("selection-change") || mode == QStringLiteral("missing-selection")) {
        QVERIFY(QDir().mkdir(temporary.filePath(".yacreaderlibrary")));
        const auto connection = QUuid::createUuid().toString();
        bool created = false;
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(temporary.filePath(".yacreaderlibrary/library.ydb"));
            if (db.open()) {
                QSqlQuery query(db);
                created = query.exec("CREATE TABLE comic(id INTEGER, comicInfoId INTEGER, path TEXT)") && query.exec("INSERT INTO comic VALUES(1, 2, '/selected')");
            }
        }
        QSqlDatabase::removeDatabase(connection);
        QVERIFY(created);
    }
    auto dialog = std::make_unique<YACReaderArchiveInspectorDialog>();
    dialog->sourcePath = folder;
    if (dialog->quality->findData(2) < 0)
        dialog->quality->addItem(QStringLiteral("Synthetic neural selection"), 2);
    dialog->quality->setCurrentIndex(dialog->quality->findData(2));
    dialog->libraryPath = temporary.path();
    dialog->cancellation = std::make_shared<std::atomic_bool>(false);
    const auto flag = dialog->cancellation;
    QSemaphore entered, release;
    bool completed = false;
    QPointer<QThread> thread = dialog->createWorker(flag, [&] {
        entered.release();
        release.acquire(); // Deliberately hold cleanup after cancellation.
    },
                                                    [&] { completed = true; });
    QVERIFY(thread);
    auto cleanup = qScopeGuard([&] {
        release.release();
        if (thread)
            thread->wait(5000);
    });
    dialog->setBusy(true);
    thread->start();
    QVERIFY(entered.tryAcquire(1, 5000));
    if (mode == QStringLiteral("selection-change") || mode == QStringLiteral("missing-selection")) {
        dialog->inspectComic(temporary.path(), mode == QStringLiteral("missing-selection") ? 999 : 2);
        QCOMPARE(dialog->pendingPreview, mode == QStringLiteral("selection-change"));
        QCOMPARE(dialog->sourcePath, mode == QStringLiteral("selection-change") ? QDir::cleanPath(folder) : QString());
    } else if (mode == QStringLiteral("destroy")) {
        dialog.reset();
    } else if (mode == QStringLiteral("close")) {
        dialog->reject();
    } else {
        dialog->cancelButton->click();
        QVERIFY(!dialog->cancelButton->isEnabled());
        QVERIFY(dialog->statusLabel->text().contains(QStringLiteral("정리")));
    }
    QVERIFY(flag->load());
    QVERIFY(!completed);
    if (dialog) {
        QVERIFY(!dialog->ocrButton->isEnabled());
        QVERIFY(!dialog->resumeButton->isEnabled());
        const auto old = dialog->activeWorker;
        dialog->start(false); // Even a direct request cannot overlap cleanup.
        dialog->startSaved(LocalOcrSession::Action::Continue);
        QCOMPARE(dialog->activeWorker, old);
        QVERIFY(!dialog->createWorker(flag, [] { }, [] { }));
    }
    release.release();
    QTRY_VERIFY_WITH_TIMEOUT(thread.isNull() || thread->isFinished(), 5000);
    if (dialog) {
        QTRY_VERIFY_WITH_TIMEOUT(dialog->activeWorker.isNull(), 5000);
        QCOMPARE(dialog->ocrButton->isEnabled(), mode != QStringLiteral("missing-selection"));
        if (mode == QStringLiteral("missing-selection"))
            QVERIFY(!dialog->statusLabel->text().contains(QStringLiteral("다시 실행")));
        QVERIFY(!dialog->pendingPreview);
        if (mode == QStringLiteral("selection-change")) {
            QCOMPARE(dialog->result.pages.size(), 1);
            QCOMPARE(dialog->result.pages[0].name, QStringLiteral("new-selection.png"));
        }
    }
    QVERIFY(!completed); // Cancelled/stale completion never updates the UI.
}

void LocalMetadataTest::isolatedLocalServerNames()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const bool hadRoot = qEnvironmentVariableIsSet("YACREADER_DATA_DIR");
    const auto previousRoot = qEnvironmentVariable("YACREADER_DATA_DIR");
    const auto setRoot = [](const QString &value) {
#ifdef Q_OS_WIN
        return _wputenv_s(L"YACREADER_DATA_DIR", value.toStdWString().c_str()) == 0;
#else
        return qputenv("YACREADER_DATA_DIR", value.toUtf8());
#endif
    };
    const auto restore = qScopeGuard([&] {
        if (hadRoot)
            setRoot(previousRoot);
        else
            qunsetenv("YACREADER_DATA_DIR");
    });
    qunsetenv("YACREADER_DATA_DIR");
    const auto production = YACReader::localServerName();
    QCOMPARE(production, QStringLiteral(YACREADERLIBRARY_GUID));
    // Never listen on, remove or connect to the production endpoint.
    const auto rootA = temporary.filePath(QStringLiteral("한글-profile-A"));
    QVERIFY(setRoot(rootA));
    const auto nameA = YACReader::localServerName();
    QVERIFY(nameA != production);
    QVERIFY(!nameA.contains(rootA));
    QVERIFY(setRoot(rootA + QStringLiteral("/./")));
    QCOMPARE(YACReader::localServerName(), nameA);
#ifdef Q_OS_WIN
    QVERIFY(setRoot(QDir::toNativeSeparators(rootA.toUpper())));
    QCOMPARE(YACReader::localServerName(), nameA);
#endif
    QVERIFY(setRoot(temporary.filePath(QStringLiteral("profile-B"))));
    const auto nameB = YACReader::localServerName();
    QVERIFY(nameB != production && nameB != nameA);
    QLocalServer serverA;
    QLocalServer serverB;
    QVERIFY2(serverA.listen(nameA), qPrintable(serverA.errorString()));
    QVERIFY2(serverB.listen(nameB), qPrintable(serverB.errorString()));
    for (const bool first : { true, false }) {
        QVERIFY(setRoot(first ? rootA : temporary.filePath(QStringLiteral("profile-B"))));
        QLocalSocket client;
        client.connectToServer(YACReader::localServerName());
        QVERIFY2(client.waitForConnected(5000), qPrintable(client.errorString()));
        auto &selected = first ? serverA : serverB;
        auto &other = first ? serverB : serverA;
        QTRY_VERIFY_WITH_TIMEOUT(selected.hasPendingConnections(), 5000);
        QVERIFY(!other.hasPendingConnections());
        std::unique_ptr<QLocalSocket> peer(selected.nextPendingConnection());
        QVERIFY(peer);
        const QByteArray marker = first ? "profile-A" : "profile-B";
        QCOMPARE(client.write(marker), qint64(marker.size()));
        client.flush();
        QTRY_COMPARE_WITH_TIMEOUT(peer->bytesAvailable(), qint64(marker.size()), 5000);
        QCOMPARE(peer->readAll(), marker);
    }
}

// Explicit one-work probe for a private copied library; never registered with real inputs in CI.
void LocalMetadataTest::privateSelectedInspector()
{
    using namespace LocalMetadata;
    const auto manifestPath = qEnvironmentVariable("YACREADER_PRIVATE_INSPECTOR_CASE");
    if (manifestPath.isEmpty())
        QSKIP("Explicit private copied-library inspector case only.");
    QVERIFY(qEnvironmentVariableIsEmpty("YACREADER_SYNTHETIC_RECOVERY_WORKER"));
    QFile manifest(manifestPath);
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    QVERIFY(manifest.size() > 0 && manifest.size() < 16384);
    const auto request = QJsonDocument::fromJson(manifest.readAll()).object();
    QCOMPARE(request["version"].toInt(), 1);
    QCOMPARE(request["purpose"].toString(), QStringLiteral("private-selected-inspector"));
    const auto id = request["id"].toString();
    QVERIFY(QRegularExpression(QStringLiteral("^W(0[1-9]|1[0-6])$")).match(id).hasMatch());
    const auto device = request["device"].toString();
    QVERIFY(device == "cpu" || device == "gpu:0");
    const auto root = request["libraryRoot"].toString();
    const auto input = request["source"].toString();
    const auto profile = qEnvironmentVariable("YACREADER_DATA_DIR");
    const auto output = request["output"].toString();
    const auto regular = [](const QString &path) {
        if (!QDir::isAbsolutePath(path))
            return false;
        auto part = QDir::cleanPath(path);
        while (!part.isEmpty()) {
            const QFileInfo info(part);
            if (info.isSymLink() || info.isJunction())
                return false;
            const auto parent = info.absolutePath();
            if (parent == part)
                break;
            part = parent;
        }
        return true;
    };
    QVERIFY(regular(root) && regular(input) && regular(profile) && regular(output));
    const auto within = [](const QString &parent, const QString &path) {
        const auto relative = QDir(parent).relativeFilePath(path);
        return relative != ".." && !relative.startsWith("../") && !QDir::isAbsolutePath(relative);
    };
    QVERIFY(QFileInfo(root).isDir() && QFileInfo(input).isFile() && within(root, input));
    QVERIFY(!within(root, output) && !within(root, profile));
    QVERIFY(!within(QCoreApplication::applicationDirPath(), output));
    QVERIFY(!QFileInfo::exists(output));
    QVERIFY(QFileInfo(QFileInfo(output).absolutePath()).isDir());
    QFile marker(QDir(root).filePath("private-test-library.json"));
    QVERIFY(marker.open(QIODevice::ReadOnly));
    QVERIFY(marker.size() > 0 && marker.size() < 16384);
    const auto approved = QJsonDocument::fromJson(marker.readAll()).object();
    QCOMPARE(approved["purpose"].toString(), QStringLiteral("private-fixed16-library"));
    int index = -1;
    const auto ids = approved["ids"].toArray();
    for (int i = 0; i < ids.size(); ++i)
        if (ids[i].toString() == id)
            index = i;
    QVERIFY(index >= 0 && approved["sourceSha256"].toArray().size() == ids.size());
    const auto digest = [](const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return QByteArray();
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file))
            return QByteArray();
        return hash.result().toHex();
    };
    const auto inputBefore = digest(input);
    QVERIFY(!inputBefore.isEmpty());
    QCOMPARE(QString::fromLatin1(inputBefore), approved["sourceSha256"].toArray()[index].toString());
    const auto libraryFile = YACReader::LibraryPaths::libraryDatabasePath(root);
    const auto libraryBefore = digest(libraryFile);
    QVERIFY(!libraryBefore.isEmpty());
    const auto dataRoot = QDir(YACReader::getCommonSettingsPath()).filePath("local-ocr/v1");
    const auto database = QDir(dataRoot).filePath("ocr-jobs.sqlite");
    QVERIFY(!QFileInfo::exists(database)); // Every timed case starts without a cache.
    const auto comicId = request["comicInfoId"].toInt();
    QVERIFY(comicId > 0);
    YACReaderArchiveInspectorDialog dialog;
    const auto stop = qScopeGuard([&] { dialog.cancel(); if (dialog.activeWorker) dialog.activeWorker->wait(15000); });
    dialog.autoSearch->setChecked(false);
    dialog.inspectComic(root, qulonglong(comicId));
    QTRY_VERIFY_WITH_TIMEOUT(dialog.activeWorker.isNull(), 30000);
    QCOMPARE(QDir::cleanPath(dialog.sourcePath), QDir::cleanPath(input));
    const auto neuralIndex = dialog.quality->findData(2);
    const auto deviceIndex = dialog.performance->findData(device == "gpu:0" ? -1 : 8);
    const auto languageIndex = dialog.language->findData(QStringLiteral("auto"));
    QVERIFY(neuralIndex >= 0 && deviceIndex >= 0 && languageIndex >= 0);
    dialog.quality->setCurrentIndex(neuralIndex);
    dialog.performance->setCurrentIndex(deviceIndex);
    dialog.language->setCurrentIndex(languageIndex);
    dialog.pageLimit->setValue(3);
    QSignalSpy searches(&dialog, &YACReaderArchiveInspectorDialog::titleSearchRequested);
    QSignalSpy saves(&dialog, &YACReaderArchiveInspectorDialog::metadataSaved);
    QElapsedTimer elapsed;
    QJsonArray executionTimeline, reopenTimeline;
    auto *timeline = &executionTimeline;
    QString phase = QStringLiteral("read");
    const auto tracePath = output + QStringLiteral(".stages.json");
    QVERIFY(!QFileInfo::exists(tracePath));
    bool traceSaved = true;
    const auto captureStage = [&] {
        const auto stage = dialog.statusLabel->text();
        // Count-only updates must not fill the bounded trace before OCR starts.
        const auto stageKey = stage.section(QStringLiteral(" · 파일 "), 0, 0);
        if (!traceSaved || (!timeline->isEmpty() && timeline->last().toObject()["stageKey"].toString() == stageKey))
            return;
        const QJsonObject sample { { "atMs", elapsed.elapsed() }, { "stage", stage }, { "stageKey", stageKey } };
        if (timeline->size() < 128)
            timeline->append(sample);
        else
            (*timeline)[timeline->size() - 1] = sample;
        // Save status changes while work runs, so a watchdog failure retains
        // its last observed stage. This private trace is not an OCR benchmark.
        QSaveFile file(tracePath);
        file.setDirectWriteFallback(false);
        const auto bytes = QJsonDocument(QJsonObject { { "version", 1 }, { "phase", phase }, { "read", executionTimeline }, { "reopen", reopenTimeline }, { "sampleIntervalMs", 100 } }).toJson();
        traceSaved = file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    };
    QTimer stageTimer;
    connect(&stageTimer, &QTimer::timeout, &dialog, captureStage);
    elapsed.start();
    dialog.ocrButton->click();
    captureStage();
    stageTimer.start(100);
    QVERIFY(dialog.activeWorker);
    QTRY_VERIFY_WITH_TIMEOUT(dialog.activeWorker.isNull(), 900000);
    const auto executionMs = elapsed.elapsed();
    stageTimer.stop();
    captureStage();
    QVERIFY(traceSaved);
    QJsonArray pages, candidates, receipts;
    bool devicesMatch = !dialog.result.pages.isEmpty();
    for (const auto &page : dialog.result.pages) {
        const auto &reading = page.reading;
        devicesMatch = devicesMatch && reading.device == device;
        pages.append(QJsonObject { { "number", page.number }, { "sourceEntry", page.name }, { "text", page.text }, { "error", page.error }, { "actualDevice", reading.device }, { "elapsedMs", reading.elapsedMs }, { "initializationMs", reading.initializationMs } });
    }
    for (const auto &candidate : dialog.result.suggestions)
        candidates.append(QJsonObject { { "field", int(candidate.field) }, { "value", candidate.value }, { "page", candidate.page }, { "labelled", candidate.labelled }, { "reason", candidate.reason } });
    OcrJobs::Store store;
    QVERIFY(store.open(database));
    const auto jobs = store.list();
    QCOMPARE(jobs.size(), 1);
    const auto job = jobs.first();
    for (const auto &receipt : job.pages)
        receipts.append(QJsonObject { { "page", receipt.page }, { "cacheKey", receipt.cacheKey }, { "resultSha256", receipt.resultSha256 }, { "actualDevice", receipt.actualDevice } });
    QJsonObject report { { "version", 1 }, { "id", id }, { "requestedDevice", device }, { "cpuThreads", 8 }, { "perEnd", 3 }, { "executionMs", executionMs }, { "executionTimeline", executionTimeline }, { "qtFunctionTimeoutMs", qEnvironmentVariable("QTEST_FUNCTION_TIMEOUT") }, { "jobId", job.id }, { "state", OcrJobs::stateName(job.state) }, { "attempts", job.attempts }, { "error", dialog.result.error }, { "pages", pages }, { "candidates", candidates }, { "receipts", receipts }, { "sourceFingerprint", dialog.savedSourceFingerprint }, { "settingsFingerprint", job.spec.settingsFingerprint }, { "sourceUnchanged", digest(input) == inputBefore }, { "libraryUnchanged", digest(libraryFile) == libraryBefore }, { "externalLookups", searches.count() }, { "metadataSaves", saves.count() }, { "devicesMatch", devicesMatch }, { "completedReviewRestored", false } };
    const auto writeReport = [&] {
        QSaveFile file(output);
        file.setDirectWriteFallback(false);
        const auto bytes = QJsonDocument(report).toJson();
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    };
    QVERIFY(writeReport()); // Preserve failure evidence before assertions.
    QVERIFY2(dialog.result.error.isEmpty(), qPrintable(dialog.result.error));
    QVERIFY(job.state == OcrJobs::State::PageReview || job.state == OcrJobs::State::FilenameReview);
    QCOMPARE(job.attempts, 1);
    QCOMPARE(job.pages.size(), dialog.result.pages.size());
    QVERIFY(devicesMatch && report["sourceUnchanged"].toBool() && report["libraryUnchanged"].toBool());
    QCOMPARE(searches.count(), 0);
    QCOMPARE(saves.count(), 0);
    const auto cacheDigest = [&] {
        QMap<QString, QByteArray> result;
        const auto cache = QDir(dataRoot).filePath("pages");
        for (const auto &name : QDir(cache).entryList(QDir::Files))
            result[name] = digest(QDir(cache).filePath(name));
        return result;
    };
    const auto originalCache = cacheDigest();
    QVERIFY(!originalCache.isEmpty());
    timeline = &reopenTimeline;
    phase = QStringLiteral("reopen");
    elapsed.restart();
    dialog.resumeButton->click();
    captureStage();
    stageTimer.start(100);
    QVERIFY(dialog.activeWorker);
    QTRY_VERIFY_WITH_TIMEOUT(dialog.activeWorker.isNull(), 120000);
    report["reopenMs"] = elapsed.elapsed();
    stageTimer.stop();
    captureStage();
    report["reopenTimeline"] = reopenTimeline;
    QVERIFY(traceSaved);
    QVERIFY2(dialog.result.error.isEmpty(), qPrintable(dialog.result.error));
    QVERIFY(dialog.statusLabel->text().contains(QStringLiteral("원래 OCR 기록")));
    const auto restored = store.get(job.id);
    QVERIFY(restored.has_value());
    QCOMPARE(restored->attempts, 1);
    QCOMPARE(restored->state, job.state);
    QCOMPARE(cacheDigest(), originalCache);
    QCOMPARE(digest(input), inputBefore);
    QCOMPARE(digest(libraryFile), libraryBefore);
    QCOMPARE(searches.count(), 0);
    QCOMPARE(saves.count(), 0);
    report["completedReviewRestored"] = true;
    QVERIFY(writeReport());
    qInfo("Private selected inspector %s device=%s pages=%lld complete, no metadata writes", qPrintable(id), qPrintable(device), qint64(pages.size()));
}

void LocalMetadataTest::inspectorSessionNeural()
{
    using namespace LocalMetadata;
    const auto expected = qEnvironmentVariable("YACREADER_EXPECT_NEURAL_DEVICE");
    if (expected.isEmpty())
        QSKIP("Explicit packaged CPU or isolated physical GPU inspector session only.");
    QVERIFY(expected == QStringLiteral("cpu") || expected == QStringLiteral("gpu:0"));
    QVERIFY(qEnvironmentVariableIsEmpty("YACREADER_SYNTHETIC_RECOVERY_WORKER"));
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath("library");
    const auto folder = root + QStringLiteral("/synthetic-session");
    const auto data = root + QStringLiteral("/.yacreaderlibrary");
    QVERIFY(QDir().mkpath(folder));
    QVERIFY(QDir().mkpath(data));
    for (const auto &item : { QPair<QString, QString>("kor", "1.png"), { "jpn", "2.png" } })
        QVERIFY(QFile::copy(QCoreApplication::applicationDirPath() + QStringLiteral("/ocr-colophon-%1.png").arg(item.first), QDir(folder).filePath(item.second)));
    const auto libraryDatabase = data + QStringLiteral("/library.ydb");
    const auto connection = QUuid::createUuid().toString();
    bool created = false;
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(libraryDatabase);
        if (db.open()) {
            QSqlQuery query(db);
            created = query.exec("CREATE TABLE comic(id INTEGER,comicInfoId INTEGER,path TEXT)") && query.exec("INSERT INTO comic VALUES(1,42,'/synthetic-session')");
        }
    }
    QSqlDatabase::removeDatabase(connection);
    QVERIFY(created);
    const auto bytes = [](const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); };
    const auto originalLibrary = bytes(libraryDatabase);
    const auto koreanBytes = bytes(folder + "/1.png");
    const auto japaneseBytes = bytes(folder + "/2.png");
    QVERIFY(!originalLibrary.isEmpty() && !koreanBytes.isEmpty() && !japaneseBytes.isEmpty());
    const bool hadRoot = qEnvironmentVariableIsSet("YACREADER_DATA_DIR");
    const auto previousRoot = qEnvironmentVariable("YACREADER_DATA_DIR");
    const auto setRoot = [](const QString &value) {
#ifdef Q_OS_WIN
        return _wputenv_s(L"YACREADER_DATA_DIR", value.toStdWString().c_str()) == 0;
#else
        return qputenv("YACREADER_DATA_DIR", value.toUtf8());
#endif
    };
    const auto restoreRoot = qScopeGuard([&] { if (hadRoot) setRoot(previousRoot); else qunsetenv("YACREADER_DATA_DIR"); });
    QVERIFY(setRoot(temporary.filePath("settings")));
    const auto configure = [&](YACReaderArchiveInspectorDialog &dialog) {
        const auto qualityIndex = dialog.quality->findData(2);
        const auto deviceIndex = dialog.performance->findData(expected == "gpu:0" ? -1 : 8);
        if (qualityIndex < 0 || deviceIndex < 0)
            return false;
        dialog.quality->setCurrentIndex(qualityIndex);
        dialog.performance->setCurrentIndex(deviceIndex);
        return true;
    };
    auto dialog = std::make_unique<YACReaderArchiveInspectorDialog>();
    const auto stopFirst = qScopeGuard([&] { if (dialog) { dialog->cancel(); if (dialog->activeWorker) dialog->activeWorker->wait(15000); } });
    dialog->autoSearch->setChecked(false);
    dialog->inspectComic(root, 42);
    QTRY_VERIFY_WITH_TIMEOUT(dialog->activeWorker.isNull(), 30000);
    QVERIFY(configure(*dialog));
    LocalOcrSession::Request request;
    request.libraryRoot = root;
    request.comicInfoId = 42;
    request.sourcePath = folder;
    request.applicationPath = QCoreApplication::applicationFilePath();
    request.dataRoot = QDir(YACReader::getCommonSettingsPath()).filePath("local-ocr/v1");
    request.options = dialog->ocrOptions();
    const auto flag = std::make_shared<std::atomic_bool>(false);
    int accepted = 0;
    // Use the actual OCR runner. Cancel synchronously after the first durable
    // receipt so even batched file collection cannot publish a second receipt.
    const auto first = LocalOcrSession::run(request, LocalOcrSession::Action::Start, flag, { }, { },
                                            [&](const QVector<QImage> &images, const OcrOptions &options, const Cancellation &cancel, const Progress &progress, const PageSink &sink) {
                                                return recognizePagesWithOutcome(images, options, cancel, progress, [&](const NeuralPageEvidence &page, QString *error) {
                                                    if (!sink(page, error))
                                                        return false;
                                                    ++accepted;
                                                    cancel->store(true);
                                                    return true;
                                                });
                                            });
    QVERIFY(!first.complete);
    QVERIFY(first.state.has_value());
    QCOMPARE(*first.state, OcrJobs::State::Paused);
    QCOMPARE(accepted, 1);
    QCOMPARE(first.metadata.pages.size(), 2);
    QCOMPARE(first.metadata.pages[0].reading.device, expected);
    const auto database = QDir(request.dataRoot).filePath("ocr-jobs.sqlite");
    const auto job = [&]() -> std::optional<OcrJobs::Job> { OcrJobs::Store store; if (!store.open(database)) return std::nullopt; return store.get(first.jobId); };
    const auto paused = job();
    QVERIFY(paused.has_value());
    QCOMPARE(paused->pages.size(), 1);
    QCOMPARE(paused->attempts, 1);
    dialog->titleEdit->setText(QStringLiteral("My reviewed title"));
    QSignalSpy searches(dialog.get(), &YACReaderArchiveInspectorDialog::titleSearchRequested);
    QSignalSpy saves(dialog.get(), &YACReaderArchiveInspectorDialog::metadataSaved);
    QVERIFY(dialog->resumeButton->isEnabled());
    dialog->resumeButton->click();
    QVERIFY(dialog->activeWorker);
    QVERIFY(!dialog->ocrButton->isEnabled() && !dialog->resumeButton->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(dialog->activeWorker.isNull(), 240000);
    QVERIFY2(dialog->result.error.isEmpty(), qPrintable(dialog->statusLabel->text()));
    QCOMPARE(dialog->result.pages.size(), 2);
    QCOMPARE(dialog->titleEdit->text(), QStringLiteral("My reviewed title"));
    QCOMPARE(searches.count(), 0);
    QCOMPARE(saves.count(), 0);
    QVERIFY(dialog->savedSelection.has_value());
    QVector<qint64> originalTimes;
    for (int i = 0; i < 2; ++i) {
        const auto &reading = dialog->result.pages[i].reading;
        QCOMPARE(reading.device, expected);
        originalTimes.append(reading.elapsedMs);
        auto text = reading.text;
        text.remove(QRegularExpression(QStringLiteral("\\s+")));
        QVERIFY2(text.contains(i == 0 ? QStringLiteral("홍길동") : QStringLiteral("見本太郎")), qPrintable(reading.text));
    }
    QCOMPARE(originalTimes[0], first.metadata.pages[0].reading.elapsedMs);
    const auto completed = job();
    QVERIFY(completed.has_value());
    QCOMPARE(completed->attempts, 2);
    QCOMPARE(completed->pages.size(), 2);
    QVERIFY(completed->state == OcrJobs::State::PageReview || completed->state == OcrJobs::State::FilenameReview);
    const auto savedCache = [&] {
        QMap<QString, QByteArray> files;
        const auto cache = QDir(request.dataRoot).filePath("pages");
        for (const auto &name : QDir(cache).entryList(QDir::Files))
            files[name] = bytes(QDir(cache).filePath(name));
        return files;
    };
    const auto beforeCache = savedCache();
    QVERIFY(!beforeCache.isEmpty());
    dialog.reset(); // Fresh inspector instance and freshly owned worker/Store.
    YACReaderArchiveInspectorDialog reopened;
    const auto stopReopened = qScopeGuard([&] { reopened.cancel(); if (reopened.activeWorker) reopened.activeWorker->wait(15000); });
    reopened.inspectComic(root, 42);
    QTRY_VERIFY_WITH_TIMEOUT(reopened.activeWorker.isNull(), 30000);
    QVERIFY(configure(reopened));
    reopened.autoSearch->setChecked(true); // Restoration must still avoid lookup.
    reopened.titleEdit->setText(QStringLiteral("Keep edited title"));
    reopened.authorEdit->setText(QStringLiteral("Keep edited author"));
    QSignalSpy reopenedSearches(&reopened, &YACReaderArchiveInspectorDialog::titleSearchRequested);
    reopened.resumeButton->click();
    QVERIFY(reopened.activeWorker);
    QTRY_VERIFY_WITH_TIMEOUT(reopened.activeWorker.isNull(), 240000);
    QVERIFY2(reopened.result.error.isEmpty(), qPrintable(reopened.statusLabel->text()));
    QVERIFY(reopened.statusLabel->text().contains(QStringLiteral("원래 OCR 기록")));
    QCOMPARE(reopened.titleEdit->text(), QStringLiteral("Keep edited title"));
    QCOMPARE(reopened.authorEdit->text(), QStringLiteral("Keep edited author"));
    QCOMPARE(reopenedSearches.count(), 0);
    QCOMPARE(reopened.result.pages.size(), 2);
    for (int i = 0; i < 2; ++i) {
        QCOMPARE(reopened.result.pages[i].reading.device, expected);
        QCOMPARE(reopened.result.pages[i].reading.elapsedMs, originalTimes[i]);
    }
    const auto restored = job();
    QVERIFY(restored.has_value());
    QCOMPARE(restored->attempts, completed->attempts);
    QCOMPARE(restored->state, completed->state);
    QCOMPARE(savedCache(), beforeCache); // No raw result replacement during UI restoration.
    QCOMPARE(bytes(libraryDatabase), originalLibrary);
    QCOMPARE(bytes(folder + "/1.png"), koreanBytes);
    QCOMPARE(bytes(folder + "/2.png"), japaneseBytes);
    QCOMPARE(QDir(data).entryList(QDir::Files), QStringList({ QStringLiteral("library.ydb") }));
    qInfo("Actual inspector session device=%s attempts=%d receipts=%lld; restored without another claim", qPrintable(expected), restored->attempts, qint64(restored->pages.size()));
}

void LocalMetadataTest::claimedExecutorNeural()
{
    using namespace LocalMetadata;
    const auto expected = qEnvironmentVariable("YACREADER_EXPECT_NEURAL_DEVICE");
    if (expected.isEmpty())
        QSKIP("Explicit packaged CPU or physical GPU executor verification only.");
    QVERIFY(expected == QStringLiteral("cpu") || expected == QStringLiteral("gpu:0"));
    QVERIFY(qEnvironmentVariableIsEmpty("YACREADER_SYNTHETIC_RECOVERY_WORKER"));
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto folder = temporary.filePath("synthetic-resume");
    const auto cache = temporary.filePath("cache");
    QVERIFY(QDir().mkdir(folder));
    QVERIFY(QDir().mkdir(cache));
    for (const auto &item : { QPair<QString, QString>("kor", "1.png"), { "jpn", "2.png" }, { "kor", "3.png" } })
        QVERIFY(QFile::copy(QCoreApplication::applicationDirPath() + QStringLiteral("/ocr-colophon-%1.png").arg(item.first), QDir(folder).filePath(item.second)));
    auto options = defaultOcrOptions();
    options.neural = true;
    options.gpu = expected == QStringLiteral("gpu:0");
    options.language = QStringLiteral("auto");
    QString error;
    const auto data = temporary.filePath(".yacreaderlibrary");
    QVERIFY(QDir().mkdir(data));
    const auto libraryDatabase = data + QStringLiteral("/library.ydb");
    const auto connection = QUuid::createUuid().toString();
    bool created = false;
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(libraryDatabase);
        if (db.open()) {
            QSqlQuery query(db);
            created = query.exec("CREATE TABLE comic(id INTEGER, comicInfoId INTEGER, path TEXT)") && query.exec("INSERT INTO comic VALUES(1,42,'/synthetic-resume')");
        }
    }
    QSqlDatabase::removeDatabase(connection);
    QVERIFY(created);
    const auto libraryBytes = [&] {
        QFile file(libraryDatabase);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const auto originalLibrary = libraryBytes();
    QVERIFY(!originalLibrary.isEmpty());
    const auto prepared = LocalOcrPreflight::read(temporary.path(), 42, folder, 3, QCoreApplication::applicationFilePath(), options, &error);
    QVERIFY2(prepared.has_value(), qPrintable(error));
    const auto *source = &prepared->source;
    const auto *measured = &prepared->runtime;
    const auto spec = prepared->spec;
    OcrJobs::Store store;
    QVERIFY(store.open(temporary.filePath("ocr-jobs.sqlite")));
    const auto id = store.enqueue(spec);
    QVERIFY(id.has_value());
    const auto now = [] { return QDateTime::currentMSecsSinceEpoch(); };
    const auto firstLease = store.claimWhenCurrent(*id, QStringLiteral("first-actual-owner"), 300000, now);
    QVERIFY(firstLease.has_value());
    const auto first = recognizePagesWithOutcome({ source->source.pages[1].image }, options, { });
    QCOMPARE(first.status, RecognitionStatus::Complete);
    QVERIFY(first.evidence[0].has_value());
    QCOMPARE(first.evidence[0]->actualDevice, expected);
    const auto identity = LocalOcrPersistence::identity(*measured, *first.evidence[0], &error);
    QVERIFY2(identity.has_value(), qPrintable(error));
    QVERIFY(LocalOcrCache::save(cache, *identity, first.evidence[0]->rawResult, &error));
    QVERIFY(store.get(*id)->pages.isEmpty()); // Interrupt after cache publication, before the page receipt.
    QVERIFY(store.pauseWhenCurrent(*firstLease, now));
    // Both input and deployed runtime are remeasured after the first worker exit.
    const auto current = LocalOcrPreflight::read(temporary.path(), 42, folder, 3, QCoreApplication::applicationFilePath(), options, &error);
    QVERIFY2(current.has_value(), qPrintable(error));
    const auto *currentSource = &current->source;
    const auto *currentRuntime = &current->runtime;
    QCOMPARE(current->spec.libraryGeneration, spec.libraryGeneration);
    QCOMPARE(current->spec.comicId, spec.comicId);
    QCOMPARE(currentSource->fingerprint, source->fingerprint);
    QCOMPARE(currentRuntime->settingsFingerprint, measured->settingsFingerprint);
    QVERIFY(store.resume(*id));
    const auto lease = store.claimWhenCurrent(*id, QStringLiteral("resumed-actual-owner"), 300000, now);
    QVERIFY(lease.has_value());
    const auto out = LocalOcrExecutor::executeClaimed(store, *lease, *currentSource, *currentRuntime, cache, { });
    QCOMPARE(out.batch.status, RecognitionStatus::Complete);
    QVERIFY2(out.stateSaved, qPrintable(out.stateError));
    QCOMPARE(out.cachedPages, 1);
    QCOMPARE(out.recoveredUnrecordedPages, 1);
    QCOMPARE(out.repeatedInputPages, 1);
    QCOMPARE(out.cacheHits, QVector<bool>({ false, true, false }));
    QCOMPARE(out.batch.validPages, QVector<bool>({ true, true, true }));
    QCOMPARE(out.batch.evidence[1]->rawResult, first.evidence[0]->rawResult);
    const auto job = store.get(*id);
    QVERIFY(job.has_value());
    QCOMPARE(job->pages.size(), 3);
    QVERIFY(job->state == OcrJobs::State::PageReview || job->state == OcrJobs::State::FilenameReview);
    for (int i = 0; i < 3; ++i) {
        QCOMPARE(out.batch.readings[i].device, expected);
        QString text = out.batch.readings[i].text;
        text.remove(QRegularExpression(QStringLiteral("\\s+")));
        QVERIFY2(text.contains(i == 1 ? QStringLiteral("見本太郎") : QStringLiteral("홍길동")), qPrintable(out.batch.readings[i].text));
        const auto &page = *out.batch.evidence[i];
        const auto recovered = LocalOcrPersistence::restorePage(*job, *currentRuntime, i, page.geometry, page.imageSha256, cache, &error);
        QVERIFY2(recovered.has_value(), qPrintable(error));
        QCOMPARE(recovered->rawResult, page.rawResult);
        QCOMPARE(recovered->actualDevice, expected);
    }
    const auto reviewPrepared = LocalOcrPreflight::read(temporary.path(), 42, folder, 3, QCoreApplication::applicationFilePath(), options, &error);
    QVERIFY2(reviewPrepared.has_value(), qPrintable(error));
    QCOMPARE(reviewPrepared->runtime.settingsFingerprint, measured->settingsFingerprint);
    const auto review = LocalOcrExecutor::restoreReview(*job, reviewPrepared->library, reviewPrepared->source, reviewPrepared->runtime, cache, &error);
    QVERIFY2(review.has_value(), qPrintable(error));
    QCOMPARE(review->evidence.size(), 3);
    for (int i = 0; i < 3; ++i) {
        QCOMPARE(review->evidence[i].rawResult, out.batch.evidence[i]->rawResult);
        QCOMPARE(review->metadata.pages[i].reading.device, expected);
        QCOMPARE(review->metadata.pages[i].reading.elapsedMs, out.batch.readings[i].elapsedMs);
    }
    QCOMPARE(out.batch.evidence[0]->rawResult, out.batch.evidence[2]->rawResult);
    QCOMPARE(out.batch.evidence[2]->selectedIndex, 2);
    QCOMPARE(job->pages[0].cacheKey, job->pages[2].cacheKey);
    QCOMPARE(store.get(*id)->state, job->state);
    QCOMPARE(libraryBytes(), originalLibrary);
    QCOMPARE(QDir(data).entryList(QDir::Files), QStringList({ QStringLiteral("library.ydb") }));
    qInfo("Actual executor device=%s cached=%d cache-phase=%lld ms unique-new-input-ocr=%lld ms",
          qPrintable(expected), out.cachedPages, out.cacheReadMs, out.batch.readings[0].elapsedMs);
}

void LocalMetadataTest::claimedExecutorWorker_data()
{
    QTest::addColumn<QString>("mode");
    for (const auto &mode : { "partial-gap", "cancel-after-page", "all-output-failure", "partial-cpu-failure" })
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

void LocalMetadataTest::claimedExecutorWorker()
{
    using namespace LocalMetadata;
    if (qEnvironmentVariable("YACREADER_SYNTHETIC_RECOVERY_WORKER") != QStringLiteral("1"))
        QSKIP("Explicit isolated synthetic worker failure injection only.");
    QFETCH(QString, mode);
    QFile worker(QCoreApplication::applicationDirPath() + QStringLiteral("/ocr-neural/worker.py"));
    QVERIFY(worker.open(QIODevice::ReadOnly));
    QVERIFY(worker.readLine().startsWith("# YACReader synthetic recovery worker v1"));
    QVERIFY(QFile::exists(QCoreApplication::applicationDirPath() + QStringLiteral("/ocr-neural-gpu/runtime/python.exe")));
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto oldMode = qgetenv("YACREADER_FAULT_MODE");
    const auto oldTrace = qgetenv("YACREADER_FAULT_TRACE_BASE64");
    const auto restore = qScopeGuard([&] {
        if (oldMode.isNull())
            qunsetenv("YACREADER_FAULT_MODE");
        else
            qputenv("YACREADER_FAULT_MODE", oldMode);
        if (oldTrace.isNull())
            qunsetenv("YACREADER_FAULT_TRACE_BASE64");
        else
            qputenv("YACREADER_FAULT_TRACE_BASE64", oldTrace);
    });
    const auto tracePath = temporary.filePath("trace.jsonl");
    QVERIFY(qputenv("YACREADER_FAULT_MODE", mode.toUtf8()));
    QVERIFY(qputenv("YACREADER_FAULT_TRACE_BASE64", tracePath.toUtf8().toBase64()));
    const auto folder = temporary.filePath("synthetic-live-executor");
    const auto cache = temporary.filePath("cache");
    QVERIFY(QDir().mkdir(folder));
    QVERIFY(QDir().mkdir(cache));
    for (int i = 0; i < 3; ++i) {
        QImage image(80 + i * 10, 40, QImage::Format_RGB32);
        image.fill(Qt::white);
        QVERIFY(image.save(QDir(folder).filePath(QStringLiteral("%1.png").arg(i + 1)), "PNG"));
    }
    QString error;
    const auto source = LocalOcrSource::read(folder, 3, { }, &error);
    QVERIFY2(source.has_value(), qPrintable(error));
    const auto settings = persistenceSettings(temporary.path()); // Synthetic identities only.
    auto spec = persistenceSpec(temporary.path(), settings);
    spec.sourceSnapshot = source->fingerprint;
    spec.sourceContext = { { "path", source->manifest["path"] }, { "sourceKind", "folder" }, { "libraryRoot", temporary.path() } };
    spec.totalPages = 3;
    spec.pages = { 1, 2, 3 };
    OcrJobs::Store store;
    QVERIFY(store.open(temporary.filePath("ocr-jobs.sqlite")));
    const auto id = store.enqueue(spec);
    QVERIFY(id.has_value());
    const auto lease = store.claim(*id, QStringLiteral("synthetic-live-owner"), 1000, 1000);
    QVERIFY(lease.has_value());
    const auto hash = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
    const auto prepared = prepareOcrPage(source->source.pages[0].image, { });
    QByteArray png;
    QBuffer buffer(&png);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(prepared.image.save(&buffer, "PNG"));
    const QByteArray raw = R"({"version":1,"engine":"paddle-regions","device":"cpu","language":"auto","lines":[]})";
    const NeuralPageEvidence cached { 0, prepared.geometry, hash(png), raw, hash(raw), QStringLiteral("cpu"), QStringLiteral("cpu") };
    QVERIFY(LocalOcrPersistence::recordPage(store, *lease, settings, cached, cache, { }, [] { return 1001; }).receiptSaved);
    auto cancel = std::make_shared<std::atomic_bool>(false);
    QElapsedTimer elapsed;
    elapsed.start();
    // Default runner launches the isolated Python processes; no injected Runner.
    const auto out = LocalOcrExecutor::executeClaimed(store, *lease, *source, settings, cache, cancel, [&](int completed, int, const QString &) {
                if (mode == QStringLiteral("cancel-after-page") && completed > 1)
                    cancel->store(true); }, [] { return 1002; });
    QVERIFY(out.stateSaved);
    QCOMPARE(out.cachedPages, 1);
    QCOMPARE(out.cacheHits, QVector<bool>({ true, false, false }));
    const auto job = store.get(*id);
    QVERIFY(job.has_value());
    if (mode == QStringLiteral("cancel-after-page")) {
        QVERIFY(elapsed.elapsed() < 15000);
        QCOMPARE(out.batch.status, RecognitionStatus::Cancelled);
        QCOMPARE(job->state, OcrJobs::State::Paused);
        QCOMPARE(job->pages.size(), 2);
        QCOMPARE(out.batch.validPages, QVector<bool>({ true, false, true }));
    } else {
        QCOMPARE(job->pages.size(), 3);
        QCOMPARE(out.batch.validPages, QVector<bool>({ true, true, true }));
        if (mode == QStringLiteral("partial-gap")) {
            QCOMPARE(out.batch.status, RecognitionStatus::Complete);
            QVERIFY(job->state == OcrJobs::State::PageReview || job->state == OcrJobs::State::FilenameReview);
        } else {
            QCOMPARE(out.batch.status, RecognitionStatus::Failed);
            QCOMPARE(job->state, OcrJobs::State::Failed);
            QVERIFY(!out.batch.error.isEmpty());
        }
    }
    QFile trace(tracePath);
    QVERIFY(trace.open(QIODevice::ReadOnly));
    QVector<QJsonObject> calls;
    while (!trace.atEnd())
        calls.append(QJsonDocument::fromJson(trace.readLine()).object());
    const bool retry = mode == QStringLiteral("partial-gap") || mode == QStringLiteral("partial-cpu-failure");
    QCOMPARE(calls.size(), retry ? 2 : 1);
    const auto gpuImages = calls[0]["images"].toArray();
    QCOMPARE(gpuImages.size(), 2); // Cached first image was never sent to a worker.
    QCOMPARE(gpuImages[1].toString(), out.batch.evidence[2]->imageSha256);
    QVERIFY(!gpuImages.contains(cached.imageSha256));
    if (retry) {
        QCOMPARE(calls[1]["images"].toArray(), QJsonArray({ gpuImages[0] }));
        QCOMPARE(out.batch.evidence[1]->requestedDevice, QStringLiteral("cpu"));
    }
    QCOMPARE(out.batch.evidence[2]->requestedDevice, QStringLiteral("gpu:0"));
    for (int i = 0; i < 3; ++i) {
        if (!out.batch.validPages[i])
            continue;
        const auto &page = *out.batch.evidence[i];
        const auto loaded = LocalOcrPersistence::restorePage(*job, settings, i, page.geometry, page.imageSha256, cache, &error);
        QVERIFY2(loaded.has_value(), qPrintable(error));
        QCOMPARE(loaded->selectedIndex, i);
        QCOMPARE(loaded->rawResult, page.rawResult);
        QCOMPARE(loaded->requestedDevice, page.requestedDevice);
    }
    for (const auto &call : calls)
        for (const auto &output : call["outputs"].toArray())
            QVERIFY(!QFile::exists(output.toString()));
    const auto unchanged = LocalOcrSource::read(folder, 3, { }, &error);
    QVERIFY(unchanged.has_value());
    QCOMPARE(unchanged->fingerprint, source->fingerprint);
}

void LocalMetadataTest::claimedExecutor_data()
{
    QTest::addColumn<QString>("mode");
    for (const auto &mode : { "fresh", "resume", "orphan-first", "orphan-all", "orphan-damaged", "orphan-ambiguous", "page-candidate", "cancel", "worker-failure", "missing-delivery", "lease-expired", "changed-source", "changed-settings", "damaged-cache", "all-recorded", "changed-delivery", "changed-completion", "callback-throw", "stale-owner" })
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

void LocalMetadataTest::claimedExecutor()
{
    using namespace LocalMetadata;
    QFETCH(QString, mode);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto folder = temporary.filePath("synthetic-executor");
    const auto cache = temporary.filePath("cache");
    QVERIFY(QDir().mkdir(folder));
    QVERIFY(QDir().mkdir(cache));
    for (int i = 0; i < 2; ++i) {
        QImage image(80 + i * 10, 40, QImage::Format_RGB32);
        image.fill(Qt::white);
        QVERIFY(image.save(QDir(folder).filePath(QStringLiteral("%1.png").arg(i + 1)), "PNG"));
    }
    QString error;
    const auto source = LocalOcrSource::read(folder, 3, { }, &error);
    QVERIFY2(source.has_value(), qPrintable(error));
    const auto settings = persistenceSettings(temporary.path());
    auto spec = persistenceSpec(temporary.path(), settings);
    spec.sourceSnapshot = source->fingerprint;
    spec.sourceContext = { { "path", source->manifest["path"] }, { "sourceKind", "folder" }, { "libraryRoot", temporary.path() } };
    OcrJobs::Store store;
    QVERIFY(store.open(temporary.filePath("ocr-jobs.sqlite")));
    const auto id = store.enqueue(spec);
    QVERIFY(id.has_value());
    const auto lease = store.claim(*id, QStringLiteral("synthetic-owner"), 1000, 1000);
    QVERIFY(lease.has_value());
    const auto evidenceFor = [](const QImage &image, int selectedIndex, const OcrOptions &options) {
        const auto prepared = prepareOcrPage(image, options);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        prepared.image.save(&buffer, "PNG");
        const auto hash = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
        const QByteArray raw = R"({"version":1,"engine":"paddle-regions","device":"cpu","language":"auto","lines":[]})";
        return NeuralPageEvidence { selectedIndex, prepared.geometry, hash(png), raw, hash(raw), QStringLiteral("cpu"), QStringLiteral("cpu") };
    };
    QString firstKey;
    const bool orphanMode = mode.startsWith(QStringLiteral("orphan-"));
    if (mode != QStringLiteral("fresh") && !orphanMode) {
        const auto saved = LocalOcrPersistence::recordPage(store, *lease, settings, evidenceFor(source->source.pages[0].image, 0, { }), cache, { }, [] { return 1001; });
        QVERIFY2(saved.receiptSaved, qPrintable(saved.error));
        firstKey = saved.cacheKey;
    }
    if (orphanMode) {
        auto first = evidenceFor(source->source.pages[0].image, 0, { });
        first.rawResult.replace("\"lines\":[]", "\"elapsedMs\":7,\"lines\":[]");
        first.resultSha256 = QString::fromLatin1(QCryptographicHash::hash(first.rawResult, QCryptographicHash::Sha256).toHex());
        const auto identity = LocalOcrPersistence::identity(settings, first, &error);
        QVERIFY(identity.has_value());
        QVERIFY(LocalOcrCache::save(cache, *identity, first.rawResult, &error));
        firstKey = LocalOcrCache::key(*identity);
        if (mode == QStringLiteral("orphan-all")) {
            const auto second = evidenceFor(source->source.pages[1].image, 1, { });
            const auto other = LocalOcrPersistence::identity(settings, second, &error);
            QVERIFY(other.has_value());
            QVERIFY(LocalOcrCache::save(cache, *other, second.rawResult, &error));
        } else if (mode == QStringLiteral("orphan-ambiguous")) {
            first.requestedDevice = first.actualDevice = QStringLiteral("gpu:0");
            first.rawResult.replace("cpu", "gpu:0");
            first.resultSha256 = QString::fromLatin1(QCryptographicHash::hash(first.rawResult, QCryptographicHash::Sha256).toHex());
            const auto other = LocalOcrPersistence::identity(settings, first, &error);
            QVERIFY(other.has_value());
            QVERIFY(LocalOcrCache::save(cache, *other, first.rawResult, &error));
        }
        QVERIFY(store.get(*id)->pages.isEmpty()); // Model output exists, DB receipt does not.
    }
    if (mode == QStringLiteral("all-recorded"))
        QVERIFY(LocalOcrPersistence::recordPage(store, *lease, settings, evidenceFor(source->source.pages[1].image, 1, { }), cache, { }, [] { return 1001; }).receiptSaved);
    if (mode == QStringLiteral("damaged-cache") || mode == QStringLiteral("orphan-damaged")) {
        QFile file(QDir(cache).filePath(firstKey + QStringLiteral(".json")));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write("bad cache"), qint64(9));
    }
    if (mode == QStringLiteral("stale-owner")) {
        QVERIFY(store.pause(*id));
        QVERIFY(store.resume(*id));
        QVERIFY(store.claim(*id, QStringLiteral("successor"), 1002, 1000));
    }
    auto currentSource = *source;
    auto currentSettings = settings;
    if (mode == QStringLiteral("changed-source"))
        currentSource.fingerprint = QString(64, u'f');
    if (mode == QStringLiteral("changed-settings")) {
        auto environment = currentSettings.settingsSnapshot["environment"].toObject();
        environment["applicationSha256"] = QString(64, u'f');
        currentSettings.settingsSnapshot["environment"] = environment;
        currentSettings.settingsFingerprint = OcrJobs::settingsFingerprint(currentSettings.settingsSnapshot);
    }
    auto cancel = std::make_shared<std::atomic_bool>(false);
    qint64 now = 1003;
    int calls = 0;
    QVector<int> widths;
    bool correctOptions = true;
    const auto out = LocalOcrExecutor::executeClaimed(store, *lease, currentSource, currentSettings, cache, cancel, { }, [&] { return now; }, [&](const QVector<QImage> &images, const OcrOptions &options, const Cancellation &flag, const Progress &progress, const PageSink &sink) {
                ++calls;
                correctOptions = options.neural && options.gpu && options.cpuThreads == 8 && options.language == QStringLiteral("auto");
                RecognitionBatch result;
                result.readings.resize(images.size());
                result.evidence.resize(images.size());
                result.validPages.fill(false, images.size());
                result.status = RecognitionStatus::Complete;
                if (mode == QStringLiteral("lease-expired")) {
                    now = 400000;
                    progress(0, images.size(), QStringLiteral("synthetic lease expiry"));
                    result.status = RecognitionStatus::Cancelled;
                    result.error = QStringLiteral("synthetic cancellation");
                    return result;
                }
                for (int i = 0; i < images.size(); ++i) {
                    widths.append(images[i].width());
                    auto page = evidenceFor(images[i], i, options);
                    if (mode == QStringLiteral("page-candidate")) {
                        page.rawResult = R"({"version":1,"engine":"paddle-regions","device":"cpu","language":"auto","lines":[{"text":"Author: Alice Example","confidence":99,"box":[1,1,150,20],"language":"jpn"}]})";
                        page.resultSha256 = QString::fromLatin1(QCryptographicHash::hash(page.rawResult, QCryptographicHash::Sha256).toHex());
                    }
                    if (mode == QStringLiteral("changed-delivery"))
                        page.imageSha256 = QString(64, u'f');
                    result.evidence[i] = page;
                    result.readings[i] = parseNeuralReading(page.rawResult, page.geometry.preparedSize);
                    result.readings[i].text = QStringLiteral("Author: Unverified Runner Text");
                    result.validPages[i] = true;
                    QString error;
                    if (mode != QStringLiteral("missing-delivery") && !sink(page, &error)) {
                        result.status = RecognitionStatus::DeliveryFailed;
                        result.error = error;
                        break;
                    }
                    if (mode == QStringLiteral("changed-completion")) {
                        auto altered = page;
                        altered.rawResult = R"({"version":1,"engine":"paddle-regions","device":"cpu","language":"auto","lines":[{"text":"Author: Altered Completion","confidence":99,"box":[1,1,150,20],"language":"jpn"}]})";
                        altered.resultSha256 = QString::fromLatin1(QCryptographicHash::hash(altered.rawResult, QCryptographicHash::Sha256).toHex());
                        result.evidence[i] = altered; // Valid, but not the accepted receipt.
                    }
                    if (mode == QStringLiteral("callback-throw"))
                        throw std::runtime_error("synthetic runner failure after receipt");
                    if (mode == QStringLiteral("cancel")) {
                        flag->store(true);
                        result.status = RecognitionStatus::Cancelled;
                        result.error = QStringLiteral("synthetic cancellation");
                        break;
                    }
                    progress(i + 1, images.size(), QStringLiteral("synthetic page"));
                }
                if (mode == QStringLiteral("worker-failure")) {
                    result.status = RecognitionStatus::Failed;
                    result.error = QStringLiteral("synthetic nonzero exit after all outputs");
                }
                return result; });
    QVERIFY(correctOptions);
    const bool rejected = mode == QStringLiteral("orphan-all") || mode == QStringLiteral("orphan-damaged") || mode == QStringLiteral("orphan-ambiguous") || mode == QStringLiteral("changed-source") || mode == QStringLiteral("changed-settings") || mode == QStringLiteral("damaged-cache") || mode == QStringLiteral("all-recorded") || mode == QStringLiteral("stale-owner");
    QCOMPARE(calls, rejected ? 0 : 1);
    if (!rejected && mode != QStringLiteral("lease-expired"))
        QCOMPARE(widths, mode == QStringLiteral("fresh") ? QVector<int>({ 80, 90 }) : QVector<int>({ 90 }));
    QVERIFY(out.state.has_value());
    const auto savedJob = store.get(*id);
    QVERIFY(savedJob.has_value());
    if (mode == QStringLiteral("fresh") || mode == QStringLiteral("resume") || mode == QStringLiteral("page-candidate") || mode == QStringLiteral("orphan-first")) {
        QCOMPARE(out.batch.status, RecognitionStatus::Complete);
        QCOMPARE(*out.state, mode == QStringLiteral("page-candidate") ? OcrJobs::State::PageReview : OcrJobs::State::FilenameReview);
        QCOMPARE(out.cachedPages, mode == QStringLiteral("fresh") ? 0 : 1);
        QCOMPARE(out.cacheHits, mode == QStringLiteral("fresh") ? QVector<bool>({ false, false }) : QVector<bool>({ true, false }));
        QCOMPARE(out.recoveredUnrecordedPages, mode == QStringLiteral("orphan-first") ? 1 : 0);
        if (mode == QStringLiteral("orphan-first"))
            QCOMPARE(out.batch.readings[0].elapsedMs, qint64(7)); // Preserve the cached measurement, do not relabel it as fresh OCR.
        QVERIFY(out.stateSaved && out.stateError.isEmpty());
        QVERIFY(out.metadata.error.isEmpty());
        if (mode == QStringLiteral("page-candidate")) {
            QVERIFY(out.metadata.pages[1].text.contains(QStringLiteral("Alice Example")));
            bool author = false;
            for (const auto &candidate : out.metadata.suggestions)
                author = author || (candidate.field == Suggestion::Author && candidate.value == QStringLiteral("Alice Example") && candidate.page > 0);
            QVERIFY(author);
        } else {
            QVERIFY(out.metadata.pages[1].text.isEmpty());
        }
        QCOMPARE(savedJob->pages.size(), 2);
    } else if (mode == QStringLiteral("cancel")) {
        QCOMPARE(out.batch.status, RecognitionStatus::Cancelled);
        QCOMPARE(*out.state, OcrJobs::State::Paused);
        QVERIFY(out.stateSaved);
        QCOMPARE(savedJob->pages.size(), 2);
    } else if (mode == QStringLiteral("lease-expired") || mode == QStringLiteral("stale-owner")) {
        QCOMPARE(*out.state, OcrJobs::State::Running);
        QVERIFY(!out.stateSaved && !out.stateError.isEmpty());
        QCOMPARE(savedJob->pages.size(), 1);
    } else {
        QCOMPARE(*out.state, OcrJobs::State::Failed);
        QVERIFY(out.stateSaved);
        QVERIFY(!out.batch.error.isEmpty());
        if (mode == QStringLiteral("callback-throw"))
            QCOMPARE(out.batch.status, RecognitionStatus::CleanupFailed);
        else if (mode == QStringLiteral("missing-delivery") || mode == QStringLiteral("changed-delivery") || mode == QStringLiteral("changed-completion"))
            QCOMPARE(out.batch.status, RecognitionStatus::DeliveryFailed);
        else
            QCOMPARE(out.batch.status, RecognitionStatus::Failed);
        const int expectedPages = orphanMode ? (mode == QStringLiteral("orphan-all") ? 2 : 0) : (mode == QStringLiteral("worker-failure") || mode == QStringLiteral("all-recorded") || mode == QStringLiteral("callback-throw") || mode == QStringLiteral("changed-completion") ? 2 : 1);
        QCOMPARE(savedJob->pages.size(), expectedPages);
    }
    if (mode == QStringLiteral("orphan-all")) {
        QCOMPARE(out.cachedPages, 2);
        QCOMPARE(out.recoveredUnrecordedPages, 2);
        QCOMPARE(out.batch.status, RecognitionStatus::Failed); // Cache pages alone do not prove a prior session succeeded.
    }
    if (mode == QStringLiteral("orphan-ambiguous"))
        QCOMPARE(QDir(cache).entryList(QDir::Files).size(), 2);
    if (mode == QStringLiteral("changed-delivery") || mode == QStringLiteral("changed-completion"))
        QVERIFY(out.metadata.pages[1].text.isEmpty()); // Invalid evidence never supplies candidate text.
    if (mode == QStringLiteral("damaged-cache") || mode == QStringLiteral("orphan-damaged")) {
        QFile file(QDir(cache).filePath(firstKey + QStringLiteral(".json")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("bad cache"));
    }
    const auto unchanged = LocalOcrSource::read(folder, 3, { }, &error);
    QVERIFY(unchanged.has_value());
    QCOMPARE(unchanged->fingerprint, source->fingerprint);
}

void LocalMetadataTest::repeatedExecutorInputs_data()
{
    QTest::addColumn<QString>("mode");
    for (const auto &mode : { "fresh", "with-cache", "cancel", "lease-expired", "distinct-pixels" })
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

void LocalMetadataTest::repeatedExecutorInputs()
{
    using namespace LocalMetadata;
    QFETCH(QString, mode);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto folder = temporary.filePath("repeated-inputs");
    const auto cache = temporary.filePath("cache");
    QVERIFY(QDir().mkdir(folder));
    QVERIFY(QDir().mkdir(cache));
    for (int i = 0; i < 3; ++i) {
        QImage image(i == 1 ? 90 : 80, 40, QImage::Format_RGB32);
        image.fill(i == 2 && mode == QStringLiteral("distinct-pixels") ? Qt::black : Qt::white);
        QVERIFY(image.save(QDir(folder).filePath(QStringLiteral("%1.png").arg(i + 1)), "PNG"));
    }
    QString error;
    const auto source = LocalOcrSource::read(folder, 3, { }, &error);
    QVERIFY2(source.has_value(), qPrintable(error));
    const auto runtime = persistenceSettings(temporary.path());
    auto spec = persistenceSpec(temporary.path(), runtime);
    spec.sourceSnapshot = source->fingerprint;
    spec.sourceContext = { { "path", source->manifest["path"] }, { "sourceKind", "folder" }, { "libraryRoot", temporary.path() } };
    spec.totalPages = 3;
    spec.pages = { 1, 2, 3 };
    OcrJobs::Store store;
    QVERIFY(store.open(temporary.filePath("ocr-jobs.sqlite")));
    const auto id = store.enqueue(spec);
    QVERIFY(id.has_value());
    const auto lease = store.claim(*id, QStringLiteral("repeated-owner"), 1000, 1000);
    QVERIFY(lease.has_value());
    const auto evidenceFor = [](const QImage &image, int index, int elapsed) {
        const auto prepared = prepareOcrPage(image, { });
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        prepared.image.save(&buffer, "PNG");
        const auto hash = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
        const auto raw = QByteArray(R"({"version":1,"engine":"paddle-regions","device":"cpu","language":"auto","elapsedMs":)") + QByteArray::number(elapsed) + QByteArray(R"(,"lines":[]})");
        return NeuralPageEvidence { index, prepared.geometry, hash(png), raw, hash(raw), QStringLiteral("cpu"), QStringLiteral("cpu") };
    };
    if (mode == QStringLiteral("with-cache"))
        QVERIFY(LocalOcrPersistence::recordPage(store, *lease, runtime, evidenceFor(source->source.pages[1].image, 1, 42), cache, { }, [] { return 1001; }).receiptSaved);
    qint64 now = 1002;
    bool writing = false;
    int writes = 0;
    const auto clock = [&] {
        if (writing && ++writes == 2 && mode == QStringLiteral("lease-expired"))
            now = 400000;
        return now;
    };
    QVector<int> widths, progressCounts;
    int calls = 0;
    const LocalOcrExecutor::Runner runner = [&](const QVector<QImage> &images, const OcrOptions &, const Cancellation &flag, const Progress &progress, const PageSink &sink) {
        ++calls;
        widths.clear();
        for (const auto &image : images)
            widths.append(image.width());
        RecognitionBatch result;
        result.readings.resize(images.size());
        result.evidence.resize(images.size());
        result.validPages.fill(false, images.size());
        result.status = RecognitionStatus::Complete;
        for (int i = 0; i < images.size(); ++i) {
            const auto page = evidenceFor(images[i], i, 7 + i);
            result.evidence[i] = page;
            result.readings[i] = parseNeuralReading(page.rawResult, page.geometry.preparedSize);
            result.validPages[i] = true;
            writing = true;
            writes = 0;
            QString delivery;
            const bool accepted = sink(page, &delivery);
            writing = false;
            if (!accepted) {
                result.status = RecognitionStatus::DeliveryFailed;
                result.error = delivery;
                break;
            }
            progress(i + 1, images.size(), QStringLiteral("synthetic repeated input"));
            if (mode == QStringLiteral("cancel")) {
                flag->store(true);
                result.status = RecognitionStatus::Cancelled;
                result.error = QStringLiteral("synthetic cancellation after repeated group");
                break;
            }
        }
        return result;
    };
    const auto out = LocalOcrExecutor::executeClaimed(store, *lease, *source, runtime, cache, { }, [&](int count, int total, const QString &) {
        QCOMPARE(total, 3);
        progressCounts.append(count); }, clock, runner);
    QCOMPARE(calls, 1);
    QCOMPARE(widths, mode == QStringLiteral("with-cache") ? QVector<int>({ 80 }) : mode == QStringLiteral("distinct-pixels") ? QVector<int>({ 80, 90, 80 })
                                                                                                                             : QVector<int>({ 80, 90 }));
    QCOMPARE(out.repeatedInputPages, mode == QStringLiteral("distinct-pixels") ? 0 : 1);
    QCOMPARE(out.cachedPages, mode == QStringLiteral("with-cache") ? 1 : 0);
    const auto saved = store.get(*id);
    QVERIFY(saved.has_value());
    if (mode == QStringLiteral("lease-expired")) {
        QCOMPARE(out.batch.status, RecognitionStatus::DeliveryFailed);
        QVERIFY(!out.stateSaved);
        QCOMPARE(saved->state, OcrJobs::State::Running);
        QCOMPARE(saved->pages.size(), 1); // The second receipt in the group expired.
        QCOMPARE(saved->pages[0].page, 1);
        QVERIFY(store.interruptExpired(now));
        QVERIFY(store.resume(*id));
        const auto next = store.claim(*id, QStringLiteral("repeated-successor"), now + 1, 1000);
        QVERIFY(next.has_value());
        now += 2;
        const auto resumed = LocalOcrExecutor::executeClaimed(store, *next, *source, runtime, cache, { }, { }, clock, runner);
        QCOMPARE(resumed.batch.status, RecognitionStatus::Complete);
        QVERIFY(resumed.stateSaved);
        QCOMPARE(widths, QVector<int>({ 90 }));
        QCOMPARE(resumed.cachedPages, 2);
        QCOMPARE(resumed.recoveredUnrecordedPages, 1);
        QCOMPARE(resumed.cacheHits, QVector<bool>({ true, false, true }));
        QCOMPARE(store.get(*id)->pages.size(), 3);
    } else if (mode == QStringLiteral("cancel")) {
        QCOMPARE(out.batch.status, RecognitionStatus::Cancelled);
        QVERIFY(out.stateSaved);
        QCOMPARE(saved->state, OcrJobs::State::Paused);
        QCOMPARE(saved->pages.size(), 2);
        QCOMPARE(saved->pages[0].page, 1);
        QCOMPARE(saved->pages[1].page, 3);
        QCOMPARE(progressCounts.last(), 2); // Selected pages, not number of unique inferences.
    } else {
        QCOMPARE(out.batch.status, RecognitionStatus::Complete);
        QVERIFY(out.stateSaved);
        QCOMPARE(saved->state, OcrJobs::State::FilenameReview);
        QCOMPARE(saved->pages.size(), 3);
        QCOMPARE(progressCounts.last(), 3);
        QCOMPARE(out.batch.validPages, QVector<bool>({ true, true, true }));
        if (mode != QStringLiteral("distinct-pixels")) {
            QCOMPARE(out.batch.evidence[0]->rawResult, out.batch.evidence[2]->rawResult);
            QCOMPARE(out.batch.evidence[2]->selectedIndex, 2);
            QCOMPARE(saved->pages[0].cacheKey, saved->pages[2].cacheKey);
            QVERIFY(!out.cacheHits[0] && !out.cacheHits[2]); // A repeated current input is not an old cache hit.
        }
    }
    const auto unchanged = LocalOcrSource::read(folder, 3, { }, &error);
    QVERIFY(unchanged.has_value());
    QCOMPARE(unchanged->fingerprint, source->fingerprint);
}

void LocalMetadataTest::selectedSession_data()
{
    QTest::addColumn<QString>("mode");
    for (const auto &mode : { "fresh", "resume", "paused-start", "failed", "running", "changed-settings", "changed-source", "changed-library", "missing", "locked", "cancel-before", "cancel-preflight", "private-data-library", "corrupt-cache", "start-completed" })
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

void LocalMetadataTest::selectedSession()
{
#ifndef Q_OS_WIN
    QSKIP("Personal Windows selected OCR session");
#else
    using namespace LocalMetadata;
    using LocalOcrSession::Action;
    QFETCH(QString, mode);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath("library");
    const auto source = root + QStringLiteral("/selected");
    const auto data = root + QStringLiteral("/.yacreaderlibrary");
    const auto database = data + QStringLiteral("/library.ydb");
    const auto runtime = temporary.filePath("runtime");
    QVERIFY(QDir().mkpath(source));
    QVERIFY(QDir().mkdir(data));
    const auto write = [](const QString &path, const QByteArray &bytes) {
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            return false;
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
    };
    const auto hash = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
    QVERIFY(write(runtime + "/app.exe", "synthetic application; must never execute"));
    QVERIFY(write(runtime + "/Qt6Core.dll", "synthetic shared dependency"));
    QVERIFY(write(runtime + "/ocr-neural/worker.py", "synthetic worker; must never execute"));
    QVERIFY(write(runtime + "/ocr-neural/runtime/python.exe", "synthetic CPU interpreter; must never execute"));
    QJsonArray declaration;
    for (const QString &model : { QStringLiteral("PP-OCRv5_mobile_det"), QStringLiteral("PP-OCRv5_server_rec"), QStringLiteral("korean_PP-OCRv5_mobile_rec") })
        for (const QString &name : { QStringLiteral("inference.json"), QStringLiteral("inference.pdiparams"), QStringLiteral("inference.yml") }) {
            const QString relative = QStringLiteral("models/") + model + u'/' + name;
            const auto bytes = relative.toUtf8();
            QVERIFY(write(runtime + "/ocr-neural/" + relative, bytes));
            declaration.append(QJsonObject { { "path", relative }, { "sha256", hash(bytes) } });
        }
    QVERIFY(write(runtime + "/ocr-neural/models.json", QJsonDocument(declaration).toJson()));
    for (int i = 0; i < 2; ++i) {
        QImage image(80 + i * 10, 40, QImage::Format_RGB32);
        image.fill(Qt::white);
        QVERIFY(image.save(QDir(source).filePath(QStringLiteral("%1.png").arg(i + 1)), "PNG"));
    }
    const auto sql = [&](const QString &statement) {
        const auto connection = QUuid::createUuid().toString();
        bool ok = false;
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(database);
            if (db.open()) {
                QSqlQuery query(db);
                ok = query.exec(statement);
            }
        }
        QSqlDatabase::removeDatabase(connection);
        return ok;
    };
    QVERIFY(sql("CREATE TABLE comic(id INTEGER, comicInfoId INTEGER, path TEXT, title TEXT)"));
    QVERIFY(sql("INSERT INTO comic VALUES(1,42,'/selected','Initial')"));

    const auto bytes = [](const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); };
    const auto preserved = [&] {
        QMap<QString, QByteArray> result;
        for (const auto &directory : { root, runtime }) {
            QDirIterator it(directory, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const auto p = it.next();
                result[p] = bytes(p);
            }
        }
        return result;
    };
    auto before = preserved();
    LocalOcrSession::Request request;
    request.libraryRoot = root;
    request.comicInfoId = 42;
    request.sourcePath = source;
    request.applicationPath = runtime + "/app.exe";
    request.dataRoot = temporary.filePath("private-work");
    request.options.neural = true;
    request.options.gpu = false;
    auto flag = std::make_shared<std::atomic_bool>(false);
    bool pauseFirst = mode == "resume" || mode == "paused-start" || mode == "changed-settings" || mode == "changed-source";
    bool failFirst = mode == "failed";
    int calls = 0;
    QVector<int> inferred;
    bool deliveryOk = true;
    const auto runner = [&](const QVector<QImage> &images, const OcrOptions &options, const Cancellation &cancel, const Progress &, const PageSink &sink) {
        ++calls;
        RecognitionBatch result;
        result.readings.resize(images.size());
        result.evidence.resize(images.size());
        result.validPages.fill(false, images.size());
        result.status = RecognitionStatus::Complete;
        for (int i = 0; i < images.size(); ++i) {
            inferred.append(images[i].width());
            const auto prepared = prepareOcrPage(images[i], options);
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            prepared.image.save(&buffer, "PNG");
            const QByteArray raw = R"({"version":1,"engine":"paddle-regions","device":"cpu","language":"auto","elapsedMs":7,"lines":[]})";
            const NeuralPageEvidence page { i, prepared.geometry, hash(png), raw, hash(raw), QStringLiteral("cpu"), QStringLiteral("cpu") };
            result.evidence[i] = page;
            result.validPages[i] = true;
            result.readings[i] = parseNeuralReading(raw, prepared.geometry.preparedSize);
            QString error;
            if (!sink(page, &error)) {
                deliveryOk = false;
                result.status = RecognitionStatus::DeliveryFailed;
                result.error = error;
                break;
            }
            if (pauseFirst) {
                cancel->store(true);
                result.status = RecognitionStatus::Cancelled;
                result.error = "Synthetic clean cancellation";
                break;
            }
        }
        if (failFirst) {
            result.status = RecognitionStatus::Failed;
            result.error = "Synthetic exit failure after all outputs";
        }
        return result;
    };
    const auto run = [&](Action action, const Progress &progress = Progress()) { return LocalOcrSession::run(request, action, flag, progress, { }, runner); };
    const auto dbPath = QDir(request.dataRoot).filePath("ocr-jobs.sqlite");
    if (mode == "missing" || mode == "cancel-before" || mode == "cancel-preflight" || mode == "private-data-library" || mode == "locked") {
        std::unique_ptr<QLockFile> lock;
        if (mode == "locked") {
            QVERIFY(QDir().mkpath(request.dataRoot));
            lock = std::make_unique<QLockFile>(QDir(request.dataRoot).filePath("selected-session.lock"));
            lock->setStaleLockTime(0);
            QVERIFY(lock->tryLock(0));
        }
        if (mode == "private-data-library")
            request.dataRoot = root + "/should-not-create";
        if (mode == "cancel-before")
            flag->store(true);
        const auto result = run(mode == "missing" ? Action::Continue : Action::Start, [&](int step, int, const QString &) { if (mode == "cancel-preflight" && step == 1) flag->store(true); });
        QVERIFY(!result.complete);
        QVERIFY(!result.error.isEmpty());
        QCOMPARE(calls, 0);
        QVERIFY(!QFile::exists(dbPath));
        QCOMPARE(preserved(), before);
        return;
    }
    if (mode == "running") {
        QString error;
        const auto prepared = LocalOcrPreflight::read(root, 42, source, 3, request.applicationPath, request.options, &error);
        QVERIFY2(prepared.has_value(), qPrintable(error));
        QVERIFY(QDir().mkpath(request.dataRoot));
        {
            OcrJobs::Store store;
            QVERIFY(store.open(dbPath));
            const auto id = store.enqueue(prepared->spec);
            QVERIFY(id);
            QVERIFY(store.claim(*id, "other-synthetic-owner", 1, 1));
        }
        const auto result = run(Action::Continue);
        QVERIFY(!result.complete);
        QCOMPARE(result.state, std::optional<OcrJobs::State>(OcrJobs::State::Running));
        QVERIFY(!result.error.isEmpty());
        QCOMPARE(calls, 0);
        QCOMPARE(preserved(), before);
        return; // Even an expired Running lease is never stolen by this UI adapter.
    }
    const auto first = run(Action::Start);
    QVERIFY2(deliveryOk, qPrintable(first.error));
    QVERIFY(first.library.has_value());
    QVERIFY(!first.jobId.isEmpty());
    QCOMPARE(first.complete, !pauseFirst && !failFirst);
    QCOMPARE(calls, 1);
    QCOMPARE(preserved(), before);
    if (pauseFirst)
        QCOMPARE(first.state, std::optional<OcrJobs::State>(OcrJobs::State::Paused));
    if (failFirst)
        QCOMPARE(first.state, std::optional<OcrJobs::State>(OcrJobs::State::Failed));
    if (mode == "changed-settings")
        request.options.cpuThreads = 4;
    if (mode == "changed-source") {
        QImage changed(80, 40, QImage::Format_RGB32);
        changed.fill(Qt::red);
        QVERIFY(changed.save(source + "/1.png"));
        before = preserved();
    }
    if (mode == "changed-library") {
        QVERIFY(sql("UPDATE comic SET id=2"));
        before = preserved();
    }
    if (mode == "corrupt-cache") {
        const auto entries = QDir(QDir(request.dataRoot).filePath("pages")).entryList({ "*.json" }, QDir::Files);
        QCOMPARE(entries.size(), 2);
        QVERIFY(write(QDir(request.dataRoot).filePath("pages/" + entries.first()), "invalid evidence"));
    }
    flag = std::make_shared<std::atomic_bool>(false);
    pauseFirst = false;
    failFirst = false;
    const bool success = mode == "fresh" || mode == "resume" || mode == "start-completed";
    const auto second = run(mode == "paused-start" || mode == "start-completed" ? Action::Start : Action::Continue);
    QCOMPARE(second.complete, success);
    QCOMPARE(second.error.isEmpty(), success);
    QCOMPARE(calls, mode == "resume" ? 2 : 1);
    if (success) {
        QCOMPARE(second.cachedPages, mode == "resume" ? 1 : 2);
        QCOMPARE(second.restored, mode != "resume");
        QCOMPARE(second.state, std::optional<OcrJobs::State>(OcrJobs::State::FilenameReview));
        QCOMPARE(second.metadata.pages.size(), 2);
        for (const auto &page : second.metadata.pages) {
            QCOMPARE(page.reading.elapsedMs, qint64(7));
            QCOMPARE(page.reading.device, QStringLiteral("cpu"));
        }
        QCOMPARE(inferred, QVector<int>({ 80, 90 }));
    }
    {
        OcrJobs::Store store;
        QVERIFY(store.open(dbPath));
        QCOMPARE(store.list().size(), 1);
        const auto job = store.get(first.jobId);
        QVERIFY(job);
        if (!success)
            QCOMPARE(job->state, *first.state);
    }
    QCOMPARE(preserved(), before); // No original or library metadata writes, including rejection paths.
#endif
}

void LocalMetadataTest::selectedPreflight_data()
{
    QTest::addColumn<QString>("mode");
    for (const auto &mode : { "valid", "metadata-edit", "row-change", "database-replace", "source-path-change", "missing-library", "bad-runtime", "bad-source", "bad-page-limit", "classic-mode", "cancel-before", "cancel-during", "callback-throw" })
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

void LocalMetadataTest::selectedPreflight()
{
#ifndef Q_OS_WIN
    QSKIP("Personal Windows selected-work preflight");
#else
    using namespace LocalMetadata;
    QFETCH(QString, mode);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath("library");
    const auto source = root + QStringLiteral("/selected");
    const auto data = root + QStringLiteral("/.yacreaderlibrary");
    const auto database = data + QStringLiteral("/library.ydb");
    const auto runtime = temporary.filePath("runtime");
    QVERIFY(QDir().mkpath(source));
    QVERIFY(QDir().mkdir(data));
    const auto write = [](const QString &path, const QByteArray &bytes) {
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            return false;
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
    };
    const auto hash = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
    QVERIFY(write(runtime + "/app.exe", "synthetic application; must never execute"));
    QVERIFY(write(runtime + "/Qt6Core.dll", "synthetic shared dependency"));
    QVERIFY(write(runtime + "/ocr-neural/worker.py", "synthetic worker; must never execute"));
    QVERIFY(write(runtime + "/ocr-neural/runtime/python.exe", "synthetic CPU interpreter; must never execute"));
    QJsonArray declaration;
    for (const QString &model : { QStringLiteral("PP-OCRv5_mobile_det"), QStringLiteral("PP-OCRv5_server_rec"), QStringLiteral("korean_PP-OCRv5_mobile_rec") })
        for (const QString &name : { QStringLiteral("inference.json"), QStringLiteral("inference.pdiparams"), QStringLiteral("inference.yml") }) {
            const QString relative = QStringLiteral("models/") + model + u'/' + name;
            const auto bytes = relative.toUtf8();
            QVERIFY(write(runtime + "/ocr-neural/" + relative, bytes));
            declaration.append(QJsonObject { { "path", relative }, { "sha256", hash(bytes) } });
        }
    QVERIFY(write(runtime + "/ocr-neural/models.json", QJsonDocument(declaration).toJson()));
    for (int i = 0; i < 2; ++i) {
        QImage image(80 + i * 10, 40, QImage::Format_RGB32);
        image.fill(Qt::white);
        QVERIFY(image.save(QDir(source).filePath(QStringLiteral("%1.png").arg(i + 1)), "PNG"));
    }
    const auto sql = [&](const QString &statement) {
        const auto connection = QUuid::createUuid().toString();
        bool ok = false;
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(database);
            if (db.open()) {
                QSqlQuery query(db);
                ok = query.exec(statement);
            }
        }
        QSqlDatabase::removeDatabase(connection);
        return ok;
    };
    QVERIFY(sql("CREATE TABLE comic(id INTEGER, comicInfoId INTEGER, path TEXT, title TEXT)"));
    QVERIFY(sql("INSERT INTO comic VALUES(1,42,'/selected','Initial')"));
    if (mode == QStringLiteral("missing-library"))
        QVERIFY(QFile::remove(database));
    if (mode == QStringLiteral("bad-runtime"))
        QVERIFY(write(runtime + "/ocr-neural/models/PP-OCRv5_mobile_det/inference.pdiparams", "changed model"));
    if (mode == QStringLiteral("bad-source"))
        QVERIFY(write(source + "/1.png", "invalid encoded image"));
    const auto snapshot = [&] {
        QMap<QString, QByteArray> files;
        QDirIterator iterator(temporary.path(), QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const auto path = iterator.next();
            QFile file(path);
            files.insert(QDir(temporary.path()).relativeFilePath(path), file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("unreadable"));
        }
        return files;
    };
    auto expectedFiles = snapshot();
    QVERIFY(!expectedFiles.isEmpty());
    bool untouched = true;
    bool callbackOk = true;
    bool inventoryProgress = false;
    QVector<int> stages;
    const auto cancel = std::make_shared<std::atomic_bool>(mode == QStringLiteral("cancel-before"));
    OcrOptions options;
    options.neural = mode != QStringLiteral("classic-mode");
    options.gpu = true;
    const auto progress = [&](int step, int total, const QString &) {
        untouched = untouched && snapshot() == expectedFiles;
        if (total == 0) {
            inventoryProgress = true;
            callbackOk = callbackOk && step == 0;
            return; // Indeterminate inventory updates do not advance preparation.
        }
        callbackOk = callbackOk && total == 4;
        stages.append(step);
        if (step == 1 && mode == QStringLiteral("row-change"))
            callbackOk = callbackOk && sql("UPDATE comic SET id=2");
        if (step == 1 && mode == QStringLiteral("metadata-edit"))
            callbackOk = callbackOk && sql("UPDATE comic SET title='Edited metadata'");
        if (step == 3 && mode == QStringLiteral("source-path-change"))
            callbackOk = callbackOk && sql("UPDATE comic SET path='/different'");
        if (step == 3 && mode == QStringLiteral("database-replace"))
            callbackOk = callbackOk && QFile::copy(database, data + "/replacement.ydb") && QFile::rename(database, data + "/original.ydb") && QFile::rename(data + "/replacement.ydb", database);
        expectedFiles = snapshot(); // Only these explicit fixture edits may change bytes.
        if (step == 1 && mode == QStringLiteral("cancel-during"))
            cancel->store(true);
        if (step == 1 && mode == QStringLiteral("callback-throw"))
            throw std::runtime_error("synthetic preflight progress failure");
    };
    QString error;
    const auto prepared = LocalOcrPreflight::read(root, 42, source, mode == QStringLiteral("bad-page-limit") ? 4 : 3,
                                                  runtime + "/app.exe", options, &error, cancel, progress);
    const bool success = mode == QStringLiteral("valid") || mode == QStringLiteral("metadata-edit");
    QCOMPARE(prepared.has_value(), success);
    QCOMPARE(error.isEmpty(), success);
    QVERIFY(untouched && callbackOk);
    QCOMPARE(snapshot(), expectedFiles);
    QVERIFY(!QFile::exists(temporary.filePath("ocr-jobs.sqlite")));
    if (prepared) {
        QVERIFY(inventoryProgress);
        QCOMPARE(stages, QVector<int>({ 0, 1, 2, 3 }));
        QCOMPARE(prepared->spec.libraryGeneration, prepared->library.generation);
        QCOMPARE(prepared->spec.comicId, QStringLiteral("1"));
        QCOMPARE(prepared->spec.sourceSnapshot, prepared->source.fingerprint);
        QCOMPARE(prepared->spec.settingsFingerprint, OcrJobs::settingsFingerprint(prepared->spec.settingsSnapshot));
        QCOMPARE(prepared->spec.pages, QVector<int>({ 1, 2 }));
        QCOMPARE(prepared->spec.totalPages, 2);
        QVERIFY(prepared->runtime.settingsSnapshot["environment"].toObject()["gpu"].isNull());
        OcrJobs::Store store;
        QVERIFY(store.open(temporary.filePath("ocr-jobs.sqlite")));
        const auto id = store.enqueue(prepared->spec);
        QVERIFY2(id.has_value(), qPrintable(store.lastError())); // Caller can use this complete specification explicitly.
        QCOMPARE(store.get(*id)->state, OcrJobs::State::Queued);
        QVERIFY(store.get(*id)->pages.isEmpty());
    }
#endif
}

void LocalMetadataTest::completedReview_data()
{
    QTest::addColumn<QString>("mode");
    for (const auto &mode : { "filename", "page", "failed", "paused", "missing-receipt", "duplicate-receipt", "corrupt-cache", "changed-input", "changed-settings", "changed-library", "changed-comic", "changed-path", "wrong-classification", "cancel" })
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

void LocalMetadataTest::completedReview()
{
    using namespace LocalMetadata;
    QFETCH(QString, mode);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto folder = temporary.filePath("synthetic-review");
    const auto cache = temporary.filePath("cache");
    const auto database = temporary.filePath("ocr-jobs.sqlite");
    QVERIFY(QDir().mkdir(folder));
    QVERIFY(QDir().mkdir(cache));
    for (int i = 0; i < 2; ++i) {
        QImage image(320 + i * 10, 96, QImage::Format_RGB32);
        image.fill(Qt::white);
        QVERIFY(image.save(QDir(folder).filePath(QStringLiteral("%1.png").arg(i + 1)), "PNG"));
    }
    QString error;
    auto source = LocalOcrSource::read(folder, 3, { }, &error);
    QVERIFY2(source.has_value(), qPrintable(error));
    auto settings = persistenceSettings(temporary.path());
    auto spec = persistenceSpec(temporary.path(), settings);
    spec.sourceSnapshot = source->fingerprint;
    spec.sourceContext = { { "path", source->manifest["path"] }, { "sourceKind", "folder" }, { "libraryRoot", temporary.path() } };
    LocalOcrLibrary::Binding library;
    library.generation = spec.libraryGeneration;
    library.comicId = spec.comicId;
    library.libraryRoot = temporary.path();
    library.sourcePath = source->manifest["path"].toString();
    OcrJobs::Store store;
    QVERIFY(store.open(database));
    const auto id = store.enqueue(spec);
    QVERIFY(id.has_value());
    const auto lease = store.claim(*id, QStringLiteral("synthetic-review-owner"), 1000, 1000);
    QVERIFY(lease.has_value());
    QVector<QByteArray> originals;
    const auto hash = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
    for (int i = 0; i < 2; ++i) {
        const auto prepared = prepareOcrPage(source->source.pages[i].image, { });
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(prepared.image.save(&buffer, "PNG"));
        const QByteArray raw = mode == QStringLiteral("page") && i == 1
                ? QByteArray(R"({"version":1,"engine":"paddle-regions","device":"cpu","language":"auto","elapsedMs":7,"lines":[{"text":"Author: Alice Example","confidence":99,"box":[1,1,250,20],"language":"jpn"}]})")
                : QByteArray(R"({"version":1,"engine":"paddle-regions","device":"cpu","language":"auto","elapsedMs":7,"lines":[]})");
        const NeuralPageEvidence page { i, prepared.geometry, hash(png), raw, hash(raw), QStringLiteral("cpu"), QStringLiteral("cpu") };
        originals.append(raw);
        const auto saved = LocalOcrPersistence::recordPage(store, *lease, settings, page, cache, { }, [] { return 1001; });
        QVERIFY2(saved.receiptSaved, qPrintable(saved.error));
    }
    if (mode == QStringLiteral("failed"))
        QVERIFY(store.fail(*lease, QStringLiteral("nonzero after all pages"), 1002));
    else if (mode == QStringLiteral("paused"))
        QVERIFY(store.pauseWhenCurrent(*lease, [] { return 1002; }));
    else
        QVERIFY(store.finish(*lease, mode == QStringLiteral("page"), 1002));
    auto job = store.get(*id);
    QVERIFY(job.has_value());
    const auto savedState = job->state;
    if (mode == QStringLiteral("missing-receipt"))
        job->pages.removeLast();
    if (mode == QStringLiteral("duplicate-receipt"))
        job->pages[1] = job->pages[0];
    if (mode == QStringLiteral("corrupt-cache")) {
        QFile file(QDir(cache).filePath(job->pages[1].cacheKey + QStringLiteral(".json")));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write("corrupt review evidence"), qint64(23));
    }
    if (mode == QStringLiteral("changed-input"))
        source->source.pages[1].image.fill(Qt::black);
    if (mode == QStringLiteral("changed-settings")) {
        auto environment = settings.settingsSnapshot["environment"].toObject();
        environment["applicationSha256"] = QString(64, u'f');
        settings.settingsSnapshot["environment"] = environment;
        settings.settingsFingerprint = OcrJobs::settingsFingerprint(settings.settingsSnapshot);
    }
    if (mode == QStringLiteral("changed-library"))
        library.generation = QString(64, u'f');
    if (mode == QStringLiteral("changed-comic"))
        library.comicId += QStringLiteral("other");
    if (mode == QStringLiteral("changed-path"))
        library.sourcePath += QStringLiteral("other");
    if (mode == QStringLiteral("wrong-classification"))
        job->state = OcrJobs::State::PageReview;
    const auto snapshot = [&] {
        QVector<QByteArray> bytes;
        const auto files = QDir(cache).entryList(QDir::Files, QDir::Name);
        for (const auto &name : files) {
            QFile file(QDir(cache).filePath(name));
            if (!file.open(QIODevice::ReadOnly))
                return QVector<QByteArray> { };
            bytes.append(name.toUtf8());
            bytes.append(file.readAll());
        }
        QFile file(database);
        if (!file.open(QIODevice::ReadOnly))
            return QVector<QByteArray> { };
        bytes.append(file.readAll());
        return bytes;
    };
    const auto before = snapshot();
    QVERIFY(!before.isEmpty());
    const auto cancel = std::make_shared<std::atomic_bool>(mode == QStringLiteral("cancel"));
    const auto restored = LocalOcrExecutor::restoreReview(*job, library, *source, settings, cache, &error, cancel);
    const bool success = mode == QStringLiteral("filename") || mode == QStringLiteral("page");
    QCOMPARE(restored.has_value(), success);
    QCOMPARE(error.isEmpty(), success);
    QCOMPARE(snapshot(), before); // No orphan adoption, cache overwrite, job mutation or OCR output.
    QCOMPARE(store.get(*id)->state, savedState);
    QCOMPARE(store.get(*id)->pages.size(), 2);
    if (restored) {
        QCOMPARE(restored->savedState, savedState);
        QCOMPARE(restored->evidence.size(), 2);
        QVERIFY(restored->metadata.error.isEmpty());
        QVERIFY(restored->restoreMs >= 0);
        for (int i = 0; i < 2; ++i) {
            QCOMPARE(restored->evidence[i].rawResult, originals[i]);
            QCOMPARE(restored->metadata.pages[i].reading.elapsedMs, qint64(7));
            QCOMPARE(restored->metadata.pages[i].reading.device, QStringLiteral("cpu"));
        }
        bool author = false;
        for (const auto &candidate : restored->metadata.suggestions)
            author = author || (candidate.field == Suggestion::Author && candidate.value == QStringLiteral("Alice Example") && candidate.page > 0);
        QCOMPARE(author, mode == QStringLiteral("page"));
    }
    const auto unchanged = LocalOcrSource::read(folder, 3, { }, &error);
    QVERIFY(unchanged.has_value());
    QCOMPARE(unchanged->fingerprint, spec.sourceSnapshot);
}

void LocalMetadataTest::cacheReceiptOrdering()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    OcrJobs::Store store;
    QVERIFY(store.open(temporary.filePath("ocr-jobs.sqlite")));
    const auto settings = persistenceSettings(temporary.path());
    const auto id = store.enqueue(persistenceSpec(temporary.path(), settings));
    QVERIFY(id.has_value());
    const auto lease = store.claim(*id, QStringLiteral("synthetic-owner"), 1000, 100);
    QVERIFY(lease.has_value());
    const auto page = persistencePage();
    const auto cache = temporary.filePath("cache");
    QVERIFY(QDir().mkdir(cache));
    auto save = [&](const LocalOcrRuntime::Measurement &s, const LocalMetadata::NeuralPageEvidence &p, const QString &root, qint64 now) {
        return LocalOcrPersistence::recordPage(store, *lease, s, p, root, { }, [=] { return now; });
    };
    const auto missing = save(settings, page, temporary.filePath("missing-cache"), 1001);
    QVERIFY(!missing.cacheSaved && !missing.receiptSaved && !missing.error.isEmpty());
    QVERIFY(store.get(*id)->pages.isEmpty());
    auto wrong = settings;
    auto options = wrong.settingsSnapshot["options"].toObject();
    options["rotation"] = 90;
    wrong.settingsSnapshot["options"] = options;
    wrong.settingsFingerprint = OcrJobs::settingsFingerprint(wrong.settingsSnapshot);
    QVERIFY(!save(wrong, page, cache, 1001).cacheSaved);
    auto outside = page;
    outside.selectedIndex = 2;
    QVERIFY(!save(settings, outside, cache, 1001).cacheSaved);
    auto corrupt = page;
    corrupt.rawResult.append(' ');
    QVERIFY(!save(settings, corrupt, cache, 1001).cacheSaved);
    QVERIFY(QDir(cache).entryList(QDir::Files).isEmpty());
    const auto expired = save(settings, page, cache, 1101);
    QVERIFY(expired.cacheSaved && !expired.receiptSaved && !expired.error.isEmpty());
    QVERIFY(store.get(*id)->pages.isEmpty());
    QString error;
    const auto identity = LocalOcrPersistence::identity(settings, page, &error);
    QVERIFY(identity.has_value());
    const auto orphan = LocalOcrCache::load(cache, *identity, &error);
    QVERIFY2(orphan.has_value(), qPrintable(error));
    QCOMPARE(orphan->rawResult, page.rawResult);
    QVERIFY(store.interruptExpired(1101));
    QVERIFY(store.resume(*id));
    const auto current = store.claim(*id, QStringLiteral("new-owner"), 2000, 1000);
    QVERIFY(current.has_value());
    auto cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = LocalOcrPersistence::recordPage(store, *current, settings, page, cache, cancel, [&] {
        cancel->store(true); // Cancellation between cache publication and receipt.
        return 2001;
    });
    QVERIFY(cancelled.cacheSaved && !cancelled.receiptSaved);
    QVERIFY(store.get(*id)->pages.isEmpty());
    const auto saved = LocalOcrPersistence::recordPage(store, *current, settings, page, cache, { }, [] { return 2002; });
    QVERIFY(saved.cacheSaved && saved.receiptSaved && saved.error.isEmpty());
    QCOMPARE(store.get(*id)->pages.size(), 1);
    QCOMPARE(store.get(*id)->pages[0].page, 1);
    QCOMPARE(store.get(*id)->pages[0].cacheKey, saved.cacheKey);
    auto replacement = page;
    replacement.rawResult.append(' ');
    replacement.resultSha256 = QString::fromLatin1(QCryptographicHash::hash(replacement.rawResult, QCryptographicHash::Sha256).toHex());
    QVERIFY(!LocalOcrPersistence::recordPage(store, *current, settings, replacement, cache, { }, [] { return 2003; }).cacheSaved);
    QCOMPARE(LocalOcrCache::load(cache, *identity, &error)->rawResult, page.rawResult);
    auto second = page;
    second.selectedIndex = 1;
    QVERIFY(LocalOcrPersistence::recordPage(store, *current, settings, second, cache, { }, [] { return 2004; }).receiptSaved);
    QCOMPARE(store.get(*id)->pages.size(), 2);
    QCOMPARE(store.get(*id)->state, OcrJobs::State::Running); // Persisting all pages never finishes a session.
    QVERIFY(store.fail(*current, QStringLiteral("synthetic worker failed after all output"), 2005));
    QCOMPARE(store.get(*id)->state, OcrJobs::State::Failed);
    QCOMPARE(store.get(*id)->pages.size(), 2);
    auto gpuRuntimeOnCpu = page;
    gpuRuntimeOnCpu.requestedDevice = QStringLiteral("gpu:0");
    const auto gpuIdentity = LocalOcrPersistence::identity(settings, gpuRuntimeOnCpu, &error);
    QVERIFY(gpuIdentity.has_value());
    QCOMPARE(gpuIdentity->packageManifestSha256, QString(64, u'6'));
    QCOMPARE(identity->packageManifestSha256, QString(64, u'4'));
}

void LocalMetadataTest::cacheReceiptRecovery()
{
    for (const QString mode : { "cpu", "gpu", "gpu-package-cpu" }) {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        OcrJobs::Store store;
        QVERIFY(store.open(temporary.filePath("ocr-jobs.sqlite")));
        const auto settings = persistenceSettings(temporary.path());
        const auto id = store.enqueue(persistenceSpec(temporary.path(), settings));
        QVERIFY(id.has_value());
        const auto lease = store.claim(*id, QStringLiteral("synthetic-owner"), 1000, 1000);
        QVERIFY(lease.has_value());
        auto page = persistencePage();
        page.selectedIndex = 1; // A receipt for page 2, not array position 0.
        if (mode != QStringLiteral("cpu"))
            page.requestedDevice = QStringLiteral("gpu:0");
        if (mode == QStringLiteral("gpu")) {
            page.actualDevice = QStringLiteral("gpu:0");
            page.rawResult.replace("cpu", "gpu:0");
            page.resultSha256 = QString::fromLatin1(QCryptographicHash::hash(page.rawResult, QCryptographicHash::Sha256).toHex());
        }
        const auto cache = temporary.filePath("cache");
        QVERIFY(QDir().mkdir(cache));
        const auto saved = LocalOcrPersistence::recordPage(store, *lease, settings, page, cache, { }, [] { return 1001; });
        QVERIFY2(saved.receiptSaved, qPrintable(saved.error));
        QVERIFY(store.fail(*lease, QStringLiteral("synthetic batch failure"), 1002));
        const auto job = store.get(*id);
        QVERIFY(job.has_value());
        QString error;
        const auto restored = LocalOcrPersistence::restorePage(*job, settings, 1, page.geometry, page.imageSha256, cache, &error);
        QVERIFY2(restored.has_value(), qPrintable(error));
        QVERIFY(error.isEmpty());
        QCOMPARE(restored->selectedIndex, 1);
        QCOMPARE(restored->rawResult, page.rawResult);
        QCOMPARE(restored->resultSha256, page.resultSha256);
        QCOMPARE(restored->requestedDevice, page.requestedDevice);
        QCOMPARE(restored->actualDevice, page.actualDevice);
        QVERIFY(LocalMetadata::parseNeuralReading(restored->rawResult, restored->geometry.preparedSize).text.isEmpty());
        auto checkRejected = [&](const OcrJobs::Job &j, const LocalOcrRuntime::Measurement &m, int index,
                                 const LocalMetadata::OcrGeometry &geometry, const QString &hash, const QString &root) {
            error.clear();
            const auto value = LocalOcrPersistence::restorePage(j, m, index, geometry, hash, root, &error);
            QVERIFY(!value.has_value());
            QVERIFY(!error.isEmpty());
        };
        checkRejected(*job, settings, -1, page.geometry, page.imageSha256, cache);
        checkRejected(*job, settings, 2, page.geometry, page.imageSha256, cache);
        checkRejected(*job, settings, 0, page.geometry, page.imageSha256, cache); // Missing receipt.
        checkRejected(*job, settings, 1, page.geometry, QString(64, u'f'), cache);
        checkRejected(*job, settings, 1, { }, page.imageSha256, cache);
        auto geometry = page.geometry;
        geometry.inputToPrepared.translate(1, 0);
        checkRejected(*job, settings, 1, geometry, page.imageSha256, cache);
        checkRejected(*job, settings, 1, page.geometry, page.imageSha256, temporary.filePath("missing-cache"));
        auto changed = settings;
        auto environment = changed.settingsSnapshot["environment"].toObject();
        environment["applicationSha256"] = QString(64, u'f');
        changed.settingsSnapshot["environment"] = environment;
        changed.settingsFingerprint = OcrJobs::settingsFingerprint(changed.settingsSnapshot);
        checkRejected(*job, changed, 1, page.geometry, page.imageSha256, cache);
        // Even when supplied job/settings agree, changed package identity cannot
        // reuse an old receipt's cache key.
        environment = settings.settingsSnapshot["environment"].toObject();
        auto runtime = environment[page.requestedDevice == QStringLiteral("cpu") ? "cpu" : "gpu"].toObject();
        runtime["packageManifestSha256"] = QString(64, u'f');
        environment[page.requestedDevice == QStringLiteral("cpu") ? "cpu" : "gpu"] = runtime;
        changed = settings;
        changed.settingsSnapshot["environment"] = environment;
        changed.settingsFingerprint = OcrJobs::settingsFingerprint(changed.settingsSnapshot);
        auto alteredJob = *job;
        alteredJob.spec.settingsSnapshot = changed.settingsSnapshot;
        alteredJob.spec.settingsFingerprint = changed.settingsFingerprint;
        checkRejected(alteredJob, changed, 1, page.geometry, page.imageSha256, cache);
        for (int field = 0; field < 4; ++field) {
            alteredJob = *job;
            if (field == 0)
                alteredJob.pages[0].resultSha256 = QString(64, u'f');
            else if (field == 1)
                alteredJob.pages[0].cacheKey = QString(64, u'f');
            else if (field == 2)
                alteredJob.pages[0].actualDevice = QStringLiteral("invalid");
            else
                alteredJob.pages.append(alteredJob.pages[0]);
            checkRejected(alteredJob, settings, 1, page.geometry, page.imageSha256, cache);
        }
        auto cancel = std::make_shared<std::atomic_bool>(true);
        QVERIFY(!LocalOcrPersistence::restorePage(*job, settings, 1, page.geometry, page.imageSha256, cache, &error, cancel));
        QVERIFY(!error.isEmpty());
        // A damaged cache is retained for review, never deleted or replaced.
        const auto cachePath = QDir(cache).filePath(saved.cacheKey + QStringLiteral(".json"));
        QFile damaged(cachePath);
        QVERIFY(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(damaged.write("synthetic damage"), qint64(16));
        damaged.close();
        checkRejected(*job, settings, 1, page.geometry, page.imageSha256, cache);
        QVERIFY(damaged.open(QIODevice::ReadOnly));
        QCOMPARE(damaged.readAll(), QByteArray("synthetic damage"));
        QCOMPARE(store.get(*id)->state, OcrJobs::State::Failed);
        QCOMPARE(store.get(*id)->pages[0].resultSha256, page.resultSha256);
        QCOMPARE(QDir(cache).entryList(QDir::Files).size(), 1);
    }
}

void LocalMetadataTest::cacheReceiptCrash()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto database = temporary.filePath("ocr-jobs.sqlite");
    const auto cache = temporary.filePath("cache");
    QVERIFY(QDir().mkdir(cache));
    OcrJobs::Store store;
    QVERIFY(store.open(database));
    const auto settings = persistenceSettings(temporary.path());
    const auto id = store.enqueue(persistenceSpec(temporary.path(), settings));
    QVERIFY(id.has_value());
    const auto lease = store.claim(*id, QStringLiteral("crash-owner"), 1000, 100);
    QVERIFY(lease.has_value());
    QProcess process;
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    const auto cleanup = qScopeGuard([&] {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(5000);
        }
    });
    const auto page = persistencePage();
    process.start(QCoreApplication::applicationFilePath(), { "--ocr-persist-crash", database, cache, *id, lease->token, lease->owner, page.imageSha256 });
    QVERIFY(process.waitForFinished(10000));
    QCOMPARE(process.exitCode(), 86);
    QVERIFY(store.get(*id)->pages.isEmpty());
    QString error;
    const auto identity = LocalOcrPersistence::identity(settings, page, &error);
    QVERIFY(identity.has_value());
    const auto orphan = LocalOcrCache::load(cache, *identity, &error);
    QVERIFY2(orphan.has_value(), qPrintable(error));
    QCOMPARE(orphan->rawResult, page.rawResult);
    OcrJobs::Store reopened;
    QVERIFY(reopened.open(database));
    QVERIFY(reopened.interruptExpired(1101));
    QVERIFY(reopened.resume(*id));
    const auto current = reopened.claim(*id, QStringLiteral("restarted-owner"), 2000, 1000);
    QVERIFY(current.has_value());
    const auto saved = LocalOcrPersistence::recordPage(reopened, *current, settings, page, cache, { }, [] { return 2001; });
    QVERIFY(saved.receiptSaved);
    QCOMPARE(reopened.get(*id)->pages.size(), 1);
}

void LocalMetadataTest::cacheReceiptClockAfterContention()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto database = temporary.filePath("ocr-jobs.sqlite");
    const auto cache = temporary.filePath("cache");
    const auto ready = temporary.filePath("locked");
    const auto release = temporary.filePath("release");
    QVERIFY(QDir().mkdir(cache));
    OcrJobs::Store store;
    QVERIFY(store.open(database));
    const auto settings = persistenceSettings(temporary.path());
    const auto id = store.enqueue(persistenceSpec(temporary.path(), settings));
    QVERIFY(id.has_value());
    const auto lease = store.claim(*id, QStringLiteral("waiting-owner"), 1000, 100);
    QVERIFY(lease.has_value());
    QProcess locker;
#ifdef Q_OS_WIN
    locker.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    const auto cleanup = qScopeGuard([&] {
        if (locker.state() != QProcess::NotRunning) {
            locker.kill();
            locker.waitForFinished(5000);
        }
    });
    locker.start(QCoreApplication::applicationFilePath(), { "--ocr-persist-lock", database, ready, release });
    QElapsedTimer timer;
    timer.start();
    while (!QFile::exists(ready) && timer.elapsed() < 5000)
        QTest::qWait(10);
    QVERIFY(QFile::exists(ready));
    std::atomic<qint64> logicalTime { 1001 };
    std::thread releaseWriter([&] {
        QThread::msleep(200);
        logicalTime.store(1101);
        QFile file(release);
        if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            file.write("release");
    });
    const auto result = LocalOcrPersistence::recordPage(store, *lease, settings, persistencePage(), cache, { }, [&] { return logicalTime.load(); });
    releaseWriter.join();
    QVERIFY(locker.waitForFinished(10000));
    QCOMPARE(locker.exitCode(), 0);
    QVERIFY(result.cacheSaved);
    QVERIFY(!result.receiptSaved); // A timestamp sampled before the blocked BEGIN would wrongly pass.
    QVERIFY(!result.error.isEmpty());
    QVERIFY(store.get(*id)->pages.isEmpty());
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
    if (argc > 1 && QByteArray(argv[1]) == "--ocr-persist-lock") {
        QCoreApplication app(argc, argv);
        return persistenceLockHelper(app.arguments());
    }
    if (argc > 1 && QByteArray(argv[1]) == "--ocr-persist-crash") {
        QCoreApplication app(argc, argv);
        return persistenceCrashHelper(app.arguments());
    }
    if (argc > 1 && QByteArray(argv[1]).startsWith("--ocr-tree-")) {
        QCoreApplication app(argc, argv);
        return processTreeHelper(app.arguments());
    }

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
    // This explicit private diagnostic includes both bounded OCR and review
    // reopening. Keep ordinary tests and an explicit caller override unchanged.
    if (!qEnvironmentVariableIsEmpty("YACREADER_PRIVATE_INSPECTOR_CASE") && app.arguments().contains(QStringLiteral("privateSelectedInspector")) && qEnvironmentVariableIsEmpty("QTEST_FUNCTION_TIMEOUT"))
        qputenv("QTEST_FUNCTION_TIMEOUT", "1050000");
    LocalMetadataTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "main.moc"
