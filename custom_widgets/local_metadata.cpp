#include "local_metadata.h"

#include "comic.h"
#include "comic_image_folder.h"
#include "compressed_archive.h"
#include "data_base_management.h"
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
    if (size.width() > 2400 || size.height() > 2400)
        reader.setScaledSize(size.scaled(2400, 2400, Qt::KeepAspectRatio));
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

QVector<Suggestion> suggest(const QVector<Page> &pages, const QString &sourcePath)
{
    QVector<Suggestion> result;
    auto append = [&](Suggestion::Field field, QString value, const QString &reason, int page) {
        value = value.simplified();
        if (value.size() < 2 || value.size() > 180 || value.contains(QStringLiteral("http"), Qt::CaseInsensitive) || value.contains(QLatin1Char('@')))
            return;
        for (const auto &existing : result) {
            if (existing.field == field && existing.value.compare(value, Qt::CaseInsensitive) == 0 && existing.page == page)
                return;
        }
        result.append({ field, value, reason, page });
    };
    // Role labels are evidence, not proof of identity. Translators/publishers and
    // circles are deliberately not treated as individual authors.
    const QRegularExpression author(QStringLiteral(R"(^\s*(?:著者|作者|著作|原作|作画|漫画|작가|저자|글[・·/]그림|글|그림|author|writer|artist|story\s*(?:&|and)\s*art(?:\s*by)?)\s*(?:[:：]\s*|\s+)(.+)$)"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression title(QStringLiteral(R"(^\s*(?:作品名|書名|タイトル|제목|작품명|title)\s*(?:[:：]\s*|\s+)(.+)$)"), QRegularExpression::CaseInsensitiveOption);
    for (const auto &page : pages) {
        const auto lines = page.text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (int index = 0; index < lines.size(); ++index) {
            QString line = lines.at(index).trimmed();
            const QRegularExpression labelOnly(QStringLiteral(R"(^(?:著者|作者|原作|作画|漫画|작가|저자|author|writer|artist|作品名|書名|タイトル|제목|작품명|title)\s*[:：]?$)"), QRegularExpression::CaseInsensitiveOption);
            if (labelOnly.match(line).hasMatch() && index + 1 < lines.size()) {
                line.remove(QRegularExpression(QStringLiteral("[:：]$")));
                line += QStringLiteral(": ") + lines.at(++index).trimmed();
            }
            const auto authorMatch = author.match(line);
            const auto titleMatch = title.match(line);
            if (authorMatch.hasMatch())
                append(Suggestion::Author, authorMatch.captured(1), tr("Author role label in page text — verify the credit."), page.number);
            if (titleMatch.hasMatch())
                append(Suggestion::Title, titleMatch.captured(1), tr("Title label in page text — verify against the page."), page.number);
            if (!authorMatch.hasMatch() && !titleMatch.hasMatch() && page.number <= 2 && index < 6 && line.size() >= 3 && line.size() <= 60 && !line.contains(QLatin1Char(':')) && !line.contains(QChar(0xff1a)) && !line.contains(QRegularExpression(QStringLiteral("(?:copyright|reserved|https|www\\.|R.?18|無断|転載|번역|역자|translator|scanlation)"), QRegularExpression::CaseInsensitiveOption)))
                append(Suggestion::Title, line, tr("Unlabelled opening-page text; may be dialogue or an advertisement, not the title."), page.number);
        }
    }
    const QFileInfo info(sourcePath);
    QString base = info.isDir() ? info.fileName() : info.completeBaseName();
    const auto bracket = QRegularExpression(QStringLiteral(R"(^\[([^\]]+)\]\s*(.+)$)")).match(base);
    if (bracket.hasMatch()) {
        append(Suggestion::Author, bracket.captured(1), tr("Bracketed filename text only; may be a group or website."), 0);
        base = bracket.captured(2);
    }
    append(Suggestion::Title, base, tr("Filename/folder name only; not confirmed by OCR."), 0);
    const QString parent = info.dir().dirName();
    const QStringList generic { "downloads", "download", "comics", "manga", "images", "pictures", "만화", "다운로드", "미분류", "unidentified", "temp" };
    if (!generic.contains(parent.toCaseFolded()))
        append(Suggestion::Author, parent, tr("Parent folder name only; may not be an author."), 0);
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
        options.dataPath = bundled.filePath(QStringLiteral("tessdata"));
    else
        options.executable = QStandardPaths::findExecutable(name);
    return options;
}

QString recognize(const QImage &image, const OcrOptions &options, const Cancellation &cancel, QString *error)
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
    if (!temporary.isValid() || !image.save(temporary.filePath(QStringLiteral("page.png")))) {
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
    QStringList arguments { "page.png", "stdout", "-l", options.language, "--psm", options.vertical ? "5" : "11" };
    if (!options.dataPath.isEmpty())
        arguments << "--tessdata-dir" << options.dataPath;
    if (!run(process, options.executable, arguments, options.timeoutMs, cancel, error))
        return { };
    const QString warnings = QString::fromUtf8(process.readAllStandardError());
    if (warnings.contains(QStringLiteral("Failed loading language"))) {
        *error = tr("An OCR language is missing: %1").arg(warnings.left(1500));
        return { };
    }
    return QString::fromUtf8(process.readAllStandardOutput()).left(50000).trimmed();
}

Result analyze(const QString &path, int perEnd, const OcrOptions &options, const Cancellation &cancel)
{
    Result result = readPages(path, perEnd, cancel);
    for (auto &page : result.pages) {
        if (cancelled(cancel))
            return result;
        if (!page.image.isNull())
            page.text = recognize(page.image, options, cancel, &page.error);
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
                for (const auto &item : evidence)
                    items.append(QJsonObject { { "field", item.field == Suggestion::Title ? "title" : "author" }, { "value", item.value }, { "page", item.page }, { "reason", item.reason } });
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
