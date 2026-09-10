#include "local_ocr_source.h"

#include "comic.h"
#include "comic_image_folder.h"
#include "compressed_archive.h"
#include "qnaturalsorting.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include <algorithm>

namespace LocalOcrSource {
namespace {
struct Catalog {
    QString canonical;
    QStringList names;
    QVector<QPair<qint64, qint64>> stamps;
    bool folder = false;
};
std::optional<Catalog> catalog(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isAbsolute() || !info.exists() || info.isSymLink())
        return std::nullopt;
    Catalog result;
    result.canonical = info.canonicalFilePath();
    result.folder = info.isDir();
    if (result.folder) {
        result.names = ComicImageFolder::pages(result.canonical);
        for (const auto &name : result.names) {
            const QFileInfo page(QDir(result.canonical).filePath(name));
            if (!page.isFile() || page.isSymLink() || page.canonicalPath() != result.canonical)
                return std::nullopt;
            result.stamps.append({ page.size(), page.lastModified().toMSecsSinceEpoch() });
        }
    } else {
        if (!info.isFile())
            return std::nullopt;
        CompressedArchive archive(result.canonical);
        if (!archive.toolsLoaded() || !archive.isValid())
            return std::nullopt;
        result.names = FileComic::filter(archive.getFileNames());
        std::sort(result.names.begin(), result.names.end(), naturalSortLessThanCI);
        result.stamps.append({ info.size(), info.lastModified().toMSecsSinceEpoch() });
    }
    if (result.names.isEmpty() || result.names.size() > 100000)
        return std::nullopt;
    QSet<QString> seen;
    for (const auto &name : result.names) {
        // An archive can contain duplicate entry names. indexOf cannot identify
        // which bytes belong to that page, so refuse an ambiguous resume plan.
        if (name.isEmpty() || seen.contains(name))
            return std::nullopt;
        seen.insert(name);
    }
    return result;
}
}

std::optional<Snapshot> read(const QString &path, int perEnd,
                             const LocalMetadata::Cancellation &cancel, QString *error)
{
    if (error)
        error->clear();
    const auto invalid = [&](const QString &message) -> std::optional<Snapshot> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    const auto cancelled = [&] { return cancel && cancel->load(); };
    if (cancelled())
        return invalid(QStringLiteral("Source snapshot cancelled."));
    if (perEnd < 1 || perEnd > 3)
        return invalid(QStringLiteral("A source snapshot permits first/last one to three pages only."));
    const auto before = catalog(path);
    if (!before)
        return invalid(QStringLiteral("Unsupported, linked or ambiguous single-work source."));
    if (cancelled())
        return invalid(QStringLiteral("Source snapshot cancelled."));
    Snapshot result;
    result.source = LocalMetadata::readPages(before->canonical, perEnd, cancel);
    if (cancelled())
        return invalid(QStringLiteral("Source snapshot cancelled."));
    if (!result.source.error.isEmpty())
        return invalid(result.source.error);
    const auto after = catalog(path);
    if (!after || before->canonical != after->canonical || before->folder != after->folder || before->names != after->names || before->stamps != after->stamps || result.source.pageNames != before->names)
        return invalid(QStringLiteral("Source changed while preparing its page snapshot."));
    const auto indexes = LocalMetadata::sampleIndexes(before->names.size(), perEnd);
    if (result.source.pages.size() != indexes.size())
        return invalid(QStringLiteral("Incomplete source page snapshot."));
    QJsonArray pages, names;
    for (const auto &name : before->names)
        names.append(name);
    for (int i = 0; i < indexes.size(); ++i) {
        const auto &page = result.source.pages.at(i);
        if (!page.error.isEmpty() || page.image.isNull() || page.sourceSha256.size() != 64 || page.number != indexes.at(i) + 1 || page.name != before->names.at(indexes.at(i)))
            return invalid(QStringLiteral("A selected source page could not be validated."));
        pages.append(QJsonObject { { "number", page.number }, { "name", page.name }, { "rawSha256", page.sourceSha256 } });
    }
    result.manifest = QJsonObject { { "version", 1 }, { "path", before->canonical }, { "sourceKind", before->folder ? "folder" : "archive" }, { "perEnd", perEnd }, { "pageNames", names }, { "selectedPages", pages } };
    result.fingerprint = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(result.manifest).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    if (cancelled())
        return invalid(QStringLiteral("Source snapshot cancelled."));
    return result;
}
}
