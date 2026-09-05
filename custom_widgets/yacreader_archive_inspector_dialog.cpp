#include "yacreader_archive_inspector_dialog.h"

#include "comic.h"
#include "compressed_archive.h"
#include "qnaturalsorting.h"
#include "yacreader_global.h"

#include <QApplication>
#include <QByteArray>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QScrollArea>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>

YACReaderArchiveInspectorDialog::YACReaderArchiveInspectorDialog(QWidget *parent)
    : QDialog(parent)
    , fileLabel(new QLabel(this))
    , statusLabel(new QLabel(this))
    , scrollArea(new QScrollArea(this))
    , pagesWidget(new QWidget(scrollArea))
    , pagesLayout(new QGridLayout(pagesWidget))
{
    setWindowTitle(tr("Inspect comic pages"));
    resize(860, 720);

    fileLabel->setWordWrap(true);
    fileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusLabel->setWordWrap(true);

    pagesLayout->setContentsMargins(8, 8, 8, 8);
    pagesLayout->setSpacing(10);

    scrollArea->setWidgetResizable(true);
    scrollArea->setWidget(pagesWidget);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(fileLabel);
    layout->addWidget(statusLabel);
    layout->addWidget(scrollArea, 1);
    layout->addWidget(buttons);
}

bool YACReaderArchiveInspectorDialog::loadComicFileData(const QString &libraryPath,
                                                        qulonglong comicInfoId,
                                                        ComicFileData *data,
                                                        QString *errorMessage) const
{
    if (data == nullptr)
        return false;

    const QString databasePath = QDir(YACReader::LibraryPaths::libraryDataPath(libraryPath)).filePath(QStringLiteral("library.ydb"));
    if (!QFileInfo::exists(databasePath)) {
        if (errorMessage != nullptr)
            *errorMessage = tr("Library database not found: %1").arg(databasePath);
        return false;
    }

    const QString connectionName = QStringLiteral("yacreader-archive-inspector-%1")
                                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool success = false;
    QString error;

    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=2000"));
        db.setDatabaseName(databasePath);

        if (!db.open()) {
            error = db.lastError().text();
        } else {
            QSqlQuery query(db);
            query.prepare(QStringLiteral(
                    "SELECT c.fileName, c.path "
                    "FROM comic c INNER JOIN comic_info ci ON c.comicInfoId = ci.id "
                    "WHERE ci.id = :comicInfoId"));
            query.bindValue(QStringLiteral(":comicInfoId"), QVariant::fromValue(comicInfoId));

            if (!query.exec()) {
                error = query.lastError().text();
            } else if (!query.next()) {
                error = tr("The comic could not be found in the library database.");
            } else {
                data->fileName = query.value(0).toString();
                data->relativePath = query.value(1).toString();
                success = true;
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);

    if (!success && errorMessage != nullptr)
        *errorMessage = error;
    return success;
}

void YACReaderArchiveInspectorDialog::clearPages()
{
    while (QLayoutItem *item = pagesLayout->takeAt(0)) {
        if (item->widget() != nullptr)
            item->widget()->deleteLater();
        delete item;
    }
}

void YACReaderArchiveInspectorDialog::addPagePreview(int pageNumber, int pageCount, const QByteArray &rawData)
{
    QImage image;
    if (!image.loadFromData(rawData))
        return;

    auto *container = new QWidget(pagesWidget);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto *imageLabel = new QLabel(container);
    imageLabel->setAlignment(Qt::AlignCenter);
    imageLabel->setMinimumSize(210, 290);
    imageLabel->setPixmap(QPixmap::fromImage(image).scaled(
            QSize(240, 330), Qt::KeepAspectRatio, Qt::SmoothTransformation));

    auto *caption = new QLabel(tr("Page %1 of %2").arg(pageNumber).arg(pageCount), container);
    caption->setAlignment(Qt::AlignCenter);

    layout->addWidget(imageLabel, 1);
    layout->addWidget(caption);

    const int index = pagesLayout->count();
    pagesLayout->addWidget(container, index / 3, index % 3);
}

void YACReaderArchiveInspectorDialog::showError(const QString &message)
{
    statusLabel->setText(message);
}

void YACReaderArchiveInspectorDialog::inspectComic(const QString &libraryPath, qulonglong comicInfoId)
{
    clearPages();
    fileLabel->clear();
    statusLabel->setText(tr("Loading first and last pages…"));

    ComicFileData data;
    QString error;
    if (!loadComicFileData(libraryPath, comicInfoId, &data, &error)) {
        showError(error);
        return;
    }

    const QString absolutePath = QDir::cleanPath(libraryPath + data.relativePath);
    fileLabel->setText(data.fileName);

    const QFileInfo fileInfo(absolutePath);
    if (!fileInfo.exists()) {
        showError(tr("Comic file not found: %1").arg(absolutePath));
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);

    CompressedArchive archive(absolutePath);
    if (!archive.toolsLoaded()) {
        QApplication::restoreOverrideCursor();
        showError(tr("The archive backend is not available."));
        return;
    }
    if (!archive.isValid()) {
        QApplication::restoreOverrideCursor();
        showError(tr("This file is not a supported compressed comic archive."));
        return;
    }

    const QList<QString> archiveOrder = archive.getFileNames();
    QList<QString> pages = FileComic::filter(archiveOrder);
    std::sort(pages.begin(), pages.end(), naturalSortLessThanCI);

    if (pages.isEmpty()) {
        QApplication::restoreOverrideCursor();
        showError(tr("No supported image pages were found in this archive."));
        return;
    }

    QVector<int> requestedPages;
    const int pageCount = pages.size();
    const int leadingCount = qMin(3, pageCount);
    for (int i = 0; i < leadingCount; ++i)
        requestedPages.append(i);

    const int trailingStart = qMax(leadingCount, pageCount - 3);
    for (int i = trailingStart; i < pageCount; ++i)
        requestedPages.append(i);

    QSet<int> seenArchiveIndexes;
    int loaded = 0;
    for (const int pageIndex : std::as_const(requestedPages)) {
        const int archiveIndex = archiveOrder.indexOf(pages.at(pageIndex));
        if (archiveIndex < 0 || seenArchiveIndexes.contains(archiveIndex))
            continue;
        seenArchiveIndexes.insert(archiveIndex);

        const QByteArray rawData = archive.getRawDataAtIndex(archiveIndex);
        if (rawData.isEmpty())
            continue;

        addPagePreview(pageIndex + 1, pageCount, rawData);
        ++loaded;
    }

    QApplication::restoreOverrideCursor();

    if (loaded == 0) {
        showError(tr("The selected pages could not be extracted."));
        return;
    }

    statusLabel->setText(tr("Showing %1 page(s). These are the pages the OCR stage will inspect first.").arg(loaded));
}
