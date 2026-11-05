#pragma once

#include <QObject>
#include <QTemporaryDir>
#include <QVector>
#include <atomic>
#include <memory>

class QTimer;
class QThread;

// Decode only the first audio stream. Video frames are never displayed.
// PCM is streamed to disk. The waveform retains one peak per 20 ms.
class AudioSession final : public QObject {
    Q_OBJECT
public:
    explicit AudioSession(QObject *parent = nullptr);
    ~AudioSession() override;
    void open(const QString &path);
    void cancel();
    bool busy() const { return m_loading; }
    QString wavPath() const { return m_wavPath; }
    QString sourcePath() const { return m_sourcePath; }
    qint64 durationMs() const { return m_duration; }
    const QVector<float> &peaks() const { return m_peaks; }
signals:
    void progress(int percent, QString message);
    void ready(QString source, QString wav, qint64 duration, QVector<float> peaks);
    void failed(QString message);
    void cancelled();
private:
    void stopPending();
    std::shared_ptr<QTemporaryDir> m_temp, m_pendingTemp;
    std::shared_ptr<std::atomic_bool> m_cancelFlag;
    QVector<std::shared_ptr<QTemporaryDir>> m_retiredTemps;
    QVector<QThread*> m_workers;
    QTimer *m_cleanupTimer = nullptr;
    QString m_sourcePath, m_wavPath;
    QVector<float> m_peaks;
    qint64 m_duration = 0;
    quint64 m_generation = 0;
    bool m_loading = false;
};
