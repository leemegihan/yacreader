#include "yacreader_metadata_browser.h"

#include "yacreader_archive_inspector_dialog.h"
#include "yacreader_filename_normalizer.h"
#include "yacreader_global.h"
#include "yacreader_metadata_lookup_dialog.h"

#include <QAbstractItemView>
#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStyle>
#include <QTabWidget>
#include <QToolButton>
#include <QUuid>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>

YACReaderMetadataBrowser::YACReaderMetadataBrowser(QWidget *parent)
    : QWidget(parent)
    , filterEdit(new QLineEdit(this))
    , tabs(new QTabWidget(this))
    , writersList(new QListWidget(this))
    , tagsList(new QListWidget(this))
    , unidentifiedList(new QListWidget(this))
    , refreshButton(new QToolButton(this))
    , lookupButton(new QPushButton(tr("Find metadata…"), this))
    , inspectButton(new QPushButton(tr("Inspect first / last pages…"), this))
    , lookupDialog(new YACReaderMetadataLookupDialog(this))
    , archiveInspectorDialog(new YACReaderArchiveInspectorDialog(this))
{
    filterEdit->setPlaceholderText(tr("Filter authors, tags or unidentified comics"));
    filterEdit->setClearButtonEnabled(true);

    refreshButton->setAutoRaise(true);
    refreshButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    refreshButton->setToolTip(tr("Refresh metadata browser"));

    for (auto *list : { writersList, tagsList, unidentifiedList }) {
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        list->setUniformItemSizes(true);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    tabs->setDocumentMode(true);
    tabs->addTab(writersList, tr("Authors"));
    tabs->addTab(tagsList, tr("Tags"));
    tabs->addTab(unidentifiedList, tr("Unidentified"));

    auto *filterLayout = new QHBoxLayout;
    filterLayout->setContentsMargins(5, 0, 5, 4);
    filterLayout->setSpacing(4);
    filterLayout->addWidget(filterEdit, 1);
    filterLayout->addWidget(refreshButton);

    auto *unidentifiedActions = new QHBoxLayout;
    unidentifiedActions->setContentsMargins(5, 4, 5, 5);
    unidentifiedActions->setSpacing(4);
    unidentifiedActions->addWidget(lookupButton, 1);
    unidentifiedActions->addWidget(inspectButton, 1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(filterLayout);
    layout->addWidget(tabs, 1);
    layout->addLayout(unidentifiedActions);

    setMinimumHeight(220);
    setMaximumHeight(380);

    connect(filterEdit, &QLineEdit::textChanged, this, &YACReaderMetadataBrowser::applyFilter);
    connect(refreshButton, &QToolButton::clicked, this, &YACReaderMetadataBrowser::refresh);
    connect(writersList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        activateItem(item, QStringLiteral("writer"));
    });
    connect(tagsList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        activateItem(item, QStringLiteral("tags"));
    });
    connect(unidentifiedList, &QListWidget::itemSelectionChanged, this, &YACReaderMetadataBrowser::updateLookupButtons);
    connect(unidentifiedList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *, int) {
        openMetadataLookup();
    });
    connect(tabs, &QTabWidget::currentChanged, this, [this](int) { updateLookupButtons(); });
    connect(lookupButton, &QPushButton::clicked, this, &YACReaderMetadataBrowser::openMetadataLookup);
    connect(inspectButton, &QPushButton::clicked, this, &YACReaderMetadataBrowser::openArchiveInspector);
    connect(lookupDialog, &YACReaderMetadataLookupDialog::metadataSaved, this, [this](qulonglong comicInfoId) {
        QString renameError;
        const auto renameResult = YACReaderFilenameNormalizer::offerRename(
                this, currentLibraryPath, comicInfoId, &renameError);
        if (renameResult == YACReaderFilenameNormalizer::Result::Failed && !renameError.isEmpty())
            QMessageBox::warning(this, tr("Rename failed"), renameError);

        refresh();

        // The metadata browser lives in a shared widget library, so keep it
        // decoupled from LibraryWindow's header while still refreshing the
        // current YACReaderLibrary model after metadata or a file path changes.
        if (auto *topLevelWindow = window())
            QMetaObject::invokeMethod(topLevelWindow, "reloadCurrentLibrary", Qt::QueuedConnection);
    });

    updateLookupButtons();
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
    unidentifiedList->clear();
    tabs->setTabText(tabs->indexOf(unidentifiedList), tr("Unidentified"));
    updateLookupButtons();
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
    populateUnidentified();
    tabs->setTabText(tabs->indexOf(unidentifiedList), tr("Unidentified (%1)").arg(unidentifiedList->count()));
    applyFilter(filterEdit->text());
    updateLookupButtons();
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
        item->setData(FilterTextRole, entry.name);
        item->setToolTip(entry.name);
    }
}

void YACReaderMetadataBrowser::populateUnidentified()
{
    if (currentLibraryPath.isEmpty())
        return;

    const QString databasePath = QDir(YACReader::LibraryPaths::libraryDataPath(currentLibraryPath)).filePath(QStringLiteral("library.ydb"));
    if (!QFileInfo::exists(databasePath))
        return;

    const QString connectionName = QStringLiteral("yacreader-unidentified-browser-%1")
                                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=2000"));
        db.setDatabaseName(databasePath);

        if (db.open()) {
            QSqlQuery query(db);
            query.prepare(QStringLiteral(
                    "SELECT ci.id, c.fileName, ci.title, ci.writer, ci.tags "
                    "FROM comic c INNER JOIN comic_info ci ON c.comicInfoId = ci.id "
                    "WHERE TRIM(COALESCE(ci.title, '')) = '' "
                    "   OR TRIM(COALESCE(ci.writer, '')) = '' "
                    "   OR TRIM(COALESCE(ci.tags, '')) = '' "
                    "ORDER BY c.fileName COLLATE NOCASE"));

            if (query.exec()) {
                while (query.next()) {
                    const qulonglong comicInfoId = query.value(0).toULongLong();
                    const QString fileName = query.value(1).toString();
                    const QString title = query.value(2).toString().trimmed();
                    const QString writer = query.value(3).toString().trimmed();
                    const QString tags = query.value(4).toString().trimmed();

                    QStringList missing;
                    if (title.isEmpty())
                        missing.append(tr("title"));
                    if (writer.isEmpty())
                        missing.append(tr("author"));
                    if (tags.isEmpty())
                        missing.append(tr("tags"));

                    const QString displayTitle = title.isEmpty() ? fileName : title;
                    auto *item = new QListWidgetItem(
                            QStringLiteral("%1\n%2")
                                    .arg(displayTitle, tr("Missing: %1").arg(missing.join(QStringLiteral(", ")))),
                            unidentifiedList);
                    item->setData(ComicInfoIdRole, QVariant::fromValue(comicInfoId));
                    item->setData(FileNameRole, fileName);
                    item->setData(TitleRole, title);
                    item->setData(WriterRole, writer);
                    item->setData(TagsRole, tags);
                    item->setData(FilterTextRole, QStringLiteral("%1 %2 %3 %4").arg(fileName, title, writer, tags));
                    item->setToolTip(fileName);
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
}

void YACReaderMetadataBrowser::applyFilter(const QString &text)
{
    const QString needle = text.trimmed();
    for (auto *list : { writersList, tagsList, unidentifiedList }) {
        for (int row = 0; row < list->count(); ++row) {
            auto *item = list->item(row);
            const QString value = item->data(FilterTextRole).toString();
            item->setHidden(!needle.isEmpty() && !value.contains(needle, Qt::CaseInsensitive));
        }
    }
    updateLookupButtons();
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

void YACReaderMetadataBrowser::updateLookupButtons()
{
    const bool onUnidentifiedTab = tabs->currentWidget() == unidentifiedList;
    const bool hasVisibleSelection = onUnidentifiedTab && unidentifiedList->currentItem() != nullptr
            && !unidentifiedList->currentItem()->isHidden();

    lookupButton->setVisible(onUnidentifiedTab);
    inspectButton->setVisible(onUnidentifiedTab);
    lookupButton->setEnabled(hasVisibleSelection);
    inspectButton->setEnabled(hasVisibleSelection);
}

void YACReaderMetadataBrowser::openMetadataLookup()
{
    auto *item = unidentifiedList->currentItem();
    if (item == nullptr || currentLibraryPath.isEmpty())
        return;

    lookupDialog->setComic(
            currentLibraryPath,
            item->data(ComicInfoIdRole).toULongLong(),
            item->data(FileNameRole).toString(),
            item->data(TitleRole).toString(),
            item->data(WriterRole).toString(),
            item->data(TagsRole).toString());
    lookupDialog->open();
    lookupDialog->raise();
    lookupDialog->activateWindow();
}

void YACReaderMetadataBrowser::openArchiveInspector()
{
    auto *item = unidentifiedList->currentItem();
    if (item == nullptr || currentLibraryPath.isEmpty())
        return;

    archiveInspectorDialog->inspectComic(
            currentLibraryPath,
            item->data(ComicInfoIdRole).toULongLong());
    archiveInspectorDialog->open();
    archiveInspectorDialog->raise();
    archiveInspectorDialog->activateWindow();
}
