#include "local_metadata.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
                  PublisherAuthor,
                  AuthorOrCircle,
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
    const auto normalized = key(text);
    if (QStringList { "誌名", "책명" }.contains(normalized))
        return Role::Title;
    if (QStringList { "지음", "지은이", "글쓴이", "글·그림", "글그림" }.contains(normalized))
        return Role::Author;
    if (QStringList { "発行著者", "発行作者", "발행저자", "발행작가" }.contains(normalized))
        return Role::PublisherAuthor;
    if (QStringList { "作者サークル名", "著者サークル名" }.contains(normalized))
        return Role::AuthorOrCircle;
    static const QMap<QString, Role> roles {
        { "タイトル", Role::Title }, { "作品名", Role::Title }, { "書名", Role::Title }, { "제목", Role::Title }, { "작품명", Role::Title }, { "title", Role::Title }, { "著者", Role::Author }, { "作者", Role::Author }, { "著作", Role::Author }, { "原作", Role::Author }, { "作画", Role::Author }, { "漫画", Role::Author }, { "작가", Role::Author }, { "저자", Role::Author }, { "글그림", Role::Author }, { "글", Role::Author }, { "그림", Role::Author }, { "author", Role::Author }, { "writer", Role::Author }, { "artist", Role::Author }, { "story&art", Role::Author }, { "storyandart", Role::Author }, { "発行者", Role::Publisher }, { "発行所", Role::Publisher }, { "発行", Role::Publisher }, { "サークル", Role::Publisher }, { "発行サークル", Role::Publisher }, { "발행인", Role::Publisher }, { "발행자", Role::Publisher }, { "출판사", Role::Publisher }, { "publisher", Role::Publisher }, { "circle", Role::Publisher }, { "発行日", Role::Date }, { "発行年月日", Role::Date }, { "발행일", Role::Date }, { "publicationdate", Role::Date }, { "連絡先", Role::Contact }, { "연락처", Role::Contact }, { "contact", Role::Contact }, { "印刷所", Role::Printer }, { "印刷", Role::Printer }, { "인쇄소", Role::Printer }, { "printer", Role::Printer }, { "specialthanks", Role::Thanks }, { "thanks", Role::Thanks }, { "謝辞", Role::Thanks }, { "翻訳", Role::Translator }, { "번역", Role::Translator }, { "식자", Role::Translator }, { "translator", Role::Translator }, { "奥付", Role::Colophon }, { "판권", Role::Colophon }, { "colophon", Role::Colophon }, { "あとがき", Role::Afterword }, { "後書き", Role::Afterword }, { "후기", Role::Afterword }, { "afterword", Role::Afterword }
    };
    return roles.value(key(text), Role::None);
}
struct Label {
    Role role = Role::None;
    QString value;
    bool suffix = false;
};
Label label(const QString &raw)
{
    const auto line = raw.normalized(QString::NormalizationForm_KC).simplified();
    const auto direct = role(line);
    if (direct != Role::None)
        return { direct, { }, key(line) == QStringLiteral("지음") };
    const auto suffix = QRegularExpression(QStringLiteral("^(.+?)\\s+(지음|글[·/]그림|著)$")).match(line);
    if (suffix.hasMatch())
        return { Role::Author, suffix.captured(1), true };
    // Bracketed credits need an inline value. A standalone "[Circle]"
    // must not attach to the next OCR line, which may be a social-media label.
    const auto bracketed = QRegularExpression(QStringLiteral(R"(^\[([^\[\]]+)\]\s*(.+)$)")).match(line);
    if (bracketed.hasMatch() && role(bracketed.captured(1)) != Role::None && !bracketed.captured(2).trimmed().isEmpty())
        return { role(bracketed.captured(1)), bracketed.captured(2).trimmed() };
    // Colophons also separate a role and its value with a filled circle.
    // Match the complete role before the first delimiter; punctuation in a
    // name or a role word inside dialogue must not create another field.
    const int delimiter = line.indexOf(QRegularExpression(QStringLiteral("[:●◉]")));
    if (delimiter > 0 && role(line.left(delimiter)) != Role::None)
        return { role(line.left(delimiter)), line.mid(delimiter + 1).trimmed() };
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

bool usableCredit(const QString &value, Role field)
{
    if (!usable(value))
        return false;
    if (field == Role::Title)
        return true;
    // A readable sentence is not a person's name. Do not promote stray OCR
    // role words in dialogue to identity evidence.
    static const QRegularExpression sentence(QStringLiteral("[!?！？。…]|(?:합니다|입니다|습니다|할게요|할까요|하세요|해요|어요|아요|네요|예요|이에요)[.!。]*$"));
    return value.size() <= 80 && !sentence.match(value.trimmed()).hasMatch();
}

struct Credit {
    Role role = Role::None;
    QString value;
    QRect bounds;
    double confidence = -1;
    QString layoutText;
    bool roleUncertain = false;
};

QVector<Credit> credits(const Page &page)
{
    const auto text = page.text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    auto lineFor = [&](int i) {
        for (const auto &line : page.reading.lines)
            if (line.text == text.at(i))
                return line;
        return TextLine { text.at(i), -1, { } };
    };
    auto close = [](const QRect &a, const QRect &b, bool above) {
        if (a.isEmpty() || b.isEmpty())
            return true; // Legacy plain-text fixtures have no layout evidence.
        const int unit = qMax(a.height(), b.height());
        const bool overlap = b.left() <= a.right() && a.left() <= b.right();
        if (above)
            return overlap && b.top() < a.top() && a.top() - b.bottom() <= 2 * unit;
        return (overlap && b.top() >= a.top() && b.top() - a.bottom() <= 2 * unit) || (b.left() >= a.right() && b.left() - a.right() <= 3 * unit && b.top() <= a.bottom() && a.top() <= b.bottom());
    };
    const auto kind = classifyPage(page.text, page.number);
    QVector<Credit> result;
    for (int i = 0; i < text.size(); ++i) {
        auto marked = label(text.at(i));
        if (marked.role != Role::Title && marked.role != Role::Author && marked.role != Role::Publisher && marked.role != Role::PublisherAuthor && marked.role != Role::AuthorOrCircle)
            continue;
        const auto anchor = lineFor(i);
        QRect bounds = anchor.bounds;
        double confidence = anchor.confidence;
        QString layoutText = anchor.text;
        if (marked.value.isEmpty()) {
            // Short role words can themselves be ordinary speech or a false
            // recognition of art. Require a metadata context for these words.
            if (kind == PageKind::Unknown && QStringList { "글", "그림", "漫画" }.contains(key(text.at(i))))
                continue;
            const int next = marked.suffix ? i - 1 : i + 1;
            if (next < 0 || next >= text.size() || label(text.at(next)).role != Role::None)
                continue;
            const auto valueLine = lineFor(next);
            if (!close(anchor.bounds, valueLine.bounds, marked.suffix))
                continue;
            marked.value = valueLine.text;
            bounds = bounds.united(valueLine.bounds);
            confidence = valueLine.confidence;
            layoutText = valueLine.text;
            // Korean covers may put family/pen-name components on two lines
            // immediately above "지음". Geometry is required for this extension.
            if (marked.suffix && next > 0 && !valueLine.bounds.isEmpty()) {
                const auto previous = lineFor(next - 1);
                if (label(previous.text).role == Role::None && !previous.bounds.isEmpty() && close(valueLine.bounds, previous.bounds, true) && previous.bounds.height() <= 1.5 * valueLine.bounds.height() && valueLine.bounds.height() <= 1.5 * previous.bounds.height() && usableCredit(previous.text, Role::Author)) {
                    marked.value = previous.text + QLatin1Char(' ') + marked.value;
                    bounds = bounds.united(previous.bounds);
                    confidence = qMin(confidence, previous.confidence);
                }
            }
        }
        if (marked.role == Role::AuthorOrCircle) {
            // A shared form field does not establish whether a readable name
            // denotes a person or a circle. Offer alternatives for review only.
            if (usableCredit(marked.value, Role::Author) && !marked.value.contains(QRegularExpression(QStringLiteral("[/／]")))) {
                result.append({ Role::Author, marked.value, bounds, confidence, layoutText, true });
                result.append({ Role::Publisher, marked.value, bounds, confidence, layoutText, true });
            }
        } else if (marked.role == Role::PublisherAuthor) {
            // Order follows the two labels. One missing side must not turn
            // the publisher into an author (or vice versa).
            const auto names = marked.value.split(QRegularExpression(QStringLiteral("[/／]")), Qt::KeepEmptyParts);
            if (names.size() == 2) {
                if (usableCredit(names[0].trimmed(), Role::Publisher))
                    result.append({ Role::Publisher, names[0].trimmed(), bounds, confidence, layoutText });
                if (usableCredit(names[1].trimmed(), Role::Author))
                    result.append({ Role::Author, names[1].trimmed(), bounds, confidence, layoutText });
            }
        } else if (usableCredit(marked.value, marked.role))
            result.append({ marked.role, marked.value, bounds, confidence, layoutText });
    }
    return result;
}

bool genericPathName(QString value)
{
    value = value.normalized(QString::NormalizationForm_KC).toCaseFolded();
    value.remove(QRegularExpression(QStringLiteral("[\\s_.\\-]+")));
    value.remove(QRegularExpression(QStringLiteral("^[0-9]+|[0-9]+$")));
    static const QSet<QString> generic { "", "downloads", "download", "comics", "comic", "manga", "images", "image", "pictures", "library", "testlibrary", "test", "tests", "sample", "samples", "archive", "archives", "folder", "books", "book", "temp", "tmp", "unidentified", "unknown", "desktop", "documents", "만화", "다운로드", "미분류", "테스트", "새폴더", "라이브러리", "자료", "漫画", "未分類" };
    if (generic.contains(value) || QStringList { "japanese", "korean", "english", "한국어", "일본어", "영어", "日本語", "翻訳" }.contains(value))
        return true;
    // Number/language/storage labels are organizational names, not artists.
    return QRegularExpression(QStringLiteral("^(?:ko|kr|jp|ja|en|zh|mixed)(?:archive|folder|comic|manga|sample|test)$")).match(value).hasMatch();
}

QVector<TextLine> coverTitleLines(const Page &page)
{
    // Layout alone cannot prove identity. Only the first page, with a detected
    // author credit, may contribute an explicitly unlabelled review candidate.
    if (page.number != 1 || !page.error.isEmpty() || page.reading.uncertainLanguage || classifyPage(page.text, page.number) != PageKind::Unknown)
        return { };
    int creditSize = 0;
    for (const auto &credit : credits(page)) {
        if (credit.role == Role::Author && !credit.roleUncertain && credit.confidence >= 70 && !credit.bounds.isEmpty()) {
            // A slanted credit has an inflated bounding-box height. Estimate
            // its glyph size from the advance as well, so it cannot dwarf the
            // actual cover lettering merely because it is on an angle.
            double units = 0;
            for (const auto ch : credit.layoutText)
                units += ch.isSpace() ? 0.3 : ch.unicode() >= 0x2e80 ? 1.0
                                                                     : 0.55;
            const bool vertical = credit.bounds.height() > 1.5 * credit.bounds.width();
            const int advance = vertical ? credit.bounds.height() : credit.bounds.width();
            const int size = qMin(qMin(credit.bounds.width(), credit.bounds.height()), int(std::ceil(advance / qMax(1.0, units))));
            creditSize = qMax(creditSize, size);
        }
    }
    if (!creditSize)
        return { };
    QVector<TextLine> titles;
    for (const auto &line : page.reading.lines) {
        if (label(line.text).role != Role::None || !usable(line.text) || line.bounds.isEmpty())
            continue;
        if (qMin(line.bounds.width(), line.bounds.height()) < 1.5 * creditSize)
            continue;
        // Do not silently drop a weak/sentence-like part and present the
        // remaining pieces as a complete title.
        if (line.confidence < 70 || line.text.contains(QRegularExpression(QStringLiteral("[。！？!?…]"))))
            return { };
        titles.append(line);
    }
    if (titles.isEmpty() || titles.size() > 8)
        return { };
    const bool vertical = titles.first().bounds.height() > 1.5 * titles.first().bounds.width();
    for (const auto &line : titles)
        if ((line.bounds.height() > 1.5 * line.bounds.width()) != vertical)
            return { };
    std::sort(titles.begin(), titles.end(), [vertical](const TextLine &a, const TextLine &b) {
        return vertical ? a.bounds.left() > b.bounds.left() : a.bounds.top() < b.bounds.top();
    });
    for (int i = 1; i < titles.size(); ++i) {
        const auto a = titles.at(i - 1).bounds;
        const auto b = titles.at(i).bounds;
        const int unit = vertical ? qMax(a.width(), b.width()) : qMax(a.height(), b.height());
        const int otherUnit = vertical ? qMin(a.width(), b.width()) : qMin(a.height(), b.height());
        const int gap = vertical ? a.left() - b.right() : b.top() - a.bottom();
        const int overlap = vertical ? qMin(a.bottom(), b.bottom()) - qMax(a.top(), b.top()) : qMin(a.right(), b.right()) - qMax(a.left(), b.left());
        if (unit > 2 * otherUnit || gap > 2 * unit || gap < -unit / 2 || overlap <= 0)
            return { };
    }
    return titles;
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
    if (roles.contains(int(Role::Colophon)) || ((roles.contains(int(Role::Title)) || roles.contains(int(Role::Author)) || roles.contains(int(Role::Publisher)) || roles.contains(int(Role::PublisherAuthor))) && (roles.contains(int(Role::Date)) || roles.contains(int(Role::Contact)) || roles.contains(int(Role::Printer)))))
        return PageKind::Colophon;
    // Publication-date labels are often missed. Two explicit identity fields
    // on a later page still provide a usable credit-page candidate.
    if (pageNumber > 3 && roles.contains(int(Role::Title)) && (roles.contains(int(Role::Author)) || roles.contains(int(Role::Publisher))))
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

QString suggestionSource(const Suggestion &suggestion)
{
    QStringList pages;
    QVector<int> numbers;
    for (const auto &evidence : suggestion.evidence)
        if (evidence.page > 0 && !numbers.contains(evidence.page))
            numbers.append(evidence.page);
    if (numbers.isEmpty() && suggestion.page > 0)
        numbers.append(suggestion.page);
    std::sort(numbers.begin(), numbers.end());
    for (int number : numbers)
        pages.append(QString::number(number));
    return pages.isEmpty() ? tr("이름 힌트 · 낮은 신뢰") : tr("%1페이지").arg(pages.join(QStringLiteral(", ")));
}

QVector<Suggestion> suggest(const QVector<Page> &pages, const QString &sourcePath, const QString &libraryRoot)
{
    QVector<Suggestion> result;
    auto append = [&](Suggestion::Field field, QString value, const QString &reason, int page, bool labelled = false, double confidence = -1) {
        value = normalizeOcrText(value).simplified();
        if (!usable(value))
            return;
        for (auto &existing : result) {
            if (existing.field == field && existing.value.compare(value, Qt::CaseInsensitive) == 0) {
                if (page > 0) {
                    auto evidence = std::find_if(existing.evidence.begin(), existing.evidence.end(), [page](const SuggestionEvidence &item) { return item.page == page; });
                    if (evidence == existing.evidence.end())
                        existing.evidence.append({ page, labelled, confidence });
                    else if ((labelled && !evidence->labelled) || (labelled == evidence->labelled && confidence > evidence->confidence))
                        *evidence = { page, labelled, confidence };
                    if ((labelled && !existing.labelled) || (labelled == existing.labelled && confidence > existing.confidence)) {
                        existing.page = page;
                        existing.labelled = labelled;
                        existing.confidence = confidence;
                    }
                }
                if (!existing.reason.contains(reason))
                    existing.reason += QLatin1Char('\n') + reason;
                return;
            }
        }
        Suggestion suggestion { field, value, reason, page, labelled, confidence, { } };
        if (page > 0)
            suggestion.evidence.append({ page, labelled, confidence });
        result.append(suggestion);
    };
    for (const auto &page : pages) {
        for (const auto &credit : credits(page)) {
            const auto field = credit.role == Role::Title ? Suggestion::Title : credit.role == Role::Author ? Suggestion::Author
                                                                                                            : Suggestion::Publisher;
            const QString reason = credit.roleUncertain ? tr("작가/서클 공용 표기입니다. 개인 작가인지 서클인지 직접 확인해 주세요.") : field == Suggestion::Publisher ? tr("발행자·서클 라벨 — 개인 작가로 확정하지 않습니다.")
                                                                                                                                                                       : tr("명시된 제목·작가 라벨과 연결된 글자입니다. 원본과 대조해 주세요.");
            append(field, credit.value, reason, page.number, !credit.roleUncertain, credit.confidence);
        }
        const auto titleLines = coverTitleLines(page);
        if (!titleLines.isEmpty()) {
            QStringList parts;
            double confidence = 100;
            for (const auto &line : titleLines) {
                parts.append(line.text.trimmed());
                confidence = qMin(confidence, line.confidence);
            }
            // A space preserves Korean/Latin word boundaries; normalization
            // removes spurious inter-character spaces only for Japanese.
            append(Suggestion::Title, parts.join(QLatin1Char(' ')), tr("표지의 큰 글자를 위치 순서로 연결한 추정 제목입니다. 판권 또는 외부 정보와 대조해 주세요."), page.number, false, confidence);
        }
    }
    if (sourcePath.trimmed().isEmpty())
        return result;
    const QFileInfo info(sourcePath);
    QString base = info.isDir() ? info.fileName() : info.completeBaseName();
    // Edition markers before [name] must not hide the bracketed name hint.
    const QRegularExpression decoration(QStringLiteral("^\\s*(?:\\([^)]*\\)|【[^】]*】)\\s*"));
    while (decoration.match(base).hasMatch())
        base.remove(decoration);
    const auto bracket = QRegularExpression(QStringLiteral(R"(^\[([^\]]+)\]\s*(.+)$)")).match(base);
    if (bracket.hasMatch()) {
        if (!genericPathName(bracket.captured(1)))
            append(Suggestion::Author, bracket.captured(1), tr("파일명 이름 힌트 — 외부 artist 태그와 일치하는지 확인합니다."), 0);
        base = bracket.captured(2);
    }
    if (!genericPathName(base))
        append(Suggestion::Title, base, tr("파일명·폴더명 힌트 — OCR로 확인한 제목이 아닙니다."), 0);
    const QString parent = info.dir().dirName();
    // Compare directory objects consistently: on Windows a rooted path without
    // a drive can be expanded differently by QFileInfo and QDir.
    const QDir libraryDirectory(libraryRoot);
    const bool root = !libraryRoot.isEmpty() && (info.dir() == libraryDirectory || (info.isDir() && QDir(info.absoluteFilePath()) == libraryDirectory));
    if (!root && !genericPathName(parent))
        append(Suggestion::Author, parent, tr("부모 폴더 이름 힌트 — 작가가 아닐 수 있습니다."), 0);
    return result;
}

Reading parseNeuralReading(const QByteArray &json, const QSize &imageSize)
{
    Reading reading;
    reading.engine = QStringLiteral("PaddleOCR · 영역 탐지 (시험)");
    reading.reviewRequired = true;
    auto invalid = [&] {
        Reading failure;
        failure.reviewRequired = true;
        failure.error = tr("영역 OCR 응답이 올바르지 않습니다.");
        return failure;
    };
    if (json.size() > 4 * 1024 * 1024)
        return invalid();
    const auto document = QJsonDocument::fromJson(json);
    const auto object = document.object();
    if (!document.isObject() || object.value("version").toInt() != 1 || object.value("engine").toString() != "paddle-regions" || !object.value("lines").isArray())
        return invalid();
    reading.device = object.value("device").toString().left(40);
    reading.warning = object.value("warning").toString().left(500);
    if (reading.warning == "GPU unavailable; CPU used")
        reading.warning = tr("GPU를 사용할 수 없어 CPU로 처리했습니다.");
    reading.error = object.value("error").toString().left(1500);
    reading.elapsedMs = qMax(0, object.value("elapsedMs").toInt());
    reading.initializationMs = qMax(0, object.value("initializationMs").toInt());
    reading.language = object.value("language").toString();
    if (reading.language != "auto" && reading.language != "jpn" && reading.language != "kor")
        return invalid();
    const auto lines = object.value("lines").toArray();
    if (lines.size() > 256)
        return invalid();
    double sum = 0;
    int weight = 0;
    for (const auto &value : lines) {
        const auto line = value.toObject();
        const QString text = line.value("text").toString().simplified();
        const double confidence = line.value("confidence").toDouble(-1);
        const auto box = line.value("box").toArray();
        const auto language = line.value("language").toString();
        if (text.isEmpty() || text.size() > 1000 || !std::isfinite(confidence) || confidence < 0 || confidence > 100 || box.size() != 4 || (language != "jpn" && language != "kor"))
            return invalid();
        for (const auto &coordinate : box)
            if (!coordinate.isDouble() || !std::isfinite(coordinate.toDouble()) || coordinate.toDouble() != coordinate.toInt(-1))
                return invalid();
        const QRect bounds(box[0].toInt(), box[1].toInt(), box[2].toInt() - box[0].toInt(), box[3].toInt() - box[1].toInt());
        if (bounds.isEmpty() || !QRect(QPoint(), imageSize).contains(bounds))
            return invalid();
        reading.lines.append({ text, confidence, bounds });
        reading.text += text + QLatin1Char('\n');
        sum += confidence * text.size();
        weight += text.size();
    }
    reading.text = reading.text.trimmed();
    reading.confidence = weight ? sum / weight : -1;
    return reading;
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
        if (reading.error.isEmpty() && !reading.text.isEmpty()) {
            auto sameLanguage = std::find_if(valid.begin(), valid.end(), [&](const Reading &other) { return other.language == reading.language; });
            if (sameLanguage == valid.end())
                valid.append(reading);
            else if (reading.score > sameLanguage->score)
                *sameLanguage = reading;
        } else if (!reading.error.isEmpty())
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
