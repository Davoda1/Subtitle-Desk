#include "transcription_service.h"
#include "model_catalog.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
constexpr qint64 SampleRate = 16000;
constexpr qint64 BytesPerMs = 32;
constexpr qsizetype StdoutLimit = 16 * 1024 * 1024;
constexpr qsizetype StderrLimit = 1024 * 1024;
constexpr qint64 JsonLimit = 64 * 1024 * 1024;
QString modelId(QString id) {
    id = id.trimmed().toLower();
    if (id.startsWith("whisper-")) id.remove(0, 8);
    return id;
}
QString joinWord(const QString &a, const QString &b) {
    if (a.isEmpty()) return b;
    if (b.isEmpty()) return a;
    if (QStringLiteral(",.;:!?)]}’'\"").contains(b.front())) return a + b;
    return a + QLatin1Char(' ') + b;
}
bool numberMs(const QJsonValue &value, double scale, qint64 *out) {
    if (!value.isDouble()) return false;
    double n = value.toDouble() * scale;
    if (!std::isfinite(n) || n < 0 || n > 7.0 * 86400000.0) return false;
    *out = qRound64(n);
    return true;
}
QString displayModel(const QString &id) {
    if (id == "whistle") return QStringLiteral("Whistle");
    if (id.isEmpty()) return QStringLiteral("Whisper");
    QString title = id; title[0] = title[0].toUpper();
    return QStringLiteral("Whisper ") + title;
}
}

struct TranscriptionService::ModelLease {
    QPointer<ModelCatalog> catalog;
    // Keep the process-level lock alive even if the catalog is destroyed while
    // a recognizer is running. Its resource root and model path are captured.
    std::shared_ptr<QLockFile> lock;
    QString id;
    ~ModelLease() { if (catalog) catalog->releaseModel(id); }
};

TranscriptionService::TranscriptionService(QObject *parent) : QObject(parent) {
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        abortWithError(QStringLiteral("The recognition worker exceeded its processing time limit. Try a smaller model or a shorter audio file."));
    });
}
TranscriptionService::~TranscriptionService() {
    ++m_generation;
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(2000);
    }
    m_modelLease.reset();
}
void TranscriptionService::setResourceRoot(const QString &path) {
    if (!m_busy) m_root = QDir(path).absolutePath();
}
void TranscriptionService::setModelCatalog(ModelCatalog *catalog) {
    if (!m_busy) m_catalog = catalog;
}
QString TranscriptionService::resourceRoot() const {
    if (m_busy && !m_workerRoot.isEmpty()) return m_workerRoot;
    if (m_catalog) return m_catalog->resourceRoot();
    if (!m_root.isEmpty()) return m_root;
    const QDir dir(QCoreApplication::applicationDirPath());
    const QString adjacent = dir.filePath(QStringLiteral("resources"));
    if (QFileInfo::exists(adjacent + QStringLiteral("/models/whistle.cact"))) return adjacent;
#ifndef Q_OS_WIN
    const QString shared = dir.filePath(QStringLiteral("../share/subtitledesk/resources"));
    if (QFileInfo::exists(shared + QStringLiteral("/models/whistle.cact"))) return QDir(shared).absolutePath();
    const QString hyphenShared = dir.filePath(QStringLiteral("../share/subtitle-desk/resources"));
    if (QFileInfo::exists(hyphenShared + QStringLiteral("/models/whistle.cact"))) return QDir(hyphenShared).absolutePath();
#endif
    return adjacent;
}
QString TranscriptionService::enginePath(const QString &name) const {
#ifdef Q_OS_WIN
    return QDir(resourceRoot()).filePath(QStringLiteral("bin/") + name + QStringLiteral(".exe"));
#else
    return QDir(resourceRoot()).filePath(QStringLiteral("bin/") + name);
#endif
}
QString TranscriptionService::availability(const QString &rawModel) const {
    const QString id = modelId(rawModel);
    if (m_catalog) return m_catalog->availability(id);
    if (!QStringList{"whistle", "tiny", "base", "small"}.contains(id))
        return QStringLiteral("Choose Whistle, Whisper Tiny, Whisper Base, or Whisper Small.");
    const QString weights = QDir(resourceRoot()).filePath(QStringLiteral("models/") +
        (id == "whistle" ? QStringLiteral("whistle.cact") : QStringLiteral("ggml-%1-q5_1.bin").arg(id)));
    const QFileInfo model(weights);
    if (!model.isFile() || !model.isReadable() || model.size() < 1000000)
        return QStringLiteral("The bundled %1 model is missing or unreadable. Keep the complete resources folder beside the application.").arg(displayModel(id));
    const QFileInfo engine(enginePath(id == "whistle" ? QStringLiteral("needle") : QStringLiteral("whisper-cli")));
    if (!engine.isFile() || !engine.isReadable()
#ifndef Q_OS_WIN
        || !engine.isExecutable()
#endif
    ) return QStringLiteral("The bundled %1 recognition engine is missing or cannot be executed.").arg(displayModel(id));
    return {};
}
QString TranscriptionService::modelPath() const {
    return m_modelPath;
}
bool TranscriptionService::inspectWav(const QString &path, WavInfo &info, QString &error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error = QStringLiteral("The decoded audio file cannot be opened: ") + file.errorString(); return false; }
    const QByteArray header = file.read(12);
    if (header.size() != 12 || header.left(4) != "RIFF" || header.mid(8,4) != "WAVE") {
        error = QStringLiteral("Recognition requires a decoded PCM WAV audio file."); return false;
    }
    bool validFormat = false;
    while (!file.atEnd()) {
        QByteArray chunk = file.read(8);
        if (chunk.size() != 8) break;
        const quint32 size = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(chunk.constData() + 4));
        const qint64 begin = file.pos();
        if (qint64(size) > file.size() - begin) { error = QStringLiteral("The decoded audio file is incomplete."); return false; }
        if (chunk.left(4) == "fmt ") {
            const QByteArray fmt = file.read(qMin<quint32>(size, 40));
            if (fmt.size() >= 16) {
                const auto p = reinterpret_cast<const uchar *>(fmt.constData());
                const auto format = qFromLittleEndian<quint16>(p);
                validFormat = format == 1 && qFromLittleEndian<quint16>(p + 2) == 1
                    && qFromLittleEndian<quint32>(p + 4) == SampleRate
                    && qFromLittleEndian<quint16>(p + 14) == 16;
            }
        } else if (chunk.left(4) == "data") {
            info.dataOffset = begin; info.bytes = size;
        }
        if (!file.seek(begin + qint64(size) + (size & 1))) break;
    }
    if (!validFormat || !info.dataOffset || info.bytes < 2 || info.bytes % 2) {
        error = QStringLiteral("Recognition requires mono 16 kHz, 16-bit PCM audio. Reopen the original media to decode it again."); return false;
    }
    info.durationMs = info.bytes / BytesPerMs;
    if (info.durationMs < 20) { error = QStringLiteral("The audio file is too short to transcribe."); return false; }
    return true;
}
void TranscriptionService::start(const QString &wavPath, const QString &id, const QString &language, int threads, int maxChars) {
    if (m_busy) { emit failed(QStringLiteral("A transcription is already running.")); return; }
    const QString normalized = modelId(id);
    QString error = availability(normalized);
    if (!error.isEmpty()) { emit failed(error); return; }
    ModelInfo selected;
    if (m_catalog) selected = m_catalog->model(normalized);
    else {
        selected.id = normalized; selected.name = displayModel(normalized); selected.bundled = true;
        selected.engine = normalized == QStringLiteral("whistle") ? QStringLiteral("whistle") : QStringLiteral("whisper");
        selected.path = QDir(resourceRoot()).filePath(QStringLiteral("models/") +
            (normalized == QStringLiteral("whistle") ? QStringLiteral("whistle.cact") : QStringLiteral("ggml-%1-q5_1.bin").arg(normalized)));
    }
    QString lang = language.trimmed().toLower();
    if (lang.isEmpty()) lang = QStringLiteral("auto");
    if (!QRegularExpression(QStringLiteral("^(auto|[a-z]{2,3})$")).match(lang).hasMatch()) {
        emit failed(QStringLiteral("Choose a valid transcription language.")); return;
    }
    if (selected.englishOnly && lang != QStringLiteral("auto") && lang != QStringLiteral("en")) {
        emit failed(QStringLiteral("This Whisper model recognizes English only. Choose English or automatic language detection, or select a multilingual model.")); return;
    }
    if (selected.englishOnly && lang == QStringLiteral("auto")) lang = QStringLiteral("en");
    if (selected.engine == "whistle" && !QStringList{"auto","en","de","fr","es","it","nl","pl"}.contains(lang)) {
        emit failed(QStringLiteral("Whistle supports English, German, French, Spanish, Italian, Dutch, and Polish. Choose a Whisper model for this language.")); return;
    }
    WavInfo info;
    if (!inspectWav(wavPath, info, error)) { emit failed(error); return; }
    m_temp = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/subtitle-desk-stt-XXXXXX"));
    if (!m_temp->isValid()) { m_temp.reset(); emit failed(QStringLiteral("A temporary transcription directory could not be created.")); return; }
    if (m_catalog) {
        if (!m_catalog->acquireModel(normalized, &selected, &error)) { m_temp.reset(); emit failed(error); return; }
        m_modelLease = std::make_shared<ModelLease>();
        m_modelLease->catalog = m_catalog; m_modelLease->id = normalized;
        m_modelLease->lock = m_catalog->m_leases.value(normalized);
    }
    m_wav = QFileInfo(wavPath).absoluteFilePath(); m_model = normalized; m_language = lang;
    m_engine = selected.engine; m_modelPath = selected.path; m_modelName = selected.name;
    m_workerRoot = resourceRoot();
    m_info = info; m_threads = qBound(1, threads, 32); m_maxChars = qBound(16, maxChars, 160);
    m_busy = true; m_cancelling = false; m_approximate = false; ++m_generation; m_cues.clear();
    m_coreStart = 0; m_coreEnd = 0;
    emit progress(0, QStringLiteral("Loading %1 · processing locally on the CPU…").arg(m_modelName));
    if (!m_busy || m_cancelling) return;
    if (m_engine == "whistle") runWhistleChunk(); else runWhisper();
}
void TranscriptionService::runWhisper() {
    if (!m_busy || m_cancelling || !m_temp) return;
    const QString base = m_temp->filePath(QStringLiteral("transcript"));
    const QStringList args {QStringLiteral("-m"), modelPath(), QStringLiteral("-f"), m_wav,
        QStringLiteral("-l"), m_language, QStringLiteral("-oj"), QStringLiteral("-of"), base,
        QStringLiteral("-t"), QString::number(m_threads), QStringLiteral("--no-gpu"),
        QStringLiteral("-pp"), QStringLiteral("-ml"), QString::number(m_maxChars), QStringLiteral("-sow")};
    startWorker(enginePath(QStringLiteral("whisper-cli")), args);
}
void TranscriptionService::startWorker(const QString &program, const QStringList &arguments) {
    if (!m_busy || m_cancelling) return;
    m_stdout.clear(); m_stderr.clear();
    QProcess *p = m_process = new QProcess(this);
#ifdef Q_OS_WIN
    p->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    p->setProgram(program); p->setArguments(arguments);
    p->setWorkingDirectory(m_temp->path());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("NEEDLE_TELEMETRY"), QStringLiteral("0"));
    env.insert(QStringLiteral("DO_NOT_TRACK"), QStringLiteral("1"));
    env.insert(QStringLiteral("HF_HUB_OFFLINE"), QStringLiteral("1"));
    env.insert(QStringLiteral("OMP_NUM_THREADS"), QString::number(m_threads));
#ifndef Q_OS_WIN
    QString libs = QDir(resourceRoot()).filePath(QStringLiteral("lib")) + QLatin1Char(':') +
        QDir(resourceRoot()).filePath(QStringLiteral("bin")) + QLatin1Char(':') +
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("lib"));
    if (!env.value(QStringLiteral("LD_LIBRARY_PATH")).isEmpty()) libs += QLatin1Char(':') + env.value(QStringLiteral("LD_LIBRARY_PATH"));
    env.insert(QStringLiteral("LD_LIBRARY_PATH"), libs);
#endif
    p->setProcessEnvironment(env);
    const quint64 generation = m_generation;
    connect(p, &QProcess::readyReadStandardOutput, this, &TranscriptionService::collectOutput);
    connect(p, &QProcess::readyReadStandardError, this, &TranscriptionService::collectOutput);
    connect(p, qOverload<int,QProcess::ExitStatus>(&QProcess::finished), this,
        [this,p,generation](int code, QProcess::ExitStatus exit) {
            if (generation == m_generation && p == m_process) finishWorker(code, exit);
        });
    connect(p, &QProcess::errorOccurred, this, [this,p,generation](QProcess::ProcessError error) {
        if (generation != m_generation || p != m_process) return;
        if (error == QProcess::FailedToStart) {
            if (m_cancelling) { reset(); emit cancelled(); }
            else abortWithError(QStringLiteral("The recognition engine could not start: ") + p->errorString());
        }
    });
    // Allow long recordings and slower CPUs; Whistle runs in bounded chunks.
    m_timeout.start(m_engine == "whistle" ? 10 * 60 * 1000 : 6 * 60 * 60 * 1000);
    p->start();
}
void TranscriptionService::collectOutput() {
    if (!m_process) return;
    m_stdout += m_process->readAllStandardOutput();
    const QByteArray errors = m_process->readAllStandardError();
    m_stderr += errors;
    if (m_stdout.size() > StdoutLimit) { abortWithError(QStringLiteral("The recognition engine produced too much output.")); return; }
    if (m_stderr.size() > StderrLimit) m_stderr = m_stderr.right(StderrLimit);
    if (m_engine != "whistle" && !m_cancelling) {
        static const QRegularExpression re(QStringLiteral("progress\\s*=\\s*([0-9]{1,3})%"));
        auto matches = re.globalMatch(QString::fromUtf8(errors));
        int last = -1;
        while (matches.hasNext()) last = qBound(0, matches.next().captured(1).toInt(), 99);
        if (last >= 0) emit progress(last, QStringLiteral("Transcribing with %1 · %2%").arg(m_modelName).arg(last));
    }
}
bool TranscriptionService::writeWhistleChunk(QString &path, QString &error) {
    QFile source(m_wav);
    if (!source.open(QIODevice::ReadOnly)) { error = QStringLiteral("The decoded audio is no longer available."); return false; }
    m_coreEnd = qMin(m_info.durationMs, m_coreStart + 26000);
    // Prefer a quiet boundary within the final six seconds of the core window.
    // Word timestamps remain recognizer estimates; these boundaries only bound memory.
    if (m_coreEnd < m_info.durationMs) {
        const qint64 searchStart = m_coreStart + 20000;
        const qint64 searchBytes = (m_coreEnd - searchStart) * BytesPerMs;
        if (!source.seek(m_info.dataOffset + searchStart * BytesPerMs)) { error = QStringLiteral("Could not seek in decoded audio."); return false; }
        const QByteArray audio = source.read(searchBytes);
        double minimum = std::numeric_limits<double>::max();
        qint64 best = m_coreEnd;
        for (qsizetype offset = 0; offset + 640 <= audio.size(); offset += 640) {
            double energy = 0;
            for (qsizetype i = offset; i < offset + 640; i += 2) {
                const qint16 sample = qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(audio.constData() + i));
                energy += double(sample) * double(sample);
            }
            // Slightly prefer later boundaries when the audio is equally quiet.
            if (energy <= minimum) { minimum = energy; best = searchStart + offset / BytesPerMs + 10; }
        }
        m_coreEnd = best;
    }
    m_chunkStart = qMax<qint64>(0, m_coreStart - 750);
    m_chunkEnd = qMin(m_info.durationMs, m_coreEnd + 750);
    const qint64 byteCount = (m_chunkEnd - m_chunkStart) * BytesPerMs;
    if (byteCount <= 0 || !source.seek(m_info.dataOffset + m_chunkStart * BytesPerMs)) {
        error = QStringLiteral("Could not prepare the next audio section."); return false;
    }
    QByteArray audio = source.read(byteCount);
    if (audio.size() != byteCount) { error = QStringLiteral("The decoded audio was truncated during recognition."); return false; }
    path = m_temp->filePath(QStringLiteral("section.wav"));
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) { error = QStringLiteral("Could not write an audio section: ") + output.errorString(); return false; }
    QByteArray header(44, '\0');
    memcpy(header.data(), "RIFF", 4); memcpy(header.data()+8, "WAVEfmt ", 8); memcpy(header.data()+36, "data", 4);
    auto p = reinterpret_cast<uchar *>(header.data());
    qToLittleEndian<quint32>(quint32(byteCount + 36), p+4); qToLittleEndian<quint32>(16,p+16);
    qToLittleEndian<quint16>(1,p+20); qToLittleEndian<quint16>(1,p+22);
    qToLittleEndian<quint32>(16000,p+24); qToLittleEndian<quint32>(32000,p+28);
    qToLittleEndian<quint16>(2,p+32); qToLittleEndian<quint16>(16,p+34);
    qToLittleEndian<quint32>(quint32(byteCount),p+40);
    if (output.write(header) != header.size() || output.write(audio) != audio.size() || !output.flush()) {
        error = QStringLiteral("Could not finish writing an audio section: ") + output.errorString(); return false;
    }
    return true;
}
void TranscriptionService::runWhistleChunk() {
    if (!m_busy || m_cancelling) return;
    if (m_coreStart >= m_info.durationMs) { finishSuccess(); return; }
    QString path, error;
    if (!writeWhistleChunk(path, error)) { abortWithError(error); return; }
    QStringList args {QStringLiteral("--model"), modelPath(), QStringLiteral("--audio"), path,
        QStringLiteral("--audio-word-timestamps"),
        QStringLiteral("--threads"), QString::number(m_threads)};
    if (m_language != "auto") args << QStringLiteral("--audio-language") << m_language;
    emit progress(int(100 * m_coreStart / m_info.durationMs),
        QStringLiteral("%1 · %2 / %3 · local CPU processing").arg(m_modelName, SubtitleDocument::formatTime(m_coreStart),SubtitleDocument::formatTime(m_info.durationMs)));
    startWorker(enginePath(QStringLiteral("needle")), args);
}
bool TranscriptionService::parseWhisper(const QByteArray &json, QString &error) {
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parse);
    if (!doc.isObject() || !doc.object().value(QStringLiteral("transcription")).isArray()) {
        error = QStringLiteral("Whisper did not return a readable timestamped transcript."); return false;
    }
    const QJsonArray rows = doc.object().value(QStringLiteral("transcription")).toArray();
    if (rows.size() > 250000) { error = QStringLiteral("The transcript contains too many segments."); return false; }
    for (const auto &value : rows) {
        if (!value.isObject()) { error = QStringLiteral("Whisper returned an invalid transcript segment."); return false; }
        const auto row = value.toObject();
        if (!row.value(QStringLiteral("text")).isString()) { error = QStringLiteral("Whisper returned an invalid transcript text field."); return false; }
        const QString text = row.value(QStringLiteral("text")).toString().trimmed();
        if (text.isEmpty()) continue;
        qint64 start = 0, end = 0;
        const auto offsets = row.value(QStringLiteral("offsets")).toObject();
        if (!numberMs(offsets.value(QStringLiteral("from")),1,&start) || !numberMs(offsets.value(QStringLiteral("to")),1,&end)) {
            bool startOk = false, endOk = false;
            const auto times = row.value(QStringLiteral("timestamps")).toObject();
            start = SubtitleDocument::parseTime(times.value(QStringLiteral("from")).toString(),&startOk);
            end = SubtitleDocument::parseTime(times.value(QStringLiteral("to")).toString(),&endOk);
            if (!startOk || !endOk) { error = QStringLiteral("Whisper returned a segment without valid timestamps."); return false; }
        }
        start = qMin(start, m_info.durationMs - 1); end = qMin(end, m_info.durationMs);
        if (end <= start) { end = qMin(start + 100, m_info.durationMs); m_approximate = true; }
        m_cues.append(Cue{SubtitleDocument::newCueId(),start,end,text,false});
    }
    return true;
}
bool TranscriptionService::parseWhistle(const QByteArray &output, QString &error) {
    const qsizetype first = output.indexOf('{'), last = output.lastIndexOf('}');
    if (first < 0 || last < first) { error = QStringLiteral("Whistle did not return a readable transcript."); return false; }
    const QJsonDocument doc = QJsonDocument::fromJson(output.mid(first,last-first+1));
    if (!doc.isObject() || !doc.object().value(QStringLiteral("text")).isString()) {
        error = QStringLiteral("Whistle did not return a readable transcript."); return false;
    }
    const auto object = doc.object();
    const QString transcript = object.value(QStringLiteral("text")).toString().trimmed();
    const auto wordsValue = object.value(QStringLiteral("words"));
    const auto words = wordsValue.toArray();
    if (words.isEmpty()) {
        if (!transcript.isEmpty()) {
            // An older replacement engine can return text without timing data.
            // Keep it visibly approximate rather than inventing precise words.
            m_cues.append(Cue{SubtitleDocument::newCueId(),m_coreStart,m_coreEnd,transcript,false});
            m_approximate = true;
        }
        return true;
    }
    QString text; qint64 start = -1, end = -1;
    const auto flush = [&] {
        if (!text.isEmpty() && start >= 0 && end > start)
            m_cues.append(Cue{SubtitleDocument::newCueId(),start,end,text,false});
        text.clear(); start = end = -1;
    };
    for (const auto &value : words) {
        const auto word = value.toObject();
        if (!word.value(QStringLiteral("word")).isString()) { error = QStringLiteral("Whistle returned an invalid timed word."); return false; }
        QString token = word.value(QStringLiteral("word")).toString().trimmed();
        if (token.isEmpty()) continue;
        qint64 from = 0, to = 0;
        if (!numberMs(word.value(QStringLiteral("start")),1000,&from) || !numberMs(word.value(QStringLiteral("end")),1000,&to) || to < from) {
            error = QStringLiteral("Whistle returned invalid word timestamps."); return false;
        }
        from += m_chunkStart; to += m_chunkStart;
        const qint64 midpoint = from + (to-from)/2;
        if (midpoint < m_coreStart || midpoint >= m_coreEnd) continue;
        from = qMax(from,m_coreStart); to = qMin(to,m_coreEnd);
        if (to <= from) { to = qMin(m_coreEnd, from+40); m_approximate=true; }
        if (to <= from) continue;
        if (!text.isEmpty() && (joinWord(text,token).size() > m_maxChars || to-start > 6500 || from-end > 700)) flush();
        if (start < 0) start = from;
        text = joinWord(text,token); end = to;
        if (text.endsWith('.') || text.endsWith('!') || text.endsWith('?')) flush();
    }
    flush();
    return true;
}
void TranscriptionService::finishWorker(int code, QProcess::ExitStatus exit) {
    if (!m_process) return;
    collectOutput();
    if (!m_process) return;
    QProcess *p = m_process; m_process = nullptr; m_timeout.stop(); p->deleteLater();
    if (m_cancelling) { reset(); emit cancelled(); return; }
    if (exit != QProcess::NormalExit || code != 0) {
        QString detail = QString::fromUtf8(m_stderr.right(1800)).trimmed();
        if (detail.isEmpty()) detail = QString::fromUtf8(m_stdout.right(1800)).trimmed();
        abortWithError(QStringLiteral("%1 recognition failed (exit %2). %3").arg(m_modelName).arg(code).arg(detail)); return;
    }
    QString error;
    if (m_engine == "whistle") {
        if (!parseWhistle(m_stdout,error)) { abortWithError(error); return; }
        m_coreStart = m_coreEnd;
        const quint64 generation = m_generation;
        QTimer::singleShot(0,this,[this,generation] { if (generation==m_generation) runWhistleChunk(); });
    } else {
        QFile file(m_temp->filePath(QStringLiteral("transcript.json")));
        if (file.size() > JsonLimit || !file.open(QIODevice::ReadOnly)) {
            abortWithError(QStringLiteral("Whisper's transcript file is missing, unreadable, or too large.")); return;
        }
        if (!parseWhisper(file.readAll(),error)) { abortWithError(error); return; }
        finishSuccess();
    }
}
void TranscriptionService::finishSuccess() {
    std::stable_sort(m_cues.begin(),m_cues.end(),[](const Cue &a,const Cue &b){ return a.startMs<b.startMs; });
    QVector<Cue> result = m_cues;
    QString note = m_engine == "whistle"
        ? QStringLiteral("Whistle word timestamps grouped into subtitle lines. All recognition timings are estimates; review against the audio.")
        : QStringLiteral("Whisper segment timestamps. All recognition timings are estimates; review against the audio.");
    if (m_approximate) note += QStringLiteral(" Some segments use approximate section timing or repaired zero-length intervals; check those timings carefully.");
    if (result.isEmpty()) note = QStringLiteral("No speech was recognized. Try another model or check the audio track.");
    reset(); emit progress(100,QStringLiteral("Transcription complete · %1 segments").arg(result.size())); emit completed(result,note);
}
void TranscriptionService::cancel() {
    if (!m_busy || m_cancelling) return;
    m_cancelling=true; m_timeout.stop();
    emit progress(0,QStringLiteral("Cancelling recognition…"));
    if (!m_process) { reset(); emit cancelled(); return; }
    QProcess *p=m_process;
    if (p->state()==QProcess::NotRunning) { reset(); emit cancelled(); return; }
    p->terminate();
    const quint64 generation=m_generation;
    QTimer::singleShot(1500,this,[this,p,generation] { if (generation==m_generation && p==m_process) p->kill(); });
}
void TranscriptionService::abortWithError(const QString &error) {
    reset(); emit failed(error);
}
void TranscriptionService::reset() {
    ++m_generation; m_timeout.stop();
    if (m_process) {
        QProcess *p=m_process; m_process=nullptr;
        p->disconnect(this); p->kill();
        // Keep the temporary directory alive until Windows releases its files.
        auto temp = std::shared_ptr<QTemporaryDir>(m_temp.release());
        auto lease = std::move(m_modelLease);
        if (p->state()==QProcess::NotRunning) p->deleteLater();
        else connect(p,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),p,[p,temp,lease](int,QProcess::ExitStatus){p->deleteLater();});
    }
    m_modelLease.reset();
    m_temp.reset(); m_busy=false; m_cancelling=false; m_stdout.clear(); m_stderr.clear(); m_cues.clear();
    m_engine.clear(); m_modelPath.clear(); m_modelName.clear(); m_workerRoot.clear();
}
