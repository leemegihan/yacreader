#include "yacreader_filename_normalizer.h"

#include "yacreader_global.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace {

struct RenameData {
    qulonglong comicId = 0;
    QString fileName;
    QString relativePath;
    QString title;
    QString writer;
};

bool isReservedDeviceName(const QString &segment)
{
    static const QStringList reserved {
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"), QStringLiteral("NUL"),
        QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"), QStringLiteral("COM4"),
        QStringLiteral("COM5"), QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"), QStringLiteral("LPT7"),
        QStringLiteral("LPT8"), QStringLiteral("LPT9")
    };

    return reserved.contains(segment.section(QLatin1Char('.'), 0, 0), Qt::CaseInsensitive);
}

QString databaseFilePath(const QString &libraryPath)
{
    return QDir(YACReader::LibraryPaths::libraryDataPath(libraryPath)).filePath(QStringLiteral("library.ydb"));
}

bool loadRenameData(const QString &libraryPath, qulonglong comicInfoId, RenameData *data, QString *errorMessage)
{
    if (data == nullptr)
        return false;

    const QString databasePath = databaseFilePath(libraryPath);
    if (!QFileInfo::exists(databasePath)) {
        if (errorMessage != nullptr)
            *errorMessage = QObject::tr("Library database not found: %1").arg(databasePath);
        return false;
    }

    const QString connectionName = QStringLiteral("yacreader-filename-read-%1")
                                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool success = false;
    QString error;

    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=2000"));
        db.setDatabaseName(databasePath);

        if (!db.open()) {
            error = db.lastError().text();
        } else {
            QSqlQuery query(db);
            query.prepare(QStringLiteral(
                    "SELECT c.id, c.fileName, c.path, ci.title, ci.writer "
                    "FROM comic c INNER JOIN comic_info ci ON c.comicInfoId = ci.id "
                    "WHERE ci.id = :comicInfoId"));
            query.bindValue(QStringLiteral(":comicInfoId"), QVariant::fromValue(comicInfoId));

            if (!query.exec()) {
                error = query.lastError().text();
            } else if (!query.next()) {
                error = QObject::tr("The comic could not be found in the library database.");
            } else {
                data->comicId = query.value(0).toULongLong();
                data->fileName = query.value(1).toString();
                data->relativePath = query.value(2).toString();
                data->title = query.value(3).toString().trimmed();
                data->writer = query.value(4).toString().trimmed();
                success = true;
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);

    if (!success && errorMessage != nullptr)
        *errorMessage = error;
    return success;
}

bool updateDatabasePath(const QString &libraryPath,
                        qulonglong comicId,
                        const QString &fileName,
                        const QString &relativePath,
                        QString *errorMessage)
{
    const QString connectionName = QStringLiteral("yacreader-filename-write-%1")
                                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool success = false;
    QString error;

    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=2000"));
        db.setDatabaseName(databaseFilePath(libraryPath));

        if (!db.open()) {
            error = db.lastError().text();
        } else if (!db.transaction()) {
            error = db.lastError().text();
        } else {
            QSqlQuery update(db);
            update.prepare(QStringLiteral(
                    "UPDATE comic SET fileName = :fileName, path = :path WHERE id = :id"));
            update.bindValue(QStringLiteral(":fileName"), fileName);
            update.bindValue(QStringLiteral(":path"), relativePath);
            update.bindValue(QStringLiteral(":id"), QVariant::fromValue(comicId));

            if (!update.exec() || update.numRowsAffected() != 1) {
                error = update.lastError().text();
                if (error.isEmpty())
                    error = QObject::tr("The comic database row could not be updated.");
                db.rollback();
            } else if (!db.commit()) {
                error = db.lastError().text();
                db.rollback();
            } else {
                success = true;
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);

    if (!success && errorMessage != nullptr)
        *errorMessage = error;
    return success;
}

}

QString YACReaderFilenameNormalizer::sanitizeSegment(QString segment)
{
    static const QString invalid = QStringLiteral("<>:\"/\\|?*");
    for (QChar &character : segment) {
        if (invalid.contains(character) || character < QChar(0x20))
            character = QLatin1Char('_');
    }

    segment.replace(QRegularExpression(QStringLiteral(R"(\s+)")), QStringLiteral(" "));
    segment = segment.trimmed();

    while (segment.endsWith(QLatin1Char('.')) || segment.endsWith(QLatin1Char(' ')))
        segment.chop(1);

    if (!segment.isEmpty() && isReservedDeviceName(segment))
        segment.append(QLatin1Char('_'));

    return segment;
}

QString YACReaderFilenameNormalizer::normalizedFileName(const QString &writer,
                                                        const QString &title,
                                                        const QString &extension)
{
    QString cleanWriter = sanitizeSegment(writer);
    QString cleanTitle = sanitizeSegment(title);

    if (cleanWriter.isEmpty() || cleanTitle.isEmpty())
        return QString();

    // Keep the complete file name comfortably below the Windows component limit.
    constexpr int maximumStemLength = 220;
    const int fixedLength = cleanWriter.size() + 3; // '[' + ']' + space
    const int maximumTitleLength = qMax(20, maximumStemLength - fixedLength);
    if (cleanTitle.size() > maximumTitleLength)
        cleanTitle = cleanTitle.left(maximumTitleLength).trimmed();

    QString stem = QStringLiteral("[%1] %2").arg(cleanWriter, cleanTitle);
    if (stem.size() > maximumStemLength)
        stem = stem.left(maximumStemLength).trimmed();

    return stem + extension;
}

YACReaderFilenameNormalizer::Result YACReaderFilenameNormalizer::offerRename(QWidget *parent,
                                                                             const QString &libraryPath,
                                                                             qulonglong comicInfoId,
                                                                             QString *errorMessage)
{
    RenameData data;
    QString error;
    if (!loadRenameData(libraryPath, comicInfoId, &data, &error)) {
        if (errorMessage != nullptr)
            *errorMessage = error;
        return Result::Failed;
    }

    const QString sourceAbsolute = QDir::cleanPath(libraryPath + data.relativePath);
    const QFileInfo sourceInfo(sourceAbsolute);
    // Image-folder comics need a directory-aware rename journal. Never run the
    // archive-only rename path against their original page directories.
    if (sourceInfo.isDir())
        return Result::Skipped;
    if (!sourceInfo.exists()) {
        if (errorMessage != nullptr)
            *errorMessage = QObject::tr("The comic file does not exist: %1").arg(sourceAbsolute);
        return Result::Failed;
    }

    const QString extension = sourceInfo.suffix().isEmpty()
            ? QString()
            : QLatin1Char('.') + sourceInfo.suffix();
    const QString proposedName = normalizedFileName(data.writer, data.title, extension);
    if (proposedName.isEmpty())
        return Result::Skipped;

    if (proposedName == sourceInfo.fileName())
        return Result::Unchanged;

    QMessageBox prompt(parent);
    prompt.setIcon(QMessageBox::Question);
    prompt.setWindowTitle(QObject::tr("Normalize manga file name"));
    prompt.setText(QObject::tr("Rename this comic using the metadata you just confirmed?"));
    prompt.setInformativeText(QObject::tr("Current:\n%1\n\nProposed:\n%2")
                                      .arg(sourceInfo.fileName(), proposedName));
    auto *renameButton = prompt.addButton(QObject::tr("Rename"), QMessageBox::AcceptRole);
    prompt.addButton(QObject::tr("Keep current name"), QMessageBox::RejectRole);
    prompt.exec();

    if (prompt.clickedButton() != renameButton)
        return Result::Skipped;

    const QString destinationAbsolute = sourceInfo.dir().filePath(proposedName);
    if (QFileInfo::exists(destinationAbsolute)) {
        if (errorMessage != nullptr)
            *errorMessage = QObject::tr("A file with the proposed name already exists: %1").arg(destinationAbsolute);
        return Result::Failed;
    }

    if (!QFile::rename(sourceAbsolute, destinationAbsolute)) {
        if (errorMessage != nullptr)
            *errorMessage = QObject::tr("The file could not be renamed. It may be open in another program or the path may be too long.");
        return Result::Failed;
    }

    QString relativePath = QDir(libraryPath).relativeFilePath(destinationAbsolute);
    relativePath.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!relativePath.startsWith(QLatin1Char('/')))
        relativePath.prepend(QLatin1Char('/'));

    if (!updateDatabasePath(libraryPath, data.comicId, proposedName, relativePath, &error)) {
        // Keep the file system and the database in sync if the database update fails.
        if (!QFile::rename(destinationAbsolute, sourceAbsolute))
            error += QObject::tr(" The database update failed and the original file name could not be restored.");

        if (errorMessage != nullptr)
            *errorMessage = error;
        return Result::Failed;
    }

    return Result::Renamed;
}
