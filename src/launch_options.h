#pragma once

#include "ghostty_config_snapshot.h"
#include "terminal_session_options.h"

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <expected>

enum class ConfirmCloseMode {
    Never,
    RunningProcesses,
    Always,
};

// Mirrors Ghostty's link-previews configuration without leaking its canonical
// text representation into the terminal UI.
enum class LinkPreviewMode {
    Never,
    Always,
    Osc8,
};

enum class MiddleClickAction {
    PrimaryPaste,
    Ignore,
};

struct LaunchOptions {
    QString workingDirectory;
    QString fontFamily;
    double fontSize = 12.0;
    // These bits distinguish parser defaults from an explicit command-line
    // override. Ghostty configuration must not replace explicit CLI fonts.
    bool fontFamilyExplicit = false;
    bool fontSizeExplicit = false;
    TerminalAppearance appearance;
    ScrollbackLimit scrollbackLimit;
    bool scrollbackLimitExplicit = false;
    ConfirmCloseMode confirmCloseMode = ConfirmCloseMode::RunningProcesses;
    TerminalSelectionClipboardOptions selectionClipboard;
    TerminalClipboardPasteOptions clipboardPaste;
    // Middle-click is a GUI input policy. It intentionally stays outside the
    // worker-owned terminal session options.
    MiddleClickAction middleClickAction = MiddleClickAction::PrimaryPaste;
    // Enables Ghostty's built-in URL matcher. OSC 8 hyperlinks remain
    // available independently of this setting.
    bool linkUrl = true;
    // Controls whether matched link destinations are shown before activation.
    // Ghostty defaults to previews for both regex and OSC 8 links.
    LinkPreviewMode linkPreviews = LinkPreviewMode::Always;
    // The helper publishes Ghostty's finalized structured sets. The explicit
    // bit distinguishes an intentionally empty root from the built-in
    // emergency shortcuts used when configuration is unavailable.
    GhosttyKeybindConfig keybindConfig;
    // Text fallback used by config-disabled builds and direct compatibility
    // tests. A structured snapshot clears this list before reaching a pane.
    QStringList keybindings;
    bool keybindingsConfigured = false;
    bool hold = false;
    bool showHelp = false;
    bool showVersion = false;
    QStringList program;
};

// Explicitly project the broad application/pane configuration onto the
// smaller value types allowed to cross the session-thread boundary.
TerminalSessionLaunchOptions toTerminalSessionLaunchOptions(
    const LaunchOptions &options);
TerminalSessionRuntimeOptions toTerminalSessionRuntimeOptions(
    const LaunchOptions &options);

// Overlay the compatibility slice of an available Ghostty snapshot onto a
// launch request. The function has no side effects and ignores malformed or
// unavailable values. A byte-valued Ghostty scrollback-limit is marked as
// Bytes and passed unchanged to the pinned libghostty max_scrollback field:
// despite that C field's legacy "lines" wording, the pinned implementation
// forwards it to PageList's byte-sized logical history cap. The legacy
// --scrollback-lines option is converted through the documented
// column-aware capacity estimate in scrollbackLimitInBytes().
LaunchOptions applyGhosttyConfigSnapshot(const LaunchOptions &base,
                                         const GhosttyConfigSnapshot &snapshot);

// A live child and an active process are deliberately separate. For an
// interactive shell, the shell remains live at its prompt while only a
// foreground job is active. Explicitly launched programs are active for their
// entire lifetime.
bool shouldConfirmClose(ConfirmCloseMode mode, bool childIsRunning,
                        bool hasActiveProcess);

// The first argument is expected to be the application name, as it is in
// QCoreApplication::arguments().
[[nodiscard]] std::expected<LaunchOptions, QString> parseLaunchOptions(
    const QStringList &arguments);
