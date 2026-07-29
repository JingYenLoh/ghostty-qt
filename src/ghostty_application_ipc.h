#pragma once

#include "terminal_command.h"

#include <QByteArray>
#include <QDBusConnection>
#include <QList>
#include <QString>
#include <QStringList>

#include <chrono>
#include <expected>
#include <optional>
#include <span>

enum class GhosttyApplicationIpcAction {
    NewWindow,
    ToggleQuickTerminal,
};

// Process facts used while preparing the argv payload. Keeping them explicit
// makes path expansion and realpath behavior deterministic in tests while the
// production factory samples them at the command's point of invocation.
struct GhosttyApplicationIpcParseContext final {
    QString defaultApplicationId;
    QString workingDirectory;
    QString homeDirectory;

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
// new-window always carries a single string-array parameter because the caller
// cwd is injected when no concrete working directory was supplied.
struct GhosttyApplicationIpcRequest final {
    QString applicationId;
    QString objectPath;
    QString actionName;
    std::optional<QStringList> stringArrayParameter;

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

// Receiver-side projection of the string-array parameter accepted by
// new-window-command. Unknown entries are deliberately absent from this DTO.
struct GhosttyNewWindowTransportOverrides final {
    std::optional<TerminalCommand> command;
    std::optional<QString> workingDirectory;
    std::optional<QString> titleOverride;

    bool operator==(const GhosttyNewWindowTransportOverrides &) const = default;
};

[[nodiscard]] std::expected<GhosttyNewWindowTransportOverrides,
                            GhosttyApplicationIpcError>
decodeGhosttyNewWindowArguments(const QStringList &arguments);
