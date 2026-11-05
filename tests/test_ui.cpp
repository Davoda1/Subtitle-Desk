#include "mainwindow.h"
#include "audio_session.h"
#include "transcription_service.h"
#include "model_catalog.h"
#include "model_manager_dialog.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QStyleOptionButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <QTimer>

namespace {
SubtitleDocument twoCues() {
    SubtitleDocument doc;
    doc.title=QStringLiteral("UI verification");doc.language=QStringLiteral("en");doc.model=QStringLiteral("base");
    doc.cues={Cue{QStringLiteral("first"),1000,5000,QStringLiteral("one two three four"),true},
              Cue{QStringLiteral("second"),6000,9000,QStringLiteral("Second phrase for review."),false}};
    return doc;
}
template<typename T> T *child(QObject &window,const char *name) {
    T *result=window.findChild<T *>(QString::fromLatin1(name));
    if(!result)qFatal("Required UI control not found: %s",name);
    return result;
}
QAction *menuAction(MainWindow &window,const QString &caption) {
    for(auto *action:window.findChildren<QAction *>()) {
        QString text=action->text();text.remove(QLatin1Char('&'));
        if(text==caption)return action;
    }
    qFatal("Required UI action not found: %s",qPrintable(caption));return nullptr;
}
void selectRow(MainWindow &window,int row) {
    child<QTableWidget>(window,"cueTable")->setCurrentCell(row,3,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);
    QCoreApplication::processEvents();
}
void show(MainWindow &window) {
    window.show();window.activateWindow();QCoreApplication::processEvents();
}
void replaceEditorText(MainWindow &window,const QString &text) {
    auto *editor=child<QPlainTextEdit>(window,"cueTextEdit");
    editor->setFocus();editor->selectAll();QTest::keyClicks(editor,text);
}
void commitTime(QLineEdit *edit,const QString &text) {
    edit->setFocus();edit->setText(text);QTest::keyClick(edit,Qt::Key_Return);QCoreApplication::processEvents();
}
void selectModelRow(ModelManagerDialog &dialog,const QString &id) {
    auto *table=child<QTableWidget>(dialog,"modelManagerTable");
    for(int row=0;row<table->rowCount();++row) {
        if(table->item(row,0)->data(Qt::UserRole).toString()==id) {
            table->setCurrentCell(row,0,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);
            QCoreApplication::processEvents();return;
        }
    }
    qFatal("Required model row not found: %s",qPrintable(id));
}
}

class UiTest final:public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::setApplicationName(QStringLiteral("SubtitleDeskUiTests"));
        QCoreApplication::setOrganizationName(QStringLiteral("SubtitleDeskTests"));
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    }
    void modelAndLanguageChoices() {
        MainWindow window(true);show(window);
        auto *models=child<QComboBox>(window,"modelCombo");
        auto *languages=child<QComboBox>(window,"languageCombo");
        QCOMPARE(models->count(),4);
        QCOMPARE(models->itemData(0).toString(),QStringLiteral("whistle"));
        QCOMPARE(models->itemData(1).toString(),QStringLiteral("tiny"));
        QCOMPARE(models->itemData(2).toString(),QStringLiteral("base"));
        QCOMPARE(models->itemData(3).toString(),QStringLiteral("small"));
        models->setCurrentIndex(models->findData(QStringLiteral("base")));
        QVERIFY(languages->findData(QStringLiteral("hu"))>=0);
        models->setCurrentIndex(models->findData(QStringLiteral("whistle")));
        QCOMPARE(languages->count(),8); // Auto plus seven supported languages.
        QCOMPARE(languages->findData(QStringLiteral("hu")),-1);
        QVERIFY(languages->findData(QStringLiteral("pl"))>=0);
        QVERIFY(!child<QPushButton>(window,"transcribeButton")->isEnabled());
    }
    void modelManagerKeepsControlsVisibleAndProtectsBundledModels() {
        QTemporaryDir temp;QVERIFY(temp.isValid());
        ModelCatalog catalog(qEnvironmentVariable("SUBTITLEDESK_RESOURCES"),temp.path());
        ModelManagerDialog dialog(&catalog,temp.path(),QStringLiteral("base"));
        dialog.resize(560,440);dialog.show();QCoreApplication::processEvents();
        QCOMPARE(dialog.size(),QSize(560,440));
        auto *table=child<QTableWidget>(dialog,"modelManagerTable");QCOMPARE(table->rowCount(),4);
        auto *remove=child<QPushButton>(dialog,"removeModelButton");
        for(const QString &id:QStringList{"whistle","tiny","base","small"}) {
            selectModelRow(dialog,id);QCOMPARE(dialog.selectedModelId(),id);QVERIFY(!remove->isEnabled());
        }
        for(const QString &name:QStringList{"modelManagerTable","modelManagerStatus","modelStoragePath","importModelButton","removeModelButton","useModelButton","closeModelManagerButton","modelFormatHelpButton","openModelFolderButton"}) {
            auto *control=dialog.findChild<QWidget *>(name);QVERIFY2(control,qPrintable(name));
            QVERIFY2(control->isVisibleTo(&dialog),qPrintable(name+" is hidden at minimum size"));
            const QRect bounds(control->mapTo(&dialog,QPoint(0,0)),control->size());
            QVERIFY2(dialog.rect().contains(bounds),qPrintable(name+" is outside the model manager"));
            QVERIFY2(bounds.width()>12&&bounds.height()>10,qPrintable(name+" has collapsed"));
        }
        QVERIFY(table->viewport()->height()>=70);
        dialog.setRecognitionActive(true);
        QVERIFY(!child<QPushButton>(dialog,"useModelButton")->isEnabled());QVERIFY(!remove->isEnabled());
        QVERIFY(child<QPushButton>(dialog,"closeModelManagerButton")->isEnabled());
        dialog.setRecognitionActive(false);
        QTest::mouseClick(child<QPushButton>(dialog,"closeModelManagerButton"),Qt::LeftButton);
        QVERIFY(!dialog.isVisible());
    }
    void missingSavedModelIsPreservedUntilUserChoosesAnother() {
        QTemporaryDir temp;QVERIFY(temp.isValid());
        auto doc=twoCues();doc.model=QStringLiteral("custom-")+QString(64,QLatin1Char('a'));doc.language=QStringLiteral("hu");
        const QString path=temp.filePath(QStringLiteral("missing-model.sdesk"));QString error;
        QVERIFY2(doc.saveProject(path,&error),qPrintable(error));
        MainWindow window(true);show(window);window.openPath(path);
        auto *models=child<QComboBox>(window,"modelCombo");auto *languages=child<QComboBox>(window,"languageCombo");
        QCOMPARE(models->currentData().toString(),doc.model);
        QVERIFY(models->currentText().contains(QStringLiteral("Missing")));
        QCOMPARE(languages->currentData().toString(),doc.language);QVERIFY(!languages->isEnabled());
        QVERIFY(!child<QPushButton>(window,"transcribeButton")->isEnabled());
        menuAction(window,QStringLiteral("Save project"))->trigger();
        SubtitleDocument saved;QVERIFY2(saved.load(path,&error),qPrintable(error));
        QCOMPARE(saved.model,doc.model);QCOMPARE(saved.language,doc.language);
        models->setCurrentIndex(models->findData(QStringLiteral("base")));QVERIFY(languages->isEnabled());
        menuAction(window,QStringLiteral("Save project"))->trigger();
        QVERIFY2(saved.load(path,&error),qPrintable(error));QCOMPARE(saved.model,QStringLiteral("base"));
    }
    void importedModelCanBeChosenSavedAndRemovedThroughManager() {
        const QString importFile=qEnvironmentVariable("SUBTITLEDESK_TEST_IMPORT_MODEL");
        if(importFile.isEmpty()||qEnvironmentVariableIsEmpty("SUBTITLEDESK_RESOURCES"))
            QSKIP("Set SUBTITLEDESK_RESOURCES and SUBTITLEDESK_TEST_IMPORT_MODEL for imported-model UI integration.");
        QTemporaryDir temp;QVERIFY(temp.isValid());
        const QString project=temp.filePath(QStringLiteral("imported-model.sdesk"));QString error;
        QVERIFY2(twoCues().saveProject(project,&error),qPrintable(error));
        MainWindow window(true);show(window);window.openPath(project);
        auto *catalog=window.modelCatalog();QVERIFY(catalog);
        QSignalSpy imported(catalog,&ModelCatalog::modelImported),failed(catalog,&ModelCatalog::importFailed);
        const QString storage=window.settingsDirectory()+QStringLiteral("/models");
        {
            ModelManagerDialog dialog(catalog,storage,QStringLiteral("base"),&window);
            dialog.show();QCoreApplication::processEvents();
            const auto importThroughDialogs=[&](bool acceptName) {
                QTimer::singleShot(0,&dialog,[&,acceptName] {
                    auto *picker=qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
                    if(!picker)qFatal("Import model did not open a file picker");
                    QTimer::singleShot(0,&dialog,[&,acceptName] {
                        auto *name=child<QInputDialog>(dialog,"importModelNameDialog");
                        name->setTextValue(QStringLiteral("English speech model"));
                        if(acceptName)name->accept();else name->reject();
                    });
                    picker->selectFile(importFile);
                    QMetaObject::invokeMethod(picker,"accept",Qt::DirectConnection);
                });
                QTest::mouseClick(child<QPushButton>(dialog,"importModelButton"),Qt::LeftButton);
            };
            importThroughDialogs(false);
            QVERIFY(!catalog->busy());QVERIFY(imported.isEmpty());QCOMPARE(catalog->models().size(),4);
            importThroughDialogs(true);QVERIFY(catalog->busy());
            QVERIFY(!child<QPushButton>(dialog,"importModelButton")->isEnabled());
            QVERIFY(!child<QPushButton>(dialog,"useModelButton")->isEnabled());
            QVERIFY(!child<QPushButton>(dialog,"removeModelButton")->isEnabled());
            QVERIFY(child<QPushButton>(dialog,"cancelImportButton")->isVisibleTo(&dialog));
            QTRY_VERIFY_WITH_TIMEOUT(!imported.isEmpty()||!failed.isEmpty(),120000);
            QVERIFY2(failed.isEmpty(),qPrintable(failed.isEmpty()?QString():failed.first().first().toString()));
            QCOMPARE(imported.count(),1);QVERIFY(!catalog->busy());
            QCOMPARE(child<QTableWidget>(dialog,"modelManagerTable")->rowCount(),5);
            QVERIFY(!child<QPushButton>(dialog,"cancelImportButton")->isVisibleTo(&dialog));
        }
        const QString id=imported.first().first().toString();QVERIFY(catalog->model(id).englishOnly);
        QCOMPARE(catalog->model(id).name,QStringLiteral("English speech model"));
        const QString installedPath=catalog->model(id).path;
        auto *models=child<QComboBox>(window,"modelCombo");auto *languages=child<QComboBox>(window,"languageCombo");
        QVERIFY(models->findData(id)>=0);
        // Exercise the real MainWindow -> modal manager -> Use model signal.
        QString chosen;
        QTimer::singleShot(0,&window,[&] {
            auto *dialog=child<ModelManagerDialog>(window,"modelManagerDialog");
            QObject::connect(dialog,&ModelManagerDialog::modelChosen,&window,[&](QString value){chosen=value;});
            selectModelRow(*dialog,id);
            QTest::mouseClick(child<QPushButton>(*dialog,"useModelButton"),Qt::LeftButton);
        });
        QTest::mouseClick(child<QPushButton>(window,"manageModelsButton"),Qt::LeftButton);
        QCOMPARE(chosen,id);QCOMPARE(models->currentData().toString(),id);
        QVERIFY(languages->findData(QStringLiteral("en"))>=0);
        for(int index=0;index<languages->count();++index) {
            const QString language=languages->itemData(index).toString();
            QVERIFY(language==QStringLiteral("en")||language==QStringLiteral("auto"));
        }
        languages->setCurrentIndex(languages->findData(QStringLiteral("en")));
        menuAction(window,QStringLiteral("Save project"))->trigger();
        SubtitleDocument saved;QVERIFY2(saved.load(project,&error),qPrintable(error));
        QCOMPARE(saved.model,id);QCOMPARE(saved.language,QStringLiteral("en"));
        QFile json(project);QVERIFY(json.open(QIODevice::ReadOnly));const QByteArray bytes=json.readAll();json.close();
        QVERIFY(!bytes.contains(installedPath.toUtf8()));QVERIFY(!bytes.contains(importFile.toUtf8()));
        models->setCurrentIndex(models->findData(QStringLiteral("base")));
        window.openPath(project);QCOMPARE(models->currentData().toString(),id);
        // Remove the installed copy with the actual confirmation dialog.
        bool confirmationShown=false;
        QTimer::singleShot(0,&window,[&] {
            auto *dialog=child<ModelManagerDialog>(window,"modelManagerDialog");selectModelRow(*dialog,id);
            QTimer::singleShot(0,dialog,[&] {
                auto *confirmation=child<QMessageBox>(window,"removeModelConfirmation");confirmationShown=true;
                QTest::mouseClick(confirmation->button(QMessageBox::Yes),Qt::LeftButton);
            });
            QTest::mouseClick(child<QPushButton>(*dialog,"removeModelButton"),Qt::LeftButton);
            QTest::mouseClick(child<QPushButton>(*dialog,"closeModelManagerButton"),Qt::LeftButton);
        });
        QTest::mouseClick(child<QPushButton>(window,"manageModelsButton"),Qt::LeftButton);
        QVERIFY(confirmationShown);QVERIFY(catalog->model(id).id.isEmpty());QVERIFY(!QFileInfo::exists(installedPath));
        QVERIFY(QFileInfo(importFile).isFile());QCOMPARE(models->currentData().toString(),id);
        QVERIFY(models->currentText().contains(QStringLiteral("Missing")));QVERIFY(!languages->isEnabled());
        QVERIFY(!child<QPushButton>(window,"transcribeButton")->isEnabled());
        QVERIFY2(saved.load(project,&error),qPrintable(error));QCOMPARE(saved.model,id);
    }
    void closingModelManagerCancelsPendingImportBeforeClosing() {
        const QString resources=qEnvironmentVariable("SUBTITLEDESK_RESOURCES");
        if(resources.isEmpty())QSKIP("Set SUBTITLEDESK_RESOURCES for asynchronous import cancellation.");
        QTemporaryDir temp;QVERIFY(temp.isValid());
        ModelCatalog catalog(resources,temp.path());
        ModelManagerDialog dialog(&catalog,temp.path(),QStringLiteral("base"));
        dialog.show();QCoreApplication::processEvents();
        QSignalSpy cancelled(&catalog,&ModelCatalog::importCancelled),imported(&catalog,&ModelCatalog::modelImported);
        catalog.importModel(QDir(resources).filePath(QStringLiteral("models/ggml-tiny-q5_1.bin")));
        QVERIFY(catalog.busy());
        QTest::mouseClick(child<QPushButton>(dialog,"closeModelManagerButton"),Qt::LeftButton);
        QVERIFY(dialog.isVisible()); // Closing waits for the worker to stop.
        QTRY_VERIFY_WITH_TIMEOUT(!catalog.busy()&&!dialog.isVisible(),10000);
        QCOMPARE(cancelled.count(),1);QVERIFY(imported.isEmpty());QCOMPARE(catalog.models().size(),4);
    }
    void typingCoalescesAndUndoRestoresReviewFlag() {
        MainWindow window(true);window.setDocument(twoCues(),false);show(window);
        replaceEditorText(window,QStringLiteral("A revised first line."));
        QCOMPARE(window.document().cues[0].text,QStringLiteral("A revised first line."));
        QVERIFY(!window.document().cues[0].reviewed);
        QVERIFY(window.isDirty());
        QCOMPARE(window.document().cues[1].text,twoCues().cues[1].text);
        QTest::keyClick(child<QPlainTextEdit>(window,"cueTextEdit"),Qt::Key_Z,Qt::ControlModifier);
        QCOMPARE(window.document().cues[0].text,twoCues().cues[0].text);
        QVERIFY(window.document().cues[0].reviewed);
        QTest::keyClick(child<QPlainTextEdit>(window,"cueTextEdit"),Qt::Key_Z,Qt::ControlModifier|Qt::ShiftModifier);
        QCOMPARE(window.document().cues[0].text,QStringLiteral("A revised first line."));
        QVERIFY(!window.document().cues[0].reviewed);
        // An edit in another cue starts a new undo step even within 700 ms.
        selectRow(window,1);replaceEditorText(window,QStringLiteral("A revised second line."));
        menuAction(window,QStringLiteral("Undo"))->trigger();
        QCOMPARE(window.document().cues[1].text,twoCues().cues[1].text);
        QCOMPARE(window.document().cues[0].text,QStringLiteral("A revised first line."));
        menuAction(window,QStringLiteral("Undo"))->trigger();
        QCOMPARE(window.document().cues[0].text,twoCues().cues[0].text);
    }
    void timingValidationAndUndo() {
        MainWindow window(true);window.setDocument(twoCues(),false);show(window);
        auto *start=child<QLineEdit>(window,"startEdit"),*end=child<QLineEdit>(window,"endEdit");
        commitTime(start,QStringLiteral("invalid"));
        QCOMPARE(window.document().cues[0].startMs,qint64(1000));
        QCOMPARE(start->text(),SubtitleDocument::formatTime(1000));
        QVERIFY(child<QLabel>(window,"statusText")->text().contains(QStringLiteral("end must be later")));
        commitTime(start,QStringLiteral("00:00:05.000"));
        QCOMPARE(window.document().cues[0].startMs,qint64(1000));
        commitTime(end,QStringLiteral("00:00:00.500"));
        QCOMPARE(window.document().cues[0].endMs,qint64(5000));
        commitTime(start,QStringLiteral("00:00:02.250"));
        QCOMPARE(window.document().cues[0].startMs,qint64(2250));
        QVERIFY(!window.document().cues[0].reviewed);
        menuAction(window,QStringLiteral("Undo"))->trigger();
        QCOMPARE(window.document().cues[0].startMs,qint64(1000));
        QVERIFY(window.document().cues[0].reviewed);
    }
    void playingHighlightDoesNotChangeEditingSelection() {
        MainWindow window(true);window.loadDemo();
        SubtitleDocument doc=twoCues();doc.cues[1].startMs=3000;doc.cues[1].endMs=7000;
        window.setDocument(doc,false);show(window);selectRow(window,0);
        auto *table=child<QTableWidget>(window,"cueTable");
        auto *editor=child<QPlainTextEdit>(window,"cueTextEdit");editor->setFocus();
        QTextCursor cursor=editor->textCursor();cursor.setPosition(7);editor->setTextCursor(cursor);
        window.setPlaybackPosition(3500);
        QCOMPARE(table->property("activeCue").toInt(),0);
        QVERIFY(table->item(0,3)->font().bold());QVERIFY(table->item(1,3)->font().bold());
        window.setPlaybackPosition(5000); // Half-open intervals: first is now inactive.
        QCOMPARE(table->property("activeCue").toInt(),1);
        QVERIFY(!table->item(0,3)->font().bold());QVERIFY(table->item(1,3)->font().bold());
        QCOMPARE(table->currentRow(),0);QCOMPARE(editor->toPlainText(),doc.cues[0].text);
        QCOMPARE(editor->textCursor().position(),7);
        window.setTheme(QStringLiteral("light"));
        QCOMPARE(table->property("activeCue").toInt(),1);QVERIFY(table->item(1,3)->font().bold());
        window.setPlaybackPosition(7000);QCOMPARE(table->property("activeCue").toInt(),-1);
        QVERIFY(!table->item(0,3)->font().bold());QVERIFY(!table->item(1,3)->font().bold());
        QCOMPARE(table->currentRow(),0);
    }
    void splitMergeDeleteAddAndUndo() {
        MainWindow window(true);window.loadDemo();window.setDocument(twoCues(),false);show(window);
        window.setPlaybackPosition(3000);
        auto *editor=child<QPlainTextEdit>(window,"cueTextEdit");
        QTextCursor cursor=editor->textCursor();cursor.setPosition(7);editor->setTextCursor(cursor);
        QTest::mouseClick(child<QPushButton>(window,"splitButton"),Qt::LeftButton);
        QCOMPARE(window.document().cues.size(),3);
        QCOMPARE(window.document().cues[0].text,QStringLiteral("one two"));
        QCOMPARE(window.document().cues[1].text,QStringLiteral("three four"));
        QCOMPARE(window.document().cues[0].endMs,qint64(3000));
        QCOMPARE(window.document().cues[1].startMs,qint64(3000));
        QVERIFY(!window.document().cues[0].reviewed);QVERIFY(!window.document().cues[1].reviewed);
        QVERIFY(window.document().cues[0].id!=window.document().cues[1].id);
        menuAction(window,QStringLiteral("Undo"))->trigger();
        QCOMPARE(window.document().cues.size(),2);QCOMPARE(window.document().cues[0].text,twoCues().cues[0].text);
        menuAction(window,QStringLiteral("Redo"))->trigger();QCOMPARE(window.document().cues.size(),3);
        auto *table=child<QTableWidget>(window,"cueTable");selectRow(window,0);
        table->selectionModel()->select(table->model()->index(1,3),QItemSelectionModel::Select|QItemSelectionModel::Rows);
        QTest::mouseClick(child<QPushButton>(window,"mergeButton"),Qt::LeftButton);
        QCOMPARE(window.document().cues.size(),2);
        QCOMPARE(window.document().cues[0].text,QStringLiteral("one two\nthree four"));
        QCOMPARE(window.document().cues[0].startMs,qint64(1000));QCOMPARE(window.document().cues[0].endMs,qint64(5000));
        QTest::mouseClick(child<QPushButton>(window,"deleteButton"),Qt::LeftButton);
        QCOMPARE(window.document().cues.size(),1);QCOMPARE(window.document().cues[0].id,QStringLiteral("second"));
        menuAction(window,QStringLiteral("Undo"))->trigger();QCOMPARE(window.document().cues.size(),2);
        window.setPlaybackPosition(4000);QTest::mouseClick(child<QPushButton>(window,"addButton"),Qt::LeftButton);
        QCOMPARE(window.document().cues.size(),3);QCOMPARE(window.document().cues[1].startMs,qint64(4000));
        QCOMPARE(window.document().cues[1].endMs,qint64(6500));QVERIFY(window.document().cues[1].text.isEmpty());
        menuAction(window,QStringLiteral("Undo"))->trigger();QCOMPARE(window.document().cues.size(),2);
    }
    void nonConsecutiveMergeIsRejected() {
        MainWindow window(true);auto doc=twoCues();doc.cues.append(Cue{"third",10000,13000,"Third phrase.",false});window.setDocument(doc,false);show(window);
        auto *table=child<QTableWidget>(window,"cueTable");selectRow(window,0);
        table->selectionModel()->select(table->model()->index(2,3),QItemSelectionModel::Select|QItemSelectionModel::Rows);
        QTest::mouseClick(child<QPushButton>(window,"mergeButton"),Qt::LeftButton);
        QCOMPARE(window.document().cues.size(),3);QVERIFY(!window.isDirty());
        QVERIFY(child<QLabel>(window,"statusText")->text().contains(QStringLiteral("consecutive")));
    }
    void reviewAndSearchFilters() {
        MainWindow window(true);window.setDocument(twoCues(),false);show(window);
        auto *table=child<QTableWidget>(window,"cueTable");
        auto *search=child<QLineEdit>(window,"searchEdit");search->setText(QStringLiteral("SECOND"));
        QVERIFY(table->isRowHidden(0));QVERIFY(!table->isRowHidden(1));
        search->clear();QVERIFY(!table->isRowHidden(0));
        auto *unreviewed=child<QCheckBox>(window,"unreviewedCheck");unreviewed->setChecked(true);
        QVERIFY(table->isRowHidden(0));QVERIFY(!table->isRowHidden(1));
        selectRow(window,1);QTest::mouseClick(child<QPushButton>(window,"reviewButton"),Qt::LeftButton);
        QVERIFY(window.document().cues[1].reviewed);QVERIFY(table->isRowHidden(1));
        menuAction(window,QStringLiteral("Undo"))->trigger();QVERIFY(!window.document().cues[1].reviewed);
        QVERIFY(!table->isRowHidden(1));
    }
    void failedOpenPreservesCurrentEdits() {
        MainWindow window(true);window.setDocument(twoCues(),true);show(window);
        const QString original=window.document().exportText(QStringLiteral("json"));
        QTemporaryDir temp;QVERIFY(temp.isValid());
        window.openPath(temp.filePath(QStringLiteral("missing-media.mp4")));
        QCOMPARE(window.document().exportText(QStringLiteral("json")),original);QVERIFY(window.isDirty());
        QFile broken(temp.filePath(QStringLiteral("broken.sdesk")));QVERIFY(broken.open(QIODevice::WriteOnly));broken.write("{ incomplete");broken.close();
        window.openPath(broken.fileName());QCOMPARE(window.document().exportText(QStringLiteral("json")),original);
        QVERIFY(window.isDirty());
    }
    void missingProjectAudioDoesNotReuseDemoOrEnableTranscription() {
        MainWindow window(true);window.loadDemo();show(window);
        QTemporaryDir temp;QVERIFY(temp.isValid());SubtitleDocument doc=twoCues();doc.mediaPath=temp.filePath(QStringLiteral("missing.wav"));
        const QString project=temp.filePath(QStringLiteral("project.sdesk"));QString error;QVERIFY2(doc.saveProject(project,&error),qPrintable(error));
        window.openPath(project);QCOMPARE(window.document().cues[0].text,doc.cues[0].text);
        QVERIFY(!window.isDirty());QVERIFY(!child<QPushButton>(window,"transcribeButton")->isEnabled());
        QTest::mouseClick(child<QPushButton>(window,"playButton"),Qt::LeftButton);
        QVERIFY(child<QLabel>(window,"statusText")->text().contains(QStringLiteral("Open an audio or video")));
    }
    void keyboardSaveIncludesPendingTimingEdit() {
        // A global Save action must serialize the visible time even if its
        // QLineEdit still owns focus and editingFinished has not fired yet.
        MainWindow window(true);show(window);QTemporaryDir temp;QVERIFY(temp.isValid());
        auto doc=twoCues();const QString path=temp.filePath(QStringLiteral("pending-time.sdesk"));QString error;
        QVERIFY2(doc.saveProject(path,&error),qPrintable(error));window.openPath(path);
        auto *start=child<QLineEdit>(window,"startEdit");start->setFocus();start->setText(QStringLiteral("00:00:01.750"));
        menuAction(window,QStringLiteral("Save project"))->trigger();
        SubtitleDocument reopened;QVERIFY2(reopened.load(path,&error),qPrintable(error));
        QCOMPARE(reopened.cues[0].startMs,qint64(1750));
    }
    void destroyingWithPendingTimeDoesNotCallFreedDocument() {
        // Qt can emit editingFinished while QWidget closes native surfaces in
        // its base destructor. Application callbacks must be disconnected
        // before the derived document/history members are destroyed.
        MainWindow window(true);window.setDocument(twoCues(),false);show(window);
        auto *start=child<QLineEdit>(window,"startEdit");
        start->setFocus();start->setText(QStringLiteral("00:00:01.750"));
    }
    void compactLayoutKeepsEditingControlsReachable() {
        MainWindow window(true);window.loadDemo();show(window);window.resize(680,540);QCoreApplication::processEvents();
        QCOMPARE(window.size(),QSize(680,540));
        const QStringList essential={"modelCombo","languageCombo","manageModelsButton","transcribeButton","waveform","playButton","backButton","forwardButton","clock","loopCheck","speedCombo","cueTable","searchEdit","unreviewedCheck","followCheck","addButton","splitButton","mergeButton","deleteButton","startEdit","endEdit","cueTextEdit","replayButton","reviewButton","previousButton","nextButton","statusText"};
        for(const QString &name:essential) {
            auto *control=window.findChild<QWidget *>(name);QVERIFY2(control,qPrintable(name));
            QVERIFY2(control->isVisibleTo(&window),qPrintable(name+" is hidden at minimum size"));
            const QRect bounds(control->mapTo(&window,QPoint(0,0)),control->size());
            QVERIFY2(window.rect().contains(bounds),qPrintable(name+" is outside the window: "+QString::number(bounds.x())+","+QString::number(bounds.y())+" "+QString::number(bounds.width())+"x"+QString::number(bounds.height())));
            QVERIFY2(bounds.width()>12&&bounds.height()>10,qPrintable(name+" has collapsed"));
        }
        auto *editor=child<QPlainTextEdit>(window,"cueTextEdit");QVERIFY(editor->height()>=55);
        QVERIFY(menuAction(window,QStringLiteral("Set cue start at playhead"))->isEnabled());
        QVERIFY(menuAction(window,QStringLiteral("Set cue end at playhead"))->isEnabled());
        QVERIFY(!menuAction(window,QStringLiteral("Set cue start at playhead"))->shortcut().isEmpty());
        QVERIFY(!menuAction(window,QStringLiteral("Export…"))->shortcut().isEmpty());
        QVERIFY(child<QTableWidget>(window,"cueTable")->viewport()->height()>=50);
        window.resize(1180,800);QCoreApplication::processEvents();
        QVERIFY(child<QPushButton>(window,"startEditSet")->isVisibleTo(&window));
        QVERIFY(child<QPushButton>(window,"endEditSet")->isVisibleTo(&window));
        QVERIFY(child<QPushButton>(window,"openMediaButton")->isVisibleTo(&window));
    }
    void realMediaTranscriptionReviewSaveAndExport() {
        // Optional integration uses the short synthetic fixture (or another
        // caller-supplied recording) and the real bundled models.
        const QString media=qEnvironmentVariable("SUBTITLEDESK_TEST_MEDIA");
        if(media.isEmpty())QSKIP("Set SUBTITLEDESK_TEST_MEDIA and SUBTITLEDESK_RESOURCES to run real audio transcription.");
        QVERIFY2(QFileInfo(media).isFile(),"The supplied integration media must exist.");
        QTemporaryDir temp;QVERIFY(temp.isValid());
        SubtitleDocument seed;seed.title=QStringLiteral("Audio review integration");seed.mediaPath=QFileInfo(media).absoluteFilePath();
        seed.model=QStringLiteral("tiny");seed.language=QStringLiteral("en");
        const QString project=temp.filePath(QStringLiteral("review.sdesk"));QString error;
        QVERIFY2(seed.saveProject(project,&error),qPrintable(error));
        MainWindow window(true);show(window);window.openPath(project);
        auto *transcribe=child<QPushButton>(window,"transcribeButton");
        auto *status=child<QLabel>(window,"statusText");
        QTRY_VERIFY_WITH_TIMEOUT(transcribe->isEnabled(),30000);
        auto *audio=window.findChild<AudioSession *>();QVERIFY(audio);QVERIFY(audio->durationMs()>0);
        QVERIFY(QFileInfo::exists(audio->wavPath()));
        auto *models=child<QComboBox>(window,"modelCombo");models->setCurrentIndex(models->findData(QStringLiteral("tiny")));
        auto *languages=child<QComboBox>(window,"languageCombo");languages->setCurrentIndex(languages->findData(QStringLiteral("en")));
        auto *recognizer=window.findChild<TranscriptionService *>();QVERIFY(recognizer);
        QVERIFY2(recognizer->availability(QStringLiteral("tiny")).isEmpty(),qPrintable(recognizer->availability(QStringLiteral("tiny"))));
        QTest::mouseClick(transcribe,Qt::LeftButton);QVERIFY(recognizer->busy());
        QVERIFY(!models->isEnabled());QVERIFY(!languages->isEnabled());
        bool modelChangesProtected=false;
        QTimer::singleShot(0,&window,[&] {
            auto *dialog=child<ModelManagerDialog>(window,"modelManagerDialog");
            modelChangesProtected=!child<QPushButton>(*dialog,"useModelButton")->isEnabled()
                &&!child<QPushButton>(*dialog,"removeModelButton")->isEnabled();
            QTest::mouseClick(child<QPushButton>(*dialog,"closeModelManagerButton"),Qt::LeftButton);
        });
        QTest::mouseClick(child<QPushButton>(window,"manageModelsButton"),Qt::LeftButton);
        QVERIFY(modelChangesProtected);
        // The demonstration must not replace state owned by a real worker.
        window.loadDemo();QCOMPARE(window.document().title,seed.title);
        QTRY_VERIFY_WITH_TIMEOUT(!recognizer->busy(),120000);
        QVERIFY2(!window.document().cues.isEmpty(),qPrintable(status->text()));
        QVERIFY2(window.document().validationErrors().isEmpty(),qPrintable(window.document().validationErrors().join('\n')));
        const qsizetype count=window.document().cues.size();
        for(const auto &cue:window.document().cues) {
            QVERIFY(cue.startMs>=0);QVERIFY(cue.endMs>cue.startMs);QVERIFY(cue.endMs<=audio->durationMs());
            QVERIFY(!cue.reviewed);QVERIFY(!cue.text.trimmed().isEmpty());
        }
        QCOMPARE(window.document().model,QStringLiteral("tiny"));QCOMPARE(window.document().language,QStringLiteral("en"));
        auto *player=window.findChild<QMediaPlayer *>();QVERIFY(player);
        QTRY_VERIFY_WITH_TIMEOUT(player->mediaStatus()==QMediaPlayer::LoadedMedia||player->mediaStatus()==QMediaPlayer::BufferedMedia,10000);
        selectRow(window,0);
        const int target=count>1?1:0;const Cue targetCue=window.document().cues[target];
        const qint64 position=targetCue.startMs+(targetCue.endMs-targetCue.startMs)/2;
        window.setPlaybackPosition(position);
        auto *table=child<QTableWidget>(window,"cueTable");
        QTRY_COMPARE_WITH_TIMEOUT(table->property("activeCue").toInt(),window.document().cueAt(position),10000);
        QCOMPARE(table->currentRow(),0);
        replaceEditorText(window,QStringLiteral("Reviewed transcription line."));
        QTest::mouseClick(child<QPushButton>(window,"reviewButton"),Qt::LeftButton);
        QVERIFY(window.document().cues[0].reviewed);QVERIFY(window.isDirty());
        auto *editor=child<QPlainTextEdit>(window,"cueTextEdit");editor->setFocus();
        QTest::keyClick(editor,Qt::Key_S,Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(!window.isDirty(),3000);
        SubtitleDocument reopened;QVERIFY2(reopened.load(project,&error),qPrintable(error));
        QCOMPARE(reopened.cues.size(),count);QCOMPARE(reopened.cues[0].text,QStringLiteral("Reviewed transcription line."));
        QVERIFY(reopened.cues[0].reviewed);QCOMPARE(reopened.model,QStringLiteral("tiny"));
        QCOMPARE(QFileInfo(reopened.mediaPath).absoluteFilePath(),QFileInfo(media).absoluteFilePath());
        for(const QString &format:QStringList{"srt","vtt","ass","lrc","txt","csv","json"}) {
            const QString output=temp.filePath(QStringLiteral("export.")+format);
            QVERIFY2(reopened.exportFile(output,format,&error),qPrintable(format+": "+error));
            QFile file(output);QVERIFY(file.open(QIODevice::ReadOnly));const QString text=QString::fromUtf8(file.readAll());
            QVERIFY2(!text.isEmpty(),qPrintable(format+" produced an empty file"));
            if(format=="txt") {
                QCOMPARE(text.trimmed().split(QStringLiteral("\n\n"),Qt::SkipEmptyParts).size(),count);
            } else if(format=="csv") {
                // Count RFC 4180 records without counting quoted line breaks.
                int rows=0;bool quoted=false;
                for(qsizetype i=0;i<text.size();++i) {
                    if(text[i]=='"') {if(quoted&&i+1<text.size()&&text[i+1]=='"')++i;else quoted=!quoted;}
                    else if(text[i]=='\n'&&!quoted)++rows;
                }
                QVERIFY(!quoted);QCOMPARE(rows,count+1);
            } else {
                SubtitleDocument roundtrip;QVERIFY2(roundtrip.load(output,&error),qPrintable(format+": "+error));
                QCOMPARE(roundtrip.cues.size(),count);QCOMPARE(roundtrip.cues[0].text,reopened.cues[0].text);
            }
        }
        // Cancelling a replacement transcription preserves the reviewed edits.
        const QString before=window.document().exportText(QStringLiteral("json"));
        models->setCurrentIndex(models->findData(QStringLiteral("small")));
        QTest::mouseClick(transcribe,Qt::LeftButton);QVERIFY(recognizer->busy());
        QTest::mouseClick(child<QPushButton>(window,"cancelButton"),Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!recognizer->busy(),5000);
        QCOMPARE(window.document().exportText(QStringLiteral("json")),before);
        QVERIFY(!window.isDirty());
    }
    void testModeDoesNotWriteRecovery() {
        MainWindow window(true);window.setDocument(twoCues(),true);show(window);
        const QString recovery=window.settingsDirectory()+QStringLiteral("/recovery.sdesk");
        QFile::remove(recovery);
        replaceEditorText(window,QStringLiteral("Unsaved test edit."));
        window.close();QCoreApplication::processEvents();QVERIFY(!QFile::exists(recovery));
    }
    void cleanup() {
        // Every window has been destroyed when this slot runs. Only this
        // process's explicit test-mode model copies are removed here.
        const QString models=QDir::tempPath()+QStringLiteral("/subtitledesk-test-")+QString::number(QCoreApplication::applicationPid())+QStringLiteral("/models");
        QDir(models).removeRecursively();
    }
    void cleanupTestCase() {
        const QString ownPath=QDir::tempPath()+QStringLiteral("/subtitledesk-test-")+QString::number(QCoreApplication::applicationPid());
        QDir(ownPath).removeRecursively();
    }
};
QTEST_MAIN(UiTest)
#include "test_ui.moc"
