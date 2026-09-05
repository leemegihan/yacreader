#include "yacreader_metadata_browser.h"

#include "yacreader_global.h"

#include <QAbstractItemView>
#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStyle>
#include <QTabWidget>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

YACReaderMetadataBrowser::YACReaderMetadataBrowser(QWidget *parent)
    : QWidget(parent)
    , filterEdit(new QLineEdit(this))
    , tabs(new QTabWidget(this))
    , writersList(new QListWidget(this))
    , tagsList(new QListWidget(this))
    , refreshButton(new QToolButton(this))
{
    filterEdit->setPlaceholderText(tr("Filter authors or tags"));
    filterEdit->setClearButtonEnabled(true);

    refreshButton->setAutoRaise(true);
    refreshButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    refreshButton->setToolTip(tr("Refresh authors and tags"));

    for (auto *list : { writersList, tagsList }) {
        list->setHeaderHidden(true);
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        list->setUniformItemSizes(true);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    tabs->setDocumentMode(true);
    tabs->addTab(writersList, tr("Authors"));
    tabs->addTab(tagsList, tr("Tags"));

    auto *filterLayout = new QHBoxLayout;
    filterLayout->setContentsMargins(5, 0, 5, 4);
    filterLayout->setSpacing(4);
    filterLayout->addWidget(filterEdit, 1);
    filterLayout->addWidget(refreshButton);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(filterLayout);
    layout->addWidget(tabs, 1);

    setMinimumHeight(190);
    setMaximumHeight(300);

    connect(filterEdit, &QLineEdit::textChanged, this, &YACReaderMetadataBrowser::applyFilter);
    connect(refreshButton, &QToolButton::clicked, this, &YACReaderMetadataBrowser::refresh);
    connect(writersList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        activateItem(item, QStringLiteral("writer"));
    });
    connect(tagsList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        activateItem(item, QStringLiteral("tags"));
    });
}

void YACReaderMetadataBrowser::setLibraryPath(const QString &libraryPath)
{
    if (currentLibraryPath == libraryPath)
        return;

    currentLibraryPath = libraryPath;
    filterEdit->clear();
    refresh();
}

QString YACReaderMetadataBrowser::libraryPath() const
{
    return currentLibraryPath;
}

void YACReaderMetadataBrowser::clear()
{
    writersList->clear();
    tagsList->clear();
}

void YACReaderMetadataBrowser::refresh()
{
    clear();

    if (currentLibraryPath.isEmpty())
        return;

    const QString databasePath = QDir(YACReader::LibraryPaths::libraryDataPath(currentLibraryPath)).filePath(QStringLiteral("library.ydb"));
    if (!QFileInfo::exists(databasePath))
        return;

    QHash<QString, FacetEntry> writerEntries;
    QHash<QString, FacetEntry> tagEntries;

    const QString connectionName = QStringLiteral("yacreader-metadata-browser-%1")
                                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));

    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=2000"));
        db.setDatabaseName(databasePath);

        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(QStringLiteral(
                        "SELECT ci.writer, ci.tags "
                        "FROM comic c INNER JOIN comic_info ci ON c.comicInfoId = ci.id"))) {
                const QRegularExpression separator(QStringLiteral("[,;\\n\\r]+"));

                auto addValues = [&separator](const QString &rawValue, QHash<QString, FacetEntry> &entries) {
                    QSet<QString> seenForComic;
                    const auto values = rawValue.split(separator, Qt::SkipEmptyParts);
                    for (const auto &raw : values) {
                        const QString value = raw.trimmed();
                        if (value.isEmpty())
                            continue;

                        const QString key = value.toCaseFolded();
                        if (seenForComic.contains(key))
                            continue;
                        seenForComic.insert(key);

                        auto it = entries.find(key);
                        if (it == entries.end()) {
                            FacetEntry entry;
                            entry.name = value;
                            entry.count = 1;
                            entries.insert(key, entry);
                        } else {
                            ++it->count;
                        }
                    }
                };

                while (query.next()) {
                    addValues(query.value(0).toString(), writerEntries);
                    addValues(query.value(1).toString(), tagEntries);
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);

    populateList(writersList, writerEntries.values());
    populateList(tagsList, tagEntries.values());
    applyFilter(filterEdit->text());
}

void YACReaderMetadataBrowser::populateList(QListWidget *list, QList<FacetEntry> entries)
{
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);

    std::sort(entries.begin(), entries.end(), [&collator](const FacetEntry &left, const FacetEntry &right) {
        return collator.compare(left.name, right.name) < 0;
    });

    for (const auto &entry : entries) {
        auto *item = new QListWidgetItem(QStringLiteral("%1  (%2)").arg(entry.name).arg(entry.count), list);
        item->setData(Qt::UserRole, entry.name);
        item->setToolTip(entry.name);
    }
}

void YACReaderMetadataBrowser::applyFilter(const QString &text)
{
    const QString needle = text.trimmed();
    for (auto *list : { writersList, tagsList }) {
        for (int row = 0; row < list->count(); ++row) {
            auto *item = list->item(row);
            const QString value = item->data(Qt::UserRole).toString();
            item->setHidden(!needle.isEmpty() && !value.contains(needle, Qt::CaseInsensitive));
        }
    }
}

void YACReaderMetadataBrowser::activateItem(QListWidgetItem *item, const QString &field)
{
    if (item == nullptr)
        return;

    QString value = item->data(Qt::UserRole).toString();
    if (value.isEmpty())
        return;

    value.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    value.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    emit searchRequested(QStringLiteral("%1:\"%2\"").arg(field, value));
}
