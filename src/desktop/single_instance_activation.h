#pragma once

#include "desktop/desktop_activation.h"

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
#include <memory>
#include <vector>

struct ApplicationActivationRequest {
    enum class Kind {
        Activate,
        NewTab,
        NewWindow,
        NewWindowCommand,
        ToggleQuickTerminal,
    };

    Kind kind = Kind::Activate;
    QStringList arguments;
    quint64 surfaceId = 0;
    DesktopActivationContext activation;
};

class GtkActionsAdaptor;

// Coordinates application activation through the standard
// org.freedesktop.Application endpoint and its org.gtk.Actions sibling on
// the user's session bus.
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
        std::move_only_function<bool(ApplicationActivationRequest)>;

    static constexpr auto InterfaceName = "org.freedesktop.Application";
    static constexpr auto GtkActionsInterfaceName = "org.gtk.Actions";

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
        ApplicationActivationRequest activation;
        bool alwaysAcknowledgeAfterHandler = false;
    };

    friend class GtkActionsAdaptor;

    [[nodiscard]] ClaimResult tryClaimService();
    [[nodiscard]] std::expected<QString, QString> currentOwner() const;
    void activateGtkAction(const QString &actionName,
                           const QVariantList &parameter,
                           const QVariantMap &platformData);
    void activateAction(const QString &actionName,
                        const QVariantList &parameter,
                        const QVariantMap &platformData,
                        const QDBusMessage *request,
                        bool alwaysAcknowledgeAfterHandler);
    void submit(ApplicationActivationRequest activation,
                const QDBusMessage *request,
                bool alwaysAcknowledgeAfterHandler);
    static void completeActivation(const QDBusConnection &connection,
                                   const QDBusMessage &request, bool accepted,
                                   QStringView failure = {});
    void rejectRequest(const QDBusMessage &request, QStringView errorName,
                       QStringView diagnostic);
    void rejectUnsupported(QStringView methodName);
    void unregisterObject();

    QDBusConnection connection_;
    QString serviceName_;
    QString objectPath_;
    // A local shared snapshot keeps the currently executing callable alive if
    // it reentrantly replaces/releases this endpoint or destroys the owner.
    std::shared_ptr<ActivationHandler> handler_;
    std::vector<PendingActivation> pendingActivations_;
    bool started_ = false;
    bool objectRegistered_ = false;
    bool ownsService_ = false;

    static constexpr std::size_t MaximumPendingActivations = 64;
    static constexpr std::size_t MaximumOwnerTransitions = 8;
};
