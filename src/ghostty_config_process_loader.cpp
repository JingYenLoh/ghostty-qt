#include "ghostty_config_process_loader.h"
#include "ghostty_action_catalog.h"
#include "ghostty_keybind_set.h"

#include <QColor>
#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <limits>
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
    bool hasCursorColor = false;
    QVariant cursorColor;
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
    case GhosttyKeybindUnsupportedReason::Sequence:
        return QStringLiteral("key sequences are not implemented yet");
    case GhosttyKeybindUnsupportedReason::KeyTable:
        return QStringLiteral("named key tables are not implemented yet");
    case GhosttyKeybindUnsupportedReason::CatchAll:
        return QStringLiteral("catch-all triggers are not implemented yet");
    case GhosttyKeybindUnsupportedReason::NonLocal:
        return QStringLiteral("all-surface/global triggers are not implemented yet");
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
        } else if (key == QStringLiteral("cursor-color")) {
            parsed->hasCursorColor = true;
            if (value.isEmpty()
                || value == QStringLiteral("cell-foreground")
                || value == QStringLiteral("cell-background")) {
                parsed->cursorColor = value;
            } else {
                QColor color;
                if (!parseColor(value, &color)) {
                    setError(errorMessage,
                             QStringLiteral("Invalid cursor-color in Ghostty config output at line %1")
                                 .arg(displayLine));
                    return false;
                }
                parsed->cursorColor = color;
            }
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

} // namespace

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
        || !defaults.hasCursorColor
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
    const QVariant cursorColor =
        mergedValue(changes.hasCursorColor, changes.cursorColor,
                    defaults.cursorColor);
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
    snapshot.values.insert(QStringLiteral("cursor-color"), cursorColor);
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

        // `+show-config` deliberately prints even when its independently
        // loaded config contains diagnostics. Validate again after both
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

        GhosttyConfigLoadResult parsed = parseGhosttyConfigShowOutputs(
            defaults.standardOutput, changes.standardOutput, candidatePaths);
        if (!parsed.succeeded()) {
            return parsed;
        }

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
                                 QStringLiteral("post-query validation"),
                                 postQueryValidation.standardOutput);
        appendProcessDiagnostics(&*parsed.snapshot,
                                 QStringLiteral("post-query validation"),
                                 postQueryValidation.standardError);
        return parsed;
    };
}
