#include "private_session_bus.h"
#include "single_instance_activation.h"

#include <QDBusContext>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDir>
#include <QHash>
#include <QStandardPaths>
#include <QTest>
#include <QUuid>
#include <QVariantMap>
#include <QXmlStreamReader>

#include <chrono>
#include <future>
#include <memory>
#include <utility>

using namespace std::chrono_literals;

namespace {

QString uniqueServiceName()
{
    QString suffix =
        QUuid::createUuid().toString(QUuid::WithoutBraces).remove(u'-');
    return QStringLiteral("io.github.JingYenLoh.ghostty_qt.Test.t%1")
        .arg(suffix);
}

QString objectPath(const QString &service)
{
    return SingleInstanceActivation::objectPathForApplicationId(service);
}

QDBusMessage activationCall(const QString &service,
                            QVariantMap platformData = {})
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        service, objectPath(service),
        QString::fromLatin1(SingleInstanceActivation::InterfaceName),
        QStringLiteral("Activate"));
    message << platformData;
    return message;
}

QDBusMessage actionCall(const QString &service, QStringView interfaceName,
                        QStringView methodName, const QString &actionName,
                        QVariantList parameter = {},
                        QVariantMap platformData = {})
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        service, objectPath(service), interfaceName.toString(),
        methodName.toString());
    message << actionName << parameter << platformData;
    return message;
}

QDBusMessage standardActionCall(const QString &service,
                                const QString &actionName,
                                QVariantList parameter = {},
                                QVariantMap platformData = {})
{
    return actionCall(
        service, QString::fromLatin1(SingleInstanceActivation::InterfaceName),
        QStringLiteral("ActivateAction"), actionName, std::move(parameter),
        std::move(platformData));
}

QDBusMessage gtkActionCall(const QString &service, const QString &actionName,
                           QVariantList parameter = {},
                           QVariantMap platformData = {})
{
    return actionCall(
        service,
        QString::fromLatin1(SingleInstanceActivation::GtkActionsInterfaceName),
        QStringLiteral("Activate"), actionName, std::move(parameter),
        std::move(platformData));
}

class NeverReplyEndpoint final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Application")

public Q_SLOTS:
    Q_SCRIPTABLE void Activate(const QVariantMap &) { setDelayedReply(true); }
};

class AcceptEndpoint final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Application")

public:
    int calls = 0;
    QVariantMap platformData;

public Q_SLOTS:
    Q_SCRIPTABLE void Activate(const QVariantMap &data)
    {
        ++calls;
        platformData = data;
    }
};

class HandoffEndpoint final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Application")

public:
    HandoffEndpoint(QDBusConnection retiring, QDBusConnection successor,
                    QString service)
        : retiring_(std::move(retiring))
        , successor_(std::move(successor))
        , service_(std::move(service))
    {}

    int calls = 0;
    bool handoffSucceeded = false;

public Q_SLOTS:
    Q_SCRIPTABLE void Activate(const QVariantMap &)
    {
        setDelayedReply(true);
        ++calls;
        handoffSucceeded = retiring_.unregisterService(service_)
            && successor_.registerService(service_);
        (void)retiring_.send(message().createErrorReply(
            QStringLiteral("org.freedesktop.DBus.Error.Failed"),
            QStringLiteral("owner retired")));
    }

private:
    QDBusConnection retiring_;
    QDBusConnection successor_;
    QString service_;
};

} // namespace

class SingleInstanceActivationTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void firstRegistrantBecomesPrimary();
    void secondaryActivatesPrimaryExactlyOnce();
    void secondaryCanExitWithoutActivatingPrimary();
    void activationWaitsUntilHandlerIsInstalled();
    void mixedActivationsQueueInFifoOrder();
    void queuedActivationReportsHandlerFailure();
    void gtkActionAcknowledgesHandlerFailure();
    void invalidActionsAreRejectedWithoutQueuing();
    void pendingQueueIsBoundedAcrossInterfaces();
    void releaseRejectsQueuedActions();
    void handlerMayReleaseEndpointDuringDispatch();
    void handlerMayDestroyEndpointWhileDrainingQueue();
    void directCallsDispatchTypedRequests();
    void activationFollowsAnOwnerHandoff();
    void exportsBothInterfacesAndRejectsUnsupportedMethods();
    void disconnectedBusFallsBackAndNoReplyFailsClosed();
};

void SingleInstanceActivationTest::firstRegistrantBecomesPrimary()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    SingleInstanceActivation activation(bus.server(), uniqueServiceName());
    const auto result = activation.start({
        .timeout = 1s,
        .existingInstanceAction =
            SingleInstanceActivation::ExistingInstanceAction::DoNotActivate,
    });
    QCOMPARE(result.role, SingleInstanceActivation::Role::Primary);
    QVERIFY(result.diagnostic.isEmpty());
    QVERIFY(activation.isPrimary());
    const auto repeated = activation.start({.timeout = 1s});
    QCOMPARE(repeated.role, SingleInstanceActivation::Role::Failed);
    QVERIFY(!repeated.diagnostic.isEmpty());
    QVERIFY(activation.isPrimary());
}

void SingleInstanceActivationTest::secondaryActivatesPrimaryExactlyOnce()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    SingleInstanceActivation primary(bus.server(), service);
    QCOMPARE(primary.start({.timeout = 1s}).role,
             SingleInstanceActivation::Role::Primary);
    int activations = 0;
    DesktopActivationContext observed;
    auto observedKind = ApplicationActivationRequest::Kind::NewWindow;
    primary.setActivationHandler([&](ApplicationActivationRequest request) {
        ++activations;
        observedKind = request.kind;
        observed = std::move(request.activation);
        return true;
    });

    auto future = std::async(std::launch::async, [&bus, service] {
        SingleInstanceActivation secondary(bus.client(), service);
        return secondary.start({
            .timeout = 2s,
            .activation =
                {
                    .xdgActivationToken = QStringLiteral("forwarded-token"),
                    .desktopStartupId = QStringLiteral("forwarded-startup"),
                },
        });
    });
    QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(0ms) == std::future_status::ready,
                             3000);
    const auto result = future.get();
    QCOMPARE(result.role, SingleInstanceActivation::Role::ActivatedExisting);
    QCOMPARE(activations, 1);
    QCOMPARE(observedKind, ApplicationActivationRequest::Kind::Activate);
    QCOMPARE(observed.xdgActivationToken, QStringLiteral("forwarded-token"));
    QCOMPARE(observed.desktopStartupId, QStringLiteral("forwarded-startup"));
}

void SingleInstanceActivationTest::secondaryCanExitWithoutActivatingPrimary()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    SingleInstanceActivation primary(bus.server(), service);
    QCOMPARE(primary.start({.timeout = 1s}).role,
             SingleInstanceActivation::Role::Primary);
    int activations = 0;
    primary.setActivationHandler([&activations](ApplicationActivationRequest) {
        ++activations;
        return true;
    });

    SingleInstanceActivation secondary(bus.client(), service);
    const auto result = secondary.start({
        .timeout = 1s,
        .existingInstanceAction =
            SingleInstanceActivation::ExistingInstanceAction::DoNotActivate,
    });
    QCOMPARE(result.role, SingleInstanceActivation::Role::ExistingInstance);
    QVERIFY(result.diagnostic.isEmpty());
    QCOMPARE(activations, 0);
}

void SingleInstanceActivationTest::activationWaitsUntilHandlerIsInstalled()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    SingleInstanceActivation primary(bus.server(), service);
    QCOMPARE(primary.start({.timeout = 1s}).role,
             SingleInstanceActivation::Role::Primary);

    QDBusPendingCallWatcher firstCall(bus.client().asyncCall(
        activationCall(service,
                       {
                           {QStringLiteral("activation-token"),
                            QStringLiteral("queued-token")},
                       }),
        2000));
    QDBusPendingCallWatcher secondCall(bus.client().asyncCall(
        activationCall(service,
                       {
                           {QStringLiteral("activation-token"),
                            QStringLiteral("queued-token-two")},
                       }),
        2000));
    QTest::qWait(100);
    QVERIFY(!firstCall.isFinished());
    QVERIFY(!secondCall.isFinished());

    int activations = 0;
    QStringList observedTokens;
    std::vector<ApplicationActivationRequest::Kind> observedKinds;
    primary.setActivationHandler([&](ApplicationActivationRequest request) {
        ++activations;
        observedKinds.push_back(request.kind);
        observedTokens.append(std::move(request.activation.xdgActivationToken));
        return true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(firstCall.isFinished(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(secondCall.isFinished(), 3000);
    QCOMPARE(firstCall.reply().type(), QDBusMessage::ReplyMessage);
    QCOMPARE(secondCall.reply().type(), QDBusMessage::ReplyMessage);
    QCOMPARE(activations, 2);
    QCOMPARE(observedKinds,
             std::vector({
                 ApplicationActivationRequest::Kind::Activate,
                 ApplicationActivationRequest::Kind::Activate,
             }));
    QCOMPARE(observedTokens,
             QStringList({
                 QStringLiteral("queued-token"),
                 QStringLiteral("queued-token-two"),
             }));
}

void SingleInstanceActivationTest::queuedActivationReportsHandlerFailure()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    SingleInstanceActivation primary(bus.server(), service);
    QCOMPARE(primary.start({.timeout = 1s}).role,
             SingleInstanceActivation::Role::Primary);

    auto future = std::async(std::launch::async, [&bus, service] {
        SingleInstanceActivation secondary(bus.client(), service);
        return secondary.start({.timeout = 2s});
    });
    QTest::qWait(100);
    QCOMPARE(future.wait_for(0ms), std::future_status::timeout);

    int activations = 0;
    auto observedKind = ApplicationActivationRequest::Kind::NewWindow;
    primary.setActivationHandler(
        [&activations, &observedKind](ApplicationActivationRequest request) {
            observedKind = request.kind;
            ++activations;
            return false;
        });
    QTRY_COMPARE_WITH_TIMEOUT(future.wait_for(0ms), std::future_status::ready,
                              3000);
    QCOMPARE(future.get().role, SingleInstanceActivation::Role::Failed);
    QCOMPARE(activations, 1);
    QCOMPARE(observedKind, ApplicationActivationRequest::Kind::Activate);
}

void SingleInstanceActivationTest::mixedActivationsQueueInFifoOrder()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    SingleInstanceActivation primary(bus.server(), service);
    QCOMPARE(primary.start({.timeout = 1s}).role,
             SingleInstanceActivation::Role::Primary);

    QDBusPendingCallWatcher activate(bus.client().asyncCall(
        activationCall(service,
                       {
                           {QStringLiteral("activation-token"),
                            QStringLiteral("activate-token")},
                       }),
        2000));
    QDBusPendingCallWatcher newWindow(bus.client().asyncCall(
        standardActionCall(service, QStringLiteral("new-window"), {},
                           {
                               {QStringLiteral("activation-token"),
                                QStringLiteral("window-token")},
                           }),
        2000));
    QDBusPendingCallWatcher command(bus.client().asyncCall(
        gtkActionCall(service, QStringLiteral("new-window-command"),
                      {QVariant::fromValue(QStringList{
                          QStringLiteral("-e"),
                          QStringLiteral("printf"),
                          QStringLiteral("queued"),
                      })},
                      {
                          {QStringLiteral("desktop-startup-id"),
                           QStringLiteral("command-startup")},
                      }),
        2000));
    QDBusPendingCallWatcher toggle(bus.client().asyncCall(
        gtkActionCall(service, QStringLiteral("toggle-quick-terminal")), 2000));
    QTest::qWait(100);
    QVERIFY(!activate.isFinished());
    QVERIFY(!newWindow.isFinished());
    QVERIFY(!command.isFinished());
    QVERIFY(!toggle.isFinished());

    std::vector<ApplicationActivationRequest> observed;
    primary.setActivationHandler(
        [&observed](ApplicationActivationRequest request) {
            const auto kind = request.kind;
            observed.push_back(std::move(request));
            return kind != ApplicationActivationRequest::Kind::NewWindowCommand;
        });
    QTRY_VERIFY_WITH_TIMEOUT(activate.isFinished(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(newWindow.isFinished(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(command.isFinished(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(toggle.isFinished(), 3000);
    QCOMPARE(activate.reply().type(), QDBusMessage::ReplyMessage);
    QCOMPARE(newWindow.reply().type(), QDBusMessage::ReplyMessage);
    QCOMPARE(command.reply().type(), QDBusMessage::ReplyMessage);
    QCOMPARE(toggle.reply().type(), QDBusMessage::ReplyMessage);

    QCOMPARE(observed.size(), 4);
    QCOMPARE(observed[0].kind, ApplicationActivationRequest::Kind::Activate);
    QCOMPARE(observed[0].activation.xdgActivationToken,
             QStringLiteral("activate-token"));
    QCOMPARE(observed[1].kind, ApplicationActivationRequest::Kind::NewWindow);
    QCOMPARE(observed[1].activation.xdgActivationToken,
             QStringLiteral("window-token"));
    QCOMPARE(observed[2].kind,
             ApplicationActivationRequest::Kind::NewWindowCommand);
    QCOMPARE(observed[2].arguments,
             QStringList({
                 QStringLiteral("-e"),
                 QStringLiteral("printf"),
                 QStringLiteral("queued"),
             }));
    QCOMPARE(observed[2].activation.desktopStartupId,
             QStringLiteral("command-startup"));
    QCOMPARE(observed[3].kind,
             ApplicationActivationRequest::Kind::ToggleQuickTerminal);
}

void SingleInstanceActivationTest::gtkActionAcknowledgesHandlerFailure()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    SingleInstanceActivation primary(bus.server(), service);
    QCOMPARE(primary.start({.timeout = 1s}).role,
             SingleInstanceActivation::Role::Primary);
    std::vector<ApplicationActivationRequest::Kind> observed;
    primary.setActivationHandler(
        [&observed](ApplicationActivationRequest request) {
            observed.push_back(request.kind);
            return false;
        });

    QDBusPendingCallWatcher standard(bus.client().asyncCall(
        standardActionCall(service, QStringLiteral("new-window")), 1000));
    QDBusPendingCallWatcher gtk(bus.client().asyncCall(
        gtkActionCall(service, QStringLiteral("new-window")), 1000));
    QTRY_VERIFY_WITH_TIMEOUT(standard.isFinished(), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(gtk.isFinished(), 2000);
    QCOMPARE(QDBusError(standard.reply()).type(), QDBusError::Failed);
    QCOMPARE(gtk.reply().type(), QDBusMessage::ReplyMessage);
    QCOMPARE(observed,
             std::vector({
                 ApplicationActivationRequest::Kind::NewWindow,
                 ApplicationActivationRequest::Kind::NewWindow,
             }));
}

void SingleInstanceActivationTest::invalidActionsAreRejectedWithoutQueuing()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    SingleInstanceActivation primary(bus.server(), service);
    QCOMPARE(primary.start({.timeout = 1s}).role,
             SingleInstanceActivation::Role::Primary);

    QDBusPendingCallWatcher unknown(bus.client().asyncCall(
        standardActionCall(service, QStringLiteral("not-an-action")), 1000));
    QDBusPendingCallWatcher newWindowWithParameter(bus.client().asyncCall(
        standardActionCall(service, QStringLiteral("new-window"),
                           {QVariant::fromValue(QStringList{})}),
        1000));
    QDBusPendingCallWatcher commandWithoutParameter(bus.client().asyncCall(
        gtkActionCall(service, QStringLiteral("new-window-command")), 1000));
    QDBusPendingCallWatcher commandWithWrongType(bus.client().asyncCall(
        gtkActionCall(service, QStringLiteral("new-window-command"),
                      {QStringLiteral("not-an-array")}),
        1000));
    QDBusPendingCallWatcher toggleWithParameter(bus.client().asyncCall(
        gtkActionCall(service, QStringLiteral("toggle-quick-terminal"),
                      {QVariant::fromValue(QStringList{})}),
        1000));

    QTRY_VERIFY_WITH_TIMEOUT(unknown.isFinished(), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(newWindowWithParameter.isFinished(), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(commandWithoutParameter.isFinished(), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(commandWithWrongType.isFinished(), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(toggleWithParameter.isFinished(), 2000);
    QCOMPARE(QDBusError(unknown.reply()).type(), QDBusError::NotSupported);
    QCOMPARE(QDBusError(newWindowWithParameter.reply()).type(),
             QDBusError::InvalidArgs);
    QCOMPARE(QDBusError(commandWithoutParameter.reply()).type(),
             QDBusError::InvalidArgs);
    QCOMPARE(QDBusError(commandWithWrongType.reply()).type(),
             QDBusError::InvalidArgs);
    QCOMPARE(QDBusError(toggleWithParameter.reply()).type(),
             QDBusError::InvalidArgs);

    int activations = 0;
    primary.setActivationHandler([&activations](ApplicationActivationRequest) {
        ++activations;
        return true;
    });
    QCOMPARE(activations, 0);
}

void SingleInstanceActivationTest::pendingQueueIsBoundedAcrossInterfaces()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    SingleInstanceActivation primary(bus.server(), service);
    QCOMPARE(primary.start({.timeout = 1s}).role,
             SingleInstanceActivation::Role::Primary);

    constexpr std::size_t pendingLimit = 64;
    std::vector<std::unique_ptr<QDBusPendingCallWatcher>> calls;
    calls.reserve(pendingLimit + 1);
    for (std::size_t index = 0; index <= pendingLimit; ++index) {
        const QDBusMessage request = index % 2 == 0
            ? standardActionCall(service, QStringLiteral("new-window"))
            : gtkActionCall(service, QStringLiteral("toggle-quick-terminal"));
        calls.push_back(std::make_unique<QDBusPendingCallWatcher>(
            bus.client().asyncCall(request, 3000)));
    }

    QTRY_VERIFY_WITH_TIMEOUT(calls.back()->isFinished(), 2000);
    QCOMPARE(QDBusError(calls.back()->reply()).type(), QDBusError::Failed);
    for (std::size_t index = 0; index < pendingLimit; ++index) {
        QVERIFY(!calls[index]->isFinished());
    }

    primary.release();
    for (std::size_t index = 0; index < pendingLimit; ++index) {
        QTRY_VERIFY_WITH_TIMEOUT(calls[index]->isFinished(), 2000);
        QCOMPARE(QDBusError(calls[index]->reply()).type(), QDBusError::Failed);
    }
}

void SingleInstanceActivationTest::releaseRejectsQueuedActions()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    SingleInstanceActivation primary(bus.server(), service);
    QCOMPARE(primary.start({.timeout = 1s}).role,
             SingleInstanceActivation::Role::Primary);

    QDBusPendingCallWatcher queued(bus.client().asyncCall(
        gtkActionCall(service, QStringLiteral("toggle-quick-terminal")), 1000));
    QTest::qWait(100);
    QVERIFY(!queued.isFinished());
    primary.release();
    QTRY_VERIFY_WITH_TIMEOUT(queued.isFinished(), 2000);
    QCOMPARE(QDBusError(queued.reply()).type(), QDBusError::Failed);
    QVERIFY(!primary.isPrimary());
}

void SingleInstanceActivationTest::handlerMayReleaseEndpointDuringDispatch()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    SingleInstanceActivation primary(bus.server(), service);
    QCOMPARE(primary.start({.timeout = 1s}).role,
             SingleInstanceActivation::Role::Primary);

    int calls = 0;
    primary.setActivationHandler(
        [&primary, &calls](ApplicationActivationRequest) {
            ++calls;
            primary.release();
            return true;
        });

    QDBusPendingCallWatcher request(bus.client().asyncCall(
        standardActionCall(service, QStringLiteral("new-window")), 2000));
    QTRY_VERIFY_WITH_TIMEOUT(request.isFinished(), 3000);
    QCOMPARE(request.reply().type(), QDBusMessage::ReplyMessage);
    QCOMPARE(calls, 1);
    QVERIFY(!primary.isPrimary());
}

void SingleInstanceActivationTest::handlerMayDestroyEndpointWhileDrainingQueue()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    auto primary =
        std::make_unique<SingleInstanceActivation>(bus.server(), service);
    QCOMPARE(primary->start({.timeout = 1s}).role,
             SingleInstanceActivation::Role::Primary);

    QDBusPendingCallWatcher first(bus.client().asyncCall(
        standardActionCall(service, QStringLiteral("new-window")), 2000));
    QDBusPendingCallWatcher second(bus.client().asyncCall(
        gtkActionCall(service, QStringLiteral("toggle-quick-terminal")), 2000));
    QTest::qWait(100);
    QVERIFY(!first.isFinished());
    QVERIFY(!second.isFinished());

    int calls = 0;
    SingleInstanceActivation *const endpoint = primary.get();
    endpoint->setActivationHandler(
        [&primary, &calls](ApplicationActivationRequest) {
            ++calls;
            primary.reset();
            return true;
        });

    QVERIFY(primary == nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(first.isFinished(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(second.isFinished(), 3000);
    QCOMPARE(first.reply().type(), QDBusMessage::ReplyMessage);
    QCOMPARE(QDBusError(second.reply()).type(), QDBusError::Failed);
    QCOMPARE(calls, 1);
}

void SingleInstanceActivationTest::directCallsDispatchTypedRequests()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    SingleInstanceActivation primary(bus.server(), uniqueServiceName());
    QCOMPARE(primary.start({.timeout = 1s}).role,
             SingleInstanceActivation::Role::Primary);

    std::vector<ApplicationActivationRequest> observed;
    primary.setActivationHandler(
        [&observed](ApplicationActivationRequest request) {
            observed.push_back(std::move(request));
            return true;
        });
    primary.Activate({
        {QStringLiteral("activation-token"), QStringLiteral("direct-token")},
    });
    primary.ActivateAction(QStringLiteral("new-window"), {}, {});
    primary.ActivateAction(QStringLiteral("new-window-command"),
                           {QVariant::fromValue(QStringList{
                               QStringLiteral("-e"),
                               QStringLiteral("true"),
                           })},
                           {});
    primary.ActivateAction(QStringLiteral("toggle-quick-terminal"), {}, {});
    primary.ActivateAction(QStringLiteral("new-window"),
                           {QStringLiteral("invalid")}, {});
    primary.ActivateAction(QStringLiteral("unknown"), {}, {});

    QCOMPARE(observed.size(), 4);
    QCOMPARE(observed[0].kind, ApplicationActivationRequest::Kind::Activate);
    QCOMPARE(observed[0].activation.xdgActivationToken,
             QStringLiteral("direct-token"));
    QCOMPARE(observed[1].kind, ApplicationActivationRequest::Kind::NewWindow);
    QCOMPARE(observed[2].kind,
             ApplicationActivationRequest::Kind::NewWindowCommand);
    QCOMPARE(observed[2].arguments,
             QStringList({
                 QStringLiteral("-e"),
                 QStringLiteral("true"),
             }));
    QCOMPARE(observed[3].kind,
             ApplicationActivationRequest::Kind::ToggleQuickTerminal);
}

void SingleInstanceActivationTest::activationFollowsAnOwnerHandoff()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    AcceptEndpoint successor;
    QVERIFY(bus.client().registerObject(
        objectPath(service),
        QString::fromLatin1(SingleInstanceActivation::InterfaceName),
        &successor, QDBusConnection::ExportScriptableSlots));
    HandoffEndpoint retiring(bus.server(), bus.client(), service);
    QVERIFY(bus.server().registerObject(
        objectPath(service),
        QString::fromLatin1(SingleInstanceActivation::InterfaceName), &retiring,
        QDBusConnection::ExportScriptableSlots));
    QVERIFY(bus.server().registerService(service));

    const QDBusConnection thirdClient = bus.connectClient();
    QVERIFY2(thirdClient.isConnected(),
             qPrintable(thirdClient.lastError().message()));
    auto future = std::async(std::launch::async, [thirdClient, service] {
        SingleInstanceActivation activation(thirdClient, service);
        return activation.start({
            .timeout = 2s,
            .activation =
                {
                    .xdgActivationToken = QStringLiteral("handoff-token"),
                },
        });
    });
    QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(0ms) == std::future_status::ready,
                             3000);
    QCOMPARE(future.get().role,
             SingleInstanceActivation::Role::ActivatedExisting);
    QCOMPARE(retiring.calls, 1);
    QVERIFY(retiring.handoffSucceeded);
    QCOMPARE(successor.calls, 1);
    QCOMPARE(successor.platformData.value(QStringLiteral("activation-token"))
                 .toString(),
             QStringLiteral("handoff-token"));
}

void SingleInstanceActivationTest::
    exportsBothInterfacesAndRejectsUnsupportedMethods()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    SingleInstanceActivation primary(bus.server(), service);
    QCOMPARE(primary.start({.timeout = 1s}).role,
             SingleInstanceActivation::Role::Primary);
    int activations = 0;
    DesktopActivationContext observed;
    auto observedKind = ApplicationActivationRequest::Kind::NewWindow;
    primary.setActivationHandler([&](ApplicationActivationRequest request) {
        ++activations;
        observedKind = request.kind;
        observed = std::move(request.activation);
        return true;
    });

    QCOMPARE(objectPath(QStringLiteral("org.example.app-debug")),
             QStringLiteral("/org/example/app_debug"));

    auto introspection = std::async(std::launch::async, [&bus, service] {
        return bus.client().call(
            QDBusMessage::createMethodCall(
                service, objectPath(service),
                QStringLiteral("org.freedesktop.DBus.Introspectable"),
                QStringLiteral("Introspect")),
            QDBus::Block, 1000);
    });
    QTRY_COMPARE_WITH_TIMEOUT(introspection.wait_for(0ms),
                              std::future_status::ready, 2000);
    const QDBusMessage introspectionReply = introspection.get();
    QCOMPARE(introspectionReply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(introspectionReply.arguments().size(), 1);

    QHash<QString, QStringList> inputSignatures;
    QXmlStreamReader xml(introspectionReply.arguments().front().toString());
    QString currentInterface;
    QString currentMethod;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QStringLiteral("interface")) {
            currentInterface =
                xml.attributes().value(QStringLiteral("name")).toString();
        } else if (xml.isStartElement()
                   && xml.name() == QStringLiteral("method")) {
            currentMethod =
                xml.attributes().value(QStringLiteral("name")).toString();
        } else if (xml.isStartElement() && xml.name() == QStringLiteral("arg")
                   && !currentMethod.isEmpty()
                   && xml.attributes().value(QStringLiteral("direction"))
                       != QStringLiteral("out")) {
            inputSignatures[currentInterface + u'.' + currentMethod].append(
                xml.attributes().value(QStringLiteral("type")).toString());
        } else if (xml.isEndElement()
                   && xml.name() == QStringLiteral("method")) {
            currentMethod.clear();
        } else if (xml.isEndElement()
                   && xml.name() == QStringLiteral("interface")) {
            currentInterface.clear();
        }
    }
    QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
    QCOMPARE(inputSignatures.value(
                 QStringLiteral("org.freedesktop.Application.ActivateAction")),
             QStringList({QStringLiteral("s"), QStringLiteral("av"),
                          QStringLiteral("a{sv}")}));
    QCOMPARE(inputSignatures.value(QStringLiteral("org.gtk.Actions.Activate")),
             QStringList({QStringLiteral("s"), QStringLiteral("av"),
                          QStringLiteral("a{sv}")}));

    auto future = std::async(std::launch::async, [&bus, service] {
        QVariantMap platformData;
        platformData.insert(QStringLiteral("activation-token"),
                            QStringLiteral("token"));
        platformData.insert(QStringLiteral("desktop-startup-id"),
                            QStringLiteral("startup"));
        platformData.insert(QStringLiteral("unknown"), 42);
        return bus.client().call(activationCall(service, platformData),
                                 QDBus::Block, 1000);
    });
    QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(0ms) == std::future_status::ready,
                             2000);
    const QDBusMessage reply = future.get();
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QVERIFY(reply.arguments().isEmpty());
    QCOMPARE(activations, 1);
    QCOMPARE(observedKind, ApplicationActivationRequest::Kind::Activate);
    QCOMPARE(observed.xdgActivationToken, QStringLiteral("token"));
    QCOMPARE(observed.desktopStartupId, QStringLiteral("startup"));

    auto malformed = std::async(std::launch::async, [&bus, service] {
        return bus.client().call(
            activationCall(service,
                           {
                               {QStringLiteral("activation-token"), 7},
                               {QStringLiteral("desktop-startup-id"),
                                QByteArrayLiteral("not-a-string")},
                           }),
            QDBus::Block, 1000);
    });
    QTRY_COMPARE_WITH_TIMEOUT(malformed.wait_for(0ms),
                              std::future_status::ready, 2000);
    QCOMPARE(malformed.get().type(), QDBusMessage::ReplyMessage);
    QCOMPARE(activations, 2);
    QVERIFY(observed.isEmpty());

    const auto unsupported = [&bus, &service](QString method,
                                              QList<QVariant> arguments) {
        return std::async(
            std::launch::async,
            [&bus, &service, method = std::move(method),
             arguments = std::move(arguments)] {
                QDBusMessage message = QDBusMessage::createMethodCall(
                    service, objectPath(service),
                    QString::fromLatin1(
                        SingleInstanceActivation::InterfaceName),
                    method);
                message.setArguments(arguments);
                return bus.client().call(message, QDBus::Block, 1000);
            });
    };

    auto open = unsupported(
        QStringLiteral("Open"),
        {QVariant::fromValue(QStringList{QStringLiteral("file:tmp/a")}),
         QVariant::fromValue(QVariantMap{})});
    QTRY_COMPARE_WITH_TIMEOUT(open.wait_for(0ms), std::future_status::ready,
                              2000);
    QCOMPARE(QDBusError(open.get()).type(), QDBusError::NotSupported);

    auto action = unsupported(QStringLiteral("ActivateAction"),
                              {QStringLiteral("unknown"),
                               QVariant::fromValue(QVariantList{}),
                               QVariant::fromValue(QVariantMap{})});
    QTRY_COMPARE_WITH_TIMEOUT(action.wait_for(0ms), std::future_status::ready,
                              2000);
    QCOMPARE(QDBusError(action.get()).type(), QDBusError::NotSupported);

    primary.release();
    QVERIFY(!primary.isPrimary());
    SingleInstanceActivation replacement(bus.client(), service);
    QCOMPARE(replacement.start({.timeout = 1s}).role,
             SingleInstanceActivation::Role::Primary);
}

void SingleInstanceActivationTest::
    disconnectedBusFallsBackAndNoReplyFailsClosed()
{
    const QString connectionName =
        QStringLiteral("ghostty_missing_bus_%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString missingSocket = QDir::current().filePath(
        QStringLiteral("tmp/missing-session-bus-%1.sock")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    {
        SingleInstanceActivation disconnected(
            QDBusConnection::connectToBus(
                QStringLiteral("unix:path=%1").arg(missingSocket),
                connectionName),
            uniqueServiceName());
        const auto result = disconnected.start({.timeout = 50ms});
        QCOMPARE(result.role, SingleInstanceActivation::Role::Independent);
        QVERIFY(!result.diagnostic.isEmpty());
    }
    QDBusConnection::disconnectFromBus(connectionName);

    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    NeverReplyEndpoint endpoint;
    QVERIFY(bus.server().registerObject(
        objectPath(service),
        QString::fromLatin1(SingleInstanceActivation::InterfaceName), &endpoint,
        QDBusConnection::ExportScriptableSlots));
    QVERIFY(bus.server().registerService(service));

    auto future = std::async(std::launch::async, [&bus, service] {
        SingleInstanceActivation secondary(bus.client(), service);
        return secondary.start({.timeout = 50ms});
    });
    QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(0ms) == std::future_status::ready,
                             2000);
    const auto result = future.get();
    QCOMPARE(result.role, SingleInstanceActivation::Role::Failed);
    QVERIFY(result.diagnostic.contains(QStringLiteral("possibly duplicate")));
}

QTEST_GUILESS_MAIN(SingleInstanceActivationTest)

#include "test_single_instance_activation.moc"
