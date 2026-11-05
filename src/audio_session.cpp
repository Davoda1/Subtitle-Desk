#include "audio_session.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QLibraryInfo>
#include <QThread>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <functional>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace {
constexpr qint64 SampleRate = 16000;
constexpr qint64 MaxFrames = (qint64(0xffffffff) - 36) / 2;
constexpr int PeakFrames = 320;

QByteArray wavHeader(quint32 bytes) {
    QByteArray h(44, '\0');
    std::copy_n("RIFF",4,h.data()); qToLittleEndian<quint32>(bytes+36,h.data()+4);
    std::copy_n("WAVEfmt ",8,h.data()+8); qToLittleEndian<quint32>(16,h.data()+16);
    qToLittleEndian<quint16>(1,h.data()+20); qToLittleEndian<quint16>(1,h.data()+22);
    qToLittleEndian<quint32>(SampleRate,h.data()+24); qToLittleEndian<quint32>(SampleRate*2,h.data()+28);
    qToLittleEndian<quint16>(2,h.data()+32); qToLittleEndian<quint16>(16,h.data()+34);
    std::copy_n("data",4,h.data()+36); qToLittleEndian<quint32>(bytes,h.data()+40);
    return h;
}

// QAudioDecoder replaces source timestamps with a cumulative sample counter.
// Decoding directly preserves offsets and gaps when subtitles are later used
// with the original video. These are the same LGPL shared libraries already
// required by the Qt multimedia playback plugin.
class FFmpegApi {
public:
    QLibrary util, codec, format, resample;
#define FN(name) decltype(&::name) name = nullptr
    FN(avutil_version); FN(avcodec_version); FN(avformat_version); FN(swresample_version);
    FN(av_strerror); FN(av_dict_set); FN(av_dict_free); FN(av_rescale_q);
    FN(avformat_alloc_context); FN(avformat_open_input); FN(avformat_find_stream_info);
    FN(avformat_close_input); FN(av_read_frame);
    FN(avcodec_find_decoder); FN(avcodec_alloc_context3); FN(avcodec_parameters_to_context);
    FN(avcodec_open2); FN(avcodec_free_context); FN(avcodec_send_packet); FN(avcodec_receive_frame);
    FN(av_packet_alloc); FN(av_packet_free); FN(av_packet_unref);
    FN(av_frame_alloc); FN(av_frame_free); FN(av_frame_unref);
    FN(swr_alloc_set_opts2); FN(swr_init); FN(swr_convert); FN(swr_get_delay);
    FN(swr_get_out_samples); FN(swr_free);
#undef FN
    bool load(QString *error) {
        auto library = [&](QLibrary &lib, const QString &name, int major) {
#ifdef Q_OS_WIN
            const QString filename=name+"-"+QString::number(major)+".dll";
#else
            const QString filename="lib"+name+".so."+QString::number(major);
#endif
            const QString app=QCoreApplication::applicationDirPath();
            QStringList candidates{app+"/"+filename,app+"/lib/"+filename,
                app+"/../lib/subtitledesk/lib/"+filename,
                QLibraryInfo::path(QLibraryInfo::LibrariesPath)+"/"+filename,
                QLibraryInfo::path(QLibraryInfo::BinariesPath)+"/"+filename,filename};
            candidates.removeDuplicates();
            for(const QString &path:candidates){lib.setFileName(path);if(lib.load())return true;}
            *error="The bundled audio decoder is unavailable ("+filename+"). Keep the application and its runtime files together.";
            return false;
        };
        if(!library(util,"avutil",59)||!library(codec,"avcodec",61)||!library(format,"avformat",61)||!library(resample,"swresample",5))return false;
#define BIND(lib,name) name=reinterpret_cast<decltype(name)>(lib.resolve(#name)); if(!name){*error="The bundled audio decoder is incomplete: " #name ".";return false;}
        BIND(util,avutil_version); BIND(codec,avcodec_version); BIND(format,avformat_version); BIND(resample,swresample_version);
        BIND(util,av_strerror); BIND(util,av_dict_set); BIND(util,av_dict_free); BIND(util,av_rescale_q);
        BIND(format,avformat_alloc_context); BIND(format,avformat_open_input); BIND(format,avformat_find_stream_info);
        BIND(format,avformat_close_input); BIND(format,av_read_frame);
        BIND(codec,avcodec_find_decoder); BIND(codec,avcodec_alloc_context3); BIND(codec,avcodec_parameters_to_context);
        BIND(codec,avcodec_open2); BIND(codec,avcodec_free_context); BIND(codec,avcodec_send_packet); BIND(codec,avcodec_receive_frame);
        BIND(codec,av_packet_alloc); BIND(codec,av_packet_free); BIND(codec,av_packet_unref);
        BIND(util,av_frame_alloc); BIND(util,av_frame_free); BIND(util,av_frame_unref);
        BIND(resample,swr_alloc_set_opts2); BIND(resample,swr_init); BIND(resample,swr_convert);
        BIND(resample,swr_get_delay); BIND(resample,swr_get_out_samples); BIND(resample,swr_free);
#undef BIND
        if((avutil_version()>>16)!=59||(avcodec_version()>>16)!=61||(avformat_version()>>16)!=61||(swresample_version()>>16)!=5){
            *error="The audio runtime has incompatible versions. Restore the bundled multimedia files.";return false;
        }
        return true;
    }
    QString message(int code) const {
        char text[AV_ERROR_MAX_STRING_SIZE]{};av_strerror(code,text,sizeof(text));return QString::fromUtf8(text);
    }
};

struct DecodeResult {
    QString source, wav, error;
    qint64 duration = 0;
    QVector<float> peaks;
    std::shared_ptr<QTemporaryDir> temp;
    bool cancelled = false;
};

class PcmWriter {
public:
    QFile file;
    QVector<float> peaks;
    qint64 frames=0, limit=-1;
    int bucket=0;
    float peak=0;
    QString error;
    std::shared_ptr<std::atomic_bool> cancelled;

    bool append(const qint16 *samples, qint64 count) {
        if(count<=0)return true;
        if(count>MaxFrames-frames){error="This audio timeline exceeds the WAV processing limit (about 37 hours). Split it into shorter files first.";return false;}
        static const qint16 silence[8192]{};
        while(count>0){
            if(cancelled->load())return false;
            const int n=int(std::min(qint64(8192),count));const qint16 *p=samples?samples:silence;
            if(file.write(reinterpret_cast<const char*>(p),qint64(n)*2)!=qint64(n)*2){error="Writing decoded audio failed. Check available disk space.";return false;}
            for(int i=0;i<n;++i){
                peak=std::max(peak,float(std::abs(int(p[i])))/32768.0f);
                if(++bucket==PeakFrames){peaks.push_back(peak);bucket=0;peak=0;}
            }
            frames+=n;count-=n;if(samples)samples+=n;
        }
        return true;
    }
    bool at(const qint16 *samples,qint64 count,qint64 target) {
        if(cancelled->load())return false;
        if(target<0){const qint64 skip=std::min(-target,count);samples+=skip;count-=skip;target+=skip;}
        if(count<=0)return true;
        if(limit>=0){if(target>=limit)return true;count=std::min(count,limit-target);}
        // Container timestamp units can round by fractions of a millisecond.
        // Avoid tiny resampling discontinuities while retaining real gaps.
        if(std::abs(target-frames)<=16)target=frames;
        if(target>frames&&!append(nullptr,target-frames))return false;
        if(target<frames){const qint64 skip=std::min(frames-target,count);samples+=skip;count-=skip;}
        if(limit>=0)count=std::min(count,limit-frames);
        return append(samples,count);
    }
};

std::shared_ptr<DecodeResult> decodeAudio(const QString &source,const std::shared_ptr<QTemporaryDir> &temp,
        const std::shared_ptr<std::atomic_bool> &cancelled,const std::function<void(int,const QString&)> &progress) {
    auto result=std::make_shared<DecodeResult>();result->source=source;result->temp=temp;
    FFmpegApi api;if(!api.load(&result->error))return result;
    AVFormatContext *format=api.avformat_alloc_context();
    AVCodecContext *codec=nullptr;AVPacket *packet=nullptr;AVFrame *frame=nullptr;SwrContext *swr=nullptr;
    struct Cleanup {
        FFmpegApi &api;AVFormatContext *&format;AVCodecContext *&codec;AVPacket *&packet;AVFrame *&frame;SwrContext *&swr;
        ~Cleanup(){if(swr)api.swr_free(&swr);if(frame)api.av_frame_free(&frame);if(packet)api.av_packet_free(&packet);if(codec)api.avcodec_free_context(&codec);if(format)api.avformat_close_input(&format);}
    } cleanup{api,format,codec,packet,frame,swr};
    if(!format){result->error="Could not allocate the audio decoder.";return result;}
    format->interrupt_callback.callback=[](void *p){return static_cast<std::atomic_bool*>(p)->load()?1:0;};
    format->interrupt_callback.opaque=cancelled.get();
    AVDictionary *options=nullptr;api.av_dict_set(&options,"protocol_whitelist","file",0);
    const QByteArray path=QFileInfo(source).absoluteFilePath().toUtf8();
    int rc=api.avformat_open_input(&format,path.constData(),nullptr,&options);api.av_dict_free(&options);
    auto fail=[&](const QString &what,int code=0){
        result->cancelled=cancelled->load();if(!result->cancelled)result->error=what+(code<0?"\n\n"+api.message(code):QString());return result;
    };
    if(rc<0)return fail("Cannot open this file's audio. It may be damaged or use an unavailable format.",rc);
    for(unsigned i=0;i<format->nb_streams;++i)
        if(format->streams[i]->codecpar->codec_type!=AVMEDIA_TYPE_AUDIO)format->streams[i]->discard=AVDISCARD_ALL;
    if(cancelled->load())return fail({});
    rc=api.avformat_find_stream_info(format,nullptr);if(rc<0)return fail("Cannot read this file's audio stream information.",rc);
    int audio=-1;bool hasVideo=false;
    for(unsigned i=0;i<format->nb_streams;++i){
        const AVStream *s=format->streams[i];
        if(audio<0&&s->codecpar->codec_type==AVMEDIA_TYPE_AUDIO)audio=int(i);
        if(s->codecpar->codec_type==AVMEDIA_TYPE_VIDEO&&!(s->disposition&AV_DISPOSITION_ATTACHED_PIC))hasVideo=true;
    }
    if(audio<0)return fail("This file has no audio track. Choose an audio file or a video that contains audio.");
    AVStream *stream=format->streams[audio];
    for(unsigned i=0;i<format->nb_streams;++i)format->streams[i]->discard=int(i)==audio?AVDISCARD_DEFAULT:AVDISCARD_ALL;
    const AVCodec *decoder=api.avcodec_find_decoder(stream->codecpar->codec_id);
    if(!decoder)return fail("This file's audio codec is not supported by the bundled decoder.");
    codec=api.avcodec_alloc_context3(decoder);if(!codec)return fail("Could not allocate the audio decoder.");
    rc=api.avcodec_parameters_to_context(codec,stream->codecpar);if(rc<0)return fail("Cannot configure the audio decoder.",rc);
    codec->pkt_timebase=stream->time_base;codec->thread_count=2;
    rc=api.avcodec_open2(codec,decoder,nullptr);if(rc<0)return fail("Cannot start this file's audio decoder.",rc);
    packet=api.av_packet_alloc();frame=api.av_frame_alloc();if(!packet||!frame)return fail("Could not allocate audio decoding buffers.");
    const qint64 originUs=format->start_time==AV_NOPTS_VALUE?0:format->start_time;
    const qint64 originFrames=api.av_rescale_q(originUs,AVRational{1,AV_TIME_BASE},AVRational{1,int(SampleRate)});
    const qint64 durationUs=format->duration==AV_NOPTS_VALUE?-1:format->duration;
    const qint64 sourceFrames=durationUs>0?api.av_rescale_q(durationUs,AVRational{1,AV_TIME_BASE},AVRational{1,int(SampleRate)}):-1;
    if(sourceFrames>MaxFrames)return fail("This audio timeline exceeds the WAV processing limit (about 37 hours). Split it into shorter files first.");
    PcmWriter writer;writer.cancelled=cancelled;if(hasVideo&&sourceFrames>0)writer.limit=sourceFrames;
    writer.file.setFileName(temp->filePath("audio.partial.wav"));
    if(!writer.file.open(QIODevice::ReadWrite)||writer.file.write(wavHeader(0))!=44)return fail("Cannot create the temporary audio file. Check available disk space.");
    int inputRate=0,inputFormat=-1,inputChannels=0;quint64 inputMask=0;qint64 decodedFrames=0;
    QElapsedTimer progressClock;progressClock.start();
    auto report=[&](){
        if(progressClock.elapsed()<100)return;progressClock.restart();
        const int percent=sourceFrames>0?int(std::clamp(qint64(writer.frames*100/sourceFrames),qint64(0),qint64(99))):-1;
        progress(percent,"Decoding audio · "+QString::number(writer.frames/SampleRate)+" seconds");
    };
    auto flushResampler=[&](){
        if(!swr)return true;
        for(;;){
            if(cancelled->load())return false;
            const int capacity=api.swr_get_out_samples(swr,0);
            if(capacity<0){result->error="Cannot finish resampling audio.";return false;}
            QVector<qint16> pcm(std::max(capacity,64));uint8_t *out=reinterpret_cast<uint8_t*>(pcm.data());
            const int n=api.swr_convert(swr,&out,int(pcm.size()),nullptr,0);
            if(n<0){result->error="Cannot finish resampling audio.\n\n"+api.message(n);return false;}
            if(!n)return true;
            if(!writer.at(pcm.constData(),n,writer.frames)){result->error=writer.error;return false;}
        }
    };
    auto receiveFrames=[&](){
        for(;;){
            if(cancelled->load())return false;
            const int status=api.avcodec_receive_frame(codec,frame);
            if(status==AVERROR(EAGAIN)||status==AVERROR_EOF)return true;
            if(status<0){result->error="An audio frame could not be decoded.\n\n"+api.message(status);return false;}
            const int rate=frame->sample_rate>0?frame->sample_rate:codec->sample_rate;
            const AVChannelLayout *layout=frame->ch_layout.nb_channels?&frame->ch_layout:&codec->ch_layout;
            if(rate<=0||rate>384000||layout->nb_channels<=0||layout->nb_channels>64||frame->nb_samples<0||frame->nb_samples>rate*4){
                result->error="The file contains invalid or unsupported audio stream parameters.";return false;
            }
            const quint64 mask=layout->order==AV_CHANNEL_ORDER_NATIVE?layout->u.mask:0;
            if(!swr||rate!=inputRate||frame->format!=inputFormat||layout->nb_channels!=inputChannels||mask!=inputMask){
                if(!flushResampler())return false;if(swr)api.swr_free(&swr);
                AVChannelLayout mono=AV_CHANNEL_LAYOUT_MONO;
                const int status=api.swr_alloc_set_opts2(&swr,&mono,AV_SAMPLE_FMT_S16,int(SampleRate),layout,AVSampleFormat(frame->format),rate,0,nullptr);
                if(status<0||!swr||api.swr_init(swr)<0){result->error="Cannot convert this audio stream to mono PCM.";return false;}
                inputRate=rate;inputFormat=frame->format;inputChannels=layout->nb_channels;inputMask=mask;
            }
            const qint64 pts=frame->best_effort_timestamp!=AV_NOPTS_VALUE?frame->best_effort_timestamp:frame->pts;
            qint64 target=writer.frames;
            if(pts!=AV_NOPTS_VALUE){
                const long double normalized=static_cast<long double>(api.av_rescale_q(pts,stream->time_base,AVRational{1,int(SampleRate)}))-originFrames
                    -qRound64(double(api.swr_get_delay(swr,rate))*SampleRate/rate);
                if(normalized < -MaxFrames || normalized > MaxFrames){
                    result->error="This audio timestamp exceeds the WAV processing limit (about 37 hours). Split the file into shorter sections first.";return false;
                }
                target=qint64(normalized);
            }
            const int capacity=api.swr_get_out_samples(swr,frame->nb_samples);
            if(capacity<0||capacity>int(SampleRate*5)){result->error="The file contains an invalid audio frame size.";return false;}
            QVector<qint16> pcm(std::max(capacity,64));uint8_t *out=reinterpret_cast<uint8_t*>(pcm.data());
            const int n=api.swr_convert(swr,&out,int(pcm.size()),const_cast<const uint8_t**>(frame->extended_data),frame->nb_samples);
            if(n<0){result->error="An audio frame could not be resampled.\n\n"+api.message(n);return false;}
            decodedFrames+=n;
            if(!writer.at(pcm.constData(),n,target)){result->error=writer.error;return false;}
            api.av_frame_unref(frame);report();
        }
    };
    progress(0,"Decoding the first audio track…");
    while(!cancelled->load()&&(rc=api.av_read_frame(format,packet))>=0){
        if(packet->stream_index==audio){
            int status=api.avcodec_send_packet(codec,packet);
            if(status==AVERROR(EAGAIN)){if(!receiveFrames())return fail(result->error);status=api.avcodec_send_packet(codec,packet);}
            if(status<0){api.av_packet_unref(packet);return fail("An audio packet could not be decoded.",status);}
            if(!receiveFrames()){api.av_packet_unref(packet);return fail(result->error);}
        }
        api.av_packet_unref(packet);
    }
    if(cancelled->load())return fail({});
    if(rc!=AVERROR_EOF)return fail("Reading the audio stream failed before the end of the file.",rc);
    rc=api.avcodec_send_packet(codec,nullptr);if(rc<0&&rc!=AVERROR_EOF)return fail("Cannot finish decoding the audio stream.",rc);
    if(!receiveFrames()||!flushResampler())return fail(result->error);
    if(decodedFrames==0)return fail("The file contains no decodable audio samples.");
    if(writer.limit>=0&&writer.frames<writer.limit&&!writer.append(nullptr,writer.limit-writer.frames))return fail(writer.error);
    if(writer.bucket)writer.peaks.push_back(writer.peak);
    if(!writer.file.seek(0)||writer.file.write(wavHeader(quint32(writer.frames*2)))!=44||!writer.file.flush())return fail("Could not finish writing decoded audio. Check available disk space.");
    writer.file.close();if(cancelled->load())return fail({});
    result->wav=temp->filePath("audio.wav");if(!writer.file.rename(result->wav))return fail("Could not complete the temporary audio file.");
    result->duration=writer.frames*1000/SampleRate;result->peaks=std::move(writer.peaks);return result;
}
}

class AudioDecodeWorker final : public QThread {
    Q_OBJECT
public:
    QString source;
    std::shared_ptr<QTemporaryDir> temp;
    std::shared_ptr<std::atomic_bool> cancelled;
    std::shared_ptr<DecodeResult> result;
signals:
    void progress(int percent,QString text);
protected:
    void run() override {
        result=decodeAudio(source,temp,cancelled,[this](int value,const QString &text){emit progress(value,text);});
        // Cancellation can coincide with application shutdown, when deferred
        // QObject deletion is no longer serviced. Release temporary files here.
        if(cancelled->load()){result.reset();temp.reset();}
    }
};

AudioSession::AudioSession(QObject *parent):QObject(parent) {
    m_cleanupTimer=new QTimer(this);m_cleanupTimer->setInterval(1000);
    connect(m_cleanupTimer,&QTimer::timeout,this,[this](){
        for(qsizetype i=m_retiredTemps.size();i-->0;)if(m_retiredTemps[i]->remove())m_retiredTemps.removeAt(i);
        if(m_retiredTemps.isEmpty())m_cleanupTimer->stop();
    });
}
AudioSession::~AudioSession(){
    stopPending();
    // Cancellation is checked in demux, decode, resample and file-write loops.
    // Join before teardown so temporary files and DLL state are released even
    // when the application event loop has already stopped.
    for(QThread *worker:m_workers){worker->disconnect(this);worker->wait();delete worker;}
    m_workers.clear();
}
void AudioSession::stopPending(){
    ++m_generation;m_loading=false;if(m_cancelFlag)m_cancelFlag->store(true);m_cancelFlag.reset();m_pendingTemp.reset();
}
void AudioSession::cancel(){if(!m_loading)return;stopPending();emit cancelled();}
void AudioSession::open(const QString &path){
    stopPending();
    const QFileInfo input(path);
    if(!input.isFile()||!input.isReadable()){emit failed("The media file does not exist or cannot be read.");return;}
    m_pendingTemp=std::make_shared<QTemporaryDir>(QDir::tempPath()+"/subtitledesk-audio-XXXXXX");
    if(!m_pendingTemp->isValid()){m_pendingTemp.reset();emit failed("Cannot create a temporary audio folder. Check available disk space.");return;}
    m_loading=true;m_cancelFlag=std::make_shared<std::atomic_bool>(false);
    const quint64 generation=m_generation;
    auto *worker=new AudioDecodeWorker;worker->source=input.absoluteFilePath();worker->temp=m_pendingTemp;worker->cancelled=m_cancelFlag;
    m_workers.push_back(worker);
    connect(worker,&AudioDecodeWorker::progress,this,[this,generation](int percent,const QString &text){if(m_loading&&generation==m_generation)emit progress(percent,text);});
    connect(worker,&QThread::finished,this,[this,worker,generation](){
        if(generation!=m_generation)return;
        const auto result=worker->result;m_loading=false;m_cancelFlag.reset();m_pendingTemp.reset();
        if(!result){emit failed("The audio decoder did not produce a result.");return;}
        if(result->cancelled){emit cancelled();return;}
        if(!result->error.isEmpty()){emit failed(result->error);return;}
        if(m_temp){m_retiredTemps.push_back(m_temp);m_cleanupTimer->start();}
        m_temp=result->temp;m_sourcePath=result->source;m_wavPath=result->wav;m_duration=result->duration;m_peaks=result->peaks;
        emit progress(100,"Audio ready");emit ready(m_sourcePath,m_wavPath,m_duration,m_peaks);
    });
    connect(worker,&QThread::finished,this,[this,worker](){worker->wait();m_workers.removeAll(worker);delete worker;});
    emit progress(0,"Inspecting the first audio track…");worker->start();
}

#include "audio_session.moc"
