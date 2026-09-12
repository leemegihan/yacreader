#ifndef LOCAL_METADATA_H
#define LOCAL_METADATA_H

#include <QImage>
#include <QRect>
#include <QStringList>
#include <QTransform>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

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

// Coordinates refer to the decoded (possibly cropped) input, after file EXIF
// handling. Pixel edges use [0,width] / [0,height], not QRect::right()/bottom().
struct OcrGeometry {
    QSize inputSize;
    QSize preparedSize;
    QTransform inputToPrepared;
    QString revision;
};
struct PreparedOcrPage {
    QImage image;
    OcrGeometry geometry;
};
struct NeuralPageEvidence {
    int selectedIndex = -1; // Zero-based position in the caller's selected images.
    OcrGeometry geometry;
    QString imageSha256; // Exact PNG bytes passed to this attempt's worker.
    QByteArray rawResult;
    QString resultSha256;
    QString requestedDevice; // Per-attempt request; CPU fallback records cpu.
    QString actualDevice;
};

// Called synchronously on the recognition thread for each validated page.
// Copy evidence if retaining it. False or an exception stops this session;
// it does not authorize an OCR retry. Never assume Complete from a receipt.
using PageSink = std::function<bool(const NeuralPageEvidence &, QString *error)>;

enum class RecognitionStatus { Complete,
                               Failed,
                               Cancelled,
                               CleanupFailed,
                               DeliveryFailed };

// In-memory execution outcome, not a durable cache receipt. Valid blank pages
// remain valid even when the worker later fails. Callers must check status.
struct RecognitionBatch {
    QVector<Reading> readings;
    QVector<bool> validPages;
    // Optional for non-neural/injected runners. Presence is page evidence only,
    // not a successful session, immutable cache entry or durable DB receipt.
    QVector<std::optional<NeuralPageEvidence>> evidence;
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
    QString sourceSha256; // Exact encoded page bytes, before decoding.
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
    QStringList pageNames; // Complete ordered image-entry list, kept private.
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
RecognitionBatch recognizePagesWithOutcome(const QVector<QImage> &images, const OcrOptions &options, const Cancellation &cancel, const Progress &progress = { }, const PageSink &sink = { });
QVector<Reading> recognizePages(const QVector<QImage> &images, const OcrOptions &options, const Cancellation &cancel, const Progress &progress = { });
OcrOptions defaultOcrOptions();
PreparedOcrPage prepareOcrPage(const QImage &image, const OcrOptions &options);
QImage prepareOcrImage(const QImage &image, const OcrOptions &options);
bool validOcrGeometry(const OcrGeometry &geometry);
// Presentation only: map a prepared box onto the decoded page with optional crop.
// OCR Reading bounds remain in prepared coordinates for candidate interpretation.
std::optional<QRectF> mapOcrBoundsToPage(const OcrGeometry &geometry, const QRectF &bounds,
                                         const QSize &decodedSize, const QRect &crop = { });
bool validNeuralEvidence(const NeuralPageEvidence &evidence);
QString recognize(const QImage &image, const OcrOptions &options, const Cancellation &cancel, QString *error);
Result analyze(const QString &path, int perEnd, const OcrOptions &options, const Cancellation &cancel, const Progress &progress = { });
QString comicPath(const QString &libraryPath, qulonglong comicInfoId, QString *error);
// Fresh identity/source checks for a selected persisted review, under the library maintenance lock.
struct SaveGuard {
    QString libraryGeneration;
    QString comicId;
    QString sourceFingerprint;
    int perEnd = 3;
};
bool save(const QString &libraryPath, qulonglong comicInfoId, const QString &sourcePath,
          const QString &title, const QString &author, bool overwrite,
          const QVector<Suggestion> &evidence, QString *error, const SaveGuard *guard = nullptr);
}

#endif
