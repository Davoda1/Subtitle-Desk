// Copyright (c) 2026 davoda1
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <QDialog>
#include <QPointer>

class ModelCatalog;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;

class ModelManagerDialog final : public QDialog {
    Q_OBJECT
public:
    ModelManagerDialog(ModelCatalog *catalog, QString modelsDirectory,
                       QString currentModelId, QWidget *parent = nullptr);
    void setRecognitionActive(bool active);
    QString selectedModelId() const;

signals:
    void modelChosen(QString id);

protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;

private:
    void refreshModels(const QString &preferredId = {});
    void updateControls();
    void describeSelection();
    void chooseImportFile();
    void chooseSelectedModel();
    void removeSelectedModel();
    void cancelImport(bool closeAfterwards = false);
    void finishImport(const QString &message);
    void showFormatHelp();

    QPointer<ModelCatalog> m_catalog;
    QString m_directory;
    QString m_currentModelId;
    bool m_recognitionActive = false;
    bool m_cancelRequested = false;
    bool m_closeAfterImport = false;
    QLabel *m_summary = nullptr;
    QTableWidget *m_table = nullptr;
    QPlainTextEdit *m_status = nullptr;
    QLineEdit *m_storage = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_import = nullptr;
    QPushButton *m_remove = nullptr;
    QPushButton *m_use = nullptr;
    QPushButton *m_cancel = nullptr;
    QPushButton *m_close = nullptr;
};
