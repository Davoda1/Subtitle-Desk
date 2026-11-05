// Copyright (c) 2026 davoda1
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <memory>

class QThread;
class QLockFile;

struct ModelInfo {
    QString id;
    QString name;
    // The engine family is "whisper" or "whistle".
    QString engine;
    QString path;
    QString sha256;
    QString description;
    bool bundled = false;
    bool englishOnly = false;
    qint64 size = 0;
};

// Owns the four bundled choices and validated, locally imported model data.
// Call this object's methods on its owning thread. Imports run in a worker
// thread and invoke only the application's bundled recognition executables.
class ModelCatalog : public QObject {
    Q_OBJECT
public:
    ModelCatalog(QString resourceRoot, QString modelsDirectory, QObject *parent = nullptr);
    ~ModelCatalog() override;
    QString resourceRoot() const { return m_resourceRoot; }
    QVector<ModelInfo> models() const;
    // An unknown ID returns a default ModelInfo with an empty id.
    ModelInfo model(QString id) const;
    // An empty result means the model and its bundled engine are available.
    QString availability(QString id) const;
    bool busy() const { return m_busy; }
    void importModel(QString sourcePath, QString displayName = {});
    void cancelImport();
    bool removeModel(QString id, QString *error = nullptr);

    // An active recognition keeps its installed model from being removed.
    bool acquireModel(QString id, ModelInfo *info, QString *error = nullptr);
    void releaseModel(QString id);
    bool inUse(QString id) const;

signals:
    void modelsChanged();
    void importProgress(int percent, QString status);
    void modelImported(QString id, bool alreadyInstalled);
    void importFailed(QString message);
    void importCancelled();
    void busyChanged(bool busy);

private:
    friend class TranscriptionService;
    void reload();
    QString m_resourceRoot;
    QString m_directory;
    QString m_storageError;
    QVector<ModelInfo> m_models;
    QHash<QString, int> m_users;
    QHash<QString, std::shared_ptr<QLockFile>> m_leases;
    QThread *m_worker = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancel;
    bool m_busy = false;
};
