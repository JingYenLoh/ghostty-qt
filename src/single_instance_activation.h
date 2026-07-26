#pragma once

#include "desktop_activation.h"

#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QVariantList>
#include <QVariantMap>

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <vector>

// Coordinates no-payload application activation through the standard
// org.freedesktop.Application endpoint on the user's session bus. It
// carries only standard presentation context: never command-line payload,
// general environment state, cwd, or shell text.
class SingleInstanceActivation final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Application")

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
        DesktopActivationContext activation;
    };

    using ActivationHandler =
        std::move_only_function<bool(DesktopActivationContext)>;

    static constexpr auto InterfaceName = "org.freedesktop.Application";

    [[nodiscard]] static QDBusConnection defaultConnection();
    explicit SingleInstanceActivation(
        QDBusConnection connection = defaultConnection(),
        QString serviceName = defaultServiceName(), QObject *parent = nullptr);
    ~SingleInstanceActivation() override;

    [[nodiscard]] static QString defaultServiceName();
    [[nodiscard]] static QString
    objectPathForApplicationId(QStringView applicationId);
    [[nodiscard]] StartResult start(StartOptions options);
    void setActivationHandler(ActivationHandler handler);
    void release();

    [[nodiscard]] bool isPrimary() const { return ownsService_; }

public Q_SLOTS:
    Q_SCRIPTABLE void Activate(const QVariantMap &platformData);
    Q_SCRIPTABLE void Open(const QStringList &uris,
                           const QVariantMap &platformData);
    Q_SCRIPTABLE void ActivateAction(const QString &actionName,
                                     const QVariantList &parameter,
                                     const QVariantMap &platformData);

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
    struct PendingActivation {
        QDBusMessage request;
        DesktopActivationContext activation;
    };

    [[nodiscard]] ClaimResult tryClaimService();
    [[nodiscard]] std::expected<QString, QString> currentOwner() const;
    void completeActivation(const QDBusMessage &request, bool accepted);
    void rejectUnsupported(QStringView methodName);
    void unregisterObject();

    QDBusConnection connection_;
    QString serviceName_;
    QString objectPath_;
    ActivationHandler handler_;
    std::vector<PendingActivation> pendingActivations_;
    bool started_ = false;
    bool objectRegistered_ = false;
    bool ownsService_ = false;

    static constexpr std::size_t MaximumPendingActivations = 64;
    static constexpr std::size_t MaximumOwnerTransitions = 8;
};
