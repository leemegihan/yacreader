#ifndef CATALOG_METADATA_H
#define CATALOG_METADATA_H

#include <QJsonArray>
#include <QStringList>
#include <QUrl>
#include <QVector>

namespace CatalogMetadata {
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
    int pageCount = 0;
    QString provider = QStringLiteral("AniList");
};

QJsonArray galleryReference(const QUrl &url);
QJsonArray galleryReferences(const QString &html);
QVector<Candidate> parseGalleryMetadata(const QByteArray &json, QString *error);
QString plainTitle(QString title);
}
#endif
