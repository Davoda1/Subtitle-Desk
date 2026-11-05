#include "audio_session.h"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>
#include <QtEndian>
#include <cmath>

namespace {
int checks=0,failures=0;
void check(bool ok,const QString &label){++checks;QTextStream(stdout)<<(ok?"PASS ":"FAIL ")<<label<<Qt::endl;if(!ok)++failures;}
struct Outcome{bool ready=false,failed=false,cancelled=false,timedOut=false;QString message;};
Outcome load(AudioSession &session,const QString &path){
    QEventLoop loop;Outcome out;QTimer timeout;timeout.setSingleShot(true);
    QObject::connect(&session,&AudioSession::ready,&loop,[&](){out.ready=true;loop.quit();});
    QObject::connect(&session,&AudioSession::failed,&loop,[&](const QString &e){out.failed=true;out.message=e;loop.quit();});
    QObject::connect(&session,&AudioSession::cancelled,&loop,[&](){out.cancelled=true;loop.quit();});
    QObject::connect(&timeout,&QTimer::timeout,&loop,[&](){out.timedOut=true;session.cancel();loop.quit();});
    timeout.start(20000);QTimer::singleShot(0,&loop,[&](){session.open(path);});loop.exec();return out;
}
QVector<qint16> samples(const QString &path){
    QFile f(path);if(!f.open(QIODevice::ReadOnly))return {};
    const QByteArray data=f.readAll();if(data.size()<44)return {};
    check(data.left(4)=="RIFF"&&data.mid(8,4)=="WAVE"&&qFromLittleEndian<quint32>(data.constData()+40)==quint32(data.size()-44),"PCM WAV header and length");
    QVector<qint16> result((data.size()-44)/2);
    for(qsizetype i=0;i<result.size();++i)result[i]=qFromLittleEndian<qint16>(data.constData()+44+i*2);
    return result;
}
double rms(const QVector<qint16>& pcm,int startMs,int endMs){
    const qsizetype first=std::min(qsizetype(startMs)*16,pcm.size()),last=std::min(qsizetype(endMs)*16,pcm.size());
    if(last<=first)return 0;double sum=0;for(qsizetype i=first;i<last;++i)sum+=double(pcm[i])*pcm[i];return std::sqrt(sum/double(last-first))/32768.;
}
void pump(int ms){QEventLoop loop;QTimer::singleShot(ms,&loop,&QEventLoop::quit);loop.exec();}
}

int main(int argc,char **argv){
    QCoreApplication app(argc,argv);
    if(argc!=2){QTextStream(stderr)<<"Usage: audio_test <fixture-directory>\n";return 2;}
    QDir fixtures(QString::fromLocal8Bit(argv[1]));AudioSession session;
    QFile expectationFile(fixtures.filePath("expectations.json"));
    if(!expectationFile.open(QIODevice::ReadOnly)){QTextStream(stderr)<<"Generate fixtures with create_audio_fixtures.py first.\n";return 2;}
    const auto expectations=QJsonDocument::fromJson(expectationFile.readAll()).object();
    for(const auto &name:{"tone.wav","stereo.wav","tone.mp3","tone.mp4","tone.mkv","tone.flac","tone.ogg"}){
        const auto out=load(session,fixtures.filePath(name));check(out.ready&&!out.timedOut,QString("Decode ")+name+" "+out.message);
        if(!out.ready)continue;
        check(!session.busy(),QString("Idle after ")+name);
        check(std::abs(session.durationMs()-2000)<=120,QString("Duration ")+name);
        const auto pcm=samples(session.wavPath());check(rms(pcm,200,1800)>.1,QString("Audible PCM ")+name);
        check(session.peaks().size()==(pcm.size()+319)/320,QString("Waveform buckets ")+name);
    }
    const auto unicode=load(session,fixtures.filePath(QString::fromUtf8("Árvíztűrő tükörfúrógép 日本語.wav")));
    check(unicode.ready,"Unicode and spaces in media path");
    for(const auto &name:{"delayed.mp4","shifted.mp4"}){
        const auto out=load(session,fixtures.filePath(name));check(out.ready,QString("Decode timing fixture ")+name+" "+out.message);
        if(!out.ready)continue;
        const auto pcm=samples(session.wavPath());
        const qint64 expected=expectations.value(name).toObject().value("duration_ms").toInteger();
        check(expected>0&&std::abs(session.durationMs()-expected)<=5,QString("Normalized video timeline ")+name);
        check(rms(pcm,0,2900)<.0001,QString("Leading silence preserved ")+name);
        check(rms(pcm,3200,4800)>.1,QString("Audio aligns after 3s ")+name);
    }
    const auto gap=load(session,fixtures.filePath("gap.mkv"));check(gap.ready,"Decode internal timestamp gap");
    if(gap.ready){
        const auto pcm=samples(session.wavPath());check(std::abs(session.durationMs()-3000)<=3,"Internal gap retained in duration");
        check(rms(pcm,1100,1900)<.0001,"Internal gap rendered as silence");
        check(rms(pcm,2200,2800)>.1,"Audio resumes on original timeline");
    }
    const auto tracks=load(session,fixtures.filePath("two-tracks.mkv"));check(tracks.ready,"Decode multitrack MKV");
    if(tracks.ready){
        const auto pcm=samples(session.wavPath());int crossings=0;for(int i=3201;i<28800&&i<pcm.size();++i)if(pcm[i-1]<=0&&pcm[i]>0)++crossings;
        check(std::abs(crossings-704)<=2,"First audio track selected regardless of default flag");
    }
    const QString previousWav=session.wavPath(),previousSource=session.sourcePath();
    for(const auto &name:{"corrupt.mp4","video-only.mp4","remote.m3u8","too-long.mkv","not-found.wav"}){
        const auto out=load(session,fixtures.filePath(name));check(out.failed&&!out.timedOut,QString("Reject ")+name+" "+out.message);
        check(session.wavPath()==previousWav&&session.sourcePath()==previousSource&&QFileInfo::exists(previousWav),QString("Preserve session after ")+name);
        check(!session.busy(),QString("Idle after rejected ")+name);
        if(QString(name)=="too-long.mkv")check(out.message.contains("37 hours"),"Long timestamp failure is actionable");
    }
    {
        int ready=0,cancelled=0;QObject scope;
        QObject::connect(&session,&AudioSession::ready,&scope,[&](){++ready;});
        QObject::connect(&session,&AudioSession::cancelled,&scope,[&](){++cancelled;});
        session.open(fixtures.filePath("delayed.mp4"));check(session.busy(),"Import busy immediately");session.cancel();pump(200);
        check(cancelled==1&&ready==0&&!session.busy(),"Cancellation does not publish a late result");
        check(session.wavPath()==previousWav&&QFileInfo::exists(previousWav),"Cancellation preserves previous audio");
    }
    {
        int ready=0;QObject scope;
        QObject::connect(&session,&AudioSession::ready,&scope,[&](){++ready;});
        session.open(fixtures.filePath("delayed.mp4"));
        const auto out=load(session,fixtures.filePath("tone.wav"));pump(200);
        check(out.ready&&ready==1&&session.sourcePath()==QFileInfo(fixtures.filePath("tone.wav")).absoluteFilePath(),"Replacing an active import ignores stale completion");
    }
    {
        auto *dying=new AudioSession;dying->open(fixtures.filePath("delayed.mp4"));delete dying;pump(200);
        check(true,"Destroying an active decoder is safe");
    }
    QTextStream(stdout)<<"Audio tests: "<<checks-failures<<"/"<<checks<<" passed"<<Qt::endl;
    return failures?1:0;
}
