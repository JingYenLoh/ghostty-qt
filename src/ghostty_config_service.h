#pragma once

#include "ghostty_config_loader.h"
#include "revision_counter.h"

#include <QFileSystemWatcher>
#include <QMutex>
#include <QObject>
#include <QProcessEnvironment>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>

#include <optional>

class GhosttyConfigService : public QObject {
    Q_OBJECT

public:
    static constexpr int DefaultDebounceMilliseconds = 75;
    static constexpr int InitialFailureRetryMilliseconds = 5'000;
    static constexpr int MaximumFailureRetryMilliseconds = 5 * 60 * 1'000;

    explicit GhosttyConfigService(GhosttyConfigLoader loader,
                                  QObject *parent = nullptr);
    GhosttyConfigService(GhosttyConfigLoader loader,
                         TerminalColorScheme initialColorScheme,
                         QObject *parent = nullptr);
    // Startup-only CLI policy. False suppresses standard candidate watches
    // even before the first successful load; explicit recursive and theme
    // dependencies from later snapshots remain watched.
    GhosttyConfigService(GhosttyConfigLoader loader,
                         TerminalColorScheme initialColorScheme,
                         bool watchDefaultConfigCandidates,
                         QObject *parent = nullptr);
    GhosttyConfigService(QStringList candidatePaths, GhosttyConfigLoader loader,
                         int debounceMilliseconds, QObject *parent = nullptr);
    GhosttyConfigService(QStringList candidatePaths, GhosttyConfigLoader loader,
                         int debounceMilliseconds,
                         TerminalColorScheme initialColorScheme,
                         QObject *parent = nullptr);
    GhosttyConfigService(QStringList candidatePaths, GhosttyConfigLoader loader,
                         int debounceMilliseconds,
                         TerminalColorScheme initialColorScheme,
                         bool watchDefaultConfigCandidates,
                         QObject *parent = nullptr);
    // Custom retry bounds keep backoff behavior deterministic in contract
    // tests without weakening production retry intervals.
    GhosttyConfigService(QStringList candidatePaths, GhosttyConfigLoader loader,
                         int debounceMilliseconds,
                         int initialFailureRetryMilliseconds,
                         int maximumFailureRetryMilliseconds,
                         QObject *parent = nullptr);
    ~GhosttyConfigService() override;

    static QStringList
    standardConfigPaths(const QProcessEnvironment &environment =
                            QProcessEnvironment::systemEnvironment());
    // Ghostty loads the legacy path before config.ghostty, but its GUI edit
    // action examines those same files in the opposite order. Its edit-path
    // discovery also retains a non-empty relative XDG_CONFIG_HOME so the
    // subsequent absolute-file preparation fails as it does upstream.
    static QStringList
    standardConfigEditPaths(const QProcessEnvironment &environment =
                                QProcessEnvironment::systemEnvironment());

    const QStringList &candidatePaths() const { return candidatePaths_; }
    TerminalColorScheme colorScheme() const { return colorScheme_; }
    void setColorScheme(TerminalColorScheme colorScheme);
    bool hasSnapshot() const { return snapshot_.has_value(); }
    const GhosttyConfigSnapshot &snapshot() const;
    const QString &lastError() const { return lastError_; }

    // Exposed for diagnostics and deterministic contract tests. These lists
    // reflect paths QFileSystemWatcher currently accepted.
    QStringList watchedFiles() const;
    QStringList watchedDirectories() const;
    // Zero means no automatic failure retry is currently armed.
    int scheduledFailureRetryMilliseconds() const;

public Q_SLOTS:
    void requestReload();
    void reloadNow();

Q_SIGNALS:
    // The latest request epoch is carried through debouncing and async
    // coalescing so application readiness never completes an older reload.
    void reloadScheduled(quint64 requestEpoch);
    void reloadSettled(quint64 requestEpoch);
    // Published after every successful reload, including an unchanged
    // snapshot, so runtime-only surface overrides can be restored.
    void changed(const GhosttyConfigSnapshot &snapshot);
    void reloadFailed(const QString &message);

private:
    GhosttyConfigService(QStringList candidatePaths, GhosttyConfigLoader loader,
                         int debounceMilliseconds,
                         TerminalColorScheme initialColorScheme,
                         bool asynchronousReloads,
                         bool watchDefaultConfigCandidates,
                         int initialFailureRetryMilliseconds,
                         int maximumFailureRetryMilliseconds, QObject *parent);
    GhosttyConfigLoadRequest loadRequest() const;
    static QString normalizedAbsolutePath(const QString &path);
    static QString closestExistingDirectory(const QString &path);
    void scheduleReload();
    void resetFailureRetryBackoff();
    void scheduleFailureRetry();
    void reloadSynchronously();
    void refreshWatchPaths();
    void watchedPathChanged(const QString &path);
    void beginAsyncReload();
    void applyLoadResult(GhosttyConfigLoadResult result, quint64 requestEpoch);

    QStringList candidatePaths_;
    TerminalColorScheme colorScheme_;
    GhosttyConfigLoader loader_;
    const bool watchDefaultConfigCandidates_;
    // reloadNow() is intentionally synchronous, but it may supersede an
    // already-running watched reload. Serialize calls into an injected loader
    // so its captures do not need to be independently thread-safe.
    QMutex loaderMutex_;
    QFileSystemWatcher watcher_;
    QTimer debounceTimer_;
    QTimer failureRetryTimer_;
    QThreadPool reloadPool_;
    std::optional<GhosttyConfigSnapshot> snapshot_;
    QString lastError_;
    bool asynchronousReloads_ = false;
    bool loadInProgress_ = false;
    bool reloadPending_ = false;
    RevisionCounter requestEpoch_;
    quint64 loadGeneration_ = 0;
    const int initialFailureRetryMilliseconds_;
    const int maximumFailureRetryMilliseconds_;
    int nextFailureRetryMilliseconds_;
};
