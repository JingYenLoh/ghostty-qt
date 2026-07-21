#include "single_instance_activation.h"

#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusReply>
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

} // namespace

SingleInstanceActivation::SingleInstanceActivation(
    QDBusConnection connection,
    QString serviceName,
    QObject *parent)
    : QObject(parent)
    , connection_(std::move(connection))
    , serviceName_(std::move(serviceName))
    , objectPath_(objectPathForApplicationId(serviceName_))
{
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

QString SingleInstanceActivation::objectPathForApplicationId(
    QStringView applicationId)
{
    QString path = QStringLiteral("/");
    path += applicationId;
    path.replace(u'.', u'/');
    path.replace(u'-', u'_');
    return path;
}

SingleInstanceActivation::StartResult SingleInstanceActivation::start(
    StartOptions options)
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
    objectRegistered_ = connection_.registerObject(objectPath_,
        QString::fromLatin1(InterfaceName), this,
        QDBusConnection::ExportScriptableSlots);
    if (!objectRegistered_) {
        return {
            .role = Role::Failed,
            .diagnostic = QStringLiteral(
                "Could not export the activation endpoint"),
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
    for (std::size_t transition = 0;
         transition <= MaximumOwnerTransitions; ++transition) {
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

        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline
                                       - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) {
            return fail(QStringLiteral(
                "Single-instance activation exceeded its deadline"));
        }

        QDBusMessage message = QDBusMessage::createMethodCall(
            serviceName_, objectPath_,
            QString::fromLatin1(InterfaceName), QStringLiteral("Activate"));
        message << QVariantMap{};
        const QDBusMessage activationReply = connection_.call(
            message, QDBus::Block, boundedTimeout(remaining));
        const bool accepted
            = activationReply.type() == QDBusMessage::ReplyMessage
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

        const QString reason = activationReply.type()
                == QDBusMessage::ReplyMessage
            ? QStringLiteral(
                  "The existing instance returned an invalid activation reply")
            : QStringLiteral("The existing instance rejected activation: %1")
                  .arg(activationError.message());
        return fail(reason);
    }

    return fail(QStringLiteral(
        "The activation service changed owners too many times"));
}

void SingleInstanceActivation::setActivationHandler(
    ActivationHandler handler)
{
    handler_ = std::move(handler);
    std::vector<QDBusMessage> pending =
        std::exchange(pendingActivations_, {});
    for (const QDBusMessage &request : pending) {
        completeActivation(request, handler_ && handler_());
    }
}

void SingleInstanceActivation::release()
{
    for (const QDBusMessage &request : pendingActivations_) {
        completeActivation(request, false);
    }
    pendingActivations_.clear();
    if (ownsService_) {
        if (QDBusConnectionInterface *const interface =
                connection_.interface()) {
            (void) interface->unregisterService(serviceName_);
        }
        ownsService_ = false;
    }
    unregisterObject();
    handler_ = nullptr;
}

void SingleInstanceActivation::Activate(const QVariantMap &platformData)
{
    Q_UNUSED(platformData);
    if (!calledFromDBus()) {
        if (ownsService_ && handler_) (void) handler_();
        return;
    }

    // The owner claims its name before QML startup so concurrent launches
    // cannot both become primary. Keep the method call pending until startup
    // installs the handler: the secondary is acknowledged only after the
    // corresponding window has actually been registered.
    setDelayedReply(true);
    const QDBusMessage request = message();
    if (!ownsService_
        || pendingActivations_.size() >= MaximumPendingActivations) {
        completeActivation(request, false);
    } else if (handler_) {
        completeActivation(request, handler_());
    } else {
        pendingActivations_.push_back(request);
    }
}

void SingleInstanceActivation::Open(
    const QStringList &uris, const QVariantMap &platformData)
{
    Q_UNUSED(uris);
    Q_UNUSED(platformData);
    rejectUnsupported(QStringLiteral("Open"));
}

void SingleInstanceActivation::ActivateAction(const QString &actionName,
    const QVariantList &parameter, const QVariantMap &platformData)
{
    Q_UNUSED(actionName);
    Q_UNUSED(parameter);
    Q_UNUSED(platformData);
    rejectUnsupported(QStringLiteral("ActivateAction"));
}

SingleInstanceActivation::ClaimResult
SingleInstanceActivation::tryClaimService()
{
    QDBusConnectionInterface *const interface = connection_.interface();
    if (interface == nullptr) {
        return {
            .status = ClaimStatus::Failed,
            .diagnostic = QStringLiteral(
                "The session D-Bus has no connection interface"),
        };
    }

    const QDBusReply<QDBusConnectionInterface::RegisterServiceReply> reply =
        interface->registerService(
            serviceName_, QDBusConnectionInterface::DontQueueService,
            QDBusConnectionInterface::DontAllowReplacement);
    if (!reply.isValid()) {
        return {
            .status = ClaimStatus::Failed,
            .diagnostic = QStringLiteral(
                "Could not request the activation service name: %1")
                              .arg(reply.error().message()),
        };
    }
    if (reply.value()
        == QDBusConnectionInterface::ServiceRegistered) {
        ownsService_ = true;
        return {.status = ClaimStatus::Primary, .diagnostic = {}};
    }
    if (reply.value()
        == QDBusConnectionInterface::ServiceNotRegistered) {
        return {.status = ClaimStatus::Occupied, .diagnostic = {}};
    }
    return {
        .status = ClaimStatus::Failed,
        .diagnostic = QStringLiteral(
            "The activation service was unexpectedly queued"),
    };
}

std::expected<QString, QString>
SingleInstanceActivation::currentOwner() const
{
    QDBusConnectionInterface *const interface = connection_.interface();
    if (interface == nullptr) {
        return std::unexpected(QStringLiteral(
            "The session D-Bus has no connection interface"));
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
    const QDBusMessage &request, bool accepted)
{
    QDBusMessage reply = accepted
        ? request.createReply()
        : request.createErrorReply(
              QStringLiteral("org.freedesktop.DBus.Error.Failed"),
              QStringLiteral("The application could not create a window"));
    (void)connection_.send(reply);
}

void SingleInstanceActivation::rejectUnsupported(QStringView methodName)
{
    if (!calledFromDBus()) return;
    setDelayedReply(true);
    (void)connection_.send(message().createErrorReply(
        QStringLiteral("org.freedesktop.DBus.Error.NotSupported"),
        QStringLiteral(
            "%1 is not supported by this no-payload activation endpoint")
            .arg(methodName)));
}

void SingleInstanceActivation::unregisterObject()
{
    if (!objectRegistered_) return;
    connection_.unregisterObject(objectPath_);
    objectRegistered_ = false;
}
