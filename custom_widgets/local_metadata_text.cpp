#include "local_metadata.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace LocalMetadata {
namespace {
QString tr(const char *text)
{
    return QCoreApplication::translate("LocalMetadata", text);
}
enum class Role { None,
                  Title,
                  Author,
                  Publisher,
                  Date,
                  Contact,
                  Printer,
                  Thanks,
                  Translator,
                  Colophon,
                  Afterword };
QString key(QString text)
{
    text = text.normalized(QString::NormalizationForm_KC).toCaseFolded();
    text.remove(QRegularExpression(QStringLiteral("[\\s:：・·/]+")));
    return text;
}
Role role(const QString &text)
{
    static const QMap<QString, Role> roles {
        { "タイトル", Role::Title }, { "作品名", Role::Title }, { "書名", Role::Title }, { "제목", Role::Title }, { "작품명", Role::Title }, { "title", Role::Title }, { "著者", Role::Author }, { "作者", Role::Author }, { "著作", Role::Author }, { "原作", Role::Author }, { "作画", Role::Author }, { "漫画", Role::Author }, { "작가", Role::Author }, { "저자", Role::Author }, { "글그림", Role::Author }, { "글", Role::Author }, { "그림", Role::Author }, { "author", Role::Author }, { "writer", Role::Author }, { "artist", Role::Author }, { "story&art", Role::Author }, { "storyandart", Role::Author }, { "発行者", Role::Publisher }, { "発行所", Role::Publisher }, { "発行", Role::Publisher }, { "サークル", Role::Publisher }, { "발행인", Role::Publisher }, { "발행자", Role::Publisher }, { "출판사", Role::Publisher }, { "publisher", Role::Publisher }, { "circle", Role::Publisher }, { "発行日", Role::Date }, { "発行年月日", Role::Date }, { "발행일", Role::Date }, { "publicationdate", Role::Date }, { "連絡先", Role::Contact }, { "연락처", Role::Contact }, { "contact", Role::Contact }, { "印刷所", Role::Printer }, { "印刷", Role::Printer }, { "인쇄소", Role::Printer }, { "printer", Role::Printer }, { "specialthanks", Role::Thanks }, { "thanks", Role::Thanks }, { "謝辞", Role::Thanks }, { "翻訳", Role::Translator }, { "번역", Role::Translator }, { "식자", Role::Translator }, { "translator", Role::Translator }, { "奥付", Role::Colophon }, { "판권", Role::Colophon }, { "colophon", Role::Colophon }, { "あとがき", Role::Afterword }, { "後書き", Role::Afterword }, { "후기", Role::Afterword }, { "afterword", Role::Afterword }
    };
    return roles.value(key(text), Role::None);
}
struct Label {
    Role role = Role::None;
    QString value;
};
Label label(const QString &raw)
{
    const auto line = raw.normalized(QString::NormalizationForm_KC).simplified();
    const auto direct = role(line);
    if (direct != Role::None)
        return { direct, { } };
    const int colon = line.indexOf(QLatin1Char(':'));
    if (colon > 0 && role(line.left(colon)) != Role::None)
        return { role(line.left(colon)), line.mid(colon + 1).trimmed() };
    // Boundaries must be present; do not match a role word buried in dialogue.
    for (int i = line.size() - 1; i > 0; --i) {
        if (line.at(i).isSpace() && role(line.left(i)) != Role::None)
            return { role(line.left(i)), line.mid(i + 1).trimmed() };
    }
    return { };
}
bool usable(const QString &value)
{
    int letters = 0;
    for (const auto ch : value)
        letters += ch.isLetter();
    return value.size() >= 2 && value.size() <= 180 && letters >= 2 && !value.contains(QRegularExpression(QStringLiteral("(?:https?://|www\\.|@)"), QRegularExpression::CaseInsensitiveOption));
}
}

QString normalizeOcrText(const QString &text)
{
    auto normalized = text.normalized(QString::NormalizationForm_KC);
    // Japanese OCR commonly emits one token per character. Korean word spaces
    // and English word boundaries must remain intact.
    normalized.replace(QRegularExpression(QStringLiteral("(?<=[\\x{3040}-\\x{30ff}\\x{3400}-\\x{9fff}])[ \\t]+(?=[\\x{3040}-\\x{30ff}\\x{3400}-\\x{9fff}])")), QString());
    return normalized.normalized(QString::NormalizationForm_KC).trimmed();
}

PageKind classifyPage(const QString &text, int pageNumber)
{
    QSet<int> roles;
    for (const auto &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
        roles.insert(int(label(line).role));
    if (roles.contains(int(Role::Colophon)) || ((roles.contains(int(Role::Title)) || roles.contains(int(Role::Author)) || roles.contains(int(Role::Publisher))) && (roles.contains(int(Role::Date)) || roles.contains(int(Role::Contact)) || roles.contains(int(Role::Printer)))))
        return PageKind::Colophon;
    if (roles.contains(int(Role::Afterword)))
        return PageKind::Afterword;
    if (pageNumber <= 3 && roles.contains(int(Role::Title)))
        return PageKind::TitlePage;
    return PageKind::Unknown;
}

QString pageKindName(PageKind kind)
{
    switch (kind) {
    case PageKind::Colophon:
        return tr("판권 후보");
    case PageKind::Afterword:
        return tr("후기 후보");
    case PageKind::TitlePage:
        return tr("표지·속표지 후보");
    default:
        return tr("본문 / 미분류");
    }
}

QVector<Suggestion> suggest(const QVector<Page> &pages, const QString &sourcePath)
{
    QVector<Suggestion> result;
    auto append = [&](Suggestion::Field field, QString value, const QString &reason, int page, bool labelled = false, double confidence = -1) {
        value = normalizeOcrText(value).simplified();
        if (!usable(value))
            return;
        for (const auto &existing : result) {
            if (existing.field == field && existing.value.compare(value, Qt::CaseInsensitive) == 0 && existing.page == page)
                return;
        }
        result.append({ field, value, reason, page, labelled, confidence });
    };
    for (const auto &page : pages) {
        const auto lines = page.text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (int i = 0; i < lines.size(); ++i) {
            auto credit = label(lines.at(i));
            if (credit.role != Role::Title && credit.role != Role::Author && credit.role != Role::Publisher)
                continue;
            if (credit.value.isEmpty()) {
                // A new label is a hard boundary. Never consume a contact,
                // printer or thanks entry as the preceding title/author.
                if (i + 1 >= lines.size() || label(lines.at(i + 1)).role != Role::None)
                    continue;
                credit.value = lines.at(++i);
            }
            const auto field = credit.role == Role::Title ? Suggestion::Title : credit.role == Role::Author ? Suggestion::Author
                                                                                                            : Suggestion::Publisher;
            const QString reason = field == Suggestion::Publisher ? tr("발행자·서클 라벨 — 개인 작가로 확정하지 않습니다.") : tr("명시된 제목·작가 라벨과 연결된 글자입니다. 원본과 대조해 주세요.");
            double confidence = -1;
            // Match normalized lines rather than assuming blank-line counts in
            // TSV and plaintext are identical. Unknown confidence is explicit.
            for (const auto &line : page.reading.lines) {
                if (normalizeOcrText(line.text).contains(normalizeOcrText(credit.value))) {
                    confidence = line.confidence;
                    break;
                }
            }
            append(field, credit.value, reason, page.number, true, confidence);
        }
    }
    const QFileInfo info(sourcePath);
    QString base = info.isDir() ? info.fileName() : info.completeBaseName();
    // Edition markers before [name] must not hide the bracketed name hint.
    const QRegularExpression decoration(QStringLiteral("^\\s*(?:\\([^)]*\\)|【[^】]*】)\\s*"));
    while (decoration.match(base).hasMatch())
        base.remove(decoration);
    const auto bracket = QRegularExpression(QStringLiteral(R"(^\[([^\]]+)\]\s*(.+)$)")).match(base);
    if (bracket.hasMatch()) {
        append(Suggestion::Author, bracket.captured(1), tr("파일명 이름 힌트 — 외부 artist 태그와 일치하는지 확인합니다."), 0);
        base = bracket.captured(2);
    }
    append(Suggestion::Title, base, tr("파일명·폴더명 힌트 — OCR로 확인한 제목이 아닙니다."), 0);
    const QString parent = info.dir().dirName();
    const QStringList generic { "downloads", "download", "comics", "manga", "images", "pictures", "만화", "다운로드", "미분류", "unidentified", "temp" };
    if (!generic.contains(parent.toCaseFolded()))
        append(Suggestion::Author, parent, tr("부모 폴더 이름 힌트 — 작가가 아닐 수 있습니다."), 0);
    return result;
}

Reading parseTsv(const QByteArray &tsv, const QString &language)
{
    Reading reading;
    reading.language = language;
    if (tsv.size() > 4 * 1024 * 1024 || !tsv.startsWith("level\tpage_num\t")) {
        reading.error = tr("OCR 위치·신뢰도 응답을 읽지 못했습니다.");
        return reading;
    }
    QString currentKey;
    QStringList words;
    double sum = 0, lineSum = 0;
    int weight = 0, lineWeight = 0;
    auto finishLine = [&] {
        if (!words.isEmpty()) {
            const auto text = words.join(QLatin1Char(' '));
            reading.lines.append({ text, lineWeight ? lineSum / lineWeight : -1 });
            reading.text += text + QLatin1Char('\n');
        }
        words.clear();
        lineSum = 0;
        lineWeight = 0;
    };
    for (const auto &row : tsv.split('\n')) {
        const auto columns = row.split('\t');
        if (columns.size() < 12 || columns[0] != "5")
            continue;
        const QString word = QString::fromUtf8(columns[11]).trimmed();
        if (word.isEmpty())
            continue;
        const QString lineKey = QString::fromLatin1(columns[1] + ":" + columns[2] + ":" + columns[3] + ":" + columns[4]);
        if (lineKey != currentKey) {
            finishLine();
            currentKey = lineKey;
        }
        words.append(word);
        bool ok = false;
        const double confidence = columns[10].toDouble(&ok);
        if (ok && std::isfinite(confidence) && confidence >= 0 && confidence <= 100) {
            sum += confidence * word.size();
            weight += word.size();
            lineSum += confidence * word.size();
            lineWeight += word.size();
        }
        if (reading.lines.size() > 3000 || reading.text.size() > 50000)
            break;
    }
    finishLine();
    reading.text = reading.text.trimmed();
    reading.confidence = weight ? sum / weight : -1;
    int letters = 0, hangul = 0, kana = 0;
    for (const auto ch : reading.text) {
        letters += ch.isLetter();
        hangul += ch.unicode() >= 0xac00 && ch.unicode() <= 0xd7a3;
        kana += ch.unicode() >= 0x3040 && ch.unicode() <= 0x30ff;
    }
    const double script = letters ? double(language.startsWith("kor") ? hangul : kana) / letters : 0;
    // Scores are a ranking heuristic, not calibrated cross-language accuracy.
    reading.score = reading.confidence < 0 || letters < 2 ? -1 : reading.confidence + qMin(15.0, script * 30.0) + qMin(5.0, letters / 10.0);
    if (reading.score >= 0 && classifyPage(reading.text, 0) == PageKind::Colophon)
        reading.score += 8;
    return reading;
}

Reading chooseReading(const QVector<Reading> &readings)
{
    QVector<Reading> valid;
    QStringList errors;
    for (const auto &reading : readings) {
        if (reading.error.isEmpty() && !reading.text.isEmpty())
            valid.append(reading);
        else if (!reading.error.isEmpty())
            errors.append(reading.language + ": " + reading.error);
    }
    if (valid.isEmpty()) {
        Reading empty;
        empty.error = errors.join(QLatin1Char('\n'));
        return empty;
    }
    std::stable_sort(valid.begin(), valid.end(), [](const Reading &a, const Reading &b) { return a.score > b.score; });
    auto best = valid.first();
    best.uncertainLanguage = valid.size() < 2 || best.score < 45 || (valid.size() > 1 && best.score - valid.at(1).score < 5 && normalizeOcrText(best.text) != normalizeOcrText(valid.at(1).text));
    if (!errors.isEmpty())
        best.error = tr("일부 언어 판독 실패. 사용 가능한 결과만 표시합니다.\n") + errors.join(QLatin1Char('\n'));
    return best;
}
}
