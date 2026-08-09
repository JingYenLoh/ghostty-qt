#include "ghostty_shell_integration.h"
#include "ghostty_shell_integration_p.h"
#include "unique_file_descriptor.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QSet>
#include <QWaitCondition>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <memory>
#include <ranges>
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr qsizetype kMaximumRequestProtocolBytes = 4 * 1024 * 1024;
// Setup can add command arguments and environment entries to a request that
// was already close to its input limit. Keep the response bounded, but leave
// explicit headroom for that legitimate expansion.
constexpr qsizetype kMaximumResponseProtocolBytes = 8 * 1024 * 1024;
constexpr qsizetype kMaximumHelperDiagnosticBytes = 256 * 1024;
constexpr int kProcessDrainIntervalMilliseconds = 50;
constexpr qsizetype kMaximumCachedEntries = 32;
constexpr qsizetype kMaximumCachedBytes = 8 * 1024 * 1024;
constexpr qsizetype kMaximumCachedResultBytes = 1024 * 1024;
constexpr qsizetype kMaximumInFlightPreparations = 64;

struct OpenedFileStatus {
    quint64 device = 0;
    quint64 inode = 0;
    quint64 mode = 0;
    quint64 user = 0;
    quint64 group = 0;
    quint64 links = 0;
    qint64 size = -1;
    qint64 modifiedSeconds = 0;
    qint64 modifiedNanoseconds = 0;
    qint64 changedSeconds = 0;
    qint64 changedNanoseconds = 0;

    bool operator==(const OpenedFileStatus &) const = default;
};

struct ResourceNodeIdentity {
    QByteArray path;
    int openError = 0;
    std::optional<OpenedFileStatus> status;

    bool operator==(const ResourceNodeIdentity &) const = default;
};

struct ShellIntegrationFilesystemIdentity {
    QString helperAbsolutePath;
    QByteArray helperNativePath;
    OpenedFileStatus helperStatus;
    QVector<std::pair<QByteArray, OpenedFileStatus>> helperRuntimeIdentities;
    QVector<ResourceNodeIdentity> resourceNodes;

    bool operator==(const ShellIntegrationFilesystemIdentity &) const = default;
};

struct ShellIntegrationCacheIdentity {
    ShellIntegrationFilesystemIdentity filesystem;
    QByteArray key;
};

using ShellIntegrationPreparation =
    std::expected<GhosttyShellIntegrationResult, QString>;

struct CachedPreparation {
    GhosttyShellIntegrationResult result;
    qsizetype cost = 0;
    quint64 lastAccess = 0;
};

struct PendingPreparation {
    QWaitCondition ready;
    std::optional<ShellIntegrationPreparation> outcome;
    bool finished = false;
};

struct ShellIntegrationCacheState {
    QMutex mutex;
    QHash<QByteArray, CachedPreparation> successful;
    QHash<QByteArray, std::shared_ptr<PendingPreparation>> pending;
    GhosttyShellIntegrationCacheSnapshot counters;
    quint64 accessSerial = 0;
    qsizetype retainedBytes = 0;
    QString trustedTestHelperPath;
};

ShellIntegrationCacheState &shellIntegrationCacheState()
{
    static ShellIntegrationCacheState state;
    return state;
}

OpenedFileStatus openedFileStatus(const struct stat &status) noexcept
{
    return {
        .device = static_cast<quint64>(status.st_dev),
        .inode = static_cast<quint64>(status.st_ino),
        .mode = static_cast<quint64>(status.st_mode),
        .user = static_cast<quint64>(status.st_uid),
        .group = static_cast<quint64>(status.st_gid),
        .links = static_cast<quint64>(status.st_nlink),
        .size = static_cast<qint64>(status.st_size),
        .modifiedSeconds = static_cast<qint64>(status.st_mtim.tv_sec),
        .modifiedNanoseconds = static_cast<qint64>(status.st_mtim.tv_nsec),
        .changedSeconds = static_cast<qint64>(status.st_ctim.tv_sec),
        .changedNanoseconds = static_cast<qint64>(status.st_ctim.tv_nsec),
    };
}

int openRetryingInterrupts(const QByteArray &path, int flags) noexcept
{
    int descriptor = -1;
    do {
        descriptor = ::open(path.constData(), flags);
    } while (descriptor < 0 && errno == EINTR);
    return descriptor;
}

std::optional<OpenedFileStatus> inspectOpenedFile(int descriptor) noexcept
{
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || status.st_size < 0) {
        return std::nullopt;
    }
    return openedFileStatus(status);
}

std::optional<std::pair<QString, OpenedFileStatus>>
helperIdentity(const QString &helperPath)
{
    // QProcess PATH-search and lexical `..` resolution are observable. Cache
    // only the production contract's absolute spelling and execute that exact
    // spelling rather than normalizing it into a different program path.
    if (helperPath.isEmpty() || !QFileInfo(helperPath).isAbsolute()) {
        return std::nullopt;
    }
    const QString &absolutePath = helperPath;
    const QByteArray nativePath = QFile::encodeName(absolutePath);
    if (nativePath.isEmpty() || nativePath.contains('\0')) return std::nullopt;

    const int descriptor =
        openRetryingInterrupts(nativePath, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (descriptor < 0) return std::nullopt;
    UniqueFileDescriptor file(descriptor);
    const auto status = inspectOpenedFile(file.get());
    if (!status.has_value()
        || (status->mode & static_cast<quint64>(S_IFMT))
            != static_cast<quint64>(S_IFREG)) {
        return std::nullopt;
    }
    if (::access(nativePath.constData(), X_OK) != 0) return std::nullopt;
    return std::pair(absolutePath, *status);
}

std::optional<std::pair<QByteArray, OpenedFileStatus>>
regularFileIdentity(const QString &path)
{
    const QByteArray nativePath = QFile::encodeName(QDir::cleanPath(path));
    if (nativePath.isEmpty() || nativePath.contains('\0')) return std::nullopt;
    const int descriptor =
        openRetryingInterrupts(nativePath, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (descriptor < 0) return std::nullopt;
    UniqueFileDescriptor file(descriptor);
    const auto status = inspectOpenedFile(file.get());
    if (!status.has_value()
        || (status->mode & static_cast<quint64>(S_IFMT))
            != static_cast<quint64>(S_IFREG)) {
        return std::nullopt;
    }
    return std::pair(nativePath, *status);
}

std::optional<QVector<std::pair<QByteArray, OpenedFileStatus>>>
helperRuntimeIdentity(const QString &helperAbsolutePath)
{
    // The production helper has one revision-matched DT_NEEDED dependency on
    // private libghostty. With loader injection disabled below, its build and
    // installed RUNPATHs select one of these two deterministic locations.
    // A deliberately private test seam can trust one standalone protocol
    // fixture. Arbitrary helpers may depend on state this frontend cannot
    // fingerprint, so they always bypass the production cache.
    if (QFileInfo(helperAbsolutePath).fileName()
        != QLatin1StringView("ghostty-qt-config-helper")) {
        ShellIntegrationCacheState &state = shellIntegrationCacheState();
        QMutexLocker locker(&state.mutex);
        if (helperAbsolutePath != state.trustedTestHelperPath) {
            return std::nullopt;
        }
        return QVector<std::pair<QByteArray, OpenedFileStatus>>{};
    }
#if defined(GHOSTTY_QT_CONFIG_HELPER_BUILD_PATH)                               \
    && defined(GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY_BUILD_PATH)                   \
    && defined(GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY_RELATIVE_TO_BINDIR)
    const QFileInfo requested(helperAbsolutePath);
    const QFileInfo buildHelper(
        QStringLiteral(GHOSTTY_QT_CONFIG_HELPER_BUILD_PATH));
    const bool buildHelperSelected =
        requested.absoluteFilePath() == buildHelper.absoluteFilePath()
        || (!requested.canonicalFilePath().isEmpty()
            && requested.canonicalFilePath()
                == buildHelper.canonicalFilePath());
    const QString selectedHelperPath = requested.canonicalFilePath().isEmpty()
        ? requested.absoluteFilePath()
        : requested.canonicalFilePath();
    const QString runtime = buildHelperSelected
        ? QStringLiteral(GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY_BUILD_PATH)
        : QDir(QFileInfo(selectedHelperPath).absolutePath())
              .absoluteFilePath(QStringLiteral(
                  GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY_RELATIVE_TO_BINDIR));
    auto identity = regularFileIdentity(runtime);
    if (!identity.has_value()) return std::nullopt;
    QVector<std::pair<QByteArray, OpenedFileStatus>> result;
    result.append(std::move(*identity));
    return result;
#else
    return std::nullopt;
#endif
}

QByteArray resourceNodePath(QByteArray root, QByteArrayView relative)
{
    if (!root.endsWith('/')) root.append('/');
    root.append(relative.data(), relative.size());
    return root;
}

std::optional<ResourceNodeIdentity> resourceNodeIdentity(QByteArray path,
                                                         bool directory)
{
    const int flags =
        O_RDONLY | O_CLOEXEC | O_NOCTTY | (directory ? O_DIRECTORY : 0);
    const int descriptor = openRetryingInterrupts(path, flags);
    if (descriptor >= 0) {
        UniqueFileDescriptor file(descriptor);
        auto status = inspectOpenedFile(file.get());
        if (!status.has_value()) return std::nullopt;
        return ResourceNodeIdentity{
            .path = std::move(path),
            .openError = 0,
            .status = *status,
        };
    }

    const int openError = errno;
    struct stat status{};
    if (::stat(path.constData(), &status) == 0 && status.st_size >= 0) {
        // ENOTDIR is a stable, behavior-affecting mismatch for a directory
        // probe. Other open failures (permissions, descriptor pressure, I/O)
        // are not safe cache identities even when stat happens to succeed.
        if (openError != ENOTDIR) return std::nullopt;
        return ResourceNodeIdentity{
            .path = std::move(path),
            .openError = openError,
            .status = openedFileStatus(status),
        };
    }
    const int statusError = errno;
    if ((openError != ENOENT && openError != ENOTDIR)
        || (statusError != ENOENT && statusError != ENOTDIR)) {
        return std::nullopt;
    }
    return ResourceNodeIdentity{
        .path = std::move(path),
        .openError = openError,
        .status = std::nullopt,
    };
}

bool hasLoaderInjection(const QProcessEnvironment &environment)
{
    return environment.contains(QStringLiteral("LD_PRELOAD"))
        || environment.contains(QStringLiteral("LD_LIBRARY_PATH"))
        || environment.contains(QStringLiteral("LD_AUDIT"));
}

std::optional<ShellIntegrationFilesystemIdentity>
filesystemIdentity(const GhosttyShellIntegrationProcessOptions &options,
                   const GhosttyShellIntegrationRequest &request)
{
    if (options.environment.inheritsFromParent()
        || hasLoaderInjection(options.environment)
        || (request.mode != GhosttyShellIntegrationMode::None
            && !request.resourceDirectory.isEmpty()
            && !options.environment.contains(QStringLiteral("HOME")))) {
        return std::nullopt;
    }

    const auto helper = helperIdentity(options.helperPath);
    if (!helper.has_value()) return std::nullopt;
    const auto helperRuntime = helperRuntimeIdentity(helper->first);
    if (!helperRuntime.has_value()) return std::nullopt;
    ShellIntegrationFilesystemIdentity result{
        .helperAbsolutePath = helper->first,
        .helperNativePath = QFile::encodeName(helper->first),
        .helperStatus = helper->second,
        .helperRuntimeIdentities = std::move(*helperRuntime),
        .resourceNodes = {},
    };

    if (request.mode == GhosttyShellIntegrationMode::None
        || request.resourceDirectory.isEmpty()) {
        return result;
    }

    const std::array<std::pair<QByteArrayView, bool>, 3> nodes{
        std::pair{QByteArrayView("shell-integration"), true},
        std::pair{QByteArrayView("shell-integration/zsh"), true},
        std::pair{QByteArrayView("shell-integration/bash/ghostty.bash"), false},
    };
    result.resourceNodes.reserve(static_cast<qsizetype>(nodes.size()));
    for (const auto &[relative, directory] : nodes) {
        auto identity = resourceNodeIdentity(
            resourceNodePath(request.resourceDirectory, relative), directory);
        if (!identity.has_value()) return std::nullopt;
        result.resourceNodes.append(std::move(*identity));
    }
    return result;
}

void addUnsigned(QCryptographicHash &hash, quint64 value)
{
    std::array<char, sizeof(value)> bytes{};
    for (qsizetype index = 0; index < std::ssize(bytes); ++index) {
        const auto shift =
            static_cast<unsigned int>(8 * (std::ssize(bytes) - index - 1));
        bytes.at(static_cast<size_t>(index)) =
            static_cast<char>((value >> shift) & 0xffU);
    }
    hash.addData(QByteArrayView(bytes.data(), std::ssize(bytes)));
}

void addSigned(QCryptographicHash &hash, qint64 value)
{
    addUnsigned(hash, static_cast<quint64>(value));
}

void addBytes(QCryptographicHash &hash, QByteArrayView bytes)
{
    addUnsigned(hash, static_cast<quint64>(bytes.size()));
    hash.addData(bytes);
}

void addString(QCryptographicHash &hash, QStringView value)
{
    addUnsigned(hash, static_cast<quint64>(value.size()));
    for (const QChar character : value) {
        const quint16 codeUnit = character.unicode();
        const std::array<char, 2> bytes{
            static_cast<char>((codeUnit >> 8U) & 0xffU),
            static_cast<char>(codeUnit & 0xffU),
        };
        hash.addData(QByteArrayView(bytes.data(), std::ssize(bytes)));
    }
}

void addFileStatus(QCryptographicHash &hash, const OpenedFileStatus &status)
{
    addUnsigned(hash, status.device);
    addUnsigned(hash, status.inode);
    addUnsigned(hash, status.mode);
    addUnsigned(hash, status.user);
    addUnsigned(hash, status.group);
    addUnsigned(hash, status.links);
    addSigned(hash, status.size);
    addSigned(hash, status.modifiedSeconds);
    addSigned(hash, status.modifiedNanoseconds);
    addSigned(hash, status.changedSeconds);
    addSigned(hash, status.changedNanoseconds);
}

QByteArray cacheKey(const QByteArray &serialized,
                    const GhosttyShellIntegrationProcessOptions &options,
                    const ShellIntegrationFilesystemIdentity &filesystem)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addBytes(hash, QByteArrayView("ghostty-qt-shell-integration-cache-v1"));
    addBytes(hash, serialized);
    addSigned(hash, std::max(1, options.timeoutMilliseconds));

    QStringList environmentKeys = options.environment.keys();
    std::ranges::sort(environmentKeys);
    addUnsigned(hash, static_cast<quint64>(environmentKeys.size()));
    for (const QString &key : environmentKeys) {
        addString(hash, key);
        addString(hash, options.environment.value(key));
    }

    addBytes(hash, filesystem.helperNativePath);
    addFileStatus(hash, filesystem.helperStatus);
    addUnsigned(
        hash, static_cast<quint64>(filesystem.helperRuntimeIdentities.size()));
    for (const auto &[path, status] : filesystem.helperRuntimeIdentities) {
        addBytes(hash, path);
        addFileStatus(hash, status);
    }
    addUnsigned(hash, static_cast<quint64>(filesystem.resourceNodes.size()));
    for (const ResourceNodeIdentity &node : filesystem.resourceNodes) {
        addBytes(hash, node.path);
        addSigned(hash, node.openError);
        addUnsigned(hash, node.status.has_value() ? 1U : 0U);
        if (node.status.has_value()) addFileStatus(hash, *node.status);
    }
    return hash.result();
}

std::optional<ShellIntegrationCacheIdentity>
makeCacheIdentity(const GhosttyShellIntegrationProcessOptions &options,
                  const GhosttyShellIntegrationRequest &request,
                  const QByteArray &serialized)
{
    auto filesystem = filesystemIdentity(options, request);
    if (!filesystem.has_value()) return std::nullopt;
    QByteArray key = cacheKey(serialized, options, *filesystem);
    return ShellIntegrationCacheIdentity{
        .filesystem = std::move(*filesystem),
        .key = std::move(key),
    };
}

qsizetype retainedResultCost(const GhosttyShellIntegrationResult &result)
{
    qsizetype cost = static_cast<qsizetype>(sizeof(result));
    const auto add = [&cost](qsizetype value) {
        if (value < 0 || cost > std::numeric_limits<qsizetype>::max() - value) {
            cost = std::numeric_limits<qsizetype>::max();
        } else {
            cost += value;
        }
    };
    add(result.command.shellCommand.size());
    add(result.command.directArguments.size()
        * static_cast<qsizetype>(sizeof(QByteArray)));
    for (const QByteArray &argument : result.command.directArguments) {
        add(argument.size());
    }
    add(result.environment.size()
        * static_cast<qsizetype>(sizeof(TerminalEnvironmentEntry)));
    for (const TerminalEnvironmentEntry &entry : result.environment) {
        add(entry.key.size());
        add(entry.value.size());
    }
    return cost;
}

void evictLeastRecentlyUsed(ShellIntegrationCacheState &state)
{
    auto victim = state.successful.end();
    for (auto iterator = state.successful.begin();
         iterator != state.successful.end(); ++iterator) {
        if (victim == state.successful.end()
            || iterator->lastAccess < victim->lastAccess) {
            victim = iterator;
        }
    }
    if (victim == state.successful.end()) return;
    state.retainedBytes -= victim->cost;
    state.successful.erase(victim);
    ++state.counters.evictions;
}

void retainSuccessfulPreparation(ShellIntegrationCacheState &state,
                                 const QByteArray &key,
                                 const GhosttyShellIntegrationResult &result)
{
    const qsizetype cost = retainedResultCost(result);
    if (cost > kMaximumCachedResultBytes) {
        ++state.counters.oversizedResults;
        return;
    }
    if (const auto existing = state.successful.find(key);
        existing != state.successful.end()) {
        state.retainedBytes -= existing->cost;
        state.successful.erase(existing);
    }
    while (!state.successful.isEmpty()
           && (state.successful.size() >= kMaximumCachedEntries
               || state.retainedBytes > kMaximumCachedBytes - cost)) {
        evictLeastRecentlyUsed(state);
    }
    state.retainedBytes += cost;
    state.successful.insert(key,
                            CachedPreparation{
                                .result = result,
                                .cost = cost,
                                .lastAccess = ++state.accessSerial,
                            });
    ++state.counters.insertions;
}

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

namespace {

ShellIntegrationPreparation runGhosttyShellIntegrationHelper(
    const GhosttyShellIntegrationProcessOptions &options,
    const QByteArray &serialized)
{
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
    while (requestOffset < serialized.size()) {
        if (deadline.hasExpired()) {
            process.kill();
            (void)process.waitForFinished(1'000);
            return std::unexpected(
                QStringLiteral("Shell integration helper timed out after %1 ms")
                    .arg(std::max(1, options.timeoutMilliseconds)));
        }
        const qint64 accepted = process.write(
            serialized.constData() + requestOffset,
            static_cast<qint64>(serialized.size() - requestOffset));
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

void removeCachedPreparation(ShellIntegrationCacheState &state,
                             const QByteArray &key)
{
    const auto cached = state.successful.find(key);
    if (cached == state.successful.end()) return;
    state.retainedBytes -= cached->cost;
    state.successful.erase(cached);
}

ShellIntegrationPreparation
runUncachedBypass(const GhosttyShellIntegrationProcessOptions &options,
                  const QByteArray &serialized)
{
    ShellIntegrationCacheState &state = shellIntegrationCacheState();
    {
        QMutexLocker locker(&state.mutex);
        ++state.counters.bypasses;
        ++state.counters.launches;
    }
    return runGhosttyShellIntegrationHelper(options, serialized);
}

} // namespace

std::expected<GhosttyShellIntegrationResult, QString>
prepareGhosttyShellIntegration(
    const GhosttyShellIntegrationProcessOptions &options,
    const GhosttyShellIntegrationRequest &request)
{
    auto serialized = serializeGhosttyShellIntegrationRequest(request);
    if (!serialized) return std::unexpected(std::move(serialized.error()));
    return runGhosttyShellIntegrationHelper(options, *serialized);
}

std::expected<GhosttyShellIntegrationResult, QString>
prepareCachedGhosttyShellIntegration(
    const GhosttyShellIntegrationProcessOptions &options,
    const GhosttyShellIntegrationRequest &request)
{
    auto serialized = serializeGhosttyShellIntegrationRequest(request);
    if (!serialized) return std::unexpected(std::move(serialized.error()));

    GhosttyShellIntegrationProcessOptions normalizedOptions = options;
    for (int identityAttempt = 0; identityAttempt < 2; ++identityAttempt) {
        auto identity =
            makeCacheIdentity(normalizedOptions, request, *serialized);
        if (!identity.has_value()) {
            return runUncachedBypass(normalizedOptions, *serialized);
        }
        normalizedOptions.helperPath = identity->filesystem.helperAbsolutePath;

        ShellIntegrationCacheState &state = shellIntegrationCacheState();
        std::optional<GhosttyShellIntegrationResult> cachedResult;
        std::shared_ptr<PendingPreparation> flight;
        bool leader = false;
        bool capacityBypass = false;
        {
            QMutexLocker locker(&state.mutex);
            const auto cached = state.successful.find(identity->key);
            if (cached != state.successful.end()) {
                cached->lastAccess = ++state.accessSerial;
                cachedResult = cached->result;
            } else if (const auto pending =
                           state.pending.constFind(identity->key);
                       pending != state.pending.cend()) {
                flight = *pending;
                ++state.counters.coalesced;
                while (!flight->finished) {
                    flight->ready.wait(&state.mutex);
                }
                Q_ASSERT(flight->outcome.has_value());
                return *flight->outcome;
            } else if (state.pending.size() >= kMaximumInFlightPreparations) {
                capacityBypass = true;
            } else {
                flight = std::make_shared<PendingPreparation>();
                state.pending.insert(identity->key, flight);
                ++state.counters.misses;
                ++state.counters.launches;
                leader = true;
            }
        }

        if (capacityBypass) {
            return runUncachedBypass(normalizedOptions, *serialized);
        }

        if (cachedResult.has_value()) {
            const auto finalFilesystem =
                filesystemIdentity(normalizedOptions, request);
            if (finalFilesystem.has_value()
                && *finalFilesystem == identity->filesystem) {
                QMutexLocker locker(&state.mutex);
                ++state.counters.hits;
                return std::move(*cachedResult);
            }

            {
                QMutexLocker locker(&state.mutex);
                removeCachedPreparation(state, identity->key);
                ++state.counters.unstableIdentities;
            }
            if (identityAttempt == 0) continue;
            return runUncachedBypass(normalizedOptions, *serialized);
        }

        Q_ASSERT(leader);
        ShellIntegrationPreparation outcome =
            runGhosttyShellIntegrationHelper(normalizedOptions, *serialized);
        const auto finalFilesystem =
            filesystemIdentity(normalizedOptions, request);
        const bool stable = finalFilesystem.has_value()
            && *finalFilesystem == identity->filesystem;
        {
            QMutexLocker locker(&state.mutex);
            if (stable && outcome.has_value()) {
                retainSuccessfulPreparation(state, identity->key, *outcome);
            } else if (!stable) {
                ++state.counters.unstableIdentities;
            }

            // The shared flight outlives removal from the lookup map, so every
            // waiter already holding it observes this exact outcome.
            flight->outcome = outcome;
            flight->finished = true;
            state.pending.remove(identity->key);
            flight->ready.wakeAll();
        }
        return outcome;
    }

    Q_UNREACHABLE_RETURN(std::unexpected(
        QStringLiteral("Shell integration cache identity did not stabilize")));
}

GhosttyShellIntegrationCacheSnapshot
ghosttyShellIntegrationCacheSnapshotForTest()
{
    ShellIntegrationCacheState &state = shellIntegrationCacheState();
    QMutexLocker locker(&state.mutex);
    GhosttyShellIntegrationCacheSnapshot result = state.counters;
    result.entries = state.successful.size();
    result.retainedBytes = state.retainedBytes;
    result.inFlight = state.pending.size();
    return result;
}

bool setGhosttyShellIntegrationTrustedHelperForTest(const QString &absolutePath)
{
    if (!QFileInfo(absolutePath).isAbsolute()) return false;
    ShellIntegrationCacheState &state = shellIntegrationCacheState();
    QMutexLocker locker(&state.mutex);
    if (!state.pending.isEmpty() || !state.successful.isEmpty()) return false;
    state.trustedTestHelperPath = absolutePath;
    return true;
}

bool resetGhosttyShellIntegrationCacheForTest()
{
    ShellIntegrationCacheState &state = shellIntegrationCacheState();
    QMutexLocker locker(&state.mutex);
    if (!state.pending.isEmpty()) return false;
    state.successful.clear();
    state.counters = {};
    state.accessSerial = 0;
    state.retainedBytes = 0;
    state.trustedTestHelperPath.clear();
    return true;
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
