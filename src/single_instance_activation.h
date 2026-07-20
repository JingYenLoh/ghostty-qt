#pragma once

#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QObject>
#include <QString>

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <vector>

// Coordinates no-payload application activation through one versioned,
// project-owned name on the user's session bus. It deliberately carries no
// command line, environment, cwd, or shell text.
class SingleInstanceActivation final : public QObject,
                                       protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface",
                "io.github.JingYenLoh.ghostty_qt.Application1")

public:
    enum class Role {
        Primary,
        ActivatedExisting,
        ExistingInstance,
        Independent,
        Failed,
    };
    Q_ENUM(Role)

    struct StartResult {
        Role role = Role::Independent;
        QString diagnostic;
    };

    enum class ExistingInstanceAction {
        Activate,
        DoNotActivate,
    };

    struct StartOptions {
        std::chrono::milliseconds timeout = std::chrono::seconds(10);
        ExistingInstanceAction existingInstanceAction =
            ExistingInstanceAction::Activate;
    };

    using ActivationHandler = std::move_only_function<bool()>;

    static constexpr quint32 ProtocolVersion = 1;
    static constexpr auto ObjectPath =
        "/io/github/JingYenLoh/ghostty_qt/Application";
    static constexpr auto InterfaceName =
        "io.github.JingYenLoh.ghostty_qt.Application1";

    explicit SingleInstanceActivation(
        QDBusConnection connection = QDBusConnection::sessionBus(),
        QString serviceName = defaultServiceName(),
        QObject *parent = nullptr);
    ~SingleInstanceActivation() override;

    [[nodiscard]] static QString defaultServiceName();
    [[nodiscard]] StartResult start(StartOptions options);
    void setActivationHandler(ActivationHandler handler);
    void release();

    [[nodiscard]] bool isPrimary() const { return ownsService_; }

public Q_SLOTS:
    Q_SCRIPTABLE bool Activate(quint32 protocolVersion);

private:
    enum class ClaimStatus {
        Primary,
        Occupied,
        Failed,
    };
    struct ClaimResult {
        ClaimStatus status = ClaimStatus::Failed;
        QString diagnostic;
    };

    [[nodiscard]] ClaimResult tryClaimService();
    [[nodiscard]] std::expected<QString, QString> currentOwner() const;
    void unregisterObject();

    QDBusConnection connection_;
    QString serviceName_;
    ActivationHandler handler_;
    std::vector<QDBusMessage> pendingActivations_;
    bool started_ = false;
    bool objectRegistered_ = false;
    bool ownsService_ = false;

    static constexpr std::size_t MaximumPendingActivations = 64;
    static constexpr std::size_t MaximumOwnerTransitions = 8;
};
