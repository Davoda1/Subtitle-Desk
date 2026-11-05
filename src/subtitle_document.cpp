#include "subtitle_document.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringConverter>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr qint64 kMaximumJsonInteger = 9007199254740991LL;

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

QString normalizedFormat(QString format)
{
    format = format.trimmed().toLower();
    if (format.startsWith('.'))
        format.remove(0, 1);
    if (format == "webvtt")
        return "vtt";
    if (format == "subrip")
        return "srt";
    if (format == "ssa")
        return "ass";
    if (format == "project" || format == "sdesk" || format == "sdproj" || format == "subtitle-desk")
        return "json";
    return format;
}

QString normalizedLines(QString text, bool stripByteOrderMark = false)
{
    if (stripByteOrderMark && text.startsWith(QChar(0xfeff)))
        text.remove(0, 1);
    text.replace("\r\n", "\n");
    text.replace('\r', '\n');
    return text;
}

QString decodeEntities(const QString &text)
{
    static const QRegularExpression expression(
        QStringLiteral("&(#(?:[xX][0-9a-fA-F]+|[0-9]+)|amp|lt|gt|quot|apos|nbsp);"));
    QString result;
    qsizetype previous = 0;
    auto matches = expression.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        result += text.mid(previous, match.capturedStart() - previous);
        const QString entity = match.captured(1);
        QString replacement = match.captured();
        if (entity == "amp") replacement = "&";
        else if (entity == "lt") replacement = "<";
        else if (entity == "gt") replacement = ">";
        else if (entity == "quot") replacement = "\"";
        else if (entity == "apos") replacement = "'";
        else if (entity == "nbsp") replacement = QChar(0x00a0);
        else if (entity.startsWith('#')) {
            bool ok = false;
            const bool hex = entity.size() > 2 && entity.at(1).toLower() == 'x';
            const uint value = entity.mid(hex ? 2 : 1).toUInt(&ok, hex ? 16 : 10);
            if (ok && value > 0 && value <= 0x10ffff && !(value >= 0xd800 && value <= 0xdfff)) {
                const char32_t character = value;
                replacement = QString::fromUcs4(&character, 1);
            }
        }
        result += replacement;
        previous = match.capturedEnd();
    }
    result += text.mid(previous);
    return result;
}

QString plainTimedText(QString text)
{
    // Import visual text from the simple markup used by SRT and WebVTT.
    // Decode entities after removing tags so escaped literal tags stay literal.
    static const QRegularExpression breaks("<br\\s*/?>", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression formatting(
        "</?(?:b|i|u|s|font|c(?:\\.[^\\s>]*)?|v|lang|ruby|rt)(?:\\s+[^>]*)?>",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression wordTime("<(?:\\d+:)?\\d{2}:\\d{2}\\.\\d{3}>");
    text.replace(breaks, "\n");
    text.remove(formatting);
    text.remove(wordTime);
    text = decodeEntities(text);
    auto lines = text.split('\n', Qt::KeepEmptyParts);
    for (QString &line : lines) {
        // A zero-width marker keeps a visually empty line inside a legal cue
        // payload, where a completely blank line would terminate the cue.
        if (line.startsWith(QChar(0x200b)) && line.mid(1).trimmed().isEmpty())
            line.remove(0, 1);
    }
    return lines.join('\n');
}

QString encodeTimedText(const QString &text)
{
    auto lines = normalizedLines(text).split('\n', Qt::KeepEmptyParts);
    for (QString &line : lines) {
        if (line.trimmed().isEmpty())
            line.prepend(QChar(0x200b));
        line.replace('&', "&amp;");
        line.replace('<', "&lt;");
        line.replace('>', "&gt;");
    }
    return lines.join('\n');
}

bool parseTimedBlocks(const QString &source, const QString &format, QVector<Cue> &cues, QString *error)
{
    QString text = normalizedLines(source, true);
    if (format == "vtt") {
        const qsizetype firstBreak = text.indexOf('\n');
        const QString header = (firstBreak < 0 ? text : text.left(firstBreak)).trimmed();
        if (!QRegularExpression("^WEBVTT(?:[ \\t].*)?$").match(header).hasMatch()) {
            setError(error, "WebVTT files must begin with WEBVTT.");
            return false;
        }
        if (firstBreak < 0) {
            setError(error, "No subtitle cues were found in the WebVTT file.");
            return false;
        }
        // Header metadata extends to the first blank line.
        const auto separator = QRegularExpression("\\n[ \\t]*\\n").match(text);
        if (!separator.hasMatch()) {
            setError(error, "WebVTT needs a blank line between its header and the first cue.");
            return false;
        }
        text = text.mid(separator.capturedEnd());
    }
    static const QRegularExpression separator("\\n[ \\t]*\\n+");
    static const QRegularExpression timing("^(\\S+)\\s+-->\\s+(\\S+)(?:[ \\t]+.*)?$");
    const auto blocks = text.split(separator, Qt::SkipEmptyParts);
    int blockNumber = 0;
    for (const QString &block : blocks) {
        ++blockNumber;
        auto lines = block.split('\n', Qt::KeepEmptyParts);
        while (!lines.isEmpty() && lines.first().trimmed().isEmpty()) lines.removeFirst();
        while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
        if (lines.isEmpty()) continue;
        const QString firstLine = lines.first().trimmed();
        if (format == "vtt" && (firstLine == "NOTE" || firstLine.startsWith("NOTE ") ||
                                firstLine.startsWith("NOTE\t") || firstLine == "STYLE" || firstLine == "REGION"))
            continue;
        int timingLine = 0;
        auto match = timing.match(firstLine);
        if (!match.hasMatch() && lines.size() > 1) {
            timingLine = 1;
            if (format == "srt" && !QRegularExpression("^\\d+$").match(firstLine).hasMatch()) {
                setError(error, QString("SRT block %1 has an invalid cue number or timestamp.").arg(blockNumber));
                return false;
            }
            match = timing.match(lines.at(1).trimmed());
        }
        if (!match.hasMatch()) {
            setError(error, QString("%1 block %2 is missing a valid 'start --> end' timestamp line.")
                               .arg(format.toUpper()).arg(blockNumber));
            return false;
        }
        bool startOk = false, endOk = false;
        const qint64 start = SubtitleDocument::parseTime(match.captured(1), &startOk);
        const qint64 end = SubtitleDocument::parseTime(match.captured(2), &endOk);
        if (!startOk || !endOk || end <= start) {
            setError(error, QString("%1 block %2 has invalid times; the end must be after the start.")
                               .arg(format.toUpper()).arg(blockNumber));
            return false;
        }
        Cue cue;
        cue.id = SubtitleDocument::newCueId();
        cue.startMs = start;
        cue.endMs = end;
        cue.text = plainTimedText(lines.mid(timingLine + 1).join('\n'));
        cues.push_back(cue);
    }
    if (cues.isEmpty()) {
        setError(error, QString("No subtitle cues were found in the %1 file.").arg(format.toUpper()));
        return false;
    }
    return true;
}

QString decodeAssText(const QString &text)
{
    QString result;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar character = text.at(i);
        if (character == '\\' && i + 1 < text.size()) {
            const QChar next = text.at(i + 1);
            if (next == 'N' || next == 'n') { result += '\n'; ++i; continue; }
            if (next == 'h') { result += QChar(0x00a0); ++i; continue; }
            if (next == '{' || next == '}') { result += next; ++i; continue; }
            if (next == QChar(0x2060)) { result += '\\'; ++i; continue; }
            // Unknown backslash sequences are literal in ASS.
        }
        if (character == '{') {
            const qsizetype end = text.indexOf('}', i + 1);
            if (end >= 0) { i = end; continue; }
        }
        result += character;
    }
    return result;
}

QString encodeAssText(const QString &source)
{
    QString result;
    const QString text = normalizedLines(source);
    for (const QChar character : text) {
        if (character == '\n') result += "\\N";
        else if (character == '\\') {
            // ASS has no literal-backslash escape.  An invisible word joiner
            // prevents following N/n/h/{/} from being interpreted as escapes.
            result += '\\';
            result += QChar(0x2060);
        } else if (character == '{' || character == '}') {
            result += '\\';
            result += character;
        } else if (character == QChar(0x00a0)) result += "\\h";
        else result += character;
    }
    return result;
}

bool parseAss(const QString &source, QVector<Cue> &cues, QString *error)
{
    const auto lines = normalizedLines(source, true).split('\n', Qt::KeepEmptyParts);
    QStringList fields {"layer", "start", "end", "style", "name", "marginl", "marginr", "marginv", "effect", "text"};
    bool inEvents = false;
    for (qsizetype lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const QString rawLine = lines.at(lineIndex);
        const QString line = rawLine.trimmed();
        if (line.startsWith('[') && line.endsWith(']')) {
            inEvents = line.compare("[Events]", Qt::CaseInsensitive) == 0;
            continue;
        }
        if (!inEvents || line.isEmpty() || line.startsWith(';')) continue;
        if (line.startsWith("Format:", Qt::CaseInsensitive)) {
            fields = line.mid(7).toLower().split(',');
            for (QString &field : fields) field = field.trimmed();
            if (!fields.contains("start") || !fields.contains("end") || fields.isEmpty() || fields.last() != "text") {
                setError(error, QString("ASS line %1 has an unsupported event format; Start, End, and a final Text field are required.").arg(lineIndex + 1));
                return false;
            }
            continue;
        }
        if (line.startsWith("Comment:", Qt::CaseInsensitive)) continue;
        if (!line.startsWith("Dialogue:", Qt::CaseInsensitive)) {
            setError(error, QString("Unrecognized ASS event on line %1.").arg(lineIndex + 1));
            return false;
        }
        // Preserve commas inside Text by splitting only the preceding fields.
        const qsizetype dialogueStart = rawLine.indexOf("Dialogue:", 0, Qt::CaseInsensitive);
        const QString event = rawLine.mid(dialogueStart + 9);
        QStringList values;
        qsizetype cursor = 0;
        for (qsizetype field = 0; field < fields.size() - 1; ++field) {
            const qsizetype comma = event.indexOf(',', cursor);
            if (comma < 0) {
                setError(error, QString("ASS dialogue on line %1 has too few fields.").arg(lineIndex + 1));
                return false;
            }
            values += event.mid(cursor, comma - cursor);
            cursor = comma + 1;
        }
        values += event.mid(cursor);
        bool startOk = false, endOk = false;
        Cue cue;
        cue.id = SubtitleDocument::newCueId();
        cue.startMs = SubtitleDocument::parseTime(values.at(fields.indexOf("start")).trimmed(), &startOk);
        cue.endMs = SubtitleDocument::parseTime(values.at(fields.indexOf("end")).trimmed(), &endOk);
        if (!startOk || !endOk || cue.endMs <= cue.startMs) {
            setError(error, QString("ASS dialogue on line %1 has invalid start or end times.").arg(lineIndex + 1));
            return false;
        }
        cue.text = decodeAssText(values.last());
        cues += cue;
    }
    if (cues.isEmpty()) {
        setError(error, "No Dialogue events were found in the ASS/SSA file.");
        return false;
    }
    return true;
}

bool parseLrc(const QString &source, QVector<Cue> &cues, QString *error, QString &title)
{
    const auto lines = normalizedLines(source, true).split('\n', Qt::KeepEmptyParts);
    static const QRegularExpression timestamp("^\\[([^\\]]+)\\]");
    static const QRegularExpression metadata("^\\[([A-Za-z][A-Za-z0-9_-]*):(.*)\\]$");
    static const QRegularExpression enhancedTime("<(?:\\d+:)?\\d+:\\d{2}(?:[.,]\\d{1,3})?>");
    qint64 offsetMs = 0;
    qint64 declaredLength = 0;
    for (qsizetype lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const QString line = lines.at(lineIndex);
        if (line.trimmed().isEmpty()) continue;
        const auto meta = metadata.match(line.trimmed());
        if (meta.hasMatch()) {
            const QString key = meta.captured(1).toLower();
            const QString value = meta.captured(2).trimmed();
            if (key == "offset") {
                bool ok = false;
                offsetMs = value.toLongLong(&ok);
                if (!ok || offsetMs < -kMaximumJsonInteger || offsetMs > kMaximumJsonInteger) {
                    setError(error, QString("LRC line %1 contains an invalid millisecond offset.").arg(lineIndex + 1));
                    return false;
                }
            } else if (key == "length") {
                bool ok = false;
                declaredLength = SubtitleDocument::parseTime(value, &ok);
                if (!ok) {
                    setError(error, QString("LRC line %1 contains an invalid length.").arg(lineIndex + 1));
                    return false;
                }
            } else if (key == "ti") title = value;
            continue;
        }
        QString rest = line;
        QVector<qint64> starts;
        while (true) {
            const auto match = timestamp.match(rest);
            if (!match.hasMatch()) break;
            bool ok = false;
            const qint64 start = SubtitleDocument::parseTime(match.captured(1), &ok);
            if (!ok) break;
            starts += start;
            rest.remove(0, match.capturedLength());
        }
        if (starts.isEmpty()) {
            setError(error, QString("LRC line %1 has no valid [mm:ss.xx] timestamp.").arg(lineIndex + 1));
            return false;
        }
        rest.remove(enhancedTime);
        for (const qint64 start : starts) {
            Cue cue;
            cue.id = SubtitleDocument::newCueId();
            cue.startMs = start;
            cue.text = rest;
            cues += cue;
        }
    }
    if (cues.isEmpty()) {
        setError(error, "No timed lyric lines were found in the LRC file.");
        return false;
    }
    // The LRC offset is a playback-clock adjustment: positive means show the
    // lyrics earlier.  Clamp pre-roll to zero; preserve every lyric entry.
    for (Cue &cue : cues) {
        if (offsetMs >= 0) cue.startMs = qMax<qint64>(0, cue.startMs - offsetMs);
        else if (cue.startMs > kMaximumJsonInteger + offsetMs) {
            setError(error, "The LRC timestamp and offset exceed the supported time range.");
            return false;
        } else cue.startMs -= offsetMs;
    }
    std::stable_sort(cues.begin(), cues.end(), [](const Cue &a, const Cue &b) { return a.startMs < b.startMs; });
    // LRC contains starts only.  Equal timestamps get the same next distinct
    // end, and the final line gets three seconds unless length supplies an end.
    for (qsizetype i = 0; i < cues.size();) {
        qsizetype next = i + 1;
        while (next < cues.size() && cues.at(next).startMs == cues.at(i).startMs) ++next;
        qint64 end = 0;
        if (next < cues.size()) end = cues.at(next).startMs;
        else {
            const qint64 adjustedLength = offsetMs >= 0 ? qMax<qint64>(0, declaredLength - offsetMs)
                                                       : (declaredLength <= kMaximumJsonInteger + offsetMs ? declaredLength - offsetMs : 0);
            if (adjustedLength > cues.at(i).startMs) end = adjustedLength;
            else if (cues.at(i).startMs <= kMaximumJsonInteger - 3000) end = cues.at(i).startMs + 3000;
            else {
                setError(error, "The final LRC timestamp exceeds the supported time range.");
                return false;
            }
        }
        for (; i < next; ++i) cues[i].endMs = end;
    }
    return true;
}

bool jsonInteger(const QJsonValue &value, qint64 &result)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0 || number > static_cast<double>(kMaximumJsonInteger) || std::floor(number) != number)
        return false;
    result = static_cast<qint64>(number);
    return true;
}

bool parseProject(const QString &text, SubtitleDocument &document, QString *error)
{
    QJsonParseError parseError;
    const auto parsed = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        setError(error, QString("Invalid project JSON at byte %1: %2").arg(parseError.offset).arg(parseError.errorString()));
        return false;
    }
    if (!parsed.isObject()) {
        setError(error, "A Subtitle Desk project must contain a JSON object.");
        return false;
    }
    const auto object = parsed.object();
    if (object.value("format").toString() != "subtitle-desk" || !object.value("version").isDouble() || object.value("version").toDouble() != 1) {
        setError(error, "This file is not a supported Subtitle Desk project (format subtitle-desk, version 1).");
        return false;
    }
    if (!object.value("cues").isArray()) {
        setError(error, "The project is missing its cues array.");
        return false;
    }
    for (const QString &field : {QString("mediaPath"), QString("model"), QString("language"), QString("title")}) {
        if (object.contains(field) && !object.value(field).isString()) {
            setError(error, QString("Project field '%1' must be text.").arg(field));
            return false;
        }
    }
    document = SubtitleDocument();
    document.mediaPath = object.value("mediaPath").toString();
    document.model = object.value("model").toString();
    document.language = object.value("language").toString("auto");
    document.title = object.value("title").toString();
    const auto array = object.value("cues").toArray();
    for (qsizetype i = 0; i < array.size(); ++i) {
        if (!array.at(i).isObject()) {
            setError(error, QString("Project cue %1 must be an object.").arg(i + 1));
            return false;
        }
        const auto item = array.at(i).toObject();
        Cue cue;
        if (!jsonInteger(item.value("startMs"), cue.startMs) || !jsonInteger(item.value("endMs"), cue.endMs) || cue.endMs <= cue.startMs) {
            setError(error, QString("Project cue %1 has invalid integer startMs/endMs values.").arg(i + 1));
            return false;
        }
        if (!item.value("text").isString() || (item.contains("id") && !item.value("id").isString()) ||
            (item.contains("reviewed") && !item.value("reviewed").isBool())) {
            setError(error, QString("Project cue %1 has an invalid text, id, or reviewed field.").arg(i + 1));
            return false;
        }
        cue.id = item.value("id").toString();
        cue.text = item.value("text").toString();
        cue.reviewed = item.value("reviewed").toBool(false);
        document.cues += cue;
    }
    return true;
}

QString csvCell(QString text)
{
    // Quoting a CSV cell alone does not stop spreadsheet formula evaluation.
    // Preserve the visible text in spreadsheet programs by adding their
    // literal-text prefix to potentially executable values.  Other export
    // formats and the project retain the exact original text.
    qsizetype firstVisible = 0;
    while (firstVisible < text.size() &&
           (text.at(firstVisible).isSpace() || text.at(firstVisible).category() == QChar::Other_Format))
        ++firstVisible;
    const bool formula = firstVisible < text.size() && QString("=+-@").contains(text.at(firstVisible));
    const bool controlPrefix = !text.isEmpty() && (text.front() == '\t' || text.front() == '\r' || text.front() == '\n');
    if (formula || controlPrefix)
        text.prepend('\'');
    text.replace('"', "\"\"");
    return '"' + text + '"';
}

QString assTime(qint64 milliseconds, bool roundUp)
{
    const qint64 centiseconds = milliseconds / 10 + (roundUp && milliseconds % 10 != 0 ? 1 : 0);
    return QString("%1:%2:%3.%4")
        .arg(centiseconds / 360000)
        .arg((centiseconds / 6000) % 60, 2, 10, QChar('0'))
        .arg((centiseconds / 100) % 60, 2, 10, QChar('0'))
        .arg(centiseconds % 100, 2, 10, QChar('0'));
}

} // namespace

QString SubtitleDocument::newCueId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

qint64 SubtitleDocument::parseTime(const QString &source, bool *ok)
{
    if (ok) *ok = false;
    static const QRegularExpression expression("^(\\d+):(\\d{1,2})(?::(\\d{1,2}))?(?:[.,](\\d{1,3}))?$");
    const auto match = expression.match(source.trimmed());
    if (!match.hasMatch()) return -1;
    bool firstOk = false;
    const qint64 first = match.captured(1).toLongLong(&firstOk);
    const qint64 second = match.captured(2).toLongLong();
    const bool hasHours = !match.captured(3).isEmpty();
    const qint64 seconds = hasHours ? match.captured(3).toLongLong() : second;
    if (!firstOk || second > 59 || seconds > 59) return -1;
    const qint64 minutes = hasHours ? second : first;
    const qint64 hours = hasHours ? first : 0;
    const QString fraction = match.captured(4);
    const qint64 fractionMs = fraction.isEmpty() ? 0 : fraction.leftJustified(3, '0').toLongLong();
    if (hours > kMaximumJsonInteger / 3600000 || minutes > kMaximumJsonInteger / 60000) return -1;
    const qint64 hoursMs = hours * 3600000;
    const qint64 remainder = minutes * 60000 + seconds * 1000 + fractionMs;
    if (remainder > kMaximumJsonInteger || hoursMs > kMaximumJsonInteger - remainder) return -1;
    if (ok) *ok = true;
    return hoursMs + remainder;
}

QString SubtitleDocument::formatTime(qint64 milliseconds, bool comma)
{
    const bool negative = milliseconds < 0;
    // Unsigned magnitude also handles the smallest signed integer safely.
    const quint64 total = negative ? quint64(-(milliseconds + 1)) + 1 : quint64(milliseconds);
    return QString("%1%2:%3:%4%5%6")
        .arg(negative ? "-" : "")
        .arg(total / 3600000, 2, 10, QChar('0'))
        .arg((total / 60000) % 60, 2, 10, QChar('0'))
        .arg((total / 1000) % 60, 2, 10, QChar('0'))
        .arg(comma ? ',' : '.')
        .arg(total % 1000, 3, 10, QChar('0'));
}

bool SubtitleDocument::importText(const QString &text, const QString &requestedFormat, QString *error)
{
    setError(error, {});
    const QString format = normalizedFormat(requestedFormat);
    SubtitleDocument candidate = *this;
    candidate.cues.clear();
    bool success = false;
    if (format == "json") success = parseProject(normalizedLines(text, true), candidate, error);
    else if (format == "srt" || format == "vtt") success = parseTimedBlocks(text, format, candidate.cues, error);
    else if (format == "ass") success = parseAss(text, candidate.cues, error);
    else if (format == "lrc") success = parseLrc(text, candidate.cues, error, candidate.title);
    else {
        setError(error, QString("Import format '%1' is not supported. Use SRT, VTT, ASS/SSA, LRC, or a Subtitle Desk project.").arg(requestedFormat));
        return false;
    }
    if (!success) return false;
    candidate.normalizeIds();
    const auto errors = candidate.validationErrors();
    if (!errors.isEmpty()) {
        setError(error, errors.join('\n'));
        return false;
    }
    candidate.stableSort();
    *this = std::move(candidate);
    return true;
}

bool SubtitleDocument::load(const QString &filePath, QString *error)
{
    setError(error, {});
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QString("Cannot open '%1': %2").arg(filePath, file.errorString()));
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        setError(error, QString("Cannot read '%1': %2").arg(filePath, file.errorString()));
        return false;
    }
    const auto detected = QStringConverter::encodingForData(bytes);
    QStringDecoder decoder(detected.value_or(QStringConverter::Utf8));
    const QString text = decoder.decode(bytes);
    if (decoder.hasError()) {
        setError(error, "The subtitle file contains invalid text encoding. Convert it to UTF-8 or UTF-16 with a byte-order mark and try again.");
        return false;
    }
    SubtitleDocument candidate = *this;
    const QString format = normalizedFormat(QFileInfo(filePath).suffix());
    if (!candidate.importText(text, format, error)) return false;
    if (format == "json" && !candidate.mediaPath.isEmpty() && QDir::isRelativePath(candidate.mediaPath))
        candidate.mediaPath = QDir(QFileInfo(filePath).absolutePath()).absoluteFilePath(candidate.mediaPath);
    *this = std::move(candidate);
    return true;
}

QString SubtitleDocument::exportText(const QString &requestedFormat, QString *error) const
{
    setError(error, {});
    const QString format = normalizedFormat(requestedFormat);
    const auto errors = validationErrors();
    if (!errors.isEmpty()) {
        setError(error, errors.join('\n'));
        return {};
    }
    SubtitleDocument copy = *this;
    copy.normalizeIds();
    copy.stableSort();
    QString output;
    if (format == "json") {
        QJsonArray array;
        for (const Cue &cue : copy.cues)
            array += QJsonObject{{"id", cue.id}, {"startMs", cue.startMs}, {"endMs", cue.endMs},
                                 {"text", cue.text}, {"reviewed", cue.reviewed}};
        return QString::fromUtf8(QJsonDocument(QJsonObject{{"format", "subtitle-desk"}, {"version", 1},
            {"title", title}, {"mediaPath", mediaPath}, {"model", model}, {"language", language}, {"cues", array}})
                                     .toJson(QJsonDocument::Indented));
    }
    if (format == "srt" || format == "vtt") {
        if (format == "vtt") output = "WEBVTT\n\n";
        for (qsizetype i = 0; i < copy.cues.size(); ++i) {
            const Cue &cue = copy.cues.at(i);
            output += QString::number(i + 1) + '\n';
            output += formatTime(cue.startMs, format == "srt") + " --> " + formatTime(cue.endMs, format == "srt") + '\n';
            output += encodeTimedText(cue.text) + "\n\n";
        }
        return output;
    }
    if (format == "ass") {
        output = "[Script Info]\nScriptType: v4.00+\nPlayResX: 1280\nPlayResY: 720\nWrapStyle: 0\nScaledBorderAndShadow: yes\n\n"
                 "[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, "
                 "Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
                 "Style: Default,Arial,42,&H00FFFFFF,&H000000FF,&H00101010,&H80000000,0,0,0,0,100,100,0,0,1,2,1,2,40,40,40,1\n\n"
                 "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
        for (const Cue &cue : copy.cues)
            output += "Dialogue: 0," + assTime(cue.startMs, false) + ',' + assTime(cue.endMs, true) + ",Default,,0,0,0,," + encodeAssText(cue.text) + '\n';
        return output;
    }
    if (format == "lrc") {
        // Standard LRC has centisecond starts but no explicit ends, IDs, or
        // review flags.  Keep one timestamp on every physical lyric line.
        for (const Cue &cue : copy.cues) {
            const QString stamp = QString("[%1:%2.%3]")
                .arg(cue.startMs / 60000, 2, 10, QChar('0'))
                .arg((cue.startMs / 1000) % 60, 2, 10, QChar('0'))
                .arg((cue.startMs / 10) % 100, 2, 10, QChar('0'));
            for (const QString &line : normalizedLines(cue.text).split('\n', Qt::KeepEmptyParts))
                output += stamp + line + '\n';
        }
        return output;
    }
    if (format == "txt") {
        QStringList paragraphs;
        for (const Cue &cue : copy.cues) paragraphs += normalizedLines(cue.text);
        return paragraphs.isEmpty() ? QString() : paragraphs.join("\n\n") + '\n';
    }
    if (format == "csv") {
        output = "id,start,end,start_ms,end_ms,text,reviewed\r\n";
        for (const Cue &cue : copy.cues)
            output += csvCell(cue.id) + ',' + csvCell(formatTime(cue.startMs)) + ',' + csvCell(formatTime(cue.endMs)) + ',' +
                QString::number(cue.startMs) + ',' + QString::number(cue.endMs) + ',' + csvCell(cue.text) + ',' +
                (cue.reviewed ? "true" : "false") + "\r\n";
        return output;
    }
    setError(error, QString("Export format '%1' is not supported.").arg(requestedFormat));
    return {};
}

bool SubtitleDocument::exportFile(const QString &filePath, const QString &format, QString *error) const
{
    setError(error, {});
    SubtitleDocument copy = *this;
    if (normalizedFormat(format) == "json" && !copy.mediaPath.isEmpty() && QDir::isAbsolutePath(copy.mediaPath))
        copy.mediaPath = QDir(QFileInfo(filePath).absolutePath()).relativeFilePath(copy.mediaPath);
    QString localError;
    const QString text = copy.exportText(format, &localError);
    if (!localError.isEmpty()) {
        setError(error, localError);
        return false;
    }
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QString("Cannot write '%1': %2").arg(filePath, file.errorString()));
        return false;
    }
    const QByteArray bytes = text.toUtf8();
    if (file.write(bytes) != bytes.size()) {
        setError(error, QString("Cannot write the complete file '%1': %2").arg(filePath, file.errorString()));
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        setError(error, QString("Cannot finish saving '%1': %2").arg(filePath, file.errorString()));
        return false;
    }
    return true;
}

bool SubtitleDocument::saveProject(const QString &filePath, QString *error) const
{
    return exportFile(filePath, "json", error);
}

QStringList SubtitleDocument::validationErrors() const
{
    QStringList errors;
    for (qsizetype i = 0; i < cues.size(); ++i) {
        const Cue &cue = cues.at(i);
        if (cue.startMs < 0 || cue.endMs <= cue.startMs)
            errors += QString("Cue %1 needs a non-negative start and an end after its start.").arg(i + 1);
        else if (cue.endMs > kMaximumJsonInteger)
            errors += QString("Cue %1 exceeds the supported timestamp range.").arg(i + 1);
        if (cue.text.contains(QChar(0)))
            errors += QString("Cue %1 contains a NUL character; remove it before saving.").arg(i + 1);
    }
    return errors;
}

QStringList SubtitleDocument::timingWarnings() const
{
    QStringList warnings;
    QVector<int> order;
    order.reserve(cues.size());
    for (qsizetype i = 0; i < cues.size(); ++i) {
        const bool validTime = cues.at(i).startMs >= 0 && cues.at(i).endMs > cues.at(i).startMs &&
                               cues.at(i).endMs <= kMaximumJsonInteger;
        if (validTime)
            order += static_cast<int>(i);
        if (cues.at(i).text.trimmed().isEmpty())
            warnings += QString("Cue %1 has no text.").arg(i + 1);
        const auto lines = cues.at(i).text.split('\n', Qt::KeepEmptyParts);
        qsizetype longest = 0;
        for (const QString &line : lines)
            longest = qMax(longest, line.size());
        if (longest > 42)
            warnings += QString("Cue %1 has a %2-character line; consider keeping lines to about 42 characters.").arg(i + 1).arg(longest);
        if (lines.size() > 2)
            warnings += QString("Cue %1 has %2 lines; consider splitting it into shorter subtitles.").arg(i + 1).arg(lines.size());
        const qint64 duration = validTime ? cues.at(i).endMs - cues.at(i).startMs : 0;
        if (duration > 0) {
            const double charactersPerSecond = cues.at(i).text.size() * 1000.0 / duration;
            if (charactersPerSecond > 20)
                warnings += QString("Cue %1 reads at %2 characters per second; consider more time or less text.").arg(i + 1).arg(charactersPerSecond, 0, 'f', 1);
        }
    }
    std::stable_sort(order.begin(), order.end(), [this](int a, int b) { return cues.at(a).startMs < cues.at(b).startMs; });
    int previous = -1;
    for (const int index : order) {
        if (previous >= 0 && cues.at(index).startMs < cues.at(previous).endMs)
            warnings += QString("Cue %1 overlaps cue %2 by %3 ms.").arg(index + 1).arg(previous + 1)
                            .arg(qMin(cues.at(index).endMs, cues.at(previous).endMs) - cues.at(index).startMs);
        if (previous < 0 || cues.at(index).endMs > cues.at(previous).endMs)
            previous = index;
    }
    return warnings;
}

void SubtitleDocument::stableSort()
{
    std::stable_sort(cues.begin(), cues.end(), [](const Cue &a, const Cue &b) { return a.startMs < b.startMs; });
}

void SubtitleDocument::normalizeIds()
{
    QSet<QString> seen;
    for (Cue &cue : cues) {
        if (cue.id.isEmpty() || seen.contains(cue.id)) cue.id = newCueId();
        seen.insert(cue.id);
    }
}

qint64 SubtitleDocument::durationMs() const
{
    qint64 duration = 0;
    for (const Cue &cue : cues) duration = qMax(duration, cue.endMs);
    return duration;
}

int SubtitleDocument::cueAt(qint64 milliseconds) const
{
    for (qsizetype i = 0; i < cues.size(); ++i)
        if (milliseconds >= cues.at(i).startMs && milliseconds < cues.at(i).endMs)
            return static_cast<int>(i);
    return -1;
}

QVector<int> SubtitleDocument::activeCuesAt(qint64 milliseconds) const
{
    QVector<int> active;
    for (qsizetype i = 0; i < cues.size(); ++i)
        if (milliseconds >= cues.at(i).startMs && milliseconds < cues.at(i).endMs)
            active += static_cast<int>(i);
    return active;
}

bool SubtitleDocument::splitCue(int index, int characterPosition, qint64 splitMs, QString *error)
{
    setError(error, {});
    if (index < 0 || index >= cues.size()) {
        setError(error, "Select a subtitle to split.");
        return false;
    }
    const Cue original = cues.at(index);
    if (original.startMs < 0 || original.endMs <= original.startMs || original.endMs - original.startMs < 2) {
        setError(error, "A subtitle must have at least two milliseconds of valid duration to split.");
        return false;
    }
    if (characterPosition <= 0 || characterPosition >= original.text.size()) {
        setError(error, "Place the text cursor inside the subtitle before splitting.");
        return false;
    }
    // A QTextCursor position is UTF-16; never cut an emoji/supplementary code
    // point into two invalid surrogate halves.
    if (original.text.at(characterPosition - 1).isHighSurrogate() && original.text.at(characterPosition).isLowSurrogate())
        ++characterPosition;
    if (characterPosition >= original.text.size()) {
        setError(error, "The split would leave an empty second subtitle.");
        return false;
    }
    if (splitMs <= original.startMs || splitMs >= original.endMs)
        splitMs = original.startMs + (original.endMs - original.startMs) / 2;
    Cue left = original;
    Cue right = original;
    left.endMs = splitMs;
    right.startMs = splitMs;
    left.text = original.text.left(characterPosition).trimmed();
    right.text = original.text.mid(characterPosition).trimmed();
    if (left.text.isEmpty() || right.text.isEmpty()) {
        setError(error, "Choose a text position that leaves words in both subtitles.");
        return false;
    }
    right.id = newCueId();
    left.reviewed = right.reviewed = false;
    cues[index] = left;
    cues.insert(index + 1, right);
    return true;
}

bool SubtitleDocument::mergeCues(int first, int last, QString *error)
{
    setError(error, {});
    if (first < 0 || last >= cues.size() || last <= first) {
        setError(error, "Select at least two adjacent subtitles to merge.");
        return false;
    }
    Cue merged = cues.at(first);
    QStringList parts;
    for (int i = first; i <= last; ++i) {
        merged.startMs = qMin(merged.startMs, cues.at(i).startMs);
        merged.endMs = qMax(merged.endMs, cues.at(i).endMs);
        parts += cues.at(i).text;
    }
    merged.text = parts.join('\n');
    merged.reviewed = false;
    cues[first] = merged;
    cues.remove(first + 1, last - first);
    return true;
}

qint64 SubtitleDocument::shiftCues(qint64 offsetMs, const QVector<int> &indices)
{
    QSet<int> selected;
    if (indices.isEmpty()) {
        for (qsizetype i = 0; i < cues.size(); ++i) selected.insert(static_cast<int>(i));
    } else {
        for (const int index : indices)
            if (index >= 0 && index < cues.size()) selected.insert(index);
    }
    if (selected.isEmpty()) return 0;
    qint64 earliest = std::numeric_limits<qint64>::max();
    qint64 latest = 0;
    for (const int index : selected) {
        earliest = qMin(earliest, cues.at(index).startMs);
        latest = qMax(latest, cues.at(index).endMs);
    }
    if (earliest < 0 || latest > kMaximumJsonInteger) return 0;
    const qint64 actual = qBound(-earliest, offsetMs, kMaximumJsonInteger - latest);
    if (actual == 0) return 0;
    for (const int index : selected) {
        cues[index].startMs += actual;
        cues[index].endMs += actual;
        cues[index].reviewed = false;
    }
    return actual;
}
