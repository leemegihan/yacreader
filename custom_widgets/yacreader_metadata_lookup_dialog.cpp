#include "yacreader_metadata_lookup_dialog.h"

#include "library_maintenance_lock.h"
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
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
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
    : QDialog(parent), networkManager(new QNetworkAccessManager(this)), fileNameLabel(new QLabel(this)), existingInfoLabel(new QLabel(this)), searchEdit(new QLineEdit(this)), searchButton(new QPushButton(tr("Search"), this)), statusLabel(new QLabel(this)), resultsList(new QListWidget(this)), titleChoice(new QComboBox(this)), authorsLabel(new QLabel(this)), genresLabel(new QLabel(this)), formatLabel(new QLabel(this)), yearLabel(new QLabel(this)), matchLabel(new QLabel(this)), tagsEdit(new QPlainTextEdit(this)), descriptionView(new QTextBrowser(this)), overwriteExisting(new QCheckBox(tr("Overwrite existing title, author and tags"), this)), buttonBox(new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this))
{
    setWindowTitle(tr("Find manga metadata"));
    resize(840, 620);

    fileNameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fileNameLabel->setWordWrap(true);
    existingInfoLabel->setWordWrap(true);
    existingInfoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    searchEdit->setClearButtonEnabled(true);
    searchEdit->setPlaceholderText(tr("Title to search"));
    searchEdit->setMaxLength(512);
    authorEdit = new QLineEdit(this);
    authorEdit->setMaxLength(160);
    authorEdit->setPlaceholderText(tr("작가 (선택 사항 · 대조용)"));
    providerChoice = new QComboBox(this);
    providerChoice->addItem("AniList");
    providerChoice->addItem("E-Hentai");

    auto *searchLayout = new QHBoxLayout;
    searchLayout->setContentsMargins(0, 0, 0, 0);
    searchLayout->addWidget(searchEdit, 1);
    searchLayout->addWidget(providerChoice);
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
    layout->addWidget(authorEdit);
    auto *privacyLabel = new QLabel(tr("조회하면 제목·작가 또는 입력한 작품 링크를 선택한 서비스에 전송합니다. 페이지 이미지는 전송하지 않습니다."), this);
    privacyLabel->setWordWrap(true);
    layout->addWidget(privacyLabel);
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
    connect(this, &QDialog::finished, this, &YACReaderMetadataLookupDialog::cancelSearch);
    connect(providerChoice, &QComboBox::currentIndexChanged, this, [this] {
        cancelSearch();
        clearResults();
        searchEdit->setPlaceholderText(providerChoice->currentIndex() == 0 ? tr("Title to search") : tr("제목 또는 작품 링크 (작가만 입력해도 조회 가능)"));
        statusLabel->setText(tr("조회 서비스를 선택했습니다. 결과를 확인한 뒤 적용해 주세요."));
    });
}

void YACReaderMetadataLookupDialog::setComic(const QString &libraryPath,
                                             qulonglong comicInfoId,
                                             const QString &fileName,
                                             const QString &currentTitle,
                                             const QString &currentWriter,
                                             const QString &currentTags)
{
    cancelSearch();
    currentLibraryPath = libraryPath;
    currentComicInfoId = comicInfoId;
    currentFileName = fileName;
    existingTitle = currentTitle.trimmed();
    existingWriter = currentWriter.trimmed();
    existingTags = currentTags.trimmed();
    authorEdit->setText(existingWriter);
    sourcePageCount = 0;

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
    statusLabel->setText(tr("Provider: %1").arg(providerChoice->currentText()));
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
    const QString normalized = title.left(512).normalized(QString::NormalizationForm_KC).toCaseFolded();
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
    authorEdit->setEnabled(!busy);
    providerChoice->setEnabled(!busy);
    searchButton->setEnabled(!busy);
    resultsList->setEnabled(!busy);
    if (!status.isEmpty())
        statusLabel->setText(status);
}

void YACReaderMetadataLookupDialog::cancelSearch()
{
    if (activeReply != nullptr) {
        // abort() can emit finished synchronously. Detach the old request before
        // aborting so it cannot populate a newly selected comic's results.
        QNetworkReply *reply = activeReply;
        activeReply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    setBusy(false);
}

void YACReaderMetadataLookupDialog::searchTitle(const QString &title)
{
    cancelSearch();
    searchEdit->setText(title.trimmed());
    startSearch();
}

void YACReaderMetadataLookupDialog::prepareOcrSearch(const QString &title, const QString &author, int pageCount)
{
    cancelSearch();
    clearResults();
    providerChoice->setCurrentIndex(1);
    searchEdit->setText(title);
    authorEdit->setText(author);
    sourcePageCount = qMax(0, pageCount);
    statusLabel->setText(tr("OCR 후보를 가져왔습니다. 제목·작가를 확인하고 조회를 눌러 주세요."));
}

void YACReaderMetadataLookupDialog::watchReply()
{
    activeReply->setReadBufferSize(2 * 1024 * 1024 + 1);
    connect(activeReply, &QNetworkReply::finished, this, &YACReaderMetadataLookupDialog::processSearchReply);
    connect(activeReply, &QNetworkReply::downloadProgress, activeReply, [reply = activeReply](qint64 received, qint64 total) {
        if (received > 2 * 1024 * 1024 || total > 2 * 1024 * 1024)
            reply->abort();
    });
    QTimer::singleShot(15000, activeReply, [reply = activeReply] {
        if (reply->isRunning())
            reply->abort();
    });
}

void YACReaderMetadataLookupDialog::requestGalleryMetadata(const QJsonArray &references)
{
    requestKind = RequestKind::GalleryMetadata;
    QNetworkRequest request(QUrl(QStringLiteral("https://api.e-hentai.org/api.php")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    const QJsonObject body { { "method", "gdata" }, { "gidlist", references }, { "namespace", 1 } };
    setBusy(true, tr("작품 메타데이터를 대조하는 중…"));
    activeReply = networkManager->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    watchReply();
}

void YACReaderMetadataLookupDialog::startSearch()
{
    const QString search = searchEdit->text().trimmed();
    if ((search.isEmpty() && (providerChoice->currentIndex() == 0 || authorEdit->text().trimmed().isEmpty())) || activeReply != nullptr)
        return;

    clearResults();
    if (providerChoice->currentIndex() == 1) {
        if (lastGalleryLookup.isValid() && lastGalleryLookup.elapsed() < 5000) {
            statusLabel->setText(tr("서비스 요청 간격을 위해 잠시 후 다시 조회해 주세요."));
            return;
        }
        lastGalleryLookup.start();
        const auto reference = CatalogMetadata::galleryReference(QUrl(search));
        if (!reference.isEmpty()) {
            requestGalleryMetadata(QJsonArray { reference });
            return;
        }
        if (search.contains("://")) {
            statusLabel->setText(tr("지원하는 작품 링크 형식이 아닙니다."));
            return;
        }
        // Only retrieve the bounded text index and metadata. Never follow image
        // URLs or import browser credentials. Access challenges stop the lookup.
        requestKind = RequestKind::GallerySearch;
        QString term = search;
        term.remove(QLatin1Char('"'));
        QString author = authorEdit->text().trimmed();
        author.remove(QLatin1Char('"'));
        QString query = term.isEmpty() ? QString() : QStringLiteral("title:\"%1\"").arg(term);
        if (!author.isEmpty())
            query += QStringLiteral(" artist:\"%1$\"").arg(author);
        QUrl url(QStringLiteral("https://e-hentai.org/"));
        QUrlQuery parameters;
        parameters.addQueryItem("f_search", query.trimmed());
        url.setQuery(parameters);
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        setBusy(true, tr("작품 목록을 조회하는 중…"));
        activeReply = networkManager->get(request);
        watchReply();
        return;
    }
    requestKind = RequestKind::AniList;
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
    watchReply();
}

void YACReaderMetadataLookupDialog::processSearchReply()
{
    if (activeReply == nullptr || sender() != activeReply)
        return;

    QNetworkReply *reply = activeReply;
    activeReply = nullptr;
    const QByteArray payload = reply->read(2 * 1024 * 1024 + 1);
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto networkError = reply->error();
    const QString networkErrorText = reply->errorString();
    reply->deleteLater();

    setBusy(false);

    if (networkError != QNetworkReply::NoError || (httpStatus != 0 && httpStatus != 200) || payload.size() > 2 * 1024 * 1024) {
        statusLabel->setText(tr("조회 실패 (%1): %2. 접근 제한이나 응답 크기를 확인해 주세요.").arg(httpStatus).arg(networkErrorText));
        return;
    }
    if (requestKind == RequestKind::GallerySearch) {
        const auto references = CatalogMetadata::galleryReferences(QString::fromUtf8(payload));
        if (references.isEmpty()) {
            statusLabel->setText(tr("조회 가능한 작품을 찾지 못했습니다. 제목·작가 철자, 서비스 접근 상태를 확인하거나 작품 링크를 입력해 주세요."));
            return;
        }
        requestGalleryMetadata(references);
        return;
    }
    if (requestKind == RequestKind::GalleryMetadata) {
        QString error;
        candidates = CatalogMetadata::parseGalleryMetadata(payload, &error);
        if (!error.isEmpty()) {
            statusLabel->setText(error);
            return;
        }
        for (auto &candidate : candidates) {
            Candidate comparison = candidate;
            comparison.romajiTitle = CatalogMetadata::plainTitle(candidate.romajiTitle);
            comparison.nativeTitle = CatalogMetadata::plainTitle(candidate.nativeTitle);
            candidate.titleSimilarity = titleSimilarity(searchEdit->text(), comparison);
        }
        displayResults();
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
            if (!role.contains(QStringLiteral("Story"), Qt::CaseInsensitive) && !role.contains(QStringLiteral("Art"), Qt::CaseInsensitive) && !role.contains(QStringLiteral("Creator"), Qt::CaseInsensitive))
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

    displayResults();
}

void YACReaderMetadataLookupDialog::displayResults()
{
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) { return a.titleSimilarity > b.titleSimilarity; });
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
        statusLabel->setText(tr("일치하는 작품이 없습니다. 조회할 제목을 조정해 보세요."));
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
    const QStringList values = candidate.provider == "E-Hentai"
            ? QStringList { CatalogMetadata::plainTitle(candidate.romajiTitle), CatalogMetadata::plainTitle(candidate.nativeTitle), candidate.englishTitle }
            : QStringList { candidate.romajiTitle, candidate.nativeTitle, candidate.englishTitle };

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
    QStringList evidence { tr("제목 유사도 %1% (정답 확률 아님)").arg(candidate.titleSimilarity) };
    if (!authorEdit->text().trimmed().isEmpty()) {
        bool matches = false;
        for (const auto &author : candidate.authors)
            matches = matches || normalizeTitle(author) == normalizeTitle(authorEdit->text());
        evidence.append(matches ? tr("작가 표기 일치") : tr("작가 표기 불일치 또는 확인 불가"));
    }
    if (sourcePageCount > 0 && candidate.pageCount > 0)
        evidence.append(tr("페이지 수: 내 파일 %1 / 후보 %2").arg(sourcePageCount).arg(candidate.pageCount));
    matchLabel->setText(evidence.join(QStringLiteral("\n")));
    matchLabel->setWordWrap(true);
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
    LibraryMaintenanceLock maintenance(currentLibraryPath);
    if (!maintenance.tryLock()) {
        if (errorMessage)
            *errorMessage = maintenance.errorString();
        return false;
    }
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
            } else {
                success = update.numRowsAffected() > 0;
                if (!success)
                    error = tr("No matching comic metadata row was updated.");
                QSqlQuery provenance(db);
                if (success)
                    success = provenance.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS catalog_metadata_evidence (id INTEGER PRIMARY KEY, comicInfoId INTEGER, provider TEXT, sourceId INTEGER, sourceUrl TEXT, reviewedTitle TEXT, reviewedAuthors TEXT, reviewedTags TEXT, created INTEGER)"));
                if (success) {
                    provenance.prepare(QStringLiteral("INSERT INTO catalog_metadata_evidence (comicInfoId,provider,sourceId,sourceUrl,reviewedTitle,reviewedAuthors,reviewedTags,created) VALUES (?,?,?,?,?,?,?,?)"));
                    provenance.addBindValue(QVariant::fromValue(currentComicInfoId));
                    provenance.addBindValue(candidate.provider);
                    provenance.addBindValue(candidate.sourceId);
                    provenance.addBindValue(candidate.siteUrl);
                    provenance.addBindValue(chosenTitle());
                    provenance.addBindValue(candidate.authors.join(QStringLiteral(", ")));
                    provenance.addBindValue(candidate.tags.join(QStringLiteral(", ")));
                    provenance.addBindValue(QDateTime::currentSecsSinceEpoch());
                    success = provenance.exec();
                }
                if (!success) {
                    if (error.isEmpty())
                        error = provenance.lastError().text();
                    db.rollback();
                } else if (!db.commit()) {
                    error = db.lastError().text();
                    success = false;
                    db.rollback();
                }
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

    const QString savedLibraryPath = currentLibraryPath;
    const qulonglong savedComicInfoId = currentComicInfoId;
    accept();
    emit metadataSaved(savedLibraryPath, savedComicInfoId);
}
