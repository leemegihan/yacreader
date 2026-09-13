#ifndef YACREADER_FILENAME_NORMALIZER_H
#define YACREADER_FILENAME_NORMALIZER_H

#include <QString>

class QWidget;

class YACReaderFilenameNormalizer
{
public:
    enum class Result {
        Renamed,
        Unchanged,
        Skipped,
        Failed
    };

    static Result offerRename(QWidget *parent,
                              const QString &libraryPath,
                              qulonglong comicInfoId,
                              QString *errorMessage = nullptr);

private:
    static QString sanitizeSegment(QString segment);
    static QString normalizedFileName(const QString &writer,
                                      const QString &title,
                                      const QString &extension);
};

#endif // YACREADER_FILENAME_NORMALIZER_H
