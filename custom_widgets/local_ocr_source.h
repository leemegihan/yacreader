#ifndef LOCAL_OCR_SOURCE_H
#define LOCAL_OCR_SOURCE_H

#include "local_metadata.h"

#include <QJsonObject>

namespace LocalOcrSource {
struct Snapshot {
    LocalMetadata::Result source;
    QJsonObject manifest; // Private names, selected raw byte hashes and location.
    QString fingerprint;
};
// Read exactly one work, at most its first/last three pages. No OCR, writes or
// operating database access. Reread before resuming and compare the fingerprint.
// Identity covers all page names and selected raw bytes, not interior page text.
std::optional<Snapshot> read(const QString &path, int perEnd,
                             const LocalMetadata::Cancellation &cancel, QString *error);
}
#endif
