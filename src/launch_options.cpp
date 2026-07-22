#include "launch_options.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QLocale>

#include <cmath>
#include <utility>
#include <variant>

namespace {

constexpr int kMaximumScrollbackLines = 10'000'000;

template<typename... Visitor>
struct Overloaded : Visitor... {
    using Visitor::operator()...;
};

std::optional<bool> parseGhosttyBoolean(QStringView value)
{
    if (value == QLatin1StringView("true")
        || value == QLatin1StringView("1")
        || value == QLatin1StringView("t")
        || value == QLatin1StringView("T")) {
        return true;
    }
    if (value == QLatin1StringView("false")
        || value == QLatin1StringView("0")
        || value == QLatin1StringView("f")
        || value == QLatin1StringView("F")) {
        return false;
    }
    return std::nullopt;
}

TerminalColorValue toTerminalColor(const GhosttyTerminalColor &configured)
{
    return std::visit(
        Overloaded{
            [](const QColor &color) {
                return TerminalColorValue::fromColor(color);
            },
            [](GhosttyCellRelativeColor relative) {
                switch (relative) {
                case GhosttyCellRelativeColor::Foreground:
                    return TerminalColorValue{
                        .kind = TerminalColorKind::CellForeground,
                        .color = {},
                    };
                case GhosttyCellRelativeColor::Background:
                    return TerminalColorValue{
                        .kind = TerminalColorKind::CellBackground,
                        .color = {},
                    };
                }
                std::unreachable();
            },
        },
        configured);
}

TerminalColorValue toTerminalColor(
    const std::optional<GhosttyTerminalColor> &configured)
{
    return configured ? toTerminalColor(*configured) : TerminalColorValue{};
}

TerminalBoldColor toTerminalBoldColor(
    const std::optional<GhosttyBoldColor> &configured)
{
    if (!configured) {
        return {};
    }
    return std::visit(
        Overloaded{
            [](const QColor &color) {
                return TerminalBoldColor{
                    .kind = TerminalBoldColorKind::Color,
                    .color = color,
                };
            },
            [](GhosttyBoldBrightness brightness) {
                switch (brightness) {
                case GhosttyBoldBrightness::Bright:
                    return TerminalBoldColor{
                        .kind = TerminalBoldColorKind::Bright,
                        .color = {},
                    };
                }
                std::unreachable();
            },
        },
        *configured);
}

TerminalAppearance toTerminalAppearance(
    const GhosttyAppearanceConfig &configured)
{
    TerminalAppearance result{
        .foregroundColor = configured.foreground,
        .backgroundColor = configured.background,
        .palette = {},
        .selectionForeground = toTerminalColor(
            configured.selectionForeground),
        .selectionBackground = toTerminalColor(
            configured.selectionBackground),
        .searchForeground = toTerminalColor(configured.searchForeground),
        .searchBackground = toTerminalColor(configured.searchBackground),
        .searchSelectedForeground = toTerminalColor(
            configured.searchSelectedForeground),
        .searchSelectedBackground = toTerminalColor(
            configured.searchSelectedBackground),
        .cursorColor = toTerminalColor(configured.cursorColor),
        .cursorStyle = configured.cursorStyle,
        .cursorBlink = configured.cursorBlink,
        .cursorOpacity = configured.cursorOpacity,
        .cursorTextColor = toTerminalColor(configured.cursorText),
        .boldColor = toTerminalBoldColor(configured.boldColor),
        .faintOpacity = configured.faintOpacity,
    };
    result.palette.reserve(static_cast<qsizetype>(configured.palette.size()));
    for (const QColor &color : configured.palette) {
        result.palette.append(color);
    }
    return result;
}

void enforceExplicitCommandLifetime(const LaunchOptions &base,
                                    LaunchOptions &destination)
{
    if (base.program.isEmpty()) return;
    destination.quitAfterLastWindowClosed = true;
    destination.quitAfterLastWindowClosedDelay.reset();
}

} // namespace

TerminalSessionRuntimeOptions toTerminalSessionRuntimeOptions(
    const LaunchOptions &options)
{
    return {
        .appearance = options.appearance,
        .selectionClipboard = options.selectionClipboard,
        .clipboardPaste = options.clipboardPaste,
        .linkUrl = options.linkUrl,
    };
}

LaunchOptions withoutInitialCommand(LaunchOptions options)
{
    options.program.clear();
    options.hold = false;
    return options;
}

TerminalSessionLaunchOptions toTerminalSessionLaunchOptions(
    const LaunchOptions &options)
{
    return {
        .workingDirectory = options.workingDirectory,
        .inheritWorkingDirectory = options.inheritWorkingDirectory,
        .program = options.program,
        .scrollbackLimit = options.scrollbackLimit,
        .hold = options.hold,
        .initialGeometry = std::nullopt,
        .runtime = toTerminalSessionRuntimeOptions(options),
    };
}

LaunchOptions applyGhosttyConfigSnapshot(const LaunchOptions &base,
                                         const GhosttyConfigSnapshot &snapshot)
{
    LaunchOptions result = base;
    const GhosttyConfigValues &config = snapshot.values;

    if (!base.workingDirectoryExplicit) {
        result.inheritWorkingDirectory = !config.workingDirectoryPath;
        if (config.workingDirectoryPath) {
            // The helper has already finalized home and tilde expansion.
            // Preserve the remaining path byte-for-byte at the QString
            // boundary: lexical cleanup changes `symlink/../...` lookup.
            result.workingDirectory = *config.workingDirectoryPath;
        }
    }
    if (!base.fontFamilyExplicit && !config.fontFamilies.isEmpty()) {
        result.fontFamily = config.fontFamilies.constFirst();
    }
    if (!base.fontSizeExplicit && std::isfinite(config.fontSize) &&
        config.fontSize > 0.0) {
        result.fontSize = config.fontSize;
    }

    result.appearance = toTerminalAppearance(config.appearance);
    result.splitAppearance = config.splitAppearance;
    result.splitInheritWorkingDirectory = config.splitInheritWorkingDirectory;
    result.splitPreserveZoomNavigation = config.splitPreserveZoom;
    result.tabInheritWorkingDirectory = config.tabInheritWorkingDirectory;
    result.windowInheritWorkingDirectory = config.windowInheritWorkingDirectory;
    result.windowInheritFontSize = config.windowInheritFontSize;
    result.windowNewTabPosition = config.windowNewTabPosition;
    result.windowShowTabBar = config.windowShowTabBar;
    result.windowWidth = config.windowWidth;
    result.windowHeight = config.windowHeight;
    result.resizeOverlay = config.resizeOverlay;
    result.maximize = config.maximize;
    // Ghostty documents every non-native mode as equivalent to native
    // fullscreen away from macOS. This frontend is Linux-only.
    result.fullscreen = config.fullscreen != GhosttyFullscreenMode::Disabled;

    result.quitAfterLastWindowClosed = config.quitAfterLastWindowClosed;
    result.quitAfterLastWindowClosedDelay =
        config.quitAfterLastWindowClosedDelay;
    if (!base.initialWindowExplicit) {
        result.initialWindow = config.initialWindow;
    }
    if (!base.singleInstanceModeExplicit) {
        result.singleInstanceMode = config.singleInstanceMode;
    }

    if (!base.scrollbackLimitExplicit) {
        result.scrollbackLimit = {
            .value = config.scrollbackLimitBytes,
            .unit = ScrollbackLimitUnit::Bytes,
        };
    }
    result.confirmCloseMode = config.confirmCloseMode;
    result.selectionClipboard = config.selectionClipboard;
    result.clipboardPaste = config.clipboardPaste;
    result.middleClickAction = config.middleClickAction;
    result.mouseReporting = config.mouseReporting;
    result.linkUrl = config.linkUrl;
    result.linkPreviews = config.linkPreviews;
    result.keybindSource =
        GhosttyKeybindSource::structured(snapshot.keybindings);

    // Match Ghostty's `-e` contract: an explicitly supplied command always
    // exits with its final window and never inherits a lingering delay.
    enforceExplicitCommandLifetime(base, result);

    return result;
}

bool shouldUseSingleInstance(const LaunchOptions &options,
                             QByteArrayView termProgram)
{
    if (options.hasUnforwardedLaunchPayload) return false;
    switch (options.singleInstanceMode) {
    case SingleInstanceMode::Enabled:
        return true;
    case SingleInstanceMode::Disabled:
        return false;
    case SingleInstanceMode::Detect:
        return termProgram.isEmpty();
    }
    return false;
}

bool shouldConfirmClose(ConfirmCloseMode mode, bool childIsRunning,
                        bool hasActiveProcess)
{
    // Ghostty never confirms an already-exited surface, including in `always`
    // mode. `true` protects active foreground work but permits an interactive
    // shell sitting at its prompt to close without an extra dialog.
    if (!childIsRunning) {
        return false;
    }
    switch (mode) {
    case ConfirmCloseMode::Never:
        return false;
    case ConfirmCloseMode::RunningProcesses:
        return hasActiveProcess;
    case ConfirmCloseMode::Always:
        return true;
    }
    return true;
}

std::expected<LaunchOptions, QString> parseLaunchOptions(
    const QStringList &arguments)
{
    if (arguments.isEmpty()) {
        return std::unexpected(
            QStringLiteral("The argument list must include the application name."));
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("A Qt terminal emulator powered by libghostty."));

    const QCommandLineOption workingDirectoryOption(
        QStringLiteral("working-directory"),
        QStringLiteral("Start the command in <directory>."),
        QStringLiteral("directory"));
    const QCommandLineOption fontFamilyOption(
        QStringLiteral("font-family"),
        QStringLiteral("Use <family> for terminal text."),
        QStringLiteral("family"));
    const QCommandLineOption fontSizeOption(
        QStringLiteral("font-size"),
        QStringLiteral("Use a font size of <points>."),
        QStringLiteral("points"));
    const QCommandLineOption scrollbackLinesOption(
        QStringLiteral("scrollback-lines"),
        QStringLiteral("Estimate scrollback capacity for <lines> rows."),
        QStringLiteral("lines"));
    const QCommandLineOption holdOption(
        QStringLiteral("hold"),
        QStringLiteral("Keep the terminal open after the child process exits."));
    const QCommandLineOption singleInstanceOption(
        QStringLiteral("gtk-single-instance"),
        QStringLiteral("Use false, true, or detect process uniqueness."),
        QStringLiteral("mode"));
    const QCommandLineOption initialWindowOption(
        QStringLiteral("initial-window"),
        QStringLiteral("Request an initial window (true or false)."),
        QStringLiteral("boolean"));
    const QCommandLineOption helpOption(
        {QStringLiteral("h"), QStringLiteral("help")},
        QStringLiteral("Show command-line help."));
    const QCommandLineOption versionOption(
        {QStringLiteral("v"), QStringLiteral("version")},
        QStringLiteral("Show version information."));

    parser.addOptions({workingDirectoryOption, fontFamilyOption, fontSizeOption,
                       scrollbackLinesOption, holdOption, singleInstanceOption,
                       initialWindowOption, helpOption, versionOption});
    parser.addPositionalArgument(
        QStringLiteral("program"),
        QStringLiteral("Program and arguments to execute after --."),
        QStringLiteral("[program [arguments...]]"));

    if (!parser.parse(arguments)) {
        return std::unexpected(parser.errorText());
    }

    LaunchOptions parsed;
    parsed.workingDirectory = QDir::currentPath();
    parsed.inheritWorkingDirectory = true;

    if (parser.isSet(workingDirectoryOption)) {
        const QString directory = parser.value(workingDirectoryOption);
        const QFileInfo directoryInfo(directory);
        if (!directoryInfo.exists() || !directoryInfo.isDir()) {
            return std::unexpected(
                QStringLiteral("Working directory does not exist or is not a directory: %1")
                    .arg(directory));
        }
        parsed.workingDirectory = directory;
        parsed.inheritWorkingDirectory = false;
        parsed.workingDirectoryExplicit = true;
    }

    if (parser.isSet(fontFamilyOption)) {
        parsed.fontFamily = parser.value(fontFamilyOption);
        parsed.fontFamilyExplicit = true;
    }

    if (parser.isSet(fontSizeOption)) {
        const QString value = parser.value(fontSizeOption);
        bool ok = false;
        const double fontSize = QLocale::c().toDouble(value, &ok);
        if (!ok || !std::isfinite(fontSize) || fontSize <= 0.0) {
            return std::unexpected(
                QStringLiteral("Invalid font size '%1': expected a finite number greater than 0.")
                    .arg(value));
        }
        parsed.fontSize = fontSize;
        parsed.fontSizeExplicit = true;
    }

    if (parser.isSet(scrollbackLinesOption)) {
        const QString value = parser.value(scrollbackLinesOption);
        bool ok = false;
        const qlonglong scrollbackLines = value.toLongLong(&ok);
        if (!ok || scrollbackLines < 0 || scrollbackLines > kMaximumScrollbackLines) {
            return std::unexpected(
                QStringLiteral("Invalid scrollback line count '%1': expected an integer from 0 to %2.")
                    .arg(value)
                    .arg(kMaximumScrollbackLines));
        }
        parsed.scrollbackLimit = {
            .value = static_cast<quint64>(scrollbackLines),
            .unit = ScrollbackLimitUnit::Lines,
        };
        parsed.scrollbackLimitExplicit = true;
    }

    parsed.hold = parser.isSet(holdOption);
    parsed.showHelp = parser.isSet(helpOption);
    parsed.showVersion = parser.isSet(versionOption);
    parsed.program = parser.positionalArguments();
    parsed.hasUnforwardedLaunchPayload = parsed.workingDirectoryExplicit
        || parsed.fontFamilyExplicit || parsed.fontSizeExplicit
        || parsed.scrollbackLimitExplicit || parsed.hold
        || !parsed.program.isEmpty();

    if (parser.isSet(singleInstanceOption)) {
        const QString mode = parser.value(singleInstanceOption);
        if (mode == QStringLiteral("true")) {
            parsed.singleInstanceMode = SingleInstanceMode::Enabled;
        } else if (mode == QStringLiteral("false")) {
            parsed.singleInstanceMode = SingleInstanceMode::Disabled;
        } else if (mode == QStringLiteral("detect")) {
            parsed.singleInstanceMode = SingleInstanceMode::Detect;
        } else {
            return std::unexpected(QStringLiteral(
                "Invalid gtk-single-instance value '%1': expected "
                "false, true, or detect.")
                    .arg(mode));
        }
        parsed.singleInstanceModeExplicit = true;
    }

    if (parser.isSet(initialWindowOption)) {
        const QString value = parser.value(initialWindowOption);
        const std::optional<bool> initialWindow =
            parseGhosttyBoolean(value);
        if (!initialWindow.has_value()) {
            return std::unexpected(QStringLiteral(
                "Invalid initial-window value '%1': expected true or false.")
                    .arg(value));
        }
        parsed.initialWindow = *initialWindow;
        parsed.initialWindowExplicit = true;
    }

    return parsed;
}
