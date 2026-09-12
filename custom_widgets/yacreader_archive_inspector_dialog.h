#ifndef YACREADER_ARCHIVE_INSPECTOR_DIALOG_H
#define YACREADER_ARCHIVE_INSPECTOR_DIALOG_H

#include "local_metadata.h"
#include "local_ocr_session.h"

#include <QDialog>
#include <QPointer>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QSettings;
class QThread;
class OcrPageView;

// Shared by the copied worker closure; destruction is queued on the UI thread.
// Signals remain safe if the dialog closes while a process is being cancelled.
class OcrProgressRelay : public QObject
{
    Q_OBJECT
signals:
    void changed(int completed, int total, const QString &stage);
};

class YACReaderArchiveInspectorDialog : public QDialog
{
    Q_OBJECT
public:
    explicit YACReaderArchiveInspectorDialog(QWidget *parent = nullptr);
    ~YACReaderArchiveInspectorDialog() override;
    void inspectComic(const QString &libraryPath, qulonglong comicInfoId);

signals:
    void metadataSaved(const QString &libraryPath, qulonglong comicInfoId);
    void titleSearchRequested(const QString &libraryPath, qulonglong comicInfoId, const QString &title, const QString &author, int pageCount, const QStringList &nameHints, const QStringList &publishers, bool runSearch, const QString &evidenceSummary);

private:
    friend class LocalMetadataTest;
    QThread *createWorker(const LocalMetadata::Cancellation &flag, const std::function<void()> &work, const std::function<void()> &completed, bool deliverCancelled = false);
    void start(bool ocr);
    void startSaved(LocalOcrSession::Action action);
    void showSavedResult(const LocalOcrSession::Outcome &output, qint64 elapsedMs, int perEnd);
    void recognizeRegion();
    void requestSearch(bool automatic);
    LocalMetadata::OcrOptions ocrOptions() const;
    void cancel();
    void showPage(int row);
    void showResult(const LocalMetadata::Result &result, bool ocrComplete = false);
    void save();
    void setBusy(bool busy);
    void restorePreferences(const QSettings &settings);
    void savePreferences(QSettings &settings) const;

    QString libraryPath;
    QString sourcePath;
    qulonglong comicInfoId = 0;
    LocalMetadata::Cancellation cancellation;
    QPointer<QThread> activeWorker;
    bool pendingPreview = false;
    LocalMetadata::Result result;
    bool automaticSearchDone = false;
    bool filenameFallback = false;
    std::optional<LocalOcrLibrary::Binding> savedSelection;
    QString savedSourceFingerprint;
    int savedPerEnd = 3;

    QLabel *fileLabel;
    QLabel *statusLabel;
    OcrPageView *imageLabel;
    QListWidget *pageList;
    QListWidget *candidateList;
    QPlainTextEdit *pageText;
    QLineEdit *titleEdit;
    QLineEdit *authorEdit;
    QLineEdit *publisherEdit;
    QCheckBox *autoSearch;
    QLabel *pageInfo;
    QCheckBox *overwrite;
    QComboBox *language;
    QComboBox *quality;
    QComboBox *performance;
    QComboBox *textLayout;
    QComboBox *rotation;
    QCheckBox *invert;
    QCheckBox *threshold;
    QPushButton *regionButton;
    QSpinBox *pageLimit;
    QPushButton *ocrButton;
    QPushButton *resumeButton;
    QPushButton *cancelButton;
    QPushButton *saveButton;
    QPushButton *searchButton;
};
#endif
