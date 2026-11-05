#include "mainwindow.h"
#include "audio_session.h"
#include "transcription_service.h"
#include "model_catalog.h"
#include "model_manager_dialog.h"
#include "waveform.h"
#include <QAction>
#include <QApplication>
#include <QAudioOutput>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyleHints>
#include <QTableWidget>
#include <QTextCursor>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {
QPushButton *button(const QString &text,const QString &name,QWidget *parent=nullptr){
    auto *b=new QPushButton(text,parent);b->setObjectName(name);b->setCursor(Qt::PointingHandCursor);b->setFocusPolicy(Qt::StrongFocus);return b;
}
QLabel *label(const QString &text,const QString &name={}){auto *l=new QLabel(text);l->setTextFormat(Qt::PlainText);l->setObjectName(name);return l;}
QString displayTime(qint64 t){return SubtitleDocument::formatTime(t);}
QString compactTime(qint64 t){const QString s=displayTime(t);return s.startsWith("00:")?s.mid(3):s;}
QString mediaFilter(){return "Audio and video (*.mp4 *.mkv *.webm *.mov *.avi *.m4v *.mp3 *.wav *.flac *.m4a *.aac *.ogg *.opus *.wma *.aiff *.aif *.mka *.ts);;All files (*)";}
QString projectFilter(){return "Subtitle Desk project (*.sdesk);;JSON project (*.json)";}
QString importFilter(){return "Subtitle and lyric files (*.srt *.vtt *.ass *.ssa *.lrc);;All files (*)";}
}

MainWindow::MainWindow(bool testMode,QWidget *parent):QMainWindow(parent),m_testMode(testMode){
    setObjectName("subtitleDeskWindow");setMinimumSize(680,540);resize(1180,800);setAcceptDrops(true);
    QString userConfig=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+"/SubtitleDesk";
#ifdef Q_OS_WIN
    if(!qEnvironmentVariable("APPDATA").isEmpty())userConfig=QDir(qEnvironmentVariable("APPDATA")).filePath("SubtitleDesk");
#endif
    m_configDir=QFileInfo::exists(QCoreApplication::applicationDirPath()+"/portable.flag")
        ?QCoreApplication::applicationDirPath()+"/data":userConfig;
    if(m_testMode)m_configDir=QDir::tempPath()+"/subtitledesk-test-"+QString::number(QCoreApplication::applicationPid());
    QDir().mkpath(m_configDir);m_settings=new QSettings(m_configDir+"/settings.ini",QSettings::IniFormat,this);
    m_threads=std::clamp(m_settings->value("threads",std::min(8,std::max(1,QThread::idealThreadCount()/2))).toInt(),1,32);
    m_audio=new AudioSession(this);m_stt=new TranscriptionService(this);m_stt->setResourceRoot(resourceRoot());
    m_modelCatalog=new ModelCatalog(resourceRoot(),m_configDir+"/models",this);m_modelCatalog->setObjectName("modelCatalog");m_stt->setModelCatalog(m_modelCatalog);
    m_player=new QMediaPlayer(this);m_output=new QAudioOutput(this);m_player->setAudioOutput(m_output);
    buildUi();buildMenus();connectActions();
    refreshModelChoices(m_settings->value("model","base").toString(),m_settings->value("language","auto").toString());
    m_volume->setValue(m_settings->value("volume",80).toInt());m_output->setVolume(m_volume->value()/100.0);
    setTheme(m_settings->value("theme","system").toString());
    if(!m_testMode&&!m_settings->value("geometry").toByteArray().isEmpty())restoreGeometry(m_settings->value("geometry").toByteArray());
    if(!m_settings->value("splitter").toByteArray().isEmpty())m_splitter->restoreState(m_settings->value("splitter").toByteArray());
    m_typingTimer.setSingleShot(true);m_typingTimer.setInterval(700);connect(&m_typingTimer,&QTimer::timeout,this,[this]{m_typing=false;});
    m_autosaveTimer.setInterval(20000);connect(&m_autosaveTimer,&QTimer::timeout,this,&MainWindow::autosave);if(!m_testMode)m_autosaveTimer.start();
    m_playTimer.setInterval(40);connect(&m_playTimer,&QTimer::timeout,this,[this]{if(m_player->playbackState()==QMediaPlayer::PlayingState)updatePosition(m_player->position());});m_playTimer.start();
    connect(qApp->styleHints(),&QStyleHints::colorSchemeChanged,this,[this]{if(m_theme=="system")setTheme("system");});
    refresh(false);setStatus("Open an audio or video file to begin. All transcription runs locally.");
    if(!m_testMode)QTimer::singleShot(150,this,&MainWindow::offerRecovery);
}
MainWindow::~MainWindow(){
    // Focus changes during QWidget destruction can emit editingFinished.
    // Remove callbacks while the document and undo history still exist.
    m_loadingEditor=true;m_refreshing=true;m_typingTimer.stop();m_autosaveTimer.stop();m_playTimer.stop();
    for(QObject *child:findChildren<QObject*>())child->disconnect(this);
    qApp->styleHints()->disconnect(this);m_stt->cancel();m_audio->cancel();m_modelCatalog->cancelImport();m_player->stop();m_player->setSource({});delete m_player;m_player=nullptr;
}
QString MainWindow::resourceRoot()const{
    const auto overridePath=qEnvironmentVariable("SUBTITLEDESK_RESOURCES");if(!overridePath.isEmpty())return overridePath;
    const QString local=QCoreApplication::applicationDirPath()+"/resources";if(QDir(local).exists())return local;
    return QDir::cleanPath(QCoreApplication::applicationDirPath()+"/../lib/subtitledesk/resources");
}
void MainWindow::buildUi(){
    auto *central=new QWidget;setCentralWidget(central);auto *root=new QVBoxLayout(central);root->setContentsMargins(18,12,18,8);root->setSpacing(10);
    auto *headingWidget=new QWidget;headingWidget->setObjectName("headingWidget");auto *heading=new QHBoxLayout(headingWidget);heading->setContentsMargins(0,0,0,0);auto *brand=new QVBoxLayout;brand->setSpacing(1);
    brand->addWidget(label("SUBTITLE DESK","brand"));brand->addWidget(label("Listen. Refine. Keep every word in time.","muted"));heading->addLayout(brand);heading->addStretch();
    auto *open=button("Open media…","openMediaButton");connect(open,&QPushButton::clicked,this,&MainWindow::openMediaDialog);heading->addWidget(open);
    auto *exportBtn=button("Export…","exportButton");exportBtn->setProperty("primary",true);connect(exportBtn,&QPushButton::clicked,this,&MainWindow::exportSubtitles);heading->addWidget(exportBtn);root->addWidget(headingWidget);

    auto *source=new QFrame;source->setObjectName("panel");auto *sl=new QVBoxLayout(source);sl->setContentsMargins(12,9,12,9);sl->setSpacing(6);
    auto *fileRow=new QHBoxLayout;m_sourceLabel=label("No media loaded","sourceName");m_sourceLabel->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);fileRow->addWidget(m_sourceLabel,1);m_sourceDetail=label("AUDIO ONLY · OFFLINE","eyebrow");fileRow->addWidget(m_sourceDetail);sl->addLayout(fileRow);
    auto *modelRow=new QHBoxLayout;modelRow->setSpacing(8);m_model=new QComboBox;m_model->setObjectName("modelCombo");
    m_model->setMinimumWidth(165);m_model->setMinimumContentsLength(16);m_model->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);m_model->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);modelRow->addWidget(m_model,2);
    m_manageModels=button("Models…","manageModelsButton");m_manageModels->setToolTip("Import, inspect, or remove local speech models.");modelRow->addWidget(m_manageModels);
    m_language=new QComboBox;m_language->setObjectName("languageCombo");m_language->setMinimumWidth(105);m_language->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);m_language->setMinimumContentsLength(10);modelRow->addWidget(m_language,1);
    m_transcribe=button("Transcribe","transcribeButton");m_transcribe->setProperty("primary",true);modelRow->addWidget(m_transcribe);
    m_cancel=button("Cancel","cancelButton");m_cancel->hide();modelRow->addWidget(m_cancel);sl->addLayout(modelRow);
    m_modelHint=label("Timed segments · runs on your CPU · no upload","modelHint");m_modelHint->setWordWrap(true);sl->addWidget(m_modelHint);root->addWidget(source);

    auto *audioPanel=new QFrame;audioPanel->setObjectName("panel");auto *al=new QVBoxLayout(audioPanel);al->setContentsMargins(0,0,0,8);al->setSpacing(4);
    m_wave=new Waveform;m_wave->setObjectName("waveform");al->addWidget(m_wave,1);
    auto *transport=new QHBoxLayout;transport->setContentsMargins(8,0,8,0);transport->setSpacing(6);
    auto *back=button("−3 s","backButton");back->setMaximumWidth(50);transport->addWidget(back);
    m_play=button("Play","playButton");m_play->setMinimumWidth(60);transport->addWidget(m_play);
    auto *forward=button("+3 s","forwardButton");forward->setMaximumWidth(50);transport->addWidget(forward);
    m_clock=label("00:00.000 / 00:00.000","clock");m_clock->setMinimumWidth(142);transport->addWidget(m_clock);transport->addStretch();
    m_loop=new QCheckBox("Loop cue");m_loop->setObjectName("loopCheck");m_loop->setToolTip("Repeat the selected cue with a short lead-in.");transport->addWidget(m_loop);
    m_speed=new QComboBox;m_speed->setObjectName("speedCombo");for(double s:{0.5,0.75,1.0,1.25,1.5,2.0})m_speed->addItem(QString::number(s,'g',3)+"×",s);m_speed->setCurrentIndex(2);m_speed->setToolTip("Playback speed");transport->addWidget(m_speed);
    m_volume=new QSlider(Qt::Horizontal);m_volume->setObjectName("volumeSlider");m_volume->setRange(0,100);m_volume->setFixedWidth(65);m_volume->setToolTip("Volume");transport->addWidget(m_volume);al->addLayout(transport);root->addWidget(audioPanel);
    connect(back,&QPushButton::clicked,this,[this]{setPlaybackPosition(std::max(qint64(0),m_player->position()-3000));});
    connect(forward,&QPushButton::clicked,this,[this]{setPlaybackPosition(std::min(m_audio->durationMs(),m_player->position()+3000));});

    m_splitter=new QSplitter(Qt::Horizontal);m_splitter->setObjectName("editorSplitter");m_splitter->setChildrenCollapsible(false);
    auto *listPanel=new QWidget;auto *ll=new QVBoxLayout(listPanel);ll->setContentsMargins(0,0,0,0);ll->setSpacing(6);
    auto *listHeader=new QHBoxLayout;m_summary=label("CUES  ·  0","sectionTitle");listHeader->addWidget(m_summary);listHeader->addStretch();
    m_follow=new QCheckBox("Follow audio");m_follow->setObjectName("followCheck");m_follow->setChecked(true);listHeader->addWidget(m_follow);ll->addLayout(listHeader);
    auto *findRow=new QHBoxLayout;m_search=new QLineEdit;m_search->setObjectName("searchEdit");m_search->setPlaceholderText("Find words in cues…");m_search->setClearButtonEnabled(true);findRow->addWidget(m_search,1);
    m_unreviewed=new QCheckBox("To review");m_unreviewed->setObjectName("unreviewedCheck");findRow->addWidget(m_unreviewed);ll->addLayout(findRow);
    m_table=new QTableWidget(0,5);m_table->setObjectName("cueTable");m_table->setHorizontalHeaderLabels({"#","Start","End","Text","✓"});m_table->verticalHeader()->hide();m_table->setShowGrid(false);m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);m_table->setWordWrap(false);
    m_table->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Fixed);m_table->setColumnWidth(0,36);m_table->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Fixed);m_table->setColumnWidth(1,94);m_table->horizontalHeader()->setSectionResizeMode(2,QHeaderView::Fixed);m_table->setColumnWidth(2,94);
    m_table->horizontalHeader()->setSectionResizeMode(3,QHeaderView::Stretch);m_table->horizontalHeader()->setSectionResizeMode(4,QHeaderView::Fixed);m_table->setColumnWidth(4,26);m_table->verticalHeader()->setDefaultSectionSize(42);m_table->setMinimumWidth(300);ll->addWidget(m_table,1);
    auto *editTools=new QHBoxLayout;editTools->setSpacing(5);
    for(const auto &pair:QList<QPair<QString,QString>>{{"+ Add","addButton"},{"Split","splitButton"},{"Merge","mergeButton"},{"Delete","deleteButton"}})editTools->addWidget(button(pair.first,pair.second));editTools->addStretch();ll->addLayout(editTools);m_splitter->addWidget(listPanel);

    auto *editor=new QFrame;editor->setObjectName("panel");editor->setMinimumWidth(275);auto *el=new QVBoxLayout(editor);el->setContentsMargins(12,10,12,10);el->setSpacing(7);
    auto *editorHeader=new QHBoxLayout;m_cueTitle=label("CUE EDITOR","sectionTitle");editorHeader->addWidget(m_cueTitle);editorHeader->addStretch();auto *previous=button("‹","previousButton");previous->setFixedWidth(28);previous->setToolTip("Previous cue (Alt+Up)");editorHeader->addWidget(previous);auto *next=button("›","nextButton");next->setFixedWidth(28);next->setToolTip("Next cue (Alt+Down)");editorHeader->addWidget(next);el->addLayout(editorHeader);
    auto *times=new QHBoxLayout;times->setSpacing(8);
    auto makeTime=[&](const QString &name,const QString &caption,bool isStart){auto *col=new QVBoxLayout;col->setSpacing(3);col->addWidget(label(caption,"eyebrow"));auto *edit=new QLineEdit;edit->setObjectName(name);edit->setPlaceholderText("00:00:00.000");edit->setMinimumWidth(100);col->addWidget(edit);auto *b=button("Set at playhead",name+"Set");b->setToolTip(isStart?"Set start to the playhead (Ctrl+[)":"Set end to the playhead (Ctrl+])");connect(b,&QPushButton::clicked,this,[this,isStart]{setBoundary(isStart);});col->addWidget(b);times->addLayout(col);return edit;};
    m_start=makeTime("startEdit","START",true);m_end=makeTime("endEdit","END",false);el->addLayout(times);
    m_text=new QPlainTextEdit;m_text->setObjectName("cueTextEdit");m_text->setPlaceholderText("Select a cue to edit its words.\n\nUse Enter for a subtitle line break.");m_text->setMinimumHeight(55);m_text->setUndoRedoEnabled(false);m_text->installEventFilter(this);m_text->setTabChangesFocus(true);el->addWidget(m_text,1);
    m_stats=label("Select a cue to inspect its timing.","muted");m_stats->setWordWrap(true);el->addWidget(m_stats);
    auto *reviewRow=new QHBoxLayout;auto *replay=button("Replay cue","replayButton");reviewRow->addWidget(replay);m_review=button("Mark reviewed","reviewButton");m_review->setProperty("primary",true);reviewRow->addWidget(m_review);el->addLayout(reviewRow);m_splitter->addWidget(editor);m_splitter->setStretchFactor(0,3);m_splitter->setStretchFactor(1,2);m_splitter->setSizes({650,400});root->addWidget(m_splitter,1);
    auto *foot=new QHBoxLayout;m_status=label("Ready","statusText");m_status->setWordWrap(true);m_status->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);foot->addWidget(m_status,1);m_progress=new QProgressBar;m_progress->setObjectName("workProgress");m_progress->setFixedWidth(140);m_progress->setMaximumHeight(14);m_progress->hide();foot->addWidget(m_progress);
    m_zoom=new QComboBox;m_zoom->setObjectName("zoomCombo");m_zoom->addItem("Full track",0);m_zoom->addItem("30 seconds",30);m_zoom->addItem("10 seconds",10);m_zoom->setToolTip("Waveform zoom follows the playhead");foot->addWidget(m_zoom);root->addLayout(foot);
}
void MainWindow::buildMenus(){
    auto add=[this](QMenu*m,const QString&t,const QKeySequence&k,auto fn){auto*a=m->addAction(t);if(!k.isEmpty())a->setShortcut(k);connect(a,&QAction::triggered,this,fn);return a;};
    auto *file=menuBar()->addMenu("&File");add(file,"Open &media…",QKeySequence::Open,[this]{openMediaDialog();});add(file,"Open &project…",QKeySequence("Ctrl+Shift+O"),[this]{openProjectDialog();});add(file,"&Import subtitles / lyrics…",QKeySequence("Ctrl+I"),[this]{importSubtitles();});add(file,"Link / replace audio…",{},[this]{auto p=QFileDialog::getOpenFileName(this,"Link audio while keeping cues",{},mediaFilter());if(!p.isEmpty())openMedia(p,true);});file->addSeparator();add(file,"&Save project",QKeySequence::Save,[this]{saveProject();});add(file,"Save project &as…",QKeySequence::SaveAs,[this]{saveProject(true);});add(file,"&Export…",QKeySequence("Ctrl+E"),[this]{exportSubtitles();});file->addSeparator();add(file,"E&xit",QKeySequence::Quit,[this]{close();});
    auto *edit=menuBar()->addMenu("&Edit");add(edit,"&Undo",QKeySequence::Undo,[this]{undo();});add(edit,"&Redo",QKeySequence::Redo,[this]{redo();})->setShortcuts({QKeySequence::Redo,QKeySequence("Ctrl+Shift+Z")});edit->addSeparator();add(edit,"Add cue",QKeySequence("Ctrl+Return"),[this]{addCue();});add(edit,"Split at text cursor / playhead",QKeySequence("Ctrl+K"),[this]{splitCue();});add(edit,"Merge selected cues",QKeySequence("Ctrl+M"),[this]{mergeCues();});add(edit,"Delete selected cues",QKeySequence("Ctrl+Delete"),[this]{deleteCues();});add(edit,"Mark cue reviewed / unreviewed",QKeySequence("Ctrl+R"),[this]{setReviewed();});edit->addSeparator();add(edit,"Shift cue times…",QKeySequence("Ctrl+Shift+T"),[this]{shiftCues();});add(edit,"Sort cues by start time",{},[this]{sortCues();});add(edit,"Check timing and readability…",QKeySequence("Ctrl+Shift+C"),[this]{showTimingCheck();});
    auto *play=menuBar()->addMenu("&Playback");add(play,"Play / pause",QKeySequence("Ctrl+Space"),[this]{playPause();});add(play,"Replay selected cue",QKeySequence("Ctrl+P"),[this]{replayCue();});add(play,"Previous cue",QKeySequence("Alt+Up"),[this]{navigate(-1);});add(play,"Next cue",QKeySequence("Alt+Down"),[this]{navigate(1);});add(play,"Set cue start at playhead",QKeySequence("Ctrl+["),[this]{setBoundary(true);});add(play,"Set cue end at playhead",QKeySequence("Ctrl+]"),[this]{setBoundary(false);});
    add(play,"Volume…",{},[this]{bool ok=false;const int v=QInputDialog::getInt(this,"Playback volume","Volume (%)",m_volume->value(),0,100,5,&ok);if(ok)m_volume->setValue(v);});
    auto *view=menuBar()->addMenu("&View");auto*zoom=view->addMenu("Waveform zoom");for(int i=0;i<m_zoom->count();++i)add(zoom,m_zoom->itemText(i),{},[this,i]{m_zoom->setCurrentIndex(i);});add(view,"&Models…",{},[this]{showModels();})->setObjectName("modelsAction");add(view,"&Settings…",QKeySequence("Ctrl+,"),[this]{showSettings();});auto*theme=view->addMenu("Theme");for(auto s:{"System","Light","Dark"})add(theme,s,{},[this,s]{setTheme(QString(s).toLower());saveSettings();});
    auto *help=menuBar()->addMenu("&Help");add(help,"Quick guide and shortcuts…",QKeySequence::HelpContents,[this]{showShortcuts();});add(help,"Open demonstration",{},[this]{if(maybeSave())loadDemo();});add(help,"About Subtitle Desk…",{},[this]{showAbout();});
}
void MainWindow::connectActions(){
    connect(m_model,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{updateLanguages();updateBusy();saveSettings();});
    connect(m_language,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{saveSettings();});
    connect(m_manageModels,&QPushButton::clicked,this,[this]{showModels();});
    connect(m_modelCatalog,&ModelCatalog::modelsChanged,this,[this]{refreshModelChoices();});
    connect(m_transcribe,&QPushButton::clicked,this,&MainWindow::transcribe);connect(m_cancel,&QPushButton::clicked,this,&MainWindow::cancelWork);
    connect(m_play,&QPushButton::clicked,this,&MainWindow::playPause);connect(m_wave,&Waveform::seekRequested,this,&MainWindow::setPlaybackPosition);
    connect(m_speed,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{m_player->setPlaybackRate(m_speed->currentData().toDouble());});
    connect(m_volume,&QSlider::valueChanged,this,[this](int v){m_output->setVolume(v/100.0);});
    connect(m_zoom,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{m_wave->setZoom(m_zoom->currentData().toInt());});
    connect(m_player,&QMediaPlayer::positionChanged,this,&MainWindow::updatePosition);
    connect(m_player,&QMediaPlayer::playbackStateChanged,this,[this](QMediaPlayer::PlaybackState s){m_play->setText(s==QMediaPlayer::PlayingState?"Pause":"Play");});
    connect(m_player,&QMediaPlayer::errorOccurred,this,[this](QMediaPlayer::Error,const QString&e){setStatus("Playback could not start: "+e);});
    connect(m_table,&QTableWidget::itemSelectionChanged,this,[this]{if(m_refreshing)return;const int r=m_table->currentRow();if(r>=0&&r<m_doc.cues.size()){m_current=r;m_typing=false;loadEditor();}});
    connect(m_table,&QTableWidget::cellDoubleClicked,this,[this](int r,int){selectCue(r,true);replayCue();});
    connect(m_text,&QPlainTextEdit::textChanged,this,&MainWindow::changeText);
    connect(m_start,&QLineEdit::editingFinished,this,[this]{commitTime(true);});connect(m_end,&QLineEdit::editingFinished,this,[this]{commitTime(false);});
    connect(m_search,&QLineEdit::textChanged,this,&MainWindow::filterRows);connect(m_unreviewed,&QCheckBox::toggled,this,&MainWindow::filterRows);
    connect(findChild<QPushButton*>("addButton"),&QPushButton::clicked,this,&MainWindow::addCue);connect(findChild<QPushButton*>("splitButton"),&QPushButton::clicked,this,&MainWindow::splitCue);
    connect(findChild<QPushButton*>("mergeButton"),&QPushButton::clicked,this,&MainWindow::mergeCues);connect(findChild<QPushButton*>("deleteButton"),&QPushButton::clicked,this,&MainWindow::deleteCues);
    connect(findChild<QPushButton*>("previousButton"),&QPushButton::clicked,this,[this]{navigate(-1);});connect(findChild<QPushButton*>("nextButton"),&QPushButton::clicked,this,[this]{navigate(1);});
    connect(findChild<QPushButton*>("replayButton"),&QPushButton::clicked,this,&MainWindow::replayCue);connect(m_review,&QPushButton::clicked,this,&MainWindow::setReviewed);
    auto progress=[this](int pct,const QString &s){m_progress->show();if(pct<0)m_progress->setRange(0,0);else{m_progress->setRange(0,100);m_progress->setValue(pct);}setStatus(s);updateBusy();};
    connect(m_audio,&AudioSession::progress,this,progress);
    connect(m_audio,&AudioSession::ready,this,[this](const QString &source,const QString &wav,qint64 duration,const QVector<float>&peaks){
        m_player->stop();m_player->setSource(QUrl::fromLocalFile(wav));m_player->setPlaybackRate(m_speed->currentData().toDouble());
        if(m_pending){m_doc=*m_pending;m_pending.reset();m_projectPath=m_pendingProjectPath;m_pendingProjectPath.clear();m_undo.clear();m_redo.clear();m_current=-1;setDirty(m_pendingDirty);if(!m_doc.model.isEmpty())refreshModelChoices(m_doc.model,m_doc.language);}
        m_doc.mediaPath=source;m_demo=false;m_mediaReady=true;m_sourceLabel->setText(QFileInfo(source).fileName());m_sourceLabel->setToolTip(source);m_sourceDetail->setText("AUDIO · "+displayTime(duration).left(8));m_wave->setAudio(peaks,duration);m_progress->hide();refresh(false);updatePosition(0);updateBusy();setStatus("Audio ready. Choose a model and transcribe, or import existing subtitles.");
    });
    connect(m_audio,&AudioSession::failed,this,[this](const QString&e){m_pending.reset();m_pendingProjectPath.clear();m_progress->hide();updateBusy();showError(e);});
    connect(m_audio,&AudioSession::cancelled,this,[this]{m_pending.reset();m_pendingProjectPath.clear();m_progress->hide();updateBusy();setStatus("Audio loading cancelled.");});
    connect(m_stt,&TranscriptionService::progress,this,progress);
    connect(m_stt,&TranscriptionService::completed,this,[this](const QVector<Cue>&cues,const QString&note){
        m_progress->hide();updateBusy();
        if(cues.isEmpty()){setStatus("No speech was recognized. The existing cues were kept. Try another model or language.");return;}
        pushUndo();m_doc.cues=cues;m_doc.model=m_model->currentData().toString();m_doc.language=m_language->currentData().toString();m_doc.normalizeIds();m_doc.stableSort();m_current=0;setDirty();refresh(false);selectCue(0);setStatus(QString("Transcription complete · %1 cues. %2").arg(cues.size()).arg(note));autosave();
    });
    connect(m_stt,&TranscriptionService::failed,this,[this](const QString&e){m_progress->hide();updateBusy();showError(e);});
    connect(m_stt,&TranscriptionService::cancelled,this,[this]{m_progress->hide();updateBusy();setStatus("Transcription cancelled. Existing cues were kept.");});
}
void MainWindow::refreshModelChoices(const QString &preferredId,const QString &preferredLanguage){
    QString selected=preferredId.isEmpty()?m_model->currentData().toString():preferredId;
    if(selected.isEmpty())selected="base";
    QString language=preferredLanguage.isEmpty()?m_language->currentData().toString():preferredLanguage;
    if(language.isEmpty())language="auto";
    const QSignalBlocker modelBlock(m_model),languageBlock(m_language);
    m_model->clear();
    for(const auto &model:m_modelCatalog->models()){
        QString title=model.name;
        if(model.bundled){
            if(model.id=="whistle")title="Whistle · compact";
            else if(model.id=="tiny")title="Whisper Tiny · fast";
            else if(model.id=="base")title="Whisper Base · balanced";
            else if(model.id=="small")title="Whisper Small · detailed";
        }else title+=" · imported";
        m_model->addItem(title,model.id);
        m_model->setItemData(m_model->count()-1,QString("<qt>%1</qt>").arg((model.name+"\n"+model.description).toHtmlEscaped().replace('\n',"<br>")),Qt::ToolTipRole);
    }
    int index=m_model->findData(selected);
    if(index<0){
        m_model->addItem(selected.startsWith("custom-")?"Missing imported model":"Unavailable saved model",selected);
        index=m_model->count()-1;
        m_model->setItemData(index,QString("<qt>This saved model is not installed. Import the same file in Models… or choose another model.<br><br>Model ID: %1</qt>").arg(selected.toHtmlEscaped()),Qt::ToolTipRole);
    }
    m_model->setCurrentIndex(index);updateLanguages();
    int languageIndex=m_language->findData(language);
    if(languageIndex<0&&m_modelCatalog->model(selected).id.isEmpty()){
        m_language->addItem(QString("Saved language: %1").arg(language),language);languageIndex=m_language->count()-1;
    }
    m_language->setCurrentIndex(std::max(0,languageIndex));updateBusy();
}
void MainWindow::updateLanguages(){
    const QString previous=m_language->currentData().toString();QSignalBlocker b(m_language);m_language->clear();m_language->addItem("Auto language","auto");
    QList<QPair<QString,QString>> languages={{"English","en"},{"German","de"},{"French","fr"},{"Spanish","es"},{"Italian","it"},{"Dutch","nl"},{"Polish","pl"}};
    const ModelInfo model=m_modelCatalog->model(m_model->currentData().toString());
    const bool whistle=model.engine=="whistle",englishOnly=model.englishOnly;
    if(englishOnly)languages={{"English","en"}};
    else if(!whistle)languages.append({{"Hungarian","hu"},{"Portuguese","pt"},{"Russian","ru"},{"Ukrainian","uk"},{"Chinese","zh"},{"Japanese","ja"},{"Korean","ko"},{"Arabic","ar"},{"Czech","cs"},{"Slovak","sk"},{"Romanian","ro"},{"Turkish","tr"},{"Swedish","sv"},{"Danish","da"},{"Finnish","fi"},{"Norwegian","no"},{"Greek","el"},{"Hindi","hi"},{"Indonesian","id"},{"Vietnamese","vi"},{"Thai","th"},{"Hebrew","he"},{"Bulgarian","bg"},{"Croatian","hr"},{"Serbian","sr"}});
    for(const auto&l:languages)m_language->addItem(l.first,l.second);m_language->setCurrentIndex(std::max(0,m_language->findData(previous)));
    const QString unavailable=m_modelCatalog->availability(m_model->currentData().toString());
    if(model.id.isEmpty())m_modelHint->setText("This saved model is missing. Import it in Models… or choose another model before transcribing.");
    else if(!unavailable.isEmpty())m_modelHint->setText("The selected model is unavailable. Open Models… to inspect it or choose another model.");
    else m_modelHint->setText(whistle?"Whistle · recognized word timing · 7 languages · runs locally on your CPU":englishOnly?"Whisper · recognized segment timing · English only · runs locally on your CPU":"Whisper · recognized segment timing · multilingual · runs locally on your CPU");
    m_modelHint->setToolTip(QString("<qt>%1</qt>").arg((unavailable.isEmpty()?model.description:unavailable).toHtmlEscaped()));
}
void MainWindow::updateBusy(){
    const bool busy=m_audio->busy()||m_stt->busy();const QString modelId=m_model->currentData().toString();
    const bool knownModel=!m_modelCatalog->model(modelId).id.isEmpty(),available=knownModel&&m_modelCatalog->availability(modelId).isEmpty();
    m_cancel->setVisible(busy);m_transcribe->setEnabled(!busy&&m_mediaReady&&available);m_splitter->setEnabled(!m_audio->busy());m_model->setEnabled(!busy);m_language->setEnabled(!busy&&knownModel);
    // Prevent a new job from replacing audio while a recognizer owns that file.
    findChild<QPushButton*>("openMediaButton")->setEnabled(!busy);
}
void MainWindow::setStatus(const QString &s){m_status->setText(s);m_status->setToolTip(QString("<qt>%1</qt>").arg(s.toHtmlEscaped().replace('\n',"<br>")));}
void MainWindow::showError(const QString &s){setStatus(s);if(!m_testMode)QMessageBox::warning(this,"Subtitle Desk",s);}
void MainWindow::setDirty(bool d){m_dirty=d;QString name=m_projectPath.isEmpty()?m_doc.title:QFileInfo(m_projectPath).fileName();if(name.isEmpty())name="Untitled";setWindowTitle((m_dirty?"* ":"")+name+" — Subtitle Desk 1.1.0");}
void MainWindow::setDocument(const SubtitleDocument &doc,bool dirty){m_doc=doc;m_doc.normalizeIds();m_current=-1;m_undo.clear();m_redo.clear();setDirty(dirty);if(!m_doc.model.isEmpty())refreshModelChoices(m_doc.model,m_doc.language);refresh(false);}
void MainWindow::refresh(bool preserveSelection){
    QString selected;if(preserveSelection&&currentValid())selected=m_doc.cues[m_current].id;
    m_refreshing=true;QSignalBlocker blocker(m_table);m_table->setRowCount(m_doc.cues.size());m_lastActive.clear();int reviewed=0;
    for(int r=0;r<m_doc.cues.size();++r){const auto &c=m_doc.cues[r];reviewed+=c.reviewed;const QStringList cells={QString::number(r+1),compactTime(c.startMs),compactTime(c.endMs),QString(c.text).replace('\n',"  /  "),c.reviewed?"✓":""};
        for(int col=0;col<5;++col){auto*item=m_table->item(r,col);if(!item){item=new QTableWidgetItem;m_table->setItem(r,col,item);}item->setBackground(QBrush());item->setForeground(QBrush());QFont itemFont=item->font();itemFont.setBold(false);item->setFont(itemFont);item->setText(cells[col]);item->setToolTip(col==3?c.text:cells[col]);if(col!=3)item->setTextAlignment(Qt::AlignCenter);}
        if(!selected.isEmpty()&&c.id==selected)m_current=r;
    }
    m_summary->setText(QString("CUES · %1   /   %2 reviewed").arg(m_doc.cues.size()).arg(reviewed));
    m_table->setColumnWidth(1,m_doc.durationMs()>=3600000?112:94);m_table->setColumnWidth(2,m_doc.durationMs()>=3600000?112:94);
    if(m_doc.cues.isEmpty())m_current=-1;else m_current=std::clamp(m_current,0,int(m_doc.cues.size()-1));
    if(currentValid())m_table->setCurrentCell(m_current,3,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);
    m_refreshing=false;filterRows();loadEditor();updatePosition(m_demo?m_demoPosition:m_player->position());updateBusy();
}
void MainWindow::selectCue(int index,bool seek){if(index<0||index>=m_doc.cues.size())return;m_current=index;m_typing=false;m_refreshing=true;m_table->setCurrentCell(index,3,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);m_refreshing=false;loadEditor();if(seek)setPlaybackPosition(m_doc.cues[index].startMs);}
void MainWindow::loadEditor(){
    m_loadingEditor=true;const bool valid=currentValid();for(auto*w:QList<QWidget*>{m_text,m_start,m_end,m_review})w->setEnabled(valid);
    if(valid){const auto&c=m_doc.cues[m_current];m_cueTitle->setText(QString("CUE %1").arg(m_current+1));m_start->setText(displayTime(c.startMs));m_end->setText(displayTime(c.endMs));m_text->setPlainText(c.text);m_review->setText(c.reviewed?"Reviewed ✓":"Mark reviewed");m_wave->setCue(c.startMs,c.endMs);}
    else{m_cueTitle->setText("CUE EDITOR");m_start->clear();m_end->clear();m_text->clear();m_wave->setCue(-1,-1);m_review->setText("Mark reviewed");}
    m_loadingEditor=false;updateStatistics();
}
void MainWindow::updateStatistics(){
    if(!currentValid()){m_stats->setText("Select a cue to inspect its timing.");return;}
    const auto&c=m_doc.cues[m_current];const double sec=(c.endMs-c.startMs)/1000.0;const int count=c.text.size();const double cps=sec>0?count/sec:0;const auto lines=c.text.split('\n');int longest=0;for(const auto&l:lines)longest=std::max(longest,int(l.size()));
    m_stats->setText(QString("%1 s  ·  %2 chars  ·  %3 chars/s%4").arg(sec,0,'f',2).arg(count).arg(cps,0,'f',1).arg(cps>20?"\nFast reading pace — consider splitting.":longest>42?"\nLong line — consider a line break.":""));
}
void MainWindow::pushUndo(){m_typing=false;m_typingTimer.stop();m_undo.push_back(m_doc);if(m_undo.size()>50)m_undo.removeFirst();m_redo.clear();}
void MainWindow::undo(){if(m_undo.isEmpty())return;m_typing=false;m_redo.push_back(m_doc);m_doc=m_undo.takeLast();setDirty();refresh(false);setStatus("Undo complete.");}
void MainWindow::redo(){if(m_redo.isEmpty())return;m_typing=false;m_undo.push_back(m_doc);m_doc=m_redo.takeLast();setDirty();refresh(false);setStatus("Redo complete.");}
void MainWindow::changeText(){
    if(m_loadingEditor||!currentValid())return;const QString text=m_text->toPlainText();if(text==m_doc.cues[m_current].text)return;
    if(!m_typing){pushUndo();m_typing=true;}m_typingTimer.start();auto&c=m_doc.cues[m_current];c.text=text;c.reviewed=false;m_review->setText("Mark reviewed");
    if(auto*i=m_table->item(m_current,3)){i->setText(QString(text).replace('\n',"  /  "));i->setToolTip(text);}if(auto*i=m_table->item(m_current,4))i->setText("");
    int reviewed=0;for(const auto&cue:m_doc.cues)reviewed+=cue.reviewed;m_summary->setText(QString("CUES · %1   /   %2 reviewed").arg(m_doc.cues.size()).arg(reviewed));setDirty();updateStatistics();
}
void MainWindow::commitTime(bool isStart){
    if(m_loadingEditor||!currentValid())return;auto*edit=isStart?m_start:m_end;bool ok=false;const qint64 t=SubtitleDocument::parseTime(edit->text(),&ok);const auto&cue=m_doc.cues[m_current];
    if(!ok||(isStart?t>=cue.endMs:t<=cue.startMs)){setStatus("Use HH:MM:SS.mmm. The end must be later than the start.");QSignalBlocker b(edit);edit->setText(displayTime(isStart?cue.startMs:cue.endMs));return;}
    if(t==(isStart?cue.startMs:cue.endMs))return;pushUndo();auto&c=m_doc.cues[m_current];if(isStart)c.startMs=t;else c.endMs=t;c.reviewed=false;setDirty();refresh();
}
void MainWindow::setBoundary(bool isStart){if(!currentValid())return;auto*edit=isStart?m_start:m_end;edit->setText(displayTime(m_demo?m_demoPosition:m_player->position()));commitTime(isStart);}
QVector<int>MainWindow::selectedIndices()const{QVector<int>v;for(const auto&i:m_table->selectionModel()->selectedRows())v.push_back(i.row());std::sort(v.begin(),v.end());return v;}
void MainWindow::addCue(){pushUndo();Cue c;c.id=SubtitleDocument::newCueId();c.startMs=m_demo?m_demoPosition:m_player->position();c.endMs=c.startMs+2500;c.text="";m_doc.cues.push_back(c);m_doc.stableSort();for(int i=0;i<m_doc.cues.size();++i)if(m_doc.cues[i].id==c.id)m_current=i;setDirty();refresh(false);m_text->setFocus();setStatus("New cue added at the playhead. Enter its words and adjust its end.");}
void MainWindow::splitCue(){if(!currentValid())return;auto copy=m_doc;QString error;if(!copy.splitCue(m_current,m_text->textCursor().position(),m_demo?m_demoPosition:m_player->position(),&error)){showError(error);return;}pushUndo();m_doc=copy;setDirty();refresh(false);setStatus("Cue split. Check both text and timing boundaries.");}
void MainWindow::mergeCues(){auto rows=selectedIndices();if(rows.size()<2&&currentValid()&&m_current+1<m_doc.cues.size())rows={m_current,m_current+1};if(rows.size()<2){setStatus("Select two or more consecutive cues to merge.");return;}for(int i=1;i<rows.size();++i)if(rows[i]!=rows[i-1]+1){showError("Select a consecutive range of cues to merge.");return;}auto copy=m_doc;QString error;if(!copy.mergeCues(rows.first(),rows.last(),&error)){showError(error);return;}pushUndo();m_doc=copy;m_current=rows.first();setDirty();refresh(false);setStatus("Cues merged. Line breaks were preserved.");}
void MainWindow::deleteCues(){auto rows=selectedIndices();if(rows.isEmpty())return;if(rows.size()>1&&!m_testMode&&QMessageBox::question(this,"Delete cues",QString("Delete %1 selected cues? You can undo this change.").arg(rows.size()))!=QMessageBox::Yes)return;pushUndo();for(auto it=rows.rbegin();it!=rows.rend();++it)m_doc.cues.removeAt(*it);m_current=std::min(rows.first(),int(m_doc.cues.size()-1));setDirty();refresh(false);setStatus("Cue deletion can be undone with Ctrl+Z.");}
void MainWindow::setReviewed(){if(!currentValid())return;pushUndo();m_doc.cues[m_current].reviewed=!m_doc.cues[m_current].reviewed;setDirty();refresh();}
void MainWindow::sortCues(){if(m_doc.cues.isEmpty())return;QString id=currentValid()?m_doc.cues[m_current].id:QString();pushUndo();m_doc.stableSort();for(int i=0;i<m_doc.cues.size();++i)if(m_doc.cues[i].id==id)m_current=i;setDirty();refresh(false);setStatus("Cues sorted chronologically.");}
void MainWindow::shiftCues(){if(m_doc.cues.isEmpty())return;bool ok=false;const int ms=QInputDialog::getInt(this,"Shift cue timing","Offset in milliseconds (negative = earlier).\nApplies to selected cues, or all cues if none are selected.",0,-86400000,86400000,100,&ok);if(!ok||!ms)return;pushUndo();const auto moved=m_doc.shiftCues(ms,selectedIndices());setDirty();refresh();setStatus(QString("Shifted by %1 ms%2.").arg(moved).arg(moved!=ms?" (limited to keep starts at or after zero)":""));}
void MainWindow::filterRows(){const auto term=m_search->text().trimmed();for(int i=0;i<m_doc.cues.size();++i)m_table->setRowHidden(i,(!term.isEmpty()&&!m_doc.cues[i].text.contains(term,Qt::CaseInsensitive))||(m_unreviewed->isChecked()&&m_doc.cues[i].reviewed));}
void MainWindow::setPlaybackPosition(qint64 ms){if(m_demo){m_demoPosition=std::clamp(ms,qint64(0),m_doc.durationMs());updatePosition(m_demoPosition);return;}m_player->setPosition(std::clamp(ms,qint64(0),m_audio->durationMs()));updatePosition(m_player->position());}
void MainWindow::updatePosition(qint64 pos){
    const qint64 duration=m_demo?m_doc.durationMs():(m_mediaReady?m_audio->durationMs():0);auto shortTime=[](qint64 t){QString s=displayTime(t);return s.startsWith("00:")?s.mid(3):s;};
    m_clock->setText(shortTime(pos)+" / "+shortTime(duration));m_wave->setPosition(pos);
    if(m_loop->isChecked()&&currentValid()&&m_player->playbackState()==QMediaPlayer::PlayingState&&!m_loopJump){const auto &cue=m_doc.cues[m_current];if(pos>=cue.endMs&&cue.endMs<=duration){m_loopJump=true;m_player->setPosition(std::max(qint64(0),cue.startMs-150));m_loopJump=false;return;}}
    const auto active=m_doc.activeCuesAt(pos);m_active=active.isEmpty()?-1:active.first();
    if(active!=m_lastActive){QVector<int> changed=m_lastActive;for(int r:active)if(!changed.contains(r))changed.push_back(r);for(int r:changed){const bool playing=active.contains(r);for(int c=0;c<5;++c)if(auto*i=m_table->item(r,c)){i->setBackground(playing?QBrush(m_dark?QColor("#344b61"):QColor("#d7e7f5")):QBrush());i->setForeground(playing?QBrush(m_dark?QColor("#f1f6ff"):QColor("#102b42")):QBrush());auto f=i->font();f.setBold(playing);i->setFont(f);}}m_lastActive=active;}
    // Scrolling never moves the editor's selection or interrupts typing.
    if(m_follow->isChecked()&&m_active>=0&&!m_table->isRowHidden(m_active)&&m_player->playbackState()==QMediaPlayer::PlayingState&&!m_text->hasFocus())m_table->scrollToItem(m_table->item(m_active,3),QAbstractItemView::PositionAtCenter);
    m_table->setProperty("activeCue",m_active);
}
void MainWindow::playPause(){
    if(m_demo){setStatus("This visual demonstration has no audio. Open your own file to listen and transcribe.");return;}
    if(!m_mediaReady){setStatus("Open an audio or video file first.");return;}
    if(m_player->playbackState()==QMediaPlayer::PlayingState)m_player->pause();else{if(m_player->position()>=m_audio->durationMs()-30)m_player->setPosition(0);m_player->play();}
}
void MainWindow::replayCue(){if(!currentValid())return;setPlaybackPosition(std::max(qint64(0),m_doc.cues[m_current].startMs-150));if(!m_demo&&m_mediaReady)m_player->play();}
void MainWindow::navigate(int d){if(m_doc.cues.isEmpty())return;int target=m_current+d;while(target>=0&&target<m_doc.cues.size()&&m_table->isRowHidden(target))target+=d;if(target>=0&&target<m_doc.cues.size())selectCue(target,true);}
void MainWindow::openMediaDialog(){const QString p=QFileDialog::getOpenFileName(this,"Open audio or video",m_settings->value("lastFolder").toString(),mediaFilter());if(!p.isEmpty())openMedia(p);}
void MainWindow::openProjectDialog(){const QString p=QFileDialog::getOpenFileName(this,"Open project",m_settings->value("lastFolder").toString(),projectFilter());if(!p.isEmpty())openProject(p);}
void MainWindow::openPath(const QString &path){m_explicitWork=true;const QString ext=QFileInfo(path).suffix().toLower();if(QStringList{"sdesk","json","sdproj"}.contains(ext))openProject(path);else if(QStringList{"srt","vtt","ass","ssa","lrc"}.contains(ext)){
    if(m_audio->busy()||m_stt->busy()){setStatus("Finish or cancel the current job first.");return;}
    if(!maybeSave())return;auto copy=m_doc;QString error;if(!copy.load(path,&error)){showError(error);return;}pushUndo();m_doc=copy;m_current=-1;setDirty();refresh(false);setStatus("Imported timed text. Link audio from the File menu if needed.");
}else openMedia(path);}
void MainWindow::openMedia(const QString &path,bool keepCues){
    m_explicitWork=true;
    if(m_audio->busy()||m_stt->busy()){setStatus("Finish or cancel the current job first.");return;}
    if(!keepCues&&!maybeSave())return;m_player->pause();m_pending=keepCues?m_doc:SubtitleDocument();m_pending->mediaPath=QFileInfo(path).absoluteFilePath();if(!keepCues)m_pending->title=QFileInfo(path).completeBaseName();
    m_pendingProjectPath=keepCues?m_projectPath:QString();m_pendingDirty=keepCues;m_settings->setValue("lastFolder",QFileInfo(path).absolutePath());m_audio->open(path);updateBusy();
}
void MainWindow::openProject(const QString &path){
    m_explicitWork=true;
    if(m_audio->busy()||m_stt->busy()){setStatus("Finish or cancel the current job first.");return;}
    SubtitleDocument loaded;QString error;if(!loaded.load(path,&error)){showError(error);return;}if(!maybeSave())return;
    if(!loaded.mediaPath.isEmpty()&&QFileInfo(loaded.mediaPath).isRelative())loaded.mediaPath=QFileInfo(path).dir().absoluteFilePath(loaded.mediaPath);
    if(QFileInfo(loaded.mediaPath).isFile()){
        m_pending=loaded;m_pendingProjectPath=QFileInfo(path).absoluteFilePath();m_pendingDirty=false;m_player->pause();m_audio->open(loaded.mediaPath);updateBusy();
    }else{
        m_player->stop();m_player->setSource({});m_projectPath=QFileInfo(path).absoluteFilePath();m_demo=false;m_mediaReady=false;setDocument(loaded,false);m_sourceLabel->setText(loaded.mediaPath.isEmpty()?"No media linked":QFileInfo(loaded.mediaPath).fileName()+" · missing");m_sourceDetail->setText("LINK AUDIO FROM FILE MENU");m_wave->setAudio({},0);setStatus("Project opened. Its audio is missing; use File → Link / replace audio to locate it.");
    }
    if(!loaded.model.isEmpty())refreshModelChoices(loaded.model,loaded.language);
}
void MainWindow::importSubtitles(){
    if(m_audio->busy()||m_stt->busy()){setStatus("Finish or cancel the current job first.");return;}
    const QString p=QFileDialog::getOpenFileName(this,"Import subtitles or lyrics",{},importFilter());if(p.isEmpty())return;
    auto copy=m_doc;QString error;if(!copy.load(p,&error)){showError(error);return;}
    if(!m_doc.cues.isEmpty()&&QMessageBox::question(this,"Replace cues","Replace the current cues with the imported file? This can be undone.")!=QMessageBox::Yes)return;
    pushUndo();m_doc=copy;m_current=-1;setDirty();refresh(false);setStatus("Imported cues. Check end times on LRC files: that format stores starts only.");
}
bool MainWindow::saveProject(bool saveAs){
    if(!commitPendingTimes())return false;
    QString path=m_projectPath;if(path.isEmpty()||saveAs){path=QFileDialog::getSaveFileName(this,"Save editable project",path.isEmpty()?(m_doc.title.isEmpty()?"Untitled":m_doc.title)+".sdesk":path,projectFilter());if(path.isEmpty())return false;if(QFileInfo(path).suffix().isEmpty())path+=".sdesk";}
    auto saved=m_doc;saved.model=m_model->currentData().toString();saved.language=m_language->currentData().toString();
    QString error;if(!saved.saveProject(path,&error)){showError(error);return false;}m_doc.model=saved.model;m_doc.language=saved.language;m_projectPath=QFileInfo(path).absoluteFilePath();setDirty(false);QFile::remove(m_configDir+"/recovery.sdesk");setStatus("Project saved. Audio stays in its original file; keep it with the project.");return true;
}
bool MainWindow::maybeSave(){
    if(!commitPendingTimes())return false;
    if(!m_dirty)return true;if(m_testMode)return true;
    const auto answer=QMessageBox::warning(this,"Unsaved project","Save your subtitle edits before continuing?",QMessageBox::Save|QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Save);
    if(answer==QMessageBox::Cancel)return false;if(answer==QMessageBox::Save)return saveProject();QFile::remove(m_configDir+"/recovery.sdesk");return true;
}
void MainWindow::exportSubtitles(){
    if(!commitPendingTimes())return;
    if(m_doc.cues.isEmpty()){showError("There are no cues to export yet. Transcribe audio or add/import cues first.");return;}
    const auto errors=m_doc.validationErrors();if(!errors.isEmpty()){showError("Fix these timing errors before export:\n\n"+errors.mid(0,15).join('\n'));return;}
    QDialog d(this);d.setWindowTitle("Export subtitles and lyrics");d.setMinimumWidth(470);auto*l=new QVBoxLayout(&d);auto*intro=label("Choose the format for your player, editor, or lyrics file.");intro->setWordWrap(true);l->addWidget(intro);
    auto*format=new QComboBox;format->setObjectName("exportFormatCombo");for(const auto&p:QList<QPair<QString,QString>>{{"SubRip subtitles (.srt)","srt"},{"WebVTT subtitles (.vtt)","vtt"},{"Advanced SubStation Alpha (.ass)","ass"},{"Synchronized lyrics (.lrc)","lrc"},{"Plain transcript (.txt)","txt"},{"Spreadsheet / timing table (.csv)","csv"},{"Editable JSON project (.json)","json"}})format->addItem(p.first,p.second);l->addWidget(format);
    auto*note=label("");note->setWordWrap(true);l->addWidget(note);
    auto describe=[=]{const auto f=format->currentData().toString();note->setText(f=="lrc"?"LRC stores each line's start time, not its end. Use SRT or the project format to keep both boundaries. This export is line-timed, not word-level karaoke.":f=="txt"?"Plain text contains words and line breaks only. Timing and review markers are kept in your project.":f=="json"?"JSON retains exact millisecond timing, review status, model, language and the linked audio path. Audio itself is not embedded.":f=="ass"?"ASS uses centisecond precision and a simple neutral subtitle style.":"UTF-8 text. Cue order is chronological. Millisecond timing is retained in SRT, VTT and CSV.");};connect(format,QOverload<int>::of(&QComboBox::currentIndexChanged),&d,describe);describe();
    const auto warnings=m_doc.timingWarnings();if(!warnings.isEmpty()){auto*w=label(QString("%1 review warnings (overlaps, empty text or reading pace). Use Edit → Check timing to inspect them.").arg(warnings.size()));w->setWordWrap(true);l->addWidget(w);}
    auto*bb=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);bb->button(QDialogButtonBox::Save)->setText("Choose file…");l->addWidget(bb);connect(bb,&QDialogButtonBox::accepted,&d,&QDialog::accept);connect(bb,&QDialogButtonBox::rejected,&d,&QDialog::reject);if(d.exec()!=QDialog::Accepted)return;
    const QString f=format->currentData().toString();QString file=QFileDialog::getSaveFileName(this,"Export "+f.toUpper(),(m_doc.title.isEmpty()?"subtitles":m_doc.title)+"."+f,f.toUpper()+" (*."+f+")");if(file.isEmpty())return;if(QFileInfo(file).suffix().isEmpty())file+="."+f;
    QString error;if(!m_doc.exportFile(file,f,&error)){showError(error);return;}setStatus("Exported "+QFileInfo(file).fileName()+". Save the project to retain your review state.");
}
void MainWindow::transcribe(){
    if(!m_mediaReady||m_audio->busy()||m_stt->busy())return;
    const QString unavailable=m_modelCatalog->availability(m_model->currentData().toString());if(!unavailable.isEmpty()){showError(unavailable+" Open Models… to import the model or select another installed model.");return;}
    if(!m_doc.cues.isEmpty()&&!m_testMode&&QMessageBox::question(this,"Transcribe again","Create a new transcription for this audio? Current cues will be replaced only after recognition succeeds; the replacement can be undone.")!=QMessageBox::Yes)return;
    m_player->pause();m_stt->start(m_audio->wavPath(),m_model->currentData().toString(),m_language->currentData().toString(),m_threads,42);updateBusy();
}
void MainWindow::cancelWork(){if(m_stt->busy())m_stt->cancel();if(m_audio->busy())m_audio->cancel();updateBusy();}
void MainWindow::showTimingCheck(){
    QStringList lines=m_doc.validationErrors();lines.append(m_doc.timingWarnings());const qint64 duration=m_audio->durationMs();if(duration>0)for(int i=0;i<m_doc.cues.size();++i)if(m_doc.cues[i].endMs>duration)lines<<QString("Cue %1 extends past the audio's end.").arg(i+1);
    QDialog d(this);d.setWindowTitle("Timing and readability");d.resize(590,390);auto*l=new QVBoxLayout(&d);l->addWidget(label(QString("%1 cues · %2 findings").arg(m_doc.cues.size()).arg(lines.size()),"sectionTitle"));auto*text=new QPlainTextEdit;text->setReadOnly(true);text->setPlainText(lines.isEmpty()?"No timing or readability warnings were found. Listen through the transcript to verify the recognized words.":lines.join("\n\n"));l->addWidget(text);auto*b=new QDialogButtonBox(QDialogButtonBox::Close);connect(b,&QDialogButtonBox::rejected,&d,&QDialog::reject);l->addWidget(b);d.exec();
}
void MainWindow::showModels(QWidget *dialogParent){
    ModelManagerDialog dialog(m_modelCatalog,m_configDir+"/models",m_model->currentData().toString(),dialogParent?dialogParent:this);
    auto updateLock=[this,&dialog]{dialog.setRecognitionActive(m_audio->busy()||m_stt->busy());};
    updateLock();
    connect(m_audio,&AudioSession::progress,&dialog,updateLock);
    connect(m_audio,&AudioSession::ready,&dialog,updateLock);
    connect(m_audio,&AudioSession::failed,&dialog,updateLock);
    connect(m_audio,&AudioSession::cancelled,&dialog,updateLock);
    connect(m_stt,&TranscriptionService::progress,&dialog,updateLock);
    connect(m_stt,&TranscriptionService::completed,&dialog,updateLock);
    connect(m_stt,&TranscriptionService::failed,&dialog,updateLock);
    connect(m_stt,&TranscriptionService::cancelled,&dialog,updateLock);
    connect(&dialog,&ModelManagerDialog::modelChosen,this,[this](const QString &id){
        if(m_audio->busy()||m_stt->busy())return;
        refreshModelChoices(id);saveSettings();setStatus(QString("Selected %1 for transcription. Existing subtitle cues were kept.").arg(m_modelCatalog->model(id).name));
    });
    dialog.exec();
}
void MainWindow::showSettings(){
    QDialog d(this);d.setWindowTitle("Subtitle Desk settings");d.setMinimumWidth(430);auto*l=new QVBoxLayout(&d);auto*f=new QFormLayout;auto*theme=new QComboBox;theme->addItem("Follow system","system");theme->addItem("Light","light");theme->addItem("Dark","dark");theme->setCurrentIndex(std::max(0,theme->findData(m_theme)));f->addRow("Appearance",theme);
    auto*threads=new QSpinBox;threads->setRange(1,32);threads->setValue(m_threads);f->addRow("Recognition CPU threads",threads);l->addLayout(f);
    QStringList modelLines;for(const auto&p:QList<QPair<QString,QString>>{{"Whistle","whistle.cact"},{"Whisper Tiny","ggml-tiny-q5_1.bin"},{"Whisper Base","ggml-base-q5_1.bin"},{"Whisper Small","ggml-small-q5_1.bin"}}){QFileInfo info(resourceRoot()+"/models/"+p.second);modelLines<<p.first+": "+(info.exists()?QString::number(info.size()/1000000.0,'f',1)+" MB · installed":"not found");}
    auto*models=label("BUNDLED OFFLINE MODELS\n\n"+modelLines.join('\n'));models->setTextInteractionFlags(Qt::TextSelectableByMouse);l->addWidget(models);
    auto*modelRow=new QHBoxLayout;auto*installed=label("");installed->setObjectName("settingsModelSummary");modelRow->addWidget(installed,1);
    auto*manage=button("Models…","settingsModelsButton");modelRow->addWidget(manage);l->addLayout(modelRow);
    auto updateCount=[this,installed]{int imported=0;for(const auto &model:m_modelCatalog->models())if(!model.bundled)++imported;installed->setText(QString("%1 imported model%2").arg(imported).arg(imported==1?"":"s"));};updateCount();
    connect(m_modelCatalog,&ModelCatalog::modelsChanged,&d,updateCount);connect(manage,&QPushButton::clicked,&d,[this,&d]{showModels(&d);});
    auto*note=label("Use Models… to add compatible Whisper GGML (.bin) or Whistle (.cact) files. The four bundled models are kept.\n\nAll recognition is local. The application does not download models or use accounts or API keys.");note->setWordWrap(true);l->addWidget(note);
    auto*bb=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);connect(bb,&QDialogButtonBox::accepted,&d,&QDialog::accept);connect(bb,&QDialogButtonBox::rejected,&d,&QDialog::reject);l->addWidget(bb);if(d.exec()==QDialog::Accepted){m_threads=threads->value();setTheme(theme->currentData().toString());saveSettings();}
}
void MainWindow::showAbout(){QMessageBox::about(this,"About Subtitle Desk","<h2>Subtitle Desk 1.1.0</h2><p>A focused offline subtitle and lyrics editor.</p><p>Native C++ / Qt 6. Four bundled recognition models: Cactus Whistle, Whisper Tiny, Base and Small. Compatible Whisper GGML and Whistle model files can also be imported locally.</p><p>Copyright &copy; 2026 davoda1.</p><p>Application source: GNU Affero General Public License, version 3 only (AGPL-3.0-only). Recognition engines, models, Qt and multimedia components retain their own licenses; see the included licenses folder.</p><p>Audio-only processing. No accounts, telemetry or cloud transcription.</p>");}
void MainWindow::showShortcuts(){
    QDialog d(this);d.setWindowTitle("Quick guide");d.resize(650,540);auto*l=new QVBoxLayout(&d);auto*text=new QPlainTextEdit;text->setReadOnly(true);text->setPlainText(
        "1. OPEN\nOpen media or drop a local audio/video file into the window. The first audio stream is decoded locally; no video is shown. The review track is 16 kHz mono PCM, the same audio used by recognition. The original file is unchanged.\n\n"
        "2. TRANSCRIBE\nChoose a model and language. Whisper Base is a balanced starting point. Small is larger and slower. Whistle groups recognized word timings into cues and supports seven languages; multilingual Whisper models support more. Singing, overlapping vocals, music and effects can reduce recognition accuracy.\n\n"
        "MODELS\nOpen Models… beside the model selector or in the View menu. Import a compatible raw Whisper GGML .bin or Whistle .cact file. Imported files are copied into your settings folder and can be removed from the manager; the four bundled models are kept. English-only Whisper models offer Auto and English. Projects keep a model ID, so a missing imported model must be installed again or another model selected before transcription. Archives and model executables are not imported.\n\n"
        "3. REVIEW\nBlue rows show cues active at the playhead. Selecting a cue controls the editor; playback does not replace your editing selection. Double-click a row to replay it. Click the waveform to seek. Loop cue repeats the selected segment. Change playback speed for difficult passages.\n\n"
        "4. EDIT\nEdit words and line breaks on the right. Enter times as HH:MM:SS.mmm or MM:SS.mmm. Set start/end from the playhead. Split uses the text cursor and the playhead when inside the cue, otherwise the cue midpoint. Merge joins consecutive cues and preserves line breaks. Ctrl+click or Shift+click selects multiple rows. Use To review to hide checked cues.\n\n"
        "5. SAVE / EXPORT\nSave a .sdesk project to preserve text, exact times, review marks and the media link. Audio is not embedded. Export SRT, VTT, ASS, LRC, TXT, CSV or JSON. LRC stores start times only; word-level karaoke is not inferred. Use File → Link / replace audio after moving your media.\n\n"
        "SHORTCUTS\nCtrl+O: open media\nCtrl+Shift+O: open project\nCtrl+I: import timed text\nCtrl+S: save project\nCtrl+E: export\nCtrl+Space: play/pause (Space remains available when typing)\nCtrl+P: replay cue\nAlt+Up / Alt+Down: previous / next cue\nCtrl+[ / Ctrl+]: set start / end at playhead\nCtrl+Return: add cue\nCtrl+K: split cue\nCtrl+M: merge selected cues (or the next cue)\nCtrl+Delete: delete selected cues\nCtrl+R: mark reviewed\nCtrl+Z / Ctrl+Shift+Z: undo / redo\nCtrl+Shift+T: shift timing\nCtrl+Shift+C: timing checks\n\n"
        "RECOVERY\nUnsaved changes are saved to a recovery file every 20 seconds. On the next launch you can restore them. Save a project before moving between computers; recovery is not a backup of your audio.\n");l->addWidget(text);auto*b=new QDialogButtonBox(QDialogButtonBox::Close);connect(b,&QDialogButtonBox::rejected,&d,&QDialog::reject);l->addWidget(b);d.exec();
}
void MainWindow::saveSettings(){if(m_testMode)return;m_settings->setValue("theme",m_theme);m_settings->setValue("threads",m_threads);m_settings->setValue("model",m_model->currentData());m_settings->setValue("language",m_language->currentData());m_settings->setValue("volume",m_volume->value());m_settings->setValue("geometry",saveGeometry());m_settings->setValue("splitter",m_splitter->saveState());m_settings->sync();}
void MainWindow::autosave(){if(m_audio->busy()||m_start->hasFocus()||m_end->hasFocus())return;if(m_testMode||!m_dirty)return;auto recovery=m_doc;recovery.model=m_model->currentData().toString();recovery.language=m_language->currentData().toString();QString error;if(!recovery.saveProject(m_configDir+"/recovery.sdesk",&error))setStatus("Recovery save failed: "+error+" Save your project manually.");}
void MainWindow::offerRecovery(){if(m_explicitWork||m_dirty||m_audio->busy()||m_stt->busy()||!m_doc.cues.isEmpty())return;const QString path=m_configDir+"/recovery.sdesk";if(!QFileInfo::exists(path))return;SubtitleDocument recovery;QString error;if(!recovery.load(path,&error))return;const auto answer=QMessageBox::question(this,"Recover unsaved project","An unsaved subtitle project was found. Recover its edits?",QMessageBox::Yes|QMessageBox::No,QMessageBox::Yes);if(answer==QMessageBox::No){QFile::remove(path);return;}if(QFileInfo(recovery.mediaPath).isFile()){m_pending=recovery;m_pendingProjectPath.clear();m_pendingDirty=true;m_audio->open(recovery.mediaPath);}else{m_projectPath.clear();setDocument(recovery,true);setStatus("Recovered edits. Link the audio from the File menu if needed.");}}
void MainWindow::closeEvent(QCloseEvent *event){
    if((m_audio->busy()||m_stt->busy()||m_modelCatalog->busy())&&!m_testMode&&QMessageBox::question(this,"Work in progress","Cancel the current job and close Subtitle Desk?",QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes){event->ignore();return;}
    if(!maybeSave()){event->ignore();return;}cancelWork();m_modelCatalog->cancelImport();saveSettings();if(!m_dirty)QFile::remove(m_configDir+"/recovery.sdesk");event->accept();
}
void MainWindow::dragEnterEvent(QDragEnterEvent*e){if(e->mimeData()->hasUrls()&&e->mimeData()->urls().size()==1&&e->mimeData()->urls().first().isLocalFile())e->acceptProposedAction();}
void MainWindow::dropEvent(QDropEvent*e){if(e->mimeData()->hasUrls()&&e->mimeData()->urls().size()==1&&e->mimeData()->urls().first().isLocalFile()){openPath(e->mimeData()->urls().first().toLocalFile());e->acceptProposedAction();}}
void MainWindow::loadDemo(){
    if(m_audio->busy()||m_stt->busy()){setStatus("Finish or cancel the current job before opening the demonstration.");return;}m_explicitWork=true;
    m_player->stop();m_player->setSource({});m_projectPath.clear();m_demo=true;m_mediaReady=false;m_demoPosition=14320;SubtitleDocument d;d.title="A quiet morning · demonstration";d.model="base";d.language="en";
    const QStringList words={"The city is still asleep.","Beyond the window, the first light\nfinds its way across the room.","Some days begin with a question.","Today, we begin by listening.","Every voice leaves a little space\nfor the next one to arrive.","A breath. A pause. A new beginning.","Keep the words that matter.","And let the silence have its time."};
    const QVector<qint64> starts={800,4100,9400,13000,17800,23900,29500,34300};const QVector<qint64> ends={3500,8500,12400,16400,22400,28100,32800,38800};
    for(int i=0;i<words.size();++i){Cue c;c.id=SubtitleDocument::newCueId();c.startMs=starts[i];c.endMs=ends[i];c.text=words[i];c.reviewed=i<3;d.cues.push_back(c);}setDocument(d,false);m_current=3;
    QVector<float> peaks(1940);for(int i=0;i<peaks.size();++i){const int cue=d.cueAt(i*20);peaks[i]=cue>=0?float(0.08+0.5*std::abs(std::sin(i*0.09)*std::cos(i*0.17))):0.005f;}
    m_wave->setAudio(peaks,38800);m_sourceLabel->setText("A quiet morning.wav  ·  visual demonstration");m_sourceDetail->setText("DEMO · 00:00:38");refresh(false);selectCue(3);updatePosition(m_demoPosition);setStatus("Demonstration data · open your own audio or video to begin a real transcription.");
}
void MainWindow::setTheme(const QString &theme){
    m_theme=theme;m_dark=theme=="dark"||(theme=="system"&&qApp->styleHints()->colorScheme()==Qt::ColorScheme::Dark);qApp->setStyle("Fusion");
    const QString bg=m_dark?"#14161a":"#eef0f3",panel=m_dark?"#1d2026":"#ffffff",input=m_dark?"#15181d":"#f8f9fb",fg=m_dark?"#e4e7ec":"#202733",muted=m_dark?"#9da6b5":"#646e7d",border=m_dark?"#353b46":"#d7dce3",buttonBg=m_dark?"#2b3039":"#f0f2f5",hover=m_dark?"#383f4b":"#e2e7ed",accent=m_dark?"#cad8e9":"#324b66",accentText=m_dark?"#18202b":"#ffffff";
    QPalette pal;pal.setColor(QPalette::Window,QColor(bg));pal.setColor(QPalette::Base,QColor(input));pal.setColor(QPalette::AlternateBase,QColor(panel));pal.setColor(QPalette::WindowText,QColor(fg));pal.setColor(QPalette::Text,QColor(fg));pal.setColor(QPalette::Button,QColor(buttonBg));pal.setColor(QPalette::ButtonText,QColor(fg));pal.setColor(QPalette::Highlight,QColor(m_dark?"#41566e":"#c3d7e9"));pal.setColor(QPalette::HighlightedText,QColor(fg));pal.setColor(QPalette::PlaceholderText,QColor(muted));pal.setColor(QPalette::ToolTipBase,QColor(panel));pal.setColor(QPalette::ToolTipText,QColor(fg));pal.setColor(QPalette::Disabled,QPalette::Text,QColor(muted));pal.setColor(QPalette::Disabled,QPalette::ButtonText,QColor(muted));qApp->setPalette(pal);
    QString sheet=QString(R"(
        QMainWindow, QDialog { background:%1; color:%4; }
        QWidget { font-family:'Segoe UI','DejaVu Sans',sans-serif; font-size:12px; }
        QLabel { color:%4; background:transparent; }
        QLabel#brand { font-size:18px; font-weight:700; letter-spacing:2px; }
        QLabel#muted, QLabel#modelHint, QLabel#statusText { color:%5; font-size:11px; }
        QLabel#eyebrow { color:%5; font-size:10px; font-weight:600; }
        QLabel#sectionTitle { font-size:11px; font-weight:700; color:%5; }
        QLabel#sourceName { font-weight:600; font-size:13px; }
        QLabel#clock { font-family:'DejaVu Sans Mono',monospace; font-size:11px; color:%4; }
        QFrame#panel { background:%2; border:1px solid %6; border-radius:9px; }
        QPushButton { background:%7; color:%4; border:1px solid %6; border-radius:5px; padding:5px 10px; min-height:18px; }
        QPushButton:hover { background:%8; }
        QPushButton:pressed { background:%6; }
        QPushButton:focus { border-color:%9; }
        QPushButton:disabled { color:%5; }
        QPushButton[primary="true"] { background:%9; color:%10; border-color:%9; font-weight:600; }
        QPushButton[primary="true"]:disabled { background:%7; color:%5; border-color:%6; }
        QLineEdit, QComboBox, QSpinBox { background:%3; color:%4; border:1px solid %6; border-radius:5px; padding:5px 7px; min-height:18px; }
        QLineEdit:focus, QPlainTextEdit:focus { border-color:%9; }
        QComboBox::drop-down { border:0; width:19px; }
        QComboBox QAbstractItemView { background:%2; color:%4; selection-background-color:%8; selection-color:%4; }
        QPlainTextEdit { background:%3; color:%4; border:1px solid %6; border-radius:6px; padding:7px; font-size:14px; }
        QTableWidget { background:%3; alternate-background-color:%2; color:%4; border:1px solid %6; border-radius:6px; }
        QTableWidget::item { padding:5px; border-bottom:1px solid %6; }
        QTableWidget::item:selected { border-top:1px solid %9; border-bottom:1px solid %9; }
        QHeaderView::section { background:%2; color:%5; border:0; border-bottom:1px solid %6; padding:6px 3px; font-size:10px; }
        QCheckBox { color:%5; spacing:5px; font-size:11px; }
        QMenuBar, QMenu { background:%2; color:%4; }
        QMenuBar::item:selected, QMenu::item:selected { background:%8; }
        QMenu::item { padding:6px 22px; }
        QSplitter::handle { background:%1; width:10px; }
        QProgressBar { background:%3; border:1px solid %6; border-radius:4px; text-align:center; color:%4; font-size:9px; }
        QProgressBar::chunk { background:%9; border-radius:3px; }
        QScrollBar:vertical { background:%3; width:10px; margin:0; }
        QScrollBar::handle:vertical { background:%6; min-height:24px; border-radius:4px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }
        QToolTip { background:%2; color:%4; border:1px solid %6; }
    )").arg(bg,panel,input,fg,muted,border,buttonBg,hover,accent,accentText);
    sheet+=QString("QComboBox::down-arrow { image:url(:/chevron-%1.png); width:12px; height:8px; }").arg(m_dark?"dark":"light");qApp->setStyleSheet(sheet);
    if(m_wave)m_wave->setDark(m_dark);if(m_table)m_lastActive={-1};if(m_table)updatePosition(m_demo?m_demoPosition:m_player->position());
}

void MainWindow::resizeEvent(QResizeEvent *event){
    QMainWindow::resizeEvent(event);if(!m_table)return;
    const bool compact=height()<670;
    findChild<QWidget*>("headingWidget")->setVisible(!compact);
    m_modelHint->setVisible(!compact);
    m_wave->setMinimumHeight(compact?40:72);m_wave->setMaximumHeight(compact?48:100);
    m_table->setColumnHidden(2,width()<910);
    m_table->setColumnWidth(1,m_doc.durationMs()>=3600000?112:94);
    m_table->setColumnWidth(2,m_doc.durationMs()>=3600000?112:94);
    m_table->setColumnWidth(0,width()<800?28:36);
    m_table->verticalHeader()->setDefaultSectionSize(compact?32:42);
    m_summary->setVisible(width()>=800);
    m_volume->setVisible(width()>=800);
    m_zoom->setVisible(width()>=800);
    findChild<QPushButton*>("startEditSet")->setVisible(height()>=580);
    findChild<QPushButton*>("endEditSet")->setVisible(height()>=580);
    m_splitter->setHandleWidth(width()<800?6:10);
    if(auto *l=qobject_cast<QVBoxLayout*>(centralWidget()->layout())){l->setContentsMargins(compact?10:18,compact?6:12,compact?10:18,6);l->setSpacing(compact?6:10);}
}

bool MainWindow::commitPendingTimes(){
    if(m_loadingEditor||!currentValid())return true;
    bool startOk=false,endOk=false;const qint64 start=SubtitleDocument::parseTime(m_start->text(),&startOk),end=SubtitleDocument::parseTime(m_end->text(),&endOk);
    if(!startOk||!endOk||end<=start){setStatus("Use HH:MM:SS.mmm. The end must be later than the start.");return false;}
    auto &cue=m_doc.cues[m_current];if(start==cue.startMs&&end==cue.endMs)return true;
    pushUndo();auto &changedCue=m_doc.cues[m_current];changedCue.startMs=start;changedCue.endMs=end;changedCue.reviewed=false;setDirty();
    m_table->item(m_current,1)->setText(compactTime(start));m_table->item(m_current,2)->setText(compactTime(end));m_table->item(m_current,4)->setText("");m_review->setText("Mark reviewed");m_wave->setCue(start,end);updateStatistics();return true;
}

bool MainWindow::eventFilter(QObject *watched,QEvent *event){
    if(watched==m_text&&(event->type()==QEvent::ShortcutOverride||event->type()==QEvent::KeyPress)){
        auto *key=static_cast<QKeyEvent*>(event);const auto mods=key->modifiers()&(Qt::ControlModifier|Qt::ShiftModifier|Qt::AltModifier|Qt::MetaModifier);
        const bool ctrl=mods==Qt::ControlModifier,redoKey=(mods==(Qt::ControlModifier|Qt::ShiftModifier)&&key->key()==Qt::Key_Z)||(ctrl&&key->key()==Qt::Key_Y);
        const bool appKey=redoKey||(ctrl&&QList<int>{Qt::Key_Z,Qt::Key_K,Qt::Key_M,Qt::Key_R,Qt::Key_P,Qt::Key_S,Qt::Key_E,Qt::Key_Space,Qt::Key_Return,Qt::Key_Enter,Qt::Key_Delete,Qt::Key_BracketLeft,Qt::Key_BracketRight}.contains(key->key()))||(mods==Qt::AltModifier&&(key->key()==Qt::Key_Up||key->key()==Qt::Key_Down));
        if(appKey){key->accept();if(event->type()==QEvent::KeyPress){
            if(redoKey)redo();else if(mods==Qt::AltModifier)navigate(key->key()==Qt::Key_Up?-1:1);else switch(key->key()){
                case Qt::Key_Z:undo();break;case Qt::Key_K:splitCue();break;case Qt::Key_M:mergeCues();break;case Qt::Key_R:setReviewed();break;case Qt::Key_P:replayCue();break;case Qt::Key_S:saveProject();break;case Qt::Key_E:exportSubtitles();break;case Qt::Key_Space:playPause();break;case Qt::Key_Return:case Qt::Key_Enter:addCue();break;case Qt::Key_Delete:deleteCues();break;case Qt::Key_BracketLeft:setBoundary(true);break;case Qt::Key_BracketRight:setBoundary(false);break;default:break;
            }
        }return true;}
    }
    return QMainWindow::eventFilter(watched,event);
}
