#ifndef LOCAL_METADATA_H
#define LOCAL_METADATA_H

#include <QImage>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <memory>

namespace LocalMetadata {
using Cancellation = std::shared_ptr<std::atomic_bool>;

enum class PageKind { Unknown,
                      TitlePage,
                      Colophon,
                      Afterword };
struct TextLine {
    QString text;
    double confidence = -1;
};
struct Reading {
    QString text;
    QString language;
    QString error;
    QVector<TextLine> lines;
    double confidence = -1;
    double score = -1;
    bool uncertainLanguage = false;
};

struct Page {
    int number = 0;
    QString name;
    QImage image;
    QString text;
    QString error;
    Reading reading;
    PageKind kind = PageKind::Unknown;
};

struct Suggestion {
    enum Field { Title,
                 Author,
                 Publisher };
    Field field = Title;
    QString value;
    QString reason;
    int page = 0; // 0 means a path hint, not image evidence.
    bool labelled = false;
    double confidence = -1;
};

struct Result {
    QVector<Page> pages;
    QVector<Suggestion> suggestions;
    int pageCount = 0;
    QString error;
};

struct OcrOptions {
    QString executable;
    QString dataPath;
    QString language = QStringLiteral("auto");
    bool vertical = false;
    int segmentation = 11;
    int rotation = 0;
    bool invert = false;
    bool adaptiveThreshold = false;
    int timeoutMs = 90000;
};

QVector<int> sampleIndexes(int pageCount, int perEnd);
Result readPages(const QString &path, int perEnd, const Cancellation &cancel);
QVector<Suggestion> suggest(const QVector<Page> &pages, const QString &sourcePath);
QString normalizeOcrText(const QString &text);
PageKind classifyPage(const QString &text, int pageNumber);
QString pageKindName(PageKind kind);
Reading parseTsv(const QByteArray &tsv, const QString &language);
Reading chooseReading(const QVector<Reading> &readings);
Reading recognizePage(const QImage &image, const OcrOptions &options, const Cancellation &cancel);
OcrOptions defaultOcrOptions();
QImage prepareOcrImage(const QImage &image, const OcrOptions &options);
QString recognize(const QImage &image, const OcrOptions &options, const Cancellation &cancel, QString *error);
Result analyze(const QString &path, int perEnd, const OcrOptions &options, const Cancellation &cancel);
QString comicPath(const QString &libraryPath, qulonglong comicInfoId, QString *error);
bool save(const QString &libraryPath, qulonglong comicInfoId, const QString &sourcePath,
          const QString &title, const QString &author, bool overwrite,
          const QVector<Suggestion> &evidence, QString *error);
}

#endif
