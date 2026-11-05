#include "mainwindow.h"
#include <QApplication>
#include <QAction>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QTimer>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

static QString binaryDirectory(int argc,char **argv){
#ifdef Q_OS_WIN
    wchar_t buffer[32768];const DWORD n=GetModuleFileNameW(nullptr,buffer,32768);if(n)return QFileInfo(QString::fromWCharArray(buffer,n)).absolutePath();
#else
    const QString target=QFileInfo("/proc/self/exe").symLinkTarget();if(!target.isEmpty())return QFileInfo(target).absolutePath();
#endif
    return argc?QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath():QDir::currentPath();
}
int main(int argc,char **argv){
    const QString bin=binaryDirectory(argc,argv);
    const QString plugins=QDir(bin+"/plugins").exists()?bin+"/plugins":QDir::cleanPath(bin+"/../lib/subtitledesk/plugins");
    if(QDir(plugins).exists()){qputenv("QT_PLUGIN_PATH",QFile::encodeName(plugins));qputenv("QT_QPA_PLATFORM_PLUGIN_PATH",QFile::encodeName(plugins+"/platforms"));}
    if(qEnvironmentVariableIsEmpty("QT_MEDIA_BACKEND"))qputenv("QT_MEDIA_BACKEND","ffmpeg");
    QApplication app(argc,argv);app.setApplicationName("SubtitleDesk");app.setOrganizationName("SubtitleDesk");app.setApplicationVersion("1.0.0");app.setWindowIcon(QIcon(":/app.png"));
    QCommandLineParser parser;parser.setApplicationDescription("Offline subtitle and lyrics editor");parser.addHelpOption();parser.addVersionOption();parser.addPositionalArgument("file","Audio, video, subtitle, or project to open.");
    parser.addOption({"demo","Open built-in visual demonstration."});parser.addOption({"theme","Select system, light or dark theme.","theme"});parser.addOption({"screenshot","Save a native window screenshot and exit (for preview generation).","path"});parser.addOption({"panel","Preview panel: main, settings or export.","panel","main"});parser.addOption({"size","Window dimensions for preview, such as 1180x800.","size"});
    parser.process(app);const bool snapshot=parser.isSet("screenshot");MainWindow window(snapshot);if(parser.isSet("theme"))window.setTheme(parser.value("theme"));
    if(parser.isSet("size")){const auto parts=parser.value("size").split('x');if(parts.size()==2)window.resize(parts[0].toInt(),parts[1].toInt());}
    if(parser.isSet("demo"))window.loadDemo();window.show();
    if(!parser.positionalArguments().isEmpty())QTimer::singleShot(50,&window,[&]{window.openPath(parser.positionalArguments().first());});
    if(snapshot&&parser.value("panel")!="main")QTimer::singleShot(100,&window,[&]{const QString wanted=parser.value("panel")=="settings"?"Settings…":"Export…";for(auto*action:window.findChildren<QAction*>()){QString text=action->text();text.remove('&');if(text==wanted){action->trigger();break;}}});
    if(snapshot)QTimer::singleShot(750,&window,[&]{QWidget *target=app.activeModalWidget();if(!target)target=&window;const bool ok=target->grab().save(parser.value("screenshot"));app.exit(ok?0:1);});
    return app.exec();
}
