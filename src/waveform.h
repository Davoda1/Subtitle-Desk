#pragma once
#include <QWidget>
#include <QVector>

class Waveform final : public QWidget {
    Q_OBJECT
public:
    explicit Waveform(QWidget *parent=nullptr);
    void setAudio(const QVector<float> &peaks,qint64 duration);
    void setPosition(qint64 position);
    void setCue(qint64 start,qint64 end);
    void setZoom(int seconds); // 0 = entire track
    void setDark(bool dark);
signals:
    void seekRequested(qint64 position);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
private:
    void seek(QMouseEvent*);
    qint64 viewStart() const;
    qint64 viewDuration() const;
    QVector<float> m_peaks;
    qint64 m_duration=0,m_position=0,m_start=-1,m_end=-1;
    int m_zoom=0;
    bool m_dark=true;
};
