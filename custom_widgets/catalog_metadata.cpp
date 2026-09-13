#include "catalog_metadata.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

namespace CatalogMetadata {
QJsonArray galleryReference(const QUrl &url)
{
    if (url.scheme() != "https" || (url.host() != "e-hentai.org" && url.host() != "exhentai.org") || !url.userInfo().isEmpty() || url.port(-1) != -1)
        return { };
    const auto match = QRegularExpression(QStringLiteral("^/g/([1-9][0-9]{0,9})/([a-fA-F0-9]{10})/?$")).match(url.path());
    if (!match.hasMatch())
        return { };
    return { match.captured(1).toLongLong(), match.captured(2).toLower() };
}

QJsonArray galleryReferences(const QString &html)
{
    QJsonArray references;
    QSet<qint64> seen;
    const QRegularExpression link(QStringLiteral(R"re(href\s*=\s*["'](https://(?:e-hentai\.org|exhentai\.org)/g/[0-9]+/[a-fA-F0-9]{10}/?)["'])re"), QRegularExpression::CaseInsensitiveOption);
    auto matches = link.globalMatch(html.left(2 * 1024 * 1024));
    while (matches.hasNext() && references.size() < 12) {
        const auto reference = galleryReference(QUrl(matches.next().captured(1)));
        if (reference.isEmpty() || seen.contains(reference.first().toInteger()))
            continue;
        seen.insert(reference.first().toInteger());
        references.append(reference);
    }
    return references;
}

QString plainTitle(QString title)
{
    // Remove catalog wrappers, retaining the original display title separately.
    // Do not infer an artist from a circle, uploader, or bracketed filename.
    const QRegularExpression prefix(QStringLiteral("^\\s*\\[[^\\]]+\\]\\s*(?=\\S)"));
    while (prefix.match(title).hasMatch())
        title.remove(prefix);
    // Only common catalog edition/language suffixes are stripped. Brackets
    // inside an actual title (or a title consisting of brackets) are retained.
    const QRegularExpression suffix(QStringLiteral("\\s+\\[(?:English|Japanese|Korean|Chinese|Digital|Translated|Ongoing|日本語|韓国語|中国語|英訳|한국어|번역)\\]\\s*$"), QRegularExpression::CaseInsensitiveOption);
    while (suffix.match(title).hasMatch())
        title.remove(suffix);
    title.remove(QRegularExpression(QStringLiteral("^\\s*\\(C[0-9]+\\)"), QRegularExpression::CaseInsensitiveOption));
    return title.simplified();
}

QVector<Candidate> parseGalleryMetadata(const QByteArray &json, QString *error)
{
    error->clear();
    QVector<Candidate> result;
    const auto document = QJsonDocument::fromJson(json);
    if (json.size() > 2 * 1024 * 1024 || !document.isObject() || !document.object().value("gmetadata").isArray()) {
        *error = QCoreApplication::translate("CatalogMetadata", "Invalid catalog metadata response.");
        return result;
    }
    for (const auto &value : document.object().value("gmetadata").toArray()) {
        if (result.size() >= 12)
            break;
        const auto object = value.toObject();
        if (object.contains("error"))
            continue;
        Candidate candidate;
        candidate.sourceId = object.value("gid").toInteger();
        const auto url = QUrl(QStringLiteral("https://e-hentai.org/g/%1/%2/").arg(candidate.sourceId).arg(object.value("token").toString()));
        if (galleryReference(url).isEmpty())
            continue;
        candidate.siteUrl = url.toString();
        candidate.provider = "E-Hentai";
        candidate.romajiTitle = object.value("title").toString().left(512).trimmed();
        candidate.nativeTitle = object.value("title_jpn").toString().left(512).trimmed();
        if (candidate.romajiTitle.isEmpty() && candidate.nativeTitle.isEmpty())
            continue;
        bool ok = false;
        candidate.pageCount = object.value("filecount").toVariant().toInt(&ok);
        if (!ok || candidate.pageCount < 0 || candidate.pageCount > 100000)
            candidate.pageCount = 0;
        candidate.format = object.value("category").toString().left(80);
        QSet<QString> seen;
        for (const auto &tag : object.value("tags").toArray()) {
            const auto text = tag.toString().trimmed();
            if (text.isEmpty() || text.size() > 160 || seen.contains(text.toCaseFolded()))
                continue;
            seen.insert(text.toCaseFolded());
            candidate.tags.append(text);
            if (text.startsWith("artist:", Qt::CaseInsensitive)) {
                const auto name = text.mid(7).trimmed();
                if (!name.isEmpty())
                    candidate.authors.append(name);
            }
            if (candidate.tags.size() >= 100)
                break;
        }
        result.append(candidate);
    }
    if (result.isEmpty())
        *error = QCoreApplication::translate("CatalogMetadata", "No accessible metadata was returned. Check the source link and access conditions.");
    return result;
}
}
