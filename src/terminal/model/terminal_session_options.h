#pragma once

#include "session/ghostty_shell_integration_mode.h"

#include "terminal/model/terminal_path.h"

#include "desktop/linux_cgroup_config.h"
#include "terminal/model/terminal_appearance.h"
#include "terminal/model/terminal_clipboard_codepoint_map.h"
#include "terminal/model/terminal_color_scheme.h"
#include "terminal/model/terminal_command.h"
#include "terminal/model/terminal_initial_input.h"

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <limits>
#include <optional>

struct ScrollbackLimits {
    // Ghostty applies both axes simultaneously and evicts history when either
    // limit is reached. A missing value means unlimited; zero remains a real
    // limit and must not be collapsed into the missing state.
    std::optional<quint64> bytes = 50'000'000;
    std::optional<quint64> lines;

    bool operator==(const ScrollbackLimits &) const = default;
};

struct TerminalEnvironmentEntry {
    QByteArray key;
    QByteArray value;

    bool operator==(const TerminalEnvironmentEntry &) const = default;
};

using TerminalEnvironment = QVector<TerminalEnvironmentEntry>;

enum class GraphemeWidthMethod : quint8 {
    Legacy,
    Unicode,
};

// This mirrors the pinned Ghostty config bitset. It is launch policy: shell
// scripts consume the serialized feature set from the child environment.
struct GhosttyShellIntegrationFeatures {
    bool cursor = true;
    bool sudo = false;
    bool title = true;
    bool sshEnvironment = false;
    bool sshTerminfo = false;
    bool path = true;

    bool operator==(const GhosttyShellIntegrationFeatures &) const = default;
};

enum class TerminalCopyOnSelectMode : quint8 {
    Disabled,
    Primary,
    PrimaryAndClipboard,
};

enum class TerminalClipboardDestination : quint8 {
    Standard,
    Primary,
    PrimaryAndStandard,
};

// Frontend-owned scheduling for construction of a terminal session. Deferred
// mode is used only while an initially non-windowed Qt surface waits for its
// compositor-assigned viewport; it is not part of the worker launch payload.
enum class TerminalSessionStartMode : quint8 {
    Immediate,
    Deferred,
};

struct TerminalSelectionClipboardOptions {
    bool trimTrailingSpaces = true;
    TerminalCopyOnSelectMode copyOnSelect = TerminalCopyOnSelectMode::Primary;
    bool clearOnTyping = true;
    bool clearOnCopy = false;
    // Entries retain configuration order because the last overlapping range
    // wins. Ghostty expects few entries and intentionally uses a linear scan.
    TerminalClipboardCodepointMap codepointMap;

    bool operator==(const TerminalSelectionClipboardOptions &) const = default;
};

// Paste safety is worker-owned because the decision depends on the terminal's
// live bracketed-paste mode as well as live-reloaded configuration.
struct TerminalClipboardPasteOptions {
    bool protection = true;
    bool bracketedSafe = true;

    bool operator==(const TerminalClipboardPasteOptions &) const = default;
};

struct TerminalScrollToBottomOptions {
    bool keystroke = true;
    bool output = false;

    bool operator==(const TerminalScrollToBottomOptions &) const = default;
};

enum class TerminalClipboardAccess : quint8 {
    Ask,
    Allow,
    Deny,
};

enum class RightClickAction {
    ContextMenu,
    Paste,
    Copy,
    CopyOrPaste,
    Ignore,
};

// Value-only settings that an existing terminal session can apply. Keep this
// boundary limited to state that SessionWorker actually owns at runtime.
struct TerminalSessionRuntimeOptions {
    TerminalAppearance appearance;
    TerminalColorScheme colorScheme = TerminalColorScheme::Light;
    // Raw protocol response for ENQ. QByteArray preserves embedded NUL and
    // non-UTF-8 bytes across queued runtime reloads.
    QByteArray enquiryResponse;
    TerminalSelectionClipboardOptions selectionClipboard;
    QVector<quint32> selectionWordChars;
    quint32 clickRepeatIntervalMilliseconds = 500;
    // Terminal-originated clipboard writes are resolved by the worker using
    // the newest policy before a normalized request crosses to the GUI.
    TerminalClipboardAccess clipboardWrite = TerminalClipboardAccess::Allow;
    TerminalClipboardPasteOptions clipboardPaste;
    RightClickAction rightClickAction = RightClickAction::ContextMenu;
    bool linkUrl = true;
    bool linkOsc8 = true;
    // These lib-vt policies are fixed at terminal construction upstream.
    GraphemeWidthMethod graphemeWidthMethod = GraphemeWidthMethod::Unicode;
    bool titleReport = false;
    // KAM remains terminal-owned. This live frontend policy decides whether
    // ANSI mode 2 is permitted to suppress ordinary keyboard and IME input.
    bool vtKamAllowed = false;
    // Existing compressed pages remain valid when this live policy is
    // disabled; the worker gates only future compression scheduling.
    bool scrollbackCompression = true;
    quint32 kittyImageStorageLimitBytes = 320'000'000;
    TerminalScrollToBottomOptions scrollToBottom;
    // Linux classifies a nonzero child exit observed at or before this live
    // threshold as abnormal. Zero remains meaningful for a sub-millisecond
    // failure and therefore is not a disabled sentinel.
    quint32 abnormalCommandExitRuntimeMilliseconds = 250;
    // A child that exits while this live policy is enabled remains readable
    // until a terminal-encoded key dismisses it.
    bool waitAfterCommand = false;

    bool operator==(const TerminalSessionRuntimeOptions &) const = default;
};

// Complete value needed to construct libghostty and the PTY at the same
// geometry. The optional launch-time instance is consumed before either is
// created; later resizes use this representation after the GUI has laid out
// the pane.
struct TerminalSessionGeometry {
    struct Padding {
        int top = 0;
        int right = 0;
        int bottom = 0;
        int left = 0;

        bool operator==(const Padding &) const = default;
    };

    int columns = 80;
    int rows = 24;
    int cellWidthPixels = 8;
    int cellHeightPixels = 16;
    // Full pane surface, including padding. The PTY pixel extent is derived
    // from this and padding so the two representations cannot disagree.
    int surfaceWidthPixels = 640;
    int surfaceHeightPixels = 384;
    Padding padding;

    bool operator==(const TerminalSessionGeometry &) const = default;

    [[nodiscard]] int terminalWidthPixels() const noexcept
    {
        const qint64 width = static_cast<qint64>(surfaceWidthPixels)
            - padding.left - padding.right;
        return static_cast<int>(
            std::clamp<qint64>(width, 0, std::numeric_limits<int>::max()));
    }

    [[nodiscard]] int terminalHeightPixels() const noexcept
    {
        const qint64 height = static_cast<qint64>(surfaceHeightPixels)
            - padding.top - padding.bottom;
        return static_cast<int>(
            std::clamp<qint64>(height, 0, std::numeric_limits<int>::max()));
    }
};

inline TerminalSessionGeometry
normalizedTerminalSessionGeometry(TerminalSessionGeometry geometry) noexcept
{
    constexpr int maximumCells =
        static_cast<int>(std::numeric_limits<quint16>::max());
    geometry.columns = std::clamp(geometry.columns, 1, maximumCells);
    geometry.rows = std::clamp(geometry.rows, 1, maximumCells);
    geometry.cellWidthPixels = std::max(geometry.cellWidthPixels, 1);
    geometry.cellHeightPixels = std::max(geometry.cellHeightPixels, 1);
    geometry.surfaceWidthPixels = std::max(geometry.surfaceWidthPixels, 1);
    geometry.surfaceHeightPixels = std::max(geometry.surfaceHeightPixels, 1);
    geometry.padding.top = std::max(geometry.padding.top, 0);
    geometry.padding.right = std::max(geometry.padding.right, 0);
    geometry.padding.bottom = std::max(geometry.padding.bottom, 0);
    geometry.padding.left = std::max(geometry.padding.left, 0);
    return geometry;
}

// One-time process and terminal construction settings. Runtime state is
// composed here so initialization and reload use the same representation.
struct TerminalSessionLaunchOptions {
    QByteArray term = QByteArrayLiteral("xterm-ghostty");
    // Ghostty's finalized raw environment overrides replace inherited and
    // frontend-injected values immediately before exec. A concrete
    // working-directory-derived PWD is the pinned runtime's later exception.
    TerminalEnvironment environment;
    GhosttyShellIntegrationMode shellIntegration =
        GhosttyShellIntegrationMode::None;
    GhosttyShellIntegrationFeatures shellIntegrationFeatures;
    // Generic worker callers and config-disabled builds have no pinned
    // launcher helper. A finalized Ghostty snapshot sets this bit even for
    // `shell-integration = none`, because feature serialization still occurs.
    bool shellIntegrationAvailable = false;
    // Linux resource isolation is fixed when this pane is constructed. The
    // process role is resolved once during application startup; config reloads
    // can change the policy for future panes without changing that role.
    LinuxCgroupConfig linuxCgroup;
    bool processUsesSingleInstance = false;
    TerminalPath workingDirectory;
    bool inheritWorkingDirectory = false;
    // GUI-owned initial base-title policy. TerminalController applies live
    // reloads directly so title-only changes never wake SessionWorker.
    std::optional<QString> configuredTitle;
    // Shared Ghostty command for this pane. The process one-shot coordinator
    // may replace it with initial-command immediately before worker creation.
    std::optional<TerminalCommand> command;
    // The frontend positional command remains a separate direct argv until
    // its process-wide first-session lease is resolved. When present it takes
    // precedence over `command`.
    QStringList program;
    // A remote new-window command is scoped to that window's first surface,
    // but the surface must still resolve the process-wide initial-session
    // lease. TerminalController applies this after lease arbitration so the
    // global opportunity is consumed without replacing the remote command.
    std::optional<TerminalCommand> firstSessionCommandOverride;
    // Future-session input is resolved atomically before the child exists.
    // Raw entries preserve arbitrary bytes; path entries are read relative to
    // the frontend process working directory, matching pinned Ghostty.
    QVector<TerminalInitialInput> initialInput;
    ScrollbackLimits scrollbackLimits;
    // Initial-only frontend override. Shared wait-after-command remains in the
    // live runtime options so a reload can affect an existing child.
    bool hold = false;
    // Frontend window dimensions never enter the reusable LaunchOptions
    // projection. The workspace supplies this one-shot value only for its
    // first pane so tabs and splits cannot inherit window-level geometry.
    std::optional<TerminalSessionGeometry> initialGeometry;
    TerminalSessionRuntimeOptions runtime;

    bool operator==(const TerminalSessionLaunchOptions &) const = default;
};

Q_DECLARE_METATYPE(TerminalSessionLaunchOptions)
Q_DECLARE_METATYPE(TerminalSessionRuntimeOptions)
Q_DECLARE_METATYPE(TerminalSessionGeometry)
Q_DECLARE_METATYPE(TerminalClipboardDestination)
