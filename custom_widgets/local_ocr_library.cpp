#include "local_ocr_library.h"

#include "yacreader_global.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <limits>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace LocalOcrLibrary {
namespace {
bool ordinary(const QFileInfo &info)
{
    return info.isAbsolute() && info.exists() && !info.isSymLink() && !info.isJunction();
}
bool contained(const QString &root, const QString &path)
{
    const auto relative = QDir(root).relativeFilePath(path);
    return !path.isEmpty() && relative != QStringLiteral("..") && !relative.startsWith(QStringLiteral("../")) && !QDir::isAbsolutePath(relative);
}
#ifdef Q_OS_WIN
QJsonObject fileIdentity(HANDLE handle)
{
    FILE_ID_INFO identity { };
    BY_HANDLE_FILE_INFORMATION basic { };
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &identity, sizeof(identity)) || !GetFileInformationByHandle(handle, &basic) || (basic.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return { };
    const auto creation = (quint64(basic.ftCreationTime.dwHighDateTime) << 32) | basic.ftCreationTime.dwLowDateTime;
    return { { "volume", QString::number(identity.VolumeSerialNumber, 16) },
             { "fileId", QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(identity.FileId.Identifier), 16).toHex()) },
             { "created", QString::number(creation) } };
}
QString nativePath(const QString &path)
{
    auto value = QDir::toNativeSeparators(path);
    if (value.startsWith(QStringLiteral("\\\\")))
        return QStringLiteral("\\\\?\\UNC\\") + value.mid(2);
    return QStringLiteral("\\\\?\\") + value;
}
#endif
}

std::optional<Binding> read(const QString &libraryRoot, qulonglong comicInfoId,
                            const QString &expectedSource, QString *error)
{
    if (error)
        error->clear();
    const auto fail = [&](const QString &reason) -> std::optional<Binding> {
        if (error)
            *error = reason;
        return std::nullopt;
    };
#ifndef Q_OS_WIN
    Q_UNUSED(libraryRoot);
    Q_UNUSED(comicInfoId);
    Q_UNUSED(expectedSource);
    return fail(QStringLiteral("Local library generation binding requires Windows."));
#else
    const QFileInfo root(libraryRoot), source(expectedSource);
    if (comicInfoId == 0 || comicInfoId > qulonglong(std::numeric_limits<qint64>::max()) || !ordinary(root) || !root.isDir() || !ordinary(source))
        return fail(QStringLiteral("Invalid existing library or selected source."));
    Binding result;
    result.libraryRoot = root.canonicalFilePath();
    result.sourcePath = source.canonicalFilePath();
    result.comicInfoId = comicInfoId;
    if (!contained(result.libraryRoot, result.sourcePath))
        return fail(QStringLiteral("Selected source is outside this library."));
    const QFileInfo database(YACReader::LibraryPaths::libraryDatabasePath(result.libraryRoot));
    if (!ordinary(database) || !database.isFile() || !ordinary(QFileInfo(database.absolutePath())) || !contained(result.libraryRoot, database.canonicalFilePath()))
        return fail(QStringLiteral("Existing ordinary library database required."));
    const auto path = database.canonicalFilePath();
    const auto native = nativePath(path);
    // No delete sharing: retain this existing file object during the bounded
    // read, without denying ordinary SQLite reads/writes or creating a lock file.
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return fail(QStringLiteral("Could not retain library identity (Windows error %1).").arg(GetLastError()));
    auto close = qScopeGuard([&] { CloseHandle(handle); });
    const auto identity = fileIdentity(handle);
    if (identity.isEmpty())
        return fail(QStringLiteral("Could not identify the existing library file."));
    const auto connection = QUuid::createUuid().toString();
    QString queryError;
    int matches = 0;
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(path);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=2000"));
        if (!db.open()) {
            queryError = db.lastError().text();
        } else {
            QSqlQuery query(db);
            query.prepare(QStringLiteral("SELECT id, path FROM comic WHERE comicInfoId = :id ORDER BY id"));
            query.bindValue(QStringLiteral(":id"), qint64(comicInfoId));
            if (!query.exec()) {
                queryError = query.lastError().text();
            } else {
                int rows = 0;
                while (query.next()) {
                    if (++rows > 1000) {
                        queryError = QStringLiteral("Too many ambiguous source rows.");
                        break;
                    }
                    bool validId = false;
                    const auto rowId = query.value(0).toLongLong(&validId);
                    const auto selectedPath = QDir::cleanPath(QDir::fromNativeSeparators(result.libraryRoot + query.value(1).toString()));
                    if (selectedPath.compare(result.sourcePath, Qt::CaseInsensitive) != 0)
                        continue; // Do not stat unrelated copies sharing a comicInfoId.
                    const QFileInfo entry(selectedPath);
                    if (validId && rowId > 0 && ordinary(entry) && entry.canonicalFilePath() == result.sourcePath) {
                        ++matches;
                        result.comicId = QString::number(rowId);
                    }
                }
                if (query.lastError().isValid())
                    queryError = query.lastError().text();
            }
        }
    }
    QSqlDatabase::removeDatabase(connection);
    if (!queryError.isEmpty())
        return fail(queryError);
    if (matches != 1)
        return fail(QStringLiteral("Selected source is missing or ambiguous in the current library."));
    const QFileInfo after(path);
    HANDLE current = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (current == INVALID_HANDLE_VALUE)
        return fail(QStringLiteral("Library path changed during the selected-work check."));
    auto closeCurrent = qScopeGuard([&] { CloseHandle(current); });
    if (!ordinary(after) || after.canonicalFilePath() != path || fileIdentity(current) != identity)
        return fail(QStringLiteral("Library identity changed during the selected-work check."));
    result.manifest = { { "version", 1 }, { "platform", "windows" }, { "databasePath", path }, { "file", identity } };
    result.generation = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(result.manifest).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    return result;
#endif
}
}
