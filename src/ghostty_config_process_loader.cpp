#include "ghostty_config_process_loader.h"
#include "ghostty_action_catalog.h"
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

struct ParsedConfig {
    bool hasFontFamilies = false;
    QStringList fontFamilies;
    bool hasFontSize = false;
    double fontSize = 0.0;
    bool hasForeground = false;
    QColor foreground;
    bool hasBackground = false;
    QColor background;
    bool hasPalette = false;
    std::array<std::optional<QColor>, 256> palette;
    bool hasSelectionForeground = false;
    QVariant selectionForeground;
    bool hasSelectionBackground = false;
    QVariant selectionBackground;
    bool hasCursorColor = false;
    QVariant cursorColor;
    bool hasCursorOpacity = false;
    double cursorOpacity = 1.0;
    bool hasCursorStyle = false;
    QString cursorStyle;
    bool hasCursorStyleBlink = false;
    QVariant cursorStyleBlink;
    bool hasCursorText = false;
    QVariant cursorText;
    bool hasBoldColor = false;
    QVariant boldColor;
    bool hasFaintOpacity = false;
    double faintOpacity = 0.5;
    bool hasScrollbackLimit = false;
    quint64 scrollbackLimit = 0;
    bool hasConfirmCloseSurface = false;
    QString confirmCloseSurface;
    bool hasKeybinds = false;
    QStringList keybinds;
    bool hasConfigFiles = false;
    QStringList configFiles;
};

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
        return GhosttyConfigLoadResult::failed(
            QStringLiteral("Ghostty config helper could not be started during %1")
                .arg(operation));
    case ProcessResult::Status::TimedOut:
        return GhosttyConfigLoadResult::failed(
            QStringLiteral("Ghostty config helper timed out during %1 after %2 ms")
                .arg(operation)
                .arg(std::max(1, timeoutMilliseconds)));
    case ProcessResult::Status::Crashed:
        return GhosttyConfigLoadResult::failed(
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
    return GhosttyConfigLoadResult::failed(std::move(message));
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
            "named key tables require the structured binding export");
    case GhosttyKeybindUnsupportedReason::NonLocal:
        return QStringLiteral(
            "all-surface/global triggers require the structured binding export");
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
        if (key == QStringLiteral("font-family")) {
            if (!parsed->hasFontFamilies) {
                parsed->fontFamilies.clear();
                parsed->hasFontFamilies = true;
            }
            if (value.isEmpty()) {
                parsed->fontFamilies.clear();
            } else {
                parsed->fontFamilies.append(value);
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
            parsed->hasFontSize = true;
            parsed->fontSize = fontSize;
        } else if (key == QStringLiteral("foreground")) {
            if (!parseColor(value, &parsed->foreground)) {
                setError(errorMessage,
                         QStringLiteral("Invalid foreground in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->hasForeground = true;
        } else if (key == QStringLiteral("background")) {
            if (!parseColor(value, &parsed->background)) {
                setError(errorMessage,
                         QStringLiteral("Invalid background in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->hasBackground = true;
        } else if (key == QStringLiteral("palette")) {
            int index = 0;
            QColor color;
            if (!parsePaletteEntry(value, &index, &color)) {
                setError(errorMessage,
                         QStringLiteral("Invalid palette in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->hasPalette = true;
            parsed->palette[static_cast<std::size_t>(index)] = color;
        } else if (key == QStringLiteral("selection-foreground")) {
            if (!parseTerminalColor(value, &parsed->selectionForeground)) {
                setError(errorMessage,
                         QStringLiteral("Invalid selection-foreground in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->hasSelectionForeground = true;
        } else if (key == QStringLiteral("selection-background")) {
            if (!parseTerminalColor(value, &parsed->selectionBackground)) {
                setError(errorMessage,
                         QStringLiteral("Invalid selection-background in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->hasSelectionBackground = true;
        } else if (key == QStringLiteral("cursor-color")) {
            parsed->hasCursorColor = true;
            if (!parseTerminalColor(value, &parsed->cursorColor)) {
                setError(errorMessage,
                         QStringLiteral("Invalid cursor-color in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
        } else if (key == QStringLiteral("cursor-opacity")) {
            if (!parseFiniteDouble(value, &parsed->cursorOpacity)) {
                setError(errorMessage,
                         QStringLiteral("Invalid cursor-opacity in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->hasCursorOpacity = true;
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
            parsed->hasCursorStyle = true;
            parsed->cursorStyle = value;
        } else if (key == QStringLiteral("cursor-style-blink")) {
            if (!parseOptionalBool(value, &parsed->cursorStyleBlink)) {
                setError(errorMessage,
                         QStringLiteral("Invalid cursor-style-blink in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->hasCursorStyleBlink = true;
        } else if (key == QStringLiteral("cursor-text")) {
            if (!parseTerminalColor(value, &parsed->cursorText)) {
                setError(errorMessage,
                         QStringLiteral("Invalid cursor-text in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->hasCursorText = true;
        } else if (key == QStringLiteral("bold-color")) {
            parsed->hasBoldColor = true;
            if (value.isEmpty() || value == QStringLiteral("bright")) {
                parsed->boldColor = value;
            } else {
                QColor color;
                if (!parseColor(value, &color)) {
                    setError(errorMessage,
                             QStringLiteral("Invalid bold-color in Ghostty config output at line %1")
                                 .arg(displayLine));
                    return false;
                }
                parsed->boldColor = color;
            }
        } else if (key == QStringLiteral("faint-opacity")) {
            if (!parseUnitInterval(value, &parsed->faintOpacity)) {
                setError(errorMessage,
                         QStringLiteral("Invalid faint-opacity in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->hasFaintOpacity = true;
        } else if (key == QStringLiteral("scrollback-limit")) {
            bool valid = false;
            const quint64 scrollbackLimit = value.toULongLong(&valid);
            if (!valid || value.startsWith(u'-')) {
                setError(errorMessage,
                         QStringLiteral("Invalid scrollback-limit in Ghostty config output at line %1")
                             .arg(displayLine));
                return false;
            }
            parsed->hasScrollbackLimit = true;
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
            parsed->hasConfirmCloseSurface = true;
            parsed->confirmCloseSurface = value;
        } else if (key == QStringLiteral("keybind")) {
            if (!parsed->hasKeybinds) {
                parsed->keybinds.clear();
                parsed->hasKeybinds = true;
            }
            // An empty formatted entry is Ghostty's effective empty binding
            // set. Do not retain a synthetic empty binding.
            if (!value.isEmpty()) {
                parsed->keybinds.append(value);
            }
        } else if (key == QStringLiteral("config-file")) {
            if (!parsed->hasConfigFiles) {
                parsed->configFiles.clear();
                parsed->hasConfigFiles = true;
            }
            if (value.isEmpty()) {
                parsed->configFiles.clear();
            } else {
                parsed->configFiles.append(value);
            }
        }
    }
    return true;
}

template<typename Value>
Value mergedValue(bool changesHasValue,
                  const Value &changesValue,
                  const Value &defaultValue)
{
    return changesHasValue ? changesValue : defaultValue;
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

bool parseGhosttyKeybindConfigJson(const QByteArray &json,
                                   GhosttyKeybindConfig *output,
                                   QString *errorMessage)
{
    if (errorMessage) errorMessage->clear();
    if (output == nullptr) {
        setError(errorMessage,
                 QStringLiteral("No keybinding config output object was provided"));
        return false;
    }

    QJsonParseError jsonError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &jsonError);
    if (jsonError.error != QJsonParseError::NoError) {
        setError(errorMessage,
                 QStringLiteral("Invalid Ghostty keybinding JSON at offset %1: %2")
                     .arg(jsonError.offset)
                     .arg(jsonError.errorString()));
        return false;
    }
    if (!document.isObject()) {
        setError(errorMessage,
                 QStringLiteral("Ghostty keybinding JSON root must be an object"));
        return false;
    }

    const QJsonObject object = document.object();
    if (!exactObjectKeys(object,
                         {QLatin1StringView("version"),
                          QLatin1StringView("root"),
                          QLatin1StringView("tables")},
                         QStringLiteral("Ghostty keybinding JSON root"),
                         errorMessage)) {
        return false;
    }
    quint64 schemaVersion = 0;
    if (!unsignedJsonInteger(object.value(QStringLiteral("version")),
                             std::numeric_limits<int>::max(),
                             &schemaVersion)
        || schemaVersion != GhosttyKeybindConfig::CurrentSchemaVersion) {
        setError(errorMessage,
                 QStringLiteral("Unsupported Ghostty keybinding JSON schema version"));
        return false;
    }

    GhosttyKeybindConfig parsed;
    parsed.schemaVersion = static_cast<int>(schemaVersion);
    if (!parseKeybindDefinitions(object.value(QStringLiteral("root")),
                                 QStringLiteral("root"), &parsed.root,
                                 errorMessage)) {
        return false;
    }

    const QJsonValue tablesValue = object.value(QStringLiteral("tables"));
    if (!tablesValue.isArray()) {
        setError(errorMessage,
                 QStringLiteral("tables must be an array"));
        return false;
    }
    const QJsonArray tables = tablesValue.toArray();
    parsed.tables.reserve(tables.size());
    QSet<QString> tableNames;
    for (qsizetype index = 0; index < tables.size(); ++index) {
        const QString context = QStringLiteral("tables[%1]").arg(index);
        if (!tables.at(index).isObject()) {
            setError(errorMessage,
                     QStringLiteral("%1 must be an object").arg(context));
            return false;
        }
        const QJsonObject table = tables.at(index).toObject();
        if (!exactObjectKeys(table,
                             {QLatin1StringView("name"),
                              QLatin1StringView("bindings")},
                             context, errorMessage)) {
            return false;
        }
        const QJsonValue nameValue = table.value(QStringLiteral("name"));
        if (!nameValue.isString() || nameValue.toString().isEmpty()) {
            setError(errorMessage,
                     QStringLiteral("%1.name must be a non-empty string")
                         .arg(context));
            return false;
        }
        const QString name = nameValue.toString();
        if (tableNames.contains(name)) {
            setError(errorMessage,
                     QStringLiteral("Duplicate Ghostty keybinding table '%1'")
                         .arg(name));
            return false;
        }
        tableNames.insert(name);
        GhosttyKeybindTable parsedTable{
            .name = name,
            .bindings = {},
        };
        if (!parseKeybindDefinitions(table.value(QStringLiteral("bindings")),
                                     context + QStringLiteral(".bindings"),
                                     &parsedTable.bindings, errorMessage)) {
            return false;
        }
        parsed.tables.append(std::move(parsedTable));
    }

    *output = std::move(parsed);
    return true;
}

QString ghosttyConfigXdgHome(const QStringList &candidatePaths,
                             QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }

    const QString legacyPath =
        candidateNamed(candidatePaths, QString::fromLatin1(LegacyConfigName));
    const QString preferredPath =
        candidateNamed(candidatePaths, QString::fromLatin1(PreferredConfigName));
    if (legacyPath.isEmpty() || preferredPath.isEmpty()) {
        setError(errorMessage,
                 QStringLiteral("Ghostty config candidates must contain both config and config.ghostty"));
        return {};
    }

    const QString legacyDirectory = QFileInfo(legacyPath).absolutePath();
    const QString preferredDirectory = QFileInfo(preferredPath).absolutePath();
    if (legacyDirectory != preferredDirectory
        || QFileInfo(legacyDirectory).fileName() != QStringLiteral("ghostty")) {
        setError(errorMessage,
                 QStringLiteral("Ghostty config candidates must share one XDG ghostty directory"));
        return {};
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
        return GhosttyConfigLoadResult::failed(std::move(parseError));
    }

    if (!defaults.hasFontFamilies || !defaults.hasFontSize
        || !defaults.hasForeground || !defaults.hasBackground
        || !defaults.hasPalette
        || !defaults.hasSelectionForeground
        || !defaults.hasSelectionBackground
        || !defaults.hasCursorColor || !defaults.hasCursorOpacity
        || !defaults.hasCursorStyle || !defaults.hasCursorStyleBlink
        || !defaults.hasCursorText || !defaults.hasBoldColor
        || !defaults.hasFaintOpacity
        || !defaults.hasScrollbackLimit || !defaults.hasConfirmCloseSurface
        || !defaults.hasKeybinds || !defaults.hasConfigFiles) {
        return GhosttyConfigLoadResult::failed(
            QStringLiteral("Ghostty default config output is missing a required compatibility key"));
    }

    ParsedConfig changes;
    if (!parseDump(changesOutput, &changes, &parseError)) {
        return GhosttyConfigLoadResult::failed(std::move(parseError));
    }

    const QStringList fontFamilies =
        mergedValue(changes.hasFontFamilies, changes.fontFamilies,
                    defaults.fontFamilies);
    const double fontSize =
        mergedValue(changes.hasFontSize, changes.fontSize, defaults.fontSize);
    const QColor foreground =
        mergedValue(changes.hasForeground, changes.foreground,
                    defaults.foreground);
    const QColor background =
        mergedValue(changes.hasBackground, changes.background,
                    defaults.background);
    std::array<std::optional<QColor>, 256> palette = defaults.palette;
    if (changes.hasPalette) {
        for (std::size_t index = 0; index < palette.size(); ++index) {
            if (changes.palette[index].has_value()) {
                palette[index] = changes.palette[index];
            }
        }
    }
    QVariantList paletteValues;
    paletteValues.reserve(static_cast<qsizetype>(palette.size()));
    for (const std::optional<QColor> &color : palette) {
        if (!color.has_value()) {
            return GhosttyConfigLoadResult::failed(
                QStringLiteral("Ghostty default config output is missing a required compatibility key"));
        }
        paletteValues.append(*color);
    }
    const QVariant selectionForeground =
        mergedValue(changes.hasSelectionForeground,
                    changes.selectionForeground,
                    defaults.selectionForeground);
    const QVariant selectionBackground =
        mergedValue(changes.hasSelectionBackground,
                    changes.selectionBackground,
                    defaults.selectionBackground);
    const QVariant cursorColor =
        mergedValue(changes.hasCursorColor, changes.cursorColor,
                    defaults.cursorColor);
    const double cursorOpacity =
        mergedValue(changes.hasCursorOpacity, changes.cursorOpacity,
                    defaults.cursorOpacity);
    const QString cursorStyle =
        mergedValue(changes.hasCursorStyle, changes.cursorStyle,
                    defaults.cursorStyle);
    const QVariant cursorStyleBlink =
        mergedValue(changes.hasCursorStyleBlink,
                    changes.cursorStyleBlink,
                    defaults.cursorStyleBlink);
    const QVariant cursorText =
        mergedValue(changes.hasCursorText, changes.cursorText,
                    defaults.cursorText);
    const QVariant boldColor =
        mergedValue(changes.hasBoldColor, changes.boldColor,
                    defaults.boldColor);
    const double faintOpacity =
        mergedValue(changes.hasFaintOpacity, changes.faintOpacity,
                    defaults.faintOpacity);
    const quint64 scrollbackLimit =
        mergedValue(changes.hasScrollbackLimit, changes.scrollbackLimit,
                    defaults.scrollbackLimit);
    const QString confirmCloseSurface =
        mergedValue(changes.hasConfirmCloseSurface,
                    changes.confirmCloseSurface,
                    defaults.confirmCloseSurface);
    const QStringList keybinds =
        mergedValue(changes.hasKeybinds, changes.keybinds,
                    defaults.keybinds);
    const QStringList configFiles =
        mergedValue(changes.hasConfigFiles, changes.configFiles,
                    defaults.configFiles);

    GhosttyConfigSnapshot snapshot;
    snapshot.availability = GhosttyConfigAvailability::Available;
    snapshot.values.insert(QStringLiteral("font-family"), fontFamilies);
    snapshot.values.insert(QStringLiteral("font-size"), fontSize);
    snapshot.values.insert(QStringLiteral("foreground"), foreground);
    snapshot.values.insert(QStringLiteral("background"), background);
    snapshot.values.insert(QStringLiteral("palette"), paletteValues);
    snapshot.values.insert(QStringLiteral("selection-foreground"),
                           selectionForeground);
    snapshot.values.insert(QStringLiteral("selection-background"),
                           selectionBackground);
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
    snapshot.values.insert(QStringLiteral("keybind"), keybinds);
    snapshot.values.insert(QStringLiteral("config-file"), configFiles);
    if (changes.hasKeybinds) {
        QStringList changedKeybinds;
        for (const QString &keybind : keybinds) {
            if (!defaults.keybinds.contains(keybind)) {
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

    return GhosttyConfigLoadResult::loaded(std::move(snapshot));
}

GhosttyConfigLoader makeGhosttyConfigProcessLoader(
    GhosttyConfigProcessLoaderOptions options)
{
    return [options = std::move(options)](
               const QStringList &candidatePaths) -> GhosttyConfigLoadResult {
        if (options.helperPath.isEmpty()) {
            return GhosttyConfigLoadResult::failed(
                QStringLiteral("Ghostty config helper path is empty"));
        }

        QString candidateError;
        const QString xdgConfigHome =
            ghosttyConfigXdgHome(candidatePaths, &candidateError);
        if (xdgConfigHome.isEmpty()) {
            return GhosttyConfigLoadResult::failed(std::move(candidateError));
        }

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

        // Unlike +show-config's lossy formatter, this application-specific
        // action exports the complete effective root set and named tables.
        const ProcessResult keybinds =
            run({QStringLiteral("+show-keybinds-json")});
        if (keybinds.status != ProcessResult::Status::Completed
            || keybinds.exitCode != 0) {
            return processFailure(QStringLiteral("keybinding query"), keybinds,
                                  operationTimeout);
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
        const ProcessResult verifiedKeybinds =
            run({QStringLiteral("+show-keybinds-json")});
        if (verifiedKeybinds.status != ProcessResult::Status::Completed
            || verifiedKeybinds.exitCode != 0) {
            return processFailure(
                QStringLiteral("keybinding consistency query"),
                verifiedKeybinds, operationTimeout);
        }
        if (changes.standardOutput != verifiedChanges.standardOutput
            || keybinds.standardOutput != verifiedKeybinds.standardOutput) {
            return GhosttyConfigLoadResult::failed(QStringLiteral(
                "Ghostty config changed while it was being queried; reload will retry"));
        }

        GhosttyConfigLoadResult parsed = parseGhosttyConfigShowOutputs(
            defaults.standardOutput, changes.standardOutput, candidatePaths);
        if (!parsed.succeeded()) {
            return parsed;
        }
        GhosttyKeybindConfig keybindConfig;
        QString keybindParseError;
        if (!parseGhosttyKeybindConfigJson(keybinds.standardOutput,
                                           &keybindConfig,
                                           &keybindParseError)) {
            return GhosttyConfigLoadResult::failed(
                QStringLiteral("Ghostty keybinding query returned malformed data: %1")
                    .arg(keybindParseError));
        }
        parsed.snapshot->keybindConfig = std::move(keybindConfig);

        // Config values belong on stdout; any successful stderr and validator
        // stdout are warnings from the pinned parser and must remain visible
        // to callers instead of disappearing with the helper process.
        appendProcessDiagnostics(&*parsed.snapshot,
                                 QStringLiteral("validation"),
                                 validation.standardOutput);
        appendProcessDiagnostics(&*parsed.snapshot,
                                 QStringLiteral("validation"),
                                 validation.standardError);
        appendProcessDiagnostics(&*parsed.snapshot,
                                 QStringLiteral("default query"),
                                 defaults.standardError);
        appendProcessDiagnostics(&*parsed.snapshot,
                                 QStringLiteral("current query"),
                                 changes.standardError);
        appendProcessDiagnostics(&*parsed.snapshot,
                                 QStringLiteral("keybinding query"),
                                 keybinds.standardError);
        appendProcessDiagnostics(&*parsed.snapshot,
                                 QStringLiteral("post-query validation"),
                                 postQueryValidation.standardOutput);
        appendProcessDiagnostics(&*parsed.snapshot,
                                 QStringLiteral("post-query validation"),
                                 postQueryValidation.standardError);
        return parsed;
    };
}
