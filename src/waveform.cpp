#include "waveform.h"
#include "subtitle_document.h"
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

Waveform::Waveform(QWidget *p):QWidget(p){setMinimumHeight(72);setMaximumHeight(124);setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);setCursor(Qt::PointingHandCursor);setAccessibleName("Audio waveform. Click to seek.");}
void Waveform::setAudio(const QVector<float>&p,qint64 d){m_peaks=p;m_duration=d;m_position=0;update();}
void Waveform::setPosition(qint64 p){m_position=p;update();}
void Waveform::setCue(qint64 s,qint64 e){m_start=s;m_end=e;update();}
void Waveform::setZoom(int s){m_zoom=s;update();}
void Waveform::setDark(bool d){m_dark=d;update();}
qint64 Waveform::viewDuration()const{return m_zoom?std::min(m_duration,qint64(m_zoom)*1000):m_duration;}
qint64 Waveform::viewStart()const{const auto v=viewDuration();return std::clamp(m_position-v/2,qint64(0),std::max(qint64(0),m_duration-v));}
void Waveform::paintEvent(QPaintEvent*){
    QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.fillRect(rect(),m_dark?QColor("#17191d"):QColor("#f4f5f7"));
    const int w=width(),h=height(),bottom=h-21;const auto start=viewStart(),dur=viewDuration();
    if(!dur||m_peaks.isEmpty()){
        p.setPen(m_dark?QColor("#939aa6"):QColor("#626a78"));p.drawText(rect(),Qt::AlignCenter,"Open audio or video to see its waveform");return;
    }
    auto x=[=](qint64 t){return double(t-start)/dur*w;};
    if(m_start>=0){p.fillRect(QRectF(x(m_start),0,x(m_end)-x(m_start),bottom),m_dark?QColor(107,157,190,35):QColor(70,125,165,30));}
    p.setPen(QPen(m_dark?QColor("#8798aa"):QColor("#677d92"),1));
    for(int px=0;px<w;px+=2){
        const int first=std::clamp(int((start+qint64(px)*dur/w)/20),0,int(m_peaks.size()-1));
        const int last=std::clamp(int((start+qint64(px+2)*dur/w)/20)+1,first+1,int(m_peaks.size()));
        float peak=0;for(int i=first;i<last;++i)peak=std::max(peak,m_peaks[i]);
        const double a=std::max(1.0,std::sqrt(double(peak))*(bottom-6)/2);
        p.drawLine(QPointF(px,bottom/2.0-a),QPointF(px,bottom/2.0+a));
    }
    p.setPen(m_dark?QColor("#777f8e"):QColor("#747c87"));p.setFont(QFont(font().family(),8));
    const int steps=std::max(2,w/160);for(int i=0;i<=steps;++i){const int xx=i*(w-1)/steps;QString t=SubtitleDocument::formatTime(start+dur*i/steps).left(8);if(t.startsWith("00:"))t=t.mid(3);const int tw=p.fontMetrics().horizontalAdvance(t);p.drawText(std::clamp(xx-tw/2,3,w-tw-3),h-5,t);}
    p.setPen(QPen(m_dark?QColor("#d9e7f7"):QColor("#263e55"),2));p.drawLine(QPointF(x(m_position),0),QPointF(x(m_position),bottom));
    p.setBrush(m_dark?QColor("#d9e7f7"):QColor("#263e55"));p.drawEllipse(QPointF(x(m_position),5),3,3);
}
void Waveform::seek(QMouseEvent*e){if(m_duration>0)emit seekRequested(std::clamp(viewStart()+qint64(e->position().x()*viewDuration()/std::max(1,width())),qint64(0),m_duration));}
void Waveform::mousePressEvent(QMouseEvent*e){if(e->button()==Qt::LeftButton)seek(e);}
void Waveform::mouseMoveEvent(QMouseEvent*e){if(e->buttons()&Qt::LeftButton)seek(e);}
