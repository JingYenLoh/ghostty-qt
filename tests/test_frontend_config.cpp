#include "frontend_config.h"
#include "frontend_config_service.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <thread>

#include <sys/stat.h>

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

FrontendConfigSnapshot snapshotWithMarker(int marker)
{
    FrontendConfigSnapshot snapshot;
    snapshot.sourcePath = QString::number(marker);
    snapshot.values.tabsLocation =
        marker % 2 == 0 ? TabsLocation::Top : TabsLocation::Bottom;
    return snapshot;
}

QString errorMessage(const FrontendConfigLoadResult &result)
{
    return result ? QString{} : result.error();
}

} // namespace

class FrontendConfigTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void parsesDefaultsAndFullLineComments();
    void parsesEveryValue_data();
    void parsesEveryValue();
    void acceptsWhitespaceCrLfAndBom();
    void delegatesGhosttyLinesWithoutInterpretingTheirGrammar();
    void rejectsInvalidDocuments_data();
    void rejectsInvalidDocuments();
    void rejectsInvalidUtf8AndControlCharacters();
    void reportsDuplicatePathAndLine();
    void loadsMissingFileAsDefaults();
    void loadsExistingFileWithCanonicalSourcePath();
    void rejectsInvalidPathNonRegularAndOversizedFiles();
    void acceptsMaximumSizeAndReportsReadFailures();
    void resolvesStandardConfigPath();
    void watchesAbsentFileAndAtomicReplacements();
    void debouncesAsynchronousReloads();
    void asynchronousReloadDoesNotBlockEventLoop();
    void synchronousReloadSupersedesOlderAsyncResult();
    void publishesUnchangedSuccessfulReloads();
    void retainsLastGoodSnapshotAfterFailure();
    void changedSubscriberMayDeleteService();
    void failedSubscriberMayDeleteService();
};

void FrontendConfigTest::parsesDefaultsAndFullLineComments()
{
    const auto parsed =
        parseFrontendConfig(QByteArrayView("# frontend settings\n"
                                           "   # another full-line comment\n"
                                           "\n"),
                            QStringLiteral("memory"));
    QVERIFY2(parsed.has_value(),
             qPrintable(parsed ? QString{} : parsed.error()));
    QCOMPARE(parsed->singleInstanceMode, SingleInstanceMode::Detect);
    QCOMPARE(parsed->tabsLocation, TabsLocation::Top);
    QVERIFY(parsed->wideTabs);
    QVERIFY(parsed->horizontalTabScroll);
    QCOMPARE(parsed->quickTerminalLayerShell.layer, QuickTerminalLayer::Top);
    QCOMPARE(parsed->quickTerminalLayerShell.layerNamespace,
             QStringLiteral("ghostty-quick-terminal"));
    QVERIFY(parsed->paneEnterTransitionShaderPath.isEmpty());
    QVERIFY(parsed->paneExitTransitionShaderPath.isEmpty());
    QCOMPARE(parsed->paneEnterTransitionDuration,
             std::chrono::milliseconds::zero());
    QCOMPARE(parsed->paneExitTransitionDuration,
             std::chrono::milliseconds::zero());
}

void FrontendConfigTest::parsesEveryValue_data()
{
    QTest::addColumn<QByteArray>("contents");
    QTest::addColumn<SingleInstanceMode>("singleInstance");
    QTest::addColumn<TabsLocation>("tabsLocation");
    QTest::addColumn<bool>("wideTabs");
    QTest::addColumn<bool>("horizontalTabScroll");
    QTest::addColumn<int>("quickTerminalLayer");
    QTest::addColumn<QString>("quickTerminalNamespace");
    QTest::addColumn<QString>("paneEnterShader");
    QTest::addColumn<QString>("paneExitShader");
    QTest::addColumn<qint64>("paneEnterMilliseconds");
    QTest::addColumn<qint64>("paneExitMilliseconds");

    QTest::newRow("false-top-wide-scroll-background")
        << QByteArray("single-instance = false\n"
                      "tabs-location = top\n"
                      "wide-tabs = true\n"
                      "horizontal-tab-scroll = true\n"
                      "quick-terminal-layer = background\n"
                      "quick-terminal-namespace = background-terminal\n"
                      "pane-enter-transition-shader = enter-a.glsl\n"
                      "pane-exit-transition-shader = exit-a.glsl\n"
                      "pane-enter-transition-duration = 0ms\n"
                      "pane-exit-transition-duration = 10000ms\n")
        << SingleInstanceMode::Disabled << TabsLocation::Top << true << true
        << static_cast<int>(QuickTerminalLayer::Background)
        << QStringLiteral("background-terminal")
        << QStringLiteral("enter-a.glsl") << QStringLiteral("exit-a.glsl")
        << qint64(0) << qint64(10000);
    QTest::newRow("true-bottom-compact-scroll-bottom")
        << QByteArray("single-instance = true\n"
                      "tabs-location = bottom\n"
                      "wide-tabs = false\n"
                      "horizontal-tab-scroll = true\n"
                      "quick-terminal-layer = bottom\n"
                      "quick-terminal-namespace = bottom-terminal\n"
                      "pane-enter-transition-shader = enter-b.glsl\n"
                      "pane-exit-transition-shader = exit-b.glsl\n"
                      "pane-enter-transition-duration = 180ms\n"
                      "pane-exit-transition-duration = 220ms\n")
        << SingleInstanceMode::Enabled << TabsLocation::Bottom << false << true
        << static_cast<int>(QuickTerminalLayer::Bottom)
        << QStringLiteral("bottom-terminal") << QStringLiteral("enter-b.glsl")
        << QStringLiteral("exit-b.glsl") << qint64(180) << qint64(220);
    QTest::newRow("detect-bottom-wide-no-scroll-top")
        << QByteArray("single-instance = detect\n"
                      "tabs-location = bottom\n"
                      "wide-tabs = true\n"
                      "horizontal-tab-scroll = false\n"
                      "quick-terminal-layer = top\n"
                      "quick-terminal-namespace = top-terminal\n"
                      "pane-enter-transition-shader = enter-c.glsl\n"
                      "pane-exit-transition-shader = exit-c.glsl\n"
                      "pane-enter-transition-duration = 1ms\n"
                      "pane-exit-transition-duration = 9999ms\n")
        << SingleInstanceMode::Detect << TabsLocation::Bottom << true << false
        << static_cast<int>(QuickTerminalLayer::Top)
        << QStringLiteral("top-terminal") << QStringLiteral("enter-c.glsl")
        << QStringLiteral("exit-c.glsl") << qint64(1) << qint64(9999);
    QTest::newRow("detect-top-compact-no-scroll-overlay")
        << QByteArray("single-instance = detect\n"
                      "tabs-location = top\n"
                      "wide-tabs = false\n"
                      "horizontal-tab-scroll = false\n"
                      "quick-terminal-layer = overlay\n"
                      "quick-terminal-namespace = overlay-terminal\n"
                      "pane-enter-transition-shader = enter-d.glsl\n"
                      "pane-exit-transition-shader = exit-d.glsl\n"
                      "pane-enter-transition-duration = 300ms\n"
                      "pane-exit-transition-duration = 0ms\n")
        << SingleInstanceMode::Detect << TabsLocation::Top << false << false
        << static_cast<int>(QuickTerminalLayer::Overlay)
        << QStringLiteral("overlay-terminal") << QStringLiteral("enter-d.glsl")
        << QStringLiteral("exit-d.glsl") << qint64(300) << qint64(0);
}

void FrontendConfigTest::parsesEveryValue()
{
    QFETCH(QByteArray, contents);
    QFETCH(SingleInstanceMode, singleInstance);
    QFETCH(TabsLocation, tabsLocation);
    QFETCH(bool, wideTabs);
    QFETCH(bool, horizontalTabScroll);
    QFETCH(int, quickTerminalLayer);
    QFETCH(QString, quickTerminalNamespace);
    QFETCH(QString, paneEnterShader);
    QFETCH(QString, paneExitShader);
    QFETCH(qint64, paneEnterMilliseconds);
    QFETCH(qint64, paneExitMilliseconds);

    const auto parsed = parseFrontendConfig(contents, QStringLiteral("memory"));
    QVERIFY2(parsed.has_value(),
             qPrintable(parsed ? QString{} : parsed.error()));
    QCOMPARE(parsed->singleInstanceMode, singleInstance);
    QCOMPARE(parsed->tabsLocation, tabsLocation);
    QCOMPARE(parsed->wideTabs, wideTabs);
    QCOMPARE(parsed->horizontalTabScroll, horizontalTabScroll);
    QCOMPARE(static_cast<int>(parsed->quickTerminalLayerShell.layer),
             quickTerminalLayer);
    QCOMPARE(parsed->quickTerminalLayerShell.layerNamespace,
             quickTerminalNamespace);
    QCOMPARE(parsed->paneEnterTransitionShaderPath, paneEnterShader);
    QCOMPARE(parsed->paneExitTransitionShaderPath, paneExitShader);
    QCOMPARE(parsed->paneEnterTransitionDuration,
             std::chrono::milliseconds(paneEnterMilliseconds));
    QCOMPARE(parsed->paneExitTransitionDuration,
             std::chrono::milliseconds(paneExitMilliseconds));
}

void FrontendConfigTest::acceptsWhitespaceCrLfAndBom()
{
    const QByteArray contents = QByteArray::fromHex("efbbbf")
        + QByteArray("\tsingle-instance\t=\ttrue\t\r\n"
                     " tabs-location = bottom \r\n"
                     " wide-tabs = false \r\n"
                     " horizontal-tab-scroll = false \r\n"
                     " quick-terminal-layer = overlay \r\n"
                     " quick-terminal-namespace = custom-scope \r\n"
                     " pane-enter-transition-shader = enter.glsl \r\n"
                     " pane-exit-transition-shader = exit.glsl \r\n"
                     " pane-enter-transition-duration = 75ms \r\n"
                     " pane-exit-transition-duration = 125ms \r\n");
    const auto parsed = parseFrontendConfig(contents, QStringLiteral("memory"));
    QVERIFY2(parsed.has_value(),
             qPrintable(parsed ? QString{} : parsed.error()));
    QCOMPARE(parsed->singleInstanceMode, SingleInstanceMode::Enabled);
    QCOMPARE(parsed->tabsLocation, TabsLocation::Bottom);
    QVERIFY(!parsed->wideTabs);
    QVERIFY(!parsed->horizontalTabScroll);
    QCOMPARE(parsed->quickTerminalLayerShell.layer,
             QuickTerminalLayer::Overlay);
    QCOMPARE(parsed->quickTerminalLayerShell.layerNamespace,
             QStringLiteral("custom-scope"));
    QCOMPARE(parsed->paneEnterTransitionShaderPath,
             QStringLiteral("enter.glsl"));
    QCOMPARE(parsed->paneExitTransitionShaderPath, QStringLiteral("exit.glsl"));
    QCOMPARE(parsed->paneEnterTransitionDuration,
             std::chrono::milliseconds(75));
    QCOMPARE(parsed->paneExitTransitionDuration,
             std::chrono::milliseconds(125));
}

void FrontendConfigTest::rejectsInvalidDocuments_data()
{
    QTest::addColumn<QByteArray>("contents");
    QTest::addColumn<QString>("diagnostic");

    QTest::newRow("missing-equals") << QByteArray("single-instance true\n")
                                    << QStringLiteral("exactly one");
    QTest::newRow("multiple-equals")
        << QByteArray("single-instance = true = false\n")
        << QStringLiteral("exactly one");
    QTest::newRow("empty-value") << QByteArray("tabs-location = \n")
                                 << QStringLiteral("must not be empty");
    QTest::newRow("single-instance-case")
        << QByteArray("single-instance = True\n")
        << QStringLiteral("expected false, true, or detect");
    QTest::newRow("tabs-location") << QByteArray("tabs-location = left\n")
                                   << QStringLiteral("expected top or bottom");
    QTest::newRow("wide-tabs") << QByteArray("wide-tabs = yes\n")
                               << QStringLiteral("expected true or false");
    QTest::newRow("horizontal-tab-scroll")
        << QByteArray("horizontal-tab-scroll = True\n")
        << QStringLiteral("expected true or false");
    QTest::newRow("quick-terminal-layer")
        << QByteArray("quick-terminal-layer = desktop\n")
        << QStringLiteral("expected background, bottom, top, or overlay");
    QTest::newRow("quick-terminal-namespace-empty")
        << QByteArray("quick-terminal-namespace = \n")
        << QStringLiteral("must not be empty");
    QTest::newRow("pane-enter-duration-unit")
        << QByteArray("pane-enter-transition-duration = 200\n")
        << QStringLiteral("expected 0ms through 10000ms");
    QTest::newRow("pane-exit-duration-negative")
        << QByteArray("pane-exit-transition-duration = -1ms\n")
        << QStringLiteral("expected 0ms through 10000ms");
    QTest::newRow("pane-exit-duration-over-limit")
        << QByteArray("pane-exit-transition-duration = 10001ms\n")
        << QStringLiteral("expected 0ms through 10000ms");
    QTest::newRow("inline-comment")
        << QByteArray("tabs-location = top # not a full-line comment\n")
        << QStringLiteral("expected top or bottom");
}

void FrontendConfigTest::rejectsInvalidDocuments()
{
    QFETCH(QByteArray, contents);
    QFETCH(QString, diagnostic);
    const auto parsed =
        parseFrontendConfig(contents, QStringLiteral("settings.conf"));
    QVERIFY(!parsed.has_value());
    QVERIFY2(parsed.error().contains(QStringLiteral("settings.conf:1:")),
             qPrintable(parsed.error()));
    QVERIFY2(parsed.error().contains(diagnostic), qPrintable(parsed.error()));
}

void FrontendConfigTest::rejectsInvalidUtf8AndControlCharacters()
{
    QByteArray invalidUtf8("tabs-location = ");
    invalidUtf8.append(char(0xC3));
    invalidUtf8.append(char(0x28));
    auto parsed =
        parseFrontendConfig(invalidUtf8, QStringLiteral("settings.conf"));
    QVERIFY(!parsed.has_value());
    QVERIFY(parsed.error().contains(QStringLiteral("not valid UTF-8")));

    QByteArray control("tabs-location = top");
    control.append(char(0));
    control.append('\n');
    parsed = parseFrontendConfig(control, QStringLiteral("settings.conf"));
    QVERIFY(!parsed.has_value());
    QVERIFY(parsed.error().contains(
        QStringLiteral("settings.conf:1: invalid control character")));
}

void FrontendConfigTest::delegatesGhosttyLinesWithoutInterpretingTheirGrammar()
{
    QByteArray contents("maximize = true\n"
                        "maximize = false\n"
                        "env = VALUE=contains=equals\n"
                        "gtk-wide-tabs = false\n"
                        "unknown-to-both-parsers = value\n");
    contents.append("env = RAW=");
    contents.append(char(0xFF));
    contents.append('\n');
    contents.append("tabs-location = bottom\n");

    const auto parsed =
        parseFrontendConfig(contents, QStringLiteral("mixed.conf"));
    QVERIFY2(parsed.has_value(),
             qPrintable(parsed ? QString{} : parsed.error()));
    QCOMPARE(parsed->tabsLocation, TabsLocation::Bottom);
}

void FrontendConfigTest::reportsDuplicatePathAndLine()
{
    const auto parsed =
        parseFrontendConfig(QByteArrayView("single-instance = true\n"
                                           "# separator\n"
                                           "single-instance = false\n"),
                            QStringLiteral("/tmp/ghostty-qt/config"));
    QVERIFY(!parsed.has_value());
    QVERIFY2(
        parsed.error().contains(QStringLiteral("/tmp/ghostty-qt/config:3:")),
        qPrintable(parsed.error()));
    QVERIFY(parsed.error().contains(
        QStringLiteral("duplicate key 'single-instance'")));
}

void FrontendConfigTest::loadsMissingFileAsDefaults()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        QDir(temporary.path()).filePath(QStringLiteral("missing/config"));
    const FrontendConfigLoadResult loaded = loadFrontendConfigFile(path);
    QVERIFY2(loaded.has_value(), qPrintable(errorMessage(loaded)));
    QCOMPARE(loaded->values, FrontendConfigValues{});
    QVERIFY(loaded->sourcePath.isEmpty());
}

void FrontendConfigTest::loadsExistingFileWithCanonicalSourcePath()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString directory =
        QDir(temporary.path()).filePath(QStringLiteral("ghostty-qt"));
    QVERIFY(QDir().mkpath(directory));
    const QString path =
        QDir(directory).filePath(QStringLiteral("../ghostty-qt/config"));
    QVERIFY(replaceFile(QDir::cleanPath(path),
                        QByteArrayLiteral("tabs-location = bottom\n")));

    const FrontendConfigLoadResult loaded = loadFrontendConfigFile(path);
    QVERIFY2(loaded.has_value(), qPrintable(errorMessage(loaded)));
    QCOMPARE(loaded->values.tabsLocation, TabsLocation::Bottom);
    QCOMPARE(loaded->sourcePath,
             QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
}

void FrontendConfigTest::rejectsInvalidPathNonRegularAndOversizedFiles()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    QString nulPath = temporary.path();
    nulPath += QChar::Null;
    nulPath += QStringLiteral("ignored");
    const FrontendConfigLoadResult nul = loadFrontendConfigFile(nulPath);
    QVERIFY(!nul.has_value());
    QVERIFY2(nul.error().contains(QStringLiteral("NUL")),
             qPrintable(nul.error()));

    const FrontendConfigLoadResult directory =
        loadFrontendConfigFile(temporary.path());
    QVERIFY(!directory.has_value());
    QVERIFY2(directory.error().contains(QStringLiteral("regular file")),
             qPrintable(directory.error()));

    const QString fifoPath =
        QDir(temporary.path()).filePath(QStringLiteral("config-fifo"));
    const QByteArray nativeFifoPath = QFile::encodeName(fifoPath);
    QCOMPARE(::mkfifo(nativeFifoPath.constData(), 0600), 0);
    const FrontendConfigLoadResult fifo = loadFrontendConfigFile(fifoPath);
    QVERIFY(!fifo.has_value());
    QVERIFY2(fifo.error().contains(QStringLiteral("regular file")),
             qPrintable(fifo.error()));

    const QString oversizedPath =
        QDir(temporary.path()).filePath(QStringLiteral("oversized"));
    QFile oversized(oversizedPath);
    QVERIFY(oversized.open(QIODevice::WriteOnly));
    QVERIFY(oversized.resize(MaximumFrontendConfigFileSize + 1));
    oversized.close();

    const FrontendConfigLoadResult oversizedResult =
        loadFrontendConfigFile(oversizedPath);
    QVERIFY(!oversizedResult.has_value());
    QVERIFY2(oversizedResult.error().contains(QStringLiteral("1 MiB")),
             qPrintable(oversizedResult.error()));
}

void FrontendConfigTest::acceptsMaximumSizeAndReportsReadFailures()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const QString maximumPath =
        QDir(temporary.path()).filePath(QStringLiteral("maximum"));
    QByteArray maximum(MaximumFrontendConfigFileSize, 'x');
    maximum.front() = '#';
    QVERIFY(replaceFile(maximumPath, maximum));
    const FrontendConfigLoadResult accepted =
        loadFrontendConfigFile(maximumPath);
    QVERIFY2(accepted.has_value(), qPrintable(errorMessage(accepted)));

    const QString unreadablePath = QStringLiteral("/proc/self/mem");
    if (!QFileInfo::exists(unreadablePath)) QSKIP("procfs is unavailable");
    const FrontendConfigLoadResult unreadable =
        loadFrontendConfigFile(unreadablePath);
    QVERIFY(!unreadable.has_value());
    QVERIFY2(unreadable.error().contains(QStringLiteral("could not read")),
             qPrintable(unreadable.error()));
}

void FrontendConfigTest::resolvesStandardConfigPath()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QProcessEnvironment environment;
    const QString home =
        QDir(temporary.path()).filePath(QStringLiteral("home"));
    const QString xdg = QDir(temporary.path()).filePath(QStringLiteral("xdg"));
    environment.insert(QStringLiteral("HOME"), home);
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), xdg);
    QCOMPARE(FrontendConfigService::standardConfigPath(environment),
             QDir(xdg).filePath(QStringLiteral("ghostty-qt/config")));

    environment.insert(QStringLiteral("XDG_CONFIG_HOME"),
                       QStringLiteral("relative"));
    QCOMPARE(FrontendConfigService::standardConfigPath(environment),
             QDir(home).filePath(QStringLiteral(".config/ghostty-qt/config")));

    environment.remove(QStringLiteral("XDG_CONFIG_HOME"));
    QCOMPARE(FrontendConfigService::standardConfigPath(environment),
             QDir(home).filePath(QStringLiteral(".config/ghostty-qt/config")));
}

void FrontendConfigTest::watchesAbsentFileAndAtomicReplacements()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString directory =
        QDir(temporary.path()).filePath(QStringLiteral("ghostty-qt"));
    QVERIFY(QDir().mkpath(directory));
    const QString path = QDir(directory).filePath(QStringLiteral("config"));

    FrontendConfigService service(
        path,
        [](const QString &candidate) {
            return loadFrontendConfigFile(candidate);
        },
        35);
    QVERIFY(service.hasSnapshot());
    QCOMPARE(service.snapshot().values.tabsLocation, TabsLocation::Top);
    QVERIFY(service.watchedFiles().isEmpty());
    QVERIFY(service.watchedDirectories().contains(directory));

    QVERIFY(replaceFile(path, QByteArrayLiteral("tabs-location = bottom\n")));
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot().values.tabsLocation,
                              TabsLocation::Bottom, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(service.watchedFiles().contains(path), 1000);

    QVERIFY(replaceFile(path, QByteArrayLiteral("tabs-location = top\n")));
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot().values.tabsLocation,
                              TabsLocation::Top, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(service.watchedFiles().contains(path), 1000);
}

void FrontendConfigTest::debouncesAsynchronousReloads()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    std::atomic<int> loads = 0;
    FrontendConfigService service(
        QDir(temporary.path()).filePath(QStringLiteral("config")),
        [&loads](const QString &) { return snapshotWithMarker(++loads); }, 60);
    QCOMPARE(loads.load(), 1);

    QSignalSpy scheduled(&service, &FrontendConfigService::reloadScheduled);
    QSignalSpy settled(&service, &FrontendConfigService::reloadSettled);
    service.requestReload();
    service.requestReload();
    service.requestReload();
    QTRY_COMPARE_WITH_TIMEOUT(loads.load(), 2, 1000);
    QTest::qWait(100);
    QCOMPARE(loads.load(), 2);
    QCOMPARE(scheduled.count(), 3);
    QCOMPARE(settled.count(), 1);
    QCOMPARE(settled.constFirst().constFirst().toULongLong(),
             scheduled.constLast().constFirst().toULongLong());
}

void FrontendConfigTest::asynchronousReloadDoesNotBlockEventLoop()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    std::atomic<int> loads = 0;
    FrontendConfigService service(
        QDir(temporary.path()).filePath(QStringLiteral("config")),
        [&loads](const QString &) {
            const int load = ++loads;
            if (load > 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            return snapshotWithMarker(load);
        },
        0);
    QCOMPARE(loads.load(), 1);

    bool timerFired = false;
    QTimer::singleShot(100, &service, [&timerFired] { timerFired = true; });
    service.requestReload();
    QTRY_VERIFY_WITH_TIMEOUT(timerFired, 180);
    QTRY_COMPARE_WITH_TIMEOUT(loads.load(), 2, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot().sourcePath,
                              QStringLiteral("2"), 1000);
}

void FrontendConfigTest::synchronousReloadSupersedesOlderAsyncResult()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    std::atomic<int> loads = 0;
    FrontendConfigService service(
        QDir(temporary.path()).filePath(QStringLiteral("config")),
        [&loads](const QString &) {
            const int load = ++loads;
            if (load == 2) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            return snapshotWithMarker(load);
        },
        0);
    QCOMPARE(loads.load(), 1);

    QSignalSpy scheduled(&service, &FrontendConfigService::reloadScheduled);
    QSignalSpy settled(&service, &FrontendConfigService::reloadSettled);
    service.requestReload();
    QTRY_COMPARE_WITH_TIMEOUT(loads.load(), 2, 1000);

    // Let a second request reach beginAsyncReload while generation 2 still
    // owns the worker. reloadNow must supersede both that generation and its
    // queued follow-up instead of allowing an untracked generation 4 later.
    service.requestReload();
    QTest::qWait(20);
    service.reloadNow();
    QCOMPARE(loads.load(), 3);
    QCOMPARE(service.snapshot().sourcePath, QStringLiteral("3"));
    QCOMPARE(scheduled.count(), 3);
    QCOMPARE(settled.count(), 1);
    QCOMPARE(settled.constFirst(), scheduled.constLast());

    QTest::qWait(350);
    QCOMPARE(service.snapshot().sourcePath, QStringLiteral("3"));
    QCOMPARE(settled.count(), 2);
    QCOMPARE(settled.constLast(), scheduled.constFirst());
}

void FrontendConfigTest::publishesUnchangedSuccessfulReloads()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    int loads = 0;
    FrontendConfigService service(
        QDir(temporary.path()).filePath(QStringLiteral("config")),
        [&loads](const QString &) {
            ++loads;
            return snapshotWithMarker(17);
        },
        0);
    QCOMPARE(loads, 1);

    QSignalSpy changed(&service, &FrontendConfigService::changed);
    QSignalSpy scheduled(&service, &FrontendConfigService::reloadScheduled);
    QSignalSpy settled(&service, &FrontendConfigService::reloadSettled);
    service.reloadNow();
    QCOMPARE(loads, 2);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(scheduled.count(), 1);
    QCOMPARE(settled.count(), 1);
    QCOMPARE(settled.constFirst(), scheduled.constFirst());
    const QVariant published = changed.constFirst().constFirst();
    QCOMPARE(published.metaType(),
             QMetaType::fromType<FrontendConfigSnapshot>());
    const auto *snapshot =
        static_cast<const FrontendConfigSnapshot *>(published.constData());
    QVERIFY(snapshot != nullptr);
    QCOMPARE(*snapshot, service.snapshot());
}

void FrontendConfigTest::retainsLastGoodSnapshotAfterFailure()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    bool fail = false;
    FrontendConfigService service(
        QDir(temporary.path()).filePath(QStringLiteral("config")),
        [&fail](const QString &) -> FrontendConfigLoadResult {
            if (fail) {
                return std::unexpected(
                    QStringLiteral("invalid frontend config"));
            }
            return snapshotWithMarker(17);
        },
        0);
    const FrontendConfigSnapshot lastGood = service.snapshot();

    QSignalSpy changed(&service, &FrontendConfigService::changed);
    QSignalSpy failed(&service, &FrontendConfigService::reloadFailed);
    QSignalSpy settled(&service, &FrontendConfigService::reloadSettled);
    fail = true;
    service.reloadNow();

    QCOMPARE(failed.count(), 1);
    QCOMPARE(settled.count(), 1);
    QCOMPARE(failed.constFirst().constFirst().toString(),
             QStringLiteral("invalid frontend config"));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(service.snapshot(), lastGood);
    QCOMPARE(service.lastError(), QStringLiteral("invalid frontend config"));
}

void FrontendConfigTest::changedSubscriberMayDeleteService()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    std::atomic<int> loads = 0;
    QPointer<FrontendConfigService> service = new FrontendConfigService(
        QDir(temporary.path()).filePath(QStringLiteral("config")),
        [&loads](const QString &) { return snapshotWithMarker(++loads); }, 0);
    QCOMPARE(loads.load(), 1);

    QObject::connect(
        service, &FrontendConfigService::changed, service,
        [&service](const FrontendConfigSnapshot &) { delete service.data(); });
    service->requestReload();
    QTRY_VERIFY_WITH_TIMEOUT(service.isNull(), 2000);
}

void FrontendConfigTest::failedSubscriberMayDeleteService()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    std::atomic<int> loads = 0;
    QPointer<FrontendConfigService> service = new FrontendConfigService(
        QDir(temporary.path()).filePath(QStringLiteral("config")),
        [&loads](const QString &) -> FrontendConfigLoadResult {
            const int load = ++loads;
            if (load == 1) return snapshotWithMarker(load);
            return std::unexpected(QStringLiteral("delete-on-failure"));
        },
        0);
    QCOMPARE(loads.load(), 1);

    QObject::connect(service, &FrontendConfigService::reloadFailed, service,
                     [&service](const QString &) { delete service.data(); });
    service->requestReload();
    QTRY_VERIFY_WITH_TIMEOUT(service.isNull(), 2000);
}

QTEST_GUILESS_MAIN(FrontendConfigTest)

#include "test_frontend_config.moc"
