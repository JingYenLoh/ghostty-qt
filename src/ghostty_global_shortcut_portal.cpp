#include "ghostty_global_shortcut_portal.h"

#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QMap>
#include <QSet>
#include <QUuid>
#include <QVariantMap>
#include <xkbcommon/xkbcommon.h>

#include <utility>

namespace {

constexpr auto PortalService = "org.freedesktop.portal.Desktop";
constexpr auto PortalPath = "/org/freedesktop/portal/desktop";
constexpr auto GlobalShortcutsInterface =
    "org.freedesktop.portal.GlobalShortcuts";
constexpr auto RequestInterface = "org.freedesktop.portal.Request";
constexpr auto SessionInterface = "org.freedesktop.portal.Session";

struct PortalShortcut {
    QString id;
    QVariantMap options;
};

using PortalShortcutList = QList<PortalShortcut>;

QDBusArgument &operator<<(QDBusArgument &argument,
                          const PortalShortcut &shortcut)
{
    argument.beginStructure();
    argument << shortcut.id << shortcut.options;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                PortalShortcut &shortcut)
{
    argument.beginStructure();
    argument >> shortcut.id >> shortcut.options;
    argument.endStructure();
    return argument;
}

QString newPortalToken()
{
    QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    token.remove(u'-');
    return QStringLiteral("ghosttyqt_%1").arg(token);
}

QString modifierPrefix(quint8 modifiers)
{
    QString result;
    if ((modifiers & GhosttyKeybindShift) != 0) {
        result += QStringLiteral("SHIFT+");
    }
    if ((modifiers & GhosttyKeybindCtrl) != 0) {
        result += QStringLiteral("CTRL+");
    }
    if ((modifiers & GhosttyKeybindAlt) != 0) {
        result += QStringLiteral("ALT+");
    }
    if ((modifiers & GhosttyKeybindSuper) != 0) {
        result += QStringLiteral("LOGO+");
    }
    return result;
}

std::optional<QString> physicalKeysym(QStringView name)
{
    if (name.startsWith(QLatin1StringView("key_")) && name.size() == 5) {
        const QChar letter = name.back();
        if (letter >= u'a' && letter <= u'z') {
            return QString(letter);
        }
    }
    if (name.startsWith(QLatin1StringView("digit_")) && name.size() == 7) {
        const QChar digit = name.back();
        if (digit >= u'0' && digit <= u'9') {
            return QString(digit);
        }
    }
    if (name.startsWith(u'f') && name.size() >= 2 && name.size() <= 3) {
        bool valid = false;
        const int number = name.sliced(1).toInt(&valid);
        if (valid && number >= 1 && number <= 25) {
            return QStringLiteral("F%1").arg(number);
        }
    }
    if (name.startsWith(QLatin1StringView("numpad_")) && name.size() == 8) {
        const QChar digit = name.back();
        if (digit >= u'0' && digit <= u'9') {
            return QStringLiteral("KP_%1").arg(digit);
        }
    }

    static const QHash<QString, QString> keysyms = {
        {QStringLiteral("semicolon"), QStringLiteral("semicolon")},
        {QStringLiteral("space"), QStringLiteral("space")},
        {QStringLiteral("quote"), QStringLiteral("apostrophe")},
        {QStringLiteral("comma"), QStringLiteral("comma")},
        {QStringLiteral("backquote"), QStringLiteral("grave")},
        {QStringLiteral("period"), QStringLiteral("period")},
        {QStringLiteral("slash"), QStringLiteral("slash")},
        {QStringLiteral("minus"), QStringLiteral("minus")},
        {QStringLiteral("equal"), QStringLiteral("equal")},
        {QStringLiteral("bracket_left"), QStringLiteral("bracketleft")},
        {QStringLiteral("bracket_right"), QStringLiteral("bracketright")},
        {QStringLiteral("backslash"), QStringLiteral("backslash")},
        {QStringLiteral("arrow_up"), QStringLiteral("Up")},
        {QStringLiteral("arrow_down"), QStringLiteral("Down")},
        {QStringLiteral("arrow_right"), QStringLiteral("Right")},
        {QStringLiteral("arrow_left"), QStringLiteral("Left")},
        {QStringLiteral("home"), QStringLiteral("Home")},
        {QStringLiteral("end"), QStringLiteral("End")},
        {QStringLiteral("insert"), QStringLiteral("Insert")},
        {QStringLiteral("delete"), QStringLiteral("Delete")},
        {QStringLiteral("caps_lock"), QStringLiteral("Caps_Lock")},
        {QStringLiteral("scroll_lock"), QStringLiteral("Scroll_Lock")},
        {QStringLiteral("num_lock"), QStringLiteral("Num_Lock")},
        {QStringLiteral("page_up"), QStringLiteral("Page_Up")},
        {QStringLiteral("page_down"), QStringLiteral("Page_Down")},
        {QStringLiteral("escape"), QStringLiteral("Escape")},
        {QStringLiteral("enter"), QStringLiteral("Return")},
        {QStringLiteral("tab"), QStringLiteral("Tab")},
        {QStringLiteral("backspace"), QStringLiteral("BackSpace")},
        {QStringLiteral("print_screen"), QStringLiteral("Print")},
        {QStringLiteral("pause"), QStringLiteral("Pause")},
        {QStringLiteral("numpad_decimal"), QStringLiteral("KP_Decimal")},
        {QStringLiteral("numpad_divide"), QStringLiteral("KP_Divide")},
        {QStringLiteral("numpad_multiply"), QStringLiteral("KP_Multiply")},
        {QStringLiteral("numpad_subtract"), QStringLiteral("KP_Subtract")},
        {QStringLiteral("numpad_add"), QStringLiteral("KP_Add")},
        {QStringLiteral("numpad_enter"), QStringLiteral("KP_Enter")},
        {QStringLiteral("numpad_equal"), QStringLiteral("KP_Equal")},
        {QStringLiteral("numpad_separator"), QStringLiteral("KP_Separator")},
        {QStringLiteral("numpad_left"), QStringLiteral("KP_Left")},
        {QStringLiteral("numpad_right"), QStringLiteral("KP_Right")},
        {QStringLiteral("numpad_up"), QStringLiteral("KP_Up")},
        {QStringLiteral("numpad_down"), QStringLiteral("KP_Down")},
        {QStringLiteral("numpad_page_up"), QStringLiteral("KP_Page_Up")},
        {QStringLiteral("numpad_page_down"), QStringLiteral("KP_Page_Down")},
        {QStringLiteral("numpad_home"), QStringLiteral("KP_Home")},
        {QStringLiteral("numpad_end"), QStringLiteral("KP_End")},
        {QStringLiteral("numpad_insert"), QStringLiteral("KP_Insert")},
        {QStringLiteral("numpad_delete"), QStringLiteral("KP_Delete")},
        {QStringLiteral("numpad_begin"), QStringLiteral("KP_Begin")},
        {QStringLiteral("copy"), QStringLiteral("Copy")},
        {QStringLiteral("cut"), QStringLiteral("Cut")},
        {QStringLiteral("paste"), QStringLiteral("Paste")},
        {QStringLiteral("shift_left"), QStringLiteral("Shift_L")},
        {QStringLiteral("control_left"), QStringLiteral("Control_L")},
        {QStringLiteral("alt_left"), QStringLiteral("Alt_L")},
        {QStringLiteral("meta_left"), QStringLiteral("Super_L")},
        {QStringLiteral("shift_right"), QStringLiteral("Shift_R")},
        {QStringLiteral("control_right"), QStringLiteral("Control_R")},
        {QStringLiteral("alt_right"), QStringLiteral("Alt_R")},
        {QStringLiteral("meta_right"), QStringLiteral("Super_R")},
    };

    const auto found = keysyms.constFind(name.toString());
    if (found == keysyms.cend()) {
        return std::nullopt;
    }
    return *found;
}

std::optional<QString> unicodeKeysym(quint32 codepoint)
{
    if (codepoint > 0x10ffff
        || (codepoint >= 0xd800 && codepoint <= 0xdfff)
        || codepoint < 0x20 || (codepoint >= 0x7f && codepoint < 0xa0)) {
        return std::nullopt;
    }

    // Ghostty's GTK frontend passes the raw Unicode value to
    // gdk_keyval_name. GDK keyvals and XKB keysyms share these names, including
    // surprising legacy overlaps (for example U+03BB is the raw keysym named
    // "gcedilla"). Unnamed XKB values are formatted as 0x..., whereas GDK
    // returns null and Ghostty falls back to the literal character.
    char name[64] = {};
    const int length = xkb_keysym_get_name(
        static_cast<xkb_keysym_t>(codepoint), name, sizeof(name));
    if (length > 0 && length < static_cast<int>(sizeof(name))) {
        const QString keysym = QString::fromLatin1(name, length);
        if (!keysym.startsWith(QLatin1StringView("0x"))) {
            return keysym;
        }
    }

    const char32_t character = static_cast<char32_t>(codepoint);
    return QString::fromUcs4(&character, 1);
}

QString triggerIdentity(const GhosttyKeybindTrigger &trigger)
{
    switch (trigger.kind) {
    case GhosttyKeybindKeyKind::Physical:
        return QStringLiteral("0:%1:%2")
            .arg(trigger.physicalName)
            .arg(trigger.modifiers);
    case GhosttyKeybindKeyKind::Unicode:
        return QStringLiteral("1:%1:%2")
            .arg(trigger.unicodeCodepoint, 8, 16, QChar(u'0'))
            .arg(trigger.modifiers);
    case GhosttyKeybindKeyKind::CatchAll:
        return QStringLiteral("2:%1").arg(trigger.modifiers);
    }
    return {};
}

struct Candidate {
    GhosttyGlobalShortcutRegistration registration;
    GhosttyKeybindTrigger trigger;
    int rootIndex = -1;
};

bool preferredCandidate(const Candidate &candidate, const Candidate &current)
{
    const int candidateKind = candidate.trigger.kind
            == GhosttyKeybindKeyKind::Physical
        ? 0
        : 1;
    const int currentKind = current.trigger.kind
            == GhosttyKeybindKeyKind::Physical
        ? 0
        : 1;
    if (candidateKind != currentKind) {
        return candidateKind < currentKind;
    }

    const QString candidateIdentity = triggerIdentity(candidate.trigger);
    const QString currentIdentity = triggerIdentity(current.trigger);
    if (candidateIdentity != currentIdentity) {
        return candidateIdentity < currentIdentity;
    }
    if (candidate.registration.action != current.registration.action) {
        return candidate.registration.action < current.registration.action;
    }
    return candidate.rootIndex < current.rootIndex;
}

GhosttyGlobalShortcutDiagnostic diagnostic(
    GhosttyGlobalShortcutDiagnosticCode code,
    int rootIndex,
    const QString &reason)
{
    return GhosttyGlobalShortcutDiagnostic{
        .code = code,
        .rootBindingIndex = rootIndex,
        .message = QStringLiteral("Global keybind %1 was skipped: %2")
                       .arg(rootIndex)
                       .arg(reason),
    };
}

QVariant unwrapDbusVariant(QVariant value)
{
    while (value.metaType() == QMetaType::fromType<QDBusVariant>()) {
        value = value.value<QDBusVariant>().variant();
    }
    return value;
}

QString sessionHandleFromResults(const QVariantMap &results)
{
    const auto found = results.constFind(QStringLiteral("session_handle"));
    if (found == results.cend()) {
        return {};
    }

    const QVariant value = unwrapDbusVariant(*found);
    if (value.canConvert<QDBusObjectPath>()) {
        const QString path = value.value<QDBusObjectPath>().path();
        if (!path.isEmpty()) {
            return path;
        }
    }
    return value.toString();
}

} // namespace

Q_DECLARE_METATYPE(PortalShortcut)
Q_DECLARE_METATYPE(PortalShortcutList)

std::optional<QString> ghosttyXdgShortcutFromTrigger(
    const GhosttyKeybindTrigger &trigger)
{
    constexpr quint8 knownModifiers = GhosttyKeybindShift
        | GhosttyKeybindCtrl | GhosttyKeybindAlt | GhosttyKeybindSuper;
    if ((trigger.modifiers & ~knownModifiers) != 0) {
        return std::nullopt;
    }

    std::optional<QString> key;
    switch (trigger.kind) {
    case GhosttyKeybindKeyKind::Physical:
        key = physicalKeysym(trigger.physicalName);
        break;
    case GhosttyKeybindKeyKind::Unicode:
        key = unicodeKeysym(trigger.unicodeCodepoint);
        break;
    case GhosttyKeybindKeyKind::CatchAll:
        return std::nullopt;
    }

    if (!key.has_value() || key->isEmpty()) {
        return std::nullopt;
    }
    return modifierPrefix(trigger.modifiers) + *key;
}

GhosttyGlobalShortcutRegistry buildGhosttyGlobalShortcutRegistry(
    const GhosttyKeybindConfig &config)
{
    GhosttyGlobalShortcutRegistry result;
    QMap<QString, Candidate> selected;

    for (qsizetype index = 0; index < config.root.size(); ++index) {
        const GhosttyKeybindDefinition &definition = config.root.at(index);
        if (!definition.flags.global) {
            continue;
        }
        if (definition.sequence.size() != 1) {
            result.diagnostics.append(diagnostic(
                GhosttyGlobalShortcutDiagnosticCode::SequenceUnsupported,
                static_cast<int>(index),
                QStringLiteral("the portal does not support key sequences")));
            continue;
        }
        if (definition.actions.size() != 1) {
            result.diagnostics.append(diagnostic(
                GhosttyGlobalShortcutDiagnosticCode::ActionChainUnsupported,
                static_cast<int>(index),
                QStringLiteral("the pinned Linux frontend only registers one action")));
            continue;
        }

        const GhosttyKeybindTrigger &trigger = definition.sequence.front();
        if (trigger.kind == GhosttyKeybindKeyKind::CatchAll) {
            result.diagnostics.append(diagnostic(
                GhosttyGlobalShortcutDiagnosticCode::CatchAllUnsupported,
                static_cast<int>(index),
                QStringLiteral("catch-all triggers cannot be represented by XDG")));
            continue;
        }

        const auto preferredTrigger = ghosttyXdgShortcutFromTrigger(trigger);
        if (!preferredTrigger.has_value()) {
            result.diagnostics.append(diagnostic(
                GhosttyGlobalShortcutDiagnosticCode::TriggerUnsupported,
                static_cast<int>(index),
                QStringLiteral("the trigger has no XDG keysym representation")));
            continue;
        }

        Candidate candidate{
            .registration = GhosttyGlobalShortcutRegistration{
                .id = *preferredTrigger,
                .preferredTrigger = *preferredTrigger,
                .description = definition.actions.front(),
                .action = definition.actions.front(),
            },
            .trigger = trigger,
            .rootIndex = static_cast<int>(index),
        };

        auto existing = selected.find(candidate.registration.id);
        if (existing == selected.end()) {
            selected.insert(candidate.registration.id, std::move(candidate));
            continue;
        }

        const bool replace = preferredCandidate(candidate, *existing);
        const Candidate &winner = replace ? candidate : *existing;
        const Candidate &loser = replace ? *existing : candidate;
        result.diagnostics.append(GhosttyGlobalShortcutDiagnostic{
            .code = GhosttyGlobalShortcutDiagnosticCode::Collision,
            .rootBindingIndex = loser.rootIndex,
            .message = QStringLiteral(
                "Global keybind %1 was skipped: XDG trigger `%2` collides with "
                "binding %3; binding %3 wins deterministically")
                           .arg(loser.rootIndex)
                           .arg(candidate.registration.id)
                           .arg(winner.rootIndex),
        });
        if (replace) {
            *existing = std::move(candidate);
        }
    }

    result.registrations.reserve(selected.size());
    for (const Candidate &candidate : std::as_const(selected)) {
        result.registrations.append(candidate.registration);
    }
    return result;
}

GhosttyGlobalShortcutPortal::GhosttyGlobalShortcutPortal(
    const QDBusConnection &connection,
    QObject *parent)
    : QObject(parent)
    , m_connection(connection)
{
    static const bool registered = [] {
        qDBusRegisterMetaType<PortalShortcut>();
        qDBusRegisterMetaType<PortalShortcutList>();
        return true;
    }();
    Q_UNUSED(registered);
}

GhosttyGlobalShortcutPortal::~GhosttyGlobalShortcutPortal()
{
    ++m_generation;
    closePortalState(false);
}

const GhosttyGlobalShortcutRegistry &
GhosttyGlobalShortcutPortal::registry() const noexcept
{
    return m_registry;
}

quint64 GhosttyGlobalShortcutPortal::generation() const noexcept
{
    return m_generation;
}

bool GhosttyGlobalShortcutPortal::isActive() const noexcept
{
    return m_active;
}

QString GhosttyGlobalShortcutPortal::sessionHandle() const
{
    return m_sessionHandle;
}

void GhosttyGlobalShortcutPortal::setKeybindConfig(
    const GhosttyKeybindConfig &config)
{
    ++m_generation;
    closePortalState(true);
    m_registry = buildGhosttyGlobalShortcutRegistry(config);
    for (const GhosttyGlobalShortcutRegistration &registration
         : std::as_const(m_registry.registrations)) {
        m_actionsById.insert(registration.id, registration.action);
    }
    Q_EMIT registryChanged();

    for (const GhosttyGlobalShortcutDiagnostic &entry
         : std::as_const(m_registry.diagnostics)) {
        warn(entry.message);
    }

    if (m_registry.registrations.isEmpty()) {
        return;
    }
    if (!m_connection.isConnected()) {
        warn(QStringLiteral(
            "Global shortcuts are unavailable because the session D-Bus is not connected"));
        return;
    }
    beginCreateSession();
}

void GhosttyGlobalShortcutPortal::clear()
{
    ++m_generation;
    closePortalState(true);
    m_registry = {};
    Q_EMIT registryChanged();
}

void GhosttyGlobalShortcutPortal::beginCreateSession()
{
    const QString requestToken = newPortalToken();
    const QString requestPath = subscribeToResponse(
        RequestKind::CreateSession, requestToken);
    if (requestPath.isEmpty()) {
        closePortalState(true);
        return;
    }

    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), requestToken);
    options.insert(QStringLiteral("session_handle_token"), newPortalToken());

    QDBusMessage call = QDBusMessage::createMethodCall(
        QString::fromLatin1(PortalService),
        QString::fromLatin1(PortalPath),
        QString::fromLatin1(GlobalShortcutsInterface),
        QStringLiteral("CreateSession"));
    call << options;
    beginRequest(RequestKind::CreateSession, call, requestPath);
}

void GhosttyGlobalShortcutPortal::beginBindShortcuts()
{
    if (m_sessionHandle.isEmpty()) {
        warn(QStringLiteral(
            "Global shortcut registration stopped because no portal session exists"));
        closePortalState(true);
        return;
    }

    const QString requestToken = newPortalToken();
    const QString requestPath = subscribeToResponse(
        RequestKind::BindShortcuts, requestToken);
    if (requestPath.isEmpty()) {
        closePortalState(true);
        return;
    }

    PortalShortcutList shortcuts;
    shortcuts.reserve(m_registry.registrations.size());
    for (const GhosttyGlobalShortcutRegistration &registration
         : std::as_const(m_registry.registrations)) {
        QVariantMap properties;
        properties.insert(QStringLiteral("description"),
                          registration.description);
        properties.insert(QStringLiteral("preferred_trigger"),
                          registration.preferredTrigger);
        shortcuts.append(PortalShortcut{
            .id = registration.id,
            .options = std::move(properties),
        });
    }

    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), requestToken);

    QDBusMessage call = QDBusMessage::createMethodCall(
        QString::fromLatin1(PortalService),
        QString::fromLatin1(PortalPath),
        QString::fromLatin1(GlobalShortcutsInterface),
        QStringLiteral("BindShortcuts"));
    call << QVariant::fromValue(QDBusObjectPath(m_sessionHandle))
         << QVariant::fromValue(shortcuts)
         << QString()
         << options;
    beginRequest(RequestKind::BindShortcuts, call, requestPath);
}

void GhosttyGlobalShortcutPortal::beginRequest(
    RequestKind kind,
    const QDBusMessage &methodCall,
    const QString &expectedPath)
{
    const quint64 requestGeneration = m_generation;
    QDBusPendingCall pending = m_connection.asyncCall(methodCall);
    auto *watcher = new QDBusPendingCallWatcher(pending, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, kind, requestGeneration, expectedPath](
                QDBusPendingCallWatcher *finished) {
        const QDBusPendingReply<QDBusObjectPath> reply = *finished;
        finished->deleteLater();

        auto expected = m_pendingRequests.constFind(expectedPath);
        if (requestGeneration != m_generation
            || expected == m_pendingRequests.cend()
            || expected->generation != requestGeneration
            || expected->kind != kind) {
            return;
        }
        const PendingRequest request = *expected;

        if (reply.isError()) {
            finishPendingRequest(request.canonicalPath);
            warn(QStringLiteral("XDG global shortcut portal call failed: %1")
                     .arg(reply.error().message()));
            closePortalState(true);
            return;
        }

        const QString actualPath = reply.value().path();
        if (actualPath.isEmpty() || actualPath == expectedPath
            || m_pendingRequests.contains(actualPath)) {
            return;
        }

        // Modern portals return the predictable path constructed from the
        // handle token. Older implementations may return a different path;
        // keep the pre-call subscription and add a compatibility alias.
        if (!subscribeToResponsePath(actualPath, request)) {
            warn(QStringLiteral(
                     "Could not subscribe to portal compatibility response path `%1`")
                     .arg(actualPath));
        }
    });
}

QString GhosttyGlobalShortcutPortal::subscribeToResponse(
    RequestKind kind,
    const QString &requestToken)
{
    QString sender = m_connection.baseService();
    if (sender.startsWith(u':')) {
        sender.remove(0, 1);
    }
    sender.replace(u'.', u'_');
    sender.replace(u'-', u'_');
    if (sender.isEmpty()) {
        warn(QStringLiteral(
            "Global shortcuts are unavailable because D-Bus has no unique sender name"));
        return {};
    }

    const QString path = QStringLiteral(
        "/org/freedesktop/portal/desktop/request/%1/%2")
                             .arg(sender, requestToken);
    const PendingRequest request{
        .kind = kind,
        .generation = m_generation,
        .canonicalPath = path,
    };
    if (!subscribeToResponsePath(path, request)) {
        warn(QStringLiteral(
                 "Could not subscribe to XDG portal response path `%1`")
                 .arg(path));
        return {};
    }
    return path;
}

bool GhosttyGlobalShortcutPortal::subscribeToResponsePath(
    const QString &path,
    const PendingRequest &request)
{
    // This connection is intentionally installed before asyncCall. Portals are
    // allowed to emit Response immediately, before the method reply arrives.
    const bool connected = m_connection.connect(
        QString::fromLatin1(PortalService),
        path,
        QString::fromLatin1(RequestInterface),
        QStringLiteral("Response"),
        this,
        SLOT(onRequestResponse(uint,QVariantMap,QDBusMessage)));
    if (connected) {
        m_pendingRequests.insert(path, request);
    }
    return connected;
}

void GhosttyGlobalShortcutPortal::finishPendingRequest(
    const QString &canonicalPath)
{
    const auto paths = m_pendingRequests.keys();
    for (const QString &path : paths) {
        const auto found = m_pendingRequests.constFind(path);
        if (found == m_pendingRequests.cend()
            || found->canonicalPath != canonicalPath) {
            continue;
        }
        m_connection.disconnect(
            QString::fromLatin1(PortalService),
            path,
            QString::fromLatin1(RequestInterface),
            QStringLiteral("Response"),
            this,
            SLOT(onRequestResponse(uint,QVariantMap,QDBusMessage)));
        m_pendingRequests.remove(path);
    }
}

void GhosttyGlobalShortcutPortal::onRequestResponse(
    uint response,
    const QVariantMap &results,
    const QDBusMessage &message)
{
    const auto found = m_pendingRequests.constFind(message.path());
    if (found == m_pendingRequests.cend()) {
        return;
    }

    const PendingRequest request = *found;
    finishPendingRequest(request.canonicalPath);
    if (request.generation != m_generation) {
        return;
    }

    if (response != 0) {
        const QString outcome = response == 1
            ? QStringLiteral("was cancelled")
            : QStringLiteral("failed with response code %1").arg(response);
        warn(QStringLiteral("XDG global shortcut portal request %1")
                 .arg(outcome));
        closePortalState(true);
        return;
    }

    if (request.kind == RequestKind::BindShortcuts) {
        setActive(true);
        return;
    }

    const QString handle = sessionHandleFromResults(results);
    if (handle.isEmpty() || !handle.startsWith(u'/')) {
        warn(QStringLiteral(
            "XDG global shortcut portal returned no valid session handle"));
        closePortalState(true);
        return;
    }
    m_sessionHandle = handle;

    m_sessionClosedSubscribed = m_connection.connect(
        QString::fromLatin1(PortalService),
        m_sessionHandle,
        QString::fromLatin1(SessionInterface),
        QStringLiteral("Closed"),
        this,
        SLOT(onSessionClosed(QVariantMap,QDBusMessage)));
    if (!m_sessionClosedSubscribed) {
        warn(QStringLiteral(
            "Could not subscribe to XDG global shortcut session closure"));
        closePortalState(true);
        return;
    }

    m_activationSubscribed = m_connection.connect(
        QString::fromLatin1(PortalService),
        QString::fromLatin1(PortalPath),
        QString::fromLatin1(GlobalShortcutsInterface),
        QStringLiteral("Activated"),
        this,
        SLOT(onActivated(QDBusObjectPath,QString,qulonglong,QVariantMap,QDBusMessage)));
    if (!m_activationSubscribed) {
        warn(QStringLiteral(
            "Could not subscribe to XDG global shortcut activations"));
        closePortalState(true);
        return;
    }
    beginBindShortcuts();
}

void GhosttyGlobalShortcutPortal::onActivated(
    const QDBusObjectPath &sessionHandle,
    const QString &shortcutId,
    qulonglong timestamp,
    const QVariantMap &options,
    const QDBusMessage &message)
{
    Q_UNUSED(timestamp);
    Q_UNUSED(options);
    Q_UNUSED(message);

    if (!m_active || sessionHandle.path() != m_sessionHandle) {
        return;
    }
    const auto action = m_actionsById.constFind(shortcutId);
    if (action == m_actionsById.cend()) {
        return;
    }
    Q_EMIT shortcutActivated(*action);
}

void GhosttyGlobalShortcutPortal::onSessionClosed(
    const QVariantMap &details,
    const QDBusMessage &message)
{
    Q_UNUSED(details);
    if (!m_sessionClosedSubscribed || message.path() != m_sessionHandle) {
        return;
    }

    // The portal already destroyed the session. Disconnect this exact path
    // before clearing it so closePortalState does not send a redundant Close.
    m_connection.disconnect(
        QString::fromLatin1(PortalService),
        m_sessionHandle,
        QString::fromLatin1(SessionInterface),
        QStringLiteral("Closed"),
        this,
        SLOT(onSessionClosed(QVariantMap,QDBusMessage)));
    m_sessionClosedSubscribed = false;
    m_sessionHandle.clear();
    closePortalState(true);
    warn(QStringLiteral("XDG global shortcut portal session was closed"));
}

void GhosttyGlobalShortcutPortal::closePortalState(bool notify)
{
    const QSet<QString> requestPaths(m_pendingRequests.keyBegin(),
                                     m_pendingRequests.keyEnd());
    for (const QString &path : requestPaths) {
        m_connection.disconnect(
            QString::fromLatin1(PortalService),
            path,
            QString::fromLatin1(RequestInterface),
            QStringLiteral("Response"),
            this,
            SLOT(onRequestResponse(uint,QVariantMap,QDBusMessage)));
        if (m_connection.isConnected()) {
            QDBusMessage closeRequest = QDBusMessage::createMethodCall(
                QString::fromLatin1(PortalService),
                path,
                QString::fromLatin1(RequestInterface),
                QStringLiteral("Close"));
            m_connection.call(closeRequest, QDBus::NoBlock);
        }
    }
    m_pendingRequests.clear();

    if (m_activationSubscribed) {
        m_connection.disconnect(
            QString::fromLatin1(PortalService),
            QString::fromLatin1(PortalPath),
            QString::fromLatin1(GlobalShortcutsInterface),
            QStringLiteral("Activated"),
            this,
            SLOT(onActivated(QDBusObjectPath,QString,qulonglong,QVariantMap,QDBusMessage)));
        m_activationSubscribed = false;
    }

    if (m_sessionClosedSubscribed) {
        m_connection.disconnect(
            QString::fromLatin1(PortalService),
            m_sessionHandle,
            QString::fromLatin1(SessionInterface),
            QStringLiteral("Closed"),
            this,
            SLOT(onSessionClosed(QVariantMap,QDBusMessage)));
        m_sessionClosedSubscribed = false;
    }

    if (!m_sessionHandle.isEmpty() && m_connection.isConnected()) {
        QDBusMessage closeSession = QDBusMessage::createMethodCall(
            QString::fromLatin1(PortalService),
            m_sessionHandle,
            QString::fromLatin1(SessionInterface),
            QStringLiteral("Close"));
        m_connection.call(closeSession, QDBus::NoBlock);
    }
    m_sessionHandle.clear();
    m_actionsById.clear();

    if (m_active) {
        m_active = false;
        if (notify) {
            Q_EMIT activeChanged(false);
        }
    }
}

void GhosttyGlobalShortcutPortal::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    Q_EMIT activeChanged(active);
}

void GhosttyGlobalShortcutPortal::warn(const QString &message)
{
    Q_EMIT warningOccurred(message);
}
