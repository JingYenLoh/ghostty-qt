#include "ghostty_config_process_loader.h"
#include "ghostty_action_catalog.h"
#include "ghostty_config_export.h"
#include "ghostty_keybind_set.h"

#include <QColor>
#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QSet>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <utility>

namespace {

constexpr auto LegacyConfigName = "config";
constexpr auto PreferredConfigName = "config.ghostty";
using SparsePalette = std::array<std::optional<QColor>, 256>;

struct ParsedConfig {
    std::optional<QString> workingDirectory;
    std::optional<QStringList> fontFamilies;
    std::optional<double> fontSize;
    std::optional<QColor> foreground;
    std::optional<QColor> background;
    std::optional<double> unfocusedSplitOpacity;
    std::optional<QVariant> unfocusedSplitFill;
    std::optional<QVariant> splitDividerColor;
    std::optional<bool> splitInheritWorkingDirectory;
    std::optional<bool> splitPreserveZoomNavigation;
    std::optional<bool> tabInheritWorkingDirectory;
    std::optional<bool> windowInheritWorkingDirectory;
    std::optional<bool> windowInheritFontSize;
    std::optional<QString> windowNewTabPosition;
    std::optional<QString> windowShowTabBar;
    std::optional<quint32> windowWidth;
    std::optional<quint32> windowHeight;
    std::optional<bool> maximize;
    std::optional<QString> fullscreen;
    SparsePalette palette;
    std::optional<QVariant> selectionForeground;
    std::optional<QVariant> selectionBackground;
    std::optional<QVariant> searchForeground;
    std::optional<QVariant> searchBackground;
    std::optional<QVariant> searchSelectedForeground;
    std::optional<QVariant> searchSelectedBackground;
    std::optional<QVariant> cursorColor;
    std::optional<double> cursorOpacity;
    std::optional<QString> cursorStyle;
    std::optional<QVariant> cursorStyleBlink;
    std::optional<QVariant> cursorText;
    std::optional<QVariant> boldColor;
    std::optional<double> faintOpacity;
    std::optional<quint64> scrollbackLimit;
    std::optional<QString> confirmCloseSurface;
    std::optional<bool> clipboardTrimTrailingSpaces;
    std::optional<bool> clipboardPasteProtection;
    std::optional<bool> clipboardPasteBracketedSafe;
    std::optional<QString> copyOnSelect;
    std::optional<bool> selectionClearOnTyping;
    std::optional<bool> selectionClearOnCopy;
    std::optional<QString> middleClickAction;
    std::optional<bool> mouseReporting;
    std::optional<bool> linkUrl;
    std::optional<QString> linkPreviews;
    std::optional<QStringList> keybinds;
    std::optional<QStringList> configFiles;
};

template<typename... Values>
bool allPresent(const std::optional<Values> &...values)
{
    return (... && values.has_value());
}

bool hasRequiredFields(const ParsedConfig &parsed)
{
    const bool hasPaletteEntry = std::ranges::any_of(
        parsed.palette,
        [](const auto &color) { return color.has_value(); });
    return hasPaletteEntry
        && allPresent(
            parsed.workingDirectory,
            parsed.fontFamilies,
            parsed.fontSize,
            parsed.foreground,
            parsed.background,
            parsed.unfocusedSplitOpacity,
            parsed.unfocusedSplitFill,
            parsed.splitDividerColor,
            parsed.splitInheritWorkingDirectory,
            parsed.splitPreserveZoomNavigation,
            parsed.tabInheritWorkingDirectory,
            parsed.windowInheritWorkingDirectory,
            parsed.windowInheritFontSize,
            parsed.windowNewTabPosition,
            parsed.windowShowTabBar,
            parsed.windowWidth,
            parsed.windowHeight,
            parsed.maximize,
            parsed.fullscreen,
            parsed.selectionForeground,
            parsed.selectionBackground,
            parsed.searchForeground,
            parsed.searchBackground,
            parsed.searchSelectedForeground,
            parsed.searchSelectedBackground,
            parsed.cursorColor,
            parsed.cursorOpacity,
            parsed.cursorStyle,
            parsed.cursorStyleBlink,
            parsed.cursorText,
            parsed.boldColor,
            parsed.faintOpacity,
            parsed.scrollbackLimit,
            parsed.confirmCloseSurface,
            parsed.clipboardTrimTrailingSpaces,
            parsed.clipboardPasteProtection,
            parsed.clipboardPasteBracketedSafe,
            parsed.copyOnSelect,
            parsed.selectionClearOnTyping,
            parsed.selectionClearOnCopy,
            parsed.middleClickAction,
            parsed.mouseReporting,
            parsed.linkUrl,
            parsed.linkPreviews,
            parsed.keybinds,
            parsed.configFiles);
}

struct ProcessResult {
    enum class Status {
        Completed,
        StartFailed,
        TimedOut,
        Crashed,
    };

    Status status = Status::StartFailed;
    int exitCode = -1;
    QByteArray standardOutput;
    QByteArray standardError;
};

QString normalizedAbsolutePath(const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

void setError(QString *destination, QString message)
{
    if (destination) {
        *destination = std::move(message);
    }
}

QString conciseProcessDetails(const ProcessResult &result)
{
    QStringList details;
    const QString standardOutput = QString::fromUtf8(result.standardOutput).trimmed();
    const QString standardError = QString::fromUtf8(result.standardError).trimmed();
    if (!standardOutput.isEmpty()) {
        details.append(QStringLiteral("stdout: %1").arg(standardOutput));
    }
    if (!standardError.isEmpty()) {
        details.append(QStringLiteral("stderr: %1").arg(standardError));
    }
    return details.join(QStringLiteral("; "));
}

ProcessResult runHelper(const GhosttyConfigProcessLoaderOptions &options,
                        const QStringList &arguments,
                        const QString &xdgConfigHome,
                        int timeoutMilliseconds)
{
    QProcess process;
    QProcessEnvironment environment = options.environment;
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), xdgConfigHome);
    process.setProcessEnvironment(environment);
    process.setProgram(options.helperPath);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(QIODevice::ReadOnly);

    const int timeout = std::max(1, timeoutMilliseconds);
    QDeadlineTimer deadline(timeout);
    const int startTimeout = static_cast<int>(std::clamp<qint64>(
        deadline.remainingTime(), 1, std::numeric_limits<int>::max()));
    if (!process.waitForStarted(startTimeout)) {
        return {
            .status = ProcessResult::Status::StartFailed,
            .exitCode = -1,
            .standardOutput = {},
            .standardError = {},
        };
    }
    const int finishTimeout = static_cast<int>(std::clamp<qint64>(
        deadline.remainingTime(), 1, std::numeric_limits<int>::max()));
    if (!process.waitForFinished(finishTimeout)) {
        process.kill();
        process.waitForFinished(1'000);
        return {
            .status = ProcessResult::Status::TimedOut,
            .standardOutput = process.readAllStandardOutput(),
            .standardError = process.readAllStandardError(),
        };
    }

    ProcessResult result{
        .status = process.exitStatus() == QProcess::CrashExit
            ? ProcessResult::Status::Crashed
            : ProcessResult::Status::Completed,
        .exitCode = process.exitCode(),
        .standardOutput = process.readAllStandardOutput(),
        .standardError = process.readAllStandardError(),
    };
    return result;
}

GhosttyConfigLoadResult processFailure(const QString &operation,
                                       const ProcessResult &result,
                                       int timeoutMilliseconds)
{
    switch (result.status) {
    case ProcessResult::Status::StartFailed:
        return std::unexpected(
            QStringLiteral("Ghostty config helper could not be started during %1")
                .arg(operation));
    case ProcessResult::Status::TimedOut:
        return std::unexpected(
            QStringLiteral("Ghostty config helper timed out during %1 after %2 ms")
                .arg(operation)
                .arg(std::max(1, timeoutMilliseconds)));
    case ProcessResult::Status::Crashed:
        return std::unexpected(
            QStringLiteral("Ghostty config helper crashed during %1").arg(operation));
    case ProcessResult::Status::Completed:
        break;
    }

    QString message =
        QStringLiteral("Ghostty config helper failed during %1 with exit code %2")
            .arg(operation)
            .arg(result.exitCode);
    const QString details = conciseProcessDetails(result);
    if (!details.isEmpty()) {
        message.append(QStringLiteral(": %1").arg(details));
    }
    return std::unexpected(std::move(message));
}

void appendProcessDiagnostics(GhosttyConfigSnapshot *snapshot,
                              const QString &operation,
                              const QByteArray &output)
{
    if (snapshot == nullptr || output.trimmed().isEmpty()) {
        return;
    }
    const QList<QByteArray> lines = output.split('\n');
    for (QByteArray line : lines) {
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        const QString message = QString::fromUtf8(line).trimmed();
        if (message.isEmpty()) {
            continue;
        }
        GhosttyConfigDiagnostic diagnostic{
            .severity = GhosttyConfigDiagnosticSeverity::Warning,
            .message = QStringLiteral("Ghostty config helper %1: %2")
                           .arg(operation, message),
            .sourcePath = {},
            .line = 0,
            .column = 0,
        };
        if (!snapshot->diagnostics.contains(diagnostic)) {
            snapshot->diagnostics.append(std::move(diagnostic));
        }
    }
}

QString unsupportedKeybindReason(GhosttyKeybindUnsupportedReason reason)
{
    switch (reason) {
    case GhosttyKeybindUnsupportedReason::KeyTable:
        return QStringLiteral(
            "named key tables require the structured config export");
    case GhosttyKeybindUnsupportedReason::NonLocal:
        return QStringLiteral(
            "all-surface/global triggers require the structured config export");
    case GhosttyKeybindUnsupportedReason::ClearDirective:
        return QStringLiteral("an unresolved clear directive reached the flattened set");
    case GhosttyKeybindUnsupportedReason::OrphanChain:
        return QStringLiteral("the action chain has no adjacent supported trigger");
    case GhosttyKeybindUnsupportedReason::None:
        return QStringLiteral("the trigger is outside the current compatibility layer");
    }
    return QStringLiteral("the trigger is outside the current compatibility layer");
}

void appendKeybindCompatibilityDiagnostics(GhosttyConfigSnapshot *snapshot,
                                           const QStringList &keybinds)
{
    if (snapshot == nullptr) {
        return;
    }
    GhosttyKeybindSet set;
    const GhosttyKeybindLoadReport report = set.load(keybinds);
    for (const GhosttyKeybindParseRecord &record : report.records) {
        if (record.disposition != GhosttyKeybindEntryDisposition::Unsupported
            && record.disposition != GhosttyKeybindEntryDisposition::Invalid) {
            continue;
        }
        // Production matching uses the authoritative structured export, which
        // retains named tables and all:/global: flags. The flattened fallback
        // parser intentionally remains smaller; do not turn that limitation
        // into a false user-facing compatibility warning.
        if (record.reason == GhosttyKeybindUnsupportedReason::KeyTable
            || record.reason == GhosttyKeybindUnsupportedReason::NonLocal) {
            continue;
        }
        const QString reason = record.detail.isEmpty()
            ? unsupportedKeybindReason(record.reason)
            : record.detail;
        snapshot->diagnostics.append({
            .severity = GhosttyConfigDiagnosticSeverity::Warning,
            .message = QStringLiteral("ghostty-qt ignored keybind '%1': %2")
                           .arg(record.input, reason),
            .sourcePath = {},
            .line = 0,
            .column = 0,
        });
    }
    QSet<QString> reportedActions;
    for (const QString &action : set.serializedActions()) {
        if (GhosttyActionCatalog::isImplemented(action)
            || reportedActions.contains(action)) {
            continue;
        }
        reportedActions.insert(action);
        snapshot->diagnostics.append({
            .severity = GhosttyConfigDiagnosticSeverity::Warning,
            .message = QStringLiteral(
                "ghostty-qt does not implement configured keybind action '%1'")
                           .arg(action),
            .sourcePath = {},
            .line = 0,
            .column = 0,
        });
    }
}

bool parseColor(const QString &value, QColor *destination)
{
    const QColor parsed(value);
    if (!parsed.isValid()) {
        return false;
    }
    *destination = parsed;
    return true;
}

bool parseOptionalColor(const QString &value, QVariant *destination)
{
    if (value.isEmpty()) {
        *destination = value;
        return true;
    }
    QColor color;
    if (!parseColor(value, &color)) {
        return false;
    }
    *destination = color;
    return true;
}

bool parseTerminalColor(const QString &value, QVariant *destination)
{
    if (value.isEmpty()
        || value == QStringLiteral("cell-foreground")
        || value == QStringLiteral("cell-background")) {
        *destination = value;
        return true;
    }
    QColor color;
    if (!parseColor(value, &color)) return false;
    *destination = color;
    return true;
}

bool parsePaletteEntry(const QString &value, int *index, QColor *color)
{
    const qsizetype separator = value.indexOf(u'=');
    if (separator <= 0) return false;
    bool validIndex = false;
    const uint parsedIndex = value.left(separator).trimmed().toUInt(
        &validIndex, 10);
    if (!validIndex || parsedIndex >= 256) return false;
    if (!parseColor(value.mid(separator + 1).trimmed(), color)) return false;
    *index = static_cast<int>(parsedIndex);
    return true;
}

bool parseOptionalBool(const QString &value, QVariant *destination)
{
    if (value.isEmpty()) {
        *destination = value;
        return true;
    }
    if (value == QStringLiteral("true")) {
        *destination = true;
        return true;
    }
    if (value == QStringLiteral("false")) {
        *destination = false;
        return true;
    }
    return false;
}

bool parseBool(const QString &value, bool *destination)
{
    if (value == QStringLiteral("true")) {
        *destination = true;
        return true;
    }
    if (value == QStringLiteral("false")) {
        *destination = false;
        return true;
    }
    return false;
}

bool parseUnitInterval(const QString &value, double *destination)
{
    bool valid = false;
    const double parsed = value.toDouble(&valid);
    if (!valid || !std::isfinite(parsed) || parsed < 0.0 || parsed > 1.0) {
        return false;
    }
    *destination = parsed;
    return true;
}

bool parseFiniteDouble(const QString &value, double *destination)
{
    bool valid = false;
    const double parsed = value.toDouble(&valid);
    if (!valid || !std::isfinite(parsed)) {
        return false;
    }
    *destination = parsed;
    return true;
}

bool parseCanonicalUint32(const QString &value, quint32 *destination)
{
    if (value.isEmpty()
        || !std::ranges::all_of(value, [](QChar character) {
               return character >= u'0' && character <= u'9';
           })) {
        return false;
    }

    bool valid = false;
    const uint parsed = value.toUInt(&valid, 10);
    if (!valid) {
        return false;
    }
    *destination = static_cast<quint32>(parsed);
    return true;
}

bool parseDump(const QByteArray &dump,
               ParsedConfig *parsed,
               QString *errorMessage)
{
    const QString text = QString::fromUtf8(dump);
    const QStringList lines = text.split(u'\n');
    for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        QString line = lines.at(lineIndex);
        if (line.endsWith(u'\r')) {
            line.chop(1);
        }
        const QString trimmedLine = line.trimmed();
        if (trimmedLine.isEmpty() || trimmedLine.startsWith(u'#')) {
            continue;
        }

        const qsizetype separator = line.indexOf(u'=');
        if (separator < 0) {
            // Only keys consumed by this compatibility slice need parsing.
            // Ignore unrelated future formatter output rather than making the
            // application depend on every Ghostty config type at once.
            continue;
        }

        const QString key = line.left(separator).trimmed();
        const QString value = line.mid(separator + 1).trimmed();
        const int displayLine = lineIndex + 1;
        const auto parseRequiredBool = [&](std::optional<bool> &destination) {
            bool parsedValue = false;
            if (!parseBool(value, &parsedValue)) {
                setError(
                    errorMessage,
                    QStringLiteral("Invalid %1 in Ghostty config output at line %2")
                        .arg(key)
                        .arg(displayLine));
                return false;
            }
            destination = parsedValue;
            return true;
        };

        if (key == QStringLiteral("working-directory")) {
            parsed->workingDirectory = value;
        } else if (key == QStringLiteral("font-family")) {
            if (!parsed->fontFamilies.has_value()) {
                parsed->fontFamilies.emplace();
            }
            if (value.isEmpty()) {
                parsed->fontFamilies->clear();
            } else {
                parsed->fontFamilies->append(value);
            }
        } else if (key == QStringLiteral("font-size")) {
            bool valid = false;
            const double fontSize = value.toDouble(&valid);
            if (!valid || !std::isfinite(fontSize)) {
                setError(errorMessage,
                         QStringLiteral("Invalid font-size in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->fontSize = fontSize;
        } else if (key == QStringLiteral("foreground")) {
            QColor foreground;
            if (!parseColor(value, &foreground)) {
                setError(errorMessage,
                         QStringLiteral("Invalid foreground in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->foreground = std::move(foreground);
        } else if (key == QStringLiteral("background")) {
            QColor background;
            if (!parseColor(value, &background)) {
                setError(errorMessage,
                         QStringLiteral("Invalid background in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->background = std::move(background);
        } else if (key == QStringLiteral("unfocused-split-opacity")) {
            double opacity = 0.0;
            // Ghostty finalizes this value before +show-config publishes it.
            // Reject output outside that finalized contract so a helper/API
            // mismatch cannot silently reach the frontend.
            if (!parseUnitInterval(value, &opacity) || opacity < 0.15) {
                setError(errorMessage,
                         QStringLiteral("Invalid unfocused-split-opacity in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->unfocusedSplitOpacity = opacity;
        } else if (key == QStringLiteral("unfocused-split-fill")) {
            QVariant fill;
            if (!parseOptionalColor(value, &fill)) {
                setError(errorMessage,
                         QStringLiteral("Invalid unfocused-split-fill in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->unfocusedSplitFill = std::move(fill);
        } else if (key == QStringLiteral("split-divider-color")) {
            QVariant color;
            if (!parseOptionalColor(value, &color)) {
                setError(errorMessage,
                         QStringLiteral("Invalid split-divider-color in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->splitDividerColor = std::move(color);
        } else if (key == QStringLiteral("split-inherit-working-directory")) {
            if (!parseRequiredBool(parsed->splitInheritWorkingDirectory)) {
                return false;
            }
        } else if (key == QStringLiteral("split-preserve-zoom")) {
            if (value == QStringLiteral("navigation")) {
                parsed->splitPreserveZoomNavigation = true;
            } else if (value == QStringLiteral("no-navigation")) {
                parsed->splitPreserveZoomNavigation = false;
            } else {
                setError(
                    errorMessage,
                    QStringLiteral("Invalid split-preserve-zoom in Ghostty config output at line %1")
                        .arg(displayLine));
                return false;
            }
        } else if (key == QStringLiteral("tab-inherit-working-directory")) {
            if (!parseRequiredBool(parsed->tabInheritWorkingDirectory)) {
                return false;
            }
        } else if (key
                   == QStringLiteral("window-inherit-working-directory")) {
            if (!parseRequiredBool(parsed->windowInheritWorkingDirectory)) {
                return false;
            }
        } else if (key == QStringLiteral("window-inherit-font-size")) {
            if (!parseRequiredBool(parsed->windowInheritFontSize)) {
                return false;
            }
        } else if (key == QStringLiteral("window-new-tab-position")) {
            if (value != QStringLiteral("current")
                && value != QStringLiteral("end")) {
                setError(
                    errorMessage,
                    QStringLiteral("Invalid window-new-tab-position in Ghostty config output at line %1")
                        .arg(displayLine));
                return false;
            }
            parsed->windowNewTabPosition = value;
        } else if (key == QStringLiteral("window-show-tab-bar")) {
            if (value != QStringLiteral("always")
                && value != QStringLiteral("auto")
                && value != QStringLiteral("never")) {
                setError(
                    errorMessage,
                    QStringLiteral("Invalid window-show-tab-bar in Ghostty config output at line %1")
                        .arg(displayLine));
                return false;
            }
            parsed->windowShowTabBar = value;
        } else if (key == QStringLiteral("window-width")
                   || key == QStringLiteral("window-height")) {
            quint32 dimension = 0;
            if (!parseCanonicalUint32(value, &dimension)) {
                setError(
                    errorMessage,
                    QStringLiteral("Invalid %1 in Ghostty config output at line %2")
                        .arg(key)
                        .arg(displayLine));
                return false;
            }
            if (key == QStringLiteral("window-width")) {
                parsed->windowWidth = dimension;
            } else {
                parsed->windowHeight = dimension;
            }
        } else if (key == QStringLiteral("maximize")) {
            if (!parseRequiredBool(parsed->maximize)) {
                return false;
            }
        } else if (key == QStringLiteral("fullscreen")) {
            if (value != QStringLiteral("false")
                && value != QStringLiteral("true")
                && value != QStringLiteral("non-native")
                && value != QStringLiteral("non-native-visible-menu")
                && value != QStringLiteral("non-native-padded-notch")) {
                setError(
                    errorMessage,
                    QStringLiteral("Invalid fullscreen in Ghostty config output at line %1")
                        .arg(displayLine));
                return false;
            }
            parsed->fullscreen = value;
        } else if (key == QStringLiteral("palette")) {
            int index = 0;
            QColor color;
            if (!parsePaletteEntry(value, &index, &color)) {
                setError(errorMessage,
                         QStringLiteral("Invalid palette in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->palette[static_cast<std::size_t>(index)] = color;
        } else if (key == QStringLiteral("selection-foreground")) {
            QVariant selectionForeground;
            if (!parseTerminalColor(value, &selectionForeground)) {
                setError(errorMessage,
                         QStringLiteral("Invalid selection-foreground in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->selectionForeground = std::move(selectionForeground);
        } else if (key == QStringLiteral("selection-background")) {
            QVariant selectionBackground;
            if (!parseTerminalColor(value, &selectionBackground)) {
                setError(errorMessage,
                         QStringLiteral("Invalid selection-background in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->selectionBackground = std::move(selectionBackground);
        } else if (key == QStringLiteral("search-foreground")) {
            QVariant searchForeground;
            if (value.isEmpty()
                || !parseTerminalColor(value, &searchForeground)) {
                setError(errorMessage,
                         QStringLiteral("Invalid search-foreground in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->searchForeground = std::move(searchForeground);
        } else if (key == QStringLiteral("search-background")) {
            QVariant searchBackground;
            if (value.isEmpty()
                || !parseTerminalColor(value, &searchBackground)) {
                setError(errorMessage,
                         QStringLiteral("Invalid search-background in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->searchBackground = std::move(searchBackground);
        } else if (key == QStringLiteral("search-selected-foreground")) {
            QVariant searchSelectedForeground;
            if (value.isEmpty()
                || !parseTerminalColor(value,
                                       &searchSelectedForeground)) {
                setError(errorMessage,
                         QStringLiteral("Invalid search-selected-foreground in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->searchSelectedForeground =
                std::move(searchSelectedForeground);
        } else if (key == QStringLiteral("search-selected-background")) {
            QVariant searchSelectedBackground;
            if (value.isEmpty()
                || !parseTerminalColor(value,
                                       &searchSelectedBackground)) {
                setError(errorMessage,
                         QStringLiteral("Invalid search-selected-background in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->searchSelectedBackground =
                std::move(searchSelectedBackground);
        } else if (key == QStringLiteral("cursor-color")) {
            QVariant cursorColor;
            if (!parseTerminalColor(value, &cursorColor)) {
                setError(errorMessage,
                         QStringLiteral("Invalid cursor-color in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->cursorColor = std::move(cursorColor);
        } else if (key == QStringLiteral("cursor-opacity")) {
            double cursorOpacity = 0.0;
            if (!parseFiniteDouble(value, &cursorOpacity)) {
                setError(errorMessage,
                         QStringLiteral("Invalid cursor-opacity in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->cursorOpacity = cursorOpacity;
        } else if (key == QStringLiteral("cursor-style")) {
            if (value != QStringLiteral("block")
                && value != QStringLiteral("bar")
                && value != QStringLiteral("underline")
                && value != QStringLiteral("block_hollow")) {
                setError(errorMessage,
                         QStringLiteral("Invalid cursor-style in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->cursorStyle = value;
        } else if (key == QStringLiteral("cursor-style-blink")) {
            QVariant cursorStyleBlink;
            if (!parseOptionalBool(value, &cursorStyleBlink)) {
                setError(errorMessage,
                         QStringLiteral("Invalid cursor-style-blink in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->cursorStyleBlink = std::move(cursorStyleBlink);
        } else if (key == QStringLiteral("cursor-text")) {
            QVariant cursorText;
            if (!parseTerminalColor(value, &cursorText)) {
                setError(errorMessage,
                         QStringLiteral("Invalid cursor-text in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->cursorText = std::move(cursorText);
        } else if (key == QStringLiteral("bold-color")) {
            QVariant boldColor;
            if (value.isEmpty() || value == QStringLiteral("bright")) {
                boldColor = value;
            } else {
                QColor color;
                if (!parseColor(value, &color)) {
                    setError(errorMessage,
                             QStringLiteral("Invalid bold-color in Ghostty config output at line %1")
                                 .arg(displayLine));
                    return false;
                }
                boldColor = color;
            }
            parsed->boldColor = std::move(boldColor);
        } else if (key == QStringLiteral("faint-opacity")) {
            double faintOpacity = 0.0;
            if (!parseUnitInterval(value, &faintOpacity)) {
                setError(errorMessage,
                         QStringLiteral("Invalid faint-opacity in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->faintOpacity = faintOpacity;
        } else if (key == QStringLiteral("scrollback-limit")) {
            bool valid = false;
            const quint64 scrollbackLimit = value.toULongLong(&valid);
            if (!valid || value.startsWith(u'-')) {
                setError(errorMessage,
                         QStringLiteral("Invalid scrollback-limit in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->scrollbackLimit = scrollbackLimit;
        } else if (key == QStringLiteral("confirm-close-surface")) {
            if (value != QStringLiteral("false")
                && value != QStringLiteral("true")
                && value != QStringLiteral("always")) {
                setError(errorMessage,
                         QStringLiteral("Invalid confirm-close-surface in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->confirmCloseSurface = value;
        } else if (key == QStringLiteral("clipboard-trim-trailing-spaces")) {
            if (!parseRequiredBool(parsed->clipboardTrimTrailingSpaces)) {
                return false;
            }
        } else if (key == QStringLiteral("clipboard-paste-protection")) {
            if (!parseRequiredBool(parsed->clipboardPasteProtection)) {
                return false;
            }
        } else if (key == QStringLiteral("clipboard-paste-bracketed-safe")) {
            if (!parseRequiredBool(parsed->clipboardPasteBracketedSafe)) {
                return false;
            }
        } else if (key == QStringLiteral("copy-on-select")) {
            if (value != QStringLiteral("false")
                && value != QStringLiteral("true")
                && value != QStringLiteral("clipboard")) {
                setError(errorMessage,
                         QStringLiteral("Invalid copy-on-select in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->copyOnSelect = value;
        } else if (key == QStringLiteral("selection-clear-on-typing")) {
            if (!parseRequiredBool(parsed->selectionClearOnTyping)) {
                return false;
            }
        } else if (key == QStringLiteral("selection-clear-on-copy")) {
            if (!parseRequiredBool(parsed->selectionClearOnCopy)) {
                return false;
            }
        } else if (key == QStringLiteral("middle-click-action")) {
            if (value != QStringLiteral("primary-paste")
                && value != QStringLiteral("ignore")) {
                setError(errorMessage,
                         QStringLiteral("Invalid middle-click-action in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->middleClickAction = value;
        } else if (key == QStringLiteral("mouse-reporting")) {
            if (!parseRequiredBool(parsed->mouseReporting)) {
                return false;
            }
        } else if (key == QStringLiteral("link-url")) {
            if (!parseRequiredBool(parsed->linkUrl)) {
                return false;
            }
        } else if (key == QStringLiteral("link-previews")) {
            if (value != QStringLiteral("false")
                && value != QStringLiteral("true")
                && value != QStringLiteral("osc8")) {
                setError(errorMessage,
                         QStringLiteral("Invalid link-previews in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->linkPreviews = value;
        } else if (key == QStringLiteral("keybind")) {
            if (!parsed->keybinds.has_value()) {
                parsed->keybinds.emplace();
            }
            // An empty formatted entry is Ghostty's effective empty binding
            // set. Do not retain a synthetic empty binding.
            if (!value.isEmpty()) {
                parsed->keybinds->append(value);
            }
        } else if (key == QStringLiteral("config-file")) {
            if (!parsed->configFiles.has_value()) {
                parsed->configFiles.emplace();
            }
            if (value.isEmpty()) {
                parsed->configFiles->clear();
            } else {
                parsed->configFiles->append(value);
            }
        }
    }
    return true;
}

QString configPathWithoutOptionalMarker(QString path)
{
    if (path.startsWith(u'?')) {
        path.remove(0, 1);
    }
    return normalizedAbsolutePath(path);
}

void appendExistingSource(QStringList *sources,
                          QSet<QString> *seen,
                          const QString &path)
{
    const QString normalized = configPathWithoutOptionalMarker(path);
    if (!normalized.isEmpty() && QFileInfo(normalized).isFile()
        && !seen->contains(normalized)) {
        seen->insert(normalized);
        sources->append(normalized);
    }
}

QString candidateNamed(const QStringList &candidatePaths, const QString &name)
{
    for (const QString &path : candidatePaths) {
        const QString normalized = normalizedAbsolutePath(path);
        if (QFileInfo(normalized).fileName() == name) {
            return normalized;
        }
    }
    return {};
}

bool exactObjectKeys(const QJsonObject &object,
                     std::initializer_list<QLatin1StringView> expected,
                     const QString &context,
                     QString *errorMessage)
{
    QSet<QString> expectedKeys;
    QStringList expectedOrder;
    expectedOrder.reserve(static_cast<qsizetype>(expected.size()));
    for (const QLatin1StringView key : expected) {
        const QString name = key.toString();
        expectedKeys.insert(name);
        expectedOrder.append(name);
    }
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!expectedKeys.contains(it.key())) {
            setError(errorMessage,
                     QStringLiteral("%1 has unexpected field '%2'")
                         .arg(context, it.key()));
            return false;
        }
    }
    for (const QString &key : expectedOrder) {
        if (!object.contains(key)) {
            setError(errorMessage,
                     QStringLiteral("%1 is missing field '%2'")
                         .arg(context, key));
            return false;
        }
    }
    return true;
}

bool unsignedJsonInteger(const QJsonValue &value,
                         quint64 maximum,
                         quint64 *destination)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0.0
        || std::trunc(number) != number
        || number > static_cast<double>(maximum)) {
        return false;
    }
    *destination = static_cast<quint64>(number);
    return true;
}

bool parseKeybindTrigger(const QJsonValue &value,
                         const QString &context,
                         GhosttyKeybindTrigger *destination,
                         QString *errorMessage)
{
    if (!value.isObject()) {
        setError(errorMessage,
                 QStringLiteral("%1 must be an object").arg(context));
        return false;
    }
    const QJsonObject object = value.toObject();
    const QJsonValue kindValue = object.value(QStringLiteral("kind"));
    if (!kindValue.isString()) {
        setError(errorMessage,
                 QStringLiteral("%1.kind must be a string").arg(context));
        return false;
    }

    const QString kind = kindValue.toString();
    bool keysValid = false;
    if (kind == QStringLiteral("physical")) {
        keysValid = exactObjectKeys(
            object,
            {QLatin1StringView("kind"), QLatin1StringView("key"),
             QLatin1StringView("mods")},
            context, errorMessage);
    } else if (kind == QStringLiteral("unicode")) {
        keysValid = exactObjectKeys(
            object,
            {QLatin1StringView("kind"), QLatin1StringView("codepoint"),
             QLatin1StringView("mods")},
            context, errorMessage);
    } else if (kind == QStringLiteral("catch_all")) {
        keysValid = exactObjectKeys(
            object,
            {QLatin1StringView("kind"), QLatin1StringView("mods")},
            context, errorMessage);
    } else {
        setError(errorMessage,
                 QStringLiteral("%1.kind has unsupported value '%2'")
                     .arg(context, kind));
        return false;
    }
    if (!keysValid) return false;

    quint64 modifiers = 0;
    if (!unsignedJsonInteger(object.value(QStringLiteral("mods")),
                             GhosttyKeybindShift | GhosttyKeybindCtrl
                                 | GhosttyKeybindAlt | GhosttyKeybindSuper,
                             &modifiers)) {
        setError(errorMessage,
                 QStringLiteral("%1.mods must be an integer in [0, 15]")
                     .arg(context));
        return false;
    }

    GhosttyKeybindTrigger parsed;
    parsed.modifiers = static_cast<quint8>(modifiers);
    if (kind == QStringLiteral("physical")) {
        const QJsonValue key = object.value(QStringLiteral("key"));
        if (!key.isString() || key.toString().isEmpty()) {
            setError(errorMessage,
                     QStringLiteral("%1.key must be a non-empty string")
                         .arg(context));
            return false;
        }
        parsed.kind = GhosttyKeybindKeyKind::Physical;
        parsed.physicalName = key.toString();
    } else if (kind == QStringLiteral("unicode")) {
        quint64 codepoint = 0;
        if (!unsignedJsonInteger(object.value(QStringLiteral("codepoint")),
                                 0x10ffffU, &codepoint)
            || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            setError(errorMessage,
                     QStringLiteral("%1.codepoint must be a Unicode scalar")
                         .arg(context));
            return false;
        }
        parsed.kind = GhosttyKeybindKeyKind::Unicode;
        parsed.unicodeCodepoint = static_cast<quint32>(codepoint);
    } else {
        parsed.kind = GhosttyKeybindKeyKind::CatchAll;
    }

    *destination = std::move(parsed);
    return true;
}

bool parseKeybindFlags(const QJsonValue &value,
                       const QString &context,
                       GhosttyKeybindFlags *destination,
                       QString *errorMessage)
{
    if (!value.isObject()) {
        setError(errorMessage,
                 QStringLiteral("%1 must be an object").arg(context));
        return false;
    }
    const QJsonObject object = value.toObject();
    if (!exactObjectKeys(object,
                         {QLatin1StringView("consumed"),
                          QLatin1StringView("all"),
                          QLatin1StringView("global"),
                          QLatin1StringView("performable")},
                         context, errorMessage)) {
        return false;
    }

    const std::array<QString, 4> names{
        QStringLiteral("consumed"), QStringLiteral("all"),
        QStringLiteral("global"), QStringLiteral("performable")};
    for (const QString &name : names) {
        if (!object.value(name).isBool()) {
            setError(errorMessage,
                     QStringLiteral("%1.%2 must be a boolean")
                         .arg(context, name));
            return false;
        }
    }
    *destination = {
        .consumed = object.value(QStringLiteral("consumed")).toBool(),
        .all = object.value(QStringLiteral("all")).toBool(),
        .global = object.value(QStringLiteral("global")).toBool(),
        .performable = object.value(QStringLiteral("performable")).toBool(),
    };
    return true;
}

bool parseKeybindDefinition(const QJsonValue &value,
                            const QString &context,
                            GhosttyKeybindDefinition *destination,
                            QString *errorMessage)
{
    if (!value.isObject()) {
        setError(errorMessage,
                 QStringLiteral("%1 must be an object").arg(context));
        return false;
    }
    const QJsonObject object = value.toObject();
    if (!exactObjectKeys(object,
                         {QLatin1StringView("sequence"),
                          QLatin1StringView("actions"),
                          QLatin1StringView("flags")},
                         context, errorMessage)) {
        return false;
    }
    const QJsonValue sequenceValue = object.value(QStringLiteral("sequence"));
    if (!sequenceValue.isArray() || sequenceValue.toArray().isEmpty()) {
        setError(errorMessage,
                 QStringLiteral("%1.sequence must be a non-empty array")
                     .arg(context));
        return false;
    }
    const QJsonValue actionsValue = object.value(QStringLiteral("actions"));
    if (!actionsValue.isArray() || actionsValue.toArray().isEmpty()) {
        setError(errorMessage,
                 QStringLiteral("%1.actions must be a non-empty array")
                     .arg(context));
        return false;
    }

    GhosttyKeybindDefinition parsed;
    const QJsonArray sequence = sequenceValue.toArray();
    parsed.sequence.reserve(sequence.size());
    for (qsizetype index = 0; index < sequence.size(); ++index) {
        GhosttyKeybindTrigger trigger;
        if (!parseKeybindTrigger(
                sequence.at(index),
                QStringLiteral("%1.sequence[%2]").arg(context).arg(index),
                &trigger, errorMessage)) {
            return false;
        }
        parsed.sequence.append(std::move(trigger));
    }

    const QJsonArray actions = actionsValue.toArray();
    parsed.actions.reserve(actions.size());
    for (qsizetype index = 0; index < actions.size(); ++index) {
        const QJsonValue action = actions.at(index);
        if (!action.isString() || action.toString().isEmpty()) {
            setError(errorMessage,
                     QStringLiteral("%1.actions[%2] must be a non-empty string")
                         .arg(context)
                         .arg(index));
            return false;
        }
        parsed.actions.append(action.toString());
    }
    if (!parseKeybindFlags(object.value(QStringLiteral("flags")),
                           context + QStringLiteral(".flags"), &parsed.flags,
                           errorMessage)) {
        return false;
    }
    *destination = std::move(parsed);
    return true;
}

bool parseKeybindDefinitions(const QJsonValue &value,
                             const QString &context,
                             QVector<GhosttyKeybindDefinition> *destination,
                             QString *errorMessage)
{
    if (!value.isArray()) {
        setError(errorMessage,
                 QStringLiteral("%1 must be an array").arg(context));
        return false;
    }
    const QJsonArray array = value.toArray();
    QVector<GhosttyKeybindDefinition> parsed;
    parsed.reserve(array.size());
    for (qsizetype index = 0; index < array.size(); ++index) {
        GhosttyKeybindDefinition definition;
        if (!parseKeybindDefinition(
                array.at(index),
                QStringLiteral("%1[%2]").arg(context).arg(index),
                &definition, errorMessage)) {
            return false;
        }
        parsed.append(std::move(definition));
    }
    *destination = std::move(parsed);
    return true;
}

} // namespace

std::expected<GhosttyConfigExport, QString>
parseGhosttyConfigExportJson(const QByteArray &json)
{
    QJsonParseError jsonError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &jsonError);
    if (jsonError.error != QJsonParseError::NoError) {
        return std::unexpected(
            QStringLiteral("Invalid Ghostty structured config JSON at offset %1: %2")
                .arg(jsonError.offset)
                .arg(jsonError.errorString()));
    }
    if (!document.isObject()) {
        return std::unexpected(
            QStringLiteral("Ghostty structured config JSON root must be an object"));
    }

    QString parseError;
    const QJsonObject object = document.object();
    if (!exactObjectKeys(object,
                         {QLatin1StringView("version"),
                          QLatin1StringView("application"),
                          QLatin1StringView("keybindings")},
                         QStringLiteral("Ghostty structured config JSON root"),
                         &parseError)) {
        return std::unexpected(std::move(parseError));
    }
    quint64 schemaVersion = 0;
    if (!unsignedJsonInteger(object.value(QStringLiteral("version")),
                             std::numeric_limits<int>::max(),
                             &schemaVersion)
        || schemaVersion != GhosttyConfigExport::CurrentSchemaVersion) {
        return std::unexpected(
            QStringLiteral("Unsupported Ghostty structured config JSON schema version"));
    }

    GhosttyConfigExport parsed;
    parsed.schemaVersion = static_cast<int>(schemaVersion);

    const QJsonValue applicationValue =
        object.value(QStringLiteral("application"));
    if (!applicationValue.isObject()) {
        return std::unexpected(QStringLiteral("application must be an object"));
    }
    const QJsonObject application = applicationValue.toObject();
    if (!exactObjectKeys(
            application,
            {QLatin1StringView("quit-after-last-window-closed"),
             QLatin1StringView("quit-after-last-window-closed-delay-ms"),
             QLatin1StringView("initial-window"),
             QLatin1StringView("resize-overlay"),
             QLatin1StringView("resize-overlay-position"),
             QLatin1StringView("resize-overlay-duration-ms"),
             QLatin1StringView("gtk-single-instance")},
            QStringLiteral("application"), &parseError)) {
        return std::unexpected(std::move(parseError));
    }
    const QJsonValue quitAfterLastWindow = application.value(
        QStringLiteral("quit-after-last-window-closed"));
    if (!quitAfterLastWindow.isBool()) {
        return std::unexpected(QStringLiteral(
            "application.quit-after-last-window-closed must be a boolean"));
    }
    parsed.quitAfterLastWindowClosed = quitAfterLastWindow.toBool();

    const QJsonValue delay = application.value(
        QStringLiteral("quit-after-last-window-closed-delay-ms"));
    if (!delay.isNull()) {
        quint64 milliseconds = 0;
        if (!unsignedJsonInteger(delay, std::numeric_limits<quint32>::max(),
                                 &milliseconds)) {
            return std::unexpected(QStringLiteral(
                "application.quit-after-last-window-closed-delay-ms must be null or a uint32 integer"));
        }
        parsed.quitAfterLastWindowClosedDelayMilliseconds =
            static_cast<quint32>(milliseconds);
    }
    const QJsonValue initialWindow = application.value(
        QStringLiteral("initial-window"));
    if (!initialWindow.isBool()) {
        return std::unexpected(QStringLiteral(
            "application.initial-window must be a boolean"));
    }
    parsed.initialWindow = initialWindow.toBool();

    const QJsonValue resizeOverlay = application.value(
        QStringLiteral("resize-overlay"));
    const QString resizeOverlayMode = resizeOverlay.toString();
    if (!resizeOverlay.isString()
        || (resizeOverlayMode != QStringLiteral("always")
            && resizeOverlayMode != QStringLiteral("never")
            && resizeOverlayMode != QStringLiteral("after-first"))) {
        return std::unexpected(QStringLiteral(
            "application.resize-overlay must be always, never, or after-first"));
    }
    parsed.resizeOverlayMode = resizeOverlayMode;

    const QJsonValue resizeOverlayPosition = application.value(
        QStringLiteral("resize-overlay-position"));
    const QString position = resizeOverlayPosition.toString();
    static const QSet<QString> resizeOverlayPositions{
        QStringLiteral("center"),
        QStringLiteral("top-left"),
        QStringLiteral("top-center"),
        QStringLiteral("top-right"),
        QStringLiteral("bottom-left"),
        QStringLiteral("bottom-center"),
        QStringLiteral("bottom-right"),
    };
    if (!resizeOverlayPosition.isString()
        || !resizeOverlayPositions.contains(position)) {
        return std::unexpected(QStringLiteral(
            "application.resize-overlay-position must be a supported position"));
    }
    parsed.resizeOverlayPosition = position;

    const QJsonValue resizeOverlayDuration = application.value(
        QStringLiteral("resize-overlay-duration-ms"));
    quint64 resizeOverlayMilliseconds = 0;
    if (!unsignedJsonInteger(
            resizeOverlayDuration, std::numeric_limits<quint32>::max(),
            &resizeOverlayMilliseconds)) {
        return std::unexpected(QStringLiteral(
            "application.resize-overlay-duration-ms must be a uint32 integer"));
    }
    parsed.resizeOverlayDurationMilliseconds =
        static_cast<quint32>(resizeOverlayMilliseconds);

    const QJsonValue singleInstance = application.value(
        QStringLiteral("gtk-single-instance"));
    const QString singleInstanceMode = singleInstance.toString();
    if (!singleInstance.isString()
        || (singleInstanceMode != QStringLiteral("false")
            && singleInstanceMode != QStringLiteral("true")
            && singleInstanceMode != QStringLiteral("detect"))) {
        return std::unexpected(QStringLiteral(
            "application.gtk-single-instance must be false, true, or detect"));
    }
    parsed.singleInstanceMode = singleInstanceMode;

    const QJsonValue keybindingsValue =
        object.value(QStringLiteral("keybindings"));
    if (!keybindingsValue.isObject()) {
        return std::unexpected(QStringLiteral("keybindings must be an object"));
    }
    const QJsonObject keybindings = keybindingsValue.toObject();
    if (!exactObjectKeys(keybindings,
                         {QLatin1StringView("root"),
                          QLatin1StringView("tables")},
                         QStringLiteral("keybindings"), &parseError)) {
        return std::unexpected(std::move(parseError));
    }
    // The envelope version is the sole wire-schema authority.
    parsed.keybindings.schemaVersion = parsed.schemaVersion;
    if (!parseKeybindDefinitions(keybindings.value(QStringLiteral("root")),
                                 QStringLiteral("keybindings.root"),
                                 &parsed.keybindings.root,
                                 &parseError)) {
        return std::unexpected(std::move(parseError));
    }

    const QJsonValue tablesValue = keybindings.value(QStringLiteral("tables"));
    if (!tablesValue.isArray()) {
        return std::unexpected(
            QStringLiteral("keybindings.tables must be an array"));
    }
    const QJsonArray tables = tablesValue.toArray();
    parsed.keybindings.tables.reserve(tables.size());
    QSet<QString> tableNames;
    for (qsizetype index = 0; index < tables.size(); ++index) {
        const QString context =
            QStringLiteral("keybindings.tables[%1]").arg(index);
        if (!tables.at(index).isObject()) {
            return std::unexpected(
                QStringLiteral("%1 must be an object").arg(context));
        }
        const QJsonObject table = tables.at(index).toObject();
        if (!exactObjectKeys(table,
                             {QLatin1StringView("name"),
                              QLatin1StringView("bindings")},
                             context, &parseError)) {
            return std::unexpected(std::move(parseError));
        }
        const QJsonValue nameValue = table.value(QStringLiteral("name"));
        if (!nameValue.isString() || nameValue.toString().isEmpty()) {
            return std::unexpected(
                QStringLiteral("%1.name must be a non-empty string")
                    .arg(context));
        }
        const QString name = nameValue.toString();
        if (tableNames.contains(name)) {
            return std::unexpected(
                QStringLiteral("Duplicate Ghostty keybinding table '%1'")
                    .arg(name));
        }
        tableNames.insert(name);
        GhosttyKeybindTable parsedTable{
            .name = name,
            .bindings = {},
        };
        if (!parseKeybindDefinitions(table.value(QStringLiteral("bindings")),
                                     context + QStringLiteral(".bindings"),
                                     &parsedTable.bindings, &parseError)) {
            return std::unexpected(std::move(parseError));
        }
        parsed.keybindings.tables.append(std::move(parsedTable));
    }

    return parsed;
}

std::expected<QString, QString> ghosttyConfigXdgHome(
    const QStringList &candidatePaths)
{
    const QString legacyPath =
        candidateNamed(candidatePaths, QString::fromLatin1(LegacyConfigName));
    const QString preferredPath =
        candidateNamed(candidatePaths, QString::fromLatin1(PreferredConfigName));
    if (legacyPath.isEmpty() || preferredPath.isEmpty()) {
        return std::unexpected(
            QStringLiteral("Ghostty config candidates must contain both config and config.ghostty"));
    }

    const QString legacyDirectory = QFileInfo(legacyPath).absolutePath();
    const QString preferredDirectory = QFileInfo(preferredPath).absolutePath();
    if (legacyDirectory != preferredDirectory
        || QFileInfo(legacyDirectory).fileName() != QStringLiteral("ghostty")) {
        return std::unexpected(
            QStringLiteral("Ghostty config candidates must share one XDG ghostty directory"));
    }

    return QDir::cleanPath(QFileInfo(legacyDirectory).absolutePath());
}

GhosttyConfigLoadResult parseGhosttyConfigShowOutputs(
    const QByteArray &defaultOutput,
    const QByteArray &changesOutput,
    const QStringList &candidatePaths)
{
    ParsedConfig defaults;
    QString parseError;
    if (!parseDump(defaultOutput, &defaults, &parseError)) {
        return std::unexpected(std::move(parseError));
    }

    if (!hasRequiredFields(defaults)) {
        return std::unexpected(
            QStringLiteral("Ghostty default config output is missing a required compatibility key"));
    }

    ParsedConfig changes;
    if (!parseDump(changesOutput, &changes, &parseError)) {
        return std::unexpected(std::move(parseError));
    }

    const QString workingDirectory =
        changes.workingDirectory.value_or(*defaults.workingDirectory);
    const QStringList fontFamilies =
        changes.fontFamilies.value_or(*defaults.fontFamilies);
    const double fontSize = changes.fontSize.value_or(*defaults.fontSize);
    const QColor foreground =
        changes.foreground.value_or(*defaults.foreground);
    const QColor background =
        changes.background.value_or(*defaults.background);
    const double unfocusedSplitOpacity =
        changes.unfocusedSplitOpacity.value_or(
            *defaults.unfocusedSplitOpacity);
    const QVariant unfocusedSplitFill =
        changes.unfocusedSplitFill.value_or(*defaults.unfocusedSplitFill);
    const QVariant splitDividerColor = changes.splitDividerColor.value_or(
        *defaults.splitDividerColor);
    const bool splitInheritWorkingDirectory =
        changes.splitInheritWorkingDirectory.value_or(
            *defaults.splitInheritWorkingDirectory);
    const bool splitPreserveZoomNavigation =
        changes.splitPreserveZoomNavigation.value_or(
            *defaults.splitPreserveZoomNavigation);
    const bool tabInheritWorkingDirectory =
        changes.tabInheritWorkingDirectory.value_or(
            *defaults.tabInheritWorkingDirectory);
    const bool windowInheritWorkingDirectory =
        changes.windowInheritWorkingDirectory.value_or(
            *defaults.windowInheritWorkingDirectory);
    const bool windowInheritFontSize = changes.windowInheritFontSize.value_or(
        *defaults.windowInheritFontSize);
    const QString windowNewTabPosition =
        changes.windowNewTabPosition.value_or(
            *defaults.windowNewTabPosition);
    const QString windowShowTabBar = changes.windowShowTabBar.value_or(
        *defaults.windowShowTabBar);
    const quint32 windowWidth =
        changes.windowWidth.value_or(*defaults.windowWidth);
    const quint32 windowHeight =
        changes.windowHeight.value_or(*defaults.windowHeight);
    const bool maximize = changes.maximize.value_or(*defaults.maximize);
    const QString fullscreen =
        changes.fullscreen.value_or(*defaults.fullscreen);
    SparsePalette palette = defaults.palette;
    for (std::size_t index = 0; index < palette.size(); ++index) {
        if (changes.palette[index].has_value()) {
            palette[index] = changes.palette[index];
        }
    }
    QVariantList paletteValues;
    paletteValues.reserve(static_cast<qsizetype>(palette.size()));
    for (const std::optional<QColor> &color : palette) {
        if (!color.has_value()) {
            return std::unexpected(
                QStringLiteral("Ghostty default config output is missing a required compatibility key"));
        }
        paletteValues.append(*color);
    }
    const QVariant selectionForeground = changes.selectionForeground.value_or(
        *defaults.selectionForeground);
    const QVariant selectionBackground = changes.selectionBackground.value_or(
        *defaults.selectionBackground);
    const QVariant searchForeground =
        changes.searchForeground.value_or(*defaults.searchForeground);
    const QVariant searchBackground =
        changes.searchBackground.value_or(*defaults.searchBackground);
    const QVariant searchSelectedForeground =
        changes.searchSelectedForeground.value_or(
            *defaults.searchSelectedForeground);
    const QVariant searchSelectedBackground =
        changes.searchSelectedBackground.value_or(
            *defaults.searchSelectedBackground);
    const QVariant cursorColor =
        changes.cursorColor.value_or(*defaults.cursorColor);
    const double cursorOpacity =
        changes.cursorOpacity.value_or(*defaults.cursorOpacity);
    const QString cursorStyle =
        changes.cursorStyle.value_or(*defaults.cursorStyle);
    const QVariant cursorStyleBlink = changes.cursorStyleBlink.value_or(
        *defaults.cursorStyleBlink);
    const QVariant cursorText =
        changes.cursorText.value_or(*defaults.cursorText);
    const QVariant boldColor =
        changes.boldColor.value_or(*defaults.boldColor);
    const double faintOpacity =
        changes.faintOpacity.value_or(*defaults.faintOpacity);
    const quint64 scrollbackLimit =
        changes.scrollbackLimit.value_or(*defaults.scrollbackLimit);
    const QString confirmCloseSurface = changes.confirmCloseSurface.value_or(
        *defaults.confirmCloseSurface);
    const bool clipboardTrimTrailingSpaces =
        changes.clipboardTrimTrailingSpaces.value_or(
            *defaults.clipboardTrimTrailingSpaces);
    const bool clipboardPasteProtection =
        changes.clipboardPasteProtection.value_or(
            *defaults.clipboardPasteProtection);
    const bool clipboardPasteBracketedSafe =
        changes.clipboardPasteBracketedSafe.value_or(
            *defaults.clipboardPasteBracketedSafe);
    const QString copyOnSelect =
        changes.copyOnSelect.value_or(*defaults.copyOnSelect);
    const bool selectionClearOnTyping =
        changes.selectionClearOnTyping.value_or(
            *defaults.selectionClearOnTyping);
    const bool selectionClearOnCopy =
        changes.selectionClearOnCopy.value_or(
            *defaults.selectionClearOnCopy);
    const QString middleClickAction =
        changes.middleClickAction.value_or(*defaults.middleClickAction);
    const bool mouseReporting =
        changes.mouseReporting.value_or(*defaults.mouseReporting);
    const bool linkUrl = changes.linkUrl.value_or(*defaults.linkUrl);
    const QString linkPreviews =
        changes.linkPreviews.value_or(*defaults.linkPreviews);
    const QStringList keybinds =
        changes.keybinds.value_or(*defaults.keybinds);
    const QStringList configFiles =
        changes.configFiles.value_or(*defaults.configFiles);

    GhosttyConfigSnapshot snapshot;
    snapshot.availability = GhosttyConfigAvailability::Available;
    snapshot.values.insert(QStringLiteral("working-directory"),
                           workingDirectory);
    snapshot.values.insert(QStringLiteral("font-family"), fontFamilies);
    snapshot.values.insert(QStringLiteral("font-size"), fontSize);
    snapshot.values.insert(QStringLiteral("foreground"), foreground);
    snapshot.values.insert(QStringLiteral("background"), background);
    snapshot.values.insert(QStringLiteral("unfocused-split-opacity"),
                           unfocusedSplitOpacity);
    snapshot.values.insert(QStringLiteral("unfocused-split-fill"),
                           unfocusedSplitFill);
    snapshot.values.insert(QStringLiteral("split-divider-color"),
                           splitDividerColor);
    snapshot.values.insert(QStringLiteral("split-inherit-working-directory"),
                           splitInheritWorkingDirectory);
    snapshot.values.insert(QStringLiteral("split-preserve-zoom"),
                           splitPreserveZoomNavigation);
    snapshot.values.insert(QStringLiteral("tab-inherit-working-directory"),
                           tabInheritWorkingDirectory);
    snapshot.values.insert(
        QStringLiteral("window-inherit-working-directory"),
        windowInheritWorkingDirectory);
    snapshot.values.insert(QStringLiteral("window-inherit-font-size"),
                           windowInheritFontSize);
    snapshot.values.insert(QStringLiteral("window-new-tab-position"),
                           windowNewTabPosition);
    snapshot.values.insert(QStringLiteral("window-show-tab-bar"),
                           windowShowTabBar);
    snapshot.values.insert(QStringLiteral("window-width"), windowWidth);
    snapshot.values.insert(QStringLiteral("window-height"), windowHeight);
    snapshot.values.insert(QStringLiteral("maximize"), maximize);
    snapshot.values.insert(QStringLiteral("fullscreen"), fullscreen);
    snapshot.values.insert(QStringLiteral("palette"), paletteValues);
    snapshot.values.insert(QStringLiteral("selection-foreground"),
                           selectionForeground);
    snapshot.values.insert(QStringLiteral("selection-background"),
                           selectionBackground);
    snapshot.values.insert(QStringLiteral("search-foreground"),
                           searchForeground);
    snapshot.values.insert(QStringLiteral("search-background"),
                           searchBackground);
    snapshot.values.insert(QStringLiteral("search-selected-foreground"),
                           searchSelectedForeground);
    snapshot.values.insert(QStringLiteral("search-selected-background"),
                           searchSelectedBackground);
    snapshot.values.insert(QStringLiteral("cursor-color"), cursorColor);
    snapshot.values.insert(QStringLiteral("cursor-opacity"), cursorOpacity);
    snapshot.values.insert(QStringLiteral("cursor-style"), cursorStyle);
    snapshot.values.insert(QStringLiteral("cursor-style-blink"),
                           cursorStyleBlink);
    snapshot.values.insert(QStringLiteral("cursor-text"), cursorText);
    snapshot.values.insert(QStringLiteral("bold-color"), boldColor);
    snapshot.values.insert(QStringLiteral("faint-opacity"), faintOpacity);
    snapshot.values.insert(QStringLiteral("scrollback-limit"), scrollbackLimit);
    snapshot.values.insert(QStringLiteral("confirm-close-surface"),
                           confirmCloseSurface);
    snapshot.values.insert(QStringLiteral("clipboard-trim-trailing-spaces"),
                           clipboardTrimTrailingSpaces);
    snapshot.values.insert(QStringLiteral("clipboard-paste-protection"),
                           clipboardPasteProtection);
    snapshot.values.insert(QStringLiteral("clipboard-paste-bracketed-safe"),
                           clipboardPasteBracketedSafe);
    snapshot.values.insert(QStringLiteral("copy-on-select"), copyOnSelect);
    snapshot.values.insert(QStringLiteral("selection-clear-on-typing"),
                           selectionClearOnTyping);
    snapshot.values.insert(QStringLiteral("selection-clear-on-copy"),
                           selectionClearOnCopy);
    snapshot.values.insert(QStringLiteral("middle-click-action"),
                           middleClickAction);
    snapshot.values.insert(QStringLiteral("mouse-reporting"), mouseReporting);
    snapshot.values.insert(QStringLiteral("link-url"), linkUrl);
    snapshot.values.insert(QStringLiteral("link-previews"), linkPreviews);
    snapshot.values.insert(QStringLiteral("keybind"), keybinds);
    snapshot.values.insert(QStringLiteral("config-file"), configFiles);
    if (changes.keybinds.has_value()) {
        QStringList changedKeybinds;
        for (const QString &keybind : keybinds) {
            if (!defaults.keybinds->contains(keybind)) {
                changedKeybinds.append(keybind);
            }
        }
        appendKeybindCompatibilityDiagnostics(&snapshot, changedKeybinds);
    }

    QSet<QString> seenSources;
    // Ghostty loads the legacy file before config.ghostty regardless of which
    // one supplied the XDG root. Preserve that order for diagnostics/watchers.
    appendExistingSource(&snapshot.sourcePaths, &seenSources,
                         candidateNamed(candidatePaths,
                                        QString::fromLatin1(LegacyConfigName)));
    appendExistingSource(&snapshot.sourcePaths, &seenSources,
                         candidateNamed(candidatePaths,
                                        QString::fromLatin1(PreferredConfigName)));
    for (const QString &path : configFiles) {
        appendExistingSource(&snapshot.sourcePaths, &seenSources, path);
    }

    return snapshot;
}

GhosttyConfigLoader makeGhosttyConfigProcessLoader(
    GhosttyConfigProcessLoaderOptions options)
{
    return [options = std::move(options)](
               const QStringList &candidatePaths) -> GhosttyConfigLoadResult {
        if (options.helperPath.isEmpty()) {
            return std::unexpected(
                QStringLiteral("Ghostty config helper path is empty"));
        }

        auto xdgConfigHomeResult = ghosttyConfigXdgHome(candidatePaths);
        if (!xdgConfigHomeResult.has_value()) {
            return std::unexpected(std::move(xdgConfigHomeResult.error()));
        }
        const QString xdgConfigHome = std::move(*xdgConfigHomeResult);

        QDeadlineTimer overallDeadline(
            std::max(1, options.overallTimeoutMilliseconds));
        int operationTimeout = std::max(1, options.timeoutMilliseconds);
        const auto run = [&](const QStringList &arguments) {
            const qint64 remaining = overallDeadline.remainingTime();
            operationTimeout = static_cast<int>(std::clamp<qint64>(
                remaining, 1, std::max(1, options.timeoutMilliseconds)));
            return runHelper(options, arguments, xdgConfigHome,
                             operationTimeout);
        };

        const ProcessResult validation =
            run({QStringLiteral("+validate-config")});
        if (validation.status != ProcessResult::Status::Completed
            || validation.exitCode != 0) {
            return processFailure(QStringLiteral("validation"), validation,
                                  operationTimeout);
        }

        const ProcessResult defaults =
            run({QStringLiteral("+show-config"),
                 QStringLiteral("--default")});
        if (defaults.status != ProcessResult::Status::Completed
            || defaults.exitCode != 0) {
            return processFailure(QStringLiteral("default config query"), defaults,
                                  operationTimeout);
        }

        // QProcess stdout is a pipe, not a terminal, so Ghostty's pager remains
        // inactive without passing the compatibility-noise `--no-pager` flag.
        const ProcessResult changes =
            run({QStringLiteral("+show-config")});
        if (changes.status != ProcessResult::Status::Completed
            || changes.exitCode != 0) {
            return processFailure(QStringLiteral("current config query"), changes,
                                  operationTimeout);
        }

        // Unlike +show-config's lossy formatter, this project-specific action
        // exports one versioned generation containing exact keybindings and
        // application values such as nullable durations.
        const ProcessResult structuredConfig =
            run({QStringLiteral("+show-config-json")});
        if (structuredConfig.status != ProcessResult::Status::Completed
            || structuredConfig.exitCode != 0) {
            return processFailure(QStringLiteral("structured config query"),
                                  structuredConfig, operationTimeout);
        }

        // `+show-config` deliberately prints even when its independently
        // loaded config contains diagnostics. Validate again after all three
        // queries so a concurrently edited invalid file cannot replace the
        // last-good application snapshot.
        const ProcessResult postQueryValidation =
            run({QStringLiteral("+validate-config")});
        if (postQueryValidation.status != ProcessResult::Status::Completed
            || postQueryValidation.exitCode != 0) {
            return processFailure(QStringLiteral("post-query validation"),
                                  postQueryValidation,
                                  operationTimeout);
        }

        // Validation alone cannot detect a valid A -> valid B edit between
        // the independent helper processes. Re-read both config-dependent
        // representations and accept only a stable pair. A later filesystem
        // notification (or the service retry after this failure) will load the
        // new generation without ever publishing a mixed snapshot.
        const ProcessResult verifiedChanges =
            run({QStringLiteral("+show-config")});
        if (verifiedChanges.status != ProcessResult::Status::Completed
            || verifiedChanges.exitCode != 0) {
            return processFailure(
                QStringLiteral("config consistency query"), verifiedChanges,
                operationTimeout);
        }
        const ProcessResult verifiedStructuredConfig =
            run({QStringLiteral("+show-config-json")});
        if (verifiedStructuredConfig.status != ProcessResult::Status::Completed
            || verifiedStructuredConfig.exitCode != 0) {
            return processFailure(
                QStringLiteral("structured config consistency query"),
                verifiedStructuredConfig, operationTimeout);
        }
        if (changes.standardOutput != verifiedChanges.standardOutput
            || structuredConfig.standardOutput
                != verifiedStructuredConfig.standardOutput) {
            return std::unexpected(QStringLiteral(
                "Ghostty config changed while it was being queried; reload will retry"));
        }

        GhosttyConfigLoadResult parsed = parseGhosttyConfigShowOutputs(
            defaults.standardOutput, changes.standardOutput, candidatePaths);
        if (!parsed) {
            return parsed;
        }
        auto exportedConfig =
            parseGhosttyConfigExportJson(structuredConfig.standardOutput);
        if (!exportedConfig) {
            return std::unexpected(
                QStringLiteral("Ghostty structured config query returned malformed data: %1")
                    .arg(exportedConfig.error()));
        }
        parsed->keybindConfig = std::move(exportedConfig->keybindings);
        parsed->values.insert(
            QStringLiteral("quit-after-last-window-closed"),
            exportedConfig->quitAfterLastWindowClosed);
        parsed->values.insert(QStringLiteral("initial-window"),
                              exportedConfig->initialWindow);
        parsed->values.insert(QStringLiteral("resize-overlay"),
                              exportedConfig->resizeOverlayMode);
        parsed->values.insert(QStringLiteral("resize-overlay-position"),
                              exportedConfig->resizeOverlayPosition);
        parsed->values.insert(
            QStringLiteral("resize-overlay-duration"),
            exportedConfig->resizeOverlayDurationMilliseconds);
        parsed->values.insert(QStringLiteral("gtk-single-instance"),
                              exportedConfig->singleInstanceMode);
        if (exportedConfig->quitAfterLastWindowClosedDelayMilliseconds) {
            parsed->values.insert(
                QStringLiteral("quit-after-last-window-closed-delay"),
                *exportedConfig->quitAfterLastWindowClosedDelayMilliseconds);
        } else {
            // An invalid QVariant represents Ghostty's nullable unset value;
            // retaining the key lets a live reload clear an earlier delay.
            parsed->values.insert(
                QStringLiteral("quit-after-last-window-closed-delay"),
                QVariant{});
        }

        // Config values belong on stdout; any successful stderr and validator
        // stdout are warnings from the pinned parser and must remain visible
        // to callers instead of disappearing with the helper process.
        appendProcessDiagnostics(&*parsed,
                                 QStringLiteral("validation"),
                                 validation.standardOutput);
        appendProcessDiagnostics(&*parsed,
                                 QStringLiteral("validation"),
                                 validation.standardError);
        appendProcessDiagnostics(&*parsed,
                                 QStringLiteral("default query"),
                                 defaults.standardError);
        appendProcessDiagnostics(&*parsed,
                                 QStringLiteral("current query"),
                                 changes.standardError);
        appendProcessDiagnostics(&*parsed,
                                 QStringLiteral("structured config query"),
                                 structuredConfig.standardError);
        appendProcessDiagnostics(&*parsed,
                                 QStringLiteral("post-query validation"),
                                 postQueryValidation.standardOutput);
        appendProcessDiagnostics(&*parsed,
                                 QStringLiteral("post-query validation"),
                                 postQueryValidation.standardError);
        return parsed;
    };
}
