#include "single_instance_activation.h"

#include "ghostty_application_ipc.h"

#include <QDBusAbstractAdaptor>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QMetaType>
#include <QPointer>
#include <QVariantMap>

#include <algorithm>
#include <limits>
#include <utility>

#ifndef GHOSTTY_QT_APPLICATION_ID
#define GHOSTTY_QT_APPLICATION_ID "io.github.JingYenLoh.ghostty_qt"
#endif

namespace {

int boundedTimeout(std::chrono::milliseconds timeout)
{
    return static_cast<int>(std::clamp<std::int64_t>(
        timeout.count(), 1, std::numeric_limits<int>::max()));
}

struct InvalidAction {
    QString errorName;
    QString diagnostic;
};

std::optional<GhosttyNewTabIpcParameter>
newTabParameter(const QVariant &parameter)
{
    if (parameter.metaType()
        == QMetaType::fromType<GhosttyNewTabIpcParameter>()) {
        return parameter.value<GhosttyNewTabIpcParameter>();
    }
    if (parameter.metaType() != QMetaType::fromType<QDBusArgument>()) {
        return std::nullopt;
    }
    const QDBusArgument argument = parameter.value<QDBusArgument>();
    if (argument.currentSignature() != QLatin1StringView("(tas)")) {
        return std::nullopt;
    }
    GhosttyNewTabIpcParameter result;
    argument >> result;
    return result;
}

std::expected<ApplicationActivationRequest, InvalidAction>
parseAction(const QString &actionName, const QVariantList &parameter,
            const QVariantMap &platformData)
{
    using Kind = ApplicationActivationRequest::Kind;

    ApplicationActivationRequest request{
        .kind = Kind::Activate,
        .arguments = {},
        .activation = DesktopActivationContext::fromPlatformData(platformData),
    };
    if (actionName == QStringLiteral("new-window")) {
        request.kind = Kind::NewWindow;
    } else if (actionName == QStringLiteral("new-tab")) {
        request.kind = Kind::NewTab;
    } else if (actionName == QStringLiteral("new-window-command")) {
        request.kind = Kind::NewWindowCommand;
    } else if (actionName == QStringLiteral("toggle-quick-terminal")) {
        request.kind = Kind::ToggleQuickTerminal;
    } else {
        return std::unexpected(InvalidAction{
            .errorName =
                QStringLiteral("org.freedesktop.DBus.Error.NotSupported"),
            .diagnostic = QStringLiteral("Unknown application action: %1")
                              .arg(actionName),
        });
    }

    if (request.kind != Kind::NewWindowCommand
        && request.kind != Kind::NewTab) {
        if (parameter.isEmpty()) return request;
        return std::unexpected(InvalidAction{
            .errorName =
                QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
            .diagnostic =
                QStringLiteral("%1 does not accept parameters").arg(actionName),
        });
    }

    if (request.kind == Kind::NewTab) {
        const std::optional<GhosttyNewTabIpcParameter> value =
            parameter.size() == 1 ? newTabParameter(parameter.front())
                                  : std::nullopt;
        if (!value.has_value()) {
            return std::unexpected(InvalidAction{
                .errorName =
                    QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                .diagnostic = QStringLiteral(
                    "new-tab requires exactly one (tas) parameter"),
            });
        }
        request.surfaceId = value->surfaceId;
        request.arguments = value->arguments;
        return request;
    }

    if (parameter.size() != 1
        || parameter.front().metaType() != QMetaType::fromType<QStringList>()) {
        return std::unexpected(InvalidAction{
            .errorName =
                QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
            .diagnostic = QStringLiteral(
                "new-window-command requires exactly one string-array parameter"),
        });
    }
    request.arguments = parameter.front().toStringList();
    return request;
}

} // namespace

class GtkActionsAdaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.gtk.Actions")

public:
    explicit GtkActionsAdaptor(SingleInstanceActivation *activation)
        : QDBusAbstractAdaptor(activation)
        , activation_(activation)
    {
        setAutoRelaySignals(false);
    }

public Q_SLOTS:
    Q_SCRIPTABLE void Activate(const QString &actionName,
                               const QVariantList &parameter,
                               const QVariantMap &platformData)
    {
        activation_->activateGtkAction(actionName, parameter, platformData);
    }

private:
    SingleInstanceActivation *activation_;
};

SingleInstanceActivation::SingleInstanceActivation(QDBusConnection connection,
                                                   QString serviceName,
                                                   QObject *parent)
    : QObject(parent)
    , connection_(std::move(connection))
    , serviceName_(std::move(serviceName))
    , objectPath_(objectPathForApplicationId(serviceName_))
{
    qDBusRegisterMetaType<GhosttyNewTabIpcParameter>();
    (void)new GtkActionsAdaptor(this);
}

SingleInstanceActivation::~SingleInstanceActivation()
{
    release();
}

QString SingleInstanceActivation::defaultServiceName()
{
    return QStringLiteral(GHOSTTY_QT_APPLICATION_ID);
}

QDBusConnection SingleInstanceActivation::defaultConnection()
{
    if (!qEnvironmentVariableIsEmpty("DBUS_STARTER_ADDRESS")) {
        return QDBusConnection::connectToBus(
            QDBusConnection::ActivationBus,
            QStringLiteral("ghostty_qt_activation_bus"));
    }
    return QDBusConnection::sessionBus();
}

QString
SingleInstanceActivation::objectPathForApplicationId(QStringView applicationId)
{
    QString path = QStringLiteral("/");
    path += applicationId;
    path.replace(u'.', u'/');
    path.replace(u'-', u'_');
    return path;
}

SingleInstanceActivation::StartResult
SingleInstanceActivation::start(StartOptions options)
{
    if (started_) {
        return {
            .role = Role::Failed,
            .diagnostic = QStringLiteral(
                "Single-instance coordination was already started"),
        };
    }
    started_ = true;

    if (!connection_.isConnected()) {
        return {
            .role = Role::Independent,
            .diagnostic = QStringLiteral(
                "The session D-Bus is unavailable; starting independently"),
        };
    }

    // Export under this connection's unique name before atomically claiming
    // the well-known name. A secondary may then call as soon as RequestName
    // reports that an owner exists without observing a missing object.
    objectRegistered_ =
        connection_.registerObject(objectPath_, this,
                                   QDBusConnection::ExportScriptableSlots
                                       | QDBusConnection::ExportAdaptors);
    if (!objectRegistered_) {
        return {
            .role = Role::Failed,
            .diagnostic =
                QStringLiteral("Could not export the activation endpoint"),
        };
    }
    const auto fail = [this](QString diagnostic) {
        unregisterObject();
        return StartResult{
            .role = Role::Failed,
            .diagnostic = std::move(diagnostic),
        };
    };

    const auto deadline = std::chrono::steady_clock::now()
        + std::max(options.timeout, std::chrono::milliseconds(1));
    for (std::size_t transition = 0; transition <= MaximumOwnerTransitions;
         ++transition) {
        ClaimResult claim = tryClaimService();
        if (claim.status == ClaimStatus::Primary) {
            return {.role = Role::Primary, .diagnostic = {}};
        }
        if (claim.status == ClaimStatus::Failed) {
            return fail(std::move(claim.diagnostic));
        }
        if (options.existingInstanceAction
            == ExistingInstanceAction::DoNotActivate) {
            unregisterObject();
            return {.role = Role::ExistingInstance, .diagnostic = {}};
        }

        const std::expected<QString, QString> owner = currentOwner();
        if (!owner.has_value()) {
            return fail(owner.error());
        }
        if (owner->isEmpty()) continue;

        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) {
            return fail(QStringLiteral(
                "Single-instance activation exceeded its deadline"));
        }

        QDBusMessage message = QDBusMessage::createMethodCall(
            serviceName_, objectPath_, QString::fromLatin1(InterfaceName),
            QStringLiteral("Activate"));
        message << options.activation.toPlatformData();
        const QDBusMessage activationReply =
            connection_.call(message, QDBus::Block, boundedTimeout(remaining));
        const bool accepted =
            activationReply.type() == QDBusMessage::ReplyMessage
            && activationReply.arguments().isEmpty();
        if (accepted) {
            unregisterObject();
            return {.role = Role::ActivatedExisting, .diagnostic = {}};
        }

        const QDBusError activationError(activationReply);
        if (activationError.type() == QDBusError::NoReply
            || activationError.type() == QDBusError::Timeout
            || activationError.type() == QDBusError::TimedOut) {
            return fail(QStringLiteral(
                "The existing instance did not acknowledge activation before the deadline; refusing to create a possibly duplicate window"));
        }

        // The contacted process can disappear while multiple launches race.
        // Claim again first; if a different process won, loop once more and
        // deliver this activation to that new owner. A rejection from the
        // same live owner is definitive and never starts a duplicate locally.
        claim = tryClaimService();
        if (claim.status == ClaimStatus::Primary) {
            return {.role = Role::Primary, .diagnostic = {}};
        }
        if (claim.status == ClaimStatus::Failed) {
            return fail(std::move(claim.diagnostic));
        }

        const std::expected<QString, QString> successor = currentOwner();
        if (!successor.has_value()) {
            return fail(successor.error());
        }
        if (successor->isEmpty() || *successor != *owner) continue;

        const QString reason =
            activationReply.type() == QDBusMessage::ReplyMessage
            ? QStringLiteral(
                  "The existing instance returned an invalid activation reply")
            : QStringLiteral("The existing instance rejected activation: %1")
                  .arg(activationError.message());
        return fail(reason);
    }

    return fail(
        QStringLiteral("The activation service changed owners too many times"));
}

void SingleInstanceActivation::setActivationHandler(ActivationHandler handler)
{
    handler_ = handler ? std::make_shared<ActivationHandler>(std::move(handler))
                       : nullptr;
    std::vector<PendingActivation> pending =
        std::exchange(pendingActivations_, {});
    const QDBusConnection replyConnection = connection_;
    const QPointer<SingleInstanceActivation> guard(this);
    for (PendingActivation &activation : pending) {
        const std::shared_ptr<ActivationHandler> currentHandler =
            guard != nullptr && guard->ownsService_ ? guard->handler_ : nullptr;
        const bool invoked =
            currentHandler != nullptr && static_cast<bool>(*currentHandler);
        const bool accepted =
            invoked && (*currentHandler)(std::move(activation.activation));
        completeActivation(
            replyConnection, activation.request,
            accepted || (invoked && activation.alwaysAcknowledgeAfterHandler));
    }
}

void SingleInstanceActivation::release()
{
    for (const PendingActivation &activation : pendingActivations_) {
        completeActivation(connection_, activation.request, false);
    }
    pendingActivations_.clear();
    if (ownsService_) {
        if (QDBusConnectionInterface *const interface =
                connection_.interface()) {
            (void)interface->unregisterService(serviceName_);
        }
        ownsService_ = false;
    }
    unregisterObject();
    handler_ = nullptr;
}

void SingleInstanceActivation::Activate(const QVariantMap &platformData)
{
    ApplicationActivationRequest activation{
        .kind = ApplicationActivationRequest::Kind::Activate,
        .arguments = {},
        .activation = DesktopActivationContext::fromPlatformData(platformData),
    };
    if (!calledFromDBus()) {
        submit(std::move(activation), nullptr, false);
        return;
    }

    // The owner claims its name before QML startup so concurrent launches
    // cannot both become primary. Keep the method call pending until startup
    // installs the handler: the secondary is acknowledged only after the
    // corresponding window has actually been registered.
    setDelayedReply(true);
    const QDBusMessage request = message();
    submit(std::move(activation), &request, false);
}

void SingleInstanceActivation::Open(const QStringList &uris,
                                    const QVariantMap &platformData)
{
    Q_UNUSED(uris);
    Q_UNUSED(platformData);
    rejectUnsupported(QStringLiteral("Open"));
}

void SingleInstanceActivation::ActivateAction(const QString &actionName,
                                              const QVariantList &parameter,
                                              const QVariantMap &platformData)
{
    if (!calledFromDBus()) {
        activateAction(actionName, parameter, platformData, nullptr, false);
        return;
    }

    setDelayedReply(true);
    const QDBusMessage request = message();
    activateAction(actionName, parameter, platformData, &request, false);
}

void SingleInstanceActivation::activateGtkAction(
    const QString &actionName, const QVariantList &parameter,
    const QVariantMap &platformData)
{
    // Qt dispatches adaptor slots within the registered parent's D-Bus call
    // context. Keep delayed-reply handling here: QDBusContext on the adaptor
    // itself does not observe that context.
    if (!calledFromDBus()) {
        activateAction(actionName, parameter, platformData, nullptr, true);
        return;
    }

    setDelayedReply(true);
    const QDBusMessage request = message();
    activateAction(actionName, parameter, platformData, &request, true);
}

void SingleInstanceActivation::activateAction(
    const QString &actionName, const QVariantList &parameter,
    const QVariantMap &platformData, const QDBusMessage *request,
    bool alwaysAcknowledgeAfterHandler)
{
    auto parsed = parseAction(actionName, parameter, platformData);
    if (!parsed.has_value()) {
        if (request != nullptr) {
            rejectRequest(*request, parsed.error().errorName,
                          parsed.error().diagnostic);
        }
        return;
    }
    submit(std::move(*parsed), request, alwaysAcknowledgeAfterHandler);
}

void SingleInstanceActivation::submit(ApplicationActivationRequest activation,
                                      const QDBusMessage *request,
                                      bool alwaysAcknowledgeAfterHandler)
{
    if (request == nullptr) {
        const std::shared_ptr<ActivationHandler> currentHandler = handler_;
        if (ownsService_ && currentHandler != nullptr
            && static_cast<bool>(*currentHandler)) {
            (void)(*currentHandler)(std::move(activation));
        }
        return;
    }

    if (!ownsService_
        || pendingActivations_.size() >= MaximumPendingActivations) {
        completeActivation(connection_, *request, false);
    } else if (handler_) {
        const QDBusConnection replyConnection = connection_;
        const QDBusMessage replyRequest = *request;
        const std::shared_ptr<ActivationHandler> currentHandler = handler_;
        const bool invoked =
            currentHandler != nullptr && static_cast<bool>(*currentHandler);
        const bool accepted =
            invoked && (*currentHandler)(std::move(activation));
        completeActivation(replyConnection, replyRequest,
                           accepted
                               || (invoked && alwaysAcknowledgeAfterHandler));
    } else {
        pendingActivations_.push_back(
            {*request, std::move(activation), alwaysAcknowledgeAfterHandler});
    }
}

SingleInstanceActivation::ClaimResult
SingleInstanceActivation::tryClaimService()
{
    QDBusConnectionInterface *const interface = connection_.interface();
    if (interface == nullptr) {
        return {
            .status = ClaimStatus::Failed,
            .diagnostic =
                QStringLiteral("The session D-Bus has no connection interface"),
        };
    }

    const QDBusReply<QDBusConnectionInterface::RegisterServiceReply> reply =
        interface->registerService(
            serviceName_, QDBusConnectionInterface::DontQueueService,
            QDBusConnectionInterface::DontAllowReplacement);
    if (!reply.isValid()) {
        return {
            .status = ClaimStatus::Failed,
            .diagnostic =
                QStringLiteral(
                    "Could not request the activation service name: %1")
                    .arg(reply.error().message()),
        };
    }
    if (reply.value() == QDBusConnectionInterface::ServiceRegistered) {
        ownsService_ = true;
        return {.status = ClaimStatus::Primary, .diagnostic = {}};
    }
    if (reply.value() == QDBusConnectionInterface::ServiceNotRegistered) {
        return {.status = ClaimStatus::Occupied, .diagnostic = {}};
    }
    return {
        .status = ClaimStatus::Failed,
        .diagnostic =
            QStringLiteral("The activation service was unexpectedly queued"),
    };
}

std::expected<QString, QString> SingleInstanceActivation::currentOwner() const
{
    QDBusConnectionInterface *const interface = connection_.interface();
    if (interface == nullptr) {
        return std::unexpected(
            QStringLiteral("The session D-Bus has no connection interface"));
    }

    const QDBusReply<QString> reply = interface->serviceOwner(serviceName_);
    if (reply.isValid()) return reply.value();
    if (reply.error().type() == QDBusError::ServiceUnknown
        || reply.error().name()
            == QStringLiteral("org.freedesktop.DBus.Error.NameHasNoOwner")) {
        return QString{};
    }
    return std::unexpected(
        QStringLiteral("Could not identify the activation service owner: %1")
            .arg(reply.error().message()));
}

void SingleInstanceActivation::completeActivation(
    const QDBusConnection &connection, const QDBusMessage &request,
    bool accepted, QStringView failure)
{
    const QString diagnostic = failure.isEmpty()
        ? QStringLiteral("The application could not handle activation")
        : failure.toString();
    QDBusMessage reply = accepted
        ? request.createReply()
        : request.createErrorReply(
              QStringLiteral("org.freedesktop.DBus.Error.Failed"), diagnostic);
    (void)connection.send(reply);
}

void SingleInstanceActivation::rejectRequest(const QDBusMessage &request,
                                             QStringView errorName,
                                             QStringView diagnostic)
{
    (void)connection_.send(
        request.createErrorReply(errorName.toString(), diagnostic.toString()));
}

void SingleInstanceActivation::rejectUnsupported(QStringView methodName)
{
    if (!calledFromDBus()) return;
    setDelayedReply(true);
    rejectRequest(
        message(), QStringLiteral("org.freedesktop.DBus.Error.NotSupported"),
        QStringLiteral(
            "%1 is not supported by this no-payload activation endpoint")
            .arg(methodName));
}

void SingleInstanceActivation::unregisterObject()
{
    if (!objectRegistered_) return;
    connection_.unregisterObject(objectPath_);
    objectRegistered_ = false;
}

#include "single_instance_activation.moc"
