#include "terminal/rendering/renderer_graphics_library_manifest.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include <cerrno>
#include <cstring>

#include <sys/stat.h>
#include <sys/sysmacros.h>

namespace {

struct FileIdentity {
    quint64 deviceMajor = 0;
    quint64 deviceMinor = 0;
    quint64 inode = 0;
};

FileIdentity fileIdentity(const QString &path)
{
    struct stat status{};
    const QByteArray nativePath = QFile::encodeName(path);
    if (::stat(nativePath.constData(), &status) != 0) {
        qFatal("stat failed: %s", std::strerror(errno));
    }
    return {
        .deviceMajor = static_cast<quint64>(::major(status.st_dev)),
        .deviceMinor = static_cast<quint64>(::minor(status.st_dev)),
        .inode = static_cast<quint64>(status.st_ino),
    };
}

QByteArray mappingLine(quint64 start, const QString &path,
                       const FileIdentity &identity,
                       QByteArrayView permissions = QByteArrayView("r-xp"))
{
    return QStringLiteral("%1-%2 %3 00000000 %4:%5 %6 %7\n")
        .arg(start, 0, 16)
        .arg(start + 0x1000, 0, 16)
        .arg(QString::fromLatin1(permissions.data(), permissions.size()))
        .arg(identity.deviceMajor, 0, 16)
        .arg(identity.deviceMinor, 0, 16)
        .arg(identity.inode)
        .arg(path)
        .toUtf8();
}

QString createFile(const QTemporaryDir &directory, QStringView name,
                   QByteArrayView contents)
{
    const QString path = directory.filePath(name.toString());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(contents.data(), contents.size()) != contents.size()) {
        qFatal("could not create fixture %s", qPrintable(path));
    }
    return path;
}

quint64 mountIdForPath(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("could not open fixture for mount-ID discovery");
    }
    QFile fdInfo(QStringLiteral("/proc/self/fdinfo/%1").arg(file.handle()));
    if (!fdInfo.open(QIODevice::ReadOnly)) {
        qFatal("could not read fixture fdinfo");
    }
    for (const QByteArray &line : fdInfo.readAll().split('\n')) {
        if (!line.startsWith("mnt_id:")) continue;
        bool ok = false;
        const quint64 mountId = line.mid(7).trimmed().toULongLong(&ok);
        if (ok) return mountId;
    }
    qFatal("fixture fdinfo had no mount ID");
}

void writeMountInfo(const QTemporaryDir &directory, quint64 mountId,
                    quint64 deviceMajor, quint64 deviceMinor)
{
    if (!QFile::link(QStringLiteral("/proc/self/fdinfo"),
                     directory.filePath(QStringLiteral("fdinfo")))) {
        qFatal("could not link fdinfo fixture");
    }
    QFile mountInfo(directory.filePath(QStringLiteral("mountinfo")));
    if (!mountInfo.open(QIODevice::WriteOnly)
        || mountInfo.write(
               QStringLiteral("%1 1 %2:%3 / / rw - testfs test rw\n")
                   .arg(mountId)
                   .arg(deviceMajor)
                   .arg(deviceMinor)
                   .toUtf8())
            <= 0) {
        qFatal("could not create mountinfo fixture");
    }
}

void useFixtureDevice(FileIdentity *identity, quint64 deviceMajor,
                      quint64 deviceMinor)
{
    identity->deviceMajor = deviceMajor;
    identity->deviceMinor = deviceMinor;
}

} // namespace

class RendererGraphicsLibraryManifestTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void reportsStablePathKinds()
    {
        QCOMPARE(rendererGraphicsLibraryPathKind(u"/usr/lib/libvulkan.so.1"),
                 QStringLiteral("system"));
        QCOMPARE(rendererGraphicsLibraryPathKind(
                     u"/nix/store/hash/lib/libvulkan.so.1"),
                 QStringLiteral("system"));
        QCOMPARE(rendererGraphicsLibraryPathKind(u"/home/user/driver.so"),
                 QStringLiteral("custom"));
        QCOMPARE(rendererGraphicsLibraryPathKind(
                     u"/usr/lib/libvulkan.so.1 (deleted)"),
                 QStringLiteral("deleted"));
    }

    void parsesMapsWithoutNormalizingPathTail()
    {
        const QByteArray maps = "1000-2000 r-xp 00000000 08:01 123 "
                                "/tmp/driver  with\\040space.so\n"
                                "2000-3000 rw-p 00000000 00:00 0 [heap]\n";
        QString diagnostic;
        const QList<RendererGraphicsLibraryMapping> parsed =
            parseRendererGraphicsLibraryMaps(maps, &diagnostic);
        QCOMPARE(diagnostic, QString());
        QCOMPARE(parsed.size(), 1);
        QCOMPARE(parsed.first().start, quint64(0x1000));
        QCOMPARE(parsed.first().end, quint64(0x2000));
        QCOMPARE(parsed.first().deviceMajor, quint64(8));
        QCOMPARE(parsed.first().deviceMinor, quint64(1));
        QCOMPARE(parsed.first().inode, quint64(123));
        QCOMPARE(parsed.first().path,
                 QStringLiteral("/tmp/driver  with space.so"));

        const auto malformed = parseRendererGraphicsLibraryMaps(
            QByteArrayLiteral("not-a-map-record\n"), &diagnostic);
        QVERIFY(malformed.isEmpty());
        QVERIFY(!diagnostic.isEmpty());
    }

    void classifiesOnlyActiveGraphicsStackCandidates()
    {
        const auto expect = [](RendererGraphicsBackend backend,
                               QStringView name,
                               RendererGraphicsLibraryRole role) {
            const auto actual = classifyRendererGraphicsLibrary(backend, name);
            QVERIFY(actual.has_value());
            QCOMPARE(*actual, role);
        };
        expect(RendererGraphicsBackend::OpenGL, u"libdril_dri.so",
               RendererGraphicsLibraryRole::Driver);
        expect(RendererGraphicsBackend::OpenGL, u"libgallium-26.1.6.so",
               RendererGraphicsLibraryRole::Driver);
        expect(RendererGraphicsBackend::OpenGL, u"libgallium.so",
               RendererGraphicsLibraryRole::Driver);
        expect(RendererGraphicsBackend::OpenGL, u"libGLX_mesa.so.0",
               RendererGraphicsLibraryRole::VendorDispatch);
        expect(RendererGraphicsBackend::OpenGL, u"libGL.so.1",
               RendererGraphicsLibraryRole::ApiLoader);
        expect(RendererGraphicsBackend::Vulkan, u"libvulkan_radeon.so",
               RendererGraphicsLibraryRole::Driver);
        expect(RendererGraphicsBackend::Vulkan, u"libvulkan.so.1",
               RendererGraphicsLibraryRole::ApiLoader);
        expect(RendererGraphicsBackend::Vulkan, u"libGLX_nvidia.so.0",
               RendererGraphicsLibraryRole::VendorDispatch);
        expect(RendererGraphicsBackend::Vulkan, u"libEGL_nvidia.so.0",
               RendererGraphicsLibraryRole::VendorDispatch);
        expect(RendererGraphicsBackend::Vulkan, u"libnvidia-glcore.so.580",
               RendererGraphicsLibraryRole::VendorDispatch);
        expect(RendererGraphicsBackend::Vulkan, u"libVkLayer_MESA_overlay.so",
               RendererGraphicsLibraryRole::Layer);
        expect(RendererGraphicsBackend::Vulkan, u"libLLVM.so.20",
               RendererGraphicsLibraryRole::Compiler);
        QVERIFY(!classifyRendererGraphicsLibrary(
                     RendererGraphicsBackend::Vulkan, u"libdrm_amdgpu.so.1")
                     .has_value());
        QVERIFY(!classifyRendererGraphicsLibrary(
                     RendererGraphicsBackend::Vulkan, u"libnvidia-ml.so.1")
                     .has_value());
        QVERIFY(!classifyRendererGraphicsLibrary(
                     RendererGraphicsBackend::OpenGL, u"libgallium_helper.so")
                     .has_value());
    }

    void aggregateUsesCanonicalPortableTupleOrder()
    {
        const QList<RendererGraphicsLibraryEntry> libraries{
            {
                .role = RendererGraphicsLibraryRole::Driver,
                .name = QStringLiteral("z.so"),
                .pathKind = QStringLiteral("custom"),
                .size = 2,
                .sha256 = QByteArray(64, '2'),
            },
            {
                .role = RendererGraphicsLibraryRole::ApiLoader,
                .name = QStringLiteral("a.so"),
                .pathKind = QStringLiteral("system"),
                .size = 1,
                .sha256 = QByteArray(64, '1'),
            },
        };
        QByteArray payload;
        payload.append("api_loader\0a.so\0", 16);
        payload.append('1');
        payload.append('\0');
        payload.append(QByteArray(64, '1'));
        payload.append('\n');
        payload.append("driver\0z.so\0", 12);
        payload.append('2');
        payload.append('\0');
        payload.append(QByteArray(64, '2'));
        payload.append('\n');
        QCOMPARE(rendererGraphicsLibraryAggregate(libraries),
                 QCryptographicHash::hash(payload, QCryptographicHash::Sha256)
                     .toHex());
    }

    void collectsAndDeduplicatesVerifiedMappings()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString driver = createFile(directory, u"libvulkan_radeon.so",
                                          QByteArrayLiteral("driver"));
        const QString loader = createFile(directory, u"libvulkan.so.1",
                                          QByteArrayLiteral("loader"));
        FileIdentity driverIdentity = fileIdentity(driver);
        FileIdentity loaderIdentity = fileIdentity(loader);
        const quint64 DeviceMajor = driverIdentity.deviceMajor ^ quint64(1);
        constexpr quint64 DeviceMinor = 0x21;
        QVERIFY(DeviceMajor != driverIdentity.deviceMajor
                || DeviceMinor != driverIdentity.deviceMinor);
        useFixtureDevice(&driverIdentity, DeviceMajor, DeviceMinor);
        useFixtureDevice(&loaderIdentity, DeviceMajor, DeviceMinor);
        writeMountInfo(directory, mountIdForPath(driver), DeviceMajor,
                       DeviceMinor);
        QFile maps(directory.filePath(QStringLiteral("maps")));
        QVERIFY(maps.open(QIODevice::WriteOnly));
        maps.write(mappingLine(0x1000, driver, driverIdentity,
                               QByteArrayView("r--p")));
        maps.write(mappingLine(0x2000, driver, driverIdentity));
        maps.write(mappingLine(0x3000, loader, loaderIdentity,
                               QByteArrayView("r--p")));
        maps.write(mappingLine(0x4000, loader, loaderIdentity));
        maps.close();

        const RendererGraphicsLibraryManifest manifest =
            collectRendererGraphicsLibraryManifest(
                RendererGraphicsBackend::Vulkan, directory.path());
        QCOMPARE(manifest.status,
                 RendererGraphicsLibraryManifestStatus::Complete);
        QCOMPARE(manifest.libraries.size(), 2);
        QVERIFY(!manifest.aggregateSha256.isEmpty());
        QVERIFY(manifest.diagnostic.isEmpty());
        QVERIFY(!manifest.compactJson.contains(directory.path().toUtf8()));
        const QJsonDocument json =
            QJsonDocument::fromJson(manifest.compactJson);
        QVERIFY(json.isObject());
        QCOMPARE(json.object().value(QStringLiteral("status")).toString(),
                 QStringLiteral("complete"));
    }

    void failsClosedForMissingOrUnhashableDriverAnchor()
    {
        QTemporaryDir loaderOnlyDirectory;
        QVERIFY(loaderOnlyDirectory.isValid());
        const QString loader =
            createFile(loaderOnlyDirectory, u"libvulkan.so.1",
                       QByteArrayLiteral("loader"));
        FileIdentity loaderIdentity = fileIdentity(loader);
        useFixtureDevice(&loaderIdentity, 0, 0x21);
        writeMountInfo(loaderOnlyDirectory, mountIdForPath(loader), 0, 0x21);
        QFile loaderMaps(loaderOnlyDirectory.filePath(QStringLiteral("maps")));
        QVERIFY(loaderMaps.open(QIODevice::WriteOnly));
        loaderMaps.write(mappingLine(0x1000, loader, loaderIdentity));
        loaderMaps.close();
        const RendererGraphicsLibraryManifest loaderOnly =
            collectRendererGraphicsLibraryManifest(
                RendererGraphicsBackend::Vulkan, loaderOnlyDirectory.path());
        QCOMPARE(loaderOnly.status,
                 RendererGraphicsLibraryManifestStatus::Unavailable);
        QCOMPARE(loaderOnly.libraries.size(), 1);

        QTemporaryDir missingDirectory;
        QVERIFY(missingDirectory.isValid());
        const QString missing =
            createFile(missingDirectory, u"libvulkan_radeon.so",
                       QByteArrayLiteral("driver"));
        FileIdentity missingIdentity = fileIdentity(missing);
        const quint64 missingMountId = mountIdForPath(missing);
        useFixtureDevice(&missingIdentity, 0, 0x21);
        writeMountInfo(missingDirectory, missingMountId, 0, 0x21);
        QVERIFY(QFile::remove(missing));
        QFile missingMaps(missingDirectory.filePath(QStringLiteral("maps")));
        QVERIFY(missingMaps.open(QIODevice::WriteOnly));
        missingMaps.write(mappingLine(0x2000, missing, missingIdentity));
        missingMaps.close();
        const RendererGraphicsLibraryManifest partial =
            collectRendererGraphicsLibraryManifest(
                RendererGraphicsBackend::Vulkan, missingDirectory.path());
        QCOMPARE(partial.status,
                 RendererGraphicsLibraryManifestStatus::Partial);
        QVERIFY(partial.libraries.isEmpty());
        QVERIFY(!partial.diagnostic.contains(missingDirectory.path()));
    }

    void rejectsMountDeviceMismatch()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString driver = createFile(directory, u"libvulkan_radeon.so",
                                          QByteArrayLiteral("driver"));
        FileIdentity identity = fileIdentity(driver);
        useFixtureDevice(&identity, 0, 0x21);
        writeMountInfo(directory, mountIdForPath(driver), 0, 0x22);
        QFile maps(directory.filePath(QStringLiteral("maps")));
        QVERIFY(maps.open(QIODevice::WriteOnly));
        maps.write(mappingLine(0x1000, driver, identity));
        maps.close();

        const RendererGraphicsLibraryManifest manifest =
            collectRendererGraphicsLibraryManifest(
                RendererGraphicsBackend::Vulkan, directory.path());
        QCOMPARE(manifest.status,
                 RendererGraphicsLibraryManifestStatus::Partial);
        QVERIFY(manifest.libraries.isEmpty());
        QVERIFY(manifest.diagnostic.contains(
            QStringLiteral("mapped device identity changed")));
        QVERIFY(!manifest.diagnostic.contains(directory.path()));
    }

    void ignoresDataOnlyMappings()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString driver = createFile(directory, u"libvulkan_radeon.so",
                                          QByteArrayLiteral("driver"));
        FileIdentity identity = fileIdentity(driver);
        useFixtureDevice(&identity, 0, 0x21);
        writeMountInfo(directory, mountIdForPath(driver), 0, 0x21);
        QFile maps(directory.filePath(QStringLiteral("maps")));
        QVERIFY(maps.open(QIODevice::WriteOnly));
        maps.write(
            mappingLine(0x1000, driver, identity, QByteArrayView("r--p")));
        maps.close();

        const RendererGraphicsLibraryManifest manifest =
            collectRendererGraphicsLibraryManifest(
                RendererGraphicsBackend::Vulkan, directory.path());
        QCOMPARE(manifest.status,
                 RendererGraphicsLibraryManifestStatus::Unavailable);
        QVERIFY(manifest.libraries.isEmpty());
    }

    void rejectsZeroLengthMappings()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString driver =
            createFile(directory, u"libvulkan_radeon.so", QByteArrayView());
        FileIdentity identity = fileIdentity(driver);
        useFixtureDevice(&identity, 0, 0x21);
        writeMountInfo(directory, mountIdForPath(driver), 0, 0x21);
        QFile maps(directory.filePath(QStringLiteral("maps")));
        QVERIFY(maps.open(QIODevice::WriteOnly));
        maps.write(mappingLine(0x1000, driver, identity));
        maps.close();

        const RendererGraphicsLibraryManifest manifest =
            collectRendererGraphicsLibraryManifest(
                RendererGraphicsBackend::Vulkan, directory.path());
        QCOMPARE(manifest.status,
                 RendererGraphicsLibraryManifestStatus::Partial);
        QVERIFY(manifest.libraries.isEmpty());
        QVERIFY(manifest.diagnostic.contains(QStringLiteral("file was empty")));
    }

    void usesVerifiedMapFilesFallback()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString backing =
            createFile(directory, u"backing.bin", QByteArrayLiteral("driver"));
        const QString mapped =
            directory.filePath(QStringLiteral("libvulkan_radeon.so"))
            + QStringLiteral(" (deleted)");
        FileIdentity identity = fileIdentity(backing);
        useFixtureDevice(&identity, 0, 0x21);
        writeMountInfo(directory, mountIdForPath(backing), 0, 0x21);
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("map_files")));
        QVERIFY(QFile::link(
            backing,
            directory.filePath(QStringLiteral("map_files/1000-2000"))));
        QFile maps(directory.filePath(QStringLiteral("maps")));
        QVERIFY(maps.open(QIODevice::WriteOnly));
        maps.write(mappingLine(0x1000, mapped, identity));
        maps.close();

        const RendererGraphicsLibraryManifest manifest =
            collectRendererGraphicsLibraryManifest(
                RendererGraphicsBackend::Vulkan, directory.path());
        QCOMPARE(manifest.status,
                 RendererGraphicsLibraryManifestStatus::Complete);
        QCOMPARE(manifest.libraries.size(), 1);
        QCOMPARE(manifest.libraries.first().name,
                 QStringLiteral("libvulkan_radeon.so"));
        QCOMPARE(manifest.libraries.first().pathKind,
                 QStringLiteral("deleted"));
        QVERIFY(manifest.diagnostic.isEmpty());
        QVERIFY(!manifest.compactJson.contains(directory.path().toUtf8()));
    }
};

QTEST_GUILESS_MAIN(RendererGraphicsLibraryManifestTest)

#include "test_renderer_graphics_library_manifest.moc"
