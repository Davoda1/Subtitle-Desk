// Copyright (c) 2026 davoda1
// SPDX-License-Identifier: AGPL-3.0-only
#include "model_manager_dialog.h"
#include "model_catalog.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace {
QPushButton *makeButton(const QString &text, const QString &name) {
    auto *button = new QPushButton(text);
    button->setObjectName(name);
    button->setCursor(Qt::PointingHandCursor);
    button->setAutoDefault(false);
    return button;
}

QString displaySize(qint64 bytes) {
    if (bytes <= 0) return QStringLiteral("—");
    if (bytes >= 1000000000)
        return QStringLiteral("%1 GB").arg(bytes / 1000000000.0, 0, 'f', 2);
    return QStringLiteral("%1 MB").arg(bytes / 1000000.0, 0, 'f', 1);
}
}

ModelManagerDialog::ModelManagerDialog(ModelCatalog *catalog, QString modelsDirectory,
                                       QString currentModelId, QWidget *parent)
    : QDialog(parent), m_catalog(catalog), m_directory(std::move(modelsDirectory)),
      m_currentModelId(std::move(currentModelId)) {
    setObjectName(QStringLiteral("modelManagerDialog"));
    setWindowTitle(QStringLiteral("Subtitle Desk models"));
    setMinimumSize(560, 440);
    resize(640, 500);
    if (QScreen *availableScreen = screen()) {
        const QSize available = availableScreen->availableGeometry().size() - QSize(24, 48);
        resize(std::min(width(), available.width()), std::min(height(), available.height()));
    }

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(8);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("SPEECH MODELS"));
    title->setObjectName(QStringLiteral("sectionTitle"));
    heading->addWidget(title);
    heading->addStretch();
    m_summary = new QLabel;
    m_summary->setObjectName(QStringLiteral("modelManagerSummary"));
    heading->addWidget(m_summary);
    layout->addLayout(heading);

    auto *intro = new QLabel(QStringLiteral(
        "Use the four bundled models, or import a compatible Whisper GGML (.bin) or Whistle (.cact) file."));
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::PlainText);
    layout->addWidget(intro);

    m_table = new QTableWidget(0, 4);
    m_table->setObjectName(QStringLiteral("modelManagerTable"));
    m_table->setHorizontalHeaderLabels({QStringLiteral("Model"), QStringLiteral("Family"),
                                        QStringLiteral("Source"), QStringLiteral("Size")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setWordWrap(false);
    m_table->setMinimumHeight(110);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(34);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 4; ++column)
        m_table->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Fixed);
    m_table->setColumnWidth(1, 76);
    m_table->setColumnWidth(2, 78);
    m_table->setColumnWidth(3, 78);
    layout->addWidget(m_table, 1);

    m_status = new QPlainTextEdit;
    m_status->setObjectName(QStringLiteral("modelManagerStatus"));
    m_status->setReadOnly(true);
    m_status->setTabChangesFocus(true);
    m_status->setStyleSheet(QStringLiteral("font-size:11px; padding:5px;"));
    m_status->setFixedHeight(62);
    m_status->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_status);

    auto *storageRow = new QHBoxLayout;
    storageRow->addWidget(new QLabel(QStringLiteral("Storage")));
    m_storage = new QLineEdit(QDir::toNativeSeparators(m_directory));
    m_storage->setObjectName(QStringLiteral("modelStoragePath"));
    m_storage->setReadOnly(true);
    m_storage->setMinimumWidth(100);
    m_storage->setCursorPosition(0);
    m_storage->setToolTip(QStringLiteral("Imported models are copied here. Bundled models stay with the application."));
    storageRow->addWidget(m_storage, 1);
    auto *openFolder = makeButton(QStringLiteral("Open folder"), QStringLiteral("openModelFolderButton"));
    storageRow->addWidget(openFolder);
    layout->addLayout(storageRow);

    auto *progressRow = new QHBoxLayout;
    m_progress = new QProgressBar;
    m_progress->setObjectName(QStringLiteral("modelImportProgress"));
    m_progress->setRange(0, 100);
    m_progress->hide();
    progressRow->addWidget(m_progress, 1);
    m_cancel = makeButton(QStringLiteral("Cancel import"), QStringLiteral("cancelImportButton"));
    m_cancel->hide();
    progressRow->addWidget(m_cancel);
    layout->addLayout(progressRow);

    auto *buttons = new QHBoxLayout;
    m_import = makeButton(QStringLiteral("Import model…"), QStringLiteral("importModelButton"));
    m_remove = makeButton(QStringLiteral("Remove…"), QStringLiteral("removeModelButton"));
    auto *help = makeButton(QStringLiteral("Help"), QStringLiteral("modelFormatHelpButton"));
    buttons->addWidget(m_import);
    buttons->addWidget(m_remove);
    buttons->addWidget(help);
    buttons->addStretch();
    m_use = makeButton(QStringLiteral("Use model"), QStringLiteral("useModelButton"));
    m_use->setProperty("primary", true);
    m_close = makeButton(QStringLiteral("Close"), QStringLiteral("closeModelManagerButton"));
    buttons->addWidget(m_use);
    buttons->addWidget(m_close);
    layout->addLayout(buttons);

    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        updateControls();
        if (!m_catalog || !m_catalog->busy()) describeSelection();
    });
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this] { chooseSelectedModel(); });
    connect(m_import, &QPushButton::clicked, this, &ModelManagerDialog::chooseImportFile);
    connect(m_remove, &QPushButton::clicked, this, &ModelManagerDialog::removeSelectedModel);
    connect(m_use, &QPushButton::clicked, this, &ModelManagerDialog::chooseSelectedModel);
    connect(m_cancel, &QPushButton::clicked, this, [this] { cancelImport(); });
    connect(m_close, &QPushButton::clicked, this, &ModelManagerDialog::reject);
    connect(help, &QPushButton::clicked, this, &ModelManagerDialog::showFormatHelp);
    connect(openFolder, &QPushButton::clicked, this, [this] {
        if (!QDir().mkpath(m_directory) || !QDesktopServices::openUrl(QUrl::fromLocalFile(m_directory)))
            m_status->setPlainText(QStringLiteral("The model folder could not be opened. You can copy its path from the Storage field."));
    });

    if (m_catalog) {
        connect(m_catalog, &ModelCatalog::modelsChanged, this, [this] { refreshModels(); });
        connect(m_catalog, &ModelCatalog::busyChanged, this, [this](bool busy) {
            m_progress->setVisible(busy);
            m_cancel->setVisible(busy);
            if (busy) {
                m_progress->setRange(0, 0);
                m_status->setPlainText(QStringLiteral("Checking model file…"));
            }
            updateControls();
        });
        connect(m_catalog, &ModelCatalog::importProgress, this, [this](int percent, const QString &message) {
            if (percent < 0) m_progress->setRange(0, 0);
            else { m_progress->setRange(0, 100); m_progress->setValue(percent); }
            if (!m_cancelRequested) m_status->setPlainText(message);
        });
        connect(m_catalog, &ModelCatalog::modelImported, this, [this](const QString &id, bool alreadyInstalled) {
            refreshModels(id);
            const QString name = m_catalog ? m_catalog->model(id).name : QString();
            QString message = alreadyInstalled
                ? QStringLiteral("%1 is already installed. Its existing entry is selected; no second copy was created.").arg(name)
                : QStringLiteral("%1 is installed and ready.").arg(name);
            message += m_recognitionActive
                ? QStringLiteral(" You can use it when the current audio job finishes.")
                : QStringLiteral(" Choose Use model to select it for transcription.");
            finishImport(message);
        });
        connect(m_catalog, &ModelCatalog::importFailed, this, [this](const QString &message) {
            finishImport(QStringLiteral("Import failed. ") + message);
        });
        connect(m_catalog, &ModelCatalog::importCancelled, this, [this] {
            finishImport(QStringLiteral("Import cancelled. Existing models were kept."));
        });
        connect(m_catalog, &QObject::destroyed, this, [this] { QDialog::reject(); });
    }
    refreshModels(m_currentModelId);
    updateControls();
    if(m_catalog&&m_catalog->busy())m_progress->setRange(0,0);
}

QString ModelManagerDialog::selectedModelId() const {
    const auto *item = m_table->item(m_table->currentRow(), 0);
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void ModelManagerDialog::refreshModels(const QString &preferredId) {
    const QString desired = !preferredId.isEmpty() ? preferredId : selectedModelId();
    const QSignalBlocker blocker(m_table);
    const auto models = m_catalog ? m_catalog->models() : QVector<ModelInfo>();
    m_table->setRowCount(models.size());
    int bundled = 0, selected = -1;
    for (int row = 0; row < models.size(); ++row) {
        const auto &model = models[row];
        if (model.bundled) ++bundled;
        const QStringList cells = {
            model.name,
            model.engine == QStringLiteral("whistle") ? QStringLiteral("Whistle") : QStringLiteral("Whisper"),
            model.bundled ? QStringLiteral("Bundled") : QStringLiteral("Imported"),
            displaySize(model.size)
        };
        for (int column = 0; column < cells.size(); ++column) {
            auto *item = new QTableWidgetItem(cells[column]);
            item->setData(Qt::UserRole, model.id);
            if (column == 3) item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            item->setToolTip(QStringLiteral("<qt>%1</qt>").arg((model.name + "\n" + model.description).toHtmlEscaped().replace('\n', "<br>")));
            m_table->setItem(row, column, item);
        }
        if (model.id == desired) selected = row;
    }
    if (selected < 0 && !models.isEmpty()) selected = 0;
    if (selected >= 0) m_table->setCurrentCell(selected, 0);
    m_summary->setText(QStringLiteral("%1 bundled · %2 imported").arg(bundled).arg(models.size() - bundled));
    updateControls();
    if (!m_catalog || !m_catalog->busy()) describeSelection();
}

void ModelManagerDialog::describeSelection() {
    const ModelInfo model = m_catalog ? m_catalog->model(selectedModelId()) : ModelInfo();
    if (model.id.isEmpty()) {
        m_status->setPlainText(QStringLiteral("Select a model, or import a compatible model file."));
        return;
    }
    QString description = model.name + QStringLiteral("\n") + model.description;
    if (model.bundled) description += QStringLiteral("\nIncluded with the application. Bundled models cannot be removed.");
    else description += QStringLiteral("\nStored on this computer. Removing it keeps your original model file and saved projects.");
    const QString unavailable = m_catalog->availability(model.id);
    if (!unavailable.isEmpty()) description += QStringLiteral("\nUnavailable: ") + unavailable;
    if (m_recognitionActive) description += QStringLiteral("\nFinish or cancel the current audio job before choosing or removing a model.");
    m_status->setPlainText(description);
}

void ModelManagerDialog::updateControls() {
    const bool busy = m_catalog && m_catalog->busy();
    const ModelInfo model = m_catalog ? m_catalog->model(selectedModelId()) : ModelInfo();
    const bool canChange = m_catalog && !busy && !m_recognitionActive && !m_closeAfterImport;
    m_import->setEnabled(m_catalog && !busy && !m_closeAfterImport);
    m_progress->setVisible(busy);
    m_cancel->setVisible(busy);
    m_remove->setEnabled(canChange && !model.id.isEmpty() && !model.bundled);
    m_use->setEnabled(canChange && !model.id.isEmpty() && m_catalog->availability(model.id).isEmpty());
    m_cancel->setEnabled(busy && !m_cancelRequested);
    m_close->setEnabled(!m_closeAfterImport);
    m_table->setEnabled(!busy);
    m_remove->setToolTip(model.bundled ? QStringLiteral("Bundled models stay with the application.")
                                     : QStringLiteral("Remove only the installed copy of this imported model."));
}

void ModelManagerDialog::setRecognitionActive(bool active) {
    m_recognitionActive = active;
    updateControls();
    if (!m_catalog || !m_catalog->busy()) describeSelection();
}

void ModelManagerDialog::chooseImportFile() {
    if (!m_catalog || m_catalog->busy() || m_closeAfterImport) return;
    const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Import speech model"), {},
        QStringLiteral("Compatible models (*.bin *.cact);;Whisper GGML (*.bin);;Whistle (*.cact)"));
    if (file.isEmpty()) return;
    QInputDialog nameDialog(this);
    nameDialog.setObjectName(QStringLiteral("importModelNameDialog"));
    nameDialog.setWindowTitle(QStringLiteral("Model display name"));
    nameDialog.setLabelText(QStringLiteral("Display name (optional):"));
    nameDialog.setInputMode(QInputDialog::TextInput);
    nameDialog.setTextValue(QFileInfo(file).completeBaseName());
    nameDialog.setOkButtonText(QStringLiteral("Import"));
    nameDialog.resize(440, nameDialog.sizeHint().height());
    if (nameDialog.exec() != QDialog::Accepted) return;
    m_cancelRequested = false;
    m_catalog->importModel(file, nameDialog.textValue());
    updateControls();
}

void ModelManagerDialog::chooseSelectedModel() {
    if (!m_catalog || m_catalog->busy() || m_recognitionActive || m_closeAfterImport) return;
    const QString id = selectedModelId();
    const QString error = m_catalog->availability(id);
    if (!error.isEmpty()) { m_status->setPlainText(error); return; }
    emit modelChosen(id);
    accept();
}

void ModelManagerDialog::removeSelectedModel() {
    if (!m_catalog || m_catalog->busy() || m_recognitionActive || m_closeAfterImport) return;
    const ModelInfo model = m_catalog->model(selectedModelId());
    if (model.id.isEmpty() || model.bundled) return;
    QMessageBox confirmation(QMessageBox::Question, QStringLiteral("Remove imported model"),
        QStringLiteral("Remove %1 from Subtitle Desk?\n\nOnly its installed copy will be removed. Your original model file, media, and saved projects will be kept. You can import the same file again later.").arg(model.name),
        QMessageBox::Yes | QMessageBox::No, this);
    confirmation.setTextFormat(Qt::PlainText);
    confirmation.setDefaultButton(QMessageBox::No);
    confirmation.setObjectName(QStringLiteral("removeModelConfirmation"));
    if (confirmation.exec() != QMessageBox::Yes) return;
    QString error;
    if (!m_catalog->removeModel(model.id, &error)) {
        m_status->setPlainText(QStringLiteral("The model could not be removed. ") + error);
        return;
    }
    refreshModels();
    m_status->setPlainText(model.id == m_currentModelId
        ? QStringLiteral("The selected model was removed. Choose another model before transcribing. Saved projects still retain their original model ID.")
        : QStringLiteral("The imported model was removed. Your original file and projects were kept."));
}

void ModelManagerDialog::cancelImport(bool closeAfterwards) {
    if (!m_catalog || !m_catalog->busy()) {
        if (closeAfterwards) QDialog::reject();
        return;
    }
    m_closeAfterImport = m_closeAfterImport || closeAfterwards;
    m_cancelRequested = true;
    m_status->setPlainText(m_closeAfterImport ? QStringLiteral("Cancelling the import before closing…")
                                             : QStringLiteral("Cancelling import…"));
    m_catalog->cancelImport();
    updateControls();
}

void ModelManagerDialog::finishImport(const QString &message) {
    m_cancelRequested = false;
    m_progress->hide();
    m_cancel->hide();
    m_status->setPlainText(message);
    updateControls();
    if (m_closeAfterImport) QDialog::reject();
}

void ModelManagerDialog::showFormatHelp() {
    QMessageBox::information(this, QStringLiteral("Compatible speech models"), QStringLiteral(
        "Whisper: a raw GGML .bin model made for whisper.cpp, including supported English-only and multilingual models.\n\n"
        "Whistle: a compatible raw .cact model.\n\n"
        "Import checks the file locally and copies an installed model into the Storage folder. The original file is kept. Files must be between 1 MB and 8 GiB. The bundled recognizer checks that the model loads, with a five-minute limit; this does not assess transcription accuracy. Large models need more memory and take longer to run on the CPU.\n\n"
        "Archives, installers, executables, GGUF models, and PyTorch model files are not supported. The bundled recognition engines are used for every model; importing does not install executable code."));
}

void ModelManagerDialog::reject() {
    if (m_catalog && m_catalog->busy()) { cancelImport(true); return; }
    QDialog::reject();
}

void ModelManagerDialog::closeEvent(QCloseEvent *event) {
    if (m_catalog && m_catalog->busy()) {
        event->ignore();
        cancelImport(true);
        return;
    }
    QDialog::closeEvent(event);
}
