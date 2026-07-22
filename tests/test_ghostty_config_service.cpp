#include "ghostty_config_service.h"

#include "ghostty_config_snapshot_fixture.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QPointer>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <thread>

namespace {

bool replaceFile(const QString &path, const QByteArray &contents)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size()) {
        return false;
    }
    return file.commit();
}

GhosttyConfigSnapshot snapshotWithMarker(int marker, const QString &sourcePath = {})
{
    GhosttyConfigSnapshot snapshot = GhosttyConfigSnapshotFixture::snapshot();
    snapshot.values.windowWidth = static_cast<quint32>(marker);
    if (!sourcePath.isEmpty()) {
        snapshot.sourcePaths.append(sourcePath);
    }
    return snapshot;
}

} // namespace

class GhosttyConfigServiceTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void passesStandardPathsInPrecedenceOrder();
    void watchesAbsentFilesAndReaddsAtomicReplacements();
    void watchesLoadedIncludePaths();
    void watchesMissingOptionalIncludeCreation();
    void debouncesReloadBursts();
    void standardServiceReloadsOffGuiThread();
    void synchronousReloadSupersedesOlderAsyncResult();
    void publishesUnchangedSuccessfulReloads();
    void retainsLastGoodSnapshotAfterFailure();
    void changedSubscriberMayDeleteAsyncService();
    void failedSubscriberMayDeleteAsyncService();
};

void GhosttyConfigServiceTest::passesStandardPathsInPrecedenceOrder()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    QProcessEnvironment environment;
    environment.insert(QStringLiteral("HOME"),
                       QDir(temporary.path()).filePath(QStringLiteral("home")));
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"),
                       QDir(temporary.path()).filePath(QStringLiteral("xdg")));
    const QString ghosttyDirectory =
        QDir(temporary.path()).filePath(QStringLiteral("xdg/ghostty"));
    const QStringList expected{
        QDir(ghosttyDirectory).filePath(QStringLiteral("config")),
        QDir(ghosttyDirectory).filePath(QStringLiteral("config.ghostty")),
    };
    QCOMPARE(GhosttyConfigService::standardConfigPaths(environment), expected);
    QCOMPARE(GhosttyConfigService::standardConfigEditPaths(environment),
             QStringList({expected.at(1), expected.at(0)}));

    environment.insert(QStringLiteral("XDG_CONFIG_HOME"),
                       QStringLiteral("relative-xdg"));
    QCOMPARE(GhosttyConfigService::standardConfigPaths(environment),
             QStringList({
                 QDir(environment.value(QStringLiteral("HOME")))
                     .filePath(QStringLiteral(".config/ghostty/config")),
                 QDir(environment.value(QStringLiteral("HOME")))
                     .filePath(QStringLiteral(".config/ghostty/config.ghostty")),
             }));
    QCOMPARE(GhosttyConfigService::standardConfigEditPaths(environment),
             QStringList({
                 QStringLiteral("relative-xdg/ghostty/config.ghostty"),
                 QStringLiteral("relative-xdg/ghostty/config"),
             }));

    QStringList observed;
    GhosttyConfigService service(
        expected,
        [&observed](const QStringList &paths) {
            observed = paths;
            return snapshotWithMarker(1);
        },
        20);
    QCOMPARE(observed, expected);

    environment.remove(QStringLiteral("XDG_CONFIG_HOME"));
    const QString fallbackDirectory =
        QDir(environment.value(QStringLiteral("HOME")))
            .filePath(QStringLiteral(".config/ghostty"));
    QCOMPARE(GhosttyConfigService::standardConfigPaths(environment),
             QStringList({
                 QDir(fallbackDirectory).filePath(QStringLiteral("config")),
                 QDir(fallbackDirectory).filePath(QStringLiteral("config.ghostty")),
             }));
    QCOMPARE(GhosttyConfigService::standardConfigEditPaths(environment),
             QStringList({
                 QDir(fallbackDirectory).filePath(QStringLiteral("config.ghostty")),
                 QDir(fallbackDirectory).filePath(QStringLiteral("config")),
             }));
}

void GhosttyConfigServiceTest::watchesAbsentFilesAndReaddsAtomicReplacements()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString ghosttyDirectory =
        QDir(temporary.path()).filePath(QStringLiteral("ghostty"));
    QVERIFY(QDir().mkpath(ghosttyDirectory));
    const QString primary =
        QDir(ghosttyDirectory).filePath(QStringLiteral("config"));
    const QString fallback =
        QDir(ghosttyDirectory).filePath(QStringLiteral("config.ghostty"));

    int loads = 0;
    auto loader = [&loads](const QStringList &paths) {
        ++loads;
        // Ghostty loads candidates in order, so the later file wins.
        for (auto path = paths.crbegin(); path != paths.crend(); ++path) {
            QFile file(*path);
            if (file.open(QIODevice::ReadOnly)) {
                bool ok = false;
                const int value = QString::fromUtf8(file.readAll()).toInt(&ok);
                if (ok) {
                    return snapshotWithMarker(value, *path);
                }
            }
        }
        return snapshotWithMarker(0);
    };

    GhosttyConfigService service({primary, fallback}, loader, 35);
    QCOMPARE(loads, 1);
    QVERIFY(service.watchedFiles().isEmpty());
    QVERIFY(service.watchedDirectories().contains(ghosttyDirectory));

    QVERIFY(replaceFile(primary, QByteArrayLiteral("1")));
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot().values.windowWidth, quint32{1}, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(service.watchedFiles().contains(primary), 1000);
    const int afterFirstReplacement = loads;

    QVERIFY(replaceFile(primary, QByteArrayLiteral("2")));
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot().values.windowWidth, quint32{2}, 1000);
    QVERIFY(loads > afterFirstReplacement);
    QTRY_VERIFY_WITH_TIMEOUT(service.watchedFiles().contains(primary), 1000);
}

void GhosttyConfigServiceTest::watchesLoadedIncludePaths()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString mainPath =
        QDir(temporary.path()).filePath(QStringLiteral("ghostty/config.ghostty"));
    const QString includePath =
        QDir(temporary.path()).filePath(QStringLiteral("shared/theme.ghostty"));
    QVERIFY(QDir().mkpath(QFileInfo(mainPath).absolutePath()));
    QVERIFY(QDir().mkpath(QFileInfo(includePath).absolutePath()));
    QVERIFY(replaceFile(mainPath, QByteArrayLiteral("main")));
    QVERIFY(replaceFile(includePath, QByteArrayLiteral("included")));

    int loads = 0;
    GhosttyConfigService service(
        {mainPath},
        [&loads, &includePath](const QStringList &) {
            ++loads;
            GhosttyConfigSnapshot snapshot = snapshotWithMarker(loads);
            snapshot.sourcePaths.append(includePath);
            return snapshot;
        },
        30);

    QVERIFY(service.watchedFiles().contains(mainPath));
    QVERIFY(service.watchedFiles().contains(includePath));
    const int initialLoads = loads;
    QVERIFY(replaceFile(includePath, QByteArrayLiteral("changed")));
    QTRY_VERIFY_WITH_TIMEOUT(loads > initialLoads, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(service.watchedFiles().contains(includePath), 1000);
}

void GhosttyConfigServiceTest::watchesMissingOptionalIncludeCreation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString mainPath =
        QDir(temporary.path()).filePath(QStringLiteral("ghostty/config.ghostty"));
    const QString optionalPath =
        QDir(temporary.path()).filePath(QStringLiteral("shared/optional.ghostty"));
    QVERIFY(QDir().mkpath(QFileInfo(mainPath).absolutePath()));
    QVERIFY(QDir().mkpath(QFileInfo(optionalPath).absolutePath()));
    QVERIFY(replaceFile(mainPath, QByteArrayLiteral("main")));

    int loads = 0;
    GhosttyConfigService service(
        {mainPath},
        [&loads, &optionalPath](const QStringList &) {
            GhosttyConfigSnapshot snapshot = snapshotWithMarker(++loads);
            snapshot.values.configFiles.append(GhosttyConfigFile{
                .path = optionalPath,
                .optional = true,
            });
            return snapshot;
        },
        30);

    const int initialLoads = loads;
    QVERIFY(replaceFile(optionalPath, QByteArrayLiteral("now-present")));
    QTRY_VERIFY_WITH_TIMEOUT(loads > initialLoads, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(service.watchedFiles().contains(optionalPath), 1000);
}

void GhosttyConfigServiceTest::debouncesReloadBursts()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = QDir(temporary.path()).filePath(QStringLiteral("config"));

    int loads = 0;
    GhosttyConfigService service(
        {path},
        [&loads](const QStringList &) {
            return snapshotWithMarker(++loads);
        },
        60);
    QCOMPARE(loads, 1);

    service.requestReload();
    service.requestReload();
    service.requestReload();
    QTRY_COMPARE_WITH_TIMEOUT(loads, 2, 1000);
    QTest::qWait(100);
    QCOMPARE(loads, 2);
}

void GhosttyConfigServiceTest::standardServiceReloadsOffGuiThread()
{
    std::atomic<int> loads = 0;
    GhosttyConfigService service([&loads](const QStringList &) {
        const int load = ++loads;
        if (load > 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        return snapshotWithMarker(load);
    });
    QCOMPARE(loads.load(), 1);

    bool guiTimerFired = false;
    QTimer::singleShot(120, &service, [&guiTimerFired] {
        guiTimerFired = true;
    });
    service.requestReload();
    QTRY_VERIFY_WITH_TIMEOUT(guiTimerFired, 200);
    QTRY_COMPARE_WITH_TIMEOUT(loads.load(), 2, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot().values.windowWidth, quint32{2}, 1000);
}

void GhosttyConfigServiceTest::synchronousReloadSupersedesOlderAsyncResult()
{
    std::atomic<int> loads = 0;
    GhosttyConfigService service([&loads](const QStringList &) {
        const int load = ++loads;
        if (load == 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        return snapshotWithMarker(load);
    });
    QCOMPARE(loads.load(), 1);

    service.requestReload();
    QTRY_COMPARE_WITH_TIMEOUT(loads.load(), 2, 1000);
    service.reloadNow();
    QCOMPARE(loads.load(), 3);
    QCOMPARE(service.snapshot().values.windowWidth, quint32{3});

    // The slow generation completes later but cannot replace generation 3.
    QTest::qWait(350);
    QCOMPARE(service.snapshot().values.windowWidth, quint32{3});
}

void GhosttyConfigServiceTest::publishesUnchangedSuccessfulReloads()
{
    int loads = 0;
    GhosttyConfigService service(
        {},
        [&loads](const QStringList &) {
            ++loads;
            return snapshotWithMarker(17);
        },
        0);
    QCOMPARE(loads, 1);

    QSignalSpy changed(&service, &GhosttyConfigService::changed);
    service.reloadNow();
    QCOMPARE(loads, 2);
    QCOMPARE(changed.count(), 1);
    const QVariant published = changed.constFirst().constFirst();
    QCOMPARE(published.metaType(),
             QMetaType::fromType<GhosttyConfigSnapshot>());
    const auto *publishedSnapshot =
        static_cast<const GhosttyConfigSnapshot *>(published.constData());
    QVERIFY(publishedSnapshot != nullptr);
    QCOMPARE(*publishedSnapshot, service.snapshot());
}

void GhosttyConfigServiceTest::retainsLastGoodSnapshotAfterFailure()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = QDir(temporary.path()).filePath(QStringLiteral("config"));

    bool fail = false;
    GhosttyConfigService service(
        {path},
        [&fail](const QStringList &) -> GhosttyConfigLoadResult {
            if (fail) {
                return std::unexpected(QStringLiteral("invalid config"));
            }
            return snapshotWithMarker(17);
        },
        20);
    QVERIFY(service.hasSnapshot());
    const GhosttyConfigSnapshot lastGood = service.snapshot();

    QSignalSpy changed(&service, &GhosttyConfigService::changed);
    QSignalSpy failed(&service, &GhosttyConfigService::reloadFailed);
    fail = true;
    service.reloadNow();

    QCOMPARE(failed.count(), 1);
    QCOMPARE(failed.constFirst().constFirst().toString(), QStringLiteral("invalid config"));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(service.snapshot(), lastGood);
    QCOMPARE(service.lastError(), QStringLiteral("invalid config"));
}

void GhosttyConfigServiceTest::changedSubscriberMayDeleteAsyncService()
{
    std::atomic<int> loads = 0;
    QPointer<GhosttyConfigService> service = new GhosttyConfigService(
        [&loads](const QStringList &) {
            return snapshotWithMarker(++loads);
        });
    QCOMPARE(loads.load(), 1);

    QObject::connect(service, &GhosttyConfigService::changed,
                     service, [&service](const GhosttyConfigSnapshot &) {
                         delete service.data();
                     });
    service->requestReload();
    QTRY_VERIFY_WITH_TIMEOUT(service.isNull(), 2000);
}

void GhosttyConfigServiceTest::failedSubscriberMayDeleteAsyncService()
{
    std::atomic<int> loads = 0;
    QPointer<GhosttyConfigService> service = new GhosttyConfigService(
        [&loads](const QStringList &) -> GhosttyConfigLoadResult {
            const int load = ++loads;
            if (load == 1) {
                return snapshotWithMarker(load);
            }
            return std::unexpected(QStringLiteral("delete-on-failure"));
        });
    QCOMPARE(loads.load(), 1);

    QObject::connect(service, &GhosttyConfigService::reloadFailed,
                     service, [&service](const QString &) {
                         delete service.data();
                     });
    service->requestReload();
    QTRY_VERIFY_WITH_TIMEOUT(service.isNull(), 2000);
}

QTEST_GUILESS_MAIN(GhosttyConfigServiceTest)

#include "test_ghostty_config_service.moc"
