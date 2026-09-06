#ifndef COMIC_IMAGE_FOLDER_H
#define COMIC_IMAGE_FOLDER_H

#include "comic.h"
#include "qnaturalsorting.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace ComicImageFolder {
// Never merge a collection's child folders/archives into a single comic.
inline QStringList pages(const QString &path)
{
    QDir directory(path);
    if (!directory.exists())
        return { };
    QStringList result;
    const auto entries = directory.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const auto &entry : entries) {
        // On Windows a leading dot does not necessarily set the hidden flag.
        if (entry.fileName() == QStringLiteral(".yacreaderlibrary") || entry.fileName() == QStringLiteral("__MACOSX"))
            continue;
        if (entry.isDir() || Comic::fileIsComic(entry.fileName()))
            return { };
        if (FileComic::isSupportedImage(entry.fileName(), Comic::getSupportedImageLiteralFormats()))
            result.append(entry.fileName());
    }
    std::sort(result.begin(), result.end(), naturalSortLessThanCI);
    return result;
}

inline bool isComic(const QFileInfo &info)
{
    return info.isDir() && !info.isSymLink() && !pages(info.absoluteFilePath()).isEmpty();
}
}

#endif
