#include "systemd_notify.h"
#include "unique_file_descriptor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <utility>

#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

volatile sig_atomic_t restoredSignalCount = 0;

void countReloadSignal(int signal)
{
    if (signal == SIGUSR2) {
        restoredSignalCount = restoredSignalCount + 1;
    }
}

class UnixDatagramServer final {
public:
    explicit UnixDatagramServer(QByteArray address)
        : address_(std::move(address))
        , descriptor_(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0))
    {
        if (descriptor_.get() < 0) {
            errorCode_ = errno;
            error_ = QString::fromLocal8Bit(std::strerror(errno));
            return;
        }
        if (address_.isEmpty() || address_.contains('\0')
            || (address_.at(0) != '/' && address_.at(0) != '@')) {
            error_ = QStringLiteral("invalid test socket address");
            return;
        }

        struct sockaddr_un socketAddress{};
        socketAddress.sun_family = AF_UNIX;
        const bool abstract = address_.at(0) == '@';
        if (address_.size() > static_cast<qsizetype>(
                sizeof(socketAddress.sun_path) - (abstract ? 0U : 1U))) {
            error_ = QStringLiteral("test socket address is too long");
            return;
        }
        const auto pathLength = static_cast<std::size_t>(address_.size());
        std::memcpy(socketAddress.sun_path, address_.constData(), pathLength);
        if (abstract) socketAddress.sun_path[0] = '\0';
        const socklen_t length =
            static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + pathLength
                                   + (abstract ? 0U : 1U));
        if (::bind(descriptor_.get(),
                   reinterpret_cast<const struct sockaddr *>(&socketAddress),
                   length)
            != 0) {
            errorCode_ = errno;
            error_ = QString::fromLocal8Bit(std::strerror(errno));
        }
    }

    ~UnixDatagramServer()
    {
        if (!address_.isEmpty() && address_.at(0) == '/') {
            (void)::unlink(address_.constData());
        }
    }

    Q_DISABLE_COPY_MOVE(UnixDatagramServer)

    [[nodiscard]] bool isValid() const
    {
        return descriptor_.get() >= 0 && error_.isEmpty();
    }
    [[nodiscard]] const QString &errorString() const { return error_; }
    [[nodiscard]] int errorCode() const { return errorCode_; }
    [[nodiscard]] const QByteArray &address() const { return address_; }

    [[nodiscard]] bool hasPendingDatagram() const
    {
        struct pollfd descriptor{
            .fd = descriptor_.get(),
            .events = POLLIN,
            .revents = 0,
        };
        int ready = -1;
        do {
            ready = ::poll(&descriptor, 1, 0);
        } while (ready < 0 && errno == EINTR);
        return ready > 0 && (descriptor.revents & POLLIN) != 0;
    }

    QByteArray receive(int timeoutMilliseconds)
    {
        struct pollfd descriptor{
            .fd = descriptor_.get(),
            .events = POLLIN,
            .revents = 0,
        };
        int ready = -1;
        do {
            ready = ::poll(&descriptor, 1, timeoutMilliseconds);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0 || (descriptor.revents & POLLIN) == 0) return {};

        std::array<char, 512> buffer{};
        ssize_t size = -1;
        do {
            size = ::recv(descriptor_.get(), buffer.data(), buffer.size(), 0);
        } while (size < 0 && errno == EINTR);
        if (size <= 0) return {};
        return QByteArray(buffer.data(), static_cast<qsizetype>(size));
    }

private:
    QByteArray address_;
    UniqueFileDescriptor descriptor_;
    QString error_;
    int errorCode_ = 0;
};

QByteArray uniqueAbstractAddress()
{
    return QByteArrayLiteral("@ghostty-qt-")
        + QByteArray::number(QCoreApplication::applicationPid()) + '-'
        + QUuid::createUuid().toByteArray(QUuid::Id128);
}

} // namespace

class SystemdNotifyTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void sendsExactDatagrams_data();
    void sendsExactDatagrams();
    void rejectsMalformedAddresses();
    void coordinatesReloadEpochs();
    void bridgesAndRestoresReloadSignal();
};

void SystemdNotifyTest::sendsExactDatagrams_data()
{
    QTest::addColumn<bool>("abstract");
    QTest::newRow("filesystem") << false;
    QTest::newRow("abstract") << true;
}

void SystemdNotifyTest::sendsExactDatagrams()
{
    QFETCH(bool, abstract);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray address = abstract
        ? uniqueAbstractAddress()
        : QFile::encodeName(
              QDir(directory.path()).filePath(QStringLiteral("notify.sock")));
    UnixDatagramServer server(address);
    if (!server.isValid()
        && (server.errorCode() == EPERM || server.errorCode() == EACCES)) {
        QSKIP("The managed environment forbids AF_UNIX datagram sockets");
    }
    QVERIFY2(server.isValid(), qPrintable(server.errorString()));

    const SystemdNotifySocket notifier(server.address());
    const auto ready = notifier.ready();
    QVERIFY2(ready.has_value(), ready ? "" : qPrintable(ready.error()));
    QCOMPARE(*ready, SystemdNotifyResult::Sent);
    QCOMPARE(server.receive(1000), QByteArrayLiteral("READY=1"));

    const auto reloading = notifier.reloading();
    QVERIFY2(reloading.has_value(),
             reloading ? "" : qPrintable(reloading.error()));
    QCOMPARE(*reloading, SystemdNotifyResult::Sent);
    const QByteArray message = server.receive(1000);
    QVERIFY(QRegularExpression(
                QStringLiteral("^RELOADING=1\\nMONOTONIC_USEC=[0-9]+$"))
                .match(QString::fromLatin1(message))
                .hasMatch());
}

void SystemdNotifyTest::rejectsMalformedAddresses()
{
    const auto unconfigured = SystemdNotifySocket().ready();
    QVERIFY(unconfigured.has_value());
    QCOMPARE(*unconfigured, SystemdNotifyResult::NotConfigured);

    const auto relative =
        SystemdNotifySocket(QByteArrayLiteral("relative/socket")).ready();
    QVERIFY(!relative.has_value());
    QVERIFY(relative.error().contains(QStringLiteral("absolute or abstract")));

    const auto embeddedNul =
        SystemdNotifySocket(QByteArray("/tmp/a\0b", 8)).ready();
    QVERIFY(!embeddedNul.has_value());
    QVERIFY(embeddedNul.error().contains(QStringLiteral("embedded NUL")));

    QByteArray overlong("/", 1);
    overlong.append(256, 'x');
    const auto tooLong = SystemdNotifySocket(overlong).ready();
    QVERIFY(!tooLong.has_value());
    QVERIFY(tooLong.error().contains(QStringLiteral("too long")));

    const auto missing =
        SystemdNotifySocket(QByteArrayLiteral("/tmp/ghostty-qt-no-such-socket"))
            .ready();
    QVERIFY(!missing.has_value());
    QVERIFY(missing.error().contains(QStringLiteral("Could not")));
}

void SystemdNotifyTest::coordinatesReloadEpochs()
{
    UnixDatagramServer server(uniqueAbstractAddress());
    if (!server.isValid()
        && (server.errorCode() == EPERM || server.errorCode() == EACCES)) {
        QSKIP("The managed environment forbids AF_UNIX datagram sockets");
    }
    QVERIFY2(server.isValid(), qPrintable(server.errorString()));
    SystemdApplicationLifecycle lifecycle(
        SystemdNotifySocket(server.address()));
    QSignalSpy failures(&lifecycle,
                        &SystemdApplicationLifecycle::notificationFailed);
    QObject ghosttySource;
    QObject frontendSource;

    lifecycle.applicationReady();
    QCOMPARE(server.receive(1000), QByteArrayLiteral("READY=1"));

    lifecycle.reloadScheduled(&ghosttySource, 1);
    const QByteArray reloading = server.receive(1000);
    QVERIFY(reloading.startsWith(
        QByteArrayLiteral("RELOADING=1\nMONOTONIC_USEC=")));
    lifecycle.reloadScheduled(&frontendSource, 3);
    lifecycle.reloadSettled(&ghosttySource, 1);
    QCOMPARE(server.receive(20), QByteArray());

    // A newer coalesced request replaces the target epoch. Settling the old
    // epoch and the other source cannot complete the application reload.
    lifecycle.reloadScheduled(&ghosttySource, 2);
    lifecycle.reloadSettled(&frontendSource, 3);
    lifecycle.reloadSettled(&ghosttySource, 1);
    QCoreApplication::processEvents();
    QCOMPARE(server.receive(20), QByteArray());

    lifecycle.reloadSettled(&ghosttySource, 2);
    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingDatagram(), 1000);
    QCOMPARE(server.receive(0), QByteArrayLiteral("READY=1"));
    QVERIFY(failures.isEmpty());
}

void SystemdNotifyTest::bridgesAndRestoresReloadSignal()
{
    struct sigaction countingAction{};
    countingAction.sa_handler = countReloadSignal;
    QVERIFY(::sigemptyset(&countingAction.sa_mask) == 0);
    struct sigaction originalAction{};
    QVERIFY(::sigaction(SIGUSR2, &countingAction, &originalAction) == 0);
    const auto restore = qScopeGuard([&originalAction] {
        (void)::sigaction(SIGUSR2, &originalAction, nullptr);
    });
    restoredSignalCount = 0;

    {
        SystemdApplicationLifecycle lifecycle;
        const auto installed = lifecycle.installReloadSignal();
        QVERIFY2(installed.has_value(),
                 installed ? "" : qPrintable(installed.error()));
        QSignalSpy reloads(&lifecycle,
                           &SystemdApplicationLifecycle::reloadRequested);
        QVERIFY(::raise(SIGUSR2) == 0);
        QVERIFY(::raise(SIGUSR2) == 0);
        QVERIFY(::raise(SIGUSR2) == 0);
        QTRY_COMPARE_WITH_TIMEOUT(reloads.count(), 1, 1000);
        QCOMPARE(restoredSignalCount, 0);
    }

    QVERIFY(::raise(SIGUSR2) == 0);
    QCOMPARE(restoredSignalCount, 1);
}

QTEST_GUILESS_MAIN(SystemdNotifyTest)

#include "test_systemd_notify.moc"
