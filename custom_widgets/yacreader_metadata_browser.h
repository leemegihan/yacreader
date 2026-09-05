#ifndef YACREADER_METADATA_BROWSER_H
#define YACREADER_METADATA_BROWSER_H

#include <QWidget>

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QTabWidget;
class QToolButton;

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

    void clear();
    void populateList(QListWidget *list, QList<FacetEntry> entries);
    void applyFilter(const QString &text);
    void activateItem(QListWidgetItem *item, const QString &field);

    QString currentLibraryPath;
    QLineEdit *filterEdit;
    QTabWidget *tabs;
    QListWidget *writersList;
    QListWidget *tagsList;
    QToolButton *refreshButton;
};

#endif // YACREADER_METADATA_BROWSER_H
