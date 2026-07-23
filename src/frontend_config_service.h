#pragma once

#include "frontend_config.h"

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

    explicit FrontendConfigService(QObject *parent = nullptr);
    FrontendConfigService(QString path, FrontendConfigLoader loader,
                          int debounceMilliseconds, QObject *parent = nullptr);
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

public Q_SLOTS:
    void requestReload();
    void reloadNow();

Q_SIGNALS:
    // Every successful load is published, including an unchanged snapshot.
    void changed(const FrontendConfigSnapshot &snapshot);
    void reloadFailed(const QString &message);

private:
    static QString normalizedAbsolutePath(const QString &path);
    static QString closestExistingDirectory(const QString &path);
    void refreshWatchPaths();
    void watchedPathChanged(const QString &path);
    void beginAsyncReload();
    void applyLoadResult(FrontendConfigLoadResult result);

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
    quint64 loadGeneration_ = 0;
};
