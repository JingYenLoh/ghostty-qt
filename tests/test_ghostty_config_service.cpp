#include "ghostty_config_service.h"

#include "ghostty_config_snapshot_fixture.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <semaphore>
#include <thread>

namespace {

bool replaceFile(const QString &path, const QByteArray &contents)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(contents) != contents.size()) {
        return false;
    }
    return file.commit();
}

GhosttyConfigSnapshot snapshotWithMarker(int marker,
                                         const QString &sourcePath = {})
{
    GhosttyConfigSnapshot snapshot = GhosttyConfigSnapshotFixture::snapshot();
    snapshot.values.windowWidth = static_cast<quint32>(marker);
    snapshot.values.configDefaultFiles = true;
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
    void passesInitialColorScheme();
    void reloadsOnlyWhenColorSchemeChanges();
    void watchesAbsentFilesAndReaddsAtomicReplacements();
    void ignoresDefaultCandidatesWhenDisabled();
    void watchesLoadedIncludePaths();
    void watchesMissingOptionalIncludeCreation();
    void watchesExistingAndMissingThemePaths();
    void debouncesReloadBursts();
    void standardServiceReloadsOffGuiThread();
    void synchronousReloadSupersedesOlderAsyncResult();
    void colorSchemeChangeSupersedesBlockedAsyncResult();
    void publishesUnchangedSuccessfulReloads();
    void retainsLastGoodSnapshotAfterFailure();
    void backsOffRepeatedFailuresAndResetsOnReloadEvents();
    void retainsRequestedColorSchemeAfterFailure();
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
    QCOMPARE(
        GhosttyConfigService::standardConfigPaths(environment),
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
        [&observed](const GhosttyConfigLoadRequest &request) {
            observed = request.candidatePaths;
            return snapshotWithMarker(1);
        },
        20);
    QCOMPARE(observed, expected);

    environment.remove(QStringLiteral("XDG_CONFIG_HOME"));
    const QString fallbackDirectory =
        QDir(environment.value(QStringLiteral("HOME")))
            .filePath(QStringLiteral(".config/ghostty"));
    QCOMPARE(
        GhosttyConfigService::standardConfigPaths(environment),
        QStringList({
            QDir(fallbackDirectory).filePath(QStringLiteral("config")),
            QDir(fallbackDirectory).filePath(QStringLiteral("config.ghostty")),
        }));
    QCOMPARE(
        GhosttyConfigService::standardConfigEditPaths(environment),
        QStringList({
            QDir(fallbackDirectory).filePath(QStringLiteral("config.ghostty")),
            QDir(fallbackDirectory).filePath(QStringLiteral("config")),
        }));
}

void GhosttyConfigServiceTest::passesInitialColorScheme()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        QDir(temporary.path()).filePath(QStringLiteral("ghostty/config"));

    std::optional<GhosttyConfigLoadRequest> observed;
    GhosttyConfigService service(
        {path},
        [&observed](const GhosttyConfigLoadRequest &request) {
            observed = request;
            return snapshotWithMarker(1);
        },
        20, TerminalColorScheme::Dark);

    QVERIFY(observed.has_value());
    QCOMPARE(observed->candidatePaths, QStringList{QDir::cleanPath(path)});
    QVERIFY(observed->colorScheme == TerminalColorScheme::Dark);
    QVERIFY(service.colorScheme() == TerminalColorScheme::Dark);
}

void GhosttyConfigServiceTest::reloadsOnlyWhenColorSchemeChanges()
{
    QList<TerminalColorScheme> observed;
    GhosttyConfigService service(
        {},
        [&observed](const GhosttyConfigLoadRequest &request) {
            observed.append(request.colorScheme);
            return snapshotWithMarker(observed.size());
        },
        20);
    QCOMPARE(observed.size(), 1);
    QVERIFY(observed.constFirst() == TerminalColorScheme::Light);

    service.setColorScheme(TerminalColorScheme::Light);
    QTest::qWait(40);
    QCOMPARE(observed.size(), 1);

    service.setColorScheme(TerminalColorScheme::Dark);
    service.setColorScheme(TerminalColorScheme::Dark);
    QTRY_COMPARE_WITH_TIMEOUT(observed.size(), 2, 1000);
    QVERIFY(observed.constLast() == TerminalColorScheme::Dark);
    QVERIFY(service.colorScheme() == TerminalColorScheme::Dark);
    QTest::qWait(40);
    QCOMPARE(observed.size(), 2);
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
    auto loader = [&loads](const GhosttyConfigLoadRequest &request) {
        ++loads;
        // Ghostty loads candidates in order, so the later file wins.
        const QStringList &paths = request.candidatePaths;
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
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot().values.windowWidth, quint32{1},
                              1000);
    QTRY_VERIFY_WITH_TIMEOUT(service.watchedFiles().contains(primary), 1000);
    const int afterFirstReplacement = loads;

    QVERIFY(replaceFile(primary, QByteArrayLiteral("2")));
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot().values.windowWidth, quint32{2},
                              1000);
    QVERIFY(loads > afterFirstReplacement);
    QTRY_VERIFY_WITH_TIMEOUT(service.watchedFiles().contains(primary), 1000);
}

void GhosttyConfigServiceTest::ignoresDefaultCandidatesWhenDisabled()
{
    const QString localTemporaryRoot =
        QDir::current().filePath(QStringLiteral("tmp"));
    QVERIFY(QDir().mkpath(localTemporaryRoot));
    QTemporaryDir temporary(
        QDir(localTemporaryRoot)
            .filePath(QStringLiteral("config-watch-policy-XXXXXX")));
    QVERIFY(temporary.isValid());

    const QString ghosttyDirectory =
        QDir(temporary.path()).filePath(QStringLiteral("ghostty"));
    const QString sharedDirectory =
        QDir(temporary.path()).filePath(QStringLiteral("shared"));
    QVERIFY(QDir().mkpath(ghosttyDirectory));
    QVERIFY(QDir().mkpath(sharedDirectory));
    const QString legacy =
        QDir(ghosttyDirectory).filePath(QStringLiteral("config"));
    const QString preferred =
        QDir(ghosttyDirectory).filePath(QStringLiteral("config.ghostty"));
    const QString explicitConfig =
        QDir(sharedDirectory).filePath(QStringLiteral("included.ghostty"));
    QVERIFY(replaceFile(legacy, QByteArrayLiteral("legacy")));
    QVERIFY(replaceFile(preferred, QByteArrayLiteral("preferred")));
    QVERIFY(replaceFile(explicitConfig, QByteArrayLiteral("included")));

    bool fail = true;
    QStringList observedCandidatePaths;
    GhosttyConfigService service(
        {legacy, preferred},
        [&fail, &legacy, &preferred, &explicitConfig,
         &observedCandidatePaths](const GhosttyConfigLoadRequest &request)
            -> GhosttyConfigLoadResult {
            observedCandidatePaths = request.candidatePaths;
            if (fail) {
                return std::unexpected(QStringLiteral("startup failure"));
            }
            GhosttyConfigSnapshot snapshot = snapshotWithMarker(1);
            snapshot.values.configDefaultFiles = false;
            snapshot.values.configFiles = {
                GhosttyConfigFile{.path = explicitConfig},
            };
            // A defensive custom loader may still report default candidates.
            // The startup policy must filter those while retaining the
            // explicitly loaded dependency.
            snapshot.sourcePaths = {legacy, preferred, explicitConfig};
            return snapshot;
        },
        20, TerminalColorScheme::Light, false);

    QCOMPARE(observedCandidatePaths, QStringList({legacy, preferred}));
    QVERIFY(!service.hasSnapshot());
    QVERIFY(!service.watchedFiles().contains(legacy));
    QVERIFY(!service.watchedFiles().contains(preferred));
    QVERIFY(!service.watchedDirectories().contains(ghosttyDirectory));

    fail = false;
    service.reloadNow();
    QVERIFY(service.hasSnapshot());
    QVERIFY(service.watchedFiles().contains(explicitConfig));
    QVERIFY(!service.watchedFiles().contains(legacy));
    QVERIFY(!service.watchedFiles().contains(preferred));
    QVERIFY(!service.watchedDirectories().contains(ghosttyDirectory));
}

void GhosttyConfigServiceTest::watchesLoadedIncludePaths()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString mainPath =
        QDir(temporary.path())
            .filePath(QStringLiteral("ghostty/config.ghostty"));
    const QString includePath =
        QDir(temporary.path()).filePath(QStringLiteral("shared/theme.ghostty"));
    QVERIFY(QDir().mkpath(QFileInfo(mainPath).absolutePath()));
    QVERIFY(QDir().mkpath(QFileInfo(includePath).absolutePath()));
    QVERIFY(replaceFile(mainPath, QByteArrayLiteral("main")));
    QVERIFY(replaceFile(includePath, QByteArrayLiteral("included")));

    int loads = 0;
    GhosttyConfigService service(
        {mainPath},
        [&loads, &includePath](const GhosttyConfigLoadRequest &) {
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
    QTRY_VERIFY_WITH_TIMEOUT(service.watchedFiles().contains(includePath),
                             1000);
}

void GhosttyConfigServiceTest::watchesMissingOptionalIncludeCreation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString mainPath =
        QDir(temporary.path())
            .filePath(QStringLiteral("ghostty/config.ghostty"));
    const QString optionalPath =
        QDir(temporary.path())
            .filePath(QStringLiteral("shared/optional.ghostty"));
    QVERIFY(QDir().mkpath(QFileInfo(mainPath).absolutePath()));
    QVERIFY(QDir().mkpath(QFileInfo(optionalPath).absolutePath()));
    QVERIFY(replaceFile(mainPath, QByteArrayLiteral("main")));

    int loads = 0;
    GhosttyConfigService service(
        {mainPath},
        [&loads, &optionalPath](const GhosttyConfigLoadRequest &) {
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
    QTRY_VERIFY_WITH_TIMEOUT(service.watchedFiles().contains(optionalPath),
                             1000);
}

void GhosttyConfigServiceTest::watchesExistingAndMissingThemePaths()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString existingPath =
        QDir(temporary.path()).filePath(QStringLiteral("themes/light/active"));
    const QString missingPath =
        QDir(temporary.path()).filePath(QStringLiteral("themes/dark/missing"));
    const QString existingDirectory = QFileInfo(existingPath).absolutePath();
    const QString missingDirectory = QFileInfo(missingPath).absolutePath();
    QVERIFY(QDir().mkpath(existingDirectory));
    QVERIFY(QDir().mkpath(missingDirectory));
    QVERIFY(
        replaceFile(existingPath, QByteArrayLiteral("background = ffffff")));

    int loads = 0;
    GhosttyConfigService service(
        {},
        [&loads, &existingPath,
         &missingPath](const GhosttyConfigLoadRequest &) {
            GhosttyConfigSnapshot snapshot = snapshotWithMarker(++loads);
            snapshot.values.themeFiles = {existingPath, missingPath};
            return snapshot;
        },
        30);

    QVERIFY(service.watchedFiles().contains(existingPath));
    QVERIFY(service.watchedDirectories().contains(existingDirectory));
    QVERIFY(service.watchedDirectories().contains(missingDirectory));

    const int initialLoads = loads;
    QVERIFY(
        replaceFile(existingPath, QByteArrayLiteral("background = eeeeeee")));
    QTRY_VERIFY_WITH_TIMEOUT(loads > initialLoads, 1000);

    const int afterExistingEdit = loads;
    QVERIFY(replaceFile(missingPath, QByteArrayLiteral("background = 000000")));
    QTRY_VERIFY_WITH_TIMEOUT(loads > afterExistingEdit, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(service.watchedFiles().contains(missingPath),
                             1000);
}

void GhosttyConfigServiceTest::debouncesReloadBursts()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        QDir(temporary.path()).filePath(QStringLiteral("config"));

    int loads = 0;
    GhosttyConfigService service(
        {path},
        [&loads](const GhosttyConfigLoadRequest &) {
            return snapshotWithMarker(++loads);
        },
        60);
    QCOMPARE(loads, 1);

    QSignalSpy scheduled(&service, &GhosttyConfigService::reloadScheduled);
    QSignalSpy settled(&service, &GhosttyConfigService::reloadSettled);
    service.requestReload();
    service.requestReload();
    service.requestReload();
    QTRY_COMPARE_WITH_TIMEOUT(loads, 2, 1000);
    QTest::qWait(100);
    QCOMPARE(loads, 2);
    QVERIFY(scheduled.count() >= 3);
    QCOMPARE(settled.count(), 1);
    QCOMPARE(settled.constFirst().constFirst().toULongLong(),
             scheduled.constLast().constFirst().toULongLong());
}

void GhosttyConfigServiceTest::standardServiceReloadsOffGuiThread()
{
    std::atomic<int> loads = 0;
    GhosttyConfigService service([&loads](const GhosttyConfigLoadRequest &) {
        const int load = ++loads;
        if (load > 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        return snapshotWithMarker(load);
    });
    QCOMPARE(loads.load(), 1);

    bool guiTimerFired = false;
    QTimer::singleShot(120, &service,
                       [&guiTimerFired] { guiTimerFired = true; });
    service.requestReload();
    QTRY_VERIFY_WITH_TIMEOUT(guiTimerFired, 200);
    QTRY_COMPARE_WITH_TIMEOUT(loads.load(), 2, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot().values.windowWidth, quint32{2},
                              1000);
}

void GhosttyConfigServiceTest::synchronousReloadSupersedesOlderAsyncResult()
{
    std::atomic<int> loads = 0;
    {
        std::binary_semaphore blockedLoadStarted{0};
        std::binary_semaphore releaseBlockedLoad{0};
        bool blockedLoadReleased = false;
        GhosttyConfigService service(
            [&loads, &blockedLoadStarted,
             &releaseBlockedLoad](const GhosttyConfigLoadRequest &) {
                const int load = ++loads;
                if (load == 2) {
                    blockedLoadStarted.release();
                    releaseBlockedLoad.acquire();
                }
                return snapshotWithMarker(load);
            });
        const auto unblockOnFailure = qScopeGuard([&] {
            if (!blockedLoadReleased) releaseBlockedLoad.release();
        });
        QCOMPARE(loads.load(), 1);

        QSignalSpy scheduled(&service, &GhosttyConfigService::reloadScheduled);
        QSignalSpy settled(&service, &GhosttyConfigService::reloadSettled);
        service.requestReload();
        QTRY_COMPARE_WITH_TIMEOUT(loads.load(), 2, 1000);
        QVERIFY2(blockedLoadStarted.try_acquire_for(std::chrono::seconds(1)),
                 "the asynchronous reload did not enter its blocked section");

        // Let a second request become the worker's queued follow-up. A
        // synchronous reload supersedes that request as well as generation 2.
        service.requestReload();
        QTest::qWait(GhosttyConfigService::DefaultDebounceMilliseconds + 25);
        blockedLoadReleased = true;
        releaseBlockedLoad.release();
        service.reloadNow();
        QCOMPARE(loads.load(), 3);
        QCOMPARE(service.snapshot().values.windowWidth, quint32{3});
        QCOMPARE(scheduled.count(), 3);
        QCOMPARE(settled.count(), 1);
        QCOMPARE(settled.constFirst(), scheduled.constLast());

        // The blocked generation's queued callback cannot replace generation
        // 3 or consume the superseded follow-up to start generation 4.
        QTRY_COMPARE_WITH_TIMEOUT(settled.count(), 2, 1000);
        QCOMPARE(service.snapshot().values.windowWidth, quint32{3});
        QCOMPARE(settled.constLast(), scheduled.constFirst());
    }
    // Destruction joins any worker that the stale callback might have queued.
    QCOMPARE(loads.load(), 3);
}

void GhosttyConfigServiceTest::colorSchemeChangeSupersedesBlockedAsyncResult()
{
    std::atomic<int> loads = 0;
    std::atomic<TerminalColorScheme> blockedScheme = TerminalColorScheme::Light;
    std::binary_semaphore blockedLoadStarted{0};
    std::binary_semaphore releaseBlockedLoad{0};
    bool blockedLoadReleased = false;
    GhosttyConfigService service(
        [&loads, &blockedScheme, &blockedLoadStarted,
         &releaseBlockedLoad](const GhosttyConfigLoadRequest &request) {
            const int load = ++loads;
            if (load == 2) {
                blockedScheme.store(request.colorScheme);
                blockedLoadStarted.release();
                releaseBlockedLoad.acquire();
            }
            const int schemeMarker =
                request.colorScheme == TerminalColorScheme::Dark ? 200 : 100;
            return snapshotWithMarker(schemeMarker + load);
        });
    const auto unblockOnFailure = qScopeGuard([&] {
        if (!blockedLoadReleased) {
            releaseBlockedLoad.release();
        }
    });
    QCOMPARE(loads.load(), 1);
    QCOMPARE(service.snapshot().values.windowWidth, quint32{101});

    QSignalSpy changed(&service, &GhosttyConfigService::changed);
    service.setColorScheme(TerminalColorScheme::Dark);
    QTRY_COMPARE_WITH_TIMEOUT(loads.load(), 2, 1000);
    const bool started =
        blockedLoadStarted.try_acquire_for(std::chrono::seconds(1));
    QVERIFY2(started, "the asynchronous color-scheme reload did not start");
    QVERIFY(blockedScheme.load() == TerminalColorScheme::Dark);

    // Each update immediately invalidates the blocked result, while the
    // debounce and single-worker queue collapse them to one final light load.
    service.setColorScheme(TerminalColorScheme::Light);
    service.setColorScheme(TerminalColorScheme::Dark);
    service.setColorScheme(TerminalColorScheme::Light);
    QTest::qWait(GhosttyConfigService::DefaultDebounceMilliseconds + 25);
    blockedLoadReleased = true;
    releaseBlockedLoad.release();

    QTRY_COMPARE_WITH_TIMEOUT(loads.load(), 3, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot().values.windowWidth,
                              quint32{103}, 2000);
    QCOMPARE(changed.count(), 1);
    QVERIFY(service.colorScheme() == TerminalColorScheme::Light);
}

void GhosttyConfigServiceTest::publishesUnchangedSuccessfulReloads()
{
    int loads = 0;
    GhosttyConfigService service(
        {},
        [&loads](const GhosttyConfigLoadRequest &) {
            ++loads;
            return snapshotWithMarker(17);
        },
        0);
    QCOMPARE(loads, 1);

    QSignalSpy changed(&service, &GhosttyConfigService::changed);
    QSignalSpy scheduled(&service, &GhosttyConfigService::reloadScheduled);
    QSignalSpy settled(&service, &GhosttyConfigService::reloadSettled);
    service.reloadNow();
    QCOMPARE(loads, 2);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(scheduled.count(), 1);
    QCOMPARE(settled.count(), 1);
    QCOMPARE(settled.constFirst(), scheduled.constFirst());
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
    const QString path =
        QDir(temporary.path()).filePath(QStringLiteral("config"));

    bool fail = false;
    GhosttyConfigService service(
        {path},
        [&fail](const GhosttyConfigLoadRequest &) -> GhosttyConfigLoadResult {
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
    QSignalSpy settled(&service, &GhosttyConfigService::reloadSettled);
    fail = true;
    service.reloadNow();

    QCOMPARE(failed.count(), 1);
    QCOMPARE(settled.count(), 1);
    QCOMPARE(failed.constFirst().constFirst().toString(),
             QStringLiteral("invalid config"));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(service.snapshot(), lastGood);
    QCOMPARE(service.lastError(), QStringLiteral("invalid config"));
}

void GhosttyConfigServiceTest::backsOffRepeatedFailuresAndResetsOnReloadEvents()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        QDir(temporary.path()).filePath(QStringLiteral("config"));
    QVERIFY(replaceFile(path, QByteArrayLiteral("initial")));

    int loads = 0;
    bool fail = false;
    constexpr int initialRetryMilliseconds = 150;
    constexpr int maximumRetryMilliseconds = 300;
    GhosttyConfigService service(
        {path},
        [&loads,
         &fail](const GhosttyConfigLoadRequest &) -> GhosttyConfigLoadResult {
            ++loads;
            if (fail) {
                return std::unexpected(QStringLiteral("invalid config"));
            }
            return snapshotWithMarker(loads);
        },
        0, initialRetryMilliseconds, maximumRetryMilliseconds);
    QCOMPARE(loads, 1);

    QSignalSpy failed(&service, &GhosttyConfigService::reloadFailed);
    fail = true;
    service.reloadNow();
    QCOMPARE(failed.count(), 1);
    QCOMPARE(service.scheduledFailureRetryMilliseconds(),
             initialRetryMilliseconds);

    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 2, 1'000);
    QCOMPARE(service.scheduledFailureRetryMilliseconds(),
             maximumRetryMilliseconds);
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 3, 1'000);
    QCOMPARE(service.scheduledFailureRetryMilliseconds(),
             maximumRetryMilliseconds);

    service.requestReload();
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 4, 1'000);
    QCOMPARE(service.scheduledFailureRetryMilliseconds(),
             initialRetryMilliseconds);

    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 5, 1'000);
    QCOMPARE(service.scheduledFailureRetryMilliseconds(),
             maximumRetryMilliseconds);
    QVERIFY(replaceFile(path, QByteArrayLiteral("still-invalid")));
    QTRY_VERIFY_WITH_TIMEOUT(failed.count() >= 6, 1'000);
    QCOMPARE(service.scheduledFailureRetryMilliseconds(),
             initialRetryMilliseconds);
}

void GhosttyConfigServiceTest::retainsRequestedColorSchemeAfterFailure()
{
    int loads = 0;
    bool failDark = true;
    QList<GhosttyConfigLoadRequest> observed;
    GhosttyConfigService service(
        {},
        [&loads, &failDark, &observed](const GhosttyConfigLoadRequest &request)
            -> GhosttyConfigLoadResult {
            observed.append(request);
            ++loads;
            if (failDark && request.colorScheme == TerminalColorScheme::Dark) {
                return std::unexpected(
                    QStringLiteral("dark configuration is invalid"));
            }
            return snapshotWithMarker(loads);
        },
        0);
    const GhosttyConfigSnapshot lastGood = service.snapshot();

    QSignalSpy changed(&service, &GhosttyConfigService::changed);
    QSignalSpy failed(&service, &GhosttyConfigService::reloadFailed);
    service.setColorScheme(TerminalColorScheme::Dark);
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 1000);

    QCOMPARE(changed.count(), 0);
    QCOMPARE(service.snapshot(), lastGood);
    QVERIFY(service.colorScheme() == TerminalColorScheme::Dark);
    QVERIFY(observed.constLast().colorScheme == TerminalColorScheme::Dark);
    QCOMPARE(service.lastError(),
             QStringLiteral("dark configuration is invalid"));

    failDark = false;
    service.reloadNow();
    QCOMPARE(changed.count(), 1);
    QCOMPARE(service.snapshot().values.windowWidth, quint32{3});
    QVERIFY(observed.constLast().colorScheme == TerminalColorScheme::Dark);
    QVERIFY(service.lastError().isEmpty());
}

void GhosttyConfigServiceTest::changedSubscriberMayDeleteAsyncService()
{
    std::atomic<int> loads = 0;
    QPointer<GhosttyConfigService> service =
        new GhosttyConfigService([&loads](const GhosttyConfigLoadRequest &) {
            return snapshotWithMarker(++loads);
        });
    QCOMPARE(loads.load(), 1);

    QObject::connect(
        service, &GhosttyConfigService::changed, service,
        [&service](const GhosttyConfigSnapshot &) { delete service.data(); });
    service->requestReload();
    QTRY_VERIFY_WITH_TIMEOUT(service.isNull(), 2000);
}

void GhosttyConfigServiceTest::failedSubscriberMayDeleteAsyncService()
{
    std::atomic<int> loads = 0;
    QPointer<GhosttyConfigService> service = new GhosttyConfigService(
        [&loads](const GhosttyConfigLoadRequest &) -> GhosttyConfigLoadResult {
            const int load = ++loads;
            if (load == 1) {
                return snapshotWithMarker(load);
            }
            return std::unexpected(QStringLiteral("delete-on-failure"));
        });
    QCOMPARE(loads.load(), 1);

    QObject::connect(service, &GhosttyConfigService::reloadFailed, service,
                     [&service](const QString &) { delete service.data(); });
    service->requestReload();
    QTRY_VERIFY_WITH_TIMEOUT(service.isNull(), 2000);
}

QTEST_GUILESS_MAIN(GhosttyConfigServiceTest)

#include "test_ghostty_config_service.moc"
