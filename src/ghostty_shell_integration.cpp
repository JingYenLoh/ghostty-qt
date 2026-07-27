#include "ghostty_shell_integration.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QSet>

#include <algorithm>
#include <limits>
#include <utility>

namespace {

constexpr qsizetype kMaximumRequestProtocolBytes = 4 * 1024 * 1024;
// Setup can add command arguments and environment entries to a request that
// was already close to its input limit. Keep the response bounded, but leave
// explicit headroom for that legitimate expansion.
constexpr qsizetype kMaximumResponseProtocolBytes = 8 * 1024 * 1024;
constexpr qsizetype kMaximumHelperDiagnosticBytes = 256 * 1024;
constexpr int kProcessDrainIntervalMilliseconds = 50;

QString childContext(const QString &parent, QStringView field)
{
    return QStringLiteral("%1.%2").arg(parent, field);
}

std::expected<QJsonObject, QString>
exactObject(const QJsonValue &value, const QString &context,
            std::initializer_list<QStringView> fields)
{
    if (!value.isObject()) {
        return std::unexpected(
            QStringLiteral("%1 must be an object").arg(context));
    }
    const QJsonObject object = value.toObject();
    if (object.size() != static_cast<qsizetype>(fields.size())) {
        return std::unexpected(
            QStringLiteral("%1 has an unexpected field set").arg(context));
    }
    for (const QStringView field : fields) {
        if (!object.contains(field)) {
            return std::unexpected(
                QStringLiteral("%1 is missing field '%2'").arg(context, field));
        }
    }
    return object;
}

std::expected<QByteArray, QString> decodeBytes(const QJsonValue &value,
                                               const QString &context)
{
    if (!value.isString()) {
        return std::unexpected(
            QStringLiteral("%1 must be a base64 string").arg(context));
    }
    const QByteArray encoded = value.toString().toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(
        encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.toBase64() != encoded) {
        return std::unexpected(
            QStringLiteral("%1 must use canonical base64").arg(context));
    }
    return decoded.decoded;
}

std::expected<bool, QString> readBoolean(const QJsonValue &value,
                                         const QString &context)
{
    if (!value.isBool()) {
        return std::unexpected(
            QStringLiteral("%1 must be a boolean").arg(context));
    }
    return value.toBool();
}

bool validCommandBytes(const QByteArray &value)
{
    return !value.isEmpty() && !value.contains('\0');
}

bool validEnvironmentKey(const QByteArray &key)
{
    return !key.isEmpty() && !key.contains('=') && !key.contains('\0');
}

std::expected<TerminalCommand, QString> readCommand(const QJsonValue &value,
                                                    const QString &context)
{
    if (!value.isObject()) {
        return std::unexpected(
            QStringLiteral("%1 must be an object").arg(context));
    }
    const QJsonObject unvalidated = value.toObject();
    const QJsonValue kindValue = unvalidated.value(QStringLiteral("kind"));
    if (!kindValue.isString()) {
        return std::unexpected(
            QStringLiteral("%1.kind must be a string").arg(context));
    }
    const QString kind = kindValue.toString();
    if (kind == QLatin1StringView("shell")) {
        auto object =
            exactObject(value, context, {u"kind", u"value", u"default-shell"});
        if (!object) return std::unexpected(std::move(object.error()));
        auto command = decodeBytes(object->value(QStringLiteral("value")),
                                   childContext(context, u"value"));
        if (!command) return std::unexpected(std::move(command.error()));
        if (!validCommandBytes(*command)) {
            return std::unexpected(
                QStringLiteral("%1.value must be non-empty and contain no NUL")
                    .arg(context));
        }
        auto defaultShell =
            readBoolean(object->value(QStringLiteral("default-shell")),
                        childContext(context, u"default-shell"));
        if (!defaultShell) {
            return std::unexpected(std::move(defaultShell.error()));
        }
        return TerminalCommand::shell(std::move(*command), *defaultShell);
    }
    if (kind == QLatin1StringView("direct")) {
        auto object =
            exactObject(value, context, {u"kind", u"argv", u"default-shell"});
        if (!object) return std::unexpected(std::move(object.error()));
        const QJsonValue argvValue = object->value(QStringLiteral("argv"));
        if (!argvValue.isArray() || argvValue.toArray().isEmpty()) {
            return std::unexpected(
                QStringLiteral("%1.argv must be a non-empty array")
                    .arg(context));
        }
        QVector<QByteArray> arguments;
        const QJsonArray argv = argvValue.toArray();
        arguments.reserve(argv.size());
        for (qsizetype index = 0; index < argv.size(); ++index) {
            auto argument = decodeBytes(
                argv.at(index),
                QStringLiteral("%1.argv[%2]").arg(context).arg(index));
            if (!argument) {
                return std::unexpected(std::move(argument.error()));
            }
            if (argument->contains('\0')) {
                return std::unexpected(
                    QStringLiteral("%1.argv[%2] must contain no NUL")
                        .arg(context)
                        .arg(index));
            }
            arguments.append(std::move(*argument));
        }
        auto defaultShell =
            readBoolean(object->value(QStringLiteral("default-shell")),
                        childContext(context, u"default-shell"));
        if (!defaultShell) {
            return std::unexpected(std::move(defaultShell.error()));
        }
        if (*defaultShell) {
            return std::unexpected(
                QStringLiteral("%1.default-shell must be false for direct argv")
                    .arg(context));
        }
        return TerminalCommand::direct(std::move(arguments));
    }
    return std::unexpected(QStringLiteral("%1.kind has unsupported value '%2'")
                               .arg(context, kind));
}

std::expected<TerminalEnvironment, QString>
readEnvironment(const QJsonValue &value, const QString &context)
{
    if (!value.isArray()) {
        return std::unexpected(
            QStringLiteral("%1 must be an array").arg(context));
    }
    const QJsonArray array = value.toArray();
    TerminalEnvironment result;
    result.reserve(array.size());
    QSet<QByteArray> keys;
    for (qsizetype index = 0; index < array.size(); ++index) {
        const QString entryContext =
            QStringLiteral("%1[%2]").arg(context).arg(index);
        auto object =
            exactObject(array.at(index), entryContext, {u"key", u"value"});
        if (!object) return std::unexpected(std::move(object.error()));
        auto key = decodeBytes(object->value(QStringLiteral("key")),
                               childContext(entryContext, u"key"));
        if (!key) return std::unexpected(std::move(key.error()));
        auto entryValue = decodeBytes(object->value(QStringLiteral("value")),
                                      childContext(entryContext, u"value"));
        if (!entryValue) {
            return std::unexpected(std::move(entryValue.error()));
        }
        if (!validEnvironmentKey(*key)) {
            return std::unexpected(
                QStringLiteral("%1.key is not a valid environment key")
                    .arg(entryContext));
        }
        if (entryValue->contains('\0')) {
            return std::unexpected(
                QStringLiteral("%1.value must contain no NUL")
                    .arg(entryContext));
        }
        if (keys.contains(*key)) {
            return std::unexpected(
                QStringLiteral("%1 contains a duplicate environment key")
                    .arg(entryContext));
        }
        keys.insert(*key);
        result.append({
            .key = std::move(*key),
            .value = std::move(*entryValue),
        });
    }
    return result;
}

QString modeName(GhosttyShellIntegrationMode mode)
{
    switch (mode) {
    case GhosttyShellIntegrationMode::None: return QStringLiteral("none");
    case GhosttyShellIntegrationMode::Detect: return QStringLiteral("detect");
    case GhosttyShellIntegrationMode::Bash: return QStringLiteral("bash");
    case GhosttyShellIntegrationMode::Elvish: return QStringLiteral("elvish");
    case GhosttyShellIntegrationMode::Fish: return QStringLiteral("fish");
    case GhosttyShellIntegrationMode::Nushell: return QStringLiteral("nushell");
    case GhosttyShellIntegrationMode::Zsh: return QStringLiteral("zsh");
    }
    return {};
}

QJsonObject writeCommand(const TerminalCommand &command)
{
    QJsonObject result{
        {QStringLiteral("kind"),
         command.kind == TerminalCommandKind::Shell ? QStringLiteral("shell")
                                                    : QStringLiteral("direct")},
        {QStringLiteral("default-shell"), command.defaultShell},
    };
    if (command.kind == TerminalCommandKind::Shell) {
        result.insert(QStringLiteral("value"),
                      QString::fromLatin1(command.shellCommand.toBase64()));
    } else {
        QJsonArray arguments;
        for (const QByteArray &argument : command.directArguments) {
            arguments.append(QString::fromLatin1(argument.toBase64()));
        }
        result.insert(QStringLiteral("argv"), arguments);
    }
    return result;
}

std::expected<void, QString>
validateRequest(const GhosttyShellIntegrationRequest &request)
{
    switch (request.command.kind) {
    case TerminalCommandKind::Shell:
        if (!validCommandBytes(request.command.shellCommand)
            || !request.command.directArguments.isEmpty()) {
            return std::unexpected(QStringLiteral(
                "Shell integration command is not a valid shell command"));
        }
        break;
    case TerminalCommandKind::Direct:
        if (request.command.defaultShell
            || request.command.directArguments.isEmpty()
            || !request.command.shellCommand.isEmpty()) {
            return std::unexpected(QStringLiteral(
                "Shell integration command is not a valid direct argv"));
        }
        for (const QByteArray &argument : request.command.directArguments) {
            if (argument.contains('\0')) {
                return std::unexpected(QStringLiteral(
                    "Shell integration direct argv contains NUL"));
            }
        }
        break;
    }

    QSet<QByteArray> keys;
    for (const TerminalEnvironmentEntry &entry : request.environment) {
        if (!validEnvironmentKey(entry.key) || entry.value.contains('\0')) {
            return std::unexpected(QStringLiteral(
                "Shell integration environment contains an invalid entry"));
        }
        if (keys.contains(entry.key)) {
            return std::unexpected(QStringLiteral(
                "Shell integration environment contains a duplicate key"));
        }
        keys.insert(entry.key);
    }

    if (!request.resourceDirectory.isEmpty()
        && (!request.resourceDirectory.startsWith('/')
            || request.resourceDirectory.contains('\0'))) {
        return std::unexpected(QStringLiteral(
            "Shell integration resource directory must be an absolute "
            "NUL-free path"));
    }
    return {};
}

QString conciseProcessError(QByteArrayView standardError, QString message)
{
    const QString diagnostic =
        QString::fromUtf8(standardError.data(), standardError.size()).trimmed();
    if (!diagnostic.isEmpty()) {
        message.append(QStringLiteral(": %1").arg(diagnostic));
    }
    return message;
}

QString usableResourceRoot(const QString &candidate)
{
    if (candidate.isEmpty()) return {};
    const QFileInfo rootInfo(QDir::cleanPath(QDir(candidate).absolutePath()));
    if (!rootInfo.isDir()) return {};
    const QFileInfo integration(
        QDir(rootInfo.absoluteFilePath())
            .filePath(QStringLiteral("shell-integration")));
    if (!integration.isDir()) return {};
    const QString canonical = rootInfo.canonicalFilePath();
    return canonical.isEmpty() ? rootInfo.absoluteFilePath() : canonical;
}

} // namespace

std::expected<QByteArray, QString> serializeGhosttyShellIntegrationRequest(
    const GhosttyShellIntegrationRequest &request)
{
    if (auto valid = validateRequest(request); !valid) {
        return std::unexpected(std::move(valid.error()));
    }

    QJsonArray environment;
    for (const TerminalEnvironmentEntry &entry : request.environment) {
        environment.append(QJsonObject{
            {QStringLiteral("key"), QString::fromLatin1(entry.key.toBase64())},
            {QStringLiteral("value"),
             QString::fromLatin1(entry.value.toBase64())}});
    }
    const GhosttyShellIntegrationFeatures &features = request.features;
    const QJsonObject root{
        {QStringLiteral("version"), 1},
        {QStringLiteral("resource-dir"),
         request.mode == GhosttyShellIntegrationMode::None
                 || request.resourceDirectory.isEmpty()
             ? QJsonValue(QJsonValue::Null)
             : QJsonValue(
                   QString::fromLatin1(request.resourceDirectory.toBase64()))},
        {QStringLiteral("command"), writeCommand(request.command)},
        {QStringLiteral("environment"), environment},
        {QStringLiteral("mode"), modeName(request.mode)},
        {QStringLiteral("features"),
         QJsonObject{
             {QStringLiteral("cursor"), features.cursor},
             {QStringLiteral("sudo"), features.sudo},
             {QStringLiteral("title"), features.title},
             {QStringLiteral("ssh-env"), features.sshEnvironment},
             {QStringLiteral("ssh-terminfo"), features.sshTerminfo},
             {QStringLiteral("path"), features.path},
         }},
        {QStringLiteral("cursor-blink"), request.cursorBlink},
    };
    const QByteArray result =
        QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (result.size() > kMaximumRequestProtocolBytes) {
        return std::unexpected(QStringLiteral(
            "Shell integration request exceeds the 4 MiB protocol limit"));
    }
    return result;
}

std::expected<GhosttyShellIntegrationResult, QString>
parseGhosttyShellIntegrationResult(const QByteArray &json)
{
    if (json.size() > kMaximumResponseProtocolBytes) {
        return std::unexpected(QStringLiteral(
            "Shell integration response exceeds the 8 MiB protocol limit"));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return std::unexpected(
            QStringLiteral("Shell integration response is invalid JSON: %1")
                .arg(parseError.errorString()));
    }
    auto root = exactObject(document.isObject() ? QJsonValue(document.object())
                                                : QJsonValue(),
                            QStringLiteral("root"),
                            {u"version", u"command", u"environment", u"shell"});
    if (!root) return std::unexpected(std::move(root.error()));
    const QJsonValue version = root->value(QStringLiteral("version"));
    if (!version.isDouble() || version.toDouble() != 1.0) {
        return std::unexpected(
            QStringLiteral("root.version must be the integer 1"));
    }

    auto command = readCommand(root->value(QStringLiteral("command")),
                               QStringLiteral("root.command"));
    if (!command) return std::unexpected(std::move(command.error()));
    auto environment =
        readEnvironment(root->value(QStringLiteral("environment")),
                        QStringLiteral("root.environment"));
    if (!environment) {
        return std::unexpected(std::move(environment.error()));
    }

    std::optional<GhosttyIntegratedShell> shell;
    const QJsonValue shellValue = root->value(QStringLiteral("shell"));
    if (!shellValue.isNull()) {
        if (!shellValue.isString()) {
            return std::unexpected(
                QStringLiteral("root.shell must be a string or null"));
        }
        const QString name = shellValue.toString();
        if (name == QLatin1StringView("bash")) {
            shell = GhosttyIntegratedShell::Bash;
        } else if (name == QLatin1StringView("elvish")) {
            shell = GhosttyIntegratedShell::Elvish;
        } else if (name == QLatin1StringView("fish")) {
            shell = GhosttyIntegratedShell::Fish;
        } else if (name == QLatin1StringView("nushell")) {
            shell = GhosttyIntegratedShell::Nushell;
        } else if (name == QLatin1StringView("zsh")) {
            shell = GhosttyIntegratedShell::Zsh;
        } else {
            return std::unexpected(
                QStringLiteral("root.shell has unsupported value '%1'")
                    .arg(name));
        }
    }

    return GhosttyShellIntegrationResult{
        .command = std::move(*command),
        .environment = std::move(*environment),
        .shell = shell,
    };
}

std::expected<GhosttyShellIntegrationResult, QString>
prepareGhosttyShellIntegration(
    const GhosttyShellIntegrationProcessOptions &options,
    const GhosttyShellIntegrationRequest &request)
{
    auto serialized = serializeGhosttyShellIntegrationRequest(request);
    if (!serialized) return std::unexpected(std::move(serialized.error()));
    if (options.helperPath.isEmpty()) {
        return std::unexpected(
            QStringLiteral("Shell integration helper path is empty"));
    }

    QProcess process;
    process.setProgram(options.helperPath);
    process.setArguments({QStringLiteral("+shell-integration-json")});
    process.setProcessEnvironment(options.environment);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(QIODevice::ReadWrite);

    QDeadlineTimer deadline(std::max(1, options.timeoutMilliseconds));
    const auto remaining = [&deadline] {
        return static_cast<int>(std::clamp<qint64>(
            deadline.remainingTime(), 1, std::numeric_limits<int>::max()));
    };
    if (!process.waitForStarted(remaining())) {
        process.kill();
        (void)process.waitForFinished(1'000);
        return std::unexpected(conciseProcessError(
            process.readAllStandardError(),
            QStringLiteral("Shell integration helper could not be started")));
    }

    qsizetype requestOffset = 0;
    while (requestOffset < serialized->size()) {
        if (deadline.hasExpired()) {
            process.kill();
            (void)process.waitForFinished(1'000);
            return std::unexpected(
                QStringLiteral("Shell integration helper timed out after %1 ms")
                    .arg(std::max(1, options.timeoutMilliseconds)));
        }
        const qint64 accepted = process.write(
            serialized->constData() + requestOffset,
            static_cast<qint64>(serialized->size() - requestOffset));
        if (accepted < 0) {
            process.kill();
            (void)process.waitForFinished(1'000);
            return std::unexpected(conciseProcessError(
                process.readAllStandardError(),
                QStringLiteral("Failed to write shell integration helper "
                               "request")));
        }
        requestOffset += static_cast<qsizetype>(accepted);
        if (accepted == 0
            || (process.bytesToWrite() > 0
                && !process.waitForBytesWritten(remaining())
                && process.bytesToWrite() > 0)) {
            process.kill();
            (void)process.waitForFinished(1'000);
            const QString message = deadline.hasExpired()
                ? QStringLiteral(
                      "Shell integration helper timed out after %1 ms")
                      .arg(std::max(1, options.timeoutMilliseconds))
                : QStringLiteral("Failed to write shell integration helper "
                                 "request");
            return std::unexpected(
                conciseProcessError(process.readAllStandardError(), message));
        }
    }
    process.closeWriteChannel();

    QByteArray output;
    QByteArray standardError;
    bool diagnosticTruncated = false;
    const auto drainProcess = [&]() -> std::optional<QString> {
        const QByteArray outputChunk = process.readAllStandardOutput();
        if (outputChunk.size()
            > kMaximumResponseProtocolBytes - output.size()) {
            return QStringLiteral(
                "Shell integration response exceeds the 8 MiB protocol limit");
        }
        output.append(outputChunk);

        const QByteArray diagnosticChunk = process.readAllStandardError();
        const qsizetype remainingDiagnostic =
            kMaximumHelperDiagnosticBytes - standardError.size();
        if (diagnosticChunk.size() > remainingDiagnostic) {
            if (remainingDiagnostic > 0) {
                standardError.append(diagnosticChunk.constData(),
                                     remainingDiagnostic);
            }
            diagnosticTruncated = true;
        } else {
            standardError.append(diagnosticChunk);
        }
        return std::nullopt;
    };

    while (process.state() != QProcess::NotRunning) {
        if (deadline.hasExpired()) {
            process.kill();
            (void)process.waitForFinished(1'000);
            return std::unexpected(
                QStringLiteral("Shell integration helper timed out after %1 ms")
                    .arg(std::max(1, options.timeoutMilliseconds)));
        }
        const int waitMilliseconds =
            std::min(remaining(), kProcessDrainIntervalMilliseconds);
        (void)process.waitForReadyRead(waitMilliseconds);
        if (auto error = drainProcess(); error.has_value()) {
            process.kill();
            (void)process.waitForFinished(1'000);
            return std::unexpected(std::move(*error));
        }
    }
    if (auto error = drainProcess(); error.has_value()) {
        return std::unexpected(std::move(*error));
    }
    if (process.exitStatus() != QProcess::NormalExit
        || process.exitCode() != 0) {
        if (diagnosticTruncated) {
            standardError.append(
                QByteArrayLiteral("\n[diagnostic output truncated]"));
        }
        return std::unexpected(conciseProcessError(
            standardError,
            QStringLiteral("Shell integration helper failed with exit code %1")
                .arg(process.exitCode())));
    }
    return parseGhosttyShellIntegrationResult(output);
}

std::expected<QString, QString> resolveShellIntegrationResourceDirectory(
    const QString &applicationDirectory,
    const std::optional<QString> &overrideDirectory)
{
    if (overrideDirectory.has_value()) {
        if (overrideDirectory->isEmpty()) {
            return std::unexpected(QStringLiteral(
                "GHOSTTY_QT_SHELL_INTEGRATION_RESOURCES is set but empty"));
        }
        const QString resolved = usableResourceRoot(*overrideDirectory);
        if (resolved.isEmpty()) {
            return std::unexpected(
                QStringLiteral(
                    "GHOSTTY_QT_SHELL_INTEGRATION_RESOURCES='%1' does not "
                    "contain a shell-integration directory")
                    .arg(QDir::cleanPath(
                        QDir(*overrideDirectory).absolutePath())));
        }
        return resolved;
    }

    const QString buildRoot = usableResourceRoot(applicationDirectory);
    if (!buildRoot.isEmpty()) return buildRoot;

    const QString installedCandidate =
        QDir(applicationDirectory)
            .absoluteFilePath(
                QStringLiteral(GHOSTTY_QT_INSTALL_RESOURCES_RELATIVE_DIR));
    const QString installedRoot = usableResourceRoot(installedCandidate);
    if (!installedRoot.isEmpty()) return installedRoot;

    return std::unexpected(
        QStringLiteral(
            "Unable to locate Ghostty shell-integration resources. Checked "
            "build-tree root '%1' and installed root '%2'")
            .arg(QDir::cleanPath(QDir(applicationDirectory).absolutePath()),
                 QDir::cleanPath(QDir(installedCandidate).absolutePath())));
}

std::expected<QString, QString>
resolveRuntimeShellIntegrationResourceDirectory()
{
    std::optional<QString> overrideDirectory;
    if (qEnvironmentVariableIsSet("GHOSTTY_QT_SHELL_INTEGRATION_RESOURCES")) {
        overrideDirectory =
            qEnvironmentVariable("GHOSTTY_QT_SHELL_INTEGRATION_RESOURCES");
    }
    return resolveShellIntegrationResourceDirectory(
        QCoreApplication::applicationDirPath(), overrideDirectory);
}
