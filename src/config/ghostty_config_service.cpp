#include "config/ghostty_config_service.h"

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

void synchronizePaths(QFileSystemWatcher *watcher, const QStringList &current,
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
    : GhosttyConfigService(std::move(loader), TerminalColorScheme::Light,
                           parent)
{}

GhosttyConfigService::GhosttyConfigService(
    GhosttyConfigLoader loader, TerminalColorScheme initialColorScheme,
    QObject *parent)
    : GhosttyConfigService(standardConfigPaths(), std::move(loader),
                           DefaultDebounceMilliseconds, initialColorScheme,
                           true, true, InitialFailureRetryMilliseconds,
                           MaximumFailureRetryMilliseconds, parent)
{}

GhosttyConfigService::GhosttyConfigService(
    GhosttyConfigLoader loader, TerminalColorScheme initialColorScheme,
    bool watchDefaultConfigCandidates, QObject *parent)
    : GhosttyConfigService(standardConfigPaths(), std::move(loader),
                           DefaultDebounceMilliseconds, initialColorScheme,
                           true, watchDefaultConfigCandidates,
                           InitialFailureRetryMilliseconds,
                           MaximumFailureRetryMilliseconds, parent)
{}

GhosttyConfigService::GhosttyConfigService(QStringList candidatePaths,
                                           GhosttyConfigLoader loader,
                                           int debounceMilliseconds,
                                           QObject *parent)
    : GhosttyConfigService(std::move(candidatePaths), std::move(loader),
                           debounceMilliseconds, TerminalColorScheme::Light,
                           false, true, InitialFailureRetryMilliseconds,
                           MaximumFailureRetryMilliseconds, parent)
{}

GhosttyConfigService::GhosttyConfigService(
    QStringList candidatePaths, GhosttyConfigLoader loader,
    int debounceMilliseconds, TerminalColorScheme initialColorScheme,
    QObject *parent)
    : GhosttyConfigService(std::move(candidatePaths), std::move(loader),
                           debounceMilliseconds, initialColorScheme, false,
                           true, InitialFailureRetryMilliseconds,
                           MaximumFailureRetryMilliseconds, parent)
{}

GhosttyConfigService::GhosttyConfigService(
    QStringList candidatePaths, GhosttyConfigLoader loader,
    int debounceMilliseconds, TerminalColorScheme initialColorScheme,
    bool watchDefaultConfigCandidates, QObject *parent)
    : GhosttyConfigService(std::move(candidatePaths), std::move(loader),
                           debounceMilliseconds, initialColorScheme, false,
                           watchDefaultConfigCandidates,
                           InitialFailureRetryMilliseconds,
                           MaximumFailureRetryMilliseconds, parent)
{}

GhosttyConfigService::GhosttyConfigService(QStringList candidatePaths,
                                           GhosttyConfigLoader loader,
                                           int debounceMilliseconds,
                                           int initialFailureRetryMilliseconds,
                                           int maximumFailureRetryMilliseconds,
                                           QObject *parent)
    : GhosttyConfigService(std::move(candidatePaths), std::move(loader),
                           debounceMilliseconds, TerminalColorScheme::Light,
                           false, true, initialFailureRetryMilliseconds,
                           maximumFailureRetryMilliseconds, parent)
{}

GhosttyConfigService::GhosttyConfigService(
    QStringList candidatePaths, GhosttyConfigLoader loader,
    int debounceMilliseconds, TerminalColorScheme initialColorScheme,
    bool asynchronousReloads, bool watchDefaultConfigCandidates,
    int initialFailureRetryMilliseconds, int maximumFailureRetryMilliseconds,
    QObject *parent)
    : QObject(parent)
    , colorScheme_(initialColorScheme)
    , loader_(std::move(loader))
    , watchDefaultConfigCandidates_(watchDefaultConfigCandidates)
    , asynchronousReloads_(asynchronousReloads)
    , initialFailureRetryMilliseconds_(
          std::max(1, initialFailureRetryMilliseconds))
    , maximumFailureRetryMilliseconds_(std::max(
          initialFailureRetryMilliseconds_, maximumFailureRetryMilliseconds))
    , nextFailureRetryMilliseconds_(initialFailureRetryMilliseconds_)
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
            reloadSynchronously();
        }
    });
    failureRetryTimer_.setSingleShot(true);
    connect(&failureRetryTimer_, &QTimer::timeout, this,
            &GhosttyConfigService::scheduleReload);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this,
            &GhosttyConfigService::watchedPathChanged);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this,
            &GhosttyConfigService::watchedPathChanged);

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

QStringList GhosttyConfigService::standardConfigEditPaths(
    const QProcessEnvironment &environment)
{
    QString configHome = environment.value(QStringLiteral("XDG_CONFIG_HOME"));
    if (configHome.isEmpty()) {
        QString home = environment.value(QStringLiteral("HOME"));
        if (home.isEmpty()) home = QDir::homePath();
        configHome = QDir(home).filePath(QStringLiteral(".config"));
    }

    const QString ghosttyDirectory =
        QDir(configHome).filePath(QStringLiteral("ghostty"));
    return {
        QDir(ghosttyDirectory).filePath(QStringLiteral("config.ghostty")),
        QDir(ghosttyDirectory).filePath(QStringLiteral("config")),
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

int GhosttyConfigService::scheduledFailureRetryMilliseconds() const
{
    return failureRetryTimer_.isActive() ? failureRetryTimer_.interval() : 0;
}

void GhosttyConfigService::setColorScheme(TerminalColorScheme colorScheme)
{
    if (colorScheme_ == colorScheme) {
        return;
    }

    colorScheme_ = colorScheme;
    // Invalidate a worker result immediately rather than waiting for the
    // debounce timer. A blocked load must never publish a snapshot for a color
    // scheme that is no longer current.
    ++loadGeneration_;
    requestReload();
}

void GhosttyConfigService::requestReload()
{
    resetFailureRetryBackoff();
    scheduleReload();
}

void GhosttyConfigService::scheduleReload()
{
    const quint64 requestEpoch = requestEpoch_.advance();
    Q_EMIT reloadScheduled(requestEpoch);
    debounceTimer_.start();
}

void GhosttyConfigService::reloadNow()
{
    resetFailureRetryBackoff();
    reloadSynchronously();
}

void GhosttyConfigService::reloadSynchronously()
{
    debounceTimer_.stop();
    // This synchronous generation also supersedes a request that was queued
    // behind an in-flight worker. Its stale callback must not start another
    // load under the epoch that reloadNow is about to settle.
    reloadPending_ = false;
    refreshWatchPaths();

    // A deterministic synchronous reload supersedes any worker result that
    // was started earlier. The worker is still joined normally, but its stale
    // snapshot must not overwrite this one when its queued callback arrives.
    const quint64 requestEpoch = requestEpoch_.advance();
    Q_EMIT reloadScheduled(requestEpoch);
    ++loadGeneration_;

    if (!loader_) {
        applyLoadResult(std::unexpected(QStringLiteral(
                            "Ghostty configuration loader is unavailable")),
                        requestEpoch);
        return;
    }

    const GhosttyConfigLoadRequest request = loadRequest();
    GhosttyConfigLoadResult result = [&] {
        QMutexLocker locker(&loaderMutex_);
        return loader_(request);
    }();
    applyLoadResult(std::move(result), requestEpoch);
}

void GhosttyConfigService::applyLoadResult(GhosttyConfigLoadResult result,
                                           quint64 requestEpoch)
{
    if (!result) {
        const QString &error = result.error();
        const QString message = error.isEmpty()
            ? QStringLiteral("Ghostty configuration reload failed")
            : error;
        lastError_ = message;
        refreshWatchPaths();
        // A newer watcher/manual request already reset the backoff and owns
        // the next load. Do not let this older result arm a competing retry.
        if (requestEpoch == requestEpoch_.current()) {
            scheduleFailureRetry();
        }
        const QPointer<GhosttyConfigService> guard(this);
        Q_EMIT reloadSettled(requestEpoch);
        if (!guard) return;
        // A direct subscriber is allowed to delete this service. Keep the
        // emitted value off-object and do not access members after emission.
        Q_EMIT reloadFailed(message);
        return;
    }

    resetFailureRetryBackoff();
    lastError_.clear();
    snapshot_ = std::move(*result);
    refreshWatchPaths();
    // As above, signal handlers may synchronously delete the sender. Every
    // successful reload is published even when its values compare equal:
    // runtime-only surface actions must be replaced by configured state.
    const GhosttyConfigSnapshot published = *snapshot_;
    const QPointer<GhosttyConfigService> guard(this);
    Q_EMIT reloadSettled(requestEpoch);
    if (!guard) return;
    Q_EMIT changed(published);
}

void GhosttyConfigService::resetFailureRetryBackoff()
{
    failureRetryTimer_.stop();
    nextFailureRetryMilliseconds_ = initialFailureRetryMilliseconds_;
}

void GhosttyConfigService::scheduleFailureRetry()
{
    failureRetryTimer_.start(nextFailureRetryMilliseconds_);
    const qint64 doubled = qint64{nextFailureRetryMilliseconds_} * 2;
    nextFailureRetryMilliseconds_ = static_cast<int>(
        std::min<qint64>(maximumFailureRetryMilliseconds_, doubled));
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
        const quint64 requestEpoch = requestEpoch_.current();
        applyLoadResult(std::unexpected(QStringLiteral(
                            "Ghostty configuration loader is unavailable")),
                        requestEpoch);
        return;
    }

    loadInProgress_ = true;
    const quint64 generation = ++loadGeneration_;
    const quint64 requestEpoch = requestEpoch_.current();
    const GhosttyConfigLoader loader = loader_;
    const GhosttyConfigLoadRequest request = loadRequest();
    GhosttyConfigService *const self = this;
    QMutex *const loaderMutex = &loaderMutex_;
    reloadPool_.start(
        [self, loader, request, generation, requestEpoch, loaderMutex] {
            GhosttyConfigLoadResult result = [&] {
                QMutexLocker locker(loaderMutex);
                return loader(request);
            }();
            QMetaObject::invokeMethod(
                self,
                [self, generation, requestEpoch,
                 result = std::move(result)]() mutable {
                    const QPointer<GhosttyConfigService> guard(self);
                    self->loadInProgress_ = false;
                    if (generation == self->loadGeneration_) {
                        self->applyLoadResult(std::move(result), requestEpoch);
                    } else {
                        Q_EMIT self->reloadSettled(requestEpoch);
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

GhosttyConfigLoadRequest GhosttyConfigService::loadRequest() const
{
    return {
        .candidatePaths = candidatePaths_,
        .colorScheme = colorScheme_,
    };
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

    QStringList watchedSources;
    const bool watchDefaultCandidates = watchDefaultConfigCandidates_
        && (!snapshot_.has_value() || snapshot_->values.configDefaultFiles);
    if (watchDefaultCandidates) {
        watchedSources = candidatePaths_;
    }
    if (snapshot_.has_value()) {
        for (const QString &path : snapshot_->sourcePaths) {
            const QString normalized = normalizedAbsolutePath(path);
            if (watchDefaultCandidates
                || !candidatePaths_.contains(normalized)) {
                watchedSources.append(normalized);
            }
        }
        for (const GhosttyConfigFile &file : snapshot_->values.configFiles) {
            watchedSources.append(normalizedAbsolutePath(file.path));
        }
        for (const QString &path : snapshot_->values.themeFiles) {
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
