#ifndef YACREADER_METADATA_BROWSER_H
#define YACREADER_METADATA_BROWSER_H

#include <QWidget>

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTabWidget;
class QToolButton;
class YACReaderArchiveInspectorDialog;
class YACReaderMetadataLookupDialog;

class YACReaderMetadataBrowser : public QWidget
{
    Q_OBJECT
public:
    explicit YACReaderMetadataBrowser(QWidget *parent = nullptr);

    void setLibraryPath(const QString &libraryPath);
    QString libraryPath() const;

signals:
    void searchRequested(const QString &query);

public slots:
    void refresh();

private:
    struct FacetEntry {
        QString name;
        int count = 0;
    };

    enum ComicDataRole {
        ComicInfoIdRole = Qt::UserRole + 10,
        FileNameRole,
        TitleRole,
        WriterRole,
        TagsRole,
        FilterTextRole
    };

    void clear();
    void populateList(QListWidget *list, QList<FacetEntry> entries);
    void populateUnidentified();
    void applyFilter(const QString &text);
    void activateItem(QListWidgetItem *item, const QString &field);
    void updateLookupButtons();
    void openMetadataLookup();
    void openArchiveInspector();

    QString currentLibraryPath;
    QLineEdit *filterEdit;
    QTabWidget *tabs;
    QListWidget *writersList;
    QListWidget *tagsList;
    QListWidget *unidentifiedList;
    QToolButton *refreshButton;
    QPushButton *lookupButton;
    QPushButton *inspectButton;
    YACReaderMetadataLookupDialog *lookupDialog;
    YACReaderArchiveInspectorDialog *archiveInspectorDialog;
};

#endif // YACREADER_METADATA_BROWSER_H
