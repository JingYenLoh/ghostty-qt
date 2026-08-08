#include "renderer_graphics_library_manifest.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <map>
#include <set>
#include <tuple>

#include <sys/stat.h>

namespace {

constexpr qsizetype ProcMapsFieldCount = 5;

QString backendName(RendererGraphicsBackend backend)
{
    switch (backend) {
    case RendererGraphicsBackend::OpenGL: return QStringLiteral("opengl");
    case RendererGraphicsBackend::Vulkan: return QStringLiteral("vulkan");
    }
    return QStringLiteral("opengl");
}

QString roleName(RendererGraphicsLibraryRole role)
{
    switch (role) {
    case RendererGraphicsLibraryRole::Driver: return QStringLiteral("driver");
    case RendererGraphicsLibraryRole::VendorDispatch:
        return QStringLiteral("vendor_dispatch");
    case RendererGraphicsLibraryRole::ApiLoader:
        return QStringLiteral("api_loader");
    case RendererGraphicsLibraryRole::Layer: return QStringLiteral("layer");
    case RendererGraphicsLibraryRole::Compiler:
        return QStringLiteral("compiler");
    }
    return QStringLiteral("driver");
}

QString decodeProcMapsPath(QByteArrayView encoded)
{
    QByteArray decoded;
    decoded.reserve(encoded.size());
    for (qsizetype index = 0; index < encoded.size();) {
        if (encoded.at(index) == '\\' && index + 3 < encoded.size()) {
            const auto octalDigit = [](char value) {
                return value >= '0' && value <= '7';
            };
            if (octalDigit(encoded.at(index + 1))
                && octalDigit(encoded.at(index + 2))
                && octalDigit(encoded.at(index + 3))) {
                const int value = (encoded.at(index + 1) - '0') * 64
                    + (encoded.at(index + 2) - '0') * 8
                    + (encoded.at(index + 3) - '0');
                decoded.append(static_cast<char>(value));
                index += 4;
                continue;
            }
        }
        decoded.append(encoded.at(index));
        ++index;
    }
    return QFile::decodeName(decoded);
}

QString mappedFilePath(QString path)
{
    constexpr QStringView DeletedSuffix = u" (deleted)";
    if (path.endsWith(DeletedSuffix)) path.chop(DeletedSuffix.size());
    return path;
}

bool parseHex(QByteArrayView value, quint64 *result)
{
    if (value.isEmpty() || !std::ranges::all_of(value, [](char character) {
            return (character >= '0' && character <= '9')
                || (character >= 'a' && character <= 'f')
                || (character >= 'A' && character <= 'F');
        })) {
        return false;
    }
    bool ok = false;
    const qulonglong parsed = value.toULongLong(&ok, 16);
    if (ok && result != nullptr) *result = parsed;
    return ok;
}

bool parseDecimal(QByteArrayView value, quint64 *result)
{
    if (value.isEmpty() || !std::ranges::all_of(value, [](char character) {
            return character >= '0' && character <= '9';
        })) {
        return false;
    }
    bool ok = false;
    const qulonglong parsed = value.toULongLong(&ok, 10);
    if (ok && result != nullptr) *result = parsed;
    return ok;
}

struct HashedFile {
    quint64 size = 0;
    QByteArray sha256;
};

struct DeviceIdentity {
    quint64 major = 0;
    quint64 minor = 0;
};

using MountDeviceMap = std::map<quint64, DeviceIdentity>;

using CanonicalEntryIdentity =
    std::tuple<QByteArray, QByteArray, quint64, QByteArray>;

CanonicalEntryIdentity
canonicalEntryIdentity(const RendererGraphicsLibraryEntry &entry)
{
    return {
        roleName(entry.role).toUtf8(),
        entry.name.toUtf8(),
        entry.size,
        entry.sha256.toLower(),
    };
}

bool canonicalEntryLess(const RendererGraphicsLibraryEntry &left,
                        const RendererGraphicsLibraryEntry &right)
{
    return canonicalEntryIdentity(left) < canonicalEntryIdentity(right);
}

std::optional<quint64> openedFileMountId(int descriptor,
                                         const QString &procSelfRoot,
                                         QString *diagnostic)
{
    QFile fdInfo(
        QStringLiteral("%1/fdinfo/%2").arg(procSelfRoot).arg(descriptor));
    if (!fdInfo.open(QIODevice::ReadOnly)) {
        if (diagnostic != nullptr) {
            *diagnostic =
                QStringLiteral("could not read opened-file mount ID: %1")
                    .arg(fdInfo.errorString());
        }
        return std::nullopt;
    }

    std::optional<quint64> result;
    for (const QByteArray &line : fdInfo.readAll().split('\n')) {
        constexpr QByteArrayView Prefix("mnt_id:");
        if (!QByteArrayView(line).startsWith(Prefix)) continue;
        quint64 mountId = 0;
        if (result.has_value()
            || !parseDecimal(
                QByteArrayView(line).sliced(Prefix.size()).trimmed(), &mountId)
            || mountId == 0) {
            if (diagnostic != nullptr) {
                *diagnostic = QStringLiteral("invalid opened-file mount ID");
            }
            return std::nullopt;
        }
        result = mountId;
    }
    if (!result.has_value() && diagnostic != nullptr) {
        *diagnostic = QStringLiteral("opened-file mount ID was unavailable");
    }
    return result;
}

std::optional<MountDeviceMap> readMountDeviceMap(const QString &procSelfRoot,
                                                 QString *diagnostic)
{
    QFile mountInfo(QStringLiteral("%1/mountinfo").arg(procSelfRoot));
    if (!mountInfo.open(QIODevice::ReadOnly)) {
        if (diagnostic != nullptr) {
            *diagnostic = QStringLiteral("could not read process mountinfo: %1")
                              .arg(mountInfo.errorString());
        }
        return std::nullopt;
    }

    MountDeviceMap result;
    qsizetype lineNumber = 0;
    for (const QByteArray &line : mountInfo.readAll().split('\n')) {
        ++lineNumber;
        if (line.isEmpty()) continue;
        const QList<QByteArray> fields = line.simplified().split(' ');
        quint64 mountId = 0;
        quint64 parentMountId = 0;
        quint64 deviceMajor = 0;
        quint64 deviceMinor = 0;
        const QList<QByteArray> device =
            fields.size() >= 3 ? fields.at(2).split(':') : QList<QByteArray>{};
        const qsizetype separator = fields.indexOf(QByteArrayLiteral("-"), 6);
        if (fields.size() < 10 || separator < 6 || fields.size() < separator + 4
            || device.size() != 2 || !parseDecimal(fields.at(0), &mountId)
            || !parseDecimal(fields.at(1), &parentMountId) || mountId == 0
            || parentMountId == 0 || !parseDecimal(device.at(0), &deviceMajor)
            || !parseDecimal(device.at(1), &deviceMinor)
            || !result
                    .emplace(mountId,
                             DeviceIdentity{
                                 .major = deviceMajor,
                                 .minor = deviceMinor,
                             })
                    .second) {
            if (diagnostic != nullptr) {
                *diagnostic =
                    QStringLiteral("invalid process mountinfo at line %1")
                        .arg(lineNumber);
            }
            return std::nullopt;
        }
    }
    if (result.empty()) {
        if (diagnostic != nullptr) {
            *diagnostic = QStringLiteral("process mountinfo was empty");
        }
        return std::nullopt;
    }
    return result;
}

bool sameTimestamp(const struct timespec &left, const struct timespec &right)
{
    return left.tv_sec == right.tv_sec && left.tv_nsec == right.tv_nsec;
}

std::optional<HashedFile>
hashMappedFile(const QString &path,
               const RendererGraphicsLibraryMapping &mapping,
               const QString &procSelfRoot, QString *diagnostic)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (diagnostic != nullptr) {
            *diagnostic =
                QStringLiteral("open failed: %1").arg(file.errorString());
        }
        return std::nullopt;
    }

    struct stat status{};
    if (::fstat(file.handle(), &status) != 0) {
        if (diagnostic != nullptr) {
            *diagnostic =
                QStringLiteral("fstat failed: %1")
                    .arg(QString::fromLocal8Bit(std::strerror(errno)));
        }
        return std::nullopt;
    }
    if (!S_ISREG(status.st_mode)
        || static_cast<quint64>(status.st_ino) != mapping.inode) {
        if (diagnostic != nullptr) {
            *diagnostic = QStringLiteral("mapped inode identity changed");
        }
        return std::nullopt;
    }
    if (status.st_size <= 0) {
        if (diagnostic != nullptr) {
            *diagnostic = QStringLiteral("mapped file was empty");
        }
        return std::nullopt;
    }

    QString mountDiagnostic;
    const auto mountId =
        openedFileMountId(file.handle(), procSelfRoot, &mountDiagnostic);
    if (!mountId.has_value()) {
        if (diagnostic != nullptr) *diagnostic = mountDiagnostic;
        return std::nullopt;
    }
    QString mountInfoDiagnostic;
    const auto mountDevices =
        readMountDeviceMap(procSelfRoot, &mountInfoDiagnostic);
    if (!mountDevices.has_value()) {
        if (diagnostic != nullptr) *diagnostic = mountInfoDiagnostic;
        return std::nullopt;
    }
    const auto mountedDevice = mountDevices->find(*mountId);
    if (mountedDevice == mountDevices->end()) {
        if (diagnostic != nullptr) {
            *diagnostic =
                QStringLiteral("opened-file mount was absent from mountinfo");
        }
        return std::nullopt;
    }
    if (mountedDevice->second.major != mapping.deviceMajor
        || mountedDevice->second.minor != mapping.deviceMinor) {
        if (diagnostic != nullptr) {
            *diagnostic = QStringLiteral("mapped device identity changed");
        }
        return std::nullopt;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, 64 * 1024> buffer{};
    quint64 bytesRead = 0;
    while (true) {
        const qint64 size = file.read(buffer.data(), buffer.size());
        if (size < 0) {
            if (diagnostic != nullptr) {
                *diagnostic = QStringLiteral("could not hash file contents");
            }
            return std::nullopt;
        }
        if (size == 0) break;
        bytesRead += static_cast<quint64>(size);
        hash.addData(QByteArrayView(buffer.data(), size));
    }

    struct stat finalStatus{};
    if (::fstat(file.handle(), &finalStatus) != 0) {
        if (diagnostic != nullptr) {
            *diagnostic =
                QStringLiteral("final fstat failed: %1")
                    .arg(QString::fromLocal8Bit(std::strerror(errno)));
        }
        return std::nullopt;
    }
    if (!S_ISREG(finalStatus.st_mode) || finalStatus.st_dev != status.st_dev
        || finalStatus.st_ino != status.st_ino
        || finalStatus.st_size != status.st_size
        || bytesRead != static_cast<quint64>(status.st_size)
        || !sameTimestamp(finalStatus.st_mtim, status.st_mtim)
        || !sameTimestamp(finalStatus.st_ctim, status.st_ctim)) {
        if (diagnostic != nullptr) {
            *diagnostic = QStringLiteral("mapped file changed while hashing");
        }
        return std::nullopt;
    }
    return HashedFile{
        .size = static_cast<quint64>(status.st_size),
        .sha256 = hash.result().toHex(),
    };
}

std::optional<HashedFile>
hashMapping(const RendererGraphicsLibraryMapping &representative,
            const QList<RendererGraphicsLibraryMapping> &mappings,
            const QString &procSelfRoot, QString *diagnostic)
{
    QString pathDiagnostic;
    if (!representative.path.endsWith(u" (deleted)")) {
        if (const auto hashed =
                hashMappedFile(mappedFilePath(representative.path),
                               representative, procSelfRoot, &pathDiagnostic)) {
            return hashed;
        }
    } else {
        pathDiagnostic = QStringLiteral("mapped path was deleted");
    }

    QString mapFileDiagnostic = QStringLiteral("no mapping range was usable");
    qsizetype fallbackCount = 0;
    for (const bool requireExecutable : {true, false}) {
        for (const RendererGraphicsLibraryMapping &mapping : mappings) {
            if (mapping.permissions.contains('x') != requireExecutable)
                continue;
            ++fallbackCount;
            const QString mapFile = QStringLiteral("%1/map_files/%2-%3")
                                        .arg(procSelfRoot)
                                        .arg(mapping.start, 0, 16)
                                        .arg(mapping.end, 0, 16);
            if (const auto hashed =
                    hashMappedFile(mapFile, representative, procSelfRoot,
                                   &mapFileDiagnostic)) {
                return hashed;
            }
        }
    }
    if (diagnostic != nullptr) {
        *diagnostic =
            QStringLiteral("path failed (%1); %2 map_files fallback(s) failed "
                           "(%3)")
                .arg(pathDiagnostic)
                .arg(fallbackCount)
                .arg(mapFileDiagnostic);
    }
    return std::nullopt;
}

QByteArray manifestJson(const RendererGraphicsLibraryManifest &manifest)
{
    QJsonArray libraries;
    for (const RendererGraphicsLibraryEntry &entry : manifest.libraries) {
        libraries.append(QJsonObject{
            {QStringLiteral("role"), roleName(entry.role)},
            {QStringLiteral("name"), entry.name},
            {QStringLiteral("path_kind"), entry.pathKind},
            {QStringLiteral("size"), static_cast<qint64>(entry.size)},
            {QStringLiteral("sha256"), QString::fromLatin1(entry.sha256)},
        });
    }
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), 1},
                   {QStringLiteral("backend"), backendName(manifest.backend)},
                   {QStringLiteral("status"),
                    rendererGraphicsLibraryManifestStatusName(manifest.status)
                        .toString()},
                   {QStringLiteral("libraries"), libraries},
                   {QStringLiteral("diagnostic"),
                    manifest.diagnostic.isEmpty()
                        ? QJsonValue(QJsonValue::Null)
                        : QJsonValue(manifest.diagnostic)},
               })
        .toJson(QJsonDocument::Compact);
}

} // namespace

QStringView rendererGraphicsLibraryManifestStatusName(
    RendererGraphicsLibraryManifestStatus status)
{
    switch (status) {
    case RendererGraphicsLibraryManifestStatus::Complete: return u"complete";
    case RendererGraphicsLibraryManifestStatus::Partial: return u"partial";
    case RendererGraphicsLibraryManifestStatus::Unavailable:
        return u"unavailable";
    }
    return u"unavailable";
}

QString rendererGraphicsLibraryPathKind(QStringView path)
{
    if (path.endsWith(u" (deleted)")) return QStringLiteral("deleted");
    const auto isWithin = [path](QStringView root) {
        return path == root
            || (path.startsWith(root) && path.size() > root.size()
                && path.at(root.size()) == u'/');
    };
    if (isWithin(u"/usr") || isWithin(u"/lib") || isWithin(u"/lib64")
        || isWithin(u"/nix/store")) {
        return QStringLiteral("system");
    }
    return QStringLiteral("custom");
}

QList<RendererGraphicsLibraryMapping>
parseRendererGraphicsLibraryMaps(QByteArrayView contents, QString *diagnostic)
{
    QList<RendererGraphicsLibraryMapping> result;
    const QList<QByteArray> lines = contents.toByteArray().split('\n');
    qsizetype lineNumber = 0;
    for (const QByteArray &line : lines) {
        ++lineNumber;
        if (line.isEmpty()) continue;

        QList<QByteArrayView> fields;
        qsizetype cursor = 0;
        while (fields.size() < ProcMapsFieldCount) {
            while (cursor < line.size()
                   && (line.at(cursor) == ' ' || line.at(cursor) == '\t')) {
                ++cursor;
            }
            const qsizetype start = cursor;
            while (cursor < line.size() && line.at(cursor) != ' '
                   && line.at(cursor) != '\t') {
                ++cursor;
            }
            if (start == cursor) break;
            fields.append(QByteArrayView(line).sliced(start, cursor - start));
        }
        if (fields.size() != ProcMapsFieldCount) {
            if (diagnostic != nullptr) {
                *diagnostic =
                    QStringLiteral("invalid /proc maps record at line %1")
                        .arg(lineNumber);
            }
            return {};
        }
        while (cursor < line.size()
               && (line.at(cursor) == ' ' || line.at(cursor) == '\t')) {
            ++cursor;
        }

        const QList<QByteArray> range = fields.at(0).toByteArray().split('-');
        const QList<QByteArray> device = fields.at(3).toByteArray().split(':');
        RendererGraphicsLibraryMapping mapping;
        if (range.size() != 2 || device.size() != 2
            || !parseHex(range.at(0), &mapping.start)
            || !parseHex(range.at(1), &mapping.end)
            || mapping.end <= mapping.start
            || !parseHex(device.at(0), &mapping.deviceMajor)
            || !parseHex(device.at(1), &mapping.deviceMinor)
            || !parseDecimal(fields.at(4), &mapping.inode)) {
            if (diagnostic != nullptr) {
                *diagnostic =
                    QStringLiteral("invalid /proc maps record at line %1")
                        .arg(lineNumber);
            }
            return {};
        }
        mapping.permissions = fields.at(1).toByteArray();
        mapping.path = decodeProcMapsPath(QByteArrayView(line).sliced(cursor));
        if (mapping.inode == 0 || !mapping.path.startsWith('/')) continue;
        result.append(std::move(mapping));
    }
    if (diagnostic != nullptr) diagnostic->clear();
    return result;
}

std::optional<RendererGraphicsLibraryRole>
classifyRendererGraphicsLibrary(RendererGraphicsBackend backend,
                                QStringView basename)
{
    const QString name = basename.toString().toLower();
    const auto startsSharedObject = [&name](QStringView prefix) {
        return name.startsWith(prefix) && name.contains(QStringLiteral(".so"));
    };
    const bool nvidiaVendorDispatch = startsSharedObject(u"libglx_nvidia")
        || startsSharedObject(u"libegl_nvidia")
        || startsSharedObject(u"libnvidia-glcore")
        || startsSharedObject(u"libnvidia-eglcore");
    const bool compiler = startsSharedObject(u"libllvm")
        || startsSharedObject(u"libglslang") || startsSharedObject(u"libspirv")
        || startsSharedObject(u"libshaderc")
        || startsSharedObject(u"libnvidia-glvkspirv")
        || startsSharedObject(u"libnvidia-gpucomp")
        || startsSharedObject(u"libnvidia-ptxjitcompiler");
    const bool instrumentation = name.contains(QStringLiteral(".so"))
        && (name.contains(QStringLiteral("vklayer"))
            || name.contains(QStringLiteral("renderdoc"))
            || name.contains(QStringLiteral("mangohud"))
            || name.contains(QStringLiteral("vkbasalt"))
            || name.contains(QStringLiteral("obs-vulkan"))
            || name.contains(QStringLiteral("obs_glcapture"))
            || name.contains(QStringLiteral("obs_vkcapture")));

    if (instrumentation) return RendererGraphicsLibraryRole::Layer;
    if (compiler) return RendererGraphicsLibraryRole::Compiler;

    if (backend == RendererGraphicsBackend::OpenGL) {
        const bool galliumDriver = startsSharedObject(u"libgallium.so")
            || startsSharedObject(u"libgallium-");
        if ((name.contains(QStringLiteral("_dri.so")))
            || startsSharedObject(u"libdril_dri") || galliumDriver) {
            return RendererGraphicsLibraryRole::Driver;
        }
        if (startsSharedObject(u"libglx_mesa")
            || startsSharedObject(u"libegl_mesa") || nvidiaVendorDispatch
            || startsSharedObject(u"libglapi")) {
            return RendererGraphicsLibraryRole::VendorDispatch;
        }
        if (startsSharedObject(u"libgl.so") || startsSharedObject(u"libglx.so")
            || startsSharedObject(u"libopengl.so")
            || startsSharedObject(u"libegl.so")
            || startsSharedObject(u"libgldispatch")
            || startsSharedObject(u"libglesv1_cm.so")
            || startsSharedObject(u"libglesv2.so")) {
            return RendererGraphicsLibraryRole::ApiLoader;
        }
    } else if (backend == RendererGraphicsBackend::Vulkan) {
        const bool genericLoader = startsSharedObject(u"libvulkan.so");
        const bool mesaDriver = startsSharedObject(u"libvulkan_radeon")
            || startsSharedObject(u"libvulkan_intel")
            || startsSharedObject(u"libvulkan_intel_hasvk")
            || startsSharedObject(u"libvulkan_nouveau")
            || startsSharedObject(u"libvulkan_lvp")
            || startsSharedObject(u"libvulkan_virtio")
            || startsSharedObject(u"libvulkan_freedreno")
            || startsSharedObject(u"libvulkan_broadcom")
            || startsSharedObject(u"libvulkan_panfrost")
            || startsSharedObject(u"libvulkan_asahi")
            || startsSharedObject(u"libvulkan_dzn");
        if (mesaDriver || startsSharedObject(u"libvk_swiftshader")
            || (name.contains(QStringLiteral("amdvlk"))
                && name.contains(QStringLiteral(".so")))
            || startsSharedObject(u"libvulkan_pro")) {
            return RendererGraphicsLibraryRole::Driver;
        }
        if (nvidiaVendorDispatch) {
            return RendererGraphicsLibraryRole::VendorDispatch;
        }
        if (genericLoader) return RendererGraphicsLibraryRole::ApiLoader;
    }
    return std::nullopt;
}

QByteArray rendererGraphicsLibraryAggregate(
    const QList<RendererGraphicsLibraryEntry> &libraries)
{
    QList<RendererGraphicsLibraryEntry> sorted = libraries;
    std::ranges::sort(sorted, canonicalEntryLess);
    QCryptographicHash aggregate(QCryptographicHash::Sha256);
    for (const RendererGraphicsLibraryEntry &entry : sorted) {
        aggregate.addData(roleName(entry.role).toUtf8());
        aggregate.addData(QByteArrayView("\0", 1));
        aggregate.addData(entry.name.toUtf8());
        aggregate.addData(QByteArrayView("\0", 1));
        aggregate.addData(QByteArray::number(entry.size));
        aggregate.addData(QByteArrayView("\0", 1));
        aggregate.addData(entry.sha256.toLower());
        aggregate.addData(QByteArrayView("\n", 1));
    }
    return aggregate.result().toHex();
}

RendererGraphicsLibraryManifest
collectRendererGraphicsLibraryManifest(RendererGraphicsBackend backend,
                                       const QString &procSelfRoot)
{
    RendererGraphicsLibraryManifest result;
    result.backend = backend;
    QFile maps(QStringLiteral("%1/maps").arg(procSelfRoot));
    if (!maps.open(QIODevice::ReadOnly)) {
        result.diagnostic = QStringLiteral("could not read process maps: %1")
                                .arg(maps.errorString());
        result.compactJson = manifestJson(result);
        return result;
    }

    QString parseDiagnostic;
    const QList<RendererGraphicsLibraryMapping> mappings =
        parseRendererGraphicsLibraryMaps(maps.readAll(), &parseDiagnostic);
    if (!parseDiagnostic.isEmpty()) {
        result.diagnostic = parseDiagnostic;
        result.compactJson = manifestJson(result);
        return result;
    }

    using MappingIdentity = std::tuple<quint64, quint64, quint64>;
    std::map<MappingIdentity, QList<RendererGraphicsLibraryMapping>> groups;
    for (const RendererGraphicsLibraryMapping &mapping : mappings) {
        groups[MappingIdentity(mapping.deviceMajor, mapping.deviceMinor,
                               mapping.inode)]
            .append(mapping);
    }

    std::set<CanonicalEntryIdentity> uniqueEntries;
    QStringList hashFailures;
    for (const auto &[identity, group] : groups) {
        Q_UNUSED(identity);
        if (!std::ranges::any_of(group, [](const auto &mapping) {
                return mapping.permissions.contains('x');
            })) {
            continue;
        }

        const RendererGraphicsLibraryMapping *representative = nullptr;
        std::optional<RendererGraphicsLibraryRole> role;
        for (const bool requireExecutable : {true, false}) {
            for (const RendererGraphicsLibraryMapping &mapping : group) {
                if (mapping.permissions.contains('x') != requireExecutable) {
                    continue;
                }
                const QString basename =
                    QFileInfo(mappedFilePath(mapping.path)).fileName();
                const auto candidateRole =
                    classifyRendererGraphicsLibrary(backend, basename);
                if (!candidateRole.has_value()) continue;
                representative = &mapping;
                role = candidateRole;
                break;
            }
            if (representative != nullptr) break;
        }
        if (representative == nullptr || !role.has_value()) continue;

        const QString path = mappedFilePath(representative->path);
        const QString basename = QFileInfo(path).fileName();
        if (basename.contains(QChar(u'\0')) || basename.contains(QChar(u'\n'))
            || basename.contains(QChar(u'\r'))) {
            hashFailures.append(
                QStringLiteral("unsafe graphics library basename"));
            continue;
        }
        QString hashDiagnostic;
        const auto hashed =
            hashMapping(*representative, group, procSelfRoot, &hashDiagnostic);
        if (!hashed) {
            hashFailures.append(
                QStringLiteral("%1 (%2)").arg(basename, hashDiagnostic));
            continue;
        }
        RendererGraphicsLibraryEntry entry{
            .role = *role,
            .name = basename,
            .pathKind = rendererGraphicsLibraryPathKind(representative->path),
            .size = hashed->size,
            .sha256 = hashed->sha256.toLower(),
        };
        const CanonicalEntryIdentity entryIdentity =
            canonicalEntryIdentity(entry);
        if (!uniqueEntries.insert(entryIdentity).second) continue;
        result.libraries.append(std::move(entry));
    }

    std::ranges::sort(result.libraries, canonicalEntryLess);
    if (!result.libraries.isEmpty()) {
        result.aggregateSha256 =
            rendererGraphicsLibraryAggregate(result.libraries);
    }
    const bool hasRequiredAnchor =
        std::ranges::any_of(result.libraries, [](const auto &entry) {
            return entry.role == RendererGraphicsLibraryRole::Driver
                || entry.role == RendererGraphicsLibraryRole::VendorDispatch;
        });
    if (!hashFailures.isEmpty()) {
        result.status = RendererGraphicsLibraryManifestStatus::Partial;
        result.diagnostic = QStringLiteral("unhashable: ")
            + hashFailures.join(QStringLiteral("; "));
        if (!hasRequiredAnchor) {
            result.diagnostic.prepend(
                QStringLiteral("no hashable driver or vendor_dispatch; "));
        }
    } else if (!hasRequiredAnchor) {
        result.status = RendererGraphicsLibraryManifestStatus::Unavailable;
        result.diagnostic =
            QStringLiteral(
                "no hashable driver or vendor_dispatch library matched the %1 "
                "classifier")
                .arg(backendName(backend));
    } else {
        result.status = RendererGraphicsLibraryManifestStatus::Complete;
    }
    result.compactJson = manifestJson(result);
    return result;
}
