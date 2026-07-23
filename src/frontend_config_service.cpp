#include "frontend_config_service.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaType>
#include <QPointer>

#include <algorithm>
#include <utility>

namespace {

void synchronizePaths(QFileSystemWatcher *watcher, const QStringList &current,
                      const QStringList &desired)
{
    QStringList removed;
    for (const QString &path : current) {
        if (!desired.contains(path)) removed.append(path);
    }
    if (!removed.isEmpty()) watcher->removePaths(removed);

    QStringList added;
    for (const QString &path : desired) {
        if (!current.contains(path)) added.append(path);
    }
    if (!added.isEmpty()) watcher->addPaths(added);
}

} // namespace

FrontendConfigService::FrontendConfigService(QObject *parent)
    : FrontendConfigService(
          standardConfigPath(),
          [](const QString &path) { return loadFrontendConfigFile(path); },
          DefaultDebounceMilliseconds, parent)
{}

FrontendConfigService::FrontendConfigService(QString path,
                                             FrontendConfigLoader loader,
                                             int debounceMilliseconds,
                                             QObject *parent)
    : QObject(parent)
    , configPath_(normalizedAbsolutePath(path))
    , loader_(std::move(loader))
{
    qRegisterMetaType<FrontendConfigSnapshot>();

    reloadPool_.setMaxThreadCount(1);
    reloadPool_.setExpiryTimeout(-1);

    debounceTimer_.setSingleShot(true);
    debounceTimer_.setInterval(std::max(0, debounceMilliseconds));
    connect(&debounceTimer_, &QTimer::timeout, this,
            &FrontendConfigService::beginAsyncReload);
    failureRetryTimer_.setSingleShot(true);
    failureRetryTimer_.setInterval(5'000);
    connect(&failureRetryTimer_, &QTimer::timeout, this,
            &FrontendConfigService::requestReload);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this,
            &FrontendConfigService::watchedPathChanged);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this,
            &FrontendConfigService::watchedPathChanged);

    refreshWatchPaths();
    reloadNow();
}

FrontendConfigService::~FrontendConfigService()
{
    debounceTimer_.stop();
    failureRetryTimer_.stop();
    reloadPool_.waitForDone();
}

QString FrontendConfigService::standardConfigPath(
    const QProcessEnvironment &environment)
{
    QString configHome = environment.value(QStringLiteral("XDG_CONFIG_HOME"));
    if (configHome.isEmpty() || !QDir::isAbsolutePath(configHome)) {
        QString home = environment.value(QStringLiteral("HOME"));
        if (home.isEmpty()) home = QDir::homePath();
        configHome = QDir(home).filePath(QStringLiteral(".config"));
    }
    return QDir(configHome).filePath(QStringLiteral("ghostty-qt/config"));
}

const FrontendConfigSnapshot &FrontendConfigService::snapshot() const
{
    Q_ASSERT(snapshot_.has_value());
    return *snapshot_;
}

QStringList FrontendConfigService::watchedFiles() const
{
    QStringList paths = watcher_.files();
    paths.sort();
    return paths;
}

QStringList FrontendConfigService::watchedDirectories() const
{
    QStringList paths = watcher_.directories();
    paths.sort();
    return paths;
}

void FrontendConfigService::requestReload()
{
    debounceTimer_.start();
}

void FrontendConfigService::reloadNow()
{
    debounceTimer_.stop();
    refreshWatchPaths();
    ++loadGeneration_;

    if (!loader_) {
        applyLoadResult(std::unexpected(
            QStringLiteral("Frontend configuration loader is unavailable")));
        return;
    }

    FrontendConfigLoadResult result = [&] {
        QMutexLocker locker(&loaderMutex_);
        return loader_(configPath_);
    }();
    applyLoadResult(std::move(result));
}

void FrontendConfigService::applyLoadResult(FrontendConfigLoadResult result)
{
    if (!result) {
        const QString message = result.error().isEmpty()
            ? QStringLiteral("Frontend configuration reload failed")
            : result.error();
        lastError_ = message;
        refreshWatchPaths();
        if (!failureRetryTimer_.isActive()) failureRetryTimer_.start();
        Q_EMIT reloadFailed(message);
        return;
    }

    failureRetryTimer_.stop();
    lastError_.clear();
    snapshot_ = std::move(*result);
    refreshWatchPaths();
    const FrontendConfigSnapshot published = *snapshot_;
    Q_EMIT changed(published);
}

void FrontendConfigService::beginAsyncReload()
{
    debounceTimer_.stop();
    refreshWatchPaths();
    if (loadInProgress_) {
        reloadPending_ = true;
        return;
    }
    if (!loader_) {
        applyLoadResult(std::unexpected(
            QStringLiteral("Frontend configuration loader is unavailable")));
        return;
    }

    loadInProgress_ = true;
    const quint64 generation = ++loadGeneration_;
    const FrontendConfigLoader loader = loader_;
    const QString path = configPath_;
    FrontendConfigService *const self = this;
    QMutex *const loaderMutex = &loaderMutex_;
    reloadPool_.start([self, loader, path, generation, loaderMutex] {
        FrontendConfigLoadResult result = [&] {
            QMutexLocker locker(loaderMutex);
            return loader(path);
        }();
        QMetaObject::invokeMethod(
            self,
            [self, generation, result = std::move(result)]() mutable {
                const QPointer<FrontendConfigService> guard(self);
                self->loadInProgress_ = false;
                if (generation == self->loadGeneration_) {
                    self->applyLoadResult(std::move(result));
                }
                if (!guard) return;
                if (self->reloadPending_) {
                    self->reloadPending_ = false;
                    self->beginAsyncReload();
                }
            },
            Qt::QueuedConnection);
    });
}

QString FrontendConfigService::normalizedAbsolutePath(const QString &path)
{
    if (path.isEmpty()) return {};
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString FrontendConfigService::closestExistingDirectory(const QString &path)
{
    QString directory = QDir::cleanPath(path);
    while (!directory.isEmpty() && !QFileInfo(directory).isDir()) {
        const QString parent = QFileInfo(directory).absolutePath();
        if (parent == directory) return {};
        directory = parent;
    }
    return directory;
}

void FrontendConfigService::refreshWatchPaths()
{
    QStringList desiredFiles;
    QStringList desiredDirectories;
    const QFileInfo file(configPath_);
    if (file.isFile()) desiredFiles.append(configPath_);

    const QString directory = closestExistingDirectory(file.absolutePath());
    if (!directory.isEmpty()) desiredDirectories.append(directory);

    synchronizePaths(&watcher_, watcher_.files(), desiredFiles);
    synchronizePaths(&watcher_, watcher_.directories(), desiredDirectories);
}

void FrontendConfigService::watchedPathChanged(const QString &path)
{
    Q_UNUSED(path)
    refreshWatchPaths();
    requestReload();
}
