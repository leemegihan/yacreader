#ifndef LOCAL_OCR_LIBRARY_H
#define LOCAL_OCR_LIBRARY_H

#include <QJsonObject>
#include <QString>

#include <optional>

namespace LocalOcrLibrary {
struct Binding {
    QString generation;
    QString comicId; // The selected comic row, not the possibly shared comicInfoId.
    qulonglong comicInfoId = 0;
    QString libraryRoot;
    QString sourcePath;
    QJsonObject manifest; // Private paths and local filesystem identity.
};
// Personal Windows library binding. Opens only an existing DB, read-only, and
// checks the requested source against rows with the supplied comicInfoId.
// No schema update, OCR, enumeration of other works, or metadata save.
// This is a snapshot, not a lock covering subsequent source/OCR/save operations.
std::optional<Binding> read(const QString &libraryRoot, qulonglong comicInfoId,
                            const QString &expectedSource, QString *error);
}
#endif
