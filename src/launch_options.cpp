#include "launch_options.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <pwd.h>
#include <span>
#include <string_view>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr int kMaximumScrollbackLines = 10'000'000;

QString frontendShaderPath(QString path, const QString &configPath)
{
    if (path == QLatin1StringView("~")) {
        path = QDir::homePath();
    } else if (path.startsWith(QLatin1StringView("~/"))) {
        path = QDir::home().filePath(path.sliced(2));
    }
    if (QDir::isAbsolutePath(path)) return QDir::cleanPath(path);
    const QDir base = configPath.isEmpty()
        ? QDir::current()
        : QFileInfo(configPath).absoluteDir();
    return QDir::cleanPath(base.absoluteFilePath(path));
}

QByteArrayView trimAsciiWhitespace(QByteArrayView value)
{
    constexpr std::string_view whitespace = " \t\n\r\v\f";
    qsizetype begin = 0;
    while (begin < value.size() && whitespace.contains(value.at(begin))) {
        ++begin;
    }
    qsizetype end = value.size();
    while (end > begin && whitespace.contains(value.at(end - 1))) {
        --end;
    }
    return value.sliced(begin, end - begin);
}

std::optional<QByteArray> passwdHomeDirectory()
{
    const long suggested = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> storage(
        static_cast<std::size_t>(std::clamp<long>(suggested, 1'024, 1L << 20)));
    passwd entry{};
    passwd *result = nullptr;
    const int error = ::getpwuid_r(::getuid(), &entry, storage.data(),
                                   storage.size(), &result);
    if (error != 0 || result == nullptr || result->pw_dir == nullptr
        || result->pw_dir[0] == '\0') {
        return std::nullopt;
    }
    return QByteArray(result->pw_dir);
}

std::optional<QByteArray> shellHomeDirectory()
{
    QProcess process;
    process.setProgram(QStringLiteral("/bin/sh"));
    process.setArguments({QStringLiteral("-c"), QStringLiteral("cd && pwd")});
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(QIODevice::ReadOnly);
    if (!process.waitForStarted() || !process.waitForFinished()
        || process.exitStatus() != QProcess::NormalExit
        || process.exitCode() != 0) {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished();
        }
        return std::nullopt;
    }

    const QByteArray output = process.readAllStandardOutput();
    const QByteArrayView trimmed = trimAsciiWhitespace(output);
    if (trimmed.isEmpty()) return std::nullopt;
    return QByteArray(trimmed.data(), trimmed.size());
}

std::optional<QByteArray> expansionHomeDirectory()
{
    QByteArray home = qgetenv("HOME");
    if (!home.isEmpty()) return home;
    if (auto passwd = passwdHomeDirectory()) return passwd;
    return shellHomeDirectory();
}

std::expected<void, QString> applyWorkingDirectoryValue(LaunchOptions &options,
                                                        QByteArrayView encoded)
{
    encoded = trimAsciiWhitespace(encoded);
    if (encoded.isEmpty()) {
        return std::unexpected(
            QStringLiteral("Working directory requires a value."));
    }
    if (encoded.size() >= 2 && encoded.front() == '"'
        && encoded.back() == '"') {
        encoded = encoded.sliced(1, encoded.size() - 2);
    }

    options.workingDirectoryExplicit = true;
    if (encoded == QByteArrayView("inherit")) {
        options.inheritWorkingDirectory = true;
        return {};
    }
    if (encoded == QByteArrayView("home")) {
        const std::optional<QByteArray> home = passwdHomeDirectory();
        if (!home.has_value()) {
            options.inheritWorkingDirectory = true;
            return {};
        }
        options.workingDirectory = *home;
        options.inheritWorkingDirectory = false;
        return {};
    }

    QByteArray path(encoded.data(), encoded.size());
    if (path.startsWith("~/")) {
        if (const std::optional<QByteArray> home = expansionHomeDirectory()) {
            path.replace(0, 1, *home);
        }
    }
    options.workingDirectory = std::move(path);
    options.inheritWorkingDirectory = false;
    return {};
}

std::optional<QByteArrayView>
rawWorkingDirectoryArgument(std::span<char *const> arguments)
{
    std::optional<QByteArrayView> result;
    constexpr QByteArrayView prefix("--working-directory=");
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const char *const raw = arguments[index];
        if (raw == nullptr) continue;
        const QByteArrayView argument(raw,
                                      static_cast<qsizetype>(std::strlen(raw)));
        if (argument == QByteArrayView("--")
            || argument == QByteArrayView("-e")) {
            break;
        }
        if (argument.startsWith(prefix)) {
            result = argument.sliced(prefix.size());
            continue;
        }
        if (argument == QByteArrayView("--working-directory")
            && index + 1 < arguments.size()
            && arguments[index + 1] != nullptr) {
            const char *const value = arguments[++index];
            result = QByteArrayView(value,
                                    static_cast<qsizetype>(std::strlen(value)));
        }
    }
    return result;
}

std::optional<qsizetype> explicitCommandBoundary(const QStringList &arguments)
{
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const QStringView argument(arguments.at(index));
        if (argument == QLatin1StringView("--")) {
            // The frontend's existing `--` spelling makes everything after it
            // positional, including a literal executable named `-e`.
            return std::nullopt;
        }
        if (argument == QLatin1StringView("-e")) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<bool> parseGhosttyBoolean(QStringView value)
{
    if (value == QLatin1StringView("true") || value == QLatin1StringView("1")
        || value == QLatin1StringView("t") || value == QLatin1StringView("T")) {
        return true;
    }
    if (value == QLatin1StringView("false") || value == QLatin1StringView("0")
        || value == QLatin1StringView("f") || value == QLatin1StringView("F")) {
        return false;
    }
    return std::nullopt;
}

void enforceExplicitCommandLifetime(const LaunchOptions &base,
                                    LaunchOptions &destination)
{
    if (base.program.isEmpty()) return;
    destination.quitAfterLastWindowClosed = true;
    destination.quitAfterLastWindowClosedDelay.reset();
}

} // namespace

TerminalSessionRuntimeOptions
toTerminalSessionRuntimeOptions(const LaunchOptions &options)
{
    return {
        .appearance = options.appearance,
        .colorScheme = options.colorScheme,
        .enquiryResponse = options.enquiryResponse,
        .selectionClipboard = options.selectionClipboard,
        .selectionWordChars = options.selectionWordChars,
        .clickRepeatIntervalMilliseconds =
            options.clickRepeatIntervalMilliseconds,
        .clipboardWrite = options.clipboardWrite,
        .clipboardPaste = options.clipboardPaste,
        .rightClickAction = options.rightClickAction,
        .linkUrl = options.linkUrl,
        .linkOsc8 = options.linkOsc8,
        .graphemeWidthMethod = options.graphemeWidthMethod,
        .titleReport = options.titleReport,
        .vtKamAllowed = options.vtKamAllowed,
        .scrollbackCompression = options.scrollbackCompression,
        .kittyImageStorageLimitBytes = options.kittyImageStorageLimitBytes,
        .scrollToBottom = options.scrollToBottom,
        .abnormalCommandExitRuntimeMilliseconds =
            options.abnormalCommandExitRuntimeMilliseconds,
        .waitAfterCommand = options.waitAfterCommand,
    };
}

LaunchOptions withoutInitialCommand(LaunchOptions options)
{
    options.initialCommand.reset();
    options.program.clear();
    options.hold = false;
    return options;
}

TerminalSessionLaunchOptions
toTerminalSessionLaunchOptions(const LaunchOptions &options)
{
    return {
        .term = options.term,
        .environment = options.environment,
        .shellIntegration = options.shellIntegration,
        .shellIntegrationFeatures = options.shellIntegrationFeatures,
        .shellIntegrationAvailable = options.shellIntegrationAvailable,
        .linuxCgroup = options.linuxCgroup,
        .processUsesSingleInstance = options.processUsesSingleInstance,
        .workingDirectory = options.workingDirectory,
        .inheritWorkingDirectory = options.inheritWorkingDirectory,
        .configuredTitle = options.configuredTitle,
        .command = options.ordinaryCommand,
        .program = options.program,
        .firstSessionCommandOverride = std::nullopt,
        .initialInput = options.initialInput,
        .scrollbackLimits = options.scrollbackLimits,
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

    result.term = config.term;
    result.enquiryResponse = config.enquiryResponse;
    // Ghostty finalization normally resolves the default shell, but its
    // runtime still defines shell-form `sh` when neither SHELL nor passwd
    // provides one. Materialize that fallback while this value is known to
    // come from a finalized Ghostty snapshot; raw worker callers retain their
    // generic no-snapshot behavior.
    result.ordinaryCommand = config.ordinaryCommand.value_or(
        TerminalCommand::shell(QByteArrayLiteral("sh"), true));
    result.initialCommand = config.initialCommand;
    result.initialInput = config.initialInput;
    result.abnormalCommandExitRuntimeMilliseconds =
        config.abnormalCommandExitRuntimeMilliseconds;
    result.waitAfterCommand = config.waitAfterCommand;
    result.environment = config.environment;
    result.shellIntegration = config.shellIntegration;
    result.shellIntegrationFeatures = config.shellIntegrationFeatures;
    result.shellIntegrationAvailable = true;
    result.linuxCgroup = config.linuxCgroup;
    if (!base.workingDirectoryExplicit) {
        result.inheritWorkingDirectory = !config.workingDirectoryPath;
        if (config.workingDirectoryPath) {
            // The helper has already finalized home and tilde expansion.
            // Preserve the remaining path byte-for-byte: lexical cleanup
            // changes `symlink/../...` lookup.
            result.workingDirectory = *config.workingDirectoryPath;
        }
    }
    result.typography = config.typography;
    result.applicationClass = config.applicationClass;
    result.configDefaultFiles = config.configDefaultFiles;
    // Pinned Ghostty's generic f32 parser accepts zero and negative values,
    // but they do not form a usable font face. Preserve the previous
    // frontend contract by retaining the launch/default size for those
    // values while still applying every other finalized typography field.
    // Valid explicit font CLI arguments are already part of this snapshot:
    // Ghostty applies recursive includes after CLI parsing, so overlaying the
    // original frontend values here would corrupt that pinned precedence.
    if (!std::isfinite(config.typography.pointSize)
        || config.typography.pointSize <= 0.0) {
        result.typography.pointSize = base.typography.pointSize;
    }

    result.configuredTitle = config.title;
    result.appearance = config.appearance;
    result.alphaBlending = config.alphaBlending;
    result.background = config.background;
    const std::chrono::milliseconds paneEnterTransitionDuration =
        result.customShaders.paneEnterTransitionDuration;
    const std::chrono::milliseconds paneExitTransitionDuration =
        result.customShaders.paneExitTransitionDuration;
    const QVector<GhosttyConfigPath> paneEnterTransitionSources =
        result.customShaders.paneEnterTransitionSources;
    const QVector<GhosttyConfigPath> paneExitTransitionSources =
        result.customShaders.paneExitTransitionSources;
    result.customShaders = config.customShaders;
    result.customShaders.paneEnterTransitionDuration =
        paneEnterTransitionDuration;
    result.customShaders.paneExitTransitionDuration =
        paneExitTransitionDuration;
    result.customShaders.paneEnterTransitionSources =
        paneEnterTransitionSources;
    result.customShaders.paneExitTransitionSources = paneExitTransitionSources;
    result.backgroundBlur = config.backgroundBlur;
    result.padding = config.padding;
    result.splitAppearance = config.splitAppearance;
    result.splitInheritWorkingDirectory = config.splitInheritWorkingDirectory;
    result.splitPreserveZoomNavigation = config.splitPreserveZoom;
    result.tabInheritWorkingDirectory = config.tabInheritWorkingDirectory;
    result.windowInheritWorkingDirectory = config.windowInheritWorkingDirectory;
    result.windowInheritFontSize = config.windowInheritFontSize;
    result.windowNewTabPosition = config.windowNewTabPosition;
    result.windowShowTabBar = config.windowShowTabBar;
    result.dragHandle = config.dragHandle;
    result.windowDecoration = config.windowDecoration;
    result.windowAppearance = config.windowAppearance;
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

    result.scrollbackLimits.bytes = config.scrollbackLimitBytes;
    if (!base.scrollbackLinesExplicit) {
        result.scrollbackLimits.lines = config.scrollbackLimitLines;
    }
    result.scrollbackCompression = config.scrollbackCompression;
    result.kittyImageStorageLimitBytes = config.kittyImageStorageLimitBytes;
    result.scrollbar = config.scrollbar;
    result.desktopNotifications = config.desktopNotifications;
    result.progressStyle = config.progressStyle;
    result.bellFeatures = config.bellFeatures;
    result.bellAudioPath = config.bellAudioPath;
    result.bellAudioVolume = config.bellAudioVolume;
    result.confirmCloseMode = config.confirmCloseMode;
    result.selectionClipboard = config.selectionClipboard;
    result.selectionWordChars = config.selectionWordChars;
    result.clickRepeatIntervalMilliseconds =
        config.clickRepeatIntervalMilliseconds;
    result.clipboardWrite = config.clipboardWrite;
    result.clipboardPaste = config.clipboardPaste;
    result.scrollToBottom = config.scrollToBottom;
    result.rightClickAction = config.rightClickAction;
    result.middleClickAction = config.middleClickAction;
    result.mouseReporting = config.mouseReporting;
    result.mouseShiftCapture = config.mouseShiftCapture;
    result.mouseHideWhileTyping = config.mouseHideWhileTyping;
    result.focusFollowsMouse = config.focusFollowsMouse;
    result.mouseScrollMultiplier = config.mouseScrollMultiplier;
    result.vtKamAllowed = config.vtKamAllowed;
    result.graphemeWidthMethod = config.graphemeWidthMethod;
    result.titleReport = config.titleReport;
    result.linkUrl = config.linkUrl;
    result.linkOsc8 = config.linkOsc8;
    result.linkPreviews = config.linkPreviews;
    result.applicationShell = config.applicationShell;
    result.modifierRemaps = config.modifierRemaps;
    result.keybindSource =
        GhosttyKeybindSource::structured(snapshot.keybindings);

    // Match Ghostty's `-e` contract: an explicitly supplied command always
    // exits with its final window and never inherits a lingering delay.
    enforceExplicitCommandLifetime(base, result);

    return result;
}

LaunchOptions
applyFrontendConfigSnapshot(LaunchOptions result,
                            const FrontendConfigSnapshot &snapshot)
{
    result.tabsLocation = snapshot.values.tabsLocation;
    result.wideTabs = snapshot.values.wideTabs;
    result.horizontalTabScroll = snapshot.values.horizontalTabScroll;
    result.quickTerminalLayerShell = snapshot.values.quickTerminalLayerShell;
    result.customShaders.paneEnterTransitionDuration =
        snapshot.values.paneEnterTransitionDuration;
    result.customShaders.paneExitTransitionDuration =
        snapshot.values.paneExitTransitionDuration;
    result.customShaders.paneEnterTransitionSources.clear();
    if (!snapshot.values.paneEnterTransitionShaderPath.isEmpty()) {
        result.customShaders.paneEnterTransitionSources.append({
            .path = frontendShaderPath(
                snapshot.values.paneEnterTransitionShaderPath,
                snapshot.sourcePath),
        });
    }
    result.customShaders.paneExitTransitionSources.clear();
    if (!snapshot.values.paneExitTransitionShaderPath.isEmpty()) {
        result.customShaders.paneExitTransitionSources.append({
            .path =
                frontendShaderPath(snapshot.values.paneExitTransitionShaderPath,
                                   snapshot.sourcePath),
        });
    }
    if (!result.singleInstanceModeExplicit) {
        result.singleInstanceMode = snapshot.values.singleInstanceMode;
    }
    return result;
}

LaunchOptions
resolveLaunchOptions(const LaunchOptions &base,
                     const GhosttyConfigSnapshot *ghosttySnapshot,
                     const FrontendConfigSnapshot *frontendSnapshot)
{
    LaunchOptions result = ghosttySnapshot != nullptr
        ? applyGhosttyConfigSnapshot(base, *ghosttySnapshot)
        : base;
    if (frontendSnapshot != nullptr) {
        result =
            applyFrontendConfigSnapshot(std::move(result), *frontendSnapshot);
    }
    return result;
}

bool shouldUseSingleInstance(const LaunchOptions &options,
                             QByteArrayView termProgram)
{
    if (options.hasUnforwardedLaunchPayload()) return false;
    switch (options.singleInstanceMode) {
    case SingleInstanceMode::Enabled: return true;
    case SingleInstanceMode::Disabled: return false;
    case SingleInstanceMode::Detect: return termProgram.isEmpty();
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
    case ConfirmCloseMode::Never: return false;
    case ConfirmCloseMode::RunningProcesses: return hasActiveProcess;
    case ConfirmCloseMode::Always: return true;
    }
    return true;
}

std::expected<LaunchOptions, QString>
parseLaunchOptions(const QStringList &arguments)
{
    if (arguments.isEmpty()) {
        return std::unexpected(QStringLiteral(
            "The argument list must include the application name."));
    }

    QStringList parserArguments = arguments;
    QStringList explicitCommand;
    const std::optional<qsizetype> commandBoundary =
        explicitCommandBoundary(arguments);
    if (commandBoundary.has_value()) {
        if (*commandBoundary + 1 >= arguments.size()) {
            return std::unexpected(QStringLiteral("Missing command after -e."));
        }
        explicitCommand = arguments.sliced(*commandBoundary + 1);
        parserArguments = arguments.sliced(0, *commandBoundary);
    }

    // QCommandLineParser has no optional-value options. Normalize Ghostty's
    // bare boolean spelling while retaining explicit true/false values. Stop
    // at `--` so an earlier-compatible positional command remains opaque.
    for (qsizetype index = 1; index < parserArguments.size(); ++index) {
        if (parserArguments.at(index) == QLatin1StringView("--")) break;
        if (parserArguments.at(index)
            == QLatin1StringView("--wait-after-command")) {
            parserArguments[index] =
                QStringLiteral("--wait-after-command=true");
        }
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("A Qt terminal emulator powered by libghostty."));

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
    const QCommandLineOption applicationClassOption(
        QStringLiteral("class"),
        QStringLiteral("Use <id> as the application identity."),
        QStringLiteral("id"));
    const QCommandLineOption titleOption(
        QStringLiteral("title"),
        QStringLiteral("Use <title> as the fixed terminal title."),
        QStringLiteral("title"));
    const QCommandLineOption configDefaultFilesOption(
        QStringLiteral("config-default-files"),
        QStringLiteral(
            "Load Ghostty's standard configuration files (true or false)."),
        QStringLiteral("boolean"));
    const QCommandLineOption scrollbackLinesOption(
        QStringLiteral("scrollback-lines"),
        QStringLiteral("Estimate scrollback capacity for <lines> rows."),
        QStringLiteral("lines"));
    const QCommandLineOption holdOption(
        QStringLiteral("hold"),
        QStringLiteral(
            "Keep the terminal open after the child process exits."));
    const QCommandLineOption waitAfterCommandOption(
        QStringLiteral("wait-after-command"),
        QStringLiteral("Wait for a key press after the child process exits."),
        QStringLiteral("boolean"));
    const QCommandLineOption singleInstanceOption(
        QStringLiteral("single-instance"),
        QStringLiteral("Use false, true, or detect process uniqueness."),
        QStringLiteral("mode"));
    QCommandLineOption legacySingleInstanceOption(
        QStringLiteral("gtk-single-instance"),
        QStringLiteral("Deprecated alias for --single-instance."),
        QStringLiteral("mode"));
    legacySingleInstanceOption.setFlags(QCommandLineOption::HiddenFromHelp);
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
                       applicationClassOption, titleOption,
                       configDefaultFilesOption, scrollbackLinesOption,
                       holdOption, waitAfterCommandOption, singleInstanceOption,
                       legacySingleInstanceOption, initialWindowOption,
                       helpOption, versionOption});
    parser.addPositionalArgument(
        QStringLiteral("program"),
        QStringLiteral("Program and arguments to execute after --."),
        QStringLiteral("[program [arguments...]]"));

    if (!parser.parse(parserArguments)) {
        return std::unexpected(parser.errorText());
    }

    if (commandBoundary.has_value()
        && !parser.positionalArguments().isEmpty()) {
        return std::unexpected(
            QStringLiteral("Unexpected positional argument before -e: %1")
                .arg(parser.positionalArguments().constFirst()));
    }

    LaunchOptions parsed;
    parsed.workingDirectory = QDir::currentPath();
    parsed.inheritWorkingDirectory = true;

    if (parser.isSet(workingDirectoryOption)) {
        if (auto applied = applyWorkingDirectoryValue(
                parsed,
                QFile::encodeName(parser.value(workingDirectoryOption)));
            !applied) {
            return std::unexpected(std::move(applied.error()));
        }
    }

    if (parser.isSet(fontFamilyOption)) {
        QStringList &families =
            parsed.typography.face(TerminalFontRole::Regular).families;
        for (const QString &family : parser.values(fontFamilyOption)) {
            if (family.isEmpty()) {
                families.clear();
            } else {
                families.append(family);
            }
        }
        parsed.fontFamilyExplicit = true;
    }

    if (parser.isSet(fontSizeOption)) {
        const QString value = parser.value(fontSizeOption);
        bool ok = false;
        const double parsedFontSize = QLocale::c().toDouble(value, &ok);
        const float ghosttyFontSize = static_cast<float>(parsedFontSize);
        if (!ok || !std::isfinite(parsedFontSize) || parsedFontSize <= 0.0
            || !std::isfinite(ghosttyFontSize) || ghosttyFontSize <= 0.0F) {
            return std::unexpected(
                QStringLiteral(
                    "Invalid font size '%1': expected a finite number greater than 0.")
                    .arg(value));
        }
        // Ghostty's configuration field is f32. Store that exact value so
        // applying the helper snapshot cannot subtly change an explicit CLI
        // override through a double-versus-float rounding difference.
        parsed.typography.pointSize = static_cast<double>(ghosttyFontSize);
        parsed.fontSizeExplicit = true;
    }

    if (parser.isSet(applicationClassOption)) {
        for (const QString &value : parser.values(applicationClassOption)) {
            if (value.isEmpty()) {
                parsed.applicationClass.reset();
            } else {
                parsed.applicationClass = value.toUtf8();
            }
        }
        parsed.applicationClassExplicit = true;
    }

    if (parser.isSet(titleOption)) {
        const QString value = parser.value(titleOption);
        if (value.isEmpty()) {
            parsed.configuredTitle.reset();
        } else {
            parsed.configuredTitle = value;
        }
        parsed.configuredTitleExplicit = true;
    }

    if (parser.isSet(configDefaultFilesOption)) {
        for (const QString &value : parser.values(configDefaultFilesOption)) {
            const std::optional<bool> configDefaultFiles =
                parseGhosttyBoolean(value);
            if (!configDefaultFiles.has_value()) {
                return std::unexpected(
                    QStringLiteral(
                        "Invalid config-default-files value '%1': expected true or false.")
                        .arg(value));
            }
            parsed.configDefaultFiles = *configDefaultFiles;
        }
        parsed.configDefaultFilesExplicit = true;
    }

    if (parser.isSet(scrollbackLinesOption)) {
        const QString value = parser.value(scrollbackLinesOption);
        bool ok = false;
        const qlonglong scrollbackLines = value.toLongLong(&ok);
        if (!ok || scrollbackLines < 0
            || scrollbackLines > kMaximumScrollbackLines) {
            return std::unexpected(
                QStringLiteral(
                    "Invalid scrollback line count '%1': expected an integer from 0 to %2.")
                    .arg(value)
                    .arg(kMaximumScrollbackLines));
        }
        parsed.scrollbackLimits.lines = static_cast<quint64>(scrollbackLines);
        parsed.scrollbackLinesExplicit = true;
    }

    parsed.hold = parser.isSet(holdOption);
    if (parser.isSet(waitAfterCommandOption)) {
        for (const QString &value : parser.values(waitAfterCommandOption)) {
            const std::optional<bool> waitAfterCommand =
                parseGhosttyBoolean(value);
            if (!waitAfterCommand.has_value()) {
                return std::unexpected(
                    QStringLiteral(
                        "Invalid wait-after-command value '%1': expected true or false.")
                        .arg(value));
            }
            parsed.waitAfterCommand = *waitAfterCommand;
        }
        parsed.waitAfterCommandExplicit = true;
    }
    parsed.showHelp = parser.isSet(helpOption);
    parsed.showVersion = parser.isSet(versionOption);
    parsed.program = commandBoundary.has_value() ? std::move(explicitCommand)
                                                 : parser.positionalArguments();
    const bool singleInstanceSet = parser.isSet(singleInstanceOption);
    const bool legacySingleInstanceSet =
        parser.isSet(legacySingleInstanceOption);
    if (singleInstanceSet && legacySingleInstanceSet) {
        return std::unexpected(
            QStringLiteral("--single-instance and its deprecated alias "
                           "--gtk-single-instance cannot be used together."));
    }
    if (singleInstanceSet || legacySingleInstanceSet) {
        const QString mode =
            parser.value(singleInstanceSet ? singleInstanceOption
                                           : legacySingleInstanceOption);
        if (mode == QStringLiteral("true")) {
            parsed.singleInstanceMode = SingleInstanceMode::Enabled;
        } else if (mode == QStringLiteral("false")) {
            parsed.singleInstanceMode = SingleInstanceMode::Disabled;
        } else if (mode == QStringLiteral("detect")) {
            parsed.singleInstanceMode = SingleInstanceMode::Detect;
        } else {
            return std::unexpected(
                QStringLiteral("Invalid single-instance value '%1': expected "
                               "false, true, or detect.")
                    .arg(mode));
        }
        parsed.singleInstanceModeExplicit = true;
    }

    if (parser.isSet(initialWindowOption)) {
        const QString value = parser.value(initialWindowOption);
        const std::optional<bool> initialWindow = parseGhosttyBoolean(value);
        if (!initialWindow.has_value()) {
            return std::unexpected(
                QStringLiteral(
                    "Invalid initial-window value '%1': expected true or false.")
                    .arg(value));
        }
        parsed.initialWindow = *initialWindow;
        parsed.initialWindowExplicit = true;
    }

    return parsed;
}

std::expected<LaunchOptions, QString>
parseLaunchOptionsFromRaw(std::span<char *const> arguments)
{
    QStringList decoded;
    decoded.reserve(static_cast<qsizetype>(arguments.size()));
    for (char *const argument : arguments) {
        decoded.append(
            QString::fromLocal8Bit(argument != nullptr ? argument : ""));
    }

    auto parsed = parseLaunchOptions(decoded);
    if (!parsed) return parsed;
    if (const auto raw = rawWorkingDirectoryArgument(arguments)) {
        if (auto applied = applyWorkingDirectoryValue(*parsed, *raw);
            !applied) {
            return std::unexpected(std::move(applied.error()));
        }
    }
    return parsed;
}

QStringList ghosttyConfigurationArguments(const LaunchOptions &options)
{
    QStringList result;
    const QStringList &families =
        options.typography.face(TerminalFontRole::Regular).families;
    result.reserve((options.fontFamilyExplicit
                        ? std::max<qsizetype>(1, families.size())
                        : 0)
                   + static_cast<qsizetype>(options.fontSizeExplicit)
                   + static_cast<qsizetype>(options.applicationClassExplicit)
                   + static_cast<qsizetype>(options.configDefaultFilesExplicit)
                   + static_cast<qsizetype>(options.configuredTitleExplicit)
                   + static_cast<qsizetype>(options.waitAfterCommandExplicit));

    if (options.fontFamilyExplicit) {
        if (families.isEmpty()) {
            result.append(QStringLiteral("--font-family="));
        } else {
            for (const QString &family : families) {
                result.append(QStringLiteral("--font-family=") + family);
            }
        }
    }
    if (options.fontSizeExplicit) {
        result.append(
            QStringLiteral("--font-size=")
            + QString::number(options.typography.pointSize, 'g',
                              std::numeric_limits<float>::max_digits10));
    }
    if (options.applicationClassExplicit) {
        const QString value = options.applicationClass
            ? QString::fromUtf8(*options.applicationClass)
            : QString{};
        result.append(QStringLiteral("--class=") + value);
    }
    if (options.configuredTitleExplicit) {
        result.append(QStringLiteral("--title=")
                      + options.configuredTitle.value_or(QString{}));
    }
    if (options.configDefaultFilesExplicit) {
        result.append(options.configDefaultFiles
                          ? QStringLiteral("--config-default-files=true")
                          : QStringLiteral("--config-default-files=false"));
    }
    if (options.waitAfterCommandExplicit) {
        result.append(options.waitAfterCommand
                          ? QStringLiteral("--wait-after-command=true")
                          : QStringLiteral("--wait-after-command=false"));
    }

    return result;
}
