#include "ghostty_global_shortcut_portal.h"
#include "private_session_bus.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QVariantMap>

#include <algorithm>

namespace {

GhosttyKeybindTrigger physical(QString name, quint8 modifiers = 0)
{
    return GhosttyKeybindTrigger{
        .kind = GhosttyKeybindKeyKind::Physical,
        .physicalName = std::move(name),
        .unicodeCodepoint = 0,
        .modifiers = modifiers,
    };
}

GhosttyKeybindTrigger unicode(quint32 codepoint, quint8 modifiers = 0)
{
    return GhosttyKeybindTrigger{
        .kind = GhosttyKeybindKeyKind::Unicode,
        .physicalName = {},
        .unicodeCodepoint = codepoint,
        .modifiers = modifiers,
    };
}

GhosttyKeybindTrigger catchAll(quint8 modifiers = 0)
{
    return GhosttyKeybindTrigger{
        .kind = GhosttyKeybindKeyKind::CatchAll,
        .physicalName = {},
        .unicodeCodepoint = 0,
        .modifiers = modifiers,
    };
}

GhosttyKeybindDefinition definition(
    QVector<GhosttyKeybindTrigger> sequence,
    QStringList actions,
    bool global = true)
{
    return GhosttyKeybindDefinition{
        .sequence = std::move(sequence),
        .actions = std::move(actions),
        .flags = GhosttyKeybindFlags{
            .consumed = true,
            .all = false,
            .global = global,
            .performable = false,
        },
    };
}

GhosttyKeybindDefinition definition(GhosttyKeybindTrigger trigger,
                                    QString action,
                                    bool global = true)
{
    return definition(QVector<GhosttyKeybindTrigger>{std::move(trigger)},
                      QStringList{std::move(action)},
                      global);
}

int diagnosticCount(const GhosttyGlobalShortcutRegistry &registry,
                    GhosttyGlobalShortcutDiagnosticCode code)
{
    return static_cast<int>(std::count_if(
        registry.diagnostics.cbegin(), registry.diagnostics.cend(),
        [code](const GhosttyGlobalShortcutDiagnostic &entry) {
            return entry.code == code;
        }));
}

QVariant unwrapVariant(QVariant value)
{
    while (value.metaType() == QMetaType::fromType<QDBusVariant>()) {
        value = value.value<QDBusVariant>().variant();
    }
    return value;
}

QString optionString(const QVariantMap &options, const QString &name)
{
    return unwrapVariant(options.value(name)).toString();
}

QVariantMap decodeVariantMap(const QVariant &value)
{
    if (value.metaType() == QMetaType::fromType<QDBusArgument>()) {
        return qdbus_cast<QVariantMap>(value);
    }
    return value.toMap();
}

QString portalPath(QString sender, const QString &kind, const QString &token)
{
    if (sender.startsWith(u':')) {
        sender.remove(0, 1);
    }
    sender.replace(u'.', u'_');
    sender.replace(u'-', u'_');
    return QStringLiteral("/org/freedesktop/portal/desktop/%1/%2/%3")
        .arg(kind, sender, token);
}

class FakeGlobalShortcutsPortal final : public QDBusVirtualObject {
public:
    struct CapturedShortcut {
        QString id;
        QVariantMap options;
    };

    FakeGlobalShortcutsPortal(QDBusConnection connection,
                              QString clientUniqueName)
        : m_connection(std::move(connection))
        , m_clientUniqueName(std::move(clientUniqueName))
    {
    }

    QString introspect(const QString &path) const override
    {
        Q_UNUSED(path);
        return {};
    }

    bool handleMessage(const QDBusMessage &message,
                       const QDBusConnection &connection) override
    {
        Q_UNUSED(connection);
        if (message.interface()
                == QLatin1StringView("org.freedesktop.portal.GlobalShortcuts")
            && message.member() == QLatin1StringView("CreateSession")) {
            handleCreateSession(message);
            return true;
        }
        if (message.interface()
                == QLatin1StringView("org.freedesktop.portal.GlobalShortcuts")
            && message.member() == QLatin1StringView("BindShortcuts")) {
            handleBindShortcuts(message);
            return true;
        }
        if (message.member() == QLatin1StringView("Close")
            && message.interface()
                == QLatin1StringView("org.freedesktop.portal.Request")) {
            ++requestCloseCount;
            m_connection.send(message.createReply());
            return true;
        }
        if (message.member() == QLatin1StringView("Close")
            && message.interface()
                == QLatin1StringView("org.freedesktop.portal.Session")) {
            ++sessionCloseCount;
            m_connection.send(message.createReply());
            return true;
        }
        return false;
    }

    void activate(const QString &session, const QString &id)
    {
        QDBusMessage signal = QDBusMessage::createSignal(
            QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.portal.GlobalShortcuts"),
            QStringLiteral("Activated"));
        signal << QVariant::fromValue(QDBusObjectPath(session))
               << id
               << qulonglong(123)
               << QVariantMap{};
        m_connection.send(signal);
    }

    void closeSession(const QString &session)
    {
        QDBusMessage signal = QDBusMessage::createSignal(
            session,
            QStringLiteral("org.freedesktop.portal.Session"),
            QStringLiteral("Closed"));
        signal << QVariantMap{};
        m_connection.send(signal);
    }

    void sendDeferredCreateResponse()
    {
        if (!m_deferredCreate.has_value()) {
            return;
        }
        sendResponse(m_deferredCreate->requestPath,
                     {{QStringLiteral("session_handle"),
                       m_deferredCreate->sessionPath}});
        m_deferredCreate.reset();
    }

    int createCount = 0;
    int bindCount = 0;
    int requestCloseCount = 0;
    int sessionCloseCount = 0;
    bool deferNextCreate = false;
    bool failNextCreate = false;
    QString currentSession;
    QVector<CapturedShortcut> lastShortcuts;

private:
    struct DeferredCreate {
        QString requestPath;
        QString sessionPath;
    };

    void handleCreateSession(const QDBusMessage &message)
    {
        ++createCount;
        const QVariantMap options = decodeVariantMap(
            message.arguments().value(0));
        const QString requestToken = optionString(
            options, QStringLiteral("handle_token"));
        const QString sessionToken = optionString(
            options, QStringLiteral("session_handle_token"));
        const QString requestPath = portalPath(
            m_clientUniqueName, QStringLiteral("request"), requestToken);
        currentSession = portalPath(
            m_clientUniqueName, QStringLiteral("session"), sessionToken);

        if (deferNextCreate) {
            deferNextCreate = false;
            m_deferredCreate = DeferredCreate{requestPath, currentSession};
        } else if (failNextCreate) {
            failNextCreate = false;
            sendResponse(requestPath, {}, 2);
        } else {
            sendResponse(requestPath,
                         {{QStringLiteral("session_handle"),
                           currentSession}});
        }

        m_connection.send(message.createReply(
            QVariant::fromValue(QDBusObjectPath(requestPath))));
    }

    void handleBindShortcuts(const QDBusMessage &message)
    {
        ++bindCount;
        const QVariantList arguments = message.arguments();
        lastShortcuts.clear();
        if (arguments.size() >= 2
            && arguments.at(1).metaType()
                == QMetaType::fromType<QDBusArgument>()) {
            const QDBusArgument serialized =
                arguments.at(1).value<QDBusArgument>();
            serialized.beginArray();
            while (!serialized.atEnd()) {
                CapturedShortcut shortcut;
                serialized.beginStructure();
                serialized >> shortcut.id >> shortcut.options;
                serialized.endStructure();
                lastShortcuts.append(std::move(shortcut));
            }
            serialized.endArray();
        }

        const QVariantMap options = decodeVariantMap(arguments.value(3));
        const QString requestPath = portalPath(
            m_clientUniqueName,
            QStringLiteral("request"),
            optionString(options, QStringLiteral("handle_token")));
        sendResponse(requestPath, {});
        m_connection.send(message.createReply(
            QVariant::fromValue(QDBusObjectPath(requestPath))));
    }

    void sendResponse(const QString &path, const QVariantMap &results,
                      uint response = 0)
    {
        QDBusMessage signal = QDBusMessage::createSignal(
            path,
            QStringLiteral("org.freedesktop.portal.Request"),
            QStringLiteral("Response"));
        signal << response << results;
        m_connection.send(signal);
    }

    QDBusConnection m_connection;
    QString m_clientUniqueName;
    std::optional<DeferredCreate> m_deferredCreate;
};

} // namespace

class GhosttyGlobalShortcutPortalTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void translatesUnicodeTriggersToXdgKeysyms();
    void translatesPinnedPhysicalKeyMap();
    void rejectsUnrepresentableTriggers();
    void registryUsesOnlyEligibleRootGlobals();
    void registryDiagnosesEveryIneligibleGlobalForm();
    void collisionResolutionIsStableAndPrefersPhysical();
    void registryOrderingIsStable();
    void disconnectedPortalRetainsPureRegistryState();
    void reentrantRegistryObserverKeepsNewestConfig();
    void portalRoundTripIsRaceSafeAndRejectsStaleResponses();
};

void GhosttyGlobalShortcutPortalTest::translatesUnicodeTriggersToXdgKeysyms()
{
    QCOMPARE(ghosttyXdgShortcutFromTrigger(unicode(
                 'q', GhosttyKeybindSuper)),
             std::optional<QString>(QStringLiteral("LOGO+q")));
    QCOMPARE(ghosttyXdgShortcutFromTrigger(unicode(
                 '\\', GhosttyKeybindShift | GhosttyKeybindCtrl
                          | GhosttyKeybindAlt | GhosttyKeybindSuper)),
             std::optional<QString>(
                 QStringLiteral("SHIFT+CTRL+ALT+LOGO+backslash")));
    QCOMPARE(ghosttyXdgShortcutFromTrigger(unicode('\'')),
             std::optional<QString>(QStringLiteral("apostrophe")));
    QCOMPARE(ghosttyXdgShortcutFromTrigger(unicode(0xe9)),
             std::optional<QString>(QStringLiteral("eacute")));
    QCOMPARE(ghosttyXdgShortcutFromTrigger(unicode(0x03bb)),
             std::optional<QString>(QStringLiteral("gcedilla")));
    QCOMPARE(ghosttyXdgShortcutFromTrigger(unicode(0x1f600)),
             std::optional<QString>(QString::fromUtf8("😀")));
}

void GhosttyGlobalShortcutPortalTest::translatesPinnedPhysicalKeyMap()
{
    QCOMPARE(ghosttyXdgShortcutFromTrigger(physical(
                 QStringLiteral("key_a"), GhosttyKeybindCtrl)),
             std::optional<QString>(QStringLiteral("CTRL+a")));
    QCOMPARE(ghosttyXdgShortcutFromTrigger(physical(
                 QStringLiteral("digit_7"), GhosttyKeybindAlt)),
             std::optional<QString>(QStringLiteral("ALT+7")));
    QCOMPARE(ghosttyXdgShortcutFromTrigger(physical(
                 QStringLiteral("arrow_left"), GhosttyKeybindSuper)),
             std::optional<QString>(QStringLiteral("LOGO+Left")));
    QCOMPARE(ghosttyXdgShortcutFromTrigger(physical(
                 QStringLiteral("f25"), GhosttyKeybindShift)),
             std::optional<QString>(QStringLiteral("SHIFT+F25")));
    QCOMPARE(ghosttyXdgShortcutFromTrigger(physical(
                 QStringLiteral("numpad_3"), GhosttyKeybindCtrl)),
             std::optional<QString>(QStringLiteral("CTRL+KP_3")));
    QCOMPARE(ghosttyXdgShortcutFromTrigger(physical(
                 QStringLiteral("numpad_page_down"))),
             std::optional<QString>(QStringLiteral("KP_Page_Down")));
    QCOMPARE(ghosttyXdgShortcutFromTrigger(physical(
                 QStringLiteral("control_right"))),
             std::optional<QString>(QStringLiteral("Control_R")));
}

void GhosttyGlobalShortcutPortalTest::rejectsUnrepresentableTriggers()
{
    QVERIFY(!ghosttyXdgShortcutFromTrigger(catchAll()).has_value());
    QVERIFY(!ghosttyXdgShortcutFromTrigger(
        physical(QStringLiteral("context_menu"))).has_value());
    QVERIFY(!ghosttyXdgShortcutFromTrigger(unicode(0)).has_value());
    QVERIFY(!ghosttyXdgShortcutFromTrigger(unicode(0xd800)).has_value());
    QVERIFY(!ghosttyXdgShortcutFromTrigger(unicode(0x110000)).has_value());

    GhosttyKeybindTrigger invalidModifiers = unicode('a');
    invalidModifiers.modifiers = 0x80;
    QVERIFY(!ghosttyXdgShortcutFromTrigger(invalidModifiers).has_value());
}

void GhosttyGlobalShortcutPortalTest::registryUsesOnlyEligibleRootGlobals()
{
    GhosttyKeybindConfig config;
    config.root = {
        definition(unicode('g', GhosttyKeybindCtrl),
                   QStringLiteral("new_tab")),
        definition(unicode('l', GhosttyKeybindCtrl),
                   QStringLiteral("close_surface"), false),
    };
    config.root.back().flags.all = true;
    config.tables = {
        GhosttyKeybindTable{
            .name = QStringLiteral("command"),
            .bindings = {
                definition(unicode('t', GhosttyKeybindSuper),
                           QStringLiteral("new_tab")),
            },
        },
    };

    const GhosttyGlobalShortcutRegistry registry =
        buildGhosttyGlobalShortcutRegistry(config);
    QCOMPARE(registry.registrations.size(), 1);
    const GhosttyGlobalShortcutRegistration expected{
        .id = QStringLiteral("CTRL+g"),
        .preferredTrigger = QStringLiteral("CTRL+g"),
        .description = QStringLiteral("new_tab"),
        .action = QStringLiteral("new_tab"),
    };
    QCOMPARE(registry.registrations.front(), expected);
    QVERIFY(registry.diagnostics.isEmpty());
}

void GhosttyGlobalShortcutPortalTest::registryDiagnosesEveryIneligibleGlobalForm()
{
    GhosttyKeybindConfig config;
    config.root = {
        definition(QVector<GhosttyKeybindTrigger>{},
                   QStringList{QStringLiteral("new_tab")}),
        definition(QVector<GhosttyKeybindTrigger>{unicode('a'), unicode('b')},
                   QStringList{QStringLiteral("new_tab")}),
        definition(QVector<GhosttyKeybindTrigger>{unicode('c')},
                   QStringList{}),
        definition(QVector<GhosttyKeybindTrigger>{unicode('d')},
                   QStringList{QStringLiteral("new_tab"),
                               QStringLiteral("close_surface")}),
        definition(catchAll(GhosttyKeybindCtrl), QStringLiteral("new_tab")),
        definition(physical(QStringLiteral("context_menu")),
                   QStringLiteral("new_tab")),
    };

    const GhosttyGlobalShortcutRegistry registry =
        buildGhosttyGlobalShortcutRegistry(config);
    QVERIFY(registry.registrations.isEmpty());
    QCOMPARE(diagnosticCount(
                 registry,
                 GhosttyGlobalShortcutDiagnosticCode::SequenceUnsupported),
             2);
    QCOMPARE(diagnosticCount(
                 registry,
                 GhosttyGlobalShortcutDiagnosticCode::ActionChainUnsupported),
             2);
    QCOMPARE(diagnosticCount(
                 registry,
                 GhosttyGlobalShortcutDiagnosticCode::CatchAllUnsupported),
             1);
    QCOMPARE(diagnosticCount(
                 registry,
                 GhosttyGlobalShortcutDiagnosticCode::TriggerUnsupported),
             1);

    for (qsizetype index = 0; index < registry.diagnostics.size(); ++index) {
        QCOMPARE(registry.diagnostics.at(index).rootBindingIndex,
                 static_cast<int>(index));
        QVERIFY(!registry.diagnostics.at(index).message.isEmpty());
    }
}

void GhosttyGlobalShortcutPortalTest::collisionResolutionIsStableAndPrefersPhysical()
{
    const auto build = [](bool physicalFirst) {
        GhosttyKeybindConfig config;
        const auto physicalBinding = definition(
            physical(QStringLiteral("key_a"), GhosttyKeybindCtrl),
            QStringLiteral("physical_action"));
        const auto unicodeBinding = definition(
            unicode('a', GhosttyKeybindCtrl),
            QStringLiteral("unicode_action"));
        config.root = physicalFirst
            ? QVector{physicalBinding, unicodeBinding}
            : QVector{unicodeBinding, physicalBinding};
        return buildGhosttyGlobalShortcutRegistry(config);
    };

    const GhosttyGlobalShortcutRegistry first = build(true);
    const GhosttyGlobalShortcutRegistry second = build(false);
    QCOMPARE(first.registrations, second.registrations);
    QCOMPARE(first.registrations.size(), 1);
    QCOMPARE(first.registrations.front().id, QStringLiteral("CTRL+a"));
    QCOMPARE(first.registrations.front().action,
             QStringLiteral("physical_action"));
    QCOMPARE(diagnosticCount(first,
                             GhosttyGlobalShortcutDiagnosticCode::Collision),
             1);
    QCOMPARE(diagnosticCount(second,
                             GhosttyGlobalShortcutDiagnosticCode::Collision),
             1);
}

void GhosttyGlobalShortcutPortalTest::registryOrderingIsStable()
{
    GhosttyKeybindConfig config;
    config.root = {
        definition(unicode('z', GhosttyKeybindSuper),
                   QStringLiteral("last")),
        definition(unicode('a', GhosttyKeybindCtrl),
                   QStringLiteral("first")),
        definition(physical(QStringLiteral("f2")),
                   QStringLiteral("middle")),
    };

    const GhosttyGlobalShortcutRegistry registry =
        buildGhosttyGlobalShortcutRegistry(config);
    QCOMPARE(registry.registrations.size(), 3);
    QCOMPARE(registry.registrations.at(0).id, QStringLiteral("CTRL+a"));
    QCOMPARE(registry.registrations.at(1).id, QStringLiteral("F2"));
    QCOMPARE(registry.registrations.at(2).id, QStringLiteral("LOGO+z"));
}

void GhosttyGlobalShortcutPortalTest::disconnectedPortalRetainsPureRegistryState()
{
    const QDBusConnection disconnected(
        QStringLiteral("ghostty-qt-test-no-such-connection"));
    QVERIFY(!disconnected.isConnected());

    GhosttyGlobalShortcutPortal portal(disconnected);
    QSignalSpy registrySpy(&portal,
                           &GhosttyGlobalShortcutPortal::registryChanged);
    QSignalSpy activeSpy(&portal,
                         &GhosttyGlobalShortcutPortal::activeChanged);
    QSignalSpy warningSpy(&portal,
                          &GhosttyGlobalShortcutPortal::warningOccurred);

    GhosttyKeybindConfig config;
    config.root = {
        definition(unicode('a', GhosttyKeybindCtrl),
                   QStringLiteral("new_tab")),
    };
    portal.setKeybindConfig(config);

    QCOMPARE(portal.generation(), quint64(1));
    QCOMPARE(portal.registry().registrations.size(), 1);
    QVERIFY(!portal.isActive());
    QVERIFY(portal.sessionHandle().isEmpty());
    QCOMPARE(registrySpy.size(), 1);
    QCOMPARE(activeSpy.size(), 0);
    QCOMPARE(warningSpy.size(), 1);
    QVERIFY(warningSpy.front().front().toString().contains(
        QStringLiteral("not connected")));

    // Only an active session can reuse an equivalent registry. Disconnected
    // state retries registration so a newly available bus is not missed.
    portal.setKeybindConfig(config);
    QCOMPARE(portal.generation(), quint64(2));
    QCOMPARE(registrySpy.size(), 2);
    QCOMPARE(warningSpy.size(), 2);

    // Replacing the config still advances the stale-callback generation and
    // preserves builder diagnostics without requiring a live transport.
    GhosttyKeybindConfig invalid;
    invalid.root = {
        definition({unicode('a'), unicode('b')},
                   {QStringLiteral("new_tab")}),
    };
    portal.setKeybindConfig(invalid);
    QCOMPARE(portal.generation(), quint64(3));
    QVERIFY(portal.registry().registrations.isEmpty());
    QCOMPARE(portal.registry().diagnostics.size(), 1);
    QCOMPARE(registrySpy.size(), 3);
    QCOMPARE(warningSpy.size(), 3);

    portal.clear();
    QCOMPARE(portal.generation(), quint64(4));
    QVERIFY(portal.registry().registrations.isEmpty());
    QVERIFY(portal.registry().diagnostics.isEmpty());
    QCOMPARE(registrySpy.size(), 4);
    QCOMPARE(activeSpy.size(), 0);
}

void GhosttyGlobalShortcutPortalTest::
reentrantRegistryObserverKeepsNewestConfig()
{
    const QDBusConnection disconnected(
        QStringLiteral("ghostty-qt-test-reentrant-no-connection"));
    QVERIFY(!disconnected.isConnected());

    GhosttyGlobalShortcutPortal portal(disconnected);
    QSignalSpy registrySpy(&portal,
                           &GhosttyGlobalShortcutPortal::registryChanged);
    QSignalSpy warningSpy(&portal,
                          &GhosttyGlobalShortcutPortal::warningOccurred);

    GhosttyKeybindConfig first;
    first.root = {
        definition(unicode('a', GhosttyKeybindCtrl),
                   QStringLiteral("new_tab")),
    };
    GhosttyKeybindConfig newer;
    newer.root = {
        definition(unicode('b', GhosttyKeybindCtrl),
                   QStringLiteral("close_tab")),
    };

    bool nested = false;
    connect(&portal, &GhosttyGlobalShortcutPortal::registryChanged,
            &portal, [&] {
                if (nested) return;
                nested = true;
                portal.setKeybindConfig(newer);
            });

    portal.setKeybindConfig(first);

    QVERIFY(nested);
    QCOMPARE(portal.generation(), quint64(2));
    QCOMPARE(registrySpy.count(), 2);
    QCOMPARE(warningSpy.count(), 1);
    QCOMPARE(portal.registry().registrations.size(), 1);
    QCOMPARE(portal.registry().registrations.constFirst().id,
             QStringLiteral("CTRL+b"));
    QCOMPARE(portal.registry().registrations.constFirst().action,
             QStringLiteral("close_tab"));
}

void GhosttyGlobalShortcutPortalTest::
portalRoundTripIsRaceSafeAndRejectsStaleResponses()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon")).isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    FakeGlobalShortcutsPortal fake(bus.server(),
                                   bus.client().baseService());
    QVERIFY(bus.server().registerService(
        QStringLiteral("org.freedesktop.portal.Desktop")));
    QVERIFY(bus.server().registerVirtualObject(
        QStringLiteral("/org/freedesktop/portal/desktop"),
        &fake,
        QDBusConnection::SubPath));

    {
        GhosttyGlobalShortcutPortal portal(bus.client());
        QSignalSpy activationSpy(
            &portal, &GhosttyGlobalShortcutPortal::shortcutActivated);
        QSignalSpy registrySpy(
            &portal, &GhosttyGlobalShortcutPortal::registryChanged);
        QSignalSpy activeSpy(&portal,
                             &GhosttyGlobalShortcutPortal::activeChanged);
        QSignalSpy warningSpy(
            &portal, &GhosttyGlobalShortcutPortal::warningOccurred);

        GhosttyKeybindConfig first;
        first.root = {
            definition(unicode('a', GhosttyKeybindCtrl),
                       QStringLiteral("new_tab")),
        };
        portal.setKeybindConfig(first);

        // The fake sends Response from inside the method handler, before its
        // method reply. Success proves the client subscribed first.
        QTRY_VERIFY_WITH_TIMEOUT(portal.isActive(), 3000);
        QCOMPARE(fake.createCount, 1);
        QCOMPARE(fake.bindCount, 1);
        QCOMPARE(fake.lastShortcuts.size(), 1);
        QCOMPARE(fake.lastShortcuts.front().id, QStringLiteral("CTRL+a"));
        QCOMPARE(optionString(fake.lastShortcuts.front().options,
                              QStringLiteral("description")),
                 QStringLiteral("new_tab"));
        QCOMPARE(optionString(fake.lastShortcuts.front().options,
                              QStringLiteral("preferred_trigger")),
                 QStringLiteral("CTRL+a"));
        QVERIFY(warningSpy.isEmpty());

        const QString firstSession = portal.sessionHandle();
        const quint64 firstGeneration = portal.generation();
        QCOMPARE(registrySpy.size(), 1);
        QCOMPARE(activeSpy.size(), 1);

        // Reapplying the same config, or a config whose non-global bindings
        // differ, produces the same portal registry. Neither case should
        // close the active session or issue another pair of portal requests.
        portal.setKeybindConfig(first);
        GhosttyKeybindConfig equivalent = first;
        equivalent.root.append(
            definition(unicode('z'), QStringLiteral("ignore"), false));
        portal.setKeybindConfig(equivalent);
        QTest::qWait(20);
        QCOMPARE(portal.generation(), firstGeneration);
        QCOMPARE(portal.sessionHandle(), firstSession);
        QVERIFY(portal.isActive());
        QCOMPARE(fake.createCount, 1);
        QCOMPARE(fake.bindCount, 1);
        QCOMPARE(fake.requestCloseCount, 0);
        QCOMPARE(fake.sessionCloseCount, 0);
        QCOMPARE(registrySpy.size(), 1);
        QCOMPARE(activeSpy.size(), 1);

        fake.activate(firstSession, QStringLiteral("CTRL+a"));
        QTRY_COMPARE_WITH_TIMEOUT(activationSpy.size(), 1, 3000);
        QCOMPARE(activationSpy.front().front().toString(),
                 QStringLiteral("new_tab"));

        GhosttyKeybindConfig deferred;
        deferred.root = {
            definition(unicode('b', GhosttyKeybindCtrl),
                       QStringLiteral("close_surface")),
        };
        fake.deferNextCreate = true;
        portal.setKeybindConfig(deferred);
        QTRY_COMPARE_WITH_TIMEOUT(fake.createCount, 2, 3000);
        QVERIFY(!portal.isActive());
        const QString deferredSession = fake.currentSession;

        // Supersede the still-pending request. Its later response must not
        // replace or bind against the new generation's session.
        GhosttyKeybindConfig current;
        current.root = {
            definition(unicode('c', GhosttyKeybindCtrl),
                       QStringLiteral("quit")),
        };
        portal.setKeybindConfig(current);
        QTRY_VERIFY_WITH_TIMEOUT(portal.isActive(), 3000);
        QCOMPARE(fake.createCount, 3);
        QCOMPARE(fake.bindCount, 2);
        const QString currentSession = portal.sessionHandle();
        QVERIFY(currentSession != firstSession);
        QVERIFY(currentSession != deferredSession);

        fake.sendDeferredCreateResponse();
        QTest::qWait(20);
        QCOMPARE(portal.sessionHandle(), currentSession);
        QVERIFY(portal.isActive());
        QCOMPARE(fake.bindCount, 2);

        fake.activate(firstSession, QStringLiteral("CTRL+a"));
        fake.activate(deferredSession, QStringLiteral("CTRL+b"));
        QTest::qWait(20);
        QCOMPARE(activationSpy.size(), 1);
        fake.activate(currentSession, QStringLiteral("CTRL+c"));
        QTRY_COMPARE_WITH_TIMEOUT(activationSpy.size(), 2, 3000);
        QCOMPARE(activationSpy.back().front().toString(),
                 QStringLiteral("quit"));
        QVERIFY(warningSpy.isEmpty());

        // An active-state observer may immediately re-register after an
        // unsolicited close. The stale close callback must neither warn for
        // nor tear down that newer generation.
        GhosttyKeybindConfig activeChangedReplacement;
        activeChangedReplacement.root = {
            definition(unicode('d', GhosttyKeybindCtrl),
                       QStringLiteral("new_window")),
        };
        bool replacedFromActiveChanged = false;
        const QMetaObject::Connection activeChangedConnection = connect(
            &portal, &GhosttyGlobalShortcutPortal::activeChanged,
            &portal, [&](bool active) {
                if (active || replacedFromActiveChanged) return;
                replacedFromActiveChanged = true;
                portal.setKeybindConfig(activeChangedReplacement);
            });
        fake.closeSession(currentSession);
        QTRY_VERIFY_WITH_TIMEOUT(replacedFromActiveChanged, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(portal.isActive(), 3000);
        QObject::disconnect(activeChangedConnection);
        QCOMPARE(fake.createCount, 4);
        QCOMPARE(fake.bindCount, 3);
        QCOMPARE(portal.registry().registrations.constFirst().id,
                 QStringLiteral("CTRL+d"));
        QVERIFY(warningSpy.isEmpty());

        // A failed async request warns synchronously. A warning observer may
        // install a replacement, which the failed request must not close.
        GhosttyKeybindConfig failed;
        failed.root = {
            definition(unicode('e', GhosttyKeybindCtrl),
                       QStringLiteral("close_window")),
        };
        GhosttyKeybindConfig warningReplacement;
        warningReplacement.root = {
            definition(unicode('f', GhosttyKeybindCtrl),
                       QStringLiteral("new_tab")),
        };
        bool replacedFromWarning = false;
        const QMetaObject::Connection warningConnection = connect(
            &portal, &GhosttyGlobalShortcutPortal::warningOccurred,
            &portal, [&](const QString &message) {
                if (replacedFromWarning
                    || !message.contains(
                        QStringLiteral("failed with response code"))) {
                    return;
                }
                replacedFromWarning = true;
                portal.setKeybindConfig(warningReplacement);
            });
        fake.failNextCreate = true;
        portal.setKeybindConfig(failed);
        QTRY_VERIFY_WITH_TIMEOUT(replacedFromWarning, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(portal.isActive(), 3000);
        QObject::disconnect(warningConnection);
        QCOMPARE(fake.createCount, 6);
        QCOMPARE(fake.bindCount, 4);
        QCOMPARE(portal.registry().registrations.constFirst().id,
                 QStringLiteral("CTRL+f"));
        QCOMPARE(warningSpy.size(), 1);

        // Without a reentrant replacement, backend shutdown invalidates the
        // live state and reports the close normally.
        const QString recoveredSession = portal.sessionHandle();
        fake.closeSession(recoveredSession);
        QTRY_VERIFY_WITH_TIMEOUT(!portal.isActive(), 3000);
        QVERIFY(portal.sessionHandle().isEmpty());
        QCOMPARE(warningSpy.size(), 2);
        QVERIFY(warningSpy.back().front().toString().contains(
            QStringLiteral("session was closed")));

        // Re-registration still works after an unsolicited close.
        portal.setKeybindConfig(warningReplacement);
        QTRY_VERIFY_WITH_TIMEOUT(portal.isActive(), 3000);
        QCOMPARE(fake.createCount, 7);
        QCOMPARE(fake.bindCount, 5);

        // Signal delivery must own the mapped action. An earlier receiver can
        // clear the registry without invalidating the exact value observed by
        // receivers later in the same direct emission.
        bool clearedFromActivation = false;
        QString actionAfterClear;
        connect(&portal, &GhosttyGlobalShortcutPortal::shortcutActivated,
                &portal, [&](const QString &) {
                    if (clearedFromActivation) return;
                    clearedFromActivation = true;
                    portal.clear();
                });
        connect(&portal, &GhosttyGlobalShortcutPortal::shortcutActivated,
                &portal, [&](const QString &action) {
                    actionAfterClear = action;
                });
        fake.activate(portal.sessionHandle(), QStringLiteral("CTRL+f"));
        QTRY_VERIFY_WITH_TIMEOUT(!actionAfterClear.isNull(), 3000);
        QVERIFY(clearedFromActivation);
        QCOMPARE(actionAfterClear, QStringLiteral("new_tab"));
        QVERIFY(!portal.isActive());

        QTRY_VERIFY_WITH_TIMEOUT(fake.sessionCloseCount >= 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(fake.requestCloseCount >= 1, 3000);
    }

    QTRY_VERIFY_WITH_TIMEOUT(fake.sessionCloseCount >= 2, 3000);
    bus.server().unregisterObject(
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QDBusConnection::UnregisterTree);
    bus.server().unregisterService(
        QStringLiteral("org.freedesktop.portal.Desktop"));
}

QTEST_GUILESS_MAIN(GhosttyGlobalShortcutPortalTest)

#include "test_ghostty_global_shortcut_portal.moc"
