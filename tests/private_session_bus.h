#pragma once

#include <QDBusConnection>
#include <QDBusError>
#include <QDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QTemporaryDir>
#include <QUuid>

#include <memory>
#include <optional>
#include <vector>

// Isolated session bus with two independently named client connections. Tests
// never acquire production names on the developer's real desktop bus.
class PrivateSessionBus final {
public:
    PrivateSessionBus() = default;
    PrivateSessionBus(const PrivateSessionBus &) = delete;
    PrivateSessionBus &operator=(const PrivateSessionBus &) = delete;

    bool start()
    {
        return start(QProcessEnvironment::systemEnvironment());
    }

    bool start(QProcessEnvironment environment)
    {
        error_.clear();
        const QString temporaryRoot =
            QDir::current().filePath(QStringLiteral("tmp"));
        if (!QDir().mkpath(temporaryRoot)) {
            error_ = QStringLiteral("could not create %1")
                         .arg(temporaryRoot);
            return false;
        }
        runtimeDirectory_ = std::make_unique<QTemporaryDir>(
            QDir(temporaryRoot).filePath(
                QStringLiteral("dbus-XXXXXX")));
        if (!runtimeDirectory_->isValid()) {
            error_ = QStringLiteral("could not create a temporary D-Bus directory under %1")
                         .arg(temporaryRoot);
            return false;
        }

        const QString requestedAddress
            = QStringLiteral("unix:path=%1")
                  .arg(QDir(runtimeDirectory_->path())
                          .filePath(QStringLiteral("bus")));
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"),
                           runtimeDirectory_->path());
        // An activated service must use the starter bus supplied by the
        // daemon. Remove any developer-session values so cold-start tests
        // cannot accidentally pass through Qt's sessionBus().
        environment.remove(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"));
        environment.remove(QStringLiteral("DBUS_STARTER_ADDRESS"));
        environment.remove(QStringLiteral("DBUS_STARTER_BUS_TYPE"));
        process_.setProcessEnvironment(environment);
        process_.setProgram(QStringLiteral("dbus-daemon"));
        process_.setArguments({QStringLiteral("--session"),
                               QStringLiteral("--address=%1")
                                   .arg(requestedAddress),
                               QStringLiteral("--nofork"),
                               QStringLiteral("--print-address=1")});
        process_.start();
        if (!process_.waitForStarted(3000)
            || (!process_.canReadLine()
                && !process_.waitForReadyRead(3000))) {
            error_ = QStringLiteral(
                         "dbus-daemon did not publish an address: %1; stdout=%2; stderr=%3")
                         .arg(process_.errorString(),
                              QString::fromUtf8(process_.readAllStandardOutput()),
                              QString::fromUtf8(process_.readAllStandardError()));
            return false;
        }

        address_ = QString::fromUtf8(process_.readLine()).trimmed();
        if (address_.isEmpty()) {
            error_ = QStringLiteral("dbus-daemon published an empty address; stderr=%1")
                         .arg(QString::fromUtf8(
                             process_.readAllStandardError()));
            return false;
        }

        const QString suffix = QUuid::createUuid()
                                   .toString(QUuid::WithoutBraces)
                                   .remove(u'-');
        serverName_ = QStringLiteral("ghostty_test_server_%1").arg(suffix);
        clientName_ = QStringLiteral("ghostty_test_client_%1").arg(suffix);
        server_.emplace(
            QDBusConnection::connectToBus(address_, serverName_));
        client_.emplace(
            QDBusConnection::connectToBus(address_, clientName_));
        if (!server_->isConnected() || !client_->isConnected()) {
            error_ = QStringLiteral(
                         "could not connect test clients to %1: server=%2 client=%3")
                         .arg(address_, server_->lastError().message(),
                              client_->lastError().message());
            return false;
        }
        return true;
    }

    ~PrivateSessionBus()
    {
        client_.reset();
        server_.reset();
        if (!clientName_.isEmpty()) {
            QDBusConnection::disconnectFromBus(clientName_);
        }
        for (const QString &name : additionalClientNames_) {
            QDBusConnection::disconnectFromBus(name);
        }
        if (!serverName_.isEmpty()) {
            QDBusConnection::disconnectFromBus(serverName_);
        }
        if (process_.state() != QProcess::NotRunning) {
            process_.terminate();
            if (!process_.waitForFinished(1000)) {
                process_.kill();
                process_.waitForFinished(1000);
            }
        }
    }

    QDBusConnection &server() { return *server_; }
    QDBusConnection &client() { return *client_; }
    QDBusConnection connectClient()
    {
        const QString name = QStringLiteral("ghostty_test_client_%1")
                                 .arg(QUuid::createUuid().toString(
                                     QUuid::WithoutBraces));
        additionalClientNames_.push_back(name);
        return QDBusConnection::connectToBus(address_, name);
    }
    const QString &address() const { return address_; }
    const QString &errorString() const { return error_; }

private:
    QProcess process_;
    QString address_;
    QString error_;
    QString serverName_;
    QString clientName_;
    std::vector<QString> additionalClientNames_;
    std::optional<QDBusConnection> server_;
    std::optional<QDBusConnection> client_;
    std::unique_ptr<QTemporaryDir> runtimeDirectory_;
};
