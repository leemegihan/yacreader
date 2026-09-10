#ifndef LOCAL_OCR_RUNTIME_H
#define LOCAL_OCR_RUNTIME_H

#include "local_metadata.h"

#include <QJsonObject>

namespace LocalOcrRuntime {
struct Measurement {
    QJsonObject settingsSnapshot;
    QString settingsFingerprint;
    QJsonObject manifests;
};
// Read-only neural-runtime inventory for the personal Windows package. No OCR,
// network, library DB or automatic resume. Keep returned paths/manifests private.
std::optional<Measurement> measure(const QString &applicationPath, const LocalMetadata::OcrOptions &options,
                                   const LocalMetadata::Cancellation &cancel, QString *error);
}
#endif