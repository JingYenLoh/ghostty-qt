#pragma once

#include "ghostty_shell_integration_mode.h"
#include "terminal_command.h"
#include "workspace_ids.h"

#include <QByteArray>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QList>
#include <QString>
#include <QStringList>

#include <chrono>
#include <expected>
#include <optional>
#include <span>

enum class GhosttyApplicationIpcAction {
    NewTab,
    NewWindow,
    ToggleQuickTerminal,
};

// The GTK action transports this as one D-Bus struct with signature (tas): a
// nonzero process-global surface target (zero means focused) plus the argv
// overrides for the first surface in the new tab.
struct GhosttyNewTabIpcParameter final {
    quint64 surfaceId = 0;
    QStringList arguments;

    bool operator==(const GhosttyNewTabIpcParameter &) const = default;
};

inline QDBusArgument &operator<<(QDBusArgument &argument,
                                 const GhosttyNewTabIpcParameter &parameter)
{
    argument.beginStructure();
    argument << parameter.surfaceId << parameter.arguments;
    argument.endStructure();
    return argument;
}

inline const QDBusArgument &operator>>(const QDBusArgument &argument,
                                       GhosttyNewTabIpcParameter &parameter)
{
    argument.beginStructure();
    argument >> parameter.surfaceId >> parameter.arguments;
    argument.endStructure();
    return argument;
}

// Process facts used while preparing the argv payload. Keeping them explicit
// makes path expansion and realpath behavior deterministic in tests while the
// production factory samples them at the command's point of invocation.
struct GhosttyApplicationIpcParseContext final {
    QString defaultApplicationId;
    QString workingDirectory;
    QString homeDirectory;
    QByteArray surfaceIdEnvironment;

    [[nodiscard]] static GhosttyApplicationIpcParseContext
    fromProcess(QString defaultApplicationId);

    bool operator==(const GhosttyApplicationIpcParseContext &) const = default;
};

struct GhosttyApplicationIpcError final {
    QString diagnostic;

    [[nodiscard]] constexpr int exitCode() const noexcept { return 1; }

    bool operator==(const GhosttyApplicationIpcError &) const = default;
};

// A direct representation of org.gtk.Actions.Activate(s, av, a{sv}).
// new-window carries one string array; new-tab carries one (tas) tuple. Both
// inject the caller cwd when no concrete working directory was supplied.
struct GhosttyApplicationIpcRequest final {
    QString applicationId;
    QString objectPath;
    QString actionName;
    std::optional<QStringList> stringArrayParameter;
    std::optional<GhosttyNewTabIpcParameter> newTabParameter;

    bool operator==(const GhosttyApplicationIpcRequest &) const = default;
};

// Parse a full process argv, including argv[0]. The byte-list overload is the
// pure entry point and rejects values that cannot be represented losslessly by
// a D-Bus UTF-8 string. The span overload preserves the operating system argv
// bytes before Qt command-line decoding can replace malformed input.
[[nodiscard]] std::expected<GhosttyApplicationIpcRequest,
                            GhosttyApplicationIpcError>
parseGhosttyApplicationIpcRequest(
    GhosttyApplicationIpcAction selectedAction,
    const QList<QByteArray> &arguments,
    const GhosttyApplicationIpcParseContext &context);

[[nodiscard]] std::expected<GhosttyApplicationIpcRequest,
                            GhosttyApplicationIpcError>
parseGhosttyApplicationIpcRequest(
    GhosttyApplicationIpcAction selectedAction,
    std::span<char *const> arguments,
    const GhosttyApplicationIpcParseContext &context);

[[nodiscard]] QDBusConnection defaultGhosttyApplicationIpcConnection();

// Perform exactly one synchronous call. D-Bus service activation is left to
// the bus; failures are not retried and never fall back to a local action.
[[nodiscard]] std::expected<void, GhosttyApplicationIpcError>
sendGhosttyApplicationIpcRequest(
    const GhosttyApplicationIpcRequest &request,
    const QDBusConnection &connection =
        defaultGhosttyApplicationIpcConnection(),
    std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});

// Receiver-side projection shared by new-window-command and new-tab. Unknown
// entries are deliberately absent from this DTO.
struct GhosttyNewWindowTransportOverrides final {
    std::optional<TerminalCommand> command;
    std::optional<GhosttyShellIntegrationMode> shellIntegration;
    std::optional<QString> workingDirectory;
    std::optional<QString> titleOverride;

    bool operator==(const GhosttyNewWindowTransportOverrides &) const = default;
};

[[nodiscard]] std::expected<GhosttyNewWindowTransportOverrides,
                            GhosttyApplicationIpcError>
decodeGhosttyNewWindowArguments(const QStringList &arguments);

struct GhosttyNewTabTransportRequest final {
    SurfaceId surfaceId;
    GhosttyNewWindowTransportOverrides overrides;

    bool operator==(const GhosttyNewTabTransportRequest &) const = default;
};

[[nodiscard]] std::expected<GhosttyNewTabTransportRequest,
                            GhosttyApplicationIpcError>
decodeGhosttyNewTabParameter(const GhosttyNewTabIpcParameter &parameter);

Q_DECLARE_METATYPE(GhosttyNewTabIpcParameter)
