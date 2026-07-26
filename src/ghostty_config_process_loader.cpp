#include "ghostty_config_process_loader.h"

#include "ghostty_action_catalog.h"
#include "ghostty_config_export.h"

#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSet>

#include <algorithm>
#include <limits>
#include <ranges>
#include <utility>

namespace {

constexpr auto LegacyConfigName = "config";
constexpr auto PreferredConfigName = "config.ghostty";

struct ProcessResult {
    enum class Status {
        Completed,
        StartFailed,
        TimedOut,
        DeadlineExpired,
        Crashed,
    };

    Status status = Status::StartFailed;
    int exitCode = -1;
    QByteArray standardOutput;
    QByteArray standardError;
};

QString normalizedAbsolutePath(const QString &path)
{
    if (path.isEmpty()) return {};
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString conciseProcessDetails(const ProcessResult &result)
{
    QStringList details;
    const QString standardOutput =
        QString::fromUtf8(result.standardOutput).trimmed();
    const QString standardError =
        QString::fromUtf8(result.standardError).trimmed();
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
                        const QString &xdgConfigHome, int timeoutMilliseconds)
{
    QProcess process;
    QProcessEnvironment environment = options.environment;
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), xdgConfigHome);
    process.setProcessEnvironment(environment);
    process.setProgram(options.helperPath);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(QIODevice::ReadOnly);

    QDeadlineTimer deadline(std::max(1, timeoutMilliseconds));
    const auto remainingTimeout = [&deadline] {
        return static_cast<int>(std::clamp<qint64>(
            deadline.remainingTime(), 1, std::numeric_limits<int>::max()));
    };
    const auto stopProcess = [&process, &deadline] {
        process.kill();
        process.waitForFinished(static_cast<int>(
            std::clamp<qint64>(deadline.remainingTime(), 0, 1'000)));
    };
    if (!process.waitForStarted(remainingTimeout())) {
        if (process.error() == QProcess::Timedout) {
            stopProcess();
            return {
                .status = ProcessResult::Status::TimedOut,
                .standardOutput = process.readAllStandardOutput(),
                .standardError = process.readAllStandardError(),
            };
        }
        return {
            .status = ProcessResult::Status::StartFailed,
            .exitCode = -1,
            .standardOutput = {},
            .standardError = {},
        };
    }
    if (!process.waitForFinished(remainingTimeout())) {
        stopProcess();
        return {
            .status = ProcessResult::Status::TimedOut,
            .standardOutput = process.readAllStandardOutput(),
            .standardError = process.readAllStandardError(),
        };
    }

    return {
        .status = process.exitStatus() == QProcess::CrashExit
            ? ProcessResult::Status::Crashed
            : ProcessResult::Status::Completed,
        .exitCode = process.exitCode(),
        .standardOutput = process.readAllStandardOutput(),
        .standardError = process.readAllStandardError(),
    };
}

GhosttyConfigLoadResult processFailure(const QString &operation,
                                       const ProcessResult &result,
                                       int timeoutMilliseconds)
{
    switch (result.status) {
    case ProcessResult::Status::StartFailed:
        return std::unexpected(
            QStringLiteral(
                "Ghostty config helper could not be started during %1")
                .arg(operation));
    case ProcessResult::Status::TimedOut:
        return std::unexpected(
            QStringLiteral(
                "Ghostty config helper timed out during %1 after %2 ms")
                .arg(operation)
                .arg(std::max(1, timeoutMilliseconds)));
    case ProcessResult::Status::DeadlineExpired:
        return std::unexpected(
            QStringLiteral(
                "Ghostty config helper transaction timed out before %1")
                .arg(operation));
    case ProcessResult::Status::Crashed:
        return std::unexpected(
            QStringLiteral("Ghostty config helper crashed during %1")
                .arg(operation));
    case ProcessResult::Status::Completed: break;
    }

    QString message =
        QStringLiteral(
            "Ghostty config helper failed during %1 with exit code %2")
            .arg(operation)
            .arg(result.exitCode);
    const QString details = conciseProcessDetails(result);
    if (!details.isEmpty()) message.append(QStringLiteral(": %1").arg(details));
    return std::unexpected(std::move(message));
}

void appendProcessDiagnostics(GhosttyConfigSnapshot &snapshot,
                              const QString &operation,
                              const QByteArray &output)
{
    if (output.trimmed().isEmpty()) return;
    for (QByteArray line : output.split('\n')) {
        if (line.endsWith('\r')) line.chop(1);
        const QString message = QString::fromUtf8(line).trimmed();
        if (message.isEmpty()) continue;
        GhosttyConfigDiagnostic diagnostic{
            .severity = GhosttyConfigDiagnosticSeverity::Warning,
            .message = QStringLiteral("Ghostty config helper %1: %2")
                           .arg(operation, message),
            .sourcePath = {},
            .line = 0,
            .column = 0,
        };
        if (!snapshot.diagnostics.contains(diagnostic)) {
            snapshot.diagnostics.append(std::move(diagnostic));
        }
    }
}

const QVector<GhosttyKeybindDefinition> *
bindingsForTable(const GhosttyKeybindConfig &config, const QString &name)
{
    const auto table =
        std::ranges::find(config.tables, name, &GhosttyKeybindTable::name);
    return table == config.tables.cend() ? nullptr : &table->bindings;
}

bool isDefaultDefinition(const GhosttyKeybindDefinition &definition,
                         const QVector<GhosttyKeybindDefinition> *defaults)
{
    if (defaults == nullptr) return false;
    const auto matchingPath = std::ranges::find(
        *defaults, definition.sequence, &GhosttyKeybindDefinition::sequence);
    return matchingPath != defaults->cend() && *matchingPath == definition;
}

void appendConfiguredActionDiagnostics(GhosttyConfigSnapshot &snapshot,
                                       const GhosttyKeybindConfig &current,
                                       const GhosttyKeybindConfig &defaults)
{
    QSet<QString> reportedActions;
    const auto inspect = [&](const GhosttyKeybindDefinition &definition,
                             const QVector<GhosttyKeybindDefinition>
                                 *baseline) {
        if (isDefaultDefinition(definition, baseline)) return;
        for (const QString &action : definition.actions) {
            if (reportedActions.contains(action)
                || GhosttyActionCatalog::isImplemented(action)) {
                continue;
            }
            reportedActions.insert(action);
            snapshot.diagnostics.append({
                .severity = GhosttyConfigDiagnosticSeverity::Warning,
                .message =
                    QStringLiteral(
                        "ghostty-qt does not implement configured keybind action '%1'")
                        .arg(action),
                .sourcePath = {},
                .line = 0,
                .column = 0,
            });
        }
    };

    for (const GhosttyKeybindDefinition &definition : current.root) {
        inspect(definition, &defaults.root);
    }
    for (const GhosttyKeybindTable &table : current.tables) {
        const auto *baseline = bindingsForTable(defaults, table.name);
        for (const GhosttyKeybindDefinition &definition : table.bindings) {
            inspect(definition, baseline);
        }
    }
}

void appendExistingSource(QStringList &sources, QSet<QString> &seen,
                          const QString &path)
{
    const QString normalized = normalizedAbsolutePath(path);
    if (!normalized.isEmpty() && QFileInfo(normalized).isFile()
        && !seen.contains(normalized)) {
        seen.insert(normalized);
        sources.append(normalized);
    }
}

QString candidateNamed(const QStringList &candidatePaths, const QString &name)
{
    for (const QString &path : candidatePaths) {
        const QString normalized = normalizedAbsolutePath(path);
        if (QFileInfo(normalized).fileName() == name) return normalized;
    }
    return {};
}

void populateSourcePaths(GhosttyConfigSnapshot &snapshot,
                         const QStringList &candidatePaths)
{
    QSet<QString> seen;
    // Ghostty loads the legacy file before config.ghostty regardless of which
    // candidate supplied the XDG root. Preserve that order for watchers.
    appendExistingSource(
        snapshot.sourcePaths, seen,
        candidateNamed(candidatePaths, QString::fromLatin1(LegacyConfigName)));
    appendExistingSource(
        snapshot.sourcePaths, seen,
        candidateNamed(candidatePaths,
                       QString::fromLatin1(PreferredConfigName)));
    for (const GhosttyConfigFile &file : snapshot.values.configFiles) {
        appendExistingSource(snapshot.sourcePaths, seen, file.path);
    }
}

} // namespace

std::expected<QString, QString>
ghosttyConfigXdgHome(const QStringList &candidatePaths)
{
    const QString legacyPath =
        candidateNamed(candidatePaths, QString::fromLatin1(LegacyConfigName));
    const QString preferredPath = candidateNamed(
        candidatePaths, QString::fromLatin1(PreferredConfigName));
    if (legacyPath.isEmpty() || preferredPath.isEmpty()) {
        return std::unexpected(QStringLiteral(
            "Ghostty config candidates must contain both config and config.ghostty"));
    }

    const QString legacyDirectory = QFileInfo(legacyPath).absolutePath();
    const QString preferredDirectory = QFileInfo(preferredPath).absolutePath();
    if (legacyDirectory != preferredDirectory
        || QFileInfo(legacyDirectory).fileName() != QStringLiteral("ghostty")) {
        return std::unexpected(QStringLiteral(
            "Ghostty config candidates must share one XDG ghostty directory"));
    }
    return QDir::cleanPath(QFileInfo(legacyDirectory).absolutePath());
}

GhosttyConfigLoader
makeGhosttyConfigProcessLoader(GhosttyConfigProcessLoaderOptions options)
{
    return [options = std::move(options)](
               const QStringList &candidatePaths) -> GhosttyConfigLoadResult {
        if (options.helperPath.isEmpty()) {
            return std::unexpected(
                QStringLiteral("Ghostty config helper path is empty"));
        }

        auto xdgConfigHome = ghosttyConfigXdgHome(candidatePaths);
        if (!xdgConfigHome) {
            return std::unexpected(std::move(xdgConfigHome.error()));
        }

        QDeadlineTimer overallDeadline(
            std::max(1, options.overallTimeoutMilliseconds));
        int operationTimeout = std::max(1, options.timeoutMilliseconds);
        const auto run = [&](QStringList arguments,
                             bool includeConfigurationArguments = false) {
            const qint64 remaining = overallDeadline.remainingTime();
            if (remaining <= 0) {
                operationTimeout = 0;
                return ProcessResult{
                    .status = ProcessResult::Status::DeadlineExpired,
                    .exitCode = -1,
                    .standardOutput = {},
                    .standardError = {},
                };
            }
            operationTimeout = static_cast<int>(std::min<qint64>(
                remaining, std::max(1, options.timeoutMilliseconds)));
            if (includeConfigurationArguments) {
                arguments.append(options.configurationArguments);
            }
            return runHelper(options, arguments, *xdgConfigHome,
                             operationTimeout);
        };
        const auto requireSuccess =
            [&](const QString &operation,
                ProcessResult result) -> std::expected<ProcessResult, QString> {
            if (result.status != ProcessResult::Status::Completed
                || result.exitCode != 0) {
                auto failure =
                    processFailure(operation, result, operationTimeout);
                return std::unexpected(std::move(failure.error()));
            }
            return result;
        };

        auto validation =
            requireSuccess(QStringLiteral("validation"),
                           run({QStringLiteral("+validate-config")}));
        if (!validation) return std::unexpected(std::move(validation.error()));

        auto config =
            requireSuccess(QStringLiteral("config query"),
                           run({QStringLiteral("+show-config-json")}, true));
        if (!config) return std::unexpected(std::move(config.error()));

        auto postValidation =
            requireSuccess(QStringLiteral("post-query validation"),
                           run({QStringLiteral("+validate-config")}));
        if (!postValidation) {
            return std::unexpected(std::move(postValidation.error()));
        }

        auto verifiedConfig =
            requireSuccess(QStringLiteral("config consistency query"),
                           run({QStringLiteral("+show-config-json")}, true));
        if (!verifiedConfig) {
            return std::unexpected(std::move(verifiedConfig.error()));
        }
        if (config->standardOutput != verifiedConfig->standardOutput) {
            return std::unexpected(QStringLiteral(
                "Ghostty config changed while it was being queried; reload will retry"));
        }

        auto exported = parseGhosttyConfigExportJson(config->standardOutput);
        if (!exported) {
            return std::unexpected(
                QStringLiteral(
                    "Ghostty config query returned malformed data: %1")
                    .arg(exported.error()));
        }

        GhosttyKeybindConfig defaultKeybindings =
            std::move(exported->defaultKeybindings);
        GhosttyConfigSnapshot snapshot(std::move(*exported));
        appendConfiguredActionDiagnostics(snapshot, snapshot.keybindings,
                                          defaultKeybindings);
        populateSourcePaths(snapshot, candidatePaths);

        // Successful validator output and the first exporter stderr may carry
        // warnings from Ghostty. The consistency query is intentionally silent
        // so re-sampling cannot duplicate a warning.
        appendProcessDiagnostics(snapshot, QStringLiteral("validation"),
                                 validation->standardOutput);
        appendProcessDiagnostics(snapshot, QStringLiteral("validation"),
                                 validation->standardError);
        appendProcessDiagnostics(snapshot, QStringLiteral("config query"),
                                 config->standardError);
        appendProcessDiagnostics(snapshot,
                                 QStringLiteral("post-query validation"),
                                 postValidation->standardOutput);
        appendProcessDiagnostics(snapshot,
                                 QStringLiteral("post-query validation"),
                                 postValidation->standardError);
        return snapshot;
    };
}
