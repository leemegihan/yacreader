#include "local_metadata.h"

#include "comic.h"
#include "comic_image_folder.h"
#include "compressed_archive.h"
#include "library_maintenance_lock.h"
#include "qnaturalsorting.h"
#include "yacreader_global.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>

namespace LocalMetadata {
namespace {
QString tr(const char *text)
{
    return QCoreApplication::translate("LocalMetadata", text);
}
bool cancelled(const Cancellation &flag)
{
    return flag && flag->load();
}

QImage decode(const QByteArray &bytes)
{
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (!size.isValid() || qint64(size.width()) * size.height() > 100000000)
        return { };
    if (size.width() > 4000 || size.height() > 4000)
        reader.setScaledSize(size.scaled(4000, 4000, Qt::KeepAspectRatio));
    return reader.read();
}

bool run(QProcess &process, const QString &program, const QStringList &arguments,
         int timeout, const Cancellation &cancel, QString *error)
{
    if (cancelled(cancel))
        return false;
    process.start(program, arguments, QIODevice::ReadOnly);
    if (!process.waitForStarted(5000)) {
        *error = tr("Could not start local OCR: %1").arg(process.errorString());
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    while (!process.waitForFinished(100)) {
        if (cancelled(cancel) || timer.elapsed() > timeout) {
            process.kill();
            process.waitForFinished(5000);
            *error = cancelled(cancel) ? tr("Cancelled") : tr("OCR timed out for this page.");
            return false;
        }
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        *error = tr("Local OCR failed: %1").arg(QString::fromUtf8(process.readAllStandardError()).left(1500));
        return false;
    }
    return true;
}
}

QVector<int> sampleIndexes(int pageCount, int perEnd)
{
    QVector<int> result;
    perEnd = qBound(1, perEnd, 6);
    for (int i = 0; i < qMin(pageCount, perEnd); ++i)
        result.append(i);
    for (int i = qMax(perEnd, pageCount - perEnd); i < pageCount; ++i)
        result.append(i);
    return result;
}

Result readPages(const QString &path, int perEnd, const Cancellation &cancel)
{
    Result result;
    const QFileInfo info(path);
    if (!info.exists()) {
        result.error = tr("Comic source not found: %1").arg(path);
        return result;
    }
    QStringList names;
    QStringList archiveOrder;
    std::unique_ptr<CompressedArchive> archive;
    if (info.isDir()) {
        names = ComicImageFolder::pages(path);
        if (names.isEmpty()) {
            result.error = tr("Choose a single image folder, not a collection containing subfolders or archives.");
            return result;
        }
    } else {
        archive = std::make_unique<CompressedArchive>(path);
        if (!archive->toolsLoaded() || !archive->isValid()) {
            result.error = tr("This source is not a supported image archive.");
            return result;
        }
        archiveOrder = archive->getFileNames();
        names = FileComic::filter(archiveOrder);
        std::sort(names.begin(), names.end(), naturalSortLessThanCI);
    }
    result.pageCount = names.size();
    for (const auto index : sampleIndexes(result.pageCount, perEnd)) {
        if (cancelled(cancel))
            return result;
        Page page;
        page.number = index + 1;
        page.name = names.at(index);
        QByteArray bytes;
        if (archive) {
            bytes = archive->getRawDataAtIndex(archiveOrder.indexOf(page.name));
        } else {
            QFile file(QDir(path).filePath(page.name));
            if (file.size() <= 64 * 1024 * 1024 && file.open(QIODevice::ReadOnly))
                bytes = file.readAll();
        }
        if (bytes.size() <= 64 * 1024 * 1024)
            page.image = decode(bytes);
        if (page.image.isNull())
            page.error = tr("Page could not be decoded or exceeds the safety limit.");
        result.pages.append(page);
    }
    if (result.pages.isEmpty())
        result.error = tr("No supported image pages found.");
    return result;
}

OcrOptions defaultOcrOptions()
{
    OcrOptions options;
    const QDir bundled(QCoreApplication::applicationDirPath() + QStringLiteral("/ocr"));
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("tesseract.exe");
#else
    const QString name = QStringLiteral("tesseract");
#endif
    options.executable = bundled.filePath(name);
    if (QFileInfo::exists(options.executable))
        options.dataPath = bundled.filePath(QFileInfo::exists(bundled.filePath(QStringLiteral("tessdata_best/jpn.traineddata"))) ? QStringLiteral("tessdata_best") : QStringLiteral("tessdata"));
    else
        options.executable = QStandardPaths::findExecutable(name);
    return options;
}

QImage prepareOcrImage(const QImage &image, const OcrOptions &options)
{
    if (image.isNull())
        return { };
    // Composite transparent text onto white before grayscale conversion. Keep
    // the original page intact; all preprocessing happens on this local copy.
    QImage source = image;
    if (source.width() > 4000 || source.height() > 4000)
        source = source.scaled(4000, 4000, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QImage flat(source.size(), QImage::Format_RGB32);
    flat.fill(Qt::white);
    {
        QPainter painter(&flat);
        painter.drawImage(0, 0, source);
    }
    if (options.rotation % 360 != 0)
        flat = flat.transformed(QTransform().rotate(options.rotation), Qt::SmoothTransformation);
    if (options.invert)
        flat.invertPixels();
    // Small credit regions benefit from rescaling. Bound both dimensions so a
    // narrow, tall selection cannot allocate an unbounded bitmap.
    if (qMax(flat.width(), flat.height()) < 2000)
        flat = flat.scaled(flat.size() * 2, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QImage bordered(flat.size() + QSize(40, 40), QImage::Format_RGB32);
    bordered.fill(Qt::white);
    {
        QPainter painter(&bordered);
        painter.drawImage(20, 20, flat);
    }
    return bordered.convertToFormat(QImage::Format_Grayscale8);
}

static QByteArray runOcrTsv(const QImage &image, const OcrOptions &options, const Cancellation &cancel, QString *error)
{
    error->clear();
    if (image.isNull()) {
        *error = tr("No image to recognize.");
        return { };
    }
    if (options.executable.isEmpty()) {
        *error = tr("Local Tesseract OCR is not installed. Use an OCR-enabled build or install Tesseract with Japanese, Korean and English language data.");
        return { };
    }
    QTemporaryDir temporary;
    if (!temporary.isValid() || !prepareOcrImage(image, options).save(temporary.filePath(QStringLiteral("page.png")))) {
        *error = tr("Could not create the temporary OCR image.");
        return { };
    }
    // Relative ASCII filenames also work with Windows executables that cannot
    // open a Unicode image filename. No command shell and no URL input.
    QProcess process;
    process.setWorkingDirectory(temporary.path());
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("OMP_THREAD_LIMIT"), QStringLiteral("2"));
    process.setProcessEnvironment(environment);
    const int segmentation = options.vertical ? 5 : options.segmentation;
    if (segmentation != 5 && segmentation != 6 && segmentation != 7 && segmentation != 11 && segmentation != 13) {
        *error = tr("Unsupported OCR text layout.");
        return { };
    }
    // Use parameters rather than a 'tsv' config file: offline packages contain
    // traineddata only, and must not depend on system Tesseract config files.
    QStringList arguments { "page.png", "stdout", "-l", options.language, "--oem", "1", "--psm", QString::number(segmentation), "--dpi", "300", "-c", "tessedit_create_tsv=1", "-c", "tessedit_create_txt=0" };
    if (options.adaptiveThreshold)
        arguments << "-c" << "thresholding_method=2";
    QString dataPath = options.dataPath;
#ifdef Q_OS_WIN
    // Tesseract still uses narrow fopen() for language files. A Unicode install
    // path may not survive the executable's ANSI argv conversion. Qt can copy
    // the selected models into this private working directory; relative ASCII
    // paths then work even when Windows 8.3 short names are disabled.
    if (std::any_of(dataPath.cbegin(), dataPath.cend(), [](QChar ch) { return ch.unicode() > 127; })) {
        const QString staged = temporary.filePath(QStringLiteral("tessdata"));
        if (!QDir().mkpath(staged)) {
            *error = tr("Could not prepare temporary OCR language data.");
            return { };
        }
        const QRegularExpression languageName(QStringLiteral("^[A-Za-z0-9_]+$"));
        auto languages = options.language.split(QLatin1Char('+'), Qt::SkipEmptyParts);
        // The pinned Japanese model loads jpn_vert as a sublanguage even for
        // horizontal text. Stage that dependency beside jpn in Unicode paths.
        if (languages.contains(QStringLiteral("jpn")) && !languages.contains(QStringLiteral("jpn_vert")))
            languages.append(QStringLiteral("jpn_vert"));
        for (const auto &language : languages) {
            if (cancelled(cancel))
                return { };
            if (!languageName.match(language).hasMatch() || !QFile::copy(QDir(dataPath).filePath(language + QStringLiteral(".traineddata")), QDir(staged).filePath(language + QStringLiteral(".traineddata")))) {
                *error = tr("Could not prepare OCR language: %1").arg(language);
                return { };
            }
        }
        dataPath = QStringLiteral("tessdata");
    }
#endif
    if (!dataPath.isEmpty())
        arguments << "--tessdata-dir" << dataPath;
    if (!run(process, options.executable, arguments, options.timeoutMs, cancel, error))
        return { };
    const QString warnings = QString::fromUtf8(process.readAllStandardError());
    if (warnings.contains(QStringLiteral("Failed loading language"))) {
        *error = tr("An OCR language is missing: %1").arg(warnings.left(1500));
        return { };
    }
    return process.readAllStandardOutput().left(4 * 1024 * 1024 + 1);
}

static Reading recognizeNeuralPage(const QImage &image, const OcrOptions &options, const Cancellation &cancel)
{
    Reading reading;
    reading.reviewRequired = true;
    const QDir root(QCoreApplication::applicationDirPath() + QStringLiteral("/ocr-neural"));
    const auto python = root.filePath(QStringLiteral("runtime/python.exe"));
    if (!QFileInfo::exists(python) || !QFileInfo::exists(root.filePath(QStringLiteral("worker.py")))) {
        reading.error = tr("영역 탐지 OCR이 포함된 Windows 설치본이 필요합니다.");
        return reading;
    }
    if (options.language != "auto" && options.language != "jpn+eng" && options.language != "jpn_vert+eng" && options.language != "kor+eng") {
        reading.error = tr("영역 OCR은 자동, 일본어 또는 한국어를 선택해 주세요.");
        return reading;
    }
    QTemporaryDir temporary;
    const auto prepared = prepareOcrImage(image, options);
    if (!temporary.isValid() || prepared.isNull() || !prepared.save(temporary.filePath("page.png"))) {
        reading.error = tr("Could not create the temporary OCR image.");
        return reading;
    }
    QProcess process;
    process.setWorkingDirectory(root.path());
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("PYTHONHOME"));
    environment.remove(QStringLiteral("PYTHONPATH"));
    environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    process.setProcessEnvironment(environment);
    const QString language = options.language == "auto" ? "auto" : options.language.startsWith("kor") ? "kor"
                                                                                                      : "jpn";
    const QStringList arguments { "-I", "-X", "utf8", root.filePath("worker.py"), "--image", temporary.filePath("page.png"), "--output", temporary.filePath("result.json"), "--language", language };
    if (!run(process, python, arguments, qMax(options.timeoutMs, 180000), cancel, &reading.error))
        return reading;
    QFile output(temporary.filePath("result.json"));
    if (!output.open(QIODevice::ReadOnly) || output.size() > 4 * 1024 * 1024) {
        reading.error = tr("영역 OCR 결과를 읽지 못했습니다.");
        return reading;
    }
    return parseNeuralReading(output.readAll(), prepared.size());
}

Reading recognizePage(const QImage &image, const OcrOptions &options, const Cancellation &cancel)
{
    if (options.neural)
        return recognizeNeuralPage(image, options, cancel);
    const QStringList languages = options.language == "auto"
            ? QStringList { options.vertical || options.segmentation == 5 ? "jpn_vert+eng" : "jpn+eng", "kor+eng" }
            : QStringList { options.language };
    QVector<Reading> readings;
    for (const auto &language : languages) {
        if (cancelled(cancel))
            return { };
        auto selected = options;
        selected.language = language;
        auto read = [&](const OcrOptions &pass) {
            QString error;
            const auto tsv = runOcrTsv(image, pass, cancel, &error);
            auto reading = parseTsv(tsv, language);
            if (!error.isEmpty())
                reading.error = error;
            return reading;
        };
        const auto reading = read(selected);
        readings.append(reading);
        // Sparse layout can split Hangul syllables into separate fragments.
        // Compare a block pass for recognizable credits or weak readings, while
        // preserving an explicitly chosen layout and the cancellation boundary.
        if (options.language == "auto" && options.segmentation == 11 && !options.vertical && reading.error.isEmpty() && (reading.confidence < 60 || classifyPage(reading.text, 0) != PageKind::Unknown)) {
            if (cancelled(cancel))
                return { };
            selected.segmentation = 6;
            readings.append(read(selected));
        }
    }
    return options.language == "auto" ? chooseReading(readings) : readings.first();
}

QString recognize(const QImage &image, const OcrOptions &options, const Cancellation &cancel, QString *error)
{
    const auto reading = recognizePage(image, options, cancel);
    *error = reading.error;
    return reading.text;
}

Result analyze(const QString &path, int perEnd, const OcrOptions &options, const Cancellation &cancel)
{
    Result result = readPages(path, perEnd, cancel);
    for (auto &page : result.pages) {
        if (cancelled(cancel))
            return result;
        if (!page.image.isNull()) {
            page.reading = recognizePage(page.image, options, cancel);
            page.text = page.reading.text;
            page.error = page.reading.error;
            page.kind = classifyPage(page.text, page.number);
        }
    }
    result.suggestions = suggest(result.pages, path);
    return result;
}

QString comicPath(const QString &libraryPath, qulonglong comicInfoId, QString *error)
{
    error->clear();
    QString path;
    const QString connection = QUuid::createUuid().toString();
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(YACReader::LibraryPaths::libraryDatabasePath(libraryPath));
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=2000"));
        if (db.open()) {
            QSqlQuery query(db);
            query.prepare(QStringLiteral("SELECT path FROM comic WHERE comicInfoId = :id ORDER BY id LIMIT 1"));
            query.bindValue(":id", QVariant::fromValue(comicInfoId));
            if (query.exec() && query.next()) {
                path = QDir::cleanPath(libraryPath + query.value(0).toString());
                const auto relative = QDir(QFileInfo(libraryPath).canonicalFilePath()).relativeFilePath(QFileInfo(path).canonicalFilePath());
                if (!QFileInfo::exists(path) || relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative)) {
                    path.clear();
                    *error = tr("Comic source is missing or is outside this library.");
                }
            } else {
                *error = tr("Could not find the comic source in the database.");
            }
        } else {
            *error = db.lastError().text();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return path;
}

bool save(const QString &libraryPath, qulonglong comicInfoId, const QString &sourcePath,
          const QString &title, const QString &author, bool overwrite,
          const QVector<Suggestion> &evidence, QString *error)
{
    error->clear();
    if (title.trimmed().isEmpty() && author.trimmed().isEmpty()) {
        *error = tr("Enter a title or author before saving.");
        return false;
    }
    LibraryMaintenanceLock maintenance(libraryPath);
    if (!maintenance.tryLock()) {
        *error = maintenance.errorString();
        return false;
    }
    const auto resolvedSource = comicPath(libraryPath, comicInfoId, error);
    if (resolvedSource.isEmpty() || resolvedSource != sourcePath) {
        if (error->isEmpty())
            *error = tr("The comic path changed. Reopen the inspector before saving.");
        return false;
    }
    const QString connection = QUuid::createUuid().toString();
    bool success = false;
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(YACReader::LibraryPaths::libraryDatabasePath(libraryPath));
        db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=2000"));
        if (!db.open() || !db.transaction()) {
            *error = db.lastError().text();
        } else {
            QSqlQuery query(db);
            query.prepare(QStringLiteral(
                    "UPDATE comic_info SET "
                    "title=CASE WHEN (:overwrite OR TRIM(COALESCE(title,''))='') AND TRIM(:title)<>'' THEN :title ELSE title END, "
                    "writer=CASE WHEN (:overwrite OR TRIM(COALESCE(writer,''))='') AND TRIM(:author)<>'' THEN :author ELSE writer END, "
                    "edited=1, lastTimeMetadataSet=:time WHERE id=:id"));
            query.bindValue(":overwrite", overwrite);
            query.bindValue(":title", title.trimmed());
            query.bindValue(":author", author.trimmed());
            query.bindValue(":time", QDateTime::currentSecsSinceEpoch());
            query.bindValue(":id", QVariant::fromValue(comicInfoId));
            success = query.exec() && query.numRowsAffected() == 1;
            if (!success)
                *error = query.lastError().text().isEmpty() ? tr("No matching comic was updated.") : query.lastError().text();
            // Provenance stays in the library database, never in original pages.
            if (success)
                success = query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS local_metadata_evidence (id INTEGER PRIMARY KEY, comicInfoId INTEGER, sourcePath TEXT, reviewedTitle TEXT, reviewedAuthor TEXT, evidence TEXT, created INTEGER)"));
            if (success) {
                QJsonArray items;
                for (const auto &item : evidence) {
                    QJsonArray sources;
                    for (const auto &source : item.evidence)
                        sources.append(QJsonObject { { "page", source.page }, { "labelled", source.labelled }, { "ocrConfidence", source.confidence } });
                    items.append(QJsonObject { { "field", item.field == Suggestion::Title ? "title" : item.field == Suggestion::Author ? "author"
                                                                                                                                       : "publisher" },
                                               { "value", item.value },
                                               { "page", item.page },
                                               { "reason", item.reason },
                                               { "sources", sources },
                                               { "ocrConfidence", item.confidence } });
                }
                query.prepare(QStringLiteral("INSERT INTO local_metadata_evidence (comicInfoId,sourcePath,reviewedTitle,reviewedAuthor,evidence,created) VALUES (?,?,?,?,?,?)"));
                query.addBindValue(QVariant::fromValue(comicInfoId));
                query.addBindValue(sourcePath);
                query.addBindValue(title.trimmed());
                query.addBindValue(author.trimmed());
                query.addBindValue(QString::fromUtf8(QJsonDocument(items).toJson(QJsonDocument::Compact)));
                query.addBindValue(QDateTime::currentSecsSinceEpoch());
                success = query.exec();
            }
            if (!success) {
                if (error->isEmpty())
                    *error = query.lastError().text();
                db.rollback();
            } else if (!db.commit()) {
                *error = db.lastError().text();
                db.rollback();
                success = false;
            }
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return success;
}
}
