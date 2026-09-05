#ifndef YACREADER_METADATA_LOOKUP_DIALOG_H
#define YACREADER_METADATA_LOOKUP_DIALOG_H

#include <QDialog>
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

    void setComic(const QString &libraryPath,
                  qulonglong comicInfoId,
                  const QString &fileName,
                  const QString &currentTitle,
                  const QString &currentWriter,
                  const QString &currentTags);

signals:
    void metadataSaved(qulonglong comicInfoId);

private slots:
    void startSearch();
    void processSearchReply();
    void showCandidate(int row);
    void applySelectedCandidate();

private:
    struct Candidate {
        qint64 sourceId = 0;
        QString romajiTitle;
        QString englishTitle;
        QString nativeTitle;
        QStringList authors;
        QStringList genres;
        QStringList tags;
        QString format;
        int year = 0;
        QString description;
        QString siteUrl;
        int titleSimilarity = 0;
    };

    static QString cleanSearchText(const QString &fileName, const QString &existingTitle);
    static QString normalizeTitle(const QString &title);
    static int titleSimilarity(const QString &query, const Candidate &candidate);
    static int levenshteinDistance(const QString &left, const QString &right);
    static QString strippedDescription(const QString &description);

    void setBusy(bool busy, const QString &status = QString());
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
