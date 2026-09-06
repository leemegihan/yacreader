#ifndef YACREADER_METADATA_LOOKUP_DIALOG_H
#define YACREADER_METADATA_LOOKUP_DIALOG_H

#include "catalog_metadata.h"

#include <QDialog>
#include <QElapsedTimer>
#include <QStringList>
#include <QVector>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;

class YACReaderMetadataLookupDialog : public QDialog
{
    Q_OBJECT
public:
    explicit YACReaderMetadataLookupDialog(QWidget *parent = nullptr);
    void searchTitle(const QString &title);
    void prepareOcrSearch(const QString &title, const QString &author, int pageCount);

    void setComic(const QString &libraryPath,
                  qulonglong comicInfoId,
                  const QString &fileName,
                  const QString &currentTitle,
                  const QString &currentWriter,
                  const QString &currentTags);

signals:
    void metadataSaved(const QString &libraryPath, qulonglong comicInfoId);

private slots:
    void startSearch();
    void processSearchReply();
    void showCandidate(int row);
    void applySelectedCandidate();

private:
    friend class MetadataWorkflowTest;

    using Candidate = CatalogMetadata::Candidate;
    enum class RequestKind { AniList,
                             GallerySearch,
                             GalleryMetadata };
    RequestKind requestKind = RequestKind::AniList;
    QElapsedTimer lastGalleryLookup;
    void watchReply();
    void requestGalleryMetadata(const QJsonArray &references);
    void displayResults();

    static QString cleanSearchText(const QString &fileName, const QString &existingTitle);
    static QString normalizeTitle(const QString &title);
    static int titleSimilarity(const QString &query, const Candidate &candidate);
    static int levenshteinDistance(const QString &left, const QString &right);
    static QString strippedDescription(const QString &description);

    void setBusy(bool busy, const QString &status = QString());
    void cancelSearch();
    void clearResults();
    QString chosenTitle() const;
    bool saveCandidate(const Candidate &candidate, QString *errorMessage);
    QString databaseFilePath() const;

    QString currentLibraryPath;
    qulonglong currentComicInfoId = 0;
    QString currentFileName;
    QString existingTitle;
    QString existingWriter;
    QString existingTags;

    QNetworkAccessManager *networkManager;
    QNetworkReply *activeReply = nullptr;
    QVector<Candidate> candidates;

    QLabel *fileNameLabel;
    QLabel *existingInfoLabel;
    QLineEdit *searchEdit;
    QLineEdit *authorEdit;
    QComboBox *providerChoice;
    int sourcePageCount = 0;
    QPushButton *searchButton;
    QLabel *statusLabel;
    QListWidget *resultsList;
    QComboBox *titleChoice;
    QLabel *authorsLabel;
    QLabel *genresLabel;
    QLabel *formatLabel;
    QLabel *yearLabel;
    QLabel *matchLabel;
    QPlainTextEdit *tagsEdit;
    QTextBrowser *descriptionView;
    QCheckBox *overwriteExisting;
    QDialogButtonBox *buttonBox;
};

#endif // YACREADER_METADATA_LOOKUP_DIALOG_H
