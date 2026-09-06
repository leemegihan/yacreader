#ifndef YACREADER_ARCHIVE_INSPECTOR_DIALOG_H
#define YACREADER_ARCHIVE_INSPECTOR_DIALOG_H

#include "local_metadata.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class OcrPageView;

class YACReaderArchiveInspectorDialog : public QDialog
{
    Q_OBJECT
public:
    explicit YACReaderArchiveInspectorDialog(QWidget *parent = nullptr);
    ~YACReaderArchiveInspectorDialog() override;
    void inspectComic(const QString &libraryPath, qulonglong comicInfoId);

signals:
    void metadataSaved(const QString &libraryPath, qulonglong comicInfoId);
    void titleSearchRequested(const QString &libraryPath, qulonglong comicInfoId, const QString &title, const QString &author, int pageCount, const QStringList &nameHints, const QStringList &publishers, bool runSearch);

private:
    friend class LocalMetadataTest;
    void start(bool ocr);
    void recognizeRegion();
    void requestSearch(bool automatic);
    LocalMetadata::OcrOptions ocrOptions() const;
    void cancel();
    void showPage(int row);
    void showResult(const LocalMetadata::Result &result);
    void save();
    void setBusy(bool busy);

    QString libraryPath;
    QString sourcePath;
    qulonglong comicInfoId = 0;
    LocalMetadata::Cancellation cancellation;
    LocalMetadata::Result result;
    bool automaticSearchDone = false;

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
    QComboBox *textLayout;
    QComboBox *rotation;
    QCheckBox *invert;
    QCheckBox *threshold;
    QPushButton *regionButton;
    QSpinBox *pageLimit;
    QPushButton *ocrButton;
    QPushButton *cancelButton;
    QPushButton *saveButton;
    QPushButton *searchButton;
};
#endif
