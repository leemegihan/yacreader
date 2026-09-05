#include "yacreader_metadata_lookup_dialog.h"

#include "yacreader_global.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextBrowser>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QUuid>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {
QString firstNonEmpty(const QStringList &values)
{
    for (const auto &value : values) {
        if (!value.trimmed().isEmpty())
            return value.trimmed();
    }
    return QString();
}

void appendUnique(QStringList &target, QSet<QString> &seen, const QString &value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty())
        return;
    const QString key = trimmed.toCaseFolded();
    if (seen.contains(key))
        return;
    seen.insert(key);
    target.append(trimmed);
}
}

YACReaderMetadataLookupDialog::YACReaderMetadataLookupDialog(QWidget *parent)
    : QDialog(parent)
    , networkManager(new QNetworkAccessManager(this))
    , fileNameLabel(new QLabel(this))
    , existingInfoLabel(new QLabel(this))
    , searchEdit(new QLineEdit(this))
    , searchButton(new QPushButton(tr("Search"), this))
    , statusLabel(new QLabel(this))
    , resultsList(new QListWidget(this))
    , titleChoice(new QComboBox(this))
    , authorsLabel(new QLabel(this))
    , genresLabel(new QLabel(this))
    , formatLabel(new QLabel(this))
    , yearLabel(new QLabel(this))
    , matchLabel(new QLabel(this))
    , tagsEdit(new QPlainTextEdit(this))
    , descriptionView(new QTextBrowser(this))
    , overwriteExisting(new QCheckBox(tr("Overwrite existing title, author and tags"), this))
    , buttonBox(new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this))
{
    setWindowTitle(tr("Find manga metadata"));
    resize(840, 620);

    fileNameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fileNameLabel->setWordWrap(true);
    existingInfoLabel->setWordWrap(true);
    existingInfoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    searchEdit->setClearButtonEnabled(true);
    searchEdit->setPlaceholderText(tr("Title to search"));

    auto *searchLayout = new QHBoxLayout;
    searchLayout->setContentsMargins(0, 0, 0, 0);
    searchLayout->addWidget(searchEdit, 1);
    searchLayout->addWidget(searchButton);

    resultsList->setMinimumWidth(300);
    resultsList->setAlternatingRowColors(true);

    authorsLabel->setWordWrap(true);
    genresLabel->setWordWrap(true);
    tagsEdit->setReadOnly(true);
    tagsEdit->setMaximumHeight(90);
    descriptionView->setOpenExternalLinks(true);
    descriptionView->setMinimumHeight(170);

    auto *detailsForm = new QFormLayout;
    detailsForm->addRow(tr("Title"), titleChoice);
    detailsForm->addRow(tr("Author"), authorsLabel);
    detailsForm->addRow(tr("Genres"), genresLabel);
    detailsForm->addRow(tr("Format"), formatLabel);
    detailsForm->addRow(tr("Year"), yearLabel);
    detailsForm->addRow(tr("Title match"), matchLabel);
    detailsForm->addRow(tr("Tags"), tagsEdit);
    detailsForm->addRow(tr("Description"), descriptionView);

    auto *resultsLayout = new QHBoxLayout;
    resultsLayout->addWidget(resultsList, 2);
    auto *detailsWidget = new QWidget(this);
    detailsWidget->setLayout(detailsForm);
    resultsLayout->addWidget(detailsWidget, 3);

    auto *safeApplyLabel = new QLabel(
            tr("By default, existing metadata is preserved and only blank fields are filled."), this);
    safeApplyLabel->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("File"), this));
    layout->addWidget(fileNameLabel);
    layout->addWidget(existingInfoLabel);
    layout->addSpacing(6);
    layout->addLayout(searchLayout);
    layout->addWidget(statusLabel);
    layout->addLayout(resultsLayout, 1);
    layout->addWidget(safeApplyLabel);
    layout->addWidget(overwriteExisting);
    layout->addWidget(buttonBox);

    buttonBox->button(QDialogButtonBox::Apply)->setEnabled(false);

    connect(searchButton, &QPushButton::clicked, this, &YACReaderMetadataLookupDialog::startSearch);
    connect(searchEdit, &QLineEdit::returnPressed, this, &YACReaderMetadataLookupDialog::startSearch);
    connect(resultsList, &QListWidget::currentRowChanged, this, &YACReaderMetadataLookupDialog::showCandidate);
    connect(buttonBox->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &YACReaderMetadataLookupDialog::applySelectedCandidate);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void YACReaderMetadataLookupDialog::setComic(const QString &libraryPath,
                                              qulonglong comicInfoId,
                                              const QString &fileName,
                                              const QString &currentTitle,
                                              const QString &currentWriter,
                                              const QString &currentTags)
{
    currentLibraryPath = libraryPath;
    currentComicInfoId = comicInfoId;
    currentFileName = fileName;
    existingTitle = currentTitle.trimmed();
    existingWriter = currentWriter.trimmed();
    existingTags = currentTags.trimmed();

    fileNameLabel->setText(fileName);

    QStringList existing;
    if (!existingTitle.isEmpty())
        existing.append(tr("title: %1").arg(existingTitle));
    if (!existingWriter.isEmpty())
        existing.append(tr("author: %1").arg(existingWriter));
    if (!existingTags.isEmpty())
        existing.append(tr("tags: %1").arg(existingTags));
    existingInfoLabel->setText(existing.isEmpty()
                                       ? tr("No existing metadata")
                                       : tr("Existing — %1").arg(existing.join(QStringLiteral(" | "))));

    searchEdit->setText(cleanSearchText(fileName, existingTitle));
    overwriteExisting->setChecked(false);
    clearResults();
    statusLabel->setText(tr("Provider: AniList"));
    searchEdit->setFocus();
}

QString YACReaderMetadataLookupDialog::cleanSearchText(const QString &fileName, const QString &currentTitle)
{
    if (!currentTitle.trimmed().isEmpty())
        return currentTitle.trimmed();

    QString text = QFileInfo(fileName).completeBaseName().trimmed();
    const QRegularExpression leadingGroup(
            QStringLiteral(R"(^\s*(?:\[[^\]]+\]|【[^】]+】|\([^\)]+\))\s*)"));

    while (true) {
        const auto match = leadingGroup.match(text);
        if (!match.hasMatch() || match.capturedLength() == 0)
            break;
        text.remove(0, match.capturedLength());
    }

    text.replace(QLatin1Char('_'), QLatin1Char(' '));
    text.replace(QRegularExpression(QStringLiteral(R"(\s{2,})")), QStringLiteral(" "));
    return text.trimmed();
}

QString YACReaderMetadataLookupDialog::normalizeTitle(const QString &title)
{
    const QString normalized = title.normalized(QString::NormalizationForm_KC).toCaseFolded();
    QString result;
    result.reserve(normalized.size());
    for (const QChar ch : normalized) {
        if (ch.isLetterOrNumber())
            result.append(ch);
    }
    return result;
}

int YACReaderMetadataLookupDialog::levenshteinDistance(const QString &left, const QString &right)
{
    if (left.isEmpty())
        return right.size();
    if (right.isEmpty())
        return left.size();

    QVector<int> previous(right.size() + 1);
    QVector<int> current(right.size() + 1);
    for (int j = 0; j <= right.size(); ++j)
        previous[j] = j;

    for (int i = 1; i <= left.size(); ++i) {
        current[0] = i;
        for (int j = 1; j <= right.size(); ++j) {
            const int cost = left.at(i - 1) == right.at(j - 1) ? 0 : 1;
            current[j] = std::min({ previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost });
        }
        previous.swap(current);
    }
    return previous[right.size()];
}

int YACReaderMetadataLookupDialog::titleSimilarity(const QString &query, const Candidate &candidate)
{
    const QString normalizedQuery = normalizeTitle(query);
    if (normalizedQuery.isEmpty())
        return 0;

    int best = 0;
    const QStringList titles { candidate.romajiTitle, candidate.englishTitle, candidate.nativeTitle };
    for (const auto &title : titles) {
        const QString normalizedCandidate = normalizeTitle(title);
        if (normalizedCandidate.isEmpty())
            continue;
        if (normalizedCandidate == normalizedQuery)
            return 100;

        const int maxLength = std::max(normalizedQuery.size(), normalizedCandidate.size());
        const int distance = levenshteinDistance(normalizedQuery, normalizedCandidate);
        const int score = maxLength == 0 ? 0 : std::max(0, 100 - (distance * 100 / maxLength));
        best = std::max(best, score);
    }
    return best;
}

QString YACReaderMetadataLookupDialog::strippedDescription(const QString &description)
{
    if (description.contains(QLatin1Char('<')))
        return QTextDocumentFragment::fromHtml(description).toPlainText().trimmed();
    return description.trimmed();
}

void YACReaderMetadataLookupDialog::clearResults()
{
    candidates.clear();
    resultsList->clear();
    titleChoice->clear();
    authorsLabel->clear();
    genresLabel->clear();
    formatLabel->clear();
    yearLabel->clear();
    matchLabel->clear();
    tagsEdit->clear();
    descriptionView->clear();
    buttonBox->button(QDialogButtonBox::Apply)->setEnabled(false);
}

void YACReaderMetadataLookupDialog::setBusy(bool busy, const QString &status)
{
    searchEdit->setEnabled(!busy);
    searchButton->setEnabled(!busy);
    resultsList->setEnabled(!busy);
    if (!status.isEmpty())
        statusLabel->setText(status);
}

void YACReaderMetadataLookupDialog::startSearch()
{
    const QString search = searchEdit->text().trimmed();
    if (search.isEmpty() || activeReply != nullptr)
        return;

    clearResults();
    setBusy(true, tr("Searching AniList…"));

    static const QString query = QStringLiteral(R"GRAPHQL(
query ($search: String) {
  Page(page: 1, perPage: 12) {
    media(search: $search, type: MANGA, sort: SEARCH_MATCH) {
      id
      title { romaji english native }
      format
      startDate { year }
      description(asHtml: false)
      genres
      tags { name rank isMediaSpoiler }
      siteUrl
      staff(perPage: 25, sort: RELEVANCE) {
        edges {
          role
          node { name { full native } }
        }
      }
    }
  }
}
)GRAPHQL");

    QJsonObject body;
    body.insert(QStringLiteral("query"), query);
    body.insert(QStringLiteral("variables"), QJsonObject { { QStringLiteral("search"), search } });

    QNetworkRequest request(QUrl(QStringLiteral("https://graphql.anilist.co")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", "YACReader-MangaLibrary/0.2");

    activeReply = networkManager->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(activeReply, &QNetworkReply::finished, this, &YACReaderMetadataLookupDialog::processSearchReply);
    QTimer::singleShot(15000, activeReply, [reply = activeReply] {
        if (reply->isRunning())
            reply->abort();
    });
}

void YACReaderMetadataLookupDialog::processSearchReply()
{
    if (activeReply == nullptr)
        return;

    QNetworkReply *reply = activeReply;
    activeReply = nullptr;
    const QByteArray payload = reply->readAll();
    const auto networkError = reply->error();
    const QString networkErrorText = reply->errorString();
    reply->deleteLater();

    setBusy(false);

    if (networkError != QNetworkReply::NoError) {
        statusLabel->setText(tr("AniList search failed: %1").arg(networkErrorText));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        statusLabel->setText(tr("AniList returned an invalid response."));
        return;
    }

    const QJsonObject root = document.object();
    if (!root.value(QStringLiteral("errors")).toArray().isEmpty()) {
        const auto errors = root.value(QStringLiteral("errors")).toArray();
        const QString message = errors.first().toObject().value(QStringLiteral("message")).toString();
        statusLabel->setText(tr("AniList error: %1").arg(message));
        return;
    }

    const QJsonArray mediaList = root.value(QStringLiteral("data"))
                                         .toObject()
                                         .value(QStringLiteral("Page"))
                                         .toObject()
                                         .value(QStringLiteral("media"))
                                         .toArray();

    for (const auto &mediaValue : mediaList) {
        const QJsonObject media = mediaValue.toObject();
        Candidate candidate;
        candidate.sourceId = media.value(QStringLiteral("id")).toInteger();

        const QJsonObject title = media.value(QStringLiteral("title")).toObject();
        candidate.romajiTitle = title.value(QStringLiteral("romaji")).toString();
        candidate.englishTitle = title.value(QStringLiteral("english")).toString();
        candidate.nativeTitle = title.value(QStringLiteral("native")).toString();
        candidate.format = media.value(QStringLiteral("format")).toString();
        candidate.year = media.value(QStringLiteral("startDate")).toObject().value(QStringLiteral("year")).toInt();
        candidate.description = strippedDescription(media.value(QStringLiteral("description")).toString());
        candidate.siteUrl = media.value(QStringLiteral("siteUrl")).toString();

        for (const auto &genre : media.value(QStringLiteral("genres")).toArray())
            candidate.genres.append(genre.toString());

        struct RankedTag {
            int rank;
            QString name;
        };
        QVector<RankedTag> rankedTags;
        for (const auto &tagValue : media.value(QStringLiteral("tags")).toArray()) {
            const QJsonObject tag = tagValue.toObject();
            if (tag.value(QStringLiteral("isMediaSpoiler")).toBool())
                continue;
            const QString name = tag.value(QStringLiteral("name")).toString().trimmed();
            if (name.isEmpty())
                continue;
            rankedTags.append({ tag.value(QStringLiteral("rank")).toInt(), name });
        }
        std::sort(rankedTags.begin(), rankedTags.end(), [](const RankedTag &left, const RankedTag &right) {
            return left.rank > right.rank;
        });
        for (const auto &tag : rankedTags) {
            if (candidate.tags.size() >= 18)
                break;
            if (tag.rank >= 40 || candidate.tags.size() < 8)
                candidate.tags.append(tag.name);
        }

        QSet<QString> seenAuthors;
        const auto staffEdges = media.value(QStringLiteral("staff"))
                                        .toObject()
                                        .value(QStringLiteral("edges"))
                                        .toArray();
        for (const auto &edgeValue : staffEdges) {
            const QJsonObject edge = edgeValue.toObject();
            const QString role = edge.value(QStringLiteral("role")).toString();
            if (!role.contains(QStringLiteral("Story"), Qt::CaseInsensitive)
                && !role.contains(QStringLiteral("Art"), Qt::CaseInsensitive)
                && !role.contains(QStringLiteral("Creator"), Qt::CaseInsensitive))
                continue;

            const QJsonObject names = edge.value(QStringLiteral("node"))
                                            .toObject()
                                            .value(QStringLiteral("name"))
                                            .toObject();
            const QString name = firstNonEmpty({ names.value(QStringLiteral("native")).toString(),
                                                 names.value(QStringLiteral("full")).toString() });
            appendUnique(candidate.authors, seenAuthors, name);
        }

        candidate.titleSimilarity = titleSimilarity(searchEdit->text(), candidate);
        candidates.append(candidate);
    }

    for (const auto &candidate : candidates) {
        const QString title = firstNonEmpty({ candidate.romajiTitle, candidate.nativeTitle, candidate.englishTitle });
        const QString author = candidate.authors.isEmpty() ? tr("author unknown") : candidate.authors.join(QStringLiteral(", "));
        auto *item = new QListWidgetItem(
                QStringLiteral("%1%  %2\n%3")
                        .arg(candidate.titleSimilarity, 3)
                        .arg(title, author),
                resultsList);
        item->setToolTip(candidate.siteUrl);
    }

    if (candidates.isEmpty()) {
        statusLabel->setText(tr("No AniList matches found. Try changing the search title."));
        return;
    }

    statusLabel->setText(tr("Found %1 candidate(s). Check the title and author before applying.").arg(candidates.size()));
    resultsList->setCurrentRow(0);
}

void YACReaderMetadataLookupDialog::showCandidate(int row)
{
    titleChoice->clear();
    authorsLabel->clear();
    genresLabel->clear();
    formatLabel->clear();
    yearLabel->clear();
    matchLabel->clear();
    tagsEdit->clear();
    descriptionView->clear();

    if (row < 0 || row >= candidates.size()) {
        buttonBox->button(QDialogButtonBox::Apply)->setEnabled(false);
        return;
    }

    const auto &candidate = candidates.at(row);
    const QStringList labels { tr("Romaji"), tr("Native"), tr("English") };
    const QStringList values { candidate.romajiTitle, candidate.nativeTitle, candidate.englishTitle };

    int bestComboIndex = -1;
    int bestScore = -1;
    for (int i = 0; i < values.size(); ++i) {
        const QString value = values.at(i).trimmed();
        if (value.isEmpty())
            continue;
        titleChoice->addItem(QStringLiteral("%1 — %2").arg(labels.at(i), value), value);
        Candidate singleTitleCandidate;
        singleTitleCandidate.romajiTitle = value;
        const int score = titleSimilarity(searchEdit->text(), singleTitleCandidate);
        if (score > bestScore) {
            bestScore = score;
            bestComboIndex = titleChoice->count() - 1;
        }
    }
    if (bestComboIndex >= 0)
        titleChoice->setCurrentIndex(bestComboIndex);

    authorsLabel->setText(candidate.authors.join(QStringLiteral(", ")));
    genresLabel->setText(candidate.genres.join(QStringLiteral(", ")));
    formatLabel->setText(candidate.format);
    yearLabel->setText(candidate.year > 0 ? QString::number(candidate.year) : QString());
    matchLabel->setText(tr("%1% title similarity").arg(candidate.titleSimilarity));
    tagsEdit->setPlainText(candidate.tags.join(QStringLiteral(", ")));
    descriptionView->setPlainText(candidate.description);
    buttonBox->button(QDialogButtonBox::Apply)->setEnabled(!chosenTitle().isEmpty());
}

QString YACReaderMetadataLookupDialog::chosenTitle() const
{
    return titleChoice->currentData().toString().trimmed();
}

QString YACReaderMetadataLookupDialog::databaseFilePath() const
{
    return QDir(YACReader::LibraryPaths::libraryDataPath(currentLibraryPath)).filePath(QStringLiteral("library.ydb"));
}

bool YACReaderMetadataLookupDialog::saveCandidate(const Candidate &candidate, QString *errorMessage)
{
    const QString databasePath = databaseFilePath();
    if (!QFileInfo::exists(databasePath)) {
        if (errorMessage)
            *errorMessage = tr("Library database not found: %1").arg(databasePath);
        return false;
    }

    const QString connectionName = QStringLiteral("yacreader-metadata-lookup-%1")
                                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool success = false;
    QString error;

    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=2000"));
        db.setDatabaseName(databasePath);

        if (!db.open()) {
            error = db.lastError().text();
        } else {
            QSqlQuery update(db);
            const bool overwrite = overwriteExisting->isChecked();
            if (overwrite) {
                update.prepare(QStringLiteral(
                        "UPDATE comic_info SET "
                        "title = CASE WHEN TRIM(:title) <> '' THEN :title ELSE title END, "
                        "writer = CASE WHEN TRIM(:writer) <> '' THEN :writer ELSE writer END, "
                        "genere = CASE WHEN TRIM(:genere) <> '' THEN :genere ELSE genere END, "
                        "tags = CASE WHEN TRIM(:tags) <> '' THEN :tags ELSE tags END, "
                        "format = CASE WHEN TRIM(:format) <> '' THEN :format ELSE format END, "
                        "year = CASE WHEN :year > 0 THEN :year ELSE year END, "
                        "synopsis = CASE WHEN TRIM(:synopsis) <> '' THEN :synopsis ELSE synopsis END, "
                        "edited = 1, lastTimeMetadataSet = :metadataTime "
                        "WHERE id = :id"));
            } else {
                update.prepare(QStringLiteral(
                        "UPDATE comic_info SET "
                        "title = CASE WHEN TRIM(COALESCE(title, '')) = '' AND TRIM(:title) <> '' THEN :title ELSE title END, "
                        "writer = CASE WHEN TRIM(COALESCE(writer, '')) = '' AND TRIM(:writer) <> '' THEN :writer ELSE writer END, "
                        "genere = CASE WHEN TRIM(COALESCE(genere, '')) = '' AND TRIM(:genere) <> '' THEN :genere ELSE genere END, "
                        "tags = CASE WHEN TRIM(COALESCE(tags, '')) = '' AND TRIM(:tags) <> '' THEN :tags ELSE tags END, "
                        "format = CASE WHEN TRIM(COALESCE(format, '')) = '' AND TRIM(:format) <> '' THEN :format ELSE format END, "
                        "year = CASE WHEN COALESCE(year, 0) = 0 AND :year > 0 THEN :year ELSE year END, "
                        "synopsis = CASE WHEN TRIM(COALESCE(synopsis, '')) = '' AND TRIM(:synopsis) <> '' THEN :synopsis ELSE synopsis END, "
                        "edited = 1, lastTimeMetadataSet = :metadataTime "
                        "WHERE id = :id"));
            }

            update.bindValue(QStringLiteral(":title"), chosenTitle());
            update.bindValue(QStringLiteral(":writer"), candidate.authors.join(QStringLiteral(", ")));
            update.bindValue(QStringLiteral(":genere"), candidate.genres.join(QStringLiteral(", ")));
            update.bindValue(QStringLiteral(":tags"), candidate.tags.join(QStringLiteral(", ")));
            update.bindValue(QStringLiteral(":format"), candidate.format);
            update.bindValue(QStringLiteral(":year"), candidate.year);
            update.bindValue(QStringLiteral(":synopsis"), candidate.description);
            update.bindValue(QStringLiteral(":metadataTime"), QDateTime::currentSecsSinceEpoch());
            update.bindValue(QStringLiteral(":id"), QVariant::fromValue(currentComicInfoId));

            if (!db.transaction()) {
                error = db.lastError().text();
            } else if (!update.exec()) {
                error = update.lastError().text();
                db.rollback();
            } else if (!db.commit()) {
                error = db.lastError().text();
                db.rollback();
            } else {
                success = update.numRowsAffected() > 0;
                if (!success)
                    error = tr("No matching comic metadata row was updated.");
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);

    if (!success && errorMessage)
        *errorMessage = error;
    return success;
}

void YACReaderMetadataLookupDialog::applySelectedCandidate()
{
    const int row = resultsList->currentRow();
    if (row < 0 || row >= candidates.size())
        return;

    QString error;
    if (!saveCandidate(candidates.at(row), &error)) {
        statusLabel->setText(tr("Could not save metadata: %1").arg(error));
        return;
    }

    emit metadataSaved(currentComicInfoId);
    accept();
}
