#include "transcription_service.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>

// Real-engine smoke test. The caller supplies its own speech recording so no
// external copyrighted test audio is distributed in the source package.
// Usage: test_transcription RESOURCE_ROOT INPUT_16K_MONO_WAV MODEL [--cancel]
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QStringList args=app.arguments();
    if (args.size()<4) { QTextStream(stderr)<<"Usage: test_transcription RESOURCE_ROOT INPUT_16K_MONO_WAV MODEL [--cancel]\n"; return 2; }
    TranscriptionService service;
    service.setResourceRoot(args[1]);
    const bool expectCancel=args.contains(QStringLiteral("--cancel"));
    QObject::connect(&service,&TranscriptionService::completed,&app,[&](QVector<Cue> cues,QString note) {
        QJsonArray out;
        qint64 previous=-1;
        for (const auto &c:cues) {
            if (c.id.isEmpty() || c.text.isEmpty() || c.startMs<0 || c.endMs<=c.startMs || c.startMs<previous || c.reviewed) {
                QTextStream(stderr)<<"Invalid returned cue\n"; app.exit(1); return;
            }
            previous=c.startMs;
            out.append(QJsonObject{{"startMs",c.startMs},{"endMs",c.endMs},{"text",c.text}});
        }
        QTextStream(stdout)<<QJsonDocument(QJsonObject{{"model",args[3]},{"count",cues.size()},{"note",note},{"cues",out}}).toJson();
        app.exit(!expectCancel && !cues.isEmpty() ? 0 : 1);
    });
    QObject::connect(&service,&TranscriptionService::failed,&app,[&](QString error) { QTextStream(stderr)<<error<<'\n'; app.exit(1); });
    QObject::connect(&service,&TranscriptionService::cancelled,&app,[&] { QTextStream(stdout)<<"Recognition cancelled; no partial transcript emitted.\n"; app.exit(expectCancel ? 0 : 1); });
    QObject::connect(&service,&TranscriptionService::progress,&app,[](int n,QString status) { QTextStream(stderr)<<n<<"% "<<status<<'\n'; });
    QTimer::singleShot(0,&service,[&] { service.start(args[2],args[3],QStringLiteral("en"),2); if(expectCancel) QTimer::singleShot(40,&service,&TranscriptionService::cancel); });
    QTimer::singleShot(180000,&app,[&] { service.cancel(); QTextStream(stderr)<<"Test timeout\n"; app.exit(1); });
    return app.exec();
}
