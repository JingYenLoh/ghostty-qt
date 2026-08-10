#pragma once

#include "frontend_config.h"
#include "revision_counter.h"

#include <QFileSystemWatcher>
#include <QMutex>
#include <QObject>
#include <QProcessEnvironment>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>

#include <optional>

class FrontendConfigService : public QObject {
    Q_OBJECT

public:
    static constexpr int DefaultDebounceMilliseconds = 75;
    static constexpr int InitialFailureRetryMilliseconds = 5'000;
    static constexpr int MaximumFailureRetryMilliseconds = 5 * 60 * 1'000;

    explicit FrontendConfigService(QObject *parent = nullptr);
    FrontendConfigService(QString path, FrontendConfigLoader loader,
                          int debounceMilliseconds, QObject *parent = nullptr);
    // Custom retry bounds keep backoff behavior deterministic in contract
    // tests without weakening production retry intervals.
    FrontendConfigService(QString path, FrontendConfigLoader loader,
                          int debounceMilliseconds,
                          int initialFailureRetryMilliseconds,
                          int maximumFailureRetryMilliseconds,
                          QObject *parent = nullptr);
    ~FrontendConfigService() override;

    static QString
    standardConfigPath(const QProcessEnvironment &environment =
                           QProcessEnvironment::systemEnvironment());

    const QString &configPath() const { return configPath_; }
    bool hasSnapshot() const { return snapshot_.has_value(); }
    const FrontendConfigSnapshot &snapshot() const;
    const QString &lastError() const { return lastError_; }

    QStringList watchedFiles() const;
    QStringList watchedDirectories() const;
    // Zero means no automatic failure retry is currently armed.
    int scheduledFailureRetryMilliseconds() const;

public Q_SLOTS:
    void requestReload();
    void reloadNow();

Q_SIGNALS:
    // Request epochs identify the newest reload a coalesced asynchronous load
    // satisfies. They let process-level readiness wait through overlapping
    // requests and through both successful and failed loads.
    void reloadScheduled(quint64 requestEpoch);
    void reloadSettled(quint64 requestEpoch);
    // Every successful load is published, including an unchanged snapshot.
    void changed(const FrontendConfigSnapshot &snapshot);
    void reloadFailed(const QString &message);

private:
    static QString normalizedAbsolutePath(const QString &path);
    static QString closestExistingDirectory(const QString &path);
    void scheduleReload();
    void resetFailureRetryBackoff();
    void scheduleFailureRetry();
    void refreshWatchPaths();
    void watchedPathChanged(const QString &path);
    void beginAsyncReload();
    void applyLoadResult(FrontendConfigLoadResult result, quint64 requestEpoch);

    QString configPath_;
    FrontendConfigLoader loader_;
    QMutex loaderMutex_;
    QFileSystemWatcher watcher_;
    QTimer debounceTimer_;
    QTimer failureRetryTimer_;
    QThreadPool reloadPool_;
    std::optional<FrontendConfigSnapshot> snapshot_;
    QString lastError_;
    bool loadInProgress_ = false;
    bool reloadPending_ = false;
    RevisionCounter requestEpoch_;
    quint64 loadGeneration_ = 0;
    const int initialFailureRetryMilliseconds_;
    const int maximumFailureRetryMilliseconds_;
    int nextFailureRetryMilliseconds_;
};
