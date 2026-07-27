#pragma once

#include "terminal_command.h"
#include "terminal_session_options.h"

#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>

#include <expected>
#include <optional>

enum class GhosttyIntegratedShell {
    Bash,
    Elvish,
    Fish,
    Nushell,
    Zsh,
};

// One complete launch-time request for Ghostty's pinned shell setup. The
// environment is the inherited/frontend-injected environment before Ghostty's
// configured env overrides and concrete PWD are applied.
struct GhosttyShellIntegrationRequest {
    TerminalCommand command;
    TerminalEnvironment environment;
    GhosttyShellIntegrationMode mode = GhosttyShellIntegrationMode::Detect;
    GhosttyShellIntegrationFeatures features;
    bool cursorBlink = true;
    // Local-filesystem bytes for the resource root containing the
    // shell-integration directory. Empty represents an unavailable root and
    // still permits GHOSTTY_SHELL_FEATURES setup.
    QByteArray resourceDirectory;

    bool operator==(const GhosttyShellIntegrationRequest &) const = default;
};

struct GhosttyShellIntegrationResult {
    TerminalCommand command;
    TerminalEnvironment environment;
    std::optional<GhosttyIntegratedShell> shell;

    bool operator==(const GhosttyShellIntegrationResult &) const = default;
};

struct GhosttyShellIntegrationProcessOptions {
    QString helperPath;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    int timeoutMilliseconds = 2'000;
};

// These pure protocol functions are public for strict boundary tests and fake
// helper fixtures. Byte-valued fields use canonical base64 inside JSON.
[[nodiscard]] std::expected<QByteArray, QString>
serializeGhosttyShellIntegrationRequest(
    const GhosttyShellIntegrationRequest &request);
[[nodiscard]] std::expected<GhosttyShellIntegrationResult, QString>
parseGhosttyShellIntegrationResult(const QByteArray &json);

// Invoke the isolated revision-pinned helper and prepare one child launch.
// Failure is explicit so the worker can warn and safely retain its original
// command/environment rather than preventing terminal startup.
[[nodiscard]] std::expected<GhosttyShellIntegrationResult, QString>
prepareGhosttyShellIntegration(
    const GhosttyShellIntegrationProcessOptions &options,
    const GhosttyShellIntegrationRequest &request);

// Resolve a resource root containing the shell-integration leaf. The pure
// overload enables relocation tests; the runtime overload reads
// GHOSTTY_QT_SHELL_INTEGRATION_RESOURCES and the application directory.
[[nodiscard]] std::expected<QString, QString>
resolveShellIntegrationResourceDirectory(
    const QString &applicationDirectory,
    const std::optional<QString> &overrideDirectory);
[[nodiscard]] std::expected<QString, QString>
resolveRuntimeShellIntegrationResourceDirectory();
