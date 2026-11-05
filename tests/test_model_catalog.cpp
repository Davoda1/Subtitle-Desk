// Copyright (c) 2026 davoda1
// SPDX-License-Identifier: AGPL-3.0-only
#include "model_catalog.h"

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

namespace {
QString resourceRoot() { return qEnvironmentVariable("SUBTITLEDESK_RESOURCES"); }
QString englishModel() { return qEnvironmentVariable("SUBTITLEDESK_TEST_IMPORT_MODEL"); }
QString tinyModel() { return QDir(resourceRoot()).filePath(QStringLiteral("models/ggml-tiny-q5_1.bin")); }
QString hashFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}
bool writeBytes(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QStringList installedIds(const ModelCatalog &catalog) {
    QStringList ids;
    for (const auto &model : catalog.models()) if (!model.bundled) ids.append(model.id);
    return ids;
}
QStringList storageEntries(const QString &path) {
    return QDir(path).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
}
struct ImportResult {
    enum Kind { Imported, Failed, Cancelled, Timeout } kind = Timeout;
    QString id;
    QString error;
    bool duplicate = false;
    bool busyAtFinish = true;
};
ImportResult importAndWait(ModelCatalog &catalog, const QString &path,
                           const QString &name = {}, bool cancelImmediately = false) {
    ImportResult result;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    auto finish = [&](ImportResult::Kind kind) {
        result.kind = kind;
        result.busyAtFinish = catalog.busy();
        loop.quit();
    };
    QObject::connect(&catalog, &ModelCatalog::modelImported, &loop, [&](QString id, bool duplicate) {
        result.id = id; result.duplicate = duplicate; finish(ImportResult::Imported);
    });
    QObject::connect(&catalog, &ModelCatalog::importFailed, &loop, [&](QString error) {
        result.error = error; finish(ImportResult::Failed);
    });
    QObject::connect(&catalog, &ModelCatalog::importCancelled, &loop, [&] { finish(ImportResult::Cancelled); });
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] { catalog.cancelImport(); loop.quit(); });
    timeout.start(120000);
    catalog.importModel(path, name);
    if (cancelImmediately) catalog.cancelImport();
    if (result.kind == ImportResult::Timeout) loop.exec();
    return result;
}
QJsonObject metadataFor(const QString &source) {
    const QString sha = hashFile(source);
    return {{"schema", 1}, {"id", QStringLiteral("custom-") + sha},
            {"name", "Imported English model"}, {"engine", "whisper"},
            {"sha256", sha}, {"englishOnly", true}, {"size", QFileInfo(source).size()}};
}
bool writeMetadata(const QString &directory, const QJsonObject &object) {
    return writeBytes(QDir(directory).filePath(QStringLiteral("metadata.json")),
                      QJsonDocument(object).toJson());
}
bool stagePersistedModel(const QString &directory, const QString &source, const QJsonObject &metadata) {
    return QDir().mkpath(directory)
        && QFile::copy(source, QDir(directory).filePath(QStringLiteral("model.bin")))
        && writeMetadata(directory, metadata);
}
}

class ModelCatalogTest final : public QObject {
    Q_OBJECT
private slots:
    void bundledModelsAreStableAndCannotBeRemoved() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        ModelCatalog catalog(resourceRoot(), temp.filePath(QStringLiteral("models")));
        const QStringList expected{QStringLiteral("whistle"), QStringLiteral("tiny"),
                                   QStringLiteral("base"), QStringLiteral("small")};
        QCOMPARE(catalog.models().size(), expected.size());
        for (int index = 0; index < expected.size(); ++index) {
            const ModelInfo info = catalog.models()[index];
            QCOMPARE(info.id, expected[index]); QVERIFY(info.bundled);
            QString error;
            QVERIFY(!catalog.removeModel(info.id, &error)); QVERIFY(!error.isEmpty());
            QCOMPARE(catalog.model(info.id).path, info.path);
        }
        QVERIFY(catalog.model(QStringLiteral("../../outside")).id.isEmpty());
        QVERIFY(!catalog.availability(QStringLiteral("unknown-model")).isEmpty());
        QString error;
        QVERIFY(!catalog.removeModel(QStringLiteral("../tiny"), &error));
        QVERIFY(!catalog.removeModel(QStringLiteral("custom-../outside"), &error));
        QCOMPARE(catalog.models().size(), expected.size());
    }

    void unsupportedContentIsRejected_data() {
        QTest::addColumn<QByteArray>("content");
        QTest::addColumn<QString>("extension");
        const auto row=[](const char *name,const QByteArray &bytes,const QString &extension=QStringLiteral("bin")) {
            QTest::newRow(name)<<bytes<<extension;
        };
        row("empty", QByteArray());
        row("text-disguised-as-bin", QByteArray("This is a document, not model data.\n"));
        row("git-lfs-pointer", QByteArray("version https://git-lfs.github.com/spec/v1\noid sha256:0123456789\nsize 32166155\n"));
        row("gguf", QByteArray("GGUF\x03\0\0\0", 8));
        row("zip", QByteArray("PK\x03\x04", 4));
        row("truncated-ggml", QByteArray("lmgg", 4));
        row("invalid-ggml-dimensions", QByteArray("lmgg", 4) + QByteArray(4096, '\0'));
        row("executable", QByteArray("\x7f" "ELF", 4));
        row("needle-without-speech-manifest", QByteArray("\x84\x2a\xe1\x05",4), QStringLiteral("cact"));
    }
    void unsupportedContentIsRejected() {
        QFETCH(QByteArray, content);
        QFETCH(QString, extension);
        // Except for explicit empty/truncated cases, pass the minimum size so
        // the rejection actually exercises format/header validation.
        if(!content.isEmpty()&&content!=QByteArray("lmgg",4)) content.append(QByteArray(1000001-content.size(),'\0'));
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const QString source = temp.filePath(QStringLiteral("untrusted.")+extension);
        const QString store = temp.filePath(QStringLiteral("models"));
        QVERIFY(writeBytes(source, content));
        // Missing engine executables must not turn an invalid header into an
        // installed model, and invalid content must leave no partial entries.
        ModelCatalog catalog(temp.filePath(QStringLiteral("no-runtime")), store);
        const auto result = importAndWait(catalog, source);
        QCOMPARE(result.kind, ImportResult::Failed); QVERIFY(!result.error.isEmpty());
        QVERIFY(!result.busyAtFinish); QVERIFY(!catalog.busy());
        QVERIFY(installedIds(catalog).isEmpty()); QVERIFY(storageEntries(store).isEmpty());
        QFile file(source); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), content);
    }

    void missingFilesAndDirectoriesCannotBeImported() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const QString store = temp.filePath(QStringLiteral("models"));
        ModelCatalog catalog(resourceRoot(), store);
        for (const auto &path : {temp.filePath(QStringLiteral("missing.bin")), temp.path()}) {
            const auto result = importAndWait(catalog, path);
            QCOMPARE(result.kind, ImportResult::Failed); QVERIFY(!result.busyAtFinish);
            QVERIFY(installedIds(catalog).isEmpty()); QVERIFY(storageEntries(store).isEmpty());
        }
    }

    void importReloadDeduplicateAndRemove() {
        if (resourceRoot().isEmpty() || englishModel().isEmpty())
            QSKIP("Set SUBTITLEDESK_RESOURCES and SUBTITLEDESK_TEST_IMPORT_MODEL for real-model integration.");
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const QString store = temp.filePath(QStringLiteral("models"));
        const QString source = temp.filePath(QStringLiteral("renamed-local-model.bin"));
        QVERIFY(QFile::copy(englishModel(), source));
        const QString sha = hashFile(source); QCOMPARE(sha.size(), 64);
        const QString id = QStringLiteral("custom-") + sha;
        ModelCatalog catalog(resourceRoot(), store);
        QSignalSpy changed(&catalog, &ModelCatalog::modelsChanged);
        const auto result = importAndWait(catalog, source, QStringLiteral("My English model"));
        QCOMPARE(result.kind, ImportResult::Imported); QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(!result.busyAtFinish); QVERIFY(!result.duplicate); QCOMPARE(result.id, id);
        QCOMPARE(changed.count(), 1); QCOMPARE(installedIds(catalog), QStringList{id});
        const ModelInfo info = catalog.model(id);
        QCOMPARE(info.name, QStringLiteral("My English model")); QCOMPARE(info.engine, QStringLiteral("whisper"));
        QCOMPARE(info.sha256, sha); QCOMPARE(info.size, QFileInfo(source).size());
        QVERIFY(!info.bundled); QVERIFY(info.englishOnly);
        QCOMPARE(QFileInfo(info.path).canonicalFilePath(),
                 QFileInfo(QDir(store).filePath(id + QStringLiteral("/model.bin"))).canonicalFilePath());
        QVERIFY(!QFileInfo(info.path).isSymLink()); QCOMPARE(hashFile(info.path), sha);
        QVERIFY2(catalog.availability(id).isEmpty(), qPrintable(catalog.availability(id)));
        const auto duplicate = importAndWait(catalog, source, QStringLiteral("A duplicate display name"));
        QCOMPARE(duplicate.kind, ImportResult::Imported); QVERIFY(duplicate.duplicate);
        QCOMPARE(duplicate.id, id); QVERIFY(!duplicate.busyAtFinish);
        QCOMPARE(installedIds(catalog), QStringList{id});
        QCOMPARE(storageEntries(store), QStringList{id});
        {
            ModelCatalog reloaded(resourceRoot(), store);
            QCOMPARE(installedIds(reloaded), QStringList{id});
            QCOMPARE(reloaded.model(id).path, info.path); QVERIFY(reloaded.model(id).englishOnly);
            ModelInfo acquired; QString error;
            QVERIFY2(reloaded.acquireModel(id, &acquired, &error), qPrintable(error));
            QVERIFY(reloaded.acquireModel(id, &acquired, &error)); QVERIFY(reloaded.inUse(id));
            QVERIFY(!reloaded.removeModel(id, &error)); QVERIFY(!error.isEmpty());
            reloaded.releaseModel(id); QVERIFY(reloaded.inUse(id));
            QVERIFY(!reloaded.removeModel(id, &error));
            reloaded.releaseModel(id); QVERIFY(!reloaded.inUse(id));
            QVERIFY2(reloaded.removeModel(id, &error), qPrintable(error));
            QVERIFY(reloaded.model(id).id.isEmpty()); QVERIFY(!QFileInfo::exists(info.path));
            QVERIFY(!reloaded.removeModel(id, &error));
        }
        QCOMPARE(hashFile(source), sha); QCOMPARE(hashFile(englishModel()), sha);
        QVERIFY(storageEntries(store).isEmpty());
    }

    void multilingualHeaderOverridesMisleadingEnglishFilename() {
        if (resourceRoot().isEmpty()) QSKIP("Set SUBTITLEDESK_RESOURCES for real-model integration.");
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const QString alias = temp.filePath(QStringLiteral("custom.en.bin"));
        QVERIFY(QFile::copy(tinyModel(), alias));
        const QString bundledHash = hashFile(tinyModel());
        ModelCatalog catalog(resourceRoot(), temp.filePath(QStringLiteral("models")));
        const auto result = importAndWait(catalog, alias);
        QCOMPARE(result.kind, ImportResult::Imported); QVERIFY(!catalog.model(result.id).englishOnly);
        QCOMPARE(catalog.model(result.id).engine, QStringLiteral("whisper"));
        QCOMPARE(catalog.model(QStringLiteral("tiny")).path, tinyModel());
        QCOMPARE(hashFile(tinyModel()), bundledHash);
    }

    void importedWhistleRetainsItsEngineFamily() {
        if (resourceRoot().isEmpty()) QSKIP("Set SUBTITLEDESK_RESOURCES for real-model integration.");
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const QString original = QDir(resourceRoot()).filePath(QStringLiteral("models/whistle.cact"));
        const QString copy = temp.filePath(QStringLiteral("local-speech.cact"));
        QVERIFY(QFile::copy(original, copy)); const QString originalHash = hashFile(original);
        ModelCatalog catalog(resourceRoot(), temp.filePath(QStringLiteral("models")));
        const auto result = importAndWait(catalog, copy, QStringLiteral("My Whistle model"));
        QCOMPARE(result.kind, ImportResult::Imported); QVERIFY(!result.busyAtFinish);
        const auto imported = catalog.model(result.id);
        QCOMPARE(imported.engine, QStringLiteral("whistle")); QVERIFY(!imported.englishOnly);
        QVERIFY(imported.path.endsWith(QStringLiteral("/model.cact")));
        QVERIFY2(catalog.availability(result.id).isEmpty(), qPrintable(catalog.availability(result.id)));
        QCOMPARE(hashFile(imported.path), originalHash);
        QString error; QVERIFY2(catalog.removeModel(result.id, &error), qPrintable(error));
        QCOMPARE(hashFile(copy), originalHash); QCOMPARE(hashFile(original), originalHash);
    }

    void failedAndCancelledImportLeaveExistingModelsIntact() {
        if (resourceRoot().isEmpty() || englishModel().isEmpty())
            QSKIP("Set SUBTITLEDESK_RESOURCES and SUBTITLEDESK_TEST_IMPORT_MODEL for real-model integration.");
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const QString store = temp.filePath(QStringLiteral("models"));
        ModelCatalog catalog(resourceRoot(), store);
        const auto first = importAndWait(catalog, englishModel());
        QCOMPARE(first.kind, ImportResult::Imported);
        const ModelInfo installed = catalog.model(first.id);
        const QString before = hashFile(installed.path), bundledBefore = hashFile(tinyModel());
        const auto cancelled = importAndWait(catalog, tinyModel(), {}, true);
        QCOMPARE(cancelled.kind, ImportResult::Cancelled); QVERIFY(!cancelled.busyAtFinish);
        QCOMPARE(installedIds(catalog), QStringList{first.id});
        QCOMPARE(storageEntries(store), QStringList{first.id});
        // A valid model's header with missing tensor data must fail validation;
        // a header-only format check is not sufficient to install it.
        QFile original(tinyModel()); QVERIFY(original.open(QIODevice::ReadOnly));
        const QString truncated = temp.filePath(QStringLiteral("truncated-weights.bin"));
        QVERIFY(writeBytes(truncated, original.read(1024 * 1024)));
        const auto failed = importAndWait(catalog, truncated);
        QCOMPARE(failed.kind, ImportResult::Failed); QVERIFY(!failed.error.isEmpty());
        QVERIFY(!failed.busyAtFinish); QCOMPARE(installedIds(catalog), QStringList{first.id});
        QCOMPARE(storageEntries(store), QStringList{first.id});
        QCOMPARE(hashFile(installed.path), before); QCOMPARE(hashFile(tinyModel()), bundledBefore);
        QCOMPARE(hashFile(englishModel()), before);
    }

    void concurrentImportRequestDoesNotTerminateTheActiveOperation() {
        if (resourceRoot().isEmpty()) QSKIP("Set SUBTITLEDESK_RESOURCES for asynchronous import integration.");
        QTemporaryDir temp; QVERIFY(temp.isValid());
        ModelCatalog catalog(resourceRoot(), temp.path());
        QSignalSpy imported(&catalog,&ModelCatalog::modelImported),failed(&catalog,&ModelCatalog::importFailed),
            cancelled(&catalog,&ModelCatalog::importCancelled);
        catalog.importModel(tinyModel()); QVERIFY(catalog.busy());
        catalog.importModel(temp.filePath(QStringLiteral("another-request.bin")));
        QVERIFY(catalog.busy()); QVERIFY(failed.isEmpty()); QVERIFY(imported.isEmpty());
        catalog.cancelImport();
        QTRY_COMPARE_WITH_TIMEOUT(cancelled.count(),1,10000);
        QVERIFY(!catalog.busy()); QVERIFY(failed.isEmpty()); QVERIFY(imported.isEmpty());
        QCOMPARE(catalog.models().size(),4); QVERIFY(storageEntries(temp.path()).isEmpty());
    }

    void cancellationAndDestructionDuringProbeCleanTemporaryFiles_data() {
        QTest::addColumn<bool>("destroyCatalog");
        QTest::newRow("cancel") << false;
        QTest::newRow("destroy") << true;
    }
    void cancellationAndDestructionDuringProbeCleanTemporaryFiles() {
        QFETCH(bool, destroyCatalog);
        if (resourceRoot().isEmpty()) QSKIP("Set SUBTITLEDESK_RESOURCES for native-engine cancellation integration.");
        QTemporaryDir temp; QVERIFY(temp.isValid());
        auto catalog = std::make_unique<ModelCatalog>(resourceRoot(), temp.path());
        const QString sourceHash = hashFile(tinyModel());
        QEventLoop loop; QTimer timeout; timeout.setSingleShot(true);
        bool reachedProbe = false, terminalCancelled = false, installed = false, timedOut = false;
        QString failure;
        QObject::connect(catalog.get(), &ModelCatalog::importProgress, &loop, [&](int percent, QString) {
            if (percent != 85 || reachedProbe) return;
            reachedProbe = true;
            if (destroyCatalog) { catalog.reset(); loop.quit(); }
            else catalog->cancelImport();
        });
        QObject::connect(catalog.get(), &ModelCatalog::importCancelled, &loop, [&] { terminalCancelled = true; loop.quit(); });
        QObject::connect(catalog.get(), &ModelCatalog::modelImported, &loop, [&](QString, bool) { installed = true; loop.quit(); });
        QObject::connect(catalog.get(), &ModelCatalog::importFailed, &loop, [&](QString error) { failure = error; loop.quit(); });
        QObject::connect(&timeout, &QTimer::timeout, &loop, [&] { timedOut = true; if (catalog) catalog->cancelImport(); loop.quit(); });
        timeout.start(120000); catalog->importModel(tinyModel()); loop.exec();
        QVERIFY2(!timedOut, "Timed out before reaching or cancelling the native model probe");
        QVERIFY2(failure.isEmpty(), qPrintable(failure)); QVERIFY(reachedProbe); QVERIFY(!installed);
        if (!destroyCatalog) { QVERIFY(terminalCancelled); QVERIFY(!catalog->busy()); QCOMPARE(catalog->models().size(), 4); }
        QCoreApplication::processEvents();
        QVERIFY(storageEntries(temp.path()).isEmpty()); QCOMPARE(hashFile(tinyModel()), sourceHash);
    }

    void anotherCatalogCannotRemoveAnAcquiredModel() {
        if (resourceRoot().isEmpty() || englishModel().isEmpty())
            QSKIP("Set SUBTITLEDESK_RESOURCES and SUBTITLEDESK_TEST_IMPORT_MODEL for shared-storage integration.");
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const QJsonObject metadata = metadataFor(englishModel());
        const QString id = metadata.value(QStringLiteral("id")).toString();
        QVERIFY(stagePersistedModel(QDir(temp.path()).filePath(id), englishModel(), metadata));
        auto recognizing = std::make_unique<ModelCatalog>(resourceRoot(), temp.path());
        ModelCatalog managing(resourceRoot(), temp.path());
        ModelInfo selected; QString error;
        QVERIFY2(recognizing->acquireModel(id, &selected, &error), qPrintable(error));
        QVERIFY(recognizing->acquireModel(id, &selected, &error));
        QVERIFY(!managing.removeModel(id, &error)); QVERIFY(!error.isEmpty()); QVERIFY(QFileInfo(selected.path).isFile());
        recognizing->releaseModel(id); QVERIFY(!managing.removeModel(id, &error));
        // Destruction must release the remaining lease so a second catalog
        // does not leave a permanently undeletable model after an app closes.
        recognizing.reset();
        QVERIFY2(managing.removeModel(id, &error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(selected.path)); QCOMPARE(managing.models().size(), 4);
    }

    void persistedMetadataCannotEscapeStorage_data() {
        QTest::addColumn<QString>("mutation");
        for (const char *name : {"path-fields", "traversal-id", "bundled-id", "engine", "sha256", "size", "language"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void persistedMetadataCannotEscapeStorage() {
        QFETCH(QString, mutation);
        if (englishModel().isEmpty()) QSKIP("Set SUBTITLEDESK_TEST_IMPORT_MODEL for persisted-model validation.");
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const QString outside = temp.filePath(QStringLiteral("outside.bin"));
        const QByteArray sentinel("unrelated user data must survive"); QVERIFY(writeBytes(outside, sentinel));
        QJsonObject metadata = metadataFor(englishModel());
        const QString id = metadata.value(QStringLiteral("id")).toString();
        const QString store = temp.filePath(QStringLiteral("models"));
        const QString directory = QDir(store).filePath(id);
        if (mutation == QStringLiteral("path-fields")) {
            for (const char *key : {"path", "sourcePath", "file", "fileName", "modelFile"}) metadata[QString::fromLatin1(key)] = outside;
        } else if (mutation == QStringLiteral("traversal-id")) metadata["id"] = QStringLiteral("../../outside.bin");
        else if (mutation == QStringLiteral("bundled-id")) metadata["id"] = QStringLiteral("base");
        else if (mutation == QStringLiteral("engine")) metadata["engine"] = QStringLiteral("../../outside.bin");
        else if (mutation == QStringLiteral("sha256")) metadata["sha256"] = QString(64, QLatin1Char('0'));
        else if (mutation == QStringLiteral("size")) metadata["size"] = 1;
        else if (mutation == QStringLiteral("language")) metadata["englishOnly"] = false;
        QVERIFY(stagePersistedModel(directory, englishModel(), metadata));
        ModelCatalog catalog(resourceRoot(), store);
        QCOMPARE(catalog.model(QStringLiteral("base")).id, QStringLiteral("base"));
        QVERIFY(catalog.model(QStringLiteral("base")).bundled);
        const ModelInfo loaded = catalog.model(id);
        if (mutation == QStringLiteral("path-fields") || mutation == QStringLiteral("language")) {
            // An unknown path field may be ignored or the record rejected.
            // If accepted, only the catalog-owned model file is usable.
            if (!loaded.id.isEmpty()) {
                QCOMPARE(QFileInfo(loaded.path).canonicalFilePath(),
                         QFileInfo(QDir(directory).filePath(QStringLiteral("model.bin"))).canonicalFilePath());
                QVERIFY(loaded.englishOnly); QCOMPARE(loaded.engine, QStringLiteral("whisper"));
            }
        } else QVERIFY(loaded.id.isEmpty());
        QString error;
        QVERIFY(!catalog.removeModel(QStringLiteral("../../outside.bin"), &error));
        QVERIFY(!catalog.removeModel(QStringLiteral("base"), &error));
        if (!loaded.id.isEmpty()) QVERIFY(catalog.removeModel(id, &error));
        QFile outsideFile(outside); QVERIFY(outsideFile.open(QIODevice::ReadOnly));
        QCOMPARE(outsideFile.readAll(), sentinel);
    }

    void linkedEntriesCannotExposeOrDeleteExternalFiles_data() {
        QTest::addColumn<QString>("linkKind");
        QTest::newRow("directory") << QStringLiteral("directory");
        QTest::newRow("model") << QStringLiteral("model");
        QTest::newRow("metadata") << QStringLiteral("metadata");
    }
    void linkedEntriesCannotExposeOrDeleteExternalFiles() {
#ifndef Q_OS_UNIX
        QSKIP("This test exercises Unix symbolic links, not Windows shortcut files.");
#else
        QFETCH(QString, linkKind);
        if (englishModel().isEmpty()) QSKIP("Set SUBTITLEDESK_TEST_IMPORT_MODEL for symlink confinement validation.");
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const QJsonObject metadata = metadataFor(englishModel());
        const QString id = metadata.value(QStringLiteral("id")).toString();
        const QString store = temp.filePath(QStringLiteral("models"));
        const QString directory = QDir(store).filePath(id);
        const QString external = temp.filePath(QStringLiteral("external"));
        QVERIFY(stagePersistedModel(external, englishModel(), metadata));
        const QString externalModel = QDir(external).filePath(QStringLiteral("model.bin"));
        const QString externalMetadata = QDir(external).filePath(QStringLiteral("metadata.json"));
        const QString modelHash = hashFile(externalModel), metadataHash = hashFile(externalMetadata);
        QVERIFY(QDir().mkpath(store));
        if (linkKind == QStringLiteral("directory")) QVERIFY(QFile::link(external, directory));
        else {
            QVERIFY(QDir().mkpath(directory));
            if (linkKind == QStringLiteral("model")) {
                QVERIFY(QFile::link(externalModel, QDir(directory).filePath(QStringLiteral("model.bin"))));
                QVERIFY(writeMetadata(directory, metadata));
            } else {
                QVERIFY(QFile::copy(externalModel, QDir(directory).filePath(QStringLiteral("model.bin"))));
                QVERIFY(QFile::link(externalMetadata, QDir(directory).filePath(QStringLiteral("metadata.json"))));
            }
        }
        ModelCatalog catalog(resourceRoot(), store);
        QVERIFY(catalog.model(id).id.isEmpty()); QString error;
        QVERIFY(!catalog.removeModel(id, &error));
        QCOMPARE(hashFile(externalModel), modelHash); QCOMPARE(hashFile(externalMetadata), metadataHash);
#endif
    }

    void sourceLinksAreRejectedOrCopiedIntoPrivateStorage() {
#ifndef Q_OS_UNIX
        QSKIP("This test exercises Unix symbolic links, not Windows shortcut files.");
#else
        if (resourceRoot().isEmpty() || englishModel().isEmpty())
            QSKIP("Set SUBTITLEDESK_RESOURCES and SUBTITLEDESK_TEST_IMPORT_MODEL for real-model integration.");
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const QString link = temp.filePath(QStringLiteral("linked-model.bin"));
        const QString store = temp.filePath(QStringLiteral("models"));
        QVERIFY(QFile::link(englishModel(), link));
        const QString sourceHash = hashFile(englishModel());
        ModelCatalog catalog(resourceRoot(), store);
        const auto result = importAndWait(catalog, link);
        QVERIFY(result.kind == ImportResult::Imported || result.kind == ImportResult::Failed);
        if (result.kind == ImportResult::Imported) {
            const ModelInfo model = catalog.model(result.id);
            QVERIFY(!QFileInfo(model.path).isSymLink());
            QVERIFY(QFileInfo(model.path).canonicalFilePath().startsWith(QDir(store).canonicalPath() + QLatin1Char('/')));
            QCOMPARE(hashFile(model.path), sourceHash); QString error;
            QVERIFY(catalog.removeModel(result.id, &error));
        }
        QVERIFY(storageEntries(store).isEmpty()); QCOMPARE(hashFile(englishModel()), sourceHash);
#endif
    }
};

QTEST_GUILESS_MAIN(ModelCatalogTest)
#include "test_model_catalog.moc"
