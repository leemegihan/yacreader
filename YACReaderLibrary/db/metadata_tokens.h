#ifndef METADATA_TOKENS_H
#define METADATA_TOKENS_H

#include <QRegularExpression>
#include <QStringList>

// Keep the sidebar's token boundaries identical to exact SQL searches.
namespace MetadataTokens {
inline QString separators()
{
    return QStringLiteral("[,;\\n\\r\\x{FF0C}\\x{FF1B}\\x{3001}]");
}

inline QStringList split(const QString &value)
{
    QStringList result;
    const auto parts = value.split(QRegularExpression(separators()), Qt::SkipEmptyParts);
    for (const auto &part : parts) {
        const auto token = part.trimmed();
        if (!token.isEmpty())
            result.append(token);
    }
    return result;
}

inline QString exactPattern(const QString &value)
{
    // Only literal tokens are accepted, never caller-supplied regular expressions.
    // Unicode whitespace and case matching also apply to imported author credits.
    const auto token = value.trimmed();
    if (token.isEmpty() || token.contains(QRegularExpression(separators())))
        return QStringLiteral("(?!)");
    return QStringLiteral("(*UCP)(?i)(?:\\A|%1)\\s*%2\\s*(?=%1|\\z)")
            .arg(separators(), QRegularExpression::escape(token));
}
}

#endif // METADATA_TOKENS_H
