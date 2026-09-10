#ifndef LOCAL_OCR_CACHE_H
#define LOCAL_OCR_CACHE_H

#include "local_metadata.h"

#include <optional>

namespace LocalOcrCache {
// Hashes are lowercase SHA256. The image hash covers the exact prepared PNG;
// preprocessing includes a version and all transforms. Manifest hashes cover
// model/package versions and file hashes. No filename/candidate context belongs
// in this key: those must be interpreted separately after cache retrieval.
struct Identity {
    QString imageSha256;
    QSize preparedSize;
    QString preprocessingFingerprint;
    QString workerSha256;
    QString modelManifestSha256;
    QString packageManifestSha256;
    QString language;
    int cpuThreads = 8;
    QString requestedDevice;
    QString actualDevice;
};
struct Entry {
    LocalMetadata::Reading reading;
    QByteArray rawResult;
    QString resultSha256;
};
QString key(const Identity &identity);
// False means absent; no value means an invalid path/identity/file type. This
// does not validate an existing entry's content; callers must still load it.
std::optional<bool> entryExists(const QString &root, const Identity &identity, QString *error);
// Root must be an existing absolute private cache directory. Missing, invalid or
// mismatched entries return no value. Loading never creates files or runs OCR.
std::optional<Entry> load(const QString &root, const Identity &identity, QString *error);
// Validate with the existing OCR parser, then atomically save an immutable entry.
// Same-byte replay succeeds; conflicting or damaged existing evidence is refused.
// Callers must check success before writing a durable job receipt.
bool save(const QString &root, const Identity &identity, const QByteArray &result, QString *error);
}

#endif
