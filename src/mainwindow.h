#pragma once
#include "subtitle_document.h"
#include <QMainWindow>
#include <QTimer>
#include <optional>

class AudioSession;
class TranscriptionService;
class ModelCatalog;
class Waveform;
class QAudioOutput;
class QMediaPlayer;
class QTableWidget;
class QPlainTextEdit;
class QLineEdit;
class QLabel;
class QPushButton;
class QComboBox;
class QCheckBox;
class QProgressBar;
class QSplitter;
class QSettings;
class QSlider;

class MainWindow final:public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(bool testMode=false,QWidget *parent=nullptr);
    ~MainWindow() override;
    void openPath(const QString &path);
    void loadDemo();
    void setTheme(const QString &theme);
    const SubtitleDocument &document()const{return m_doc;}
    void setDocument(const SubtitleDocument &doc,bool dirty=true);
    void setPlaybackPosition(qint64 ms);
    bool isDirty()const{return m_dirty;}
    QString settingsDirectory()const{return m_configDir;}
    ModelCatalog *modelCatalog()const{return m_modelCatalog;}
protected:
    void closeEvent(QCloseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    bool eventFilter(QObject *,QEvent *) override;
private:
    void buildUi();
    void buildMenus();
    void connectActions();
    void refresh(bool preserveSelection=true);
    void selectCue(int index,bool seek=false);
    void loadEditor();
    void commitTime(bool start);
    bool commitPendingTimes();
    void setBoundary(bool start);
    void updateStatistics();
    void updatePosition(qint64 position);
    void updateBusy();
    void updateLanguages();
    void refreshModelChoices(const QString &preferredId=QString(),const QString &preferredLanguage=QString());
    void changeText();
    void pushUndo();
    void undo();
    void redo();
    void addCue();
    void splitCue();
    void mergeCues();
    void deleteCues();
    void shiftCues();
    void setReviewed();
    void sortCues();
    QVector<int> selectedIndices()const;
    void openMediaDialog();
    void openProjectDialog();
    void importSubtitles();
    void openMedia(const QString &path,bool keepCues=false);
    void openProject(const QString &path);
    bool saveProject(bool saveAs=false);
    bool maybeSave();
    void exportSubtitles();
    void transcribe();
    void cancelWork();
    void playPause();
    void replayCue();
    void navigate(int direction);
    void showTimingCheck();
    void showSettings();
    void showModels(QWidget *dialogParent=nullptr);
    void showAbout();
    void showShortcuts();
    void showError(const QString &message);
    void setDirty(bool dirty=true);
    void saveSettings();
    void autosave();
    void offerRecovery();
    void filterRows();
    void setStatus(const QString &message);
    QString resourceRoot()const;
    bool currentValid()const{return m_current>=0&&m_current<m_doc.cues.size();}

    SubtitleDocument m_doc;
    QVector<SubtitleDocument> m_undo,m_redo;
    std::optional<SubtitleDocument> m_pending;
    QString m_projectPath,m_pendingProjectPath,m_configDir,m_theme="system";
    int m_current=-1,m_active=-1,m_threads=4;
    bool m_dirty=false,m_loadingEditor=false,m_refreshing=false,m_typing=false,m_testMode=false;
    bool m_dark=true,m_demo=false,m_pendingDirty=false,m_loopJump=false,m_mediaReady=false,m_explicitWork=false;
    QVector<int> m_lastActive;
    qint64 m_demoPosition=0;
    QTimer m_typingTimer,m_autosaveTimer,m_playTimer;
    AudioSession *m_audio=nullptr;
    TranscriptionService *m_stt=nullptr;
    ModelCatalog *m_modelCatalog=nullptr;
    QMediaPlayer *m_player=nullptr;
    QAudioOutput *m_output=nullptr;
    QSettings *m_settings=nullptr;
    QTableWidget *m_table=nullptr;
    QPlainTextEdit *m_text=nullptr;
    QLineEdit *m_start=nullptr,*m_end=nullptr,*m_search=nullptr;
    QLabel *m_sourceLabel=nullptr,*m_sourceDetail=nullptr,*m_clock=nullptr,*m_cueTitle=nullptr,*m_stats=nullptr,*m_summary=nullptr,*m_status=nullptr,*m_modelHint=nullptr;
    QPushButton *m_transcribe=nullptr,*m_cancel=nullptr,*m_play=nullptr,*m_review=nullptr,*m_manageModels=nullptr;
    QComboBox *m_model=nullptr,*m_language=nullptr,*m_speed=nullptr,*m_zoom=nullptr;
    QCheckBox *m_loop=nullptr,*m_follow=nullptr,*m_unreviewed=nullptr;
    QProgressBar *m_progress=nullptr;
    QSplitter *m_splitter=nullptr;
    Waveform *m_wave=nullptr;
    QSlider *m_volume=nullptr;
};
