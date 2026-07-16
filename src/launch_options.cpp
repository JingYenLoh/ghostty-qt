#include "launch_options.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QMetaType>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

constexpr int kMaximumScrollbackLines = 10'000'000;

bool fail(QString *errorMessage, const QString &message)
{
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
    return false;
}

std::optional<QColor> configColor(const GhosttyConfigSnapshot &snapshot,
                                  const QString &key)
{
    const auto it = snapshot.values.constFind(key);
    if (it == snapshot.values.cend() || !it->isValid()) {
        return std::nullopt;
    }

    const QColor color = it->value<QColor>();
    return color.isValid() ? std::optional<QColor>(color) : std::nullopt;
}

std::optional<TerminalColorValue> configTerminalColor(
    const GhosttyConfigSnapshot &snapshot, const QString &key)
{
    const auto it = snapshot.values.constFind(key);
    if (it == snapshot.values.cend() || !it->isValid()) {
        return std::nullopt;
    }
    if (it->metaType() == QMetaType::fromType<QString>()) {
        const QString value = it->toString();
        if (value.isEmpty()) {
            return TerminalColorValue{};
        }
        if (value == QStringLiteral("cell-foreground")) {
            return TerminalColorValue{
                .kind = TerminalColorKind::CellForeground,
                .color = {},
            };
        }
        if (value == QStringLiteral("cell-background")) {
            return TerminalColorValue{
                .kind = TerminalColorKind::CellBackground,
                .color = {},
            };
        }
        return std::nullopt;
    }
    const QColor color = it->value<QColor>();
    if (!color.isValid()) {
        return std::nullopt;
    }
    return TerminalColorValue::fromColor(color);
}

std::optional<TerminalCursorStyle> configCursorStyle(const QVariant &value)
{
    if (value.metaType() != QMetaType::fromType<QString>()) {
        return std::nullopt;
    }
    const QString style = value.toString();
    if (style == QStringLiteral("block")) return TerminalCursorStyle::Block;
    if (style == QStringLiteral("bar")) return TerminalCursorStyle::Bar;
    if (style == QStringLiteral("underline")) return TerminalCursorStyle::Underline;
    if (style == QStringLiteral("block_hollow")) {
        return TerminalCursorStyle::BlockHollow;
    }
    return std::nullopt;
}

std::optional<TerminalBoldColor> configBoldColor(const QVariant &value)
{
    if (value.metaType() == QMetaType::fromType<QString>()) {
        const QString bold = value.toString();
        if (bold.isEmpty()) return TerminalBoldColor{};
        if (bold == QStringLiteral("bright")) {
            return TerminalBoldColor{
                .kind = TerminalBoldColorKind::Bright,
                .color = {},
            };
        }
        return std::nullopt;
    }
    const QColor color = value.value<QColor>();
    if (!color.isValid()) return std::nullopt;
    return TerminalBoldColor{
        .kind = TerminalBoldColorKind::Color,
        .color = color,
    };
}

std::optional<QVector<QColor>> configPalette(const QVariant &value)
{
    const QVariantList entries = value.toList();
    if (entries.size() != 256) return std::nullopt;
    QVector<QColor> palette;
    palette.reserve(entries.size());
    for (const QVariant &entry : entries) {
        const QColor color = entry.value<QColor>();
        if (!color.isValid()) return std::nullopt;
        palette.append(color);
    }
    return palette;
}

std::optional<double> configUnitInterval(const QVariant &value)
{
    bool valid = false;
    const double result = value.toDouble(&valid);
    if (!valid || !std::isfinite(result) || result < 0.0 || result > 1.0) {
        return std::nullopt;
    }
    return result;
}

std::optional<double> configClampedUnitInterval(const QVariant &value)
{
    bool valid = false;
    const double result = value.toDouble(&valid);
    if (!valid || !std::isfinite(result)) {
        return std::nullopt;
    }
    return std::clamp(result, 0.0, 1.0);
}

} // namespace

quint64 scrollbackLimitInBytes(ScrollbackLimit limit, int columns)
{
    if (limit.unit == ScrollbackLimitUnit::Bytes) {
        return limit.value;
    }
    const quint64 boundedColumns = static_cast<quint64>(std::max(1, columns));
    constexpr quint64 EstimatedBytesPerCell = 16;
    constexpr quint64 MinimumEstimatedRowBytes = 256;
    const quint64 rowBytes = std::max(
        MinimumEstimatedRowBytes,
        boundedColumns * EstimatedBytesPerCell);
    if (limit.value > std::numeric_limits<quint64>::max() / rowBytes) {
        return std::numeric_limits<quint64>::max();
    }
    return limit.value * rowBytes;
}

LaunchOptions applyGhosttyConfigSnapshot(const LaunchOptions &base,
                                         const GhosttyConfigSnapshot &snapshot)
{
    LaunchOptions result = base;
    if (snapshot.availability != GhosttyConfigAvailability::Available) {
        return result;
    }

    if (!base.fontFamilyExplicit) {
        const auto families = snapshot.value<QStringList>(QStringLiteral("font-family"));
        if (families.has_value() && !families->isEmpty()) {
            result.fontFamily = families->constFirst();
        }
    }

    if (!base.fontSizeExplicit) {
        const auto size = snapshot.value<double>(QStringLiteral("font-size"));
        if (size.has_value() && std::isfinite(*size) && *size > 0.0) {
            result.fontSize = *size;
        }
    }

    if (const auto foreground = configColor(
            snapshot, QStringLiteral("foreground"))) {
        result.appearance.foregroundColor = *foreground;
    }
    if (const auto background = configColor(
            snapshot, QStringLiteral("background"))) {
        result.appearance.backgroundColor = *background;
    }
    if (const auto palette = configPalette(
            snapshot.values.value(QStringLiteral("palette")))) {
        result.appearance.palette = *palette;
    }
    if (const auto selectionForeground = configTerminalColor(
            snapshot, QStringLiteral("selection-foreground"))) {
        result.appearance.selectionForeground = *selectionForeground;
    }
    if (const auto selectionBackground = configTerminalColor(
            snapshot, QStringLiteral("selection-background"))) {
        result.appearance.selectionBackground = *selectionBackground;
    }
    if (const auto cursor = configTerminalColor(
            snapshot, QStringLiteral("cursor-color"))) {
        result.appearance.cursorColor = *cursor;
    }
    if (const auto cursorStyle = configCursorStyle(
            snapshot.values.value(QStringLiteral("cursor-style")))) {
        result.appearance.cursorStyle = *cursorStyle;
    }
    const QVariant cursorBlink =
        snapshot.values.value(QStringLiteral("cursor-style-blink"));
    if (cursorBlink.metaType() == QMetaType::fromType<bool>()) {
        result.appearance.cursorBlink = cursorBlink.toBool();
    } else if (cursorBlink.metaType() == QMetaType::fromType<QString>()
               && cursorBlink.toString().isEmpty()) {
        result.appearance.cursorBlink.reset();
    }
    if (const auto cursorOpacity = configClampedUnitInterval(
            snapshot.values.value(QStringLiteral("cursor-opacity")))) {
        result.appearance.cursorOpacity = *cursorOpacity;
    }
    if (const auto cursorText = configTerminalColor(
            snapshot, QStringLiteral("cursor-text"))) {
        result.appearance.cursorTextColor = *cursorText;
    }
    if (const auto boldColor = configBoldColor(
            snapshot.values.value(QStringLiteral("bold-color")))) {
        result.appearance.boldColor = *boldColor;
    }
    if (const auto faintOpacity = configUnitInterval(
            snapshot.values.value(QStringLiteral("faint-opacity")))) {
        result.appearance.faintOpacity = *faintOpacity;
    }

    const auto scrollback = snapshot.values.constFind(
        QStringLiteral("scrollback-limit"));
    if (!base.scrollbackLimitExplicit && scrollback != snapshot.values.cend()) {
        bool valid = false;
        const qulonglong byteLimit = scrollback->toULongLong(&valid);
        // QVariant converts signed -1 to UINT64_MAX successfully, so retain
        // the canonical sign check while still accepting Ghostty's full usize
        // range when the snapshot stores a quint64.
        const bool negative = scrollback->toString().startsWith(u'-');
        if (valid && !negative) {
            result.scrollbackLimit = {
                .value = static_cast<quint64>(byteLimit),
                .unit = ScrollbackLimitUnit::Bytes,
            };
        }
    }

    const auto confirm = snapshot.value<QString>(
        QStringLiteral("confirm-close-surface"));
    if (confirm == QStringLiteral("false")) {
        result.confirmCloseMode = ConfirmCloseMode::Never;
    } else if (confirm == QStringLiteral("true")) {
        result.confirmCloseMode = ConfirmCloseMode::RunningProcesses;
    } else if (confirm == QStringLiteral("always")) {
        result.confirmCloseMode = ConfirmCloseMode::Always;
    }

    const auto keybindings = snapshot.value<QStringList>(QStringLiteral("keybind"));
    if (keybindings.has_value()) {
        result.keybindings = *keybindings;
        result.keybindingsConfigured = true;
    }

    return result;
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

bool parseLaunchOptions(const QStringList &arguments, LaunchOptions *options,
                        QString *errorMessage)
{
    if (options == nullptr) {
        return fail(errorMessage, QStringLiteral("No output options object was provided."));
    }
    if (arguments.isEmpty()) {
        return fail(errorMessage, QStringLiteral("The argument list must include the application name."));
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
    const QCommandLineOption helpOption(
        {QStringLiteral("h"), QStringLiteral("help")},
        QStringLiteral("Show command-line help."));
    const QCommandLineOption versionOption(
        {QStringLiteral("v"), QStringLiteral("version")},
        QStringLiteral("Show version information."));

    parser.addOptions({workingDirectoryOption, fontFamilyOption, fontSizeOption,
                       scrollbackLinesOption, holdOption, helpOption, versionOption});
    parser.addPositionalArgument(
        QStringLiteral("program"),
        QStringLiteral("Program and arguments to execute after --."),
        QStringLiteral("[program [arguments...]]"));

    if (!parser.parse(arguments)) {
        return fail(errorMessage, parser.errorText());
    }

    LaunchOptions parsed;
    parsed.workingDirectory = QDir::currentPath();

    if (parser.isSet(workingDirectoryOption)) {
        const QString directory = QDir::cleanPath(parser.value(workingDirectoryOption));
        const QFileInfo directoryInfo(directory);
        if (!directoryInfo.exists() || !directoryInfo.isDir()) {
            return fail(errorMessage,
                        QStringLiteral("Working directory does not exist or is not a directory: %1")
                            .arg(directory));
        }
        parsed.workingDirectory = directory;
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
            return fail(errorMessage,
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
            return fail(
                errorMessage,
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

    *options = std::move(parsed);
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

bool parseLaunchOptions(QCoreApplication &application, LaunchOptions *options,
                        QString *errorMessage)
{
    return parseLaunchOptions(application.arguments(), options, errorMessage);
}
