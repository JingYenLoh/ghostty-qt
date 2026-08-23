#include "desktop/systemd_notify.h"

#include "support/unique_file_descriptor.h"

#include <QSocketNotifier>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace {

volatile sig_atomic_t reloadSignalWriteDescriptor = -1;

void handleReloadSignal(int signal)
{
    if (signal != SIGUSR2) return;

    const int savedError = errno;
    const int descriptor = reloadSignalWriteDescriptor;
    if (descriptor >= 0) {
        constexpr unsigned char wake = 1;
        (void)::write(descriptor, &wake, sizeof(wake));
    }
    errno = savedError;
}

QString systemError(QStringView operation, int error)
{
    return QStringLiteral("%1: %2").arg(
        operation, QString::fromLocal8Bit(std::strerror(error)));
}

std::expected<quint64, QString> monotonicMicroseconds()
{
    struct timespec timestamp{};
    if (::clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        return std::unexpected(systemError(
            QStringLiteral("Could not read CLOCK_MONOTONIC"), errno));
    }

    constexpr quint64 MicrosecondsPerSecond = 1'000'000;
    constexpr quint64 NanosecondsPerMicrosecond = 1'000;
    if (timestamp.tv_sec < 0 || timestamp.tv_nsec < 0) {
        return std::unexpected(
            QStringLiteral("CLOCK_MONOTONIC returned a negative timestamp"));
    }
    const auto seconds = static_cast<quint64>(timestamp.tv_sec);
    const auto microseconds =
        static_cast<quint64>(timestamp.tv_nsec) / NanosecondsPerMicrosecond;
    if (seconds > ((std::numeric_limits<quint64>::max() - microseconds)
                   / MicrosecondsPerSecond)) {
        return std::unexpected(
            QStringLiteral("CLOCK_MONOTONIC timestamp is out of range"));
    }
    return seconds * MicrosecondsPerSecond + microseconds;
}

} // namespace

SystemdNotifySocket::SystemdNotifySocket(QByteArray address)
    : address_(std::move(address))
{}

SystemdNotifySocket SystemdNotifySocket::fromEnvironment()
{
    return SystemdNotifySocket(qgetenv("NOTIFY_SOCKET"));
}

std::expected<SystemdNotifyResult, QString>
SystemdNotifySocket::send(QByteArrayView message) const
{
    if (!isConfigured()) return SystemdNotifyResult::NotConfigured;
    if (message.isEmpty()) {
        return std::unexpected(
            QStringLiteral("Cannot send an empty systemd notification"));
    }
    if (address_.contains('\0')) {
        return std::unexpected(
            QStringLiteral("NOTIFY_SOCKET contains an embedded NUL byte"));
    }
    if (address_.at(0) != '/' && address_.at(0) != '@') {
        return std::unexpected(QStringLiteral(
            "NOTIFY_SOCKET must be an absolute or abstract AF_UNIX address"));
    }

    struct sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const bool abstract = address_.at(0) == '@';
    const qsizetype maximumLength =
        static_cast<qsizetype>(sizeof(address.sun_path) - (abstract ? 0U : 1U));
    if (address_.size() > maximumLength) {
        return std::unexpected(QStringLiteral(
            "NOTIFY_SOCKET address is too long for sockaddr_un"));
    }

    const auto pathLength = static_cast<std::size_t>(address_.size());
    std::memcpy(address.sun_path, address_.constData(), pathLength);
    if (abstract) address.sun_path[0] = '\0';
    const socklen_t addressLength = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + pathLength + (abstract ? 0U : 1U));

    const UniqueFileDescriptor socket(
        ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    if (socket.get() < 0) {
        return std::unexpected(systemError(
            QStringLiteral("Could not create NOTIFY_SOCKET client"), errno));
    }

    ssize_t sent = -1;
    do {
        sent = ::sendto(socket.get(), message.data(),
                        static_cast<std::size_t>(message.size()), MSG_NOSIGNAL,
                        reinterpret_cast<const struct sockaddr *>(&address),
                        addressLength);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) {
        return std::unexpected(systemError(
            QStringLiteral("Could not send systemd notification"), errno));
    }
    if (sent != message.size()) {
        return std::unexpected(
            QStringLiteral("Short systemd notification write: %1 of %2 bytes")
                .arg(sent)
                .arg(message.size()));
    }
    return SystemdNotifyResult::Sent;
}

std::expected<SystemdNotifyResult, QString> SystemdNotifySocket::ready() const
{
    return send(QByteArrayView("READY=1"));
}

std::expected<SystemdNotifyResult, QString>
SystemdNotifySocket::reloading() const
{
    if (!isConfigured()) return SystemdNotifyResult::NotConfigured;
    const auto timestamp = monotonicMicroseconds();
    if (!timestamp) return std::unexpected(timestamp.error());
    const QByteArray message = QByteArrayLiteral("RELOADING=1\nMONOTONIC_USEC=")
        + QByteArray::number(*timestamp);
    return send(message);
}

struct SystemdApplicationLifecycle::ReloadSignalBridge final {
    std::array<UniqueFileDescriptor, 2> pipe{
        UniqueFileDescriptor(-1),
        UniqueFileDescriptor(-1),
    };
    struct sigaction previousAction{};
    std::unique_ptr<QSocketNotifier> notifier;
    bool actionInstalled = false;
};

SystemdApplicationLifecycle::SystemdApplicationLifecycle(
    SystemdNotifySocket socket, QObject *parent)
    : QObject(parent)
    , socket_(std::move(socket))
{
    completionTimer_.setSingleShot(true);
    completionTimer_.setInterval(0);
    connect(&completionTimer_, &QTimer::timeout, this, [this] {
        if (!pendingReloads_.isEmpty() || !reloadAnnounced_) return;
        sendReady();
        reloadAnnounced_ = false;
    });
}

SystemdApplicationLifecycle::~SystemdApplicationLifecycle()
{
    if (reloadSignalBridge_ == nullptr) return;
    if (reloadSignalBridge_->notifier != nullptr) {
        reloadSignalBridge_->notifier->setEnabled(false);
    }
    if (reloadSignalBridge_->actionInstalled) {
        (void)::sigaction(SIGUSR2, &reloadSignalBridge_->previousAction,
                          nullptr);
    }
    reloadSignalWriteDescriptor = -1;

    // A process-directed signal can already be executing on another thread
    // with the old descriptor cached. Keep both ends valid until process exit
    // so that late async-signal-safe writes cannot target a reused descriptor
    // or raise SIGPIPE. The application installs one bridge for its lifetime;
    // the two kernel descriptors are reclaimed by the kernel at exit.
    (void)reloadSignalBridge_->pipe[0].release();
    (void)reloadSignalBridge_->pipe[1].release();
}

std::expected<void, QString> SystemdApplicationLifecycle::installReloadSignal()
{
    if (reloadSignalBridge_ != nullptr) return {};
    if (reloadSignalWriteDescriptor >= 0) {
        return std::unexpected(QStringLiteral(
            "A SIGUSR2 reload bridge is already installed in this process"));
    }

    std::array<int, 2> descriptors{-1, -1};
    if (::pipe2(descriptors.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
        return std::unexpected(systemError(
            QStringLiteral("Could not create SIGUSR2 self-pipe"), errno));
    }

    auto bridge = std::make_unique<ReloadSignalBridge>();
    bridge->pipe[0] = UniqueFileDescriptor(descriptors[0]);
    bridge->pipe[1] = UniqueFileDescriptor(descriptors[1]);

    struct sigaction action{};
    action.sa_handler = handleReloadSignal;
    action.sa_flags = SA_RESTART;
    if (::sigemptyset(&action.sa_mask) != 0) {
        return std::unexpected(systemError(
            QStringLiteral("Could not initialize SIGUSR2 mask"), errno));
    }

    reloadSignalWriteDescriptor = bridge->pipe[1].get();
    if (::sigaction(SIGUSR2, &action, &bridge->previousAction) != 0) {
        const int error = errno;
        reloadSignalWriteDescriptor = -1;
        return std::unexpected(systemError(
            QStringLiteral("Could not install SIGUSR2 handler"), error));
    }
    bridge->actionInstalled = true;
    bridge->notifier = std::make_unique<QSocketNotifier>(
        bridge->pipe[0].get(), QSocketNotifier::Read, this);
    connect(bridge->notifier.get(), &QSocketNotifier::activated, this,
            [this](QSocketDescriptor, QSocketNotifier::Type) {
                if (reloadSignalBridge_ == nullptr) return;
                std::array<unsigned char, 128> pending{};
                for (;;) {
                    const ssize_t count =
                        ::read(reloadSignalBridge_->pipe[0].get(),
                               pending.data(), pending.size());
                    if (count > 0) continue;
                    if (count < 0 && errno == EINTR) continue;
                    break;
                }
                Q_EMIT reloadRequested();
            });
    reloadSignalBridge_ = std::move(bridge);
    return {};
}

void SystemdApplicationLifecycle::applicationReady()
{
    if (applicationReady_) return;
    applicationReady_ = true;
    sendReady();
    if (!pendingReloads_.isEmpty()) announceReload();
}

void SystemdApplicationLifecycle::reloadScheduled(const QObject *source,
                                                  quint64 requestEpoch)
{
    if (source == nullptr || requestEpoch == 0) return;
    completionTimer_.stop();
    const auto current = pendingReloads_.constFind(source);
    if (current == pendingReloads_.cend() || *current < requestEpoch) {
        pendingReloads_.insert(source, requestEpoch);
    }
    if (!watchedSources_.contains(source)) {
        watchedSources_.insert(source);
        connect(source, &QObject::destroyed, this,
                &SystemdApplicationLifecycle::sourceDestroyed);
    }
    if (applicationReady_) announceReload();
}

void SystemdApplicationLifecycle::reloadSettled(const QObject *source,
                                                quint64 requestEpoch)
{
    const auto pending = pendingReloads_.constFind(source);
    if (pending == pendingReloads_.cend() || requestEpoch < *pending) return;
    pendingReloads_.remove(source);
    scheduleReloadCompletion();
}

void SystemdApplicationLifecycle::announceReload()
{
    if (reloadAnnounced_) return;
    reloadAnnounced_ = true;
    report(socket_.reloading());
}

void SystemdApplicationLifecycle::scheduleReloadCompletion()
{
    if (!pendingReloads_.isEmpty() || !reloadAnnounced_) return;
    completionTimer_.start();
}

void SystemdApplicationLifecycle::sendReady()
{
    report(socket_.ready());
}

void SystemdApplicationLifecycle::sourceDestroyed(QObject *source)
{
    pendingReloads_.remove(source);
    watchedSources_.remove(source);
    scheduleReloadCompletion();
}

void SystemdApplicationLifecycle::report(
    std::expected<SystemdNotifyResult, QString> result)
{
    if (!result) Q_EMIT notificationFailed(result.error());
}
