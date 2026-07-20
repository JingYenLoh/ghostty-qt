#include "ghostty_config_service.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaType>
#include <QPointer>
#include <QSet>

#include <algorithm>
#include <utility>

namespace {

QStringList uniquePathsInOrder(const QStringList &paths)
{
    QStringList result;
    QSet<QString> seen;
    for (const QString &path : paths) {
        if (!path.isEmpty() && !seen.contains(path)) {
            seen.insert(path);
            result.append(path);
        }
    }
    return result;
}

void synchronizePaths(QFileSystemWatcher *watcher,
                      const QStringList &current,
                      const QStringList &desired)
{
    QStringList removed;
    for (const QString &path : current) {
        if (!desired.contains(path)) {
            removed.append(path);
        }
    }
    if (!removed.isEmpty()) {
        watcher->removePaths(removed);
    }

    QStringList added;
    for (const QString &path : desired) {
        if (!current.contains(path)) {
            added.append(path);
        }
    }
    if (!added.isEmpty()) {
        watcher->addPaths(added);
    }
}

} // namespace

GhosttyConfigService::GhosttyConfigService(GhosttyConfigLoader loader,
                                           QObject *parent)
    : GhosttyConfigService(standardConfigPaths(), std::move(loader),
                           DefaultDebounceMilliseconds, true, parent)
{
}

GhosttyConfigService::GhosttyConfigService(QStringList candidatePaths,
                                           GhosttyConfigLoader loader,
                                           int debounceMilliseconds,
                                           QObject *parent)
    : GhosttyConfigService(std::move(candidatePaths), std::move(loader),
                           debounceMilliseconds, false, parent)
{
}

GhosttyConfigService::GhosttyConfigService(QStringList candidatePaths,
                                           GhosttyConfigLoader loader,
                                           int debounceMilliseconds,
                                           bool asynchronousReloads,
                                           QObject *parent)
    : QObject(parent)
    , loader_(std::move(loader))
    , asynchronousReloads_(asynchronousReloads)
{
    candidatePaths_.reserve(candidatePaths.size());
    for (const QString &path : std::as_const(candidatePaths)) {
        candidatePaths_.append(normalizedAbsolutePath(path));
    }
    candidatePaths_ = uniquePathsInOrder(candidatePaths_);

    qRegisterMetaType<GhosttyConfigSnapshot>();

    reloadPool_.setMaxThreadCount(1);
    reloadPool_.setExpiryTimeout(-1);

    debounceTimer_.setSingleShot(true);
    debounceTimer_.setInterval(std::max(0, debounceMilliseconds));
    connect(&debounceTimer_, &QTimer::timeout, this, [this] {
        if (asynchronousReloads_) {
            beginAsyncReload();
        } else {
            reloadNow();
        }
    });
    failureRetryTimer_.setSingleShot(true);
    failureRetryTimer_.setInterval(5'000);
    connect(&failureRetryTimer_, &QTimer::timeout,
            this, &GhosttyConfigService::requestReload);
    connect(&watcher_, &QFileSystemWatcher::fileChanged,
            this, &GhosttyConfigService::watchedPathChanged);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged,
            this, &GhosttyConfigService::watchedPathChanged);

    refreshWatchPaths();
    reloadNow();
}

GhosttyConfigService::~GhosttyConfigService()
{
    debounceTimer_.stop();
    failureRetryTimer_.stop();
    // A worker captures `this` only long enough to enqueue a QObject-targeted
    // callback. Joining here keeps the target alive through that enqueue;
    // callbacks still queued afterward are discarded by QObject teardown.
    reloadPool_.waitForDone();
}

QStringList GhosttyConfigService::standardConfigPaths(
    const QProcessEnvironment &environment)
{
    QString configHome = environment.value(QStringLiteral("XDG_CONFIG_HOME"));
    if (configHome.isEmpty() || !QDir::isAbsolutePath(configHome)) {
        QString home = environment.value(QStringLiteral("HOME"));
        if (home.isEmpty()) {
            home = QDir::homePath();
        }
        configHome = QDir(home).filePath(QStringLiteral(".config"));
    }

    const QString ghosttyDirectory =
        QDir(configHome).filePath(QStringLiteral("ghostty"));
    return {
        QDir(ghosttyDirectory).filePath(QStringLiteral("config")),
        QDir(ghosttyDirectory).filePath(QStringLiteral("config.ghostty")),
    };
}

const GhosttyConfigSnapshot &GhosttyConfigService::snapshot() const
{
    Q_ASSERT(snapshot_.has_value());
    return *snapshot_;
}

QStringList GhosttyConfigService::watchedFiles() const
{
    QStringList paths = watcher_.files();
    paths.sort();
    return paths;
}

QStringList GhosttyConfigService::watchedDirectories() const
{
    QStringList paths = watcher_.directories();
    paths.sort();
    return paths;
}

void GhosttyConfigService::requestReload()
{
    debounceTimer_.start();
}

void GhosttyConfigService::reloadNow()
{
    debounceTimer_.stop();
    refreshWatchPaths();

    // A deterministic synchronous reload supersedes any worker result that
    // was started earlier. The worker is still joined normally, but its stale
    // snapshot must not overwrite this one when its queued callback arrives.
    ++loadGeneration_;

    if (!loader_) {
        applyLoadResult(std::unexpected(
            QStringLiteral("Ghostty configuration loader is unavailable")));
        return;
    }

    GhosttyConfigLoadResult result = [&] {
        QMutexLocker locker(&loaderMutex_);
        return loader_(candidatePaths_);
    }();
    applyLoadResult(std::move(result));
}

void GhosttyConfigService::applyLoadResult(GhosttyConfigLoadResult result)
{
    if (!result) {
        const QString &error = result.error();
        const QString message = error.isEmpty()
            ? QStringLiteral("Ghostty configuration reload failed")
            : error;
        lastError_ = message;
        refreshWatchPaths();
        if (!failureRetryTimer_.isActive()) {
            failureRetryTimer_.start();
        }
        // A direct subscriber is allowed to delete this service. Keep the
        // emitted value off-object and do not access members after emission.
        Q_EMIT reloadFailed(message);
        return;
    }

    failureRetryTimer_.stop();
    lastError_.clear();
    snapshot_ = std::move(*result);
    refreshWatchPaths();
    // As above, signal handlers may synchronously delete the sender. Every
    // successful reload is published even when its values compare equal:
    // runtime-only surface actions must be replaced by configured state.
    const GhosttyConfigSnapshot published = *snapshot_;
    Q_EMIT changed(published);
}

void GhosttyConfigService::beginAsyncReload()
{
    debounceTimer_.stop();
    refreshWatchPaths();
    if (loadInProgress_) {
        reloadPending_ = true;
        return;
    }
    if (!loader_) {
        applyLoadResult(std::unexpected(
            QStringLiteral("Ghostty configuration loader is unavailable")));
        return;
    }

    loadInProgress_ = true;
    const quint64 generation = ++loadGeneration_;
    const GhosttyConfigLoader loader = loader_;
    const QStringList candidates = candidatePaths_;
    GhosttyConfigService *const self = this;
    QMutex *const loaderMutex = &loaderMutex_;
    reloadPool_.start(
        [self, loader, candidates, generation, loaderMutex] {
            GhosttyConfigLoadResult result = [&] {
                QMutexLocker locker(loaderMutex);
                return loader(candidates);
            }();
            QMetaObject::invokeMethod(
                self,
                [self, generation, result = std::move(result)]() mutable {
                    const QPointer<GhosttyConfigService> guard(self);
                    self->loadInProgress_ = false;
                    if (generation == self->loadGeneration_) {
                        self->applyLoadResult(std::move(result));
                    }
                    if (!guard) {
                        return;
                    }
                    if (self->reloadPending_) {
                        self->reloadPending_ = false;
                        self->beginAsyncReload();
                    }
                },
                Qt::QueuedConnection);
        });
}

QString GhosttyConfigService::normalizedAbsolutePath(const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString GhosttyConfigService::closestExistingDirectory(const QString &path)
{
    QString directory = QDir::cleanPath(path);
    while (!directory.isEmpty() && !QFileInfo(directory).isDir()) {
        const QString parent = QFileInfo(directory).absolutePath();
        if (parent == directory) {
            return {};
        }
        directory = parent;
    }
    return directory;
}

void GhosttyConfigService::refreshWatchPaths()
{
    QStringList desiredFiles;
    QStringList desiredDirectories;

    QStringList watchedSources = candidatePaths_;
    if (snapshot_.has_value()) {
        watchedSources.append(snapshot_->sourcePaths);
        const QStringList configuredFiles =
            snapshot_->values.value(QStringLiteral("config-file")).toStringList();
        for (QString path : configuredFiles) {
            if (path.startsWith(u'?')) {
                path.remove(0, 1);
            }
            watchedSources.append(normalizedAbsolutePath(path));
        }
    }
    watchedSources = uniquePathsInOrder(watchedSources);

    for (const QString &path : std::as_const(watchedSources)) {
        const QFileInfo file(path);
        if (file.isFile()) {
            desiredFiles.append(path);
        }

        const QString directory = closestExistingDirectory(file.absolutePath());
        if (!directory.isEmpty()) {
            desiredDirectories.append(directory);
        }
    }

    desiredFiles = uniquePathsInOrder(desiredFiles);
    desiredDirectories = uniquePathsInOrder(desiredDirectories);
    synchronizePaths(&watcher_, watcher_.files(), desiredFiles);
    synchronizePaths(&watcher_, watcher_.directories(), desiredDirectories);
}

void GhosttyConfigService::watchedPathChanged(const QString &path)
{
    Q_UNUSED(path)
    // Atomic replacement can remove a file watch. Refresh immediately and
    // again at reload time, after the writer has finished renaming files.
    refreshWatchPaths();
    requestReload();
}
