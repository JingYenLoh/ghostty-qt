#pragma once

#include "ghostty_config_snapshot.h"

#include <QColor>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QtGlobal>

class QCoreApplication;

enum class ConfirmCloseMode {
    Never,
    RunningProcesses,
    Always,
};

enum class ScrollbackLimitUnit {
    Lines,
    Bytes,
};

struct ScrollbackLimit {
    quint64 value = 10'000;
    ScrollbackLimitUnit unit = ScrollbackLimitUnit::Lines;

    bool operator==(const ScrollbackLimit &) const = default;
};

struct LaunchOptions {
    QString workingDirectory;
    QString fontFamily;
    double fontSize = 12.0;
    // These bits distinguish parser defaults from an explicit command-line
    // override. Ghostty configuration must not replace explicit CLI fonts.
    bool fontFamilyExplicit = false;
    bool fontSizeExplicit = false;
    // Carries pane-local zoom state when a split inherits its parent's font.
    bool fontSizeManuallyAdjusted = false;
    QColor foregroundColor = QColor(QStringLiteral("#d8dee9"));
    QColor backgroundColor = QColor(QStringLiteral("#1e222a"));
    // Invalid means "follow the foreground", matching libghostty's default.
    QColor cursorColor;
    ScrollbackLimit scrollbackLimit;
    bool scrollbackLimitExplicit = false;
    ConfirmCloseMode confirmCloseMode = ConfirmCloseMode::RunningProcesses;
    // The helper publishes Ghostty's flattened effective set. The explicit
    // bit distinguishes an intentionally empty set from the built-in
    // emergency shortcuts used when configuration is unavailable.
    QStringList keybindings;
    bool keybindingsConfigured = false;
    bool hold = false;
    bool showHelp = false;
    bool showVersion = false;
    QStringList program;
};

// Overlay the compatibility slice of an available Ghostty snapshot onto a
// launch request. The function has no side effects and ignores malformed or
// unavailable values. A byte-valued Ghostty scrollback-limit is marked as
// Bytes and passed unchanged to the pinned libghostty max_scrollback field:
// despite that C field's legacy "lines" wording, the pinned implementation
// forwards it to PageList's byte-sized logical history cap. The legacy
// The legacy --scrollback-lines option is converted through the documented
// column-aware capacity estimate in scrollbackLimitInBytes().
LaunchOptions applyGhosttyConfigSnapshot(const LaunchOptions &base,
                                         const GhosttyConfigSnapshot &snapshot);

// libghostty's max_scrollback is byte-valued. Preserve Ghostty config bytes
// exactly; convert the legacy line CLI using a conservative storage estimate
// of max(256, columns * 16) bytes per row, with saturating arithmetic.
quint64 scrollbackLimitInBytes(ScrollbackLimit limit, int columns);

// A live child and an active process are deliberately separate. For an
// interactive shell, the shell remains live at its prompt while only a
// foreground job is active. Explicitly launched programs are active for their
// entire lifetime.
bool shouldConfirmClose(ConfirmCloseMode mode, bool childIsRunning,
                        bool hasActiveProcess);

// The first argument is expected to be the application name, as it is in
// QCoreApplication::arguments(). This overload keeps option parsing easy to
// exercise without constructing additional application objects in tests.
bool parseLaunchOptions(const QStringList &arguments, LaunchOptions *options,
                        QString *errorMessage = nullptr);

bool parseLaunchOptions(QCoreApplication &application, LaunchOptions *options,
                        QString *errorMessage = nullptr);

Q_DECLARE_METATYPE(LaunchOptions)
