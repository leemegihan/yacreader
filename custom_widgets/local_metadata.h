#ifndef LOCAL_METADATA_H
#define LOCAL_METADATA_H

#include <QImage>
#include <QRect>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

namespace LocalMetadata {
using Cancellation = std::shared_ptr<std::atomic_bool>;
using Progress = std::function<void(int completed, int total, const QString &stage)>;

enum class PageKind { Unknown,
                      TitlePage,
                      Colophon,
                      Afterword };
struct TextLine {
    QString text;
    double confidence = -1;
    QRect bounds;
    QString language;
};
struct Reading {
    QString text;
    QString language;
    QString error;
    QVector<TextLine> lines;
    QVector<TextLine> alternatives;
    double confidence = -1;
    double score = -1;
    bool uncertainLanguage = false;
    bool reviewRequired = false;
    QString engine;
    QString device;
    QString warning;
    qint64 elapsedMs = 0;
    qint64 initializationMs = 0;
};

enum class RecognitionStatus { Complete,
                               Failed,
                               Cancelled,
                               CleanupFailed };

// In-memory execution outcome, not a durable cache receipt. Valid blank pages
// remain valid even when the worker later fails. Callers must check status.
struct RecognitionBatch {
    QVector<Reading> readings;
    QVector<bool> validPages;
    RecognitionStatus status = RecognitionStatus::Failed;
    QString error;
    QStringList attemptErrors;
    bool gpuStarted = false;
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

struct SuggestionEvidence {
    int page = 0;
    bool labelled = false;
    double confidence = -1;
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
    QVector<SuggestionEvidence> evidence;
};

struct Result {
    QVector<Page> pages;
    QVector<Suggestion> suggestions;
    int pageCount = 0;
    QString error;
};

struct OcrOptions {
    bool neural = false;
    bool gpu = false;
    int cpuThreads = 8;
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
QVector<Suggestion> suggest(const QVector<Page> &pages, const QString &sourcePath, const QString &libraryRoot = { });
QString suggestionSource(const Suggestion &suggestion);
QString normalizeOcrText(const QString &text);
PageKind classifyPage(const QString &text, int pageNumber);
QString pageKindName(PageKind kind);
Reading parseNeuralReading(const QByteArray &json, const QSize &imageSize);
Reading parseTsv(const QByteArray &tsv, const QString &language);
Reading chooseReading(const QVector<Reading> &readings);
Reading recognizePage(const QImage &image, const OcrOptions &options, const Cancellation &cancel);
RecognitionBatch recognizePagesWithOutcome(const QVector<QImage> &images, const OcrOptions &options, const Cancellation &cancel, const Progress &progress = { });
QVector<Reading> recognizePages(const QVector<QImage> &images, const OcrOptions &options, const Cancellation &cancel, const Progress &progress = { });
OcrOptions defaultOcrOptions();
QImage prepareOcrImage(const QImage &image, const OcrOptions &options);
QString recognize(const QImage &image, const OcrOptions &options, const Cancellation &cancel, QString *error);
Result analyze(const QString &path, int perEnd, const OcrOptions &options, const Cancellation &cancel, const Progress &progress = { });
QString comicPath(const QString &libraryPath, qulonglong comicInfoId, QString *error);
bool save(const QString &libraryPath, qulonglong comicInfoId, const QString &sourcePath,
          const QString &title, const QString &author, bool overwrite,
          const QVector<Suggestion> &evidence, QString *error);
}

#endif
