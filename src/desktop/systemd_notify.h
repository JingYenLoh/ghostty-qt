#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <expected>
#include <memory>

enum class SystemdNotifyResult {
    NotConfigured,
    Sent,
};

// The sd_notify protocol is one datagram sent to NOTIFY_SOCKET. Keeping this
// narrow transport local avoids making an incidental, distribution-specific
// libsystemd dependency part of ghostty-qt's build contract.
class SystemdNotifySocket final {
public:
    explicit SystemdNotifySocket(QByteArray address = {});

    [[nodiscard]] static SystemdNotifySocket fromEnvironment();
    [[nodiscard]] bool isConfigured() const noexcept
    {
        return !address_.isEmpty();
    }
    [[nodiscard]] const QByteArray &address() const noexcept
    {
        return address_;
    }

    [[nodiscard]] std::expected<SystemdNotifyResult, QString>
    send(QByteArrayView message) const;
    [[nodiscard]] std::expected<SystemdNotifyResult, QString> ready() const;
    [[nodiscard]] std::expected<SystemdNotifyResult, QString> reloading() const;

private:
    QByteArray address_;
};

// Owns process-level systemd readiness, reload transaction accounting, and
// the SIGUSR2-to-Qt self-pipe bridge used by Type=notify-reload user units.
class SystemdApplicationLifecycle final : public QObject {
    Q_OBJECT

public:
    explicit SystemdApplicationLifecycle(
        SystemdNotifySocket socket = SystemdNotifySocket::fromEnvironment(),
        QObject *parent = nullptr);
    ~SystemdApplicationLifecycle() override;

    [[nodiscard]] std::expected<void, QString> installReloadSignal();
    void applicationReady();
    void reloadScheduled(const QObject *source, quint64 requestEpoch);
    void reloadSettled(const QObject *source, quint64 requestEpoch);

Q_SIGNALS:
    void reloadRequested();
    void notificationFailed(const QString &message);

private:
    struct ReloadSignalBridge;

    void announceReload();
    void scheduleReloadCompletion();
    void sendReady();
    void sourceDestroyed(QObject *source);
    void report(std::expected<SystemdNotifyResult, QString> result);

    SystemdNotifySocket socket_;
    std::unique_ptr<ReloadSignalBridge> reloadSignalBridge_;
    QHash<const QObject *, quint64> pendingReloads_;
    QSet<const QObject *> watchedSources_;
    QTimer completionTimer_;
    bool applicationReady_ = false;
    bool reloadAnnounced_ = false;
};
