#include "transcription_service.h"
#include "model_catalog.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

// Real-engine integration test. The source includes a short synthetic speech
// fixture; callers can also supply their own 16 kHz mono PCM recording.
// Usage: test_transcription RESOURCE_ROOT INPUT_16K_MONO_WAV MODEL
//        [--cancel] [--import-model LOCAL_MODEL_FILE]
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QStringList args=app.arguments();
    if (args.size()<4) { QTextStream(stderr)<<"Usage: test_transcription RESOURCE_ROOT INPUT_16K_MONO_WAV MODEL [--cancel] [--import-model LOCAL_MODEL_FILE]\n"; return 2; }
    const int importIndex=args.indexOf(QStringLiteral("--import-model"));
    if(importIndex>=0&&(importIndex+1>=args.size()||!QFileInfo(args[importIndex+1]).isFile())) {
        QTextStream(stderr)<<"--import-model requires an existing local model file\n";return 2;
    }
    QTemporaryDir importedStorage;
    std::unique_ptr<ModelCatalog> catalog;
    std::unique_ptr<ModelCatalog> observer;
    if(importIndex>=0) {
        if(!importedStorage.isValid()){QTextStream(stderr)<<"Cannot create isolated model storage\n";return 1;}
        catalog=std::make_unique<ModelCatalog>(args[1],importedStorage.path());
    }
    TranscriptionService service;
    service.setResourceRoot(args[1]);
    if(catalog)service.setModelCatalog(catalog.get());
    const bool expectCancel=args.contains(QStringLiteral("--cancel"));
    QString selectedModel=args[3];
    auto checkModelReleased=[&] {
        if(importIndex<0)return true;
        if(!observer) { QTextStream(stderr)<<"Missing independent model observer\n";return false; }
        if(observer->inUse(selectedModel)) {
            QTextStream(stderr)<<"Recognition left the imported model locked\n";return false;
        }
        QString error;
        if(!observer->removeModel(selectedModel,&error)) {
            QTextStream(stderr)<<"Completed recognition did not release its model: "<<error<<'\n';return false;
        }
        return true;
    };
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
        QTextStream(stdout)<<QJsonDocument(QJsonObject{{"model",selectedModel},{"count",cues.size()},{"note",note},{"cues",out}}).toJson();
        app.exit(!expectCancel && !cues.isEmpty() && checkModelReleased() ? 0 : 1);
    });
    QObject::connect(&service,&TranscriptionService::failed,&app,[&](QString error) { QTextStream(stderr)<<error<<'\n'; app.exit(1); });
    QObject::connect(&service,&TranscriptionService::cancelled,&app,[&] { QTextStream(stdout)<<"Recognition cancelled; no partial transcript emitted.\n"; app.exit(expectCancel && checkModelReleased() ? 0 : 1); });
    QObject::connect(&service,&TranscriptionService::progress,&app,[](int n,QString status) { QTextStream(stderr)<<n<<"% "<<status<<'\n'; });
    auto startRecognition=[&] {
        if(catalog&&expectCancel) {
            // A progress observer can cancel synchronously before the native
            // process is created. start() must not reuse the cleared temporary
            // state, and the model lease must be released immediately.
            TranscriptionService beforeStart;
            beforeStart.setModelCatalog(catalog.get());
            bool requested=false,wasCancelled=false,unexpectedTerminal=false;
            QObject::connect(&beforeStart,&TranscriptionService::progress,&app,[&](int,QString) {
                if(!requested){requested=true;beforeStart.cancel();}
            });
            QObject::connect(&beforeStart,&TranscriptionService::cancelled,&app,[&]{wasCancelled=true;});
            QObject::connect(&beforeStart,&TranscriptionService::failed,&app,[&](QString){unexpectedTerminal=true;});
            QObject::connect(&beforeStart,&TranscriptionService::completed,&app,[&](QVector<Cue>,QString){unexpectedTerminal=true;});
            beforeStart.start(args[2],selectedModel,QStringLiteral("en"),2);
            if(!requested||!wasCancelled||unexpectedTerminal||beforeStart.busy()||catalog->inUse(selectedModel)) {
                QTextStream(stderr)<<"Cancellation before worker start did not cleanly release model state\n";
                app.exit(1);return;
            }
        }
        service.start(args[2],selectedModel,QStringLiteral("en"),2);
        if(catalog) {
            QString error;
            if(!service.busy()||!catalog->inUse(selectedModel)||catalog->removeModel(selectedModel,&error)) {
                QTextStream(stderr)<<"Active recognition did not protect its imported model\n";
                service.cancel();app.exit(1);return;
            }
            observer=std::make_unique<ModelCatalog>(args[1],importedStorage.path());
            // The worker must retain its file lease even if the catalog that
            // supplied it is destroyed. A second instance still cannot delete
            // the model before recognition has finished or been cancelled.
            catalog.reset();
            if(!observer->inUse(selectedModel)||observer->removeModel(selectedModel,&error)) {
                QTextStream(stderr)<<"Destroying the source catalog released an active model lease\n";
                service.cancel();app.exit(1);return;
            }
        }
        if(expectCancel)QTimer::singleShot(40,&service,&TranscriptionService::cancel);
    };
    if(catalog) {
        QObject::connect(catalog.get(),&ModelCatalog::modelImported,&app,[&](QString id,bool) {
            const auto model=catalog->model(id);
            if(model.id.isEmpty()||model.bundled||(model.engine!=QStringLiteral("whisper")&&model.engine!=QStringLiteral("whistle"))||catalog->busy()) {
                QTextStream(stderr)<<"Expected a validated imported speech model\n";app.exit(1);return;
            }
            selectedModel=id;startRecognition();
        });
        QObject::connect(catalog.get(),&ModelCatalog::importFailed,&app,[&](QString error) {
            QTextStream(stderr)<<"Model import failed: "<<error<<'\n';app.exit(1);
        });
        QTimer::singleShot(0,&app,[&] { catalog->importModel(args[importIndex+1]); });
    } else QTimer::singleShot(0,&service,startRecognition);
    QTimer::singleShot(180000,&app,[&] { service.cancel(); QTextStream(stderr)<<"Test timeout\n"; app.exit(1); });
    return app.exec();
}
