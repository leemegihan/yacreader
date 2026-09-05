#ifndef YACREADER_ARCHIVE_INSPECTOR_DIALOG_H
#define YACREADER_ARCHIVE_INSPECTOR_DIALOG_H

#include <QDialog>
#include <QString>

class QGridLayout;
class QLabel;
class QScrollArea;

class YACReaderArchiveInspectorDialog : public QDialog
{
    Q_OBJECT
public:
    explicit YACReaderArchiveInspectorDialog(QWidget *parent = nullptr);

    void inspectComic(const QString &libraryPath, qulonglong comicInfoId);

private:
    struct ComicFileData {
        QString fileName;
        QString relativePath;
    };

    bool loadComicFileData(const QString &libraryPath,
                           qulonglong comicInfoId,
                           ComicFileData *data,
                           QString *errorMessage) const;
    void clearPages();
    void addPagePreview(int pageNumber, int pageCount, const QByteArray &rawData);
    void showError(const QString &message);

    QLabel *fileLabel;
    QLabel *statusLabel;
    QScrollArea *scrollArea;
    QWidget *pagesWidget;
    QGridLayout *pagesLayout;
};

#endif // YACREADER_ARCHIVE_INSPECTOR_DIALOG_H
