#include "ghostty_config_process_loader.h"

#include "ghostty_config_export.h"
#include "ghostty_config_export_fixture.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <expected>
#include <limits>
#include <ranges>
#include <utility>

#ifndef GHOSTTY_QT_FAKE_CONFIG_HELPER_PATH
#define GHOSTTY_QT_FAKE_CONFIG_HELPER_PATH ""
#endif

#ifndef GHOSTTY_QT_REAL_CONFIG_HELPER_PATH
#define GHOSTTY_QT_REAL_CONFIG_HELPER_PATH ""
#endif

namespace {

using namespace GhosttyConfigExportFixture;

template<typename Value>
QString errorMessage(const std::expected<Value, QString> &result)
{
    return result ? QString{} : result.error();
}

struct ConfigFixture {
    QTemporaryDir temporary;
    QString xdgHome;
    QString legacyPath;
    QString preferredPath;

    ConfigFixture()
        : temporary(temporaryTemplate())
    {
        if (!temporary.isValid()) {
            qFatal("could not create repository-local config fixture");
        }
        xdgHome = QDir(temporary.path()).filePath(QStringLiteral("xdg"));
        const QString ghosttyDirectory =
            QDir(xdgHome).filePath(QStringLiteral("ghostty"));
        QDir().mkpath(ghosttyDirectory);
        legacyPath = QDir(ghosttyDirectory).filePath(QStringLiteral("config"));
        preferredPath =
            QDir(ghosttyDirectory).filePath(QStringLiteral("config.ghostty"));
        writeFile(legacyPath, QByteArrayLiteral("font-size = 14\n"));
        writeFile(preferredPath, QByteArrayLiteral("font-size = 15\n"));
    }

    [[nodiscard]] QStringList candidates() const
    {
        return {legacyPath, preferredPath};
    }

    [[nodiscard]] QString filePath(const QString &name) const
    {
        return QDir(QFileInfo(preferredPath).absolutePath()).filePath(name);
    }

    [[nodiscard]] static QString temporaryTemplate()
    {
        const QString directory =
            QDir::current().filePath(QStringLiteral("tmp"));
        if (!QDir().mkpath(directory)) {
            qFatal("could not create repository-local tmp directory");
        }
        return QDir(directory).filePath(
            QStringLiteral("config-loader-XXXXXX"));
    }

    static void writeFile(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qFatal("could not create test fixture file");
        }
        if (file.write(contents) != contents.size()) {
            qFatal("could not write test fixture file");
        }
    }
};

QByteArray invocationLog(const QString &path)
{
    QFile log(path);
    if (!log.open(QIODevice::ReadOnly)) return {};
    return log.readAll();
}

QJsonObject withFontSize(QJsonObject exportObject, double size)
{
    QJsonObject configValues =
        exportObject.value(QStringLiteral("values")).toObject();
    configValues.insert(QStringLiteral("font-size"), size);
    exportObject.insert(QStringLiteral("values"), configValues);
    return exportObject;
}

GhosttyConfigProcessLoaderOptions fakeOptions(
    const ConfigFixture &fixture,
    const QString &mode = {},
    const QJsonObject &first = object(),
    const QJsonObject &second = {})
{
    static int nextInvocationLog = 0;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_EXPECT_XDG_CONFIG_HOME"),
                       fixture.xdgHome);
    environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_CONFIG_JSON"),
                       QString::fromUtf8(json(first)));
    environment.insert(
        QStringLiteral("GHOSTTY_QT_FAKE_CONFIG_JSON_SECOND"),
        QString::fromUtf8(json(second.isEmpty() ? first : second)));
    environment.insert(
        QStringLiteral("GHOSTTY_QT_FAKE_INVOCATION_LOG"),
        QDir(fixture.temporary.path())
            .filePath(QStringLiteral("fake-invocations-%1")
                          .arg(++nextInvocationLog)));
    if (!mode.isEmpty()) {
        environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_MODE"), mode);
    }
    return {
        .helperPath = QString::fromUtf8(GHOSTTY_QT_FAKE_CONFIG_HELPER_PATH),
        .timeoutMilliseconds = 2'000,
        .environment = environment,
    };
}

std::expected<GhosttyConfigExport, QString> queryRealConfigExport(
    const QString &helperPath, const ConfigFixture &fixture)
{
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), fixture.xdgHome);
    process.setProcessEnvironment(environment);
    process.setProgram(helperPath);
    process.setArguments({QStringLiteral("+show-config-json")});
    process.start(QIODevice::ReadOnly);
    if (!process.waitForStarted(10'000)) {
        return std::unexpected(QStringLiteral("structured helper did not start"));
    }
    if (!process.waitForFinished(10'000)) {
        process.kill();
        process.waitForFinished(1'000);
        return std::unexpected(QStringLiteral("structured helper timed out"));
    }
    if (process.exitStatus() != QProcess::NormalExit
        || process.exitCode() != 0) {
        return std::unexpected(
            QStringLiteral("structured helper failed: %1")
                .arg(QString::fromUtf8(process.readAllStandardError()).trimmed()));
    }
    return parseGhosttyConfigExportJson(process.readAllStandardOutput());
}

GhosttyConfigProcessLoaderOptions realOptions(const QString &helperPath)
{
    return {
        .helperPath = helperPath,
        .timeoutMilliseconds = 10'000,
        .environment = QProcessEnvironment::systemEnvironment(),
    };
}

} // namespace

class GhosttyConfigProcessLoaderTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void derivesXdgHomeFromEitherCandidateOrder();
    void invokesStableFourProcessTransaction();
    void publishesTypedSnapshotAndSourcePaths();
    void diagnosesOnlyNonDefaultUnsupportedActions();
    void rejectsQueryFailuresAndMalformedData();
    void rejectsConfigThatBecomesInvalidDuringQueries();
    void rejectsConfigThatChangesValidlyDuringQueries();
    void preservesSuccessfulHelperWarnings();
    void reportsValidationFailureDeterministically();
    void reportsTimeoutCrashAndStartFailureDeterministically();
    void realHelperFinalizesSurfaceValues();
    void realHelperFinalizesAppearanceAndUnbinds();
    void realHelperExportsApplicationLifetime();
    void realHelperExportsConfigFileSources();
    void realHelperExportsFinalizedStructuredKeybindings();
    void realHelperCanonicalizesTerminalControlActionPayloads();
};

void GhosttyConfigProcessLoaderTest::derivesXdgHomeFromEitherCandidateOrder()
{
    ConfigFixture fixture;
    auto result = ghosttyConfigXdgHome(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(*result, fixture.xdgHome);

    result = ghosttyConfigXdgHome(
        {fixture.preferredPath, fixture.legacyPath});
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(*result, fixture.xdgHome);

    result = ghosttyConfigXdgHome({fixture.preferredPath});
    QVERIFY(!result);
    QCOMPARE(result.error(), QStringLiteral(
        "Ghostty config candidates must contain both config and config.ghostty"));

    const QString wrongDirectory =
        QDir(fixture.temporary.path()).filePath(QStringLiteral("config"));
    ConfigFixture::writeFile(wrongDirectory, {});
    result = ghosttyConfigXdgHome({wrongDirectory, fixture.preferredPath});
    QVERIFY(!result);
    QCOMPARE(result.error(), QStringLiteral(
        "Ghostty config candidates must share one XDG ghostty directory"));
}

void GhosttyConfigProcessLoaderTest::invokesStableFourProcessTransaction()
{
    ConfigFixture fixture;
    const QString logPath =
        QDir(fixture.temporary.path()).filePath(QStringLiteral("invocations"));
    auto options = fakeOptions(fixture);
    options.environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_INVOCATION_LOG"),
                               logPath);

    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(invocationLog(logPath),
             QByteArrayLiteral("+validate-config\n"
                               "+show-config-json\n"
                               "+validate-config\n"
                               "+show-config-json\n"));
    QCOMPARE(result->values.fontSize, 13.5);
    QCOMPARE(result->keybindings.root.size(), 1);
}

void GhosttyConfigProcessLoaderTest::publishesTypedSnapshotAndSourcePaths()
{
    ConfigFixture fixture;
    const QString included = fixture.filePath(QStringLiteral("included.ghostty"));
    const QString missing = fixture.filePath(QStringLiteral("missing.ghostty"));
    ConfigFixture::writeFile(included, QByteArrayLiteral("font-size = 18\n"));

    QJsonObject exportObject = object();
    QJsonObject configValues =
        exportObject.value(QStringLiteral("values")).toObject();
    configValues.insert(
        QStringLiteral("config-file"),
        QJsonArray{included, QStringLiteral("?") + missing, included});
    exportObject.insert(QStringLiteral("values"), configValues);

    const GhosttyConfigLoadResult result = makeGhosttyConfigProcessLoader(
        fakeOptions(fixture, {}, exportObject))(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->sourcePaths,
             QStringList({fixture.legacyPath, fixture.preferredPath, included}));
    QCOMPARE(result->values.configFiles.size(), 3);
    QCOMPARE(result->values.configFiles.at(0).path, included);
    QVERIFY(!result->values.configFiles.at(0).optional);
    QCOMPARE(result->values.configFiles.at(1).path, missing);
    QVERIFY(result->values.configFiles.at(1).optional);
    QCOMPARE(result->values.configFiles.at(2).path, included);
    QVERIFY(!result->values.configFiles.at(2).optional);
    QCOMPARE(result->values.scrollbackLimitBytes,
             std::numeric_limits<quint64>::max());
    QVERIFY(!result->keybindings.root.isEmpty());
}

void GhosttyConfigProcessLoaderTest::diagnosesOnlyNonDefaultUnsupportedActions()
{
    ConfigFixture fixture;
    GhosttyConfigLoadResult result = makeGhosttyConfigProcessLoader(
        fakeOptions(fixture))(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->diagnostics.isEmpty());

    QJsonObject configured = object();
    QJsonObject current =
        configured.value(QStringLiteral("keybindings")).toObject();
    QJsonArray root = current.value(QStringLiteral("root")).toArray();
    root.append(binding({unicodeTrigger('x', GhosttyKeybindCtrl)},
                        {QStringLiteral("toggle_command_palette")}));
    root.append(binding({unicodeTrigger('y', GhosttyKeybindCtrl)},
                        {QStringLiteral("toggle_command_palette")}));
    current.insert(QStringLiteral("root"), root);
    configured.insert(QStringLiteral("keybindings"), current);

    result = makeGhosttyConfigProcessLoader(
        fakeOptions(fixture, {}, configured))(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    const auto warnings = std::ranges::count_if(
        result->diagnostics, [](const GhosttyConfigDiagnostic &diagnostic) {
            return diagnostic.message.contains(
                QStringLiteral("toggle_command_palette"));
        });
    QCOMPARE(warnings, 1);

    // A flag-only change at the default location is also a user-visible
    // semantic change and should expose the unsupported current action.
    configured = object();
    current = configured.value(QStringLiteral("keybindings")).toObject();
    root = current.value(QStringLiteral("root")).toArray();
    QJsonObject changed = root.at(0).toObject();
    changed.insert(QStringLiteral("flags"), flags(false));
    root.replace(0, changed);
    current.insert(QStringLiteral("root"), root);
    configured.insert(QStringLiteral("keybindings"), current);
    result = makeGhosttyConfigProcessLoader(
        fakeOptions(fixture, {}, configured))(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(std::ranges::count_if(
                 result->diagnostics,
                 [](const GhosttyConfigDiagnostic &diagnostic) {
                     return diagnostic.message.contains(
                         QStringLiteral("toggle_command_palette"));
                 }),
             1);
}

void GhosttyConfigProcessLoaderTest::rejectsQueryFailuresAndMalformedData()
{
    ConfigFixture fixture;
    GhosttyConfigLoadResult result = makeGhosttyConfigProcessLoader(
        fakeOptions(fixture, QStringLiteral("config-query-failure")))(
        fixture.candidates());
    QVERIFY(!result);
    QCOMPARE(result.error(), QStringLiteral(
        "Ghostty config helper failed during config query with exit code 8: "
        "stderr: config query failed"));

    result = makeGhosttyConfigProcessLoader(
        fakeOptions(fixture, QStringLiteral("config-query-malformed")))(
        fixture.candidates());
    QVERIFY(!result);
    QVERIFY(result.error().startsWith(QStringLiteral(
        "Ghostty config query returned malformed data: Invalid Ghostty "
        "structured config JSON")));

    result = makeGhosttyConfigProcessLoader(
        fakeOptions(fixture,
                    QStringLiteral("config-consistency-query-failure")))(
        fixture.candidates());
    QVERIFY(!result);
    QCOMPARE(result.error(), QStringLiteral(
        "Ghostty config helper failed during config consistency query with "
        "exit code 9: stderr: config consistency query failed"));
}

void GhosttyConfigProcessLoaderTest::rejectsConfigThatBecomesInvalidDuringQueries()
{
    ConfigFixture fixture;
    const QString logPath =
        QDir(fixture.temporary.path()).filePath(QStringLiteral("invocations"));
    auto options = fakeOptions(fixture,
                               QStringLiteral("post-validation-failure"));
    options.environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_INVOCATION_LOG"),
                               logPath);
    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.candidates());
    QVERIFY(!result);
    QCOMPARE(result.error(), QStringLiteral(
        "Ghostty config helper failed during post-query validation with exit "
        "code 1: stdout: config changed during query"));
    QCOMPARE(invocationLog(logPath),
             QByteArrayLiteral("+validate-config\n"
                               "+show-config-json\n"
                               "+validate-config\n"));
}

void GhosttyConfigProcessLoaderTest::rejectsConfigThatChangesValidlyDuringQueries()
{
    ConfigFixture fixture;
    GhosttyConfigLoadResult result = makeGhosttyConfigProcessLoader(
        fakeOptions(fixture, QStringLiteral("config-consistency-mismatch"),
                    object(), withFontSize(object(), 18.0)))(
        fixture.candidates());
    QVERIFY(!result);
    QCOMPARE(result.error(), QStringLiteral(
        "Ghostty config changed while it was being queried; reload will retry"));

    auto formattingOnly = fakeOptions(
        fixture, QStringLiteral("config-consistency-mismatch"));
    formattingOnly.environment.insert(
        QStringLiteral("GHOSTTY_QT_FAKE_CONFIG_JSON_SECOND"),
        QStringLiteral(" ") + QString::fromUtf8(json()));
    result = makeGhosttyConfigProcessLoader(std::move(formattingOnly))(
        fixture.candidates());
    QVERIFY(!result);
    QCOMPARE(result.error(), QStringLiteral(
        "Ghostty config changed while it was being queried; reload will retry"));
}

void GhosttyConfigProcessLoaderTest::preservesSuccessfulHelperWarnings()
{
    ConfigFixture fixture;
    auto options = fakeOptions(fixture);
    options.environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_SUCCESS_WARNING"),
                               QStringLiteral("both standard files exist\n"
                                              "both standard files exist\n"));
    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->diagnostics.size(), 1);
    QCOMPARE(result->diagnostics.constFirst().message,
             QStringLiteral("Ghostty config helper config query: "
                            "both standard files exist"));
}

void GhosttyConfigProcessLoaderTest::reportsValidationFailureDeterministically()
{
    ConfigFixture fixture;
    const GhosttyConfigLoadResult result = makeGhosttyConfigProcessLoader(
        fakeOptions(fixture, QStringLiteral("validation-failure")))(
        fixture.candidates());
    QVERIFY(!result);
    QCOMPARE(result.error(), QStringLiteral(
        "Ghostty config helper failed during validation with exit code 1: "
        "stdout: config.ghostty:2:1: invalid value"));
}

void GhosttyConfigProcessLoaderTest::reportsTimeoutCrashAndStartFailureDeterministically()
{
    ConfigFixture fixture;
    auto timeout = fakeOptions(fixture, QStringLiteral("validation-timeout"));
    timeout.timeoutMilliseconds = 25;
    GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(timeout)(fixture.candidates());
    QVERIFY(!result);
    QCOMPARE(result.error(), QStringLiteral(
        "Ghostty config helper timed out during validation after 25 ms"));

    auto overall = fakeOptions(fixture, QStringLiteral("validation-timeout"));
    overall.timeoutMilliseconds = 2'000;
    overall.overallTimeoutMilliseconds = 30;
    result = makeGhosttyConfigProcessLoader(overall)(fixture.candidates());
    QVERIFY(!result);
    QVERIFY(result.error().contains(QStringLiteral(
        "timed out during validation after 30 ms")));

    const auto crash = fakeOptions(fixture, QStringLiteral("validation-crash"));
    result = makeGhosttyConfigProcessLoader(crash)(fixture.candidates());
    QVERIFY(!result);
    QCOMPARE(result.error(),
             QStringLiteral("Ghostty config helper crashed during validation"));

    auto missing = fakeOptions(fixture);
    missing.helperPath = QDir(fixture.temporary.path())
                             .filePath(QStringLiteral("does-not-exist"));
    missing.timeoutMilliseconds = 100;
    result = makeGhosttyConfigProcessLoader(missing)(fixture.candidates());
    QVERIFY(!result);
    QCOMPARE(result.error(), QStringLiteral(
        "Ghostty config helper could not be started during validation"));
}

void GhosttyConfigProcessLoaderTest::realHelperFinalizesSurfaceValues()
{
    const QString helperPath = QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) QSKIP("The pinned Ghostty config helper is disabled");

    ConfigFixture fixture;
    const QString directory = QDir(fixture.temporary.path())
                                  .filePath(QStringLiteral("working directory"));
    QVERIFY(QDir().mkpath(directory));
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QStringLiteral("working-directory = %1\n"
                       "split-inherit-working-directory = false\n"
                       "split-preserve-zoom = navigation\n"
                       "tab-inherit-working-directory = false\n"
                       "window-inherit-working-directory = false\n"
                       "window-inherit-font-size = false\n"
                       "window-new-tab-position = end\n"
                       "window-show-tab-bar = never\n"
                       "window-width = 1\n"
                       "window-height = 1\n"
                       "maximize = true\n"
                       "fullscreen = non-native-visible-menu\n"
                       "scrollback-limit = 18446744073709551615\n")
            .arg(directory)
            .toUtf8());

    auto options = realOptions(helperPath);
    options.environment.remove(QStringLiteral("TERM_PROGRAM"));
    const auto load = makeGhosttyConfigProcessLoader(std::move(options));
    GhosttyConfigLoadResult result = load(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.workingDirectoryPath.has_value());
    QCOMPARE(*result->values.workingDirectoryPath, directory);
    QVERIFY(!result->values.splitInheritWorkingDirectory);
    QVERIFY(result->values.splitPreserveZoom);
    QVERIFY(!result->values.tabInheritWorkingDirectory);
    QVERIFY(!result->values.windowInheritWorkingDirectory);
    QVERIFY(!result->values.windowInheritFontSize);
    QCOMPARE(result->values.windowNewTabPosition, WindowNewTabPosition::End);
    QCOMPARE(result->values.windowShowTabBar, WindowShowTabBar::Never);
    QCOMPARE(result->values.windowWidth, quint32(10));
    QCOMPARE(result->values.windowHeight, quint32(4));
    QVERIFY(result->values.maximize);
    QCOMPARE(result->values.fullscreen,
             GhosttyFullscreenMode::NonNativeVisibleMenu);
    QCOMPARE(result->values.scrollbackLimitBytes,
             std::numeric_limits<quint64>::max());

    ConfigFixture::writeFile(fixture.preferredPath, {});
    result = load(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(!result->values.workingDirectoryPath.has_value());

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("working-directory = home\n"));
    result = load(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.workingDirectoryPath.has_value());
    QCOMPARE(*result->values.workingDirectoryPath, QDir::homePath());

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("working-directory = ~/ghostty-qt-test\n"));
    result = load(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.workingDirectoryPath.has_value());
    QCOMPARE(*result->values.workingDirectoryPath,
             QDir(QDir::homePath())
                 .filePath(QStringLiteral("ghostty-qt-test")));
}

void GhosttyConfigProcessLoaderTest::realHelperFinalizesAppearanceAndUnbinds()
{
    const QString helperPath = QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) QSKIP("The pinned Ghostty config helper is disabled");

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral(
            "unfocused-split-opacity = -1\n"
            "unfocused-split-fill = AliceBlue\n"
            "palette = 42=#123456\n"
            "split-divider-color = AliceBlue\n"
            "selection-foreground = cell-background\n"
            "selection-background = #334455\n"
            "search-foreground = cell-background\n"
            "search-background = #123456\n"
            "search-selected-foreground = cell-foreground\n"
            "search-selected-background = #654321\n"
            "cursor-color = #abcdef\n"
            "cursor-opacity = 0.4\n"
            "cursor-style = block_hollow\n"
            "cursor-style-blink = false\n"
            "cursor-text = cell-foreground\n"
            "bold-color = bright\n"
            "faint-opacity = 0.25\n"
            "clipboard-trim-trailing-spaces = 0\n"
            "clipboard-paste-protection = 0\n"
            "clipboard-paste-bracketed-safe = t\n"
            "copy-on-select = clipboard\n"
            "selection-clear-on-typing = 0\n"
            "selection-clear-on-copy = t\n"
            "middle-click-action = ignore\n"
            "keybind = clear\n"
            "keybind = ctrl+a=ignore\n"
            "keybind = ctrl+a=unbind\n"
            "keybind = ctrl+b=new_tab\n"));

    const GhosttyConfigLoadResult result = makeGhosttyConfigProcessLoader(
        realOptions(helperPath))(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->keybindings.root.size(), 1);
    QCOMPARE(result->keybindings.root.constFirst().actions,
             QStringList({QStringLiteral("new_tab")}));
    QCOMPARE(result->values.splitAppearance.unfocusedOpacity, 0.15);
    QCOMPARE(result->values.splitAppearance.unfocusedFill,
             std::optional<QColor>(QColor(QStringLiteral("#f0f8ff"))));
    QCOMPARE(result->values.appearance.palette.size(), std::size_t{256});
    QCOMPARE(result->values.appearance.palette.at(42),
             QColor(QStringLiteral("#123456")));
    QCOMPARE(result->values.splitAppearance.dividerColor,
             std::optional<QColor>(QColor(QStringLiteral("#f0f8ff"))));

    QVERIFY(result->values.appearance.selectionForeground.has_value());
    QVERIFY(std::holds_alternative<GhosttyCellRelativeColor>(
        *result->values.appearance.selectionForeground));
    QCOMPARE(std::get<GhosttyCellRelativeColor>(
                 *result->values.appearance.selectionForeground),
             GhosttyCellRelativeColor::Background);
    QVERIFY(result->values.appearance.selectionBackground.has_value());
    QVERIFY(std::holds_alternative<QColor>(
        *result->values.appearance.selectionBackground));
    QCOMPARE(std::get<QColor>(*result->values.appearance.selectionBackground),
             QColor(QStringLiteral("#334455")));
    QVERIFY(std::holds_alternative<GhosttyCellRelativeColor>(
        result->values.appearance.searchForeground));
    QCOMPARE(std::get<GhosttyCellRelativeColor>(
                 result->values.appearance.searchForeground),
             GhosttyCellRelativeColor::Background);
    QVERIFY(result->values.appearance.cursorColor.has_value());
    QVERIFY(std::holds_alternative<QColor>(
        *result->values.appearance.cursorColor));
    QCOMPARE(std::get<QColor>(*result->values.appearance.cursorColor),
             QColor(QStringLiteral("#abcdef")));
    QCOMPARE(result->values.appearance.cursorOpacity, 0.4);
    QCOMPARE(result->values.appearance.cursorStyle,
             TerminalCursorStyle::BlockHollow);
    QCOMPARE(result->values.appearance.cursorBlink, std::optional<bool>(false));
    QVERIFY(result->values.appearance.boldColor.has_value());
    QVERIFY(std::holds_alternative<GhosttyBoldBrightness>(
        *result->values.appearance.boldColor));
    QCOMPARE(
        std::get<GhosttyBoldBrightness>(*result->values.appearance.boldColor),
        GhosttyBoldBrightness::Bright);
    QCOMPARE(result->values.appearance.faintOpacity, 0.25);
    QVERIFY(!result->values.selectionClipboard.trimTrailingSpaces);
    QVERIFY(!result->values.clipboardPaste.protection);
    QVERIFY(result->values.clipboardPaste.bracketedSafe);
    QCOMPARE(result->values.selectionClipboard.copyOnSelect,
             TerminalCopyOnSelectMode::PrimaryAndClipboard);
    QVERIFY(!result->values.selectionClipboard.clearOnTyping);
    QVERIFY(result->values.selectionClipboard.clearOnCopy);
    QCOMPARE(result->values.middleClickAction, MiddleClickAction::Ignore);
}

void GhosttyConfigProcessLoaderTest::realHelperExportsApplicationLifetime()
{
    const QString helperPath = QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) QSKIP("The pinned Ghostty config helper is disabled");

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral(
            "gtk-single-instance = false\n"
            "initial-window = false\n"
            "resize-overlay = always\n"
            "resize-overlay-position = bottom-right\n"
            "resize-overlay-duration = 1s 250ms 999us\n"
            "quit-after-last-window-closed = false\n"
            "quit-after-last-window-closed-delay = 1s 250ms 999us\n"));
    auto result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(!result->values.quitAfterLastWindowClosed);
    QVERIFY(result->values.quitAfterLastWindowClosedDelay.has_value());
    QCOMPARE(result->values.quitAfterLastWindowClosedDelay->count(),
             std::chrono::milliseconds::rep{1'250});
    QVERIFY(!result->values.initialWindow);
    QCOMPARE(result->values.resizeOverlay.mode, ResizeOverlayMode::Always);
    QCOMPARE(result->values.resizeOverlay.position,
             ResizeOverlayPosition::BottomRight);
    QCOMPARE(result->values.resizeOverlay.duration.count(),
             std::chrono::milliseconds::rep{1'250});
    QCOMPARE(result->values.singleInstanceMode, SingleInstanceMode::Disabled);

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral(
            "quit-after-last-window-closed-delay = "
            "584y 49w 23h 34m 33s 709ms 551us 615ns\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.quitAfterLastWindowClosedDelay.has_value());
    QCOMPARE(result->values.quitAfterLastWindowClosedDelay->count(),
             std::chrono::milliseconds::rep{
                 std::numeric_limits<quint32>::max()});

    ConfigFixture::writeFile(fixture.preferredPath, {});
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(!result->values.quitAfterLastWindowClosedDelay.has_value());
    QVERIFY(!result->defaultKeybindings.root.isEmpty());
}

void GhosttyConfigProcessLoaderTest::realHelperExportsConfigFileSources()
{
    const QString helperPath = QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) QSKIP("The pinned Ghostty config helper is disabled");

    ConfigFixture fixture;
    const QString included = fixture.filePath(QStringLiteral("included.ghostty"));
    const QString missing = fixture.filePath(QStringLiteral("missing.ghostty"));
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(included, QByteArrayLiteral("font-size = 19\n"));
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QStringLiteral("config-file = %1\nconfig-file = ?%2\n")
            .arg(included, missing)
            .toUtf8());

    const GhosttyConfigLoadResult result = makeGhosttyConfigProcessLoader(
        realOptions(helperPath))(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.fontSize, 19.0);
    QCOMPARE(result->values.configFiles.size(), 2);
    QCOMPARE(result->values.configFiles.at(0).path, included);
    QVERIFY(!result->values.configFiles.at(0).optional);
    QCOMPARE(result->values.configFiles.at(1).path, missing);
    QVERIFY(result->values.configFiles.at(1).optional);
    QCOMPARE(result->sourcePaths,
             QStringList({fixture.legacyPath, fixture.preferredPath, included}));
}

void GhosttyConfigProcessLoaderTest::realHelperExportsFinalizedStructuredKeybindings()
{
    const QString helperPath = QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) QSKIP("The pinned Ghostty config helper is disabled");

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QStringLiteral(
            "keybind = clear\n"
            "keybind = unconsumed:performable:ctrl+x>key_y=new_tab\n"
            "keybind = chain=goto_split:left\n"
            "keybind = catch_all=ignore\n"
            "keybind = all:ctrl+g=new_tab\n"
            "keybind = global:ctrl+j=new_tab\n"
            "keybind = ctrl+m=activate_key_table:modeé\n"
            "keybind = ctrl+s=set_surface_title:🌐 surface:detail\n"
            "keybind = ctrl+t=set_tab_title:👻 main:detail\n"
            "keybind = ctrl+v=close_tab:other\n"
            "keybind = ctrl+w=close_tab:right\n"
            "keybind = resize/ctrl+h=resize_split:left,10\n"
            "keybind = modeé/ctrl+h=resize_split:right,10\n").toUtf8());

    const GhosttyConfigLoadResult result = makeGhosttyConfigProcessLoader(
        realOptions(helperPath))(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    const GhosttyKeybindConfig &config = result->keybindings;
    QCOMPARE(config.root.size(), 9);
    QCOMPARE(config.tables.size(), 2);
    const auto chained = std::ranges::find_if(
        config.root, [](const GhosttyKeybindDefinition &definition) {
            return definition.sequence.size() == 2;
        });
    QVERIFY(chained != config.root.cend());
    QCOMPARE(chained->sequence.at(0).unicodeCodepoint, quint32('x'));
    QCOMPARE(chained->sequence.at(1).physicalName, QStringLiteral("key_y"));
    QCOMPARE(chained->actions,
             QStringList({QStringLiteral("new_tab"),
                          QStringLiteral("goto_split:left")}));
    QVERIFY(!chained->flags.consumed);
    QVERIFY(chained->flags.performable);
    QVERIFY(std::ranges::any_of(config.root, [](const auto &definition) {
        return definition.flags.all;
    }));
    QVERIFY(std::ranges::any_of(config.root, [](const auto &definition) {
        return definition.flags.global;
    }));
    const auto resize = std::ranges::find(
        config.tables, QStringLiteral("resize"), &GhosttyKeybindTable::name);
    QVERIFY(resize != config.tables.cend());
    QCOMPARE(resize->bindings.constFirst().actions,
             QStringList({QStringLiteral("resize_split:left,10")}));
    const auto unicode = std::ranges::find(
        config.tables, QStringLiteral("modeé"), &GhosttyKeybindTable::name);
    QVERIFY(unicode != config.tables.cend());
    QCOMPARE(unicode->bindings.constFirst().actions,
             QStringList({QStringLiteral("resize_split:right,10")}));
    QVERIFY(std::ranges::none_of(
        result->diagnostics, [](const GhosttyConfigDiagnostic &diagnostic) {
            return diagnostic.message.contains(QStringLiteral("close_tab:other"))
                || diagnostic.message.contains(QStringLiteral("close_tab:right"));
        }));
}

void GhosttyConfigProcessLoaderTest::realHelperCanonicalizesTerminalControlActionPayloads()
{
    const QString helperPath = QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) QSKIP("The pinned Ghostty config helper is disabled");

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    QByteArray config = QByteArrayLiteral(
        "keybind = clear\n"
        "keybind = ctrl+a=text:\\x15\n"
        "keybind = ctrl+b=text:");
    config.append(QByteArray::fromHex("f09f91bb"));
    config.append(QByteArrayLiteral("\nkeybind = ctrl+c=csi:"));
    config.append(QByteArray::fromHex("c3a9"));
    config.append(QByteArrayLiteral(
        "\nkeybind = ctrl+d=esc:\\x7f\n"
        "keybind = ctrl+e=text:\\q\n"));
    ConfigFixture::writeFile(fixture.preferredPath, config);

    const GhosttyConfigLoadResult result = makeGhosttyConfigProcessLoader(
        realOptions(helperPath))(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    const GhosttyKeybindConfig &keybinds = result->keybindings;
    QCOMPARE(keybinds.root.size(), 5);
    const auto actionFor = [&keybinds](quint32 codepoint) -> QStringList {
        const auto found = std::ranges::find_if(
            keybinds.root,
            [codepoint](const GhosttyKeybindDefinition &definition) {
                return definition.sequence.size() == 1
                    && definition.sequence.constFirst().kind
                           == GhosttyKeybindKeyKind::Unicode
                    && definition.sequence.constFirst().unicodeCodepoint
                           == codepoint;
            });
        return found == keybinds.root.cend() ? QStringList{} : found->actions;
    };
    QCOMPARE(actionFor('a'),
             QStringList({QStringLiteral(R"(text:\\x15)")}));
    QCOMPARE(actionFor('b'),
             QStringList({QStringLiteral(R"(text:\xf0\x9f\x91\xbb)")}));
    QCOMPARE(actionFor('c'),
             QStringList({QStringLiteral(R"(csi:\xc3\xa9)")}));
    QCOMPARE(actionFor('d'),
             QStringList({QStringLiteral(R"(esc:\\x7f)")}));
    QCOMPARE(actionFor('e'),
             QStringList({QStringLiteral(R"(text:\\q)")}));
}

QTEST_GUILESS_MAIN(GhosttyConfigProcessLoaderTest)

#include "test_ghostty_config_process_loader.moc"
