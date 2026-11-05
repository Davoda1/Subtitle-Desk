#pragma once
#include "subtitle_document.h"
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

// Runs the bundled native recognizers with local argument arrays. Input is a
// decoded mono, 16 kHz, signed 16-bit PCM WAV. Workers never use server mode.
class TranscriptionService : public QObject {
    Q_OBJECT
public:
    explicit TranscriptionService(QObject *parent = nullptr);
    ~TranscriptionService() override;
    void setResourceRoot(const QString &path);
    QString resourceRoot() const;
    bool busy() const { return m_busy; }
    // Empty means that the requested bundled model and native engine exist.
    QString availability(const QString &modelId) const;
    void start(const QString &wavPath, const QString &modelId, const QString &language,
               int threads, int maxChars = 42);
    void cancel();
signals:
    void progress(int percent, QString status);
    void completed(QVector<Cue> cues, QString timingNote);
    void failed(QString message);
    void cancelled();
private:
    struct WavInfo { qint64 dataOffset = 0; qint64 bytes = 0; qint64 durationMs = 0; };
    static bool inspectWav(const QString &, WavInfo &, QString &);
    QString enginePath(const QString &) const;
    QString modelPath() const;
    void startWorker(const QString &, const QStringList &);
    void runWhisper();
    void runWhistleChunk();
    bool writeWhistleChunk(QString &, QString &);
    bool parseWhisper(const QByteArray &, QString &);
    bool parseWhistle(const QByteArray &, QString &);
    void collectOutput();
    void finishWorker(int, QProcess::ExitStatus);
    void abortWithError(const QString &);
    void finishSuccess();
    void reset();
    QString m_root, m_wav, m_model, m_language;
    int m_threads = 4, m_maxChars = 42;
    bool m_busy = false, m_cancelling = false, m_approximate = false;
    quint64 m_generation = 0;
    QProcess *m_process = nullptr;
    QTimer m_timeout;
    std::unique_ptr<QTemporaryDir> m_temp;
    QByteArray m_stdout, m_stderr;
    WavInfo m_info;
    qint64 m_coreStart = 0, m_coreEnd = 0, m_chunkStart = 0, m_chunkEnd = 0;
    QVector<Cue> m_cues;
};
