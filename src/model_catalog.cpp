// Copyright (c) 2026 davoda1
// SPDX-License-Identifier: AGPL-3.0-only
#include "model_catalog.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
constexpr qint64 MinimumModelSize = 1000000;
constexpr qint64 MaximumModelSize = qint64(8) * 1024 * 1024 * 1024;
constexpr qint64 MetadataLimit = 16 * 1024;
constexpr qsizetype ProbeOutputLimit = 1024 * 1024;
constexpr int ProbeTimeoutMs = 5 * 60 * 1000;
using Cancel = std::shared_ptr<std::atomic_bool>;
using Progress = std::function<void(int, const QString &)>;

struct HeaderInfo { QString engine; bool englishOnly = false; };
struct ImportResult {
    QString id, error;
    bool cancelled = false;
    bool duplicate = false;
};

bool isLink(const QFileInfo &info) {
    return info.isSymLink() || info.isJunction();
}

// Reject links in every existing component, including Windows junctions. All
// model paths below are constructed from generated names, never from metadata.
bool safeDirectory(const QString &path, bool create = false) {
    QString current = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (;;) {
        const QFileInfo info(current);
        if (isLink(info) || (info.exists() && !info.isDir())) return false;
        const QString parent = info.dir().absolutePath();
        if (parent == current) break;
        current = parent;
    }
    if (create && !QDir().mkpath(path)) return false;
    const QFileInfo info(path);
    return !isLink(info) && info.isDir() && !info.canonicalFilePath().isEmpty();
}

bool safeFile(const QString &path) {
    const QFileInfo info(path);
    return !isLink(info) && info.isFile() && info.isReadable()
        && safeDirectory(info.absolutePath());
}

bool customId(const QString &id) {
    static const QRegularExpression pattern(QStringLiteral("^custom-[0-9a-f]{64}$"));
    return pattern.match(id).hasMatch();
}

QString modelFilename(const QString &engine) {
    return engine == QStringLiteral("whisper") ? QStringLiteral("model.bin") : QStringLiteral("model.cact");
}

QString enginePath(const QString &root, const QString &family) {
    QString name = family == QStringLiteral("whisper") ? QStringLiteral("whisper-cli") : QStringLiteral("needle");
#ifdef Q_OS_WIN
    name += QStringLiteral(".exe");
#endif
    return QDir(root).filePath(QStringLiteral("bin/") + name);
}

QString engineError(const QString &root, const QString &family) {
    const QFileInfo executable(enginePath(root, family));
    if (!executable.isFile() || !executable.isReadable()
#ifndef Q_OS_WIN
        || !executable.isExecutable()
#endif
    ) return QStringLiteral("The bundled %1 recognition engine is missing or cannot be executed. Keep the complete resources folder with the application.")
        .arg(family == QStringLiteral("whisper") ? QStringLiteral("Whisper") : QStringLiteral("Whistle"));
    return {};
}

QString displayName(QString name, const QString &fallback) {
    for (QChar &ch : name) {
        const auto category = ch.category();
        if (category == QChar::Other_Control || category == QChar::Other_Format
            || category == QChar::Separator_Line || category == QChar::Separator_Paragraph)
            ch = QLatin1Char(' ');
    }
    name = name.simplified().left(120);
    return name.isEmpty() ? fallback : name;
}

QString description(const ModelInfo &info) {
    if (info.engine == QStringLiteral("whistle"))
        return info.bundled ? QStringLiteral("Bundled Whistle speech model") : QStringLiteral("Imported Whistle speech model");
    return (info.bundled ? QStringLiteral("Bundled Whisper model · ") : QStringLiteral("Imported Whisper model · "))
        + (info.englishOnly ? QStringLiteral("English only") : QStringLiteral("Multilingual"));
}

quint32 u32(const QByteArray &data, qsizetype offset) {
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(data.constData() + offset));
}
quint64 u64(const QByteArray &data, qsizetype offset) {
    return qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(data.constData() + offset));
}
float f32(const QByteArray &data, qsizetype offset) {
    const quint32 bits = u32(data, offset);
    float result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

// whisper.cpp v1.9.5 src/whisper.cpp: GGML magic, eleven hyperparameters,
// mel filters and vocabulary precede the tensor data. Multilingual models have
// n_vocab >= 51865; a filename ending in .en is not evidence of that property.
bool inspectWhisper(QFile &file, HeaderInfo &result) {
    if (!file.seek(0)) return false;
    const QByteArray header = file.read(56);
    if (header.size() != 56 || u32(header, 0) != 0x67676d6c) return false;
    std::array<quint32, 11> h{};
    for (qsizetype i = 0; i < qsizetype(h.size()); ++i) h[size_t(i)] = u32(header, 4 + 4 * i);
    const bool geometry = (h[2] == 384 && h[3] == 6 && h[4] == 4)
        || (h[2] == 512 && h[3] == 8 && h[4] == 6)
        || (h[2] == 768 && h[3] == 12 && h[4] == 12)
        || (h[2] == 1024 && h[3] == 16 && h[4] == 24)
        || (h[2] == 1280 && h[3] == 20 && h[4] == 32);
    if (!geometry || h[0] < 51864 || h[0] > 51866 || h[1] != 1500 || h[5] != 448
        || h[6] != h[2] || h[7] != h[3] || h[8] < 1 || h[8] > h[4]
        || (h[9] != 80 && h[9] != 128) || (h[9] == 128 && h[2] != 1280)
        || h[10] / 1000 > 3 || h[10] % 1000 > 40
        || u32(header, 48) != h[9] || u32(header, 52) != 201)
        return false;
    if (!file.seek(56 + qint64(h[9]) * 201 * 4)) return false;
    const QByteArray vocabulary = file.read(4);
    if (vocabulary.size() != 4) return false;
    const quint32 words = u32(vocabulary, 0);
    if (words < 50000 || words > h[0]) return false;
    for (quint32 i = 0; i < words; ++i) {
        const QByteArray length = file.read(4);
        if (length.size() != 4) return false;
        const quint32 bytes = u32(length, 0);
        if (bytes > 4096 || bytes > quint64(file.size() - file.pos()) || !file.seek(file.pos() + bytes)) return false;
    }
    if (file.size() - file.pos() < MinimumModelSize / 2) return false;
    result.engine = QStringLiteral("whisper");
    result.englishOnly = h[0] == 51864;
    return true;
}

// The Needle 3 container is shared by text and speech models. Require the
// positional Whistle audio manifest, not just the .cact extension or magic.
// Layout: cactus-compute/needle needle/model/export.py (tag 0x05E12A84).
bool inspectWhistle(QFile &file, HeaderInfo &result) {
    if (!file.seek(0)) return false;
    const QByteArray header = file.read(196);
    if (header.size() != 196 || u32(header, 0) != 0x05e12a84) return false;
    std::array<quint32, 48> h{};
    for (qsizetype i = 0; i < qsizetype(h.size()); ++i) h[size_t(i)] = u32(header, 4 * i);
    if (h[1] < 100 || h[1] > 4096 || h[2] != 28 || h[3] > 320
        || (h[4] != 2 && h[4] != 3 && h[4] != 4 && h[4] != 8)
        || h[5] < 8192 || h[5] > 65536 || h[6] > h[5]
        || h[7] != 512 || h[8] != 8 || h[9] != 2 || h[10] < 2 || h[10] > 8
        || h[11] != 48 || h[12] != 64 || h[13] != 320 || h[14] != 512 || h[15] != 4
        || h[16] > 320 || h[19] != 3 || h[20] > 1048576 || h[21] != 128
        || h[22] > 16 || h[23] > 16 || h[24] > 16 || h[25] > 16
        || h[26] > 4 || h[31] > 16 || !std::isfinite(f32(header, 192))
        || f32(header, 192) <= 0 || f32(header, 192) > 1000000)
        return false;
    for (quint32 i = 0; i < h[26]; ++i) if (h[27 + i] < 1 || h[27 + i] > 16) return false;
    for (quint32 i = 0; i < h[31]; ++i) if (h[32 + i] >= h[10]) return false;
    const quint32 manifestIndex = 13 + 27 * h[10] + 4 * h[31];
    if (manifestIndex >= h[1]) return false;
    const qint64 directoryOffset = 196 + qint64(h[2]) * 4;
    const qint64 directoryEnd = directoryOffset + qint64(h[1]) * 44;
    if (directoryEnd >= file.size() || !file.seek(directoryOffset)) return false;
    const QByteArray directory = file.read(qint64(h[1]) * 44);
    if (directory.size() != qint64(h[1]) * 44) return false;
    quint64 previousEnd = quint64(directoryEnd);
    quint64 manifestOffset = 0;
    quint64 tokenizerOffset = 0, tokenizerBytes = 0;
    for (quint32 i = 0; i < h[1]; ++i) {
        const qsizetype at = qsizetype(i) * 44;
        const auto type = quint8(directory[at]), dimensions = quint8(directory[at + 1]);
        const quint64 offset = u64(directory, at + 20), bytes = u64(directory, at + 28);
        const quint32 group = u32(directory, at + 36), bits = u32(directory, at + 40);
        if (type < 1 || type > 4 || dimensions > 4 || offset < previousEnd || offset % 64
            || offset > quint64(file.size()) || !bytes || bytes > quint64(file.size()) - offset)
            return false;
        previousEnd = offset + bytes;
        quint64 elements = 1;
        for (quint8 d = 0; d < dimensions; ++d) {
            const quint32 length = u32(directory, at + 4 + d * 4);
            if (!length || length > 1048576 || elements > quint64(MaximumModelSize) / length) return false;
            elements *= length;
        }
        if ((type == 1 && (!dimensions || bytes != elements * 2))
            || (type == 2 && (!dimensions || bytes != elements * 4))) return false;
        if (type == 3) {
            if (dimensions != 2 || group != 128 || bits < 1 || bits > 5) return false;
            const quint64 rows = u32(directory, at + 4), columns = u32(directory, at + 8);
            const quint64 storedBits = bits == 5 ? 2 : bits;
            const quint64 expected = rows * ((columns + 127) / 128) * (16 * storedBits + 2);
            if (bytes != expected) return false;
        }
        if (type == 4) {
            if (dimensions || i != h[1] - 1 || bytes < 24 || bytes > 4 * 1024 * 1024) return false;
            tokenizerOffset = offset; tokenizerBytes = bytes;
        }
        if (i == manifestIndex) {
            if (type != 2 || dimensions != 1 || u32(directory, at + 4) != 13 || bytes != 52) return false;
            manifestOffset = offset;
        }
    }
    if (!manifestOffset || !tokenizerOffset || !file.seek(qint64(manifestOffset))) return false;
    const QByteArray manifest = file.read(52);
    if (manifest.size() != 52) return false;
    std::array<quint32, 13> audio{};
    for (qsizetype i = 0; i < qsizetype(audio.size()); ++i) {
        const float value = f32(manifest, i * 4);
        if (!std::isfinite(value) || value < 0 || value > 65536 || std::floor(value) != value) return false;
        audio[size_t(i)] = quint32(value);
    }
    if (audio[0] != 3246 || audio[1] != 1 || audio[2] < 8192 || audio[2] >= h[5]
        || audio[3] < 1 || audio[3] > 32 || audio[4] != 4 || audio[5] != 9
        || audio[6] != 128 || audio[7] != 3 || audio[8] != 80 || audio[9] != 16000
        || audio[10] != 512 || audio[11] != 400 || audio[12] != 160
        || h[1] != manifestIndex + 20 + 10 * h[10] + 43 * audio[3]) return false;
    if (!file.seek(qint64(tokenizerOffset))) return false;
    const QByteArray tokenizer = file.read(qint64(tokenizerBytes));
    if (tokenizer.size() != qint64(tokenizerBytes) || u32(tokenizer, 0) != h[5]) return false;
    qsizetype pos = 24;
    for (quint32 i = 0; i < h[5]; ++i) {
        if (pos > tokenizer.size() - 7 || quint8(tokenizer[pos + 4]) > 4) return false;
        const quint16 length = qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(tokenizer.constData() + pos + 5));
        pos += 7;
        if (length > tokenizer.size() - pos) return false;
        pos += length;
    }
    if (pos != tokenizer.size()) return false;
    result.engine = QStringLiteral("whistle");
    result.englishOnly = false;
    return true;
}

bool inspectModel(const QString &path, HeaderInfo &result, QString &error) {
    if (!safeFile(path)) { error = QStringLiteral("Choose a readable model file, not a symbolic link or directory."); return false; }
    QFile file(path);
    if (file.size() < MinimumModelSize || file.size() > MaximumModelSize) {
        error = QStringLiteral("The model must be a complete file between 1 MB and 8 GiB."); return false;
    }
    if (!file.open(QIODevice::ReadOnly)) { error = QStringLiteral("The model file could not be opened."); return false; }
    const QString extension = QFileInfo(path).suffix().toLower();
    const bool valid = (extension == QStringLiteral("bin") && inspectWhisper(file, result))
        || (extension == QStringLiteral("cact") && inspectWhistle(file, result));
    if (!valid) error = QStringLiteral("This file is not a supported Whisper GGML .bin or Whistle speech .cact model. The header, architecture, or file contents are incomplete or incompatible.");
    return valid;
}

ModelInfo readInstalled(const QString &root, const QString &id) {
    ModelInfo result;
    if (!customId(id) || !safeDirectory(root)) return result;
    const QString directory = QDir(root).filePath(id);
    if (!safeDirectory(directory)) return result;
    const QString metadata = QDir(directory).filePath(QStringLiteral("metadata.json"));
    if (!safeFile(metadata)) return result;
    QFile file(metadata);
    if (file.size() > MetadataLimit || !file.open(QIODevice::ReadOnly)) return result;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) return result;
    const QJsonObject data = document.object();
    const QString hash = id.mid(7), engine = data.value(QStringLiteral("engine")).toString();
    const qint64 bytes = data.value(QStringLiteral("size")).toInteger(-1);
    const QString name = data.value(QStringLiteral("name")).toString();
    if (data.value(QStringLiteral("schema")).toInt(-1) != 1
        || data.value(QStringLiteral("id")).toString() != id
        || data.value(QStringLiteral("sha256")).toString() != hash
        || (engine != QStringLiteral("whisper") && engine != QStringLiteral("whistle"))
        || bytes < MinimumModelSize || bytes > MaximumModelSize || name.isEmpty()
        || name != displayName(name, {})) return result;
    const QString path = QDir(directory).filePath(modelFilename(engine));
    if (!safeFile(path) || QFileInfo(path).size() != bytes) return result;
    HeaderInfo header;
    QString error;
    if (!inspectModel(path, header, error) || header.engine != engine) return result;
    result.id = id; result.name = name; result.engine = engine; result.path = path;
    result.sha256 = hash; result.size = bytes; result.englishOnly = header.englishOnly;
    result.description = description(result);
    return result;
}

bool writeSilence(const QString &path) {
    constexpr quint32 bytes = 16000 * 2;
    QByteArray wav(44 + bytes, '\0');
    std::memcpy(wav.data(), "RIFF", 4); std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
    std::memcpy(wav.data() + 36, "data", 4);
    auto p = reinterpret_cast<uchar *>(wav.data());
    qToLittleEndian<quint32>(bytes + 36, p + 4); qToLittleEndian<quint32>(16, p + 16);
    qToLittleEndian<quint16>(1, p + 20); qToLittleEndian<quint16>(1, p + 22);
    qToLittleEndian<quint32>(16000, p + 24); qToLittleEndian<quint32>(32000, p + 28);
    qToLittleEndian<quint16>(2, p + 32); qToLittleEndian<quint16>(16, p + 34);
    qToLittleEndian<quint32>(bytes, p + 40);
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(wav) == wav.size() && file.flush();
}

bool probeModel(const QString &root, const QString &path, const HeaderInfo &header,
                const QString &workDirectory, const Cancel &cancel, QString &error) {
    QTemporaryDir temporary(QDir(workDirectory).filePath(QStringLiteral(".probe-XXXXXX")));
    if (!temporary.isValid()) { error = QStringLiteral("A temporary model-check directory could not be created."); return false; }
    const QString wav = temporary.filePath(QStringLiteral("silence.wav"));
    if (!writeSilence(wav)) { error = QStringLiteral("The model-check audio could not be created."); return false; }
    QProcess process;
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    process.setProgram(enginePath(root, header.engine));
    const int threads = qBound(1, QThread::idealThreadCount(), 4);
    if (header.engine == QStringLiteral("whisper")) {
        process.setArguments({QStringLiteral("-m"), path, QStringLiteral("-f"), wav,
            QStringLiteral("-l"), QStringLiteral("en"), QStringLiteral("-oj"), QStringLiteral("-of"),
            temporary.filePath(QStringLiteral("check")), QStringLiteral("-t"), QString::number(threads),
            QStringLiteral("--no-gpu")});
    } else {
        process.setArguments({QStringLiteral("--model"), path, QStringLiteral("--audio"), wav,
            QStringLiteral("--audio-word-timestamps"), QStringLiteral("--threads"), QString::number(threads)});
    }
    process.setWorkingDirectory(temporary.path());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("NEEDLE_TELEMETRY"), QStringLiteral("0"));
    env.insert(QStringLiteral("DO_NOT_TRACK"), QStringLiteral("1"));
    env.insert(QStringLiteral("HF_HUB_OFFLINE"), QStringLiteral("1"));
    env.insert(QStringLiteral("OMP_NUM_THREADS"), QString::number(threads));
#ifndef Q_OS_WIN
    QString libraries = QDir(root).filePath(QStringLiteral("lib")) + QLatin1Char(':')
        + QDir(root).filePath(QStringLiteral("bin")) + QLatin1Char(':')
        + QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("lib"));
    if (!env.value(QStringLiteral("LD_LIBRARY_PATH")).isEmpty()) libraries += QLatin1Char(':') + env.value(QStringLiteral("LD_LIBRARY_PATH"));
    env.insert(QStringLiteral("LD_LIBRARY_PATH"), libraries);
#endif
    process.setProcessEnvironment(env);
    QElapsedTimer elapsed; elapsed.start();
    process.start();
    if (!process.waitForStarted(5000)) { error = QStringLiteral("The bundled recognition engine could not start its model check."); return false; }
    QByteArray output;
    qsizetype totalOutput = 0;
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(100);
        const QByteArray out = process.readAllStandardOutput(), err = process.readAllStandardError();
        totalOutput += out.size() + err.size();
        if (totalOutput <= ProbeOutputLimit) output += out;
        if (cancel->load() || elapsed.elapsed() >= ProbeTimeoutMs || totalOutput > ProbeOutputLimit
            || QFileInfo(temporary.filePath(QStringLiteral("check.json"))).size() > ProbeOutputLimit) {
            process.kill(); process.waitForFinished(5000);
            if (!cancel->load()) error = elapsed.elapsed() >= ProbeTimeoutMs
                ? QStringLiteral("The model check exceeded five minutes. Try a smaller model that this CPU can load and run.")
                : QStringLiteral("The model check produced too much output.");
            return false;
        }
    }
    if (cancel->load()) return false;
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        error = QStringLiteral("The bundled recognition engine could not load and run this model. Choose a compatible model for this application version.");
        return false;
    }
    QJsonDocument transcript;
    if (header.engine == QStringLiteral("whisper")) {
        QFile json(temporary.filePath(QStringLiteral("check.json")));
        if (json.size() <= ProbeOutputLimit && json.open(QIODevice::ReadOnly)) transcript = QJsonDocument::fromJson(json.readAll());
        if (transcript.isObject() && transcript.object().value(QStringLiteral("transcription")).isArray()) return true;
    } else {
        const qsizetype first = output.indexOf('{'), last = output.lastIndexOf('}');
        if (first >= 0 && last >= first) transcript = QJsonDocument::fromJson(output.mid(first, last - first + 1));
        if (transcript.isObject() && transcript.object().value(QStringLiteral("text")).isString()) return true;
    }
    error = QStringLiteral("The model check did not return a valid recognition result.");
    return false;
}

ImportResult installModel(const QString &root, const QString &directory, const QString &source,
                          const QString &name, const Cancel &cancel, const Progress &progress) {
    ImportResult result;
    const auto cancelled = [&] { result.cancelled = cancel->load(); return result.cancelled; };
    if (cancelled()) return result;
    if (!safeDirectory(directory)) { result.error = QStringLiteral("The model storage directory is missing, unreadable, or contains a symbolic link."); return result; }
    HeaderInfo header;
    progress(0, QStringLiteral("Checking the model format…"));
    if (!inspectModel(source, header, result.error)) return result;
    result.error = engineError(root, header.engine);
    if (!result.error.isEmpty() || cancelled()) return result;
    QTemporaryDir temporary(QDir(directory).filePath(QStringLiteral(".import-XXXXXX")));
    if (!temporary.isValid()) { result.error = QStringLiteral("A private temporary model directory could not be created."); return result; }
    QFile::setPermissions(temporary.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const QString destination = temporary.filePath(modelFilename(header.engine));
    QFile input(source), output(destination);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        result.error = QStringLiteral("The model could not be copied into the model storage directory."); return result;
    }
    output.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    const qint64 expected = input.size();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 copied = 0;
    int lastProgress = -1;
    while (!input.atEnd()) {
        if (cancelled()) return result;
        const QByteArray block = input.read(1024 * 1024);
        if (block.isEmpty() || copied > MaximumModelSize - block.size() || output.write(block) != block.size()) {
            result.error = QStringLiteral("Copying the model failed. Check the source file and the free space in model storage."); return result;
        }
        hash.addData(block); copied += block.size();
        const int percent = int(qMin<qint64>(80, copied * 80 / qMax<qint64>(1, expected)));
        if (percent != lastProgress) { progress(percent, QStringLiteral("Copying model and calculating SHA-256…")); lastProgress = percent; }
    }
    if (input.error() != QFileDevice::NoError || copied != expected || !output.flush()) {
        result.error = QStringLiteral("The source model changed or could not be copied completely."); return result;
    }
    input.close(); output.close();
    if (cancelled()) return result;
    const QString digest = QString::fromLatin1(hash.result().toHex());
    const QString id = QStringLiteral("custom-") + digest;
    progress(82, QStringLiteral("Validating the copied model…"));
    HeaderInfo copiedHeader;
    if (!inspectModel(destination, copiedHeader, result.error) || copiedHeader.engine != header.engine) return result;
    const auto existing = readInstalled(directory, id);
    if (!existing.id.isEmpty()) { result.id = id; result.duplicate = true; return result; }
    progress(85, QStringLiteral("Testing the model with the bundled engine…"));
    if (!probeModel(root, destination, copiedHeader, temporary.path(), cancel, result.error)) {
        cancelled(); return result;
    }
    if (cancelled()) return result;
    if (!safeDirectory(directory) || !safeDirectory(temporary.path())) {
        result.error = QStringLiteral("The model storage directory changed during import."); return result;
    }
    const QString lockPath = QDir(directory).filePath(QStringLiteral(".catalog.lock"));
    if (isLink(QFileInfo(lockPath))) { result.error = QStringLiteral("The model storage lock is not a regular file."); return result; }
    QLockFile lock(lockPath);
    if (!lock.tryLock(0)) { result.error = QStringLiteral("Another application is changing model storage. Try the import again."); return result; }
    if (!readInstalled(directory, id).id.isEmpty()) { result.id = id; result.duplicate = true; return result; }
    const QString finalPath = QDir(directory).filePath(id);
    if (QFileInfo::exists(finalPath) || isLink(QFileInfo(finalPath))) {
        result.error = QStringLiteral("An invalid existing entry blocks this model's installation. Remove that entry from model storage and import again."); return result;
    }
    const QString fallback = copiedHeader.engine == QStringLiteral("whisper") ? QStringLiteral("Whisper model") : QStringLiteral("Whistle model");
    const QJsonObject metadata{{QStringLiteral("schema"), 1}, {QStringLiteral("id"), id},
        {QStringLiteral("name"), displayName(name, fallback)}, {QStringLiteral("engine"), copiedHeader.engine},
        {QStringLiteral("sha256"), digest}, {QStringLiteral("englishOnly"), copiedHeader.englishOnly},
        {QStringLiteral("size"), copied}};
    QSaveFile metadataFile(temporary.filePath(QStringLiteral("metadata.json")));
    const QByteArray json = QJsonDocument(metadata).toJson(QJsonDocument::Indented);
    if (!metadataFile.open(QIODevice::WriteOnly) || metadataFile.write(json) != json.size() || !metadataFile.commit()) {
        result.error = QStringLiteral("The installed model's metadata could not be saved."); return result;
    }
    QFile::setPermissions(temporary.filePath(QStringLiteral("metadata.json")), QFile::ReadOwner | QFile::WriteOwner);
    if (cancelled()) return result;
    if (!QDir().rename(temporary.path(), finalPath)) {
        result.error = QStringLiteral("The checked model could not be installed atomically."); return result;
    }
    temporary.setAutoRemove(false);
    result.id = id;
    progress(100, QStringLiteral("Model installed"));
    return result;
}
}

ModelCatalog::ModelCatalog(QString resourceRoot, QString modelsDirectory, QObject *parent)
    : QObject(parent), m_resourceRoot(QDir(resourceRoot).absolutePath()),
      m_directory(QDir::cleanPath(QFileInfo(modelsDirectory).absoluteFilePath())) {
    if (modelsDirectory.trimmed().isEmpty() || !safeDirectory(m_directory, true))
        m_storageError = QStringLiteral("The model storage directory could not be created, or one of its folders is a symbolic link.");
    else QFile::setPermissions(m_directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    reload();
}

ModelCatalog::~ModelCatalog() {
    if (m_cancel) m_cancel->store(true);
    if (m_worker) {
        m_worker->disconnect(this);
        m_worker->wait();
        delete m_worker;
    }
}

void ModelCatalog::reload() {
    m_models.clear();
    const QStringList ids{QStringLiteral("whistle"), QStringLiteral("tiny"), QStringLiteral("base"), QStringLiteral("small")};
    const QStringList names{QStringLiteral("Whistle"), QStringLiteral("Whisper Tiny"), QStringLiteral("Whisper Base"), QStringLiteral("Whisper Small")};
    for (qsizetype i = 0; i < ids.size(); ++i) {
        ModelInfo info;
        info.id = ids[i]; info.name = names[i]; info.bundled = true;
        info.engine = i == 0 ? QStringLiteral("whistle") : QStringLiteral("whisper");
        info.path = QDir(m_resourceRoot).filePath(QStringLiteral("models/")
            + (i == 0 ? QStringLiteral("whistle.cact") : QStringLiteral("ggml-%1-q5_1.bin").arg(ids[i])));
        info.size = QFileInfo(info.path).size(); info.description = description(info);
        m_models.append(info);
    }
    if (!m_storageError.isEmpty() || !safeDirectory(m_directory)) return;
    const auto entries = QDir(m_directory).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
    QVector<ModelInfo> imported;
    for (const auto &entry : entries) {
        if (!customId(entry.fileName()) || isLink(entry)) continue;
        ModelInfo info = readInstalled(m_directory, entry.fileName());
        if (!info.id.isEmpty()) imported.append(info);
    }
    std::sort(imported.begin(), imported.end(), [](const ModelInfo &a, const ModelInfo &b) {
        const int compared = QString::localeAwareCompare(a.name, b.name);
        return compared == 0 ? a.id < b.id : compared < 0;
    });
    m_models += imported;
}

QVector<ModelInfo> ModelCatalog::models() const { return m_models; }

ModelInfo ModelCatalog::model(QString id) const {
    for (const auto &info : m_models) if (info.id == id) return info;
    return {};
}

QString ModelCatalog::availability(QString id) const {
    const ModelInfo info = model(id);
    if (info.id.isEmpty()) return QStringLiteral("This model is not installed. Import it again or choose an available model.");
    if (!info.bundled && readInstalled(m_directory, id).id.isEmpty())
        return QStringLiteral("The installed model is missing, changed, or unreadable. Import a complete compatible model again.");
    const QFileInfo file(info.path);
    if (!file.isFile() || !file.isReadable() || file.size() < MinimumModelSize)
        return QStringLiteral("The %1 model is missing or unreadable.").arg(info.name);
    return engineError(m_resourceRoot, info.engine);
}

void ModelCatalog::importModel(QString sourcePath, QString name) {
    // A second request does not terminate the import already in progress.
    if (m_busy) return;
    if (!m_storageError.isEmpty()) { emit importFailed(m_storageError); return; }
    sourcePath = QFileInfo(sourcePath).absoluteFilePath();
    if (name.isEmpty()) name = QFileInfo(sourcePath).completeBaseName();
    m_cancel = std::make_shared<std::atomic_bool>(false);
    auto result = std::make_shared<ImportResult>();
    const auto progress = [this](int percent, const QString &status) {
        QMetaObject::invokeMethod(this, [this, percent, status] { emit importProgress(percent, status); }, Qt::QueuedConnection);
    };
    const QString root = m_resourceRoot, directory = m_directory;
    const Cancel cancel = m_cancel;
    m_worker = QThread::create([root, directory, sourcePath, name, cancel, progress, result] {
        *result = installModel(root, directory, sourcePath, name, cancel, progress);
    });
    m_worker->setParent(this);
    connect(m_worker, &QThread::finished, this, [this, result] {
        QThread *finished = m_worker; m_worker = nullptr;
        m_cancel.reset(); m_busy = false;
        finished->deleteLater();
        emit busyChanged(false);
        if (!result->id.isEmpty()) {
            reload(); emit modelsChanged(); emit modelImported(result->id, result->duplicate);
        } else if (result->cancelled) emit importCancelled();
        else emit importFailed(result->error.isEmpty() ? QStringLiteral("The model could not be installed.") : result->error);
    });
    m_busy = true;
    emit busyChanged(true);
    m_worker->start();
}

void ModelCatalog::cancelImport() {
    if (m_busy && m_cancel) m_cancel->store(true);
}

bool ModelCatalog::removeModel(QString id, QString *error) {
    const auto fail = [error](const QString &message) { if (error) *error = message; return false; };
    if (error) error->clear();
    const ModelInfo info = model(id);
    if (info.id.isEmpty() || !customId(id) || info.bundled)
        return fail(QStringLiteral("Only installed custom models can be removed. Bundled models are protected."));
    if (inUse(id)) return fail(QStringLiteral("This model is being used for transcription. Wait for recognition to finish before removing it."));
    if (!safeDirectory(m_directory)) return fail(QStringLiteral("The model storage directory is no longer safe to use."));
    const QString directory = QDir(m_directory).filePath(id);
    if (!safeDirectory(directory)) return fail(QStringLiteral("The installed model directory is missing or is a symbolic link."));
    const QString lockPath = QDir(m_directory).filePath(QStringLiteral(".catalog.lock"));
    if (isLink(QFileInfo(lockPath))) return fail(QStringLiteral("The model storage lock is not a regular file."));
    QLockFile lock(lockPath);
    if (!lock.tryLock(0)) return fail(QStringLiteral("Another application is changing model storage. Try again shortly."));
    const QString leasePath = QDir(m_directory).filePath(QStringLiteral(".in-use-") + id + QStringLiteral(".lock"));
    if (isLink(QFileInfo(leasePath))) return fail(QStringLiteral("The model's usage lock is not a regular file."));
    QLockFile lease(leasePath);
    lease.setStaleLockTime(0);
    if (!lease.tryLock(0)) return fail(QStringLiteral("This model is being used by another application instance. Wait for recognition to finish before removing it."));
    const QStringList expected{QStringLiteral("metadata.json"), modelFilename(info.engine)};
    const auto contents = QDir(directory).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const auto &file : contents) {
        if (isLink(file) || !file.isFile() || !expected.contains(file.fileName()))
            return fail(QStringLiteral("The installed model directory contains unexpected files. It was left unchanged."));
    }
    const QString removed = QDir(m_directory).filePath(QStringLiteral(".removed-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().rename(directory, removed)) return fail(QStringLiteral("The installed model could not be removed. Close any other application using it and try again."));
    // Delete only the two generated filenames. Never follow metadata paths or
    // recursively remove an unexpected user directory.
    const QString weights = QDir(removed).filePath(modelFilename(info.engine));
    if (QFileInfo::exists(weights) && !QFile::remove(weights)) {
        const bool restored = QDir().rename(removed, directory);
        if (!restored) { reload(); emit modelsChanged(); }
        return fail(restored ? QStringLiteral("The model file is still in use or cannot be deleted. Its installation was kept unchanged.")
            : QStringLiteral("The model file could not be deleted or restored. It remains in a hidden removal folder in model storage."));
    }
    const QString metadata = QDir(removed).filePath(QStringLiteral("metadata.json"));
    const bool metadataRemoved = !QFileInfo::exists(metadata) || QFile::remove(metadata);
    const bool cleaned = metadataRemoved && QDir().rmdir(removed);
    reload(); emit modelsChanged();
    if (!cleaned) return fail(QStringLiteral("The model weights were removed, but the temporary metadata folder could not be deleted."));
    return true;
}

bool ModelCatalog::acquireModel(QString id, ModelInfo *info, QString *error) {
    const QString problem = availability(id);
    if (!problem.isEmpty()) { if (error) *error = problem; return false; }
    if (customId(id) && m_users.value(id) == 0) {
        const auto fail = [error](const QString &message) { if (error) *error = message; return false; };
        if (!safeDirectory(m_directory)) return fail(QStringLiteral("The model storage directory is no longer safe to use."));
        const QString lockPath = QDir(m_directory).filePath(QStringLiteral(".catalog.lock"));
        if (isLink(QFileInfo(lockPath))) return fail(QStringLiteral("The model storage lock is not a regular file."));
        QLockFile storageLock(lockPath);
        if (!storageLock.tryLock(0)) return fail(QStringLiteral("Another application is changing model storage. Try again shortly."));
        const QString leasePath = QDir(m_directory).filePath(QStringLiteral(".in-use-") + id + QStringLiteral(".lock"));
        if (isLink(QFileInfo(leasePath))) return fail(QStringLiteral("The model's usage lock is not a regular file."));
        auto lease = std::make_shared<QLockFile>(leasePath);
        lease->setStaleLockTime(0);
        if (!lease->tryLock(0)) return fail(QStringLiteral("This imported model is already being used by another application instance. Wait for that recognition to finish."));
        if (readInstalled(m_directory, id).id.isEmpty()) return fail(QStringLiteral("The imported model is no longer installed."));
        m_leases.insert(id, std::move(lease));
    }
    ++m_users[id];
    if (info) *info = model(id);
    if (error) error->clear();
    return true;
}

void ModelCatalog::releaseModel(QString id) {
    const auto found = m_users.find(id);
    if (found == m_users.end()) return;
    if (*found <= 1) { m_users.erase(found); m_leases.remove(id); }
    else --*found;
}

bool ModelCatalog::inUse(QString id) const {
    if (m_users.value(id) > 0) return true;
    if (!customId(id) || !safeDirectory(m_directory)) return false;
    const QString path = QDir(m_directory).filePath(QStringLiteral(".in-use-") + id + QStringLiteral(".lock"));
    if (isLink(QFileInfo(path))) return true;
    if (!QFileInfo::exists(path)) return false;
    QLockFile lease(path);
    lease.setStaleLockTime(0);
    return !lease.tryLock(0);
}
