#include "ghostty_application_ipc.h"

#include "application_identity.h"

#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDir>
#include <QFileInfo>
#include <QStringDecoder>
#include <QStringEncoder>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>

#include <algorithm>
#include <cstring>
#include <limits>
#include <ranges>
#include <utility>

namespace {

constexpr auto NewTabArgument = "+new-tab";
constexpr auto NewWindowArgument = "+new-window";
constexpr auto ToggleQuickTerminalArgument = "+toggle-quick-terminal";
constexpr auto NewTabAction = "new-tab";
constexpr auto NewWindowCommandAction = "new-window-command";
constexpr auto ToggleQuickTerminalAction = "toggle-quick-terminal";

GhosttyApplicationIpcError error(QString diagnostic)
{
    return {.diagnostic = std::move(diagnostic)};
}

bool isAsciiWhitespace(char character) noexcept
{
    switch (character) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case '\v':
    case '\f': return true;
    default: return false;
    }
}

QByteArrayView trimAsciiWhitespace(QByteArrayView value) noexcept
{
    while (!value.isEmpty() && isAsciiWhitespace(value.front())) {
        value = value.sliced(1);
    }
    while (!value.isEmpty() && isAsciiWhitespace(value.back())) {
        value.chop(1);
    }
    return value;
}

QByteArrayView trimAsciiSpaces(QByteArrayView value) noexcept
{
    while (!value.isEmpty() && value.front() == ' ') value = value.sliced(1);
    while (!value.isEmpty() && value.back() == ' ') value.chop(1);
    return value;
}

std::expected<QString, GhosttyApplicationIpcError>
decodeUtf8(QByteArrayView value, QStringView context)
{
    if (value.contains('\0')) {
        return std::unexpected(
            error(QStringLiteral("%1 contains an embedded NUL").arg(context)));
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString decoded = decoder(value);
    if (decoder.hasError()) {
        return std::unexpected(
            error(QStringLiteral("%1 is not valid UTF-8").arg(context)));
    }
    return decoded;
}

std::expected<QByteArray, GhosttyApplicationIpcError>
encodeUtf8(QStringView value, QStringView context)
{
    if (value.contains(u'\0')) {
        return std::unexpected(
            error(QStringLiteral("%1 contains an embedded NUL").arg(context)));
    }
    for (qsizetype index = 0; index < value.size(); ++index) {
        const QChar character = value.at(index);
        if (character.isHighSurrogate()) {
            if (++index >= value.size() || !value.at(index).isLowSurrogate()) {
                return std::unexpected(error(
                    QStringLiteral("%1 is not valid Unicode").arg(context)));
            }
        } else if (character.isLowSurrogate()) {
            return std::unexpected(
                error(QStringLiteral("%1 is not valid Unicode").arg(context)));
        }
    }
    QStringEncoder encoder(QStringEncoder::Utf8);
    QByteArray encoded = encoder(value);
    if (encoder.hasError()) {
        return std::unexpected(
            error(QStringLiteral("%1 is not valid Unicode").arg(context)));
    }
    return encoded;
}

QString objectPathForApplicationId(QString applicationId)
{
    applicationId.prepend(u'/');
    applicationId.replace(u'.', u'/');
    applicationId.replace(u'-', u'_');
    return applicationId;
}

std::expected<QString, GhosttyApplicationIpcError>
validatedApplicationId(QByteArrayView value, QStringView source)
{
    if (!isValidApplicationId(value)) {
        return std::unexpected(error(
            QStringLiteral("%1 is not a valid GApplication application ID")
                .arg(source)));
    }
    return QString::fromLatin1(value);
}

std::expected<QString, GhosttyApplicationIpcError>
canonicalPath(QStringView path, QStringView baseDirectory,
              QStringView description)
{
    if (path.isEmpty()) {
        return std::unexpected(
            error(QStringLiteral("%1 is empty").arg(description)));
    }
    QString candidate = path.toString();
    if (QDir::isRelativePath(candidate)) {
        if (baseDirectory.isEmpty()) {
            return std::unexpected(
                error(QStringLiteral("The caller working directory is empty")));
        }
        candidate = QDir(baseDirectory.toString()).filePath(candidate);
    }
    const QString canonical = QFileInfo(candidate).canonicalFilePath();
    if (canonical.isEmpty()) {
        return std::unexpected(
            error(QStringLiteral("%1 does not resolve to an existing path: %2")
                      .arg(description, candidate)));
    }
    if (auto encoded = encodeUtf8(canonical, description); !encoded) {
        return std::unexpected(std::move(encoded.error()));
    }
    return canonical;
}

std::expected<QString, GhosttyApplicationIpcError>
canonicalWorkingDirectory(const GhosttyApplicationIpcParseContext &context)
{
    return canonicalPath(context.workingDirectory, {},
                         QStringLiteral("The caller working directory"));
}

std::expected<QString, GhosttyApplicationIpcError>
canonicalExplicitWorkingDirectory(
    QByteArrayView rawPath, const GhosttyApplicationIpcParseContext &context)
{
    auto decoded = decodeUtf8(rawPath, QStringLiteral("Working directory"));
    if (!decoded) return std::unexpected(std::move(decoded.error()));

    QString expanded = std::move(*decoded);
    if (expanded.startsWith(QStringLiteral("~/"))) {
        if (context.homeDirectory.isEmpty()) {
            return std::unexpected(error(QStringLiteral(
                "The home directory is unavailable for working-directory "
                "expansion")));
        }
        expanded = context.homeDirectory + expanded.sliced(1);
    }
    return canonicalPath(expanded, context.workingDirectory,
                         QStringLiteral("Working directory"));
}

std::expected<QList<QByteArray>, GhosttyApplicationIpcError>
copyRawArguments(std::span<char *const> arguments)
{
    QList<QByteArray> copied;
    copied.reserve(static_cast<qsizetype>(arguments.size()));
    for (const char *const argument : arguments) {
        if (argument == nullptr) {
            return std::unexpected(
                error(QStringLiteral("The argument list contains null")));
        }
        copied.emplaceBack(argument,
                           static_cast<qsizetype>(std::strlen(argument)));
    }
    return copied;
}

std::expected<TerminalCommand, GhosttyApplicationIpcError>
parseGhosttyCommand(QStringView value)
{
    auto encoded = encodeUtf8(value, QStringLiteral("Command"));
    if (!encoded) return std::unexpected(std::move(encoded.error()));

    QByteArrayView trimmed = trimAsciiSpaces(*encoded);
    if (trimmed.isEmpty()) {
        return std::unexpected(
            error(QStringLiteral("Command must not be empty")));
    }

    enum class Kind {
        Shell,
        Direct,
    };
    Kind kind = Kind::Shell;
    QByteArrayView command = trimmed;
    const qsizetype colon = trimmed.indexOf(':');
    if (colon >= 0) {
        const QByteArrayView prefix = trimmed.first(colon);
        if (prefix == "direct") {
            kind = Kind::Direct;
            command = trimmed.sliced(colon + 1);
        } else if (prefix == "shell") {
            command = trimmed.sliced(colon + 1);
        }
    }
    command = trimAsciiSpaces(command);
    if (kind == Kind::Shell) {
        return TerminalCommand::shell(command.toByteArray());
    }

    QVector<QByteArray> arguments;
    // std.mem.splitScalar yields one empty item for an empty input and
    // preserves empty items between adjacent separators.
    qsizetype begin = 0;
    while (true) {
        const qsizetype separator = command.indexOf(' ', begin);
        if (separator < 0) {
            arguments.emplaceBack(command.sliced(begin).toByteArray());
            break;
        }
        arguments.emplaceBack(
            command.sliced(begin, separator - begin).toByteArray());
        begin = separator + 1;
    }
    return TerminalCommand::direct(std::move(arguments));
}

std::optional<GhosttyShellIntegrationMode>
parseShellIntegration(QByteArrayView value)
{
    value = trimAsciiWhitespace(value);
    if (value == "none") return GhosttyShellIntegrationMode::None;
    if (value == "detect") return GhosttyShellIntegrationMode::Detect;
    if (value == "bash") return GhosttyShellIntegrationMode::Bash;
    if (value == "elvish") return GhosttyShellIntegrationMode::Elvish;
    if (value == "fish") return GhosttyShellIntegrationMode::Fish;
    if (value == "nushell") return GhosttyShellIntegrationMode::Nushell;
    if (value == "zsh") return GhosttyShellIntegrationMode::Zsh;
    return std::nullopt;
}

std::expected<quint64, GhosttyApplicationIpcError>
parseSurfaceId(QByteArrayView value)
{
    const QByteArray text = trimAsciiWhitespace(value).toByteArray();
    bool valid = false;
    const quint64 parsed = text.toULongLong(&valid, 0);
    if (!valid || text.startsWith('-')) {
        return std::unexpected(error(
            QStringLiteral("Surface ID is not an unsigned 64-bit value")));
    }
    return parsed;
}

std::expected<void, GhosttyApplicationIpcError>
validateRequest(const GhosttyApplicationIpcRequest &request)
{
    auto encodedId =
        encodeUtf8(request.applicationId, QStringLiteral("Application ID"));
    if (!encodedId) return std::unexpected(std::move(encodedId.error()));
    if (!isValidApplicationId(*encodedId)
        || request.objectPath
            != objectPathForApplicationId(request.applicationId)) {
        return std::unexpected(
            error(QStringLiteral("The IPC request has an invalid application "
                                 "identity")));
    }

    const bool newTab = request.actionName == QLatin1StringView(NewTabAction)
        && request.newTabParameter.has_value()
        && !request.stringArrayParameter.has_value();
    const bool newWindow =
        request.actionName == QLatin1StringView(NewWindowCommandAction)
        && request.stringArrayParameter.has_value()
        && !request.newTabParameter.has_value();
    const bool toggle =
        request.actionName == QLatin1StringView(ToggleQuickTerminalAction)
        && !request.stringArrayParameter.has_value()
        && !request.newTabParameter.has_value();
    if (!newTab && !newWindow && !toggle) {
        return std::unexpected(error(
            QStringLiteral("The IPC request has an invalid action payload")));
    }
    if (request.stringArrayParameter) {
        for (const QString &argument : *request.stringArrayParameter) {
            if (auto encoded =
                    encodeUtf8(argument, QStringLiteral("Forwarded argument"));
                !encoded) {
                return std::unexpected(std::move(encoded.error()));
            }
        }
    }
    if (request.newTabParameter) {
        for (const QString &argument : request.newTabParameter->arguments) {
            if (auto encoded =
                    encodeUtf8(argument, QStringLiteral("Forwarded argument"));
                !encoded) {
                return std::unexpected(std::move(encoded.error()));
            }
        }
    }
    return {};
}

} // namespace

GhosttyApplicationIpcParseContext
GhosttyApplicationIpcParseContext::fromProcess(QString defaultApplicationId)
{
    return {
        .defaultApplicationId = std::move(defaultApplicationId),
        .workingDirectory = QDir::currentPath(),
        .homeDirectory = QDir::homePath(),
        .surfaceIdEnvironment = qgetenv("GHOSTTY_SURFACE_ID"),
    };
}

std::expected<GhosttyApplicationIpcRequest, GhosttyApplicationIpcError>
parseGhosttyApplicationIpcRequest(
    GhosttyApplicationIpcAction selectedAction,
    const QList<QByteArray> &arguments,
    const GhosttyApplicationIpcParseContext &context)
{
    if (arguments.isEmpty()) {
        return std::unexpected(
            error(QStringLiteral("The argument list must include argv[0]")));
    }
    for (const QByteArray &argument : arguments) {
        if (argument.contains('\0')) {
            return std::unexpected(error(
                QStringLiteral("The argument list contains an embedded NUL")));
        }
    }

    auto encodedDefault = encodeUtf8(context.defaultApplicationId,
                                     QStringLiteral("Default application ID"));
    if (!encodedDefault) {
        return std::unexpected(std::move(encodedDefault.error()));
    }
    auto applicationId = validatedApplicationId(
        *encodedDefault, QStringLiteral("The default application ID"));
    if (!applicationId) {
        return std::unexpected(std::move(applicationId.error()));
    }

    const QByteArrayView selectedArgument = [&] {
        switch (selectedAction) {
        case GhosttyApplicationIpcAction::NewTab:
            return QByteArrayView(NewTabArgument);
        case GhosttyApplicationIpcAction::NewWindow:
            return QByteArrayView(NewWindowArgument);
        case GhosttyApplicationIpcAction::ToggleQuickTerminal:
            return QByteArrayView(ToggleQuickTerminalArgument);
        }
        Q_UNREACHABLE_RETURN(QByteArrayView{});
    }();
    qsizetype actionCount = 0;
    for (const QByteArray &argument : arguments | std::views::drop(1)) {
        if (selectedAction != GhosttyApplicationIpcAction::ToggleQuickTerminal
            && argument == "-e") {
            break;
        }
        if (argument == selectedArgument) ++actionCount;
    }
    if (actionCount != 1) {
        return std::unexpected(error(
            QStringLiteral("The selected Ghostty action must occur exactly "
                           "once in argv")));
    }

    if (selectedAction == GhosttyApplicationIpcAction::ToggleQuickTerminal) {
        return GhosttyApplicationIpcRequest{
            .applicationId = *applicationId,
            .objectPath = objectPathForApplicationId(*applicationId),
            .actionName = QString::fromLatin1(ToggleQuickTerminalAction),
            .stringArrayParameter = std::nullopt,
            .newTabParameter = std::nullopt,
        };
    }

    QStringList forwarded;
    forwarded.reserve(arguments.size());
    std::optional<QByteArray> customClass;
    quint64 surfaceId = 0;
    if (selectedAction == GhosttyApplicationIpcAction::NewTab
        && !context.surfaceIdEnvironment.isEmpty()) {
        if (auto parsed = parseSurfaceId(context.surfaceIdEnvironment)) {
            surfaceId = *parsed;
        }
    }
    bool executeSeen = false;
    bool concreteWorkingDirectorySeen = false;

    for (const QByteArray &argument : arguments.sliced(1)) {
        if (executeSeen) {
            auto decoded =
                decodeUtf8(argument, QStringLiteral("Forwarded argument"));
            if (!decoded) return std::unexpected(std::move(decoded.error()));
            forwarded.emplaceBack(std::move(*decoded));
            continue;
        }
        if (argument == selectedArgument) continue;
        if (argument == "-e") {
            executeSeen = true;
            forwarded.emplaceBack(QStringLiteral("-e"));
            continue;
        }
        if (argument.startsWith("--class=")) {
            customClass =
                trimAsciiWhitespace(QByteArrayView(argument).sliced(8))
                    .toByteArray();
            continue;
        }
        if (selectedAction == GhosttyApplicationIpcAction::NewTab
            && argument.startsWith("--surface-id=")) {
            auto parsed = parseSurfaceId(QByteArrayView(argument).sliced(13));
            if (!parsed) return std::unexpected(std::move(parsed.error()));
            surfaceId = *parsed;
            continue;
        }
        if (argument.startsWith("--working-directory=")) {
            const QByteArrayView value =
                trimAsciiWhitespace(QByteArrayView(argument).sliced(20));
            if (value == "home" || value == "inherit") {
                auto decoded =
                    decodeUtf8(argument, QStringLiteral("Forwarded argument"));
                if (!decoded) {
                    return std::unexpected(std::move(decoded.error()));
                }
                forwarded.emplaceBack(std::move(*decoded));
                continue;
            }
            auto canonical = canonicalExplicitWorkingDirectory(value, context);
            if (!canonical) {
                return std::unexpected(std::move(canonical.error()));
            }
            forwarded.emplaceBack(
                QStringLiteral("--working-directory=%1").arg(*canonical));
            concreteWorkingDirectorySeen = true;
            continue;
        }

        auto decoded =
            decodeUtf8(argument, QStringLiteral("Forwarded argument"));
        if (!decoded) return std::unexpected(std::move(decoded.error()));
        forwarded.emplaceBack(std::move(*decoded));
    }

    if (customClass) {
        auto customId = validatedApplicationId(
            *customClass, QStringLiteral("The --class value"));
        if (!customId) return std::unexpected(std::move(customId.error()));
        applicationId = std::move(customId);
    }
    if (!concreteWorkingDirectorySeen) {
        auto canonical = canonicalWorkingDirectory(context);
        if (!canonical) {
            return std::unexpected(std::move(canonical.error()));
        }
        forwarded.prepend(
            QStringLiteral("--working-directory=%1").arg(*canonical));
    }

    GhosttyApplicationIpcRequest request{
        .applicationId = *applicationId,
        .objectPath = objectPathForApplicationId(*applicationId),
        .actionName = QString::fromLatin1(
            selectedAction == GhosttyApplicationIpcAction::NewTab
                ? NewTabAction
                : NewWindowCommandAction),
        .stringArrayParameter = std::nullopt,
        .newTabParameter = std::nullopt,
    };
    if (selectedAction == GhosttyApplicationIpcAction::NewTab) {
        request.newTabParameter = GhosttyNewTabIpcParameter{
            .surfaceId = surfaceId,
            .arguments = std::move(forwarded),
        };
    } else {
        request.stringArrayParameter = std::move(forwarded);
    }
    return request;
}

std::expected<GhosttyApplicationIpcRequest, GhosttyApplicationIpcError>
parseGhosttyApplicationIpcRequest(
    GhosttyApplicationIpcAction selectedAction,
    std::span<char *const> arguments,
    const GhosttyApplicationIpcParseContext &context)
{
    auto copied = copyRawArguments(arguments);
    if (!copied) return std::unexpected(std::move(copied.error()));
    return parseGhosttyApplicationIpcRequest(selectedAction, *copied, context);
}

QDBusConnection defaultGhosttyApplicationIpcConnection()
{
    if (!qEnvironmentVariableIsEmpty("DBUS_STARTER_ADDRESS")) {
        return QDBusConnection::connectToBus(
            QDBusConnection::ActivationBus,
            QStringLiteral("ghostty_qt_application_ipc_bus"));
    }
    return QDBusConnection::sessionBus();
}

std::expected<void, GhosttyApplicationIpcError>
sendGhosttyApplicationIpcRequest(const GhosttyApplicationIpcRequest &request,
                                 const QDBusConnection &connection,
                                 std::chrono::milliseconds timeout)
{
    if (auto valid = validateRequest(request); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    if (!connection.isConnected()) {
        return std::unexpected(error(
            QStringLiteral("Could not connect to the D-Bus session bus")));
    }

    QDBusMessage call = QDBusMessage::createMethodCall(
        request.applicationId, request.objectPath,
        QStringLiteral("org.gtk.Actions"), QStringLiteral("Activate"));
    QVariantList parameters;
    if (request.stringArrayParameter) {
        parameters.emplaceBack(
            QVariant::fromValue(*request.stringArrayParameter));
    } else if (request.newTabParameter) {
        qDBusRegisterMetaType<GhosttyNewTabIpcParameter>();
        parameters.emplaceBack(QVariant::fromValue(*request.newTabParameter));
    }
    call << request.actionName << parameters << QVariantMap{};

    constexpr auto MaximumTimeout = static_cast<std::chrono::milliseconds::rep>(
        std::numeric_limits<int>::max());
    const int timeoutMilliseconds = timeout.count() < 0
        ? -1
        : static_cast<int>(std::min(timeout.count(), MaximumTimeout));
    const QDBusMessage reply =
        connection.call(call, QDBus::Block, timeoutMilliseconds);
    if (reply.type() == QDBusMessage::ReplyMessage) return {};

    const QDBusError dbusError(reply);
    const QString detail = dbusError.message().isEmpty()
        ? QStringLiteral("unknown D-Bus error")
        : dbusError.message();
    return std::unexpected(
        error(QStringLiteral("D-Bus action %1 on %2 failed: %3 (%4)")
                  .arg(request.actionName, request.applicationId, detail,
                       dbusError.name())));
}

std::expected<GhosttyNewWindowTransportOverrides, GhosttyApplicationIpcError>
decodeGhosttyNewWindowArguments(const QStringList &arguments)
{
    GhosttyNewWindowTransportOverrides overrides;
    QVector<QByteArray> directArguments;
    bool executeSeen = false;

    for (const QString &argument : arguments) {
        auto encoded =
            encodeUtf8(argument, QStringLiteral("Forwarded argument"));
        if (!encoded) return std::unexpected(std::move(encoded.error()));

        if (executeSeen) {
            directArguments.emplaceBack(std::move(*encoded));
            continue;
        }
        if (argument == QLatin1StringView("-e")) {
            executeSeen = true;
            continue;
        }
        if (argument.startsWith(QLatin1StringView("--command="))) {
            auto parsed = parseGhosttyCommand(QStringView(argument).sliced(10));
            // Ghostty logs an invalid command and retains the previous value.
            if (parsed) overrides.command = std::move(*parsed);
            continue;
        }
        if (argument.startsWith(QLatin1StringView("--shell-integration="))) {
            if (auto parsed = parseShellIntegration(
                    QByteArrayView(*encoded).sliced(20))) {
                overrides.shellIntegration = *parsed;
            }
            continue;
        }
        if (argument.startsWith(QLatin1StringView("--working-directory="))) {
            const QByteArrayView value =
                trimAsciiWhitespace(QByteArrayView(*encoded).sliced(20));
            auto decoded =
                decodeUtf8(value, QStringLiteral("Working directory"));
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            overrides.workingDirectory = std::move(*decoded);
            continue;
        }
        if (argument.startsWith(QLatin1StringView("--title="))) {
            const QByteArrayView value =
                trimAsciiWhitespace(QByteArrayView(*encoded).sliced(8));
            auto decoded = decodeUtf8(value, QStringLiteral("Title"));
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            overrides.titleOverride = std::move(*decoded);
        }
    }

    if (!directArguments.isEmpty()) {
        overrides.command = TerminalCommand::direct(std::move(directArguments));
    }
    return overrides;
}

std::expected<GhosttyNewTabTransportRequest, GhosttyApplicationIpcError>
decodeGhosttyNewTabParameter(const GhosttyNewTabIpcParameter &parameter)
{
    auto overrides = decodeGhosttyNewWindowArguments(parameter.arguments);
    if (!overrides) return std::unexpected(std::move(overrides.error()));
    return GhosttyNewTabTransportRequest{
        .surfaceId = SurfaceId(parameter.surfaceId),
        .overrides = std::move(*overrides),
    };
}
