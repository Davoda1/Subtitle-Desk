#include "subtitle_document.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringConverter>
#include <QTemporaryDir>
#include <QTextStream>
#include <limits>

namespace {
int checks = 0;
int failures = 0;

void check(bool passed, const char *expression, int line)
{
    ++checks;
    if (!passed) {
        ++failures;
        QTextStream(stderr) << "FAIL line " << line << ": " << expression << '\n';
    }
}
#define CHECK(expression) check(bool(expression), #expression, __LINE__)

Cue cue(qint64 start, qint64 end, const QString &text, const QString &id = {}, bool reviewed = false)
{
    Cue result;
    result.id = id;
    result.startMs = start;
    result.endMs = end;
    result.text = text;
    result.reviewed = reviewed;
    return result;
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

void timeTests()
{
    bool ok = false;
    CHECK(SubtitleDocument::parseTime("00:00:00.000", &ok) == 0 && ok);
    CHECK(SubtitleDocument::parseTime("01:02:03,456", &ok) == 3723456 && ok);
    CHECK(SubtitleDocument::parseTime("123:45.6", &ok) == 7425600 && ok);
    CHECK(SubtitleDocument::parseTime("2:03.04", &ok) == 123040 && ok);
    CHECK(SubtitleDocument::parseTime(" 02:03 ", &ok) == 123000 && ok);
    CHECK(SubtitleDocument::parseTime("100:02:03.999", &ok) == 360123999 && ok);
    CHECK(SubtitleDocument::formatTime(3723456) == "01:02:03.456");
    CHECK(SubtitleDocument::formatTime(3723456, true) == "01:02:03,456");
    CHECK(SubtitleDocument::formatTime(360123999) == "100:02:03.999");
    CHECK(SubtitleDocument::formatTime(-1500) == "-00:00:01.500");
    CHECK(SubtitleDocument::formatTime(std::numeric_limits<qint64>::min()).startsWith('-'));
    for (const QString &invalid : {QString(""), QString("NaN"), QString("-01:00"), QString("1:60"),
             QString("1:60:00"), QString("1:00:60"), QString("1:00.1234"), QString("1:00.1x"),
             QString("999999999999999999:00"), QString("01:02:03:04"), QString("00:00:00.-1")}) {
        CHECK(SubtitleDocument::parseTime(invalid, &ok) == -1 && !ok);
    }
    for (qint64 value : {0LL, 1LL, 999LL, 1000LL, 3599999LL, 3600000LL, 86401234LL, 1000000000000LL})
        CHECK(SubtitleDocument::parseTime(SubtitleDocument::formatTime(value), &ok) == value && ok);
}

void projectTests()
{
    SubtitleDocument document;
    document.mediaPath = "/music/Árvíztűrő video.mkv";
    document.model = "whisper-small";
    document.language = "hu";
    document.title = "A title \"with quotes\"";
    document.cues = {cue(5000, 6000, "Later", "same", true),
                     cue(1, 2345, QString::fromUtf8("Árvíztűrő tükörfúrógép 😀\n日本語 <tag> & quote \""), "first", true),
                     cue(5000, 7000, "Same start", "same", false)};
    QString error;
    const QString serialized = document.exportText("json", &error);
    CHECK(error.isEmpty());
    CHECK(document.cues.first().startMs == 5000); // Export must not reorder edits.
    SubtitleDocument loaded;
    CHECK(loaded.importText(serialized, "project", &error));
    CHECK(error.isEmpty());
    CHECK(loaded.cues.size() == 3);
    CHECK(loaded.cues.at(0).text == document.cues.at(1).text);
    CHECK(loaded.cues.at(0).reviewed);
    CHECK(loaded.cues.at(1).text == "Later" && loaded.cues.at(2).text == "Same start");
    CHECK(loaded.cues.at(1).id != loaded.cues.at(2).id);
    CHECK(loaded.mediaPath == document.mediaPath);
    CHECK(loaded.language == "hu" && loaded.model == "whisper-small" && loaded.title == document.title);
    const QString before = loaded.exportText("json");
    for (const QString &invalid : {
             QString("{"), QString("[]"), QString("{\"format\":\"wrong\",\"version\":1,\"cues\":[]}"),
             QString("{\"format\":\"subtitle-desk\",\"version\":2,\"cues\":[]}"),
             QString("{\"format\":\"subtitle-desk\",\"version\":1}"),
             QString("{\"format\":\"subtitle-desk\",\"version\":1,\"cues\":[{\"startMs\":0,\"endMs\":0,\"text\":\"x\"}]}"),
             QString("{\"format\":\"subtitle-desk\",\"version\":1,\"cues\":[{\"startMs\":-1,\"endMs\":1,\"text\":\"x\"}]}"),
             QString("{\"format\":\"subtitle-desk\",\"version\":1,\"cues\":[{\"startMs\":0.5,\"endMs\":1,\"text\":\"x\"}]}"),
             QString("{\"format\":\"subtitle-desk\",\"version\":1,\"cues\":[{\"startMs\":0,\"endMs\":1,\"text\":42}]}"),
             QString("{\"format\":\"subtitle-desk\",\"version\":1,\"cues\":[{\"startMs\":0,\"endMs\":1,\"text\":\"x\",\"reviewed\":\"true\"}]}"),
             QString("{\"format\":\"subtitle-desk\",\"version\":1,\"mediaPath\":[],\"cues\":[]}")}) {
        CHECK(!loaded.importText(invalid, "json", &error));
        CHECK(!error.isEmpty());
        CHECK(loaded.exportText("json") == before);
    }
    CHECK(loaded.importText("{\"format\":\"subtitle-desk\",\"version\":1,\"cues\":[]}", "sdesk", &error));
    CHECK(loaded.cues.isEmpty());
    CHECK(loaded.language == "auto");
    CHECK(!loaded.importText("hello", "txt", &error));
    CHECK(error.contains("not supported"));
}

void timedSubtitleTests()
{
    SubtitleDocument original;
    const QString complex = QString::fromUtf8("Literal <i>tag</i>, A & B, &lt;, 😀\nSecond line\n\n  \n日本語");
    original.cues = {cue(1, 2345, complex), cue(3723456, 3726789, "Later"), cue(3730000, 3731000, "")};
    QString error;
    for (const QString &format : {QString("srt"), QString("vtt")}) {
        const QString text = original.exportText(format, &error);
        CHECK(error.isEmpty());
        CHECK(text.contains("&lt;i&gt;tag&lt;/i&gt;"));
        CHECK(text.contains("A &amp; B"));
        SubtitleDocument loaded;
        loaded.mediaPath = "/keep/this.mp4";
        loaded.model = "tiny";
        CHECK(loaded.importText(text, format, &error));
        CHECK(loaded.cues.size() == original.cues.size());
        if (loaded.cues.size() == original.cues.size()) {
            for (qsizetype i = 0; i < loaded.cues.size(); ++i) {
                CHECK(loaded.cues.at(i).startMs == original.cues.at(i).startMs);
                CHECK(loaded.cues.at(i).endMs == original.cues.at(i).endMs);
                CHECK(loaded.cues.at(i).text == original.cues.at(i).text);
            }
        }
        CHECK(loaded.mediaPath == "/keep/this.mp4" && loaded.model == "tiny");
    }
    SubtitleDocument srt;
    CHECK(srt.importText(QString(QChar(0xfeff)) + "42\r\n00:00:01,000 --> 00:00:02,000\r\n<i>Hello</i> &amp;lt; &#x1F600;\r\nNext\r\n", "srt", &error));
    CHECK(srt.cues.first().text == QString::fromUtf8("Hello &lt; 😀\nNext"));
    CHECK(srt.importText("00:00:01.000 --> 00:00:02.000\nWithout number\n", "srt", &error));
    const QString before = srt.exportText("json");
    CHECK(!srt.importText("1\n00:00:02,000 --> 00:00:01,000\nWrong", "srt", &error));
    CHECK(srt.exportText("json") == before);
    CHECK(!srt.importText("1\n00:00:01,000 --> 00:00:02,000\nGood\n\nBroken block", "srt", &error));
    CHECK(srt.exportText("json") == before); // No partial replacement.
    SubtitleDocument vtt;
    const QString web = "WEBVTT a description\nKind: captions\nLanguage: en\n\nNOTE comment\nNot a cue\n\n"
                        "STYLE\n::cue { color: white; }\n\nREGION\nid:r1\n\n"
                        "a cue id\n00:01.000 --> 00:02.000 align:start position:10%\n<v Speaker><c.green>Hello</c> <00:01.500>there</v>\n";
    CHECK(vtt.importText(web, "webvtt", &error));
    CHECK(vtt.cues.size() == 1);
    CHECK(vtt.cues.first().text == "Hello there");
    CHECK(!vtt.importText("00:01.000 --> 00:02.000\nMissing header", "vtt", &error));
    CHECK(!vtt.importText("WEBVTT\n00:01.000 --> 00:02.000\nMissing blank", "vtt", &error));
}

void assTests()
{
    SubtitleDocument document;
    const QString text = QString::fromUtf8("Literal {\\bord5} and \\N and \\h and C:\\new, \"quote\"\n😀\u00a0word");
    document.cues = {cue(1001, 1999, text), cue(3600001, 3600002, "Very short")};
    QString error;
    const QString exported = document.exportText("ass", &error);
    CHECK(error.isEmpty());
    CHECK(exported.contains("[V4+ Styles]"));
    CHECK(exported.contains("1:00:00.00,1:00:00.01"));
    SubtitleDocument loaded;
    CHECK(loaded.importText(exported, "ass", &error));
    CHECK(loaded.cues.size() == 2);
    CHECK(loaded.cues.at(0).text == text);
    CHECK(loaded.cues.at(0).startMs == 1000 && loaded.cues.at(0).endMs == 2000);
    document.cues = {cue(1000, 2000, "  leading and trailing spaces  ")};
    CHECK(loaded.importText(document.exportText("ass"), "ass", &error));
    CHECK(loaded.cues.first().text == document.cues.first().text);
    CHECK(loaded.importText("[Events]\nFormat: End, Start, Text\nComment: 0:00:02.00,0:00:01.00,ignored\nDialogue: 0:00:02.00,0:00:01.00,{\\i1}Styled{\\i0}, comma\\Nnew line", "ass", &error));
    CHECK(loaded.cues.first().text == "Styled, comma\nnew line");
    CHECK(loaded.cues.first().startMs == 1000 && loaded.cues.first().endMs == 2000);
    CHECK(!loaded.importText("[Events]\nFormat: Text, Start, End\nDialogue: x,0:00:01.00,0:00:02.00", "ass", &error));
    CHECK(!loaded.importText("[Events]\nDialogue: 0,0:00:01.00", "ass", &error));
    CHECK(!loaded.importText("[Events]\nFormat: Start, End, Text\nDialogue: 0:00:02.00,0:00:01.00,x", "ssa", &error));
}

void lrcTests()
{
    QString error;
    SubtitleDocument document;
    const QString input = "[ti:Song title]\n[ar:Artist]\n[offset:500]\n[length:00:12]\n"
                          "[00:01.00][00:05.00]Repeated line\n[00:01.00]Duet line\n[00:08.123]<00:08.123>Last <00:08.700>word\n";
    CHECK(document.importText(input, "lrc", &error));
    CHECK(document.title == "Song title");
    CHECK(document.cues.size() == 4);
    CHECK(document.cues.at(0).startMs == 500 && document.cues.at(0).endMs == 4500);
    CHECK(document.cues.at(1).startMs == 500 && document.cues.at(1).endMs == 4500);
    CHECK(document.cues.at(0).text == "Repeated line" && document.cues.at(1).text == "Duet line");
    CHECK(document.cues.at(2).startMs == 4500 && document.cues.at(2).endMs == 7623);
    CHECK(document.cues.at(3).startMs == 7623 && document.cues.at(3).endMs == 11500);
    CHECK(document.cues.at(3).text == "Last word");
    CHECK(document.importText("[offset:-100]\n[00:00.001]Start\n[00:01.000]End", "lrc", &error));
    CHECK(document.cues.at(0).startMs == 101);
    CHECK(document.cues.at(1).startMs == 1100 && document.cues.at(1).endMs == 4100);
    CHECK(document.importText("[offset:1000]\n[00:00.500]Pre-roll\n[00:00.700]More pre-roll", "lrc", &error));
    CHECK(document.cues.size() == 2 && document.cues.at(0).startMs == 0 && document.cues.at(1).startMs == 0);
    CHECK(document.cues.at(0).endMs == 3000 && document.cues.at(1).endMs == 3000);
    document.cues = {cue(3661234, 3670000, "First line\nSecond line")};
    const QString exported = document.exportText("lrc", &error);
    CHECK(exported == "[61:01.23]First line\n[61:01.23]Second line\n");
    CHECK(document.importText(exported, "lrc", &error));
    CHECK(document.cues.size() == 2);
    CHECK(document.cues.first().startMs == 3661230);
    document.cues = {cue(1001, 1005, "First"), cue(1005, 2000, "Second"), cue(3000, 4000, "Next")};
    CHECK(document.importText(document.exportText("lrc"), "lrc", &error));
    CHECK(document.cues.size() == 3); // Centisecond collisions retain every line.
    CHECK(document.cues.at(0).text == "First" && document.cues.at(1).text == "Second");
    CHECK(document.cues.at(0).startMs == 1000 && document.cues.at(1).startMs == 1000);
    CHECK(document.cues.at(0).endMs == 3000 && document.cues.at(1).endMs == 3000);
    const QString before = document.exportText("json");
    CHECK(!document.importText("[00:01.00]Good\nUntimed lost text", "lrc", &error));
    CHECK(document.exportText("json") == before);
    CHECK(!document.importText("[offset:not-a-number]\n[00:01.00]Text", "lrc", &error));
    CHECK(!document.importText("[00:60.00]Text", "lrc", &error));
}

void textAndCsvTests()
{
    SubtitleDocument document;
    document.cues = {cue(0, 1000, "A, \"quote\"\nnew line", "id,with\"quotes", true), cue(2000, 3000, "Second", "id2")};
    QString error;
    CHECK(document.exportText("txt", &error) == "A, \"quote\"\nnew line\n\nSecond\n");
    const QString csv = document.exportText("csv", &error);
    CHECK(csv.startsWith("id,start,end,start_ms,end_ms,text,reviewed\r\n"));
    CHECK(csv.contains("\"id,with\"\"quotes\""));
    CHECK(csv.contains("\"A, \"\"quote\"\"\nnew line\",true\r\n"));
    for (const QString &unsafe : {QString("=1+1"), QString("+SUM(A1)"), QString("-1+2"), QString("@SUM(A1)"),
             QString("  =1+1"), QString("\t=1+1"), QString("\r=1+1"), QString(QChar(0xfeff)) + "=1+1"}) {
        document.cues = {cue(0, 1000, unsafe, unsafe)};
        const QString safeCsv = document.exportText("csv", &error);
        CHECK(safeCsv.contains("\"'" + unsafe + "\""));
        CHECK(document.cues.first().text == unsafe && document.cues.first().id == unsafe);
        CHECK(document.exportText("txt").startsWith(unsafe.startsWith('\r') ? "\n" + unsafe.mid(1) : unsafe));
    }
    CHECK(document.exportText("unsupported", &error).isEmpty() && !error.isEmpty());
}

void editTests()
{
    SubtitleDocument document;
    document.cues = {cue(1000, 4000, "Hello world", "one", true), cue(3000, 5000, "Next", "two", true)};
    CHECK(document.cueAt(999) == -1);
    CHECK(document.cueAt(1000) == 0);
    CHECK(document.cueAt(4000) == 1);
    CHECK(document.cueAt(5000) == -1);
    CHECK(document.activeCuesAt(3500) == QVector<int>({0, 1}));
    CHECK(document.durationMs() == 5000);
    CHECK(document.timingWarnings().size() == 1);
    CHECK(document.validationErrors().isEmpty());
    QString error;
    CHECK(document.splitCue(0, 5, 2000, &error));
    CHECK(document.cues.size() == 3);
    CHECK(document.cues.at(0).text == "Hello" && document.cues.at(1).text == "world");
    CHECK(document.cues.at(0).endMs == 2000 && document.cues.at(1).startMs == 2000);
    CHECK(document.cues.at(0).id == "one" && document.cues.at(1).id != "one");
    CHECK(!document.cues.at(0).reviewed && !document.cues.at(1).reviewed);
    CHECK(document.mergeCues(0, 1, &error));
    CHECK(document.cues.size() == 2 && document.cues.first().text == "Hello\nworld");
    CHECK(document.cues.first().startMs == 1000 && document.cues.first().endMs == 4000);
    CHECK(document.shiftCues(-5000) == -1000);
    CHECK(document.cues.at(0).startMs == 0 && document.cues.at(1).startMs == 2000);
    CHECK(document.cues.at(1).endMs == 4000);
    CHECK(document.shiftCues(500, {1, 1, -1, 500}) == 500); // Duplicate index applied once.
    CHECK(document.cues.at(0).startMs == 0 && document.cues.at(1).startMs == 2500);
    CHECK(document.shiftCues(500, {-1, 500}) == 0);
    CHECK(!document.mergeCues(0, 0, &error));
    CHECK(!document.splitCue(0, 0, 0, &error));
    CHECK(!document.splitCue(-1, 2, 0, &error));
    document.cues = {cue(1000, 5000, "alpha beta", "x", true)};
    CHECK(document.splitCue(0, 5, 6000, &error));
    CHECK(document.cues.at(0).endMs == 3000 && document.cues.at(1).startMs == 3000); // Midpoint fallback.
    document.cues = {cue(0, 1000, QString::fromUtf8("A😀B"))};
    CHECK(document.splitCue(0, 2, 500, &error)); // Cursor inside surrogate moves to its end.
    CHECK(document.cues.at(0).text == QString::fromUtf8("A😀") && document.cues.at(1).text == "B");
    document.cues = {cue(0, 1, "tiny cue")};
    CHECK(!document.splitCue(0, 4, 0, &error));
    document.cues = {cue(1000, 2000, "x", "same"), cue(0, 1000, "y", "same"), cue(0, 500, "z")};
    document.stableSort();
    CHECK(document.cues.at(0).text == "y" && document.cues.at(1).text == "z");
    document.normalizeIds();
    QSet<QString> identifiers;
    for (const Cue &item : document.cues) identifiers.insert(item.id);
    CHECK(identifiers.size() == 3 && !identifiers.contains(""));
    CHECK(document.shiftCues(std::numeric_limits<qint64>::min()) == 0);
    CHECK(document.shiftCues(std::numeric_limits<qint64>::max()) == 9007199254740991LL - 2000);
    CHECK(document.durationMs() == 9007199254740991LL);
    CHECK(document.validationErrors().isEmpty());
    document.cues = {cue(0, 1000, "a"), cue(1000, 2000, "b")};
    CHECK(document.timingWarnings().isEmpty());
    document.cues += cue(3000, 4000, " ");
    CHECK(document.timingWarnings().size() == 1);
    const auto previous = document.cues;
    document.cues = {cue(0, 1000, QString(43, 'A') + "\nshort\nthird")};
    CHECK(document.timingWarnings().size() == 3);
    CHECK(document.timingWarnings().join('\n').contains("43-character line"));
    CHECK(document.timingWarnings().join('\n').contains("3 lines"));
    CHECK(document.timingWarnings().join('\n').contains("characters per second"));
    CHECK(document.validationErrors().isEmpty()); // Readability hints never block saving/export.
    document.cues = previous;
    document.cues += cue(5000, 5000, "invalid");
    CHECK(document.validationErrors().size() == 1);
    CHECK(document.exportText("srt", &error).isEmpty() && !error.isEmpty());
    document.cues = {cue(0, 1000, QString("a") + QChar(0) + "b")};
    CHECK(!document.validationErrors().isEmpty());
}

void fileTests()
{
    QTemporaryDir temporary;
    CHECK(temporary.isValid());
    const QString mediaPath = temporary.filePath(QString::fromUtf8("árvíz & music.mp4"));
    const QString projectPath = temporary.filePath("edit.sdesk");
    SubtitleDocument original;
    original.mediaPath = mediaPath;
    original.cues = {cue(0, 1234, QString::fromUtf8("Unicode: 日本語, őű, 😀"), "one", true)};
    QString error;
    CHECK(original.saveProject(projectPath, &error));
    QFile project(projectPath);
    CHECK(project.open(QIODevice::ReadOnly));
    const QByteArray saved = project.readAll();
    const auto object = QJsonDocument::fromJson(saved).object();
    CHECK(QDir::isRelativePath(object.value("mediaPath").toString()));
    project.close();
    SubtitleDocument loaded;
    CHECK(loaded.load(projectPath, &error));
    CHECK(QDir::cleanPath(loaded.mediaPath) == QDir::cleanPath(mediaPath));
    CHECK(loaded.cues.first().text == original.cues.first().text && loaded.cues.first().reviewed);
    original.cues.first().endMs = 0;
    CHECK(!original.saveProject(projectPath, &error));
    CHECK(project.open(QIODevice::ReadOnly));
    CHECK(project.readAll() == saved); // Validation failure must never truncate an existing file.
    project.close();
    original.cues.first().endMs = 1234;
    const QString srtPath = temporary.filePath("utf8.srt");
    CHECK(original.exportFile(srtPath, "srt", &error));
    CHECK(loaded.load(srtPath, &error));
    CHECK(loaded.cues.first().text == original.cues.first().text);
    const QString utf16Path = temporary.filePath("utf16.srt");
    const QString srt = original.exportText("srt");
    QStringEncoder encoder(QStringConverter::Utf16LE);
    const QByteArray utf16 = QByteArray::fromHex("fffe") + encoder.encode(srt);
    CHECK(writeBytes(utf16Path, utf16));
    CHECK(loaded.load(utf16Path, &error));
    CHECK(loaded.cues.first().text == original.cues.first().text);
    const QString invalidPath = temporary.filePath("invalid.srt");
    CHECK(writeBytes(invalidPath, QByteArray::fromHex("fffffe80")));
    const QString before = loaded.exportText("json");
    CHECK(!loaded.load(invalidPath, &error));
    CHECK(error.contains("encoding"));
    CHECK(loaded.exportText("json") == before);
    CHECK(!loaded.load(temporary.filePath("missing.srt"), &error));
    CHECK(!loaded.exportFile(temporary.filePath("missing/sub/file.srt"), "srt", &error));
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    timeTests();
    projectTests();
    timedSubtitleTests();
    assTests();
    lrcTests();
    textAndCsvTests();
    editTests();
    fileTests();
    QTextStream(stdout) << "Subtitle document: " << (checks - failures) << '/' << checks << " checks passed.\n";
    return failures == 0 ? 0 : 1;
}
