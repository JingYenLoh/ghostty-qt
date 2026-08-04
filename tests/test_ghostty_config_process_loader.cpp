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
#include <array>
#include <expected>
#include <limits>
#include <pwd.h>
#include <ranges>
#include <unistd.h>
#include <utility>

#ifndef GHOSTTY_QT_FAKE_CONFIG_HELPER_PATH
#define GHOSTTY_QT_FAKE_CONFIG_HELPER_PATH ""
#endif

#ifndef GHOSTTY_QT_REAL_CONFIG_HELPER_PATH
#define GHOSTTY_QT_REAL_CONFIG_HELPER_PATH ""
#endif

namespace {

using namespace GhosttyConfigExportFixture;

template <typename Value>
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

    [[nodiscard]] GhosttyConfigLoadRequest
    request(TerminalColorScheme colorScheme = TerminalColorScheme::Light) const
    {
        return {
            .candidatePaths = candidates(),
            .colorScheme = colorScheme,
        };
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
        return QDir(directory).filePath(QStringLiteral("config-loader-XXXXXX"));
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

struct HelperResult {
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    int exitCode = -1;
    QByteArray standardOutput;
    QByteArray standardError;

    bool operator==(const HelperResult &) const = default;
};

std::expected<HelperResult, QString> runRealHelper(const QString &helperPath,
                                                   const ConfigFixture &fixture,
                                                   const QStringList &arguments)
{
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), fixture.xdgHome);
    process.setProcessEnvironment(environment);
    process.setProgram(helperPath);
    process.setArguments(arguments);
    process.start(QIODevice::ReadOnly);
    if (!process.waitForStarted(10'000)) {
        return std::unexpected(
            QStringLiteral("structured helper did not start"));
    }
    if (!process.waitForFinished(10'000)) {
        process.kill();
        process.waitForFinished(1'000);
        return std::unexpected(QStringLiteral("structured helper timed out"));
    }
    return HelperResult{
        .exitStatus = process.exitStatus(),
        .exitCode = process.exitCode(),
        .standardOutput = process.readAllStandardOutput(),
        .standardError = process.readAllStandardError(),
    };
}

QJsonObject withFontSize(QJsonObject exportObject, double size)
{
    QJsonObject configValues =
        exportObject.value(QStringLiteral("values")).toObject();
    configValues.insert(QStringLiteral("font-size"), size);
    exportObject.insert(QStringLiteral("values"), configValues);
    return exportObject;
}

GhosttyConfigProcessLoaderOptions
fakeOptions(const ConfigFixture &fixture, const QString &mode = {},
            const QJsonObject &first = object(), const QJsonObject &second = {})
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
    environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_INVOCATION_LOG"),
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
    const QString &helperPath, const ConfigFixture &fixture,
    QStringList configurationArguments = {},
    TerminalColorScheme colorScheme = TerminalColorScheme::Light)
{
    configurationArguments.prepend(
        colorScheme == TerminalColorScheme::Light
            ? QStringLiteral("--ghostty-qt-color-scheme=light")
            : QStringLiteral("--ghostty-qt-color-scheme=dark"));
    configurationArguments.prepend(QStringLiteral("+show-config-json"));
    auto process = runRealHelper(helperPath, fixture, configurationArguments);
    if (!process) return std::unexpected(std::move(process.error()));
    if (process->exitStatus != QProcess::NormalExit || process->exitCode != 0) {
        return std::unexpected(
            QStringLiteral("structured helper failed: %1")
                .arg(QString::fromUtf8(process->standardError).trimmed()));
    }
    return parseGhosttyConfigExportJson(process->standardOutput);
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
    void invokesStableTwoQueryTransaction();
    void forwardsSelectedColorSchemeToEveryConfigQuery();
    void forwardsConfigurationArgumentsToEveryConfigQuery();
    void publishesTypedSnapshotAndSourcePaths();
    void diagnosesOnlyNonDefaultUnsupportedActions();
    void rejectsQueryFailuresAndMalformedData();
    void rejectsConfigThatChangesValidlyDuringQueries();
    void preservesSuccessfulHelperWarnings();
    void reportsTimeoutCrashAndStartFailureDeterministically();
    void realHelperAppliesConfigurationArgumentPrecedence();
    void realHelperDisablesDefaultConfigFiles();
    void realHelperExportsTypographyShaping();
    void realHelperRejectsInvalidConfigurationArgumentsDeterministically();
    void realHelperAppliesSelectedConditionalThemeAndWindowAppearance();
    void realHelperRejectsDiagnosticsFromSelectedConditionalTheme();
    void realHelperFinalizesSurfaceValues();
    void realHelperPreservesOriginalLaunchClassification();
    void realHelperFinalizesAppearanceAndUnbinds();
    void realHelperExportsBackdropConfiguration();
    void realHelperExportsCustomShaderConfiguration();
    void realHelperExportsBackgroundBlur_data();
    void realHelperExportsBackgroundBlur();
    void realHelperGeneratesEffectivePalette();
    void realHelperExportsApplicationLifetime();
    void realHelperExportsLinuxCgroup();
    void realHelperExportsShellIntegration();
    void realHelperExportsEnvironment();
    void realHelperExportsCommands();
    void realHelperExportsAbnormalCommandExitRuntime();
    void realHelperExportsScrollbackCompression();
    void realHelperExportsBellFeatures();
    void realHelperExportsMouseHideWhileTyping();
    void realHelperExportsFocusFollowsMouse();
    void realHelperExportsVtKamAllowed();
    void realHelperExportsSelectionWordChars();
    void realHelperExportsClickRepeatInterval();
    void realHelperExportsClipboardWrite();
    void realHelperExportsEnquiryResponse();
    void realHelperExportsScrollToBottom();
    void realHelperExportsRightClickAction();
    void realHelperExportsMouseShiftCapture();
    void realHelperFinalizesMouseScrollMultiplier();
    void realHelperExportsConfigFileSources();
    void realHelperExportsFinalizedStructuredKeybindings();
    void realHelperCanonicalizesTerminalControlActionPayloads();
};

void GhosttyConfigProcessLoaderTest::derivesXdgHomeFromEitherCandidateOrder()
{
    ConfigFixture fixture;
    auto result = ghosttyConfigXdgHome(fixture.request().candidatePaths);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(*result, fixture.xdgHome);

    result = ghosttyConfigXdgHome({fixture.preferredPath, fixture.legacyPath});
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(*result, fixture.xdgHome);

    result = ghosttyConfigXdgHome({fixture.preferredPath});
    QVERIFY(!result);
    QCOMPARE(
        result.error(),
        QStringLiteral(
            "Ghostty config candidates must contain both config and config.ghostty"));

    const QString wrongDirectory =
        QDir(fixture.temporary.path()).filePath(QStringLiteral("config"));
    ConfigFixture::writeFile(wrongDirectory, {});
    result = ghosttyConfigXdgHome({wrongDirectory, fixture.preferredPath});
    QVERIFY(!result);
    QCOMPARE(
        result.error(),
        QStringLiteral(
            "Ghostty config candidates must share one XDG ghostty directory"));
}

void GhosttyConfigProcessLoaderTest::invokesStableTwoQueryTransaction()
{
    ConfigFixture fixture;
    const QString logPath =
        QDir(fixture.temporary.path()).filePath(QStringLiteral("invocations"));
    auto options = fakeOptions(fixture);
    options.environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_INVOCATION_LOG"),
                               logPath);

    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.request());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(
        invocationLog(logPath),
        QByteArrayLiteral(
            "+show-config-json --ghostty-qt-color-scheme=light --ghostty-qt-probable-cli=true\n"
            "+show-config-json --ghostty-qt-color-scheme=light --ghostty-qt-probable-cli=true\n"));
    QCOMPARE(result->values.typography.pointSize, 13.5);
    QCOMPARE(result->keybindings.root.size(), 1);
}

void GhosttyConfigProcessLoaderTest::
    forwardsSelectedColorSchemeToEveryConfigQuery()
{
    ConfigFixture fixture;
    const QString logPath =
        QDir(fixture.temporary.path()).filePath(QStringLiteral("invocations"));
    auto options = fakeOptions(fixture);
    options.environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_INVOCATION_LOG"),
                               logPath);

    const GhosttyConfigLoadResult result = makeGhosttyConfigProcessLoader(
        options)(fixture.request(TerminalColorScheme::Dark));
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(
        invocationLog(logPath),
        QByteArrayLiteral(
            "+show-config-json --ghostty-qt-color-scheme=dark --ghostty-qt-probable-cli=true\n"
            "+show-config-json --ghostty-qt-color-scheme=dark --ghostty-qt-probable-cli=true\n"));
}

void GhosttyConfigProcessLoaderTest::
    forwardsConfigurationArgumentsToEveryConfigQuery()
{
    ConfigFixture fixture;
    const QString logPath =
        QDir(fixture.temporary.path()).filePath(QStringLiteral("invocations"));
    auto options = fakeOptions(fixture);
    options.environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_INVOCATION_LOG"),
                               logPath);
    options.configurationArguments = {
        QStringLiteral("--font-family="),
        QStringLiteral("--font-family=等号=👻"),
        QStringLiteral("--font-size=17.25"),
        QStringLiteral("--class=com.example.家👻"),
        QStringLiteral("--config-default-files=false"),
    };

    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.request());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));

    const QByteArray suffix =
        QByteArrayLiteral(" --font-family= --font-family=")
        + QStringLiteral("等号=👻").toUtf8()
        + QByteArrayLiteral(" --font-size=17.25 --class=com.example.")
        + QStringLiteral("家👻").toUtf8()
        + QByteArrayLiteral(" --config-default-files=false\n");
    QCOMPARE(
        invocationLog(logPath),
        QByteArrayLiteral(
            "+show-config-json --ghostty-qt-color-scheme=light --ghostty-qt-probable-cli=true")
            + suffix
            + QByteArrayLiteral(
                "+show-config-json --ghostty-qt-color-scheme=light --ghostty-qt-probable-cli=true")
            + suffix);
}

void GhosttyConfigProcessLoaderTest::publishesTypedSnapshotAndSourcePaths()
{
    ConfigFixture fixture;
    const QString included =
        fixture.filePath(QStringLiteral("included.ghostty"));
    const QString missing = fixture.filePath(QStringLiteral("missing.ghostty"));
    ConfigFixture::writeFile(included, QByteArrayLiteral("font-size = 18\n"));

    QJsonObject exportObject = object();
    QJsonObject configValues =
        exportObject.value(QStringLiteral("values")).toObject();
    configValues.insert(
        QStringLiteral("config-file"),
        QJsonArray{included, QStringLiteral("?") + missing, included});
    configValues.insert(QStringLiteral("config-default-files"), true);
    exportObject.insert(QStringLiteral("values"), configValues);

    const GhosttyConfigLoadResult result = makeGhosttyConfigProcessLoader(
        fakeOptions(fixture, {}, exportObject))(fixture.request());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(
        result->sourcePaths,
        QStringList({fixture.legacyPath, fixture.preferredPath, included}));
    QCOMPARE(result->values.configFiles.size(), 3);
    QCOMPARE(result->values.configFiles.at(0).path, included);
    QVERIFY(!result->values.configFiles.at(0).optional);
    QCOMPARE(result->values.configFiles.at(1).path, missing);
    QVERIFY(result->values.configFiles.at(1).optional);
    QCOMPARE(result->values.configFiles.at(2).path, included);
    QVERIFY(!result->values.configFiles.at(2).optional);
    QCOMPARE(result->values.scrollbackLimitBytes,
             std::optional<quint64>(std::numeric_limits<quint64>::max()));
    QCOMPARE(result->values.scrollbackLimitLines,
             std::optional<quint64>(quint64{9'876'543'210}));
    QCOMPARE(result->values.kittyImageStorageLimitBytes, quint32{123456789});
    QVERIFY(result->values.background.image.path.has_value());
    QCOMPARE(result->values.background.image.path->path,
             QStringLiteral("/fixture/background.png"));
    QVERIFY(result->values.background.image.path->optional);
    QCOMPARE(result->values.background.image.opacity, 1.25);
    QCOMPARE(result->values.background.image.position,
             TerminalBackgroundImagePosition::BottomRight);
    QCOMPARE(result->values.background.image.fit,
             TerminalBackgroundImageFit::Cover);
    QVERIFY(result->values.background.image.repeat);
    QCOMPARE(result->values.backgroundBlur, qint16{-2});
    QCOMPARE(result->values.padding.horizontal.leadingPoints, quint32(3));
    QCOMPARE(result->values.padding.horizontal.trailingPoints, quint32(5));
    QCOMPARE(result->values.padding.vertical.leadingPoints, quint32(7));
    QCOMPARE(result->values.padding.vertical.trailingPoints, quint32(11));
    QCOMPARE(result->values.padding.balance, TerminalPaddingBalance::Equal);
    QCOMPARE(result->values.padding.color, TerminalPaddingColor::ExtendAlways);
    QVERIFY(!result->keybindings.root.isEmpty());

    configValues.insert(QStringLiteral("config-default-files"), false);
    exportObject.insert(QStringLiteral("values"), configValues);
    const GhosttyConfigLoadResult withoutDefaults =
        makeGhosttyConfigProcessLoader(fakeOptions(fixture, {}, exportObject))(
            fixture.request());
    QVERIFY2(withoutDefaults.has_value(),
             qPrintable(errorMessage(withoutDefaults)));
    QCOMPARE(withoutDefaults->sourcePaths, QStringList{included});
    QVERIFY(!withoutDefaults->values.configDefaultFiles);
}

void GhosttyConfigProcessLoaderTest::diagnosesOnlyNonDefaultUnsupportedActions()
{
    ConfigFixture fixture;
    GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(fakeOptions(fixture))(fixture.request());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->diagnostics.isEmpty());

    QJsonObject configured = object();
    QJsonObject current =
        configured.value(QStringLiteral("keybindings")).toObject();
    QJsonArray root = current.value(QStringLiteral("root")).toArray();
    root.append(binding({unicodeTrigger('x', GhosttyKeybindCtrl)},
                        {QStringLiteral("clear_screen")}));
    root.append(binding({unicodeTrigger('y', GhosttyKeybindCtrl)},
                        {QStringLiteral("clear_screen")}));
    root.append(binding({unicodeTrigger('z', GhosttyKeybindCtrl)},
                        {QStringLiteral("inspector:toggle")}));
    current.insert(QStringLiteral("root"), root);
    configured.insert(QStringLiteral("keybindings"), current);

    result = makeGhosttyConfigProcessLoader(
        fakeOptions(fixture, {}, configured))(fixture.request());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    const auto warnings = std::ranges::count_if(
        result->diagnostics, [](const GhosttyConfigDiagnostic &diagnostic) {
            return diagnostic.message.contains(QStringLiteral("clear_screen"));
        });
    QCOMPARE(warnings, 1);
    QVERIFY(std::ranges::none_of(result->diagnostics,
                                 [](const GhosttyConfigDiagnostic &diagnostic) {
                                     return diagnostic.message.contains(
                                         QStringLiteral("inspector:toggle"));
                                 }));

    // A flag-only change at the default location is also a user-visible
    // semantic change and should expose the unsupported current action.
    configured = object();
    current = configured.value(QStringLiteral("keybindings")).toObject();
    root = current.value(QStringLiteral("root")).toArray();
    QJsonObject changed = root.at(0).toObject();
    changed.insert(QStringLiteral("flags"), flags(false));
    changed.insert(QStringLiteral("actions"),
                   QJsonArray{QStringLiteral("clear_screen")});
    root.replace(0, changed);
    current.insert(QStringLiteral("root"), root);
    configured.insert(QStringLiteral("keybindings"), current);
    result = makeGhosttyConfigProcessLoader(
        fakeOptions(fixture, {}, configured))(fixture.request());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(
        std::ranges::count_if(result->diagnostics,
                              [](const GhosttyConfigDiagnostic &diagnostic) {
                                  return diagnostic.message.contains(
                                      QStringLiteral("clear_screen"));
                              }),
        1);
}

void GhosttyConfigProcessLoaderTest::rejectsQueryFailuresAndMalformedData()
{
    ConfigFixture fixture;
    GhosttyConfigLoadResult result = makeGhosttyConfigProcessLoader(fakeOptions(
        fixture, QStringLiteral("config-query-failure")))(fixture.request());
    QVERIFY(!result);
    QCOMPARE(
        result.error(),
        QStringLiteral(
            "Ghostty config helper failed during config query with exit code 8: "
            "stderr: config query failed"));

    result = makeGhosttyConfigProcessLoader(fakeOptions(
        fixture, QStringLiteral("config-query-malformed")))(fixture.request());
    QVERIFY(!result);
    QVERIFY(result.error().startsWith(QStringLiteral(
        "Ghostty config query returned malformed data: Invalid Ghostty "
        "structured config JSON")));

    result = makeGhosttyConfigProcessLoader(fakeOptions(
        fixture, QStringLiteral("config-consistency-query-failure")))(
        fixture.request());
    QVERIFY(!result);
    QCOMPARE(
        result.error(),
        QStringLiteral(
            "Ghostty config helper failed during config consistency query with "
            "exit code 9: stderr: config consistency query failed"));
}

void GhosttyConfigProcessLoaderTest::
    rejectsConfigThatChangesValidlyDuringQueries()
{
    ConfigFixture fixture;
    GhosttyConfigLoadResult result = makeGhosttyConfigProcessLoader(
        fakeOptions(fixture, QStringLiteral("config-consistency-mismatch"),
                    object(), withFontSize(object(), 18.0)))(fixture.request());
    QVERIFY(!result);
    QCOMPARE(
        result.error(),
        QStringLiteral(
            "Ghostty config changed while it was being queried; reload will retry"));

    auto formattingOnly =
        fakeOptions(fixture, QStringLiteral("config-consistency-mismatch"));
    formattingOnly.environment.insert(
        QStringLiteral("GHOSTTY_QT_FAKE_CONFIG_JSON_SECOND"),
        QStringLiteral(" ") + QString::fromUtf8(json()));
    result = makeGhosttyConfigProcessLoader(std::move(formattingOnly))(
        fixture.request());
    QVERIFY(!result);
    QCOMPARE(
        result.error(),
        QStringLiteral(
            "Ghostty config changed while it was being queried; reload will retry"));
}

void GhosttyConfigProcessLoaderTest::preservesSuccessfulHelperWarnings()
{
    ConfigFixture fixture;
    auto options = fakeOptions(fixture);
    options.environment.insert(
        QStringLiteral("GHOSTTY_QT_FAKE_SUCCESS_WARNING"),
        QStringLiteral("both standard files exist\n"
                       "both standard files exist\n"));
    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.request());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->diagnostics.size(), 1);
    QCOMPARE(result->diagnostics.constFirst().message,
             QStringLiteral("Ghostty config helper config query: "
                            "both standard files exist"));
}

void GhosttyConfigProcessLoaderTest::
    reportsTimeoutCrashAndStartFailureDeterministically()
{
    ConfigFixture fixture;
    const QString slowHelper =
        fixture.filePath(QStringLiteral("slow-config-helper"));
    ConfigFixture::writeFile(slowHelper,
                             QByteArrayLiteral("#!/bin/sh\nexec sleep 1\n"));
    QVERIFY(QFile::setPermissions(slowHelper,
                                  QFileDevice::ReadOwner
                                      | QFileDevice::WriteOwner
                                      | QFileDevice::ExeOwner));

    auto timeout = fakeOptions(fixture);
    timeout.helperPath = slowHelper;
    timeout.timeoutMilliseconds = 25;
    GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(timeout)(fixture.request());
    QVERIFY(!result);
    QCOMPARE(
        result.error(),
        QStringLiteral(
            "Ghostty config helper timed out during config query after 25 ms"));

    auto overall = fakeOptions(fixture);
    overall.helperPath = slowHelper;
    overall.timeoutMilliseconds = 2'000;
    overall.overallTimeoutMilliseconds = 30;
    result = makeGhosttyConfigProcessLoader(overall)(fixture.request());
    QVERIFY(!result);
    QVERIFY(result.error().contains(
        QStringLiteral("timed out during config query after 30 ms")));

    const QString slowConsistencyHelper =
        fixture.filePath(QStringLiteral("slow-consistency-config-helper"));
    ConfigFixture::writeFile(
        slowConsistencyHelper,
        QByteArrayLiteral("#!/bin/sh\n"
                          "state=\"${GHOSTTY_QT_FAKE_INVOCATION_LOG}.state\"\n"
                          "if [ -e \"$state\" ]; then exec sleep 1; fi\n"
                          ": > \"$state\"\n"
                          "printf '%s' \"$GHOSTTY_QT_FAKE_CONFIG_JSON\"\n"));
    QVERIFY(QFile::setPermissions(slowConsistencyHelper,
                                  QFileDevice::ReadOwner
                                      | QFileDevice::WriteOwner
                                      | QFileDevice::ExeOwner));
    auto consistencyTimeout = fakeOptions(fixture);
    consistencyTimeout.helperPath = slowConsistencyHelper;
    consistencyTimeout.timeoutMilliseconds = 2'000;
    consistencyTimeout.overallTimeoutMilliseconds = 500;
    result =
        makeGhosttyConfigProcessLoader(consistencyTimeout)(fixture.request());
    QVERIFY(!result);
    QVERIFY(result.error().startsWith(QStringLiteral(
        "Ghostty config helper timed out during config consistency query "
        "after ")));
    QVERIFY(result.error().endsWith(QStringLiteral(" ms")));

    const QString crashingHelper =
        fixture.filePath(QStringLiteral("crashing-config-helper"));
    ConfigFixture::writeFile(crashingHelper,
                             QByteArrayLiteral("#!/bin/sh\nkill -ABRT $$\n"));
    QVERIFY(QFile::setPermissions(crashingHelper,
                                  QFileDevice::ReadOwner
                                      | QFileDevice::WriteOwner
                                      | QFileDevice::ExeOwner));
    auto crash = fakeOptions(fixture);
    crash.helperPath = crashingHelper;
    result = makeGhosttyConfigProcessLoader(crash)(fixture.request());
    QVERIFY(!result);
    QCOMPARE(
        result.error(),
        QStringLiteral("Ghostty config helper crashed during config query"));

    auto missing = fakeOptions(fixture);
    missing.helperPath = QDir(fixture.temporary.path())
                             .filePath(QStringLiteral("does-not-exist"));
    missing.timeoutMilliseconds = 100;
    result = makeGhosttyConfigProcessLoader(missing)(fixture.request());
    QVERIFY(!result);
    QCOMPARE(
        result.error(),
        QStringLiteral(
            "Ghostty config helper could not be started during config query"));
}

void GhosttyConfigProcessLoaderTest::
    realHelperAppliesConfigurationArgumentPrecedence()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral(
            "font-family = File Regular One\n"
            "font-family = File Regular Two\n"
            "font-family-bold = File Bold\n"
            "font-family-italic = File Italic\n"
            "font-family-bold-italic = File Bold Italic\n"
            "font-style = File Regular Style\n"
            "font-style-bold = File Bold Style\n"
            "font-style-italic = File Italic Style\n"
            "font-style-bold-italic = default\n"
            "font-size = 11\n"
            "title = File Title\n"
            "wait-after-command = false\n"
            "adjust-cell-width = 1\n"
            "adjust-cell-height = 25%\n"
            "adjust-font-baseline = -3\n"
            "adjust-underline-position = -20%\n"
            "adjust-underline-thickness = 5\n"
            "adjust-strikethrough-position = 60%\n"
            "adjust-strikethrough-thickness = -7\n"
            "adjust-overline-position = 80%\n"
            "adjust-overline-thickness = 9\n"
            "adjust-cursor-thickness = 100%\n"
            "adjust-cursor-height = -11\n"
            // These unsupported renderer metrics must still participate in
            // the pinned AutoHashMap because they can change the exported
            // iteration order of the eleven supported entries.
            "adjust-box-thickness = 12\n"
            "adjust-icon-height = 13\n"));
    const QStringList arguments{
        QStringLiteral("--font-family="),
        QStringLiteral("--font-family=CLI=主👻"),
        QStringLiteral("--font-family=CLI Secondary"),
        QStringLiteral("--font-family-bold="),
        QStringLiteral("--font-family-bold=CLI Bold"),
        QStringLiteral("--font-style=default"),
        QStringLiteral("--font-style-bold=false"),
        QStringLiteral("--font-size=17.25"),
        QStringLiteral("--title=  CLI Title  "),
        QStringLiteral("--wait-after-command=true"),
    };

    auto direct = queryRealConfigExport(helperPath, fixture, arguments);
    QVERIFY2(direct.has_value(), qPrintable(errorMessage(direct)));

    auto options = realOptions(helperPath);
    options.configurationArguments = arguments;
    const GhosttyConfigLoadResult loaded =
        makeGhosttyConfigProcessLoader(std::move(options))(fixture.request());
    QVERIFY2(loaded.has_value(), qPrintable(errorMessage(loaded)));

    const TerminalTypography &typography = loaded->values.typography;
    QCOMPARE(typography.face(TerminalFontRole::Regular).families,
             QStringList({QStringLiteral("CLI=主👻"),
                          QStringLiteral("CLI Secondary")}));
    QCOMPARE(typography.face(TerminalFontRole::Bold).families,
             QStringList({QStringLiteral("CLI Bold")}));
    QCOMPARE(typography.face(TerminalFontRole::Italic).families,
             QStringList({QStringLiteral("File Italic")}));
    QCOMPARE(typography.face(TerminalFontRole::BoldItalic).families,
             QStringList({QStringLiteral("File Bold Italic")}));
    QVERIFY(std::holds_alternative<TerminalFontStyles::Automatic>(
        typography.face(TerminalFontRole::Regular).style));
    QVERIFY(std::holds_alternative<TerminalFontStyles::Disabled>(
        typography.face(TerminalFontRole::Bold).style));
    QCOMPARE(std::get<TerminalFontStyles::Named>(
                 typography.face(TerminalFontRole::Italic).style)
                 .name,
             QStringLiteral("File Italic Style"));
    QVERIFY(std::holds_alternative<TerminalFontStyles::Automatic>(
        typography.face(TerminalFontRole::BoldItalic).style));
    QCOMPARE(typography.pointSize, 17.25);
    QCOMPARE(loaded->values.title,
             std::optional<QString>(QStringLiteral("  CLI Title  ")));
    QVERIFY(loaded->values.waitAfterCommand);
    QCOMPARE(direct->values.title, loaded->values.title);
    QCOMPARE(direct->values.waitAfterCommand, loaded->values.waitAfterCommand);

    const auto expectedModifiers =
        std::to_array<std::pair<TerminalMetric, TerminalMetricModifier>>({
            {TerminalMetric::CellWidth,
             TerminalMetricModifiers::Absolute{.pixels = 1}},
            {TerminalMetric::CellHeight,
             TerminalMetricModifiers::Percentage{.multiplier = 1.25}},
            {TerminalMetric::FontBaseline,
             TerminalMetricModifiers::Absolute{.pixels = -3}},
            {TerminalMetric::UnderlinePosition,
             TerminalMetricModifiers::Percentage{.multiplier = 0.8}},
            {TerminalMetric::UnderlineThickness,
             TerminalMetricModifiers::Absolute{.pixels = 5}},
            {TerminalMetric::StrikethroughPosition,
             TerminalMetricModifiers::Percentage{.multiplier = 1.6}},
            {TerminalMetric::StrikethroughThickness,
             TerminalMetricModifiers::Absolute{.pixels = -7}},
            {TerminalMetric::OverlinePosition,
             TerminalMetricModifiers::Percentage{.multiplier = 1.8}},
            {TerminalMetric::OverlineThickness,
             TerminalMetricModifiers::Absolute{.pixels = 9}},
            {TerminalMetric::CursorThickness,
             TerminalMetricModifiers::Percentage{.multiplier = 2.0}},
            {TerminalMetric::CursorHeight,
             TerminalMetricModifiers::Absolute{.pixels = -11}},
        });
    for (const auto &[metric, expected] : expectedModifiers) {
        const auto &actual = typography.metricModifiers[metric];
        QVERIFY(actual.has_value());
        QVERIFY(*actual == expected);
    }
    const std::vector<TerminalMetric> expectedModifierOrder{
        TerminalMetric::StrikethroughThickness,
        TerminalMetric::CellWidth,
        TerminalMetric::CursorThickness,
        TerminalMetric::UnderlineThickness,
        TerminalMetric::CellHeight,
        TerminalMetric::StrikethroughPosition,
        TerminalMetric::UnderlinePosition,
        TerminalMetric::FontBaseline,
        TerminalMetric::CursorHeight,
        TerminalMetric::OverlinePosition,
        TerminalMetric::OverlineThickness,
    };
    QVERIFY(typography.metricModifiers.applicationOrder
            == expectedModifierOrder);
    QVERIFY(typography == direct->values.typography);

    auto validation = runRealHelper(helperPath, fixture,
                                    {QStringLiteral("+validate-config")});
    QVERIFY2(validation.has_value(), qPrintable(errorMessage(validation)));
    QCOMPARE(validation->exitStatus, QProcess::NormalExit);
    QCOMPARE(validation->exitCode, 0);
    QVERIFY(validation->standardError.isEmpty());
}

void GhosttyConfigProcessLoaderTest::realHelperDisablesDefaultConfigFiles()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath,
                             QByteArrayLiteral("font-size = 14\n"));
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("font-size = 31\nfont-family = File Family\n"));

    auto options = realOptions(helperPath);
    options.configurationArguments = {
        QStringLiteral("--font-size=17.25"),
        QStringLiteral("--config-default-files=false"),
    };
    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(std::move(options))(fixture.request());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(!result->values.configDefaultFiles);
    QCOMPARE(result->values.typography.pointSize, 17.25);
    QVERIFY(!result->values.typography.face(TerminalFontRole::Regular)
                 .families.contains(QStringLiteral("File Family")));
    QVERIFY(result->sourcePaths.isEmpty());
}

void GhosttyConfigProcessLoaderTest::realHelperExportsTypographyShaping()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral(
            "font-feature = -calt, cv01=2\n"
            "font-feature = calt=1\n"
            "font-variation = wght=100\n"
            "font-variation = wght=200\n"
            "font-variation-bold = wght=700\n"
            "font-variation-italic = slnt=-12.5\n"
            "font-codepoint-map = U+2500-U+257F=Symbols One\n"
            "font-codepoint-map = U+2500=Symbols Override\n"
            "font-codepoint-map = U+110000-U+1FFFFF=\n"
            "font-synthetic-style = no-bold,italic,no-bold-italic\n"
            "font-shaping-break = no-cursor\n"
            "freetype-load-flags = no-hinting,force-autohint,monochrome,"
            "no-autohint,no-light\n"));

    const auto result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    const TerminalTypography &typography = result->values.typography;
    QCOMPARE(typography.features,
             QVector<TerminalFontFeature>({
                 {.tag = 1818847073U, .value = 1U},
                 {.tag = 1667329140U, .value = 0U},
                 {.tag = 1668689969U, .value = 2U},
                 {.tag = 1667329140U, .value = 1U},
             }));
    QCOMPARE(typography.face(TerminalFontRole::Regular).variations,
             QVector<TerminalFontVariation>({
                 TerminalFontVariation::fromValue(2003265652U, 100.0),
                 TerminalFontVariation::fromValue(2003265652U, 200.0),
             }));
    QCOMPARE(typography.face(TerminalFontRole::Bold).variations,
             QVector<TerminalFontVariation>({
                 TerminalFontVariation::fromValue(2003265652U, 700.0),
             }));
    QCOMPARE(typography.face(TerminalFontRole::Italic).variations,
             QVector<TerminalFontVariation>({
                 TerminalFontVariation::fromValue(1936486004U, -12.5),
             }));
    QVERIFY(typography.face(TerminalFontRole::BoldItalic).variations.isEmpty());
    QCOMPARE(typography.codepointMap,
             QVector<TerminalCodepointFontMap>({
                 {.first = 0x2500U,
                  .last = 0x257fU,
                  .family = QStringLiteral("Symbols One")},
                 {.first = 0x2500U,
                  .last = 0x2500U,
                  .family = QStringLiteral("Symbols Override")},
                 {.first = 0x110000U, .last = 0x1fffffU, .family = {}},
             }));
    QVERIFY(!typography.syntheticStyle.bold);
    QVERIFY(typography.syntheticStyle.italic);
    QVERIFY(!typography.syntheticStyle.boldItalic);
    QVERIFY(!typography.shapingBreakCursor);
    QVERIFY(!typography.freetypeLoadFlags.hinting);
    QVERIFY(typography.freetypeLoadFlags.forceAutohint);
    QVERIFY(typography.freetypeLoadFlags.monochrome);
    QVERIFY(!typography.freetypeLoadFlags.autohint);
    QVERIFY(!typography.freetypeLoadFlags.light);
}

void GhosttyConfigProcessLoaderTest::
    realHelperRejectsInvalidConfigurationArgumentsDeterministically()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(fixture.preferredPath, {});

    auto options = realOptions(helperPath);
    options.configurationArguments = {
        QStringLiteral("--font-size=not-a-number"),
    };
    const auto load = makeGhosttyConfigProcessLoader(options);
    const GhosttyConfigLoadResult first = load(fixture.request());
    const GhosttyConfigLoadResult second = load(fixture.request());
    QVERIFY(!first);
    QVERIFY(!second);
    QCOMPARE(first.error(), second.error());
    QVERIFY(first.error().startsWith(QStringLiteral(
        "Ghostty config helper failed during config query with exit code 1")));
    QVERIFY(first.error().contains(QStringLiteral("font-size")));

    const QStringList invalidPrivateArguments{
        QStringLiteral("+show-config-json"),
        QStringLiteral("--ghostty-qt-color-scheme=light"),
        QStringLiteral("--font-size=not-a-number"),
    };
    auto privateFirst =
        runRealHelper(helperPath, fixture, invalidPrivateArguments);
    auto privateSecond =
        runRealHelper(helperPath, fixture, invalidPrivateArguments);
    QVERIFY2(privateFirst.has_value(), qPrintable(errorMessage(privateFirst)));
    QVERIFY2(privateSecond.has_value(),
             qPrintable(errorMessage(privateSecond)));
    QCOMPARE(privateFirst->exitStatus, QProcess::NormalExit);
    QCOMPARE(privateFirst->exitCode, 1);
    QVERIFY(*privateFirst == *privateSecond);
    QVERIFY(privateFirst->standardOutput.isEmpty());
    QVERIFY(
        privateFirst->standardError.contains(QByteArrayLiteral("font-size")));

    auto multiple = runRealHelper(helperPath, fixture,
                                  {QStringLiteral("+show-config-json"),
                                   QStringLiteral("+validate-config")});
    QVERIFY2(multiple.has_value(), qPrintable(errorMessage(multiple)));
    QCOMPARE(multiple->exitStatus, QProcess::NormalExit);
    QCOMPARE(multiple->exitCode, 64);
    QVERIFY(multiple->standardOutput.isEmpty());
}

void GhosttyConfigProcessLoaderTest::
    realHelperAppliesSelectedConditionalThemeAndWindowAppearance()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    const QString themesDirectory = fixture.filePath(QStringLiteral("themes"));
    QVERIFY(QDir().mkpath(themesDirectory));
    const QString lightTheme =
        QDir(themesDirectory).filePath(QStringLiteral("qt-light"));
    const QString darkTheme =
        QDir(themesDirectory).filePath(QStringLiteral("qt-dark"));
    ConfigFixture::writeFile(lightTheme,
                             QByteArrayLiteral("background = #f6f7f8\n"
                                               "foreground = #112233\n"
                                               "font-size = 12\n"));
    ConfigFixture::writeFile(darkTheme,
                             QByteArrayLiteral("background = #101112\n"
                                               "foreground = #ddeeff\n"
                                               "font-size = 18\n"));
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("theme = light:qt-light,dark:qt-dark\n"
                          "title = Qt Configured Title\n"
                          "window-theme = ghostty\n"
                          "window-title-font-family = Window Sans\n"
                          "window-titlebar-background = #123456\n"
                          "window-titlebar-foreground = #abcdef\n"
                          "window-subtitle = working-directory\n"));

    const auto light = queryRealConfigExport(helperPath, fixture, {},
                                             TerminalColorScheme::Light);
    const auto dark = queryRealConfigExport(helperPath, fixture, {},
                                            TerminalColorScheme::Dark);
    QVERIFY2(light.has_value(), qPrintable(errorMessage(light)));
    QVERIFY2(dark.has_value(), qPrintable(errorMessage(dark)));
    QCOMPARE(light->values.appearance.backgroundColor,
             QColor(QStringLiteral("#f6f7f8")));
    QCOMPARE(light->values.appearance.foregroundColor,
             QColor(QStringLiteral("#112233")));
    QCOMPARE(light->values.typography.pointSize, 12.0);
    QCOMPARE(dark->values.appearance.backgroundColor,
             QColor(QStringLiteral("#101112")));
    QCOMPARE(dark->values.appearance.foregroundColor,
             QColor(QStringLiteral("#ddeeff")));
    QCOMPARE(dark->values.typography.pointSize, 18.0);

    for (const GhosttyConfigExport *exported : {
             &*light,
             &*dark,
         }) {
        QCOMPARE(exported->values.title,
                 std::optional<QString>(QStringLiteral("Qt Configured Title")));
        QCOMPARE(exported->values.windowAppearance.theme, WindowTheme::Ghostty);
        QCOMPARE(exported->values.windowAppearance.titleFontFamily,
                 std::optional<QString>(QStringLiteral("Window Sans")));
        QCOMPARE(exported->values.windowAppearance.titlebarBackground,
                 std::optional<QColor>(QColor(QStringLiteral("#123456"))));
        QCOMPARE(exported->values.windowAppearance.titlebarForeground,
                 std::optional<QColor>(QColor(QStringLiteral("#abcdef"))));
        QCOMPARE(exported->values.windowAppearance.subtitle,
                 WindowSubtitleMode::WorkingDirectory);
        QVERIFY(exported->values.themeFiles.contains(lightTheme));
        QVERIFY(exported->values.themeFiles.contains(darkTheme));
        QCOMPARE(exported->values.themeFiles, light->values.themeFiles);
    }

    const GhosttyConfigLoadResult loaded = makeGhosttyConfigProcessLoader(
        realOptions(helperPath))(fixture.request(TerminalColorScheme::Dark));
    QVERIFY2(loaded.has_value(), qPrintable(errorMessage(loaded)));
    QVERIFY(loaded->values == dark->values);
}

void GhosttyConfigProcessLoaderTest::
    realHelperRejectsDiagnosticsFromSelectedConditionalTheme()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    const QString themesDirectory = fixture.filePath(QStringLiteral("themes"));
    QVERIFY(QDir().mkpath(themesDirectory));
    ConfigFixture::writeFile(
        QDir(themesDirectory).filePath(QStringLiteral("valid-light")),
        QByteArrayLiteral("background = #ffffff\n"));
    ConfigFixture::writeFile(
        QDir(themesDirectory).filePath(QStringLiteral("invalid-dark")),
        QByteArrayLiteral("background = not-a-color\n"));
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("theme = light:valid-light,dark:invalid-dark\n"));

    const auto light =
        runRealHelper(helperPath, fixture,
                      {QStringLiteral("+show-config-json"),
                       QStringLiteral("--ghostty-qt-color-scheme=light")});
    const auto dark =
        runRealHelper(helperPath, fixture,
                      {QStringLiteral("+show-config-json"),
                       QStringLiteral("--ghostty-qt-color-scheme=dark")});
    QVERIFY2(light.has_value(), qPrintable(errorMessage(light)));
    QVERIFY2(dark.has_value(), qPrintable(errorMessage(dark)));
    QCOMPARE(light->exitStatus, QProcess::NormalExit);
    QCOMPARE(light->exitCode, 0);
    QVERIFY(!light->standardOutput.isEmpty());
    QCOMPARE(dark->exitStatus, QProcess::NormalExit);
    QCOMPARE(dark->exitCode, 1);
    QVERIFY(dark->standardOutput.isEmpty());
    QVERIFY(dark->standardError.contains(QByteArrayLiteral("not-a-color")));

    ConfigFixture::writeFile(
        QDir(themesDirectory).filePath(QStringLiteral("valid-light")),
        QByteArrayLiteral("background = still-not-a-color\n"));
    ConfigFixture::writeFile(
        QDir(themesDirectory).filePath(QStringLiteral("invalid-dark")),
        QByteArrayLiteral("background = #000000\n"));
    const auto invalidLight =
        runRealHelper(helperPath, fixture,
                      {QStringLiteral("+show-config-json"),
                       QStringLiteral("--ghostty-qt-color-scheme=light")});
    const auto validDark =
        runRealHelper(helperPath, fixture,
                      {QStringLiteral("+show-config-json"),
                       QStringLiteral("--ghostty-qt-color-scheme=dark")});
    QVERIFY2(invalidLight.has_value(), qPrintable(errorMessage(invalidLight)));
    QVERIFY2(validDark.has_value(), qPrintable(errorMessage(validDark)));
    QCOMPARE(invalidLight->exitStatus, QProcess::NormalExit);
    QCOMPARE(invalidLight->exitCode, 1);
    QVERIFY(invalidLight->standardOutput.isEmpty());
    QVERIFY(invalidLight->standardError.contains(
        QByteArrayLiteral("still-not-a-color")));
    QCOMPARE(validDark->exitStatus, QProcess::NormalExit);
    QCOMPARE(validDark->exitCode, 0);
    QVERIFY(!validDark->standardOutput.isEmpty());
}

void GhosttyConfigProcessLoaderTest::realHelperFinalizesSurfaceValues()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty())
        QSKIP("The pinned Ghostty config helper is disabled");

    ConfigFixture fixture;
    const QString directory =
        QDir(fixture.temporary.path())
            .filePath(QStringLiteral("working directory"));
    QVERIFY(QDir().mkpath(directory));
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QStringLiteral("term = ghostty-qt-configured\n"
                       "working-directory = %1\n"
                       "split-inherit-working-directory = false\n"
                       "split-preserve-zoom = navigation\n"
                       "tab-inherit-working-directory = false\n"
                       "window-inherit-working-directory = false\n"
                       "window-inherit-font-size = false\n"
                       "window-new-tab-position = end\n"
                       "window-show-tab-bar = never\n"
                       "window-decoration = server\n"
                       "window-width = 1\n"
                       "window-height = 1\n"
                       "maximize = true\n"
                       "fullscreen = non-native-visible-menu\n"
                       "scrollback-limit-bytes = unlimited\n"
                       "scrollback-limit-lines = 7654321\n"
                       "image-storage-limit = 987654321\n")
            .arg(directory)
            .toUtf8());

    auto options = realOptions(helperPath);
    options.environment.remove(QStringLiteral("TERM_PROGRAM"));
    const auto load = makeGhosttyConfigProcessLoader(std::move(options));
    GhosttyConfigLoadResult result = load(fixture.request());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.term, QByteArrayLiteral("ghostty-qt-configured"));
    QVERIFY(result->values.workingDirectoryPath.has_value());
    QCOMPARE(*result->values.workingDirectoryPath,
             QFile::encodeName(directory));
    QVERIFY(!result->values.splitInheritWorkingDirectory);
    QVERIFY(result->values.splitPreserveZoom);
    QVERIFY(!result->values.tabInheritWorkingDirectory);
    QVERIFY(!result->values.windowInheritWorkingDirectory);
    QVERIFY(!result->values.windowInheritFontSize);
    QCOMPARE(result->values.windowNewTabPosition, WindowNewTabPosition::End);
    QCOMPARE(result->values.windowShowTabBar, WindowShowTabBar::Never);
    QCOMPARE(result->values.windowDecoration, WindowDecorationMode::Server);
    QCOMPARE(result->values.windowWidth, quint32(10));
    QCOMPARE(result->values.windowHeight, quint32(4));
    QVERIFY(result->values.maximize);
    QCOMPARE(result->values.fullscreen,
             GhosttyFullscreenMode::NonNativeVisibleMenu);
    QVERIFY(!result->values.scrollbackLimitBytes.has_value());
    QCOMPARE(result->values.scrollbackLimitLines,
             std::optional<quint64>(quint64{7'654'321}));
    QCOMPARE(result->values.kittyImageStorageLimitBytes, quint32{987654321});

    ConfigFixture::writeFile(fixture.preferredPath,
                             QByteArrayLiteral("term =\n"));
    result = load(fixture.request());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.term, QByteArrayLiteral("xterm-ghostty"));
    QVERIFY(!result->values.workingDirectoryPath.has_value());
    QCOMPARE(result->values.scrollbackLimitBytes,
             std::optional<quint64>(quint64{50'000'000}));
    QVERIFY(!result->values.scrollbackLimitLines.has_value());

    QByteArray byteTerm = QByteArrayLiteral("ghostty-");
    byteTerm.append(char(0x80));
    byteTerm.append(char(0xff));
    QByteArray byteTermConfig = QByteArrayLiteral("term = ");
    byteTermConfig.append(byteTerm);
    byteTermConfig.append('\n');
    ConfigFixture::writeFile(fixture.preferredPath, byteTermConfig);
    result = load(fixture.request());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.term, byteTerm);

    ConfigFixture::writeFile(fixture.preferredPath,
                             QByteArrayLiteral("working-directory = home\n"));
    result = load(fixture.request());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.term, QByteArrayLiteral("xterm-ghostty"));
    QVERIFY(result->values.workingDirectoryPath.has_value());
    QCOMPARE(*result->values.workingDirectoryPath,
             QFile::encodeName(QDir::homePath()));

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("working-directory = ~/ghostty-qt-test\n"));
    result = load(fixture.request());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.workingDirectoryPath.has_value());
    QCOMPARE(
        *result->values.workingDirectoryPath,
        QFile::encodeName(QDir(QDir::homePath())
                              .filePath(QStringLiteral("ghostty-qt-test"))));

    QByteArray rawDirectory = QByteArrayLiteral("/work/ghostty-");
    rawDirectory.append(char(0x80));
    rawDirectory.append(char(0xff));
    QByteArray rawConfig = QByteArrayLiteral("working-directory = ");
    rawConfig.append(rawDirectory);
    rawConfig.append('\n');
    ConfigFixture::writeFile(fixture.preferredPath, rawConfig);
    result = load(fixture.request());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.workingDirectoryPath, std::optional(rawDirectory));
}

void GhosttyConfigProcessLoaderTest::
    realHelperPreservesOriginalLaunchClassification()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty())
        QSKIP("The pinned Ghostty config helper is disabled");

    const passwd *const account = ::getpwuid(::getuid());
    if (account == nullptr || account->pw_dir == nullptr
        || account->pw_dir[0] == '\0') {
        QSKIP("The current account has no passwd home directory");
    }
    const QByteArray passwdHome(account->pw_dir);

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(fixture.preferredPath, {});

    auto cliOptions = realOptions(helperPath);
    cliOptions.probableCli = true;
    cliOptions.environment.remove(QStringLiteral("TERM_PROGRAM"));
    cliOptions.environment.insert(QStringLiteral("SHELL"),
                                  QStringLiteral("/ghostty/cli-shell"));
    const GhosttyConfigLoadResult cli = makeGhosttyConfigProcessLoader(
        std::move(cliOptions))(fixture.request());
    QVERIFY2(cli.has_value(), qPrintable(errorMessage(cli)));
    QVERIFY(!cli->values.workingDirectoryPath.has_value());
    QVERIFY(cli->values.ordinaryCommand.has_value());
    QCOMPARE(cli->values.ordinaryCommand->shellCommand,
             QByteArrayLiteral("/ghostty/cli-shell"));

    auto desktopOptions = realOptions(helperPath);
    desktopOptions.probableCli = false;
    // The loader must remove this helper-only mismatch because false records
    // that the original process had an empty TERM_PROGRAM.
    desktopOptions.environment.insert(QStringLiteral("TERM_PROGRAM"),
                                      QStringLiteral("helper-sentinel"));
    desktopOptions.environment.insert(QStringLiteral("SHELL"),
                                      QStringLiteral("/ghostty/cli-shell"));
    const auto desktopLoader =
        makeGhosttyConfigProcessLoader(std::move(desktopOptions));
    const GhosttyConfigLoadResult desktop = desktopLoader(fixture.request());
    QVERIFY2(desktop.has_value(), qPrintable(errorMessage(desktop)));
    QCOMPARE(desktop->values.workingDirectoryPath, std::optional(passwdHome));
    QVERIFY(desktop->values.ordinaryCommand.has_value());
    QVERIFY(desktop->values.ordinaryCommand->shellCommand
            != QByteArrayLiteral("/ghostty/cli-shell"));

    // Reloading samples a new generation but must not reclassify the process.
    const GhosttyConfigLoadResult reloaded = desktopLoader(fixture.request());
    QVERIFY2(reloaded.has_value(), qPrintable(errorMessage(reloaded)));
    QCOMPARE(reloaded->values.workingDirectoryPath, std::optional(passwdHome));
}

void GhosttyConfigProcessLoaderTest::realHelperFinalizesAppearanceAndUnbinds()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty())
        QSKIP("The pinned Ghostty config helper is disabled");

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral(
            "unfocused-split-opacity = -1\n"
            "background-opacity = -1\n"
            "background-opacity-cells = true\n"
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
            "minimum-contrast = 99\n"
            "clipboard-trim-trailing-spaces = 0\n"
            "clipboard-codepoint-map = U+2500,U+2502-U+2503=U+002D\n"
            "clipboard-codepoint-map = U+2500=overlap\n"
            "clipboard-codepoint-map = U+03A3=SUM\n"
            "clipboard-codepoint-map = U+200B=\n"
            "clipboard-codepoint-map = U+1F642=U+1F47B\n"
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
        realOptions(helperPath))(fixture.request());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->keybindings.root.size(), 1);
    QCOMPARE(result->keybindings.root.constFirst().actions,
             QStringList({QStringLiteral("new_tab")}));
    QCOMPARE(result->values.splitAppearance.unfocusedOpacity, 0.15);
    QCOMPARE(result->values.background.opacity, 0.0);
    QVERIFY(result->values.background.opacityCells);
    QCOMPARE(result->values.splitAppearance.unfocusedFill,
             std::optional<QColor>(QColor(QStringLiteral("#f0f8ff"))));
    QCOMPARE(result->values.appearance.palette.size(), qsizetype{256});
    QCOMPARE(result->values.appearance.palette.at(42),
             QColor(QStringLiteral("#123456")));
    QCOMPARE(result->values.splitAppearance.dividerColor,
             std::optional<QColor>(QColor(QStringLiteral("#f0f8ff"))));

    QCOMPARE(result->values.appearance.selectionForeground.kind,
             TerminalColorKind::CellBackground);
    QCOMPARE(result->values.appearance.selectionBackground.kind,
             TerminalColorKind::Color);
    QCOMPARE(result->values.appearance.selectionBackground.color,
             QColor(QStringLiteral("#334455")));
    QCOMPARE(result->values.appearance.searchForeground.kind,
             TerminalColorKind::CellBackground);
    QCOMPARE(result->values.appearance.cursorColor.kind,
             TerminalColorKind::Color);
    QCOMPARE(result->values.appearance.cursorColor.color,
             QColor(QStringLiteral("#abcdef")));
    QCOMPARE(result->values.appearance.cursorOpacity, 0.4);
    QCOMPARE(result->values.appearance.cursorStyle,
             TerminalCursorStyle::BlockHollow);
    QCOMPARE(result->values.appearance.cursorBlink, std::optional<bool>(false));
    QCOMPARE(result->values.appearance.cursorTextColor.kind,
             TerminalColorKind::CellForeground);
    QCOMPARE(result->values.appearance.boldColor.kind,
             TerminalBoldColorKind::Bright);
    QCOMPARE(result->values.appearance.faintOpacity, 0.25);
    QCOMPARE(result->values.appearance.minimumContrast, 21.0);
    QVERIFY(!result->values.selectionClipboard.trimTrailingSpaces);
    const TerminalClipboardCodepointMap &clipboardMap =
        result->values.selectionClipboard.codepointMap;
    QCOMPARE(clipboardMap.size(), qsizetype{6});
    QCOMPARE(clipboardMap.at(0).first, quint32{0x2500});
    QCOMPARE(clipboardMap.at(0).last, quint32{0x2500});
    QCOMPARE(std::get<quint32>(clipboardMap.at(0).replacement), quint32{0x2d});
    QCOMPARE(clipboardMap.at(1).first, quint32{0x2502});
    QCOMPARE(clipboardMap.at(1).last, quint32{0x2503});
    QCOMPARE(std::get<quint32>(clipboardMap.at(1).replacement), quint32{0x2d});
    QCOMPARE(std::get<QString>(clipboardMap.at(2).replacement),
             QStringLiteral("overlap"));
    QCOMPARE(std::get<QString>(clipboardMap.at(3).replacement),
             QStringLiteral("SUM"));
    QVERIFY(std::get<QString>(clipboardMap.at(4).replacement).isEmpty());
    QCOMPARE(std::get<quint32>(clipboardMap.at(5).replacement),
             quint32{0x1f47b});
    QVERIFY(!result->values.clipboardPaste.protection);
    QVERIFY(result->values.clipboardPaste.bracketedSafe);
    QCOMPARE(result->values.selectionClipboard.copyOnSelect,
             TerminalCopyOnSelectMode::PrimaryAndClipboard);
    QVERIFY(!result->values.selectionClipboard.clearOnTyping);
    QVERIFY(result->values.selectionClipboard.clearOnCopy);
    QCOMPARE(result->values.middleClickAction, MiddleClickAction::Ignore);

    ConfigFixture::writeFile(fixture.preferredPath, {});
    const auto defaults = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(defaults.has_value(), qPrintable(errorMessage(defaults)));
    QCOMPARE(defaults->values.background.opacity, 1.0);
    QVERIFY(!defaults->values.background.opacityCells);
    QCOMPARE(defaults->values.appearance.minimumContrast, 1.0);
    QVERIFY(defaults->values.selectionClipboard.codepointMap.isEmpty());
}

void GhosttyConfigProcessLoaderTest::realHelperExportsBackdropConfiguration()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    const QString requiredImage =
        fixture.filePath(QStringLiteral("relative-background.png"));
    ConfigFixture::writeFile(
        requiredImage,
        QByteArray::fromBase64(QByteArrayLiteral(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk"
            "+A8AAQUBAScY42YAAAAASUVORK5CYII=")));

    const auto load = makeGhosttyConfigProcessLoader(realOptions(helperPath));
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("background-image = relative-background.png\n"
                          "background-image-opacity = 1.5\n"
                          "background-image-position = top-right\n"
                          "background-image-fit = stretch\n"
                          "background-image-repeat = true\n"
                          "window-padding-x = 3,5\n"
                          "window-padding-y = 7,11\n"
                          "window-padding-balance = true\n"
                          "window-padding-color = extend\n"));

    GhosttyConfigLoadResult result = load(fixture.request());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.background.image.path.has_value());
    QCOMPARE(result->values.background.image.path->path, requiredImage);
    QVERIFY(!result->values.background.image.path->optional);
    QCOMPARE(result->values.background.image.opacity, 1.5);
    QCOMPARE(result->values.background.image.position,
             TerminalBackgroundImagePosition::TopRight);
    QCOMPARE(result->values.background.image.fit,
             TerminalBackgroundImageFit::Stretch);
    QVERIFY(result->values.background.image.repeat);
    QCOMPARE(result->values.padding.horizontal.leadingPoints, quint32(3));
    QCOMPARE(result->values.padding.horizontal.trailingPoints, quint32(5));
    QCOMPARE(result->values.padding.vertical.leadingPoints, quint32(7));
    QCOMPARE(result->values.padding.vertical.trailingPoints, quint32(11));
    QCOMPARE(result->values.padding.balance, TerminalPaddingBalance::Balanced);
    QCOMPARE(result->values.padding.color, TerminalPaddingColor::Extend);

    const QString optionalMissingImage =
        fixture.filePath(QStringLiteral("optional-missing.png"));
    QVERIFY(!QFileInfo::exists(optionalMissingImage));
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("background-image = ?optional-missing.png\n"
                          "background-image-opacity = 0.25\n"
                          "background-image-position = bottom-left\n"
                          "background-image-fit = none\n"
                          "background-image-repeat = false\n"
                          "window-padding-x = 13\n"
                          "window-padding-y = 17,19\n"
                          "window-padding-balance = equal\n"
                          "window-padding-color = extend-always\n"));

    result = load(fixture.request());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.background.image.path.has_value());
    QCOMPARE(result->values.background.image.path->path, optionalMissingImage);
    QVERIFY(result->values.background.image.path->optional);
    QCOMPARE(result->values.background.image.opacity, 0.25);
    QCOMPARE(result->values.background.image.position,
             TerminalBackgroundImagePosition::BottomLeft);
    QCOMPARE(result->values.background.image.fit,
             TerminalBackgroundImageFit::None);
    QVERIFY(!result->values.background.image.repeat);
    QCOMPARE(result->values.padding.horizontal.leadingPoints, quint32(13));
    QCOMPARE(result->values.padding.horizontal.trailingPoints, quint32(13));
    QCOMPARE(result->values.padding.vertical.leadingPoints, quint32(17));
    QCOMPARE(result->values.padding.vertical.trailingPoints, quint32(19));
    QCOMPARE(result->values.padding.balance, TerminalPaddingBalance::Equal);
    QCOMPARE(result->values.padding.color, TerminalPaddingColor::ExtendAlways);

    // An explicit empty image value and an absent backdrop configuration both
    // finalize to Ghostty's canonical no-image/default-padding snapshot.
    ConfigFixture::writeFile(fixture.preferredPath,
                             QByteArrayLiteral("background-image =\n"));
    result = load(fixture.request());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(!result->values.background.image.path.has_value());
    QCOMPARE(result->values.background.image.opacity, 1.0);
    QCOMPARE(result->values.background.image.position,
             TerminalBackgroundImagePosition::Center);
    QCOMPARE(result->values.background.image.fit,
             TerminalBackgroundImageFit::Contain);
    QVERIFY(!result->values.background.image.repeat);
    QCOMPARE(result->values.padding.horizontal.leadingPoints, quint32(2));
    QCOMPARE(result->values.padding.horizontal.trailingPoints, quint32(2));
    QCOMPARE(result->values.padding.vertical.leadingPoints, quint32(2));
    QCOMPARE(result->values.padding.vertical.trailingPoints, quint32(2));
    QCOMPARE(result->values.padding.balance, TerminalPaddingBalance::Disabled);
    QCOMPARE(result->values.padding.color, TerminalPaddingColor::Background);

    const auto invalidValues =
        std::to_array<std::pair<QByteArray, QByteArray>>({
            {QByteArrayLiteral("background-image-opacity = opaque\n"),
             QByteArrayLiteral("background-image-opacity")},
            {QByteArrayLiteral("background-image-position = nowhere\n"),
             QByteArrayLiteral("background-image-position")},
            {QByteArrayLiteral("background-image-fit = tile\n"),
             QByteArrayLiteral("background-image-fit")},
            {QByteArrayLiteral("background-image-repeat = sometimes\n"),
             QByteArrayLiteral("background-image-repeat")},
            {QByteArrayLiteral("window-padding-x = 3,-1\n"),
             QByteArrayLiteral("window-padding-x")},
            {QByteArrayLiteral("window-padding-y = points\n"),
             QByteArrayLiteral("window-padding-y")},
            {QByteArrayLiteral("window-padding-balance = sideways\n"),
             QByteArrayLiteral("window-padding-balance")},
            {QByteArrayLiteral("window-padding-color = transparent\n"),
             QByteArrayLiteral("window-padding-color")},
        });
    for (const auto &[configuration, key] : invalidValues) {
        ConfigFixture::writeFile(fixture.preferredPath, configuration);
        const GhosttyConfigLoadResult invalid = load(fixture.request());
        QVERIFY2(!invalid, key.constData());
        QVERIFY2(invalid.error().contains(QString::fromUtf8(key)),
                 qPrintable(invalid.error()));
    }
}

void GhosttyConfigProcessLoaderTest::
    realHelperExportsCustomShaderConfiguration()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    const QString first = fixture.filePath(QStringLiteral("first-shader.glsl"));
    const QString optional =
        fixture.filePath(QStringLiteral("optional-shader.glsl"));
    ConfigFixture::writeFile(first, QByteArrayLiteral("void mainImage() {}\n"));
    QVERIFY(!QFileInfo::exists(optional));

    const auto load = makeGhosttyConfigProcessLoader(realOptions(helperPath));
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("custom-shader = first-shader.glsl\n"
                          "custom-shader = ?optional-shader.glsl\n"
                          "custom-shader = first-shader.glsl\n"
                          "custom-shader-animation = always\n"));

    GhosttyConfigLoadResult result = load(fixture.request());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.customShaders.sources.size(), qsizetype{3});
    QCOMPARE(result->values.customShaders.sources.at(0).path, first);
    QVERIFY(!result->values.customShaders.sources.at(0).optional);
    QCOMPARE(result->values.customShaders.sources.at(1).path, optional);
    QVERIFY(result->values.customShaders.sources.at(1).optional);
    QCOMPARE(result->values.customShaders.sources.at(2).path, first);
    QVERIFY(!result->values.customShaders.sources.at(2).optional);
    QCOMPARE(result->values.customShaders.animation,
             TerminalCustomShaderAnimation::Always);

    const auto spellings =
        std::to_array<std::pair<QByteArray, TerminalCustomShaderAnimation>>({
            {QByteArrayLiteral("false"), TerminalCustomShaderAnimation::Never},
            {QByteArrayLiteral("true"), TerminalCustomShaderAnimation::Focused},
            {QByteArrayLiteral("always"),
             TerminalCustomShaderAnimation::Always},
        });
    for (const auto &[spelling, expected] : spellings) {
        ConfigFixture::writeFile(fixture.preferredPath,
                                 QByteArrayLiteral("custom-shader-animation = ")
                                     + spelling + QByteArrayLiteral("\n"));
        result = load(fixture.request());
        QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
        QVERIFY(result->values.customShaders.sources.isEmpty());
        QCOMPARE(result->values.customShaders.animation, expected);
    }
}

void GhosttyConfigProcessLoaderTest::realHelperExportsBackgroundBlur_data()
{
    QTest::addColumn<QByteArray>("configuration");
    QTest::addColumn<qint16>("expected");

    QTest::newRow("default") << QByteArray{} << qint16{0};
    QTest::newRow("false") << QByteArrayLiteral("background-blur = false\n")
                           << qint16{0};
    QTest::newRow("true") << QByteArrayLiteral("background-blur = true\n")
                          << qint16{20};
    QTest::newRow("explicit-zero")
        << QByteArrayLiteral("background-blur = 0\n") << qint16{0};
    QTest::newRow("radius")
        << QByteArrayLiteral("background-blur = 73\n") << qint16{73};
    QTest::newRow("glass-regular")
        << QByteArrayLiteral("background-blur = macos-glass-regular\n")
        << qint16{-1};
    QTest::newRow("glass-clear")
        << QByteArrayLiteral("background-blur = macos-glass-clear\n")
        << qint16{-2};
    QTest::newRow("renamed-radius")
        << QByteArrayLiteral("background-blur-radius = 91\n") << qint16{91};
}

void GhosttyConfigProcessLoaderTest::realHelperExportsBackgroundBlur()
{
    QFETCH(QByteArray, configuration);
    QFETCH(qint16, expected);

    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(fixture.preferredPath, configuration);

    const auto result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.backgroundBlur, expected);
}

void GhosttyConfigProcessLoaderTest::realHelperGeneratesEffectivePalette()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});

    ConfigFixture::writeFile(fixture.preferredPath, {});
    auto result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    const auto canonical = result->values.appearance.palette;

    // Ghostty deliberately leaves its canonical palette alone until at least
    // one palette entry is explicit, even when generation is enabled and the
    // configured special colors would otherwise produce a different result.
    ConfigFixture::writeFile(fixture.preferredPath,
                             QByteArrayLiteral("background = #f0f0f0\n"
                                               "foreground = #101010\n"
                                               "palette-generate = true\n"
                                               "palette-harmonious = true\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.appearance.palette == canonical);

    // Harmonious is also inert when generation itself is disabled. The
    // explicit base entry is still finalized normally.
    ConfigFixture::writeFile(fixture.preferredPath,
                             QByteArrayLiteral("background = #f0f0f0\n"
                                               "foreground = #101010\n"
                                               "palette = 1=#112233\n"
                                               "palette-harmonious = true\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.appearance.palette.at(1),
             QColor(QStringLiteral("#112233")));
    QCOMPARE(result->values.appearance.palette.at(16), canonical.at(16));
    QCOMPARE(result->values.appearance.palette.at(231), canonical.at(231));

    // An explicit entry triggers generation. Base16 and every explicit
    // extended entry remain untouched. The cube anchors include configured
    // background, ANSI indices 1-6, and foreground; the grayscale ramp uses
    // the configured background and foreground.
    ConfigFixture::writeFile(fixture.preferredPath,
                             QByteArrayLiteral("background = #010203\n"
                                               "foreground = #f0e0d0\n"
                                               "palette = 1=#112233\n"
                                               "palette = 20=#abcdef\n"
                                               "palette = 240=#654321\n"
                                               "palette-generate = true\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.appearance.palette.at(1),
             QColor(QStringLiteral("#112233")));
    QCOMPARE(result->values.appearance.palette.at(16),
             QColor(QStringLiteral("#010203")));
    QCOMPARE(result->values.appearance.palette.at(20),
             QColor(QStringLiteral("#abcdef")));
    QVERIFY(result->values.appearance.palette.at(21) != canonical.at(21));
    QCOMPARE(result->values.appearance.palette.at(231),
             QColor(QStringLiteral("#f0e0d0")));
    QCOMPARE(result->values.appearance.palette.at(240),
             QColor(QStringLiteral("#654321")));
    const auto darkGenerated = result->values.appearance.palette;

    // Harmonious changes only the orientation of a light theme.
    ConfigFixture::writeFile(fixture.preferredPath,
                             QByteArrayLiteral("background = #010203\n"
                                               "foreground = #f0e0d0\n"
                                               "palette = 1=#112233\n"
                                               "palette = 20=#abcdef\n"
                                               "palette = 240=#654321\n"
                                               "palette-generate = true\n"
                                               "palette-harmonious = true\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.appearance.palette == darkGenerated);

    // A light theme is normalized to the traditional dark-to-light index
    // order unless harmonious requests the configured light-to-dark order.
    ConfigFixture::writeFile(fixture.preferredPath,
                             QByteArrayLiteral("background = #f0f0f0\n"
                                               "foreground = #101010\n"
                                               "palette = 1=#112233\n"
                                               "palette-generate = true\n"
                                               "palette-harmonious = false\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.appearance.palette.at(16),
             QColor(QStringLiteral("#101010")));
    QCOMPARE(result->values.appearance.palette.at(231),
             QColor(QStringLiteral("#f0f0f0")));

    ConfigFixture::writeFile(fixture.preferredPath,
                             QByteArrayLiteral("background = #f0f0f0\n"
                                               "foreground = #101010\n"
                                               "palette = 1=#112233\n"
                                               "palette-generate = true\n"
                                               "palette-harmonious = true\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.appearance.palette.at(16),
             QColor(QStringLiteral("#f0f0f0")));
    QCOMPARE(result->values.appearance.palette.at(231),
             QColor(QStringLiteral("#101010")));
}

void GhosttyConfigProcessLoaderTest::realHelperExportsApplicationLifetime()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty())
        QSKIP("The pinned Ghostty config helper is disabled");

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral(
            "gtk-single-instance = false\n"
            "scrollbar = never\n"
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
    QCOMPARE(result->values.scrollbar, ScrollbarPolicy::Never);

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("quit-after-last-window-closed-delay = "
                          "584y 49w 23h 34m 33s 709ms 551us 615ns\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.quitAfterLastWindowClosedDelay.has_value());
    QCOMPARE(
        result->values.quitAfterLastWindowClosedDelay->count(),
        std::chrono::milliseconds::rep{std::numeric_limits<quint32>::max()});

    ConfigFixture::writeFile(fixture.preferredPath, {});
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(!result->values.quitAfterLastWindowClosedDelay.has_value());
    QCOMPARE(result->values.scrollbar, ScrollbarPolicy::System);
    QVERIFY(!result->defaultKeybindings.root.isEmpty());
}

void GhosttyConfigProcessLoaderTest::realHelperExportsLinuxCgroup()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("linux-cgroup = always\n"
                          "linux-cgroup-memory-limit = 18446744073709551615\n"
                          "linux-cgroup-processes-limit = 0\n"
                          "linux-cgroup-hard-fail = true\n"));

    auto result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.linuxCgroup.mode, LinuxCgroupMode::Always);
    QCOMPARE(result->values.linuxCgroup.memoryLimitBytes,
             std::optional<quint64>(std::numeric_limits<quint64>::max()));
    QCOMPARE(result->values.linuxCgroup.processesLimit,
             std::optional<quint64>(quint64{0}));
    QVERIFY(result->values.linuxCgroup.hardFail);

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("linux-cgroup =\n"
                          "linux-cgroup-memory-limit =\n"
                          "linux-cgroup-processes-limit =\n"
                          "linux-cgroup-hard-fail =\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.linuxCgroup.mode, LinuxCgroupMode::SingleInstance);
    QVERIFY(!result->values.linuxCgroup.memoryLimitBytes.has_value());
    QVERIFY(!result->values.linuxCgroup.processesLimit.has_value());
    QVERIFY(!result->values.linuxCgroup.hardFail);

    ConfigFixture::writeFile(fixture.preferredPath, {});
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.linuxCgroup.mode, LinuxCgroupMode::SingleInstance);
    QVERIFY(!result->values.linuxCgroup.memoryLimitBytes.has_value());
    QVERIFY(!result->values.linuxCgroup.processesLimit.has_value());
    QVERIFY(!result->values.linuxCgroup.hardFail);
}

void GhosttyConfigProcessLoaderTest::realHelperExportsShellIntegration()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(fixture.preferredPath, {});

    auto result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.shellIntegration,
             GhosttyShellIntegrationMode::Detect);
    QCOMPARE(result->values.shellIntegrationFeatures,
             GhosttyShellIntegrationFeatures{});

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral(
            "shell-integration = zsh\n"
            "shell-integration-features = "
            "no-cursor,sudo,no-title,ssh-env,ssh-terminfo,no-path\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.shellIntegration, GhosttyShellIntegrationMode::Zsh);
    const GhosttyShellIntegrationFeatures configured{
        .cursor = false,
        .sudo = true,
        .title = false,
        .sshEnvironment = true,
        .sshTerminfo = true,
        .path = false,
    };
    QCOMPARE(result->values.shellIntegrationFeatures, configured);

    result = queryRealConfigExport(
        helperPath, fixture,
        {
            QStringLiteral("--shell-integration=fish"),
            QStringLiteral("--shell-integration-features=false"),
        });
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.shellIntegration,
             GhosttyShellIntegrationMode::Fish);
    QCOMPARE(result->values.shellIntegrationFeatures,
             (GhosttyShellIntegrationFeatures{
                 .cursor = false,
                 .sudo = false,
                 .title = false,
                 .sshEnvironment = false,
                 .sshTerminfo = false,
                 .path = false,
             }));

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("shell-integration = bash\n"
                          "shell-integration =\n"
                          "shell-integration-features = false\n"
                          "shell-integration-features =\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.shellIntegration,
             GhosttyShellIntegrationMode::Detect);
    QCOMPARE(result->values.shellIntegrationFeatures,
             GhosttyShellIntegrationFeatures{});
}

void GhosttyConfigProcessLoaderTest::realHelperExportsEnvironment()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    const QString includeA =
        fixture.filePath(QStringLiteral("environment-a.ghostty"));
    const QString includeB =
        fixture.filePath(QStringLiteral("environment-b.ghostty"));
    const QString includeChild =
        fixture.filePath(QStringLiteral("environment-a-child.ghostty"));

    ConfigFixture::writeFile(fixture.legacyPath,
                             QByteArrayLiteral("env = LEGACY=legacy\n"
                                               "env = ORDER=legacy\n"));
    ConfigFixture::writeFile(includeChild,
                             QByteArrayLiteral("env = CHILD=child\n"
                                               "env = ORDER=child\n"));
    ConfigFixture::writeFile(includeA,
                             QStringLiteral("env = A=include-a\n"
                                            "env = ORDER=include-a\n"
                                            "env = REMOVE_CLI=\n"
                                            "config-file = %1\n")
                                 .arg(includeChild)
                                 .toUtf8());
    ConfigFixture::writeFile(includeB,
                             QByteArrayLiteral("env = B=include-b\n"
                                               "env = ORDER=include-b\n"));

    QByteArray preferred =
        QStringLiteral("env = PREFERRED=preferred\n"
                       "env = ORDER=preferred\n"
                       "env = REMOVE_FILE=temporary\n"
                       "env = REMOVE_FILE=\n"
                       "env = VALUE_EQUALS=alpha=beta=gamma\n"
                       "env = =empty-key\n"
                       "config-file = %1\n"
                       "config-file = %2\n")
            .arg(includeA, includeB)
            .toUtf8();
    QByteArray rawKey;
    rawKey.append(char(0x80));
    rawKey.append(char(0xff));
    QByteArray rawValue = QByteArrayLiteral("raw-");
    rawValue.append(char(0xfe));
    rawValue.append(char(0x81));
    preferred.append(QByteArrayLiteral("env = "));
    preferred.append(rawKey);
    preferred.append('=');
    preferred.append(rawValue);
    preferred.append('\n');
    ConfigFixture::writeFile(fixture.preferredPath, preferred);

    const QStringList arguments{
        QStringLiteral("--env=CLI=cli"),
        QStringLiteral("--env=ORDER=cli"),
        QStringLiteral("--env=REMOVE_CLI=temporary"),
    };
    auto result = queryRealConfigExport(helperPath, fixture, arguments);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));

    const TerminalEnvironment expected{
        {
            .key = QByteArrayLiteral("LEGACY"),
            .value = QByteArrayLiteral("legacy"),
        },
        {
            .key = QByteArrayLiteral("ORDER"),
            .value = QByteArrayLiteral("child"),
        },
        {
            .key = QByteArrayLiteral("PREFERRED"),
            .value = QByteArrayLiteral("preferred"),
        },
        {
            .key = QByteArrayLiteral("VALUE_EQUALS"),
            .value = QByteArrayLiteral("alpha=beta=gamma"),
        },
        {.key = QByteArray{}, .value = QByteArrayLiteral("empty-key")},
        {.key = rawKey, .value = rawValue},
        {
            .key = QByteArrayLiteral("CLI"),
            .value = QByteArrayLiteral("cli"),
        },
        {
            .key = QByteArrayLiteral("A"),
            .value = QByteArrayLiteral("include-a"),
        },
        {
            .key = QByteArrayLiteral("B"),
            .value = QByteArrayLiteral("include-b"),
        },
        {
            .key = QByteArrayLiteral("CHILD"),
            .value = QByteArrayLiteral("child"),
        },
    };
    QVERIFY(result->values.environment == expected);
    QVERIFY(std::ranges::none_of(
        result->values.environment, [](const TerminalEnvironmentEntry &entry) {
            return entry.key == QByteArrayLiteral("REMOVE_FILE")
                || entry.key == QByteArrayLiteral("REMOVE_CLI");
        }));

    // A bare env assignment clears the map produced by every earlier source.
    ConfigFixture::writeFile(fixture.preferredPath,
                             QByteArrayLiteral("env = BEFORE_RESET=value\n"
                                               "env =\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.environment.isEmpty());

    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(fixture.preferredPath, {});
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.environment.isEmpty());
}

void GhosttyConfigProcessLoaderTest::realHelperExportsCommands()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    const QString included =
        fixture.filePath(QStringLiteral("commands.ghostty"));
    const QByteArray defaultShell =
        QByteArrayLiteral("/bin/ghostty-qt-default-shell");
    const auto load = [&](QStringList arguments = {}) {
        auto options = realOptions(helperPath);
        options.configurationArguments = std::move(arguments);
        options.environment.insert(QStringLiteral("SHELL"),
                                   QString::fromLatin1(defaultShell));
        return makeGhosttyConfigProcessLoader(std::move(options))(
            fixture.request());
    };

    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(fixture.preferredPath, {});
    auto result = load();
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.ordinaryCommand.has_value());
    QCOMPARE(result->values.ordinaryCommand->kind, TerminalCommandKind::Shell);
    QCOMPARE(result->values.ordinaryCommand->shellCommand, defaultShell);
    QVERIFY(result->values.ordinaryCommand->defaultShell);
    QVERIFY(!result->values.initialCommand.has_value());
    QVERIFY(!result->values.waitAfterCommand);

    // Provenance cannot be inferred by comparing the finalized command bytes:
    // an explicit shell command may be identical to Ghostty's resolved default.
    ConfigFixture::writeFile(fixture.preferredPath,
                             QByteArrayLiteral("command = shell:")
                                 + defaultShell + '\n');
    result = load();
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.ordinaryCommand.has_value());
    QCOMPARE(result->values.ordinaryCommand->kind, TerminalCommandKind::Shell);
    QCOMPARE(result->values.ordinaryCommand->shellCommand, defaultShell);
    QVERIFY(!result->values.ordinaryCommand->defaultShell);

    // Theme files are loaded during finalization before the original config
    // is replayed. Their commands are still explicit, even when no ordinary
    // source had populated command before finalize.
    const QString theme =
        fixture.filePath(QStringLiteral("command-theme.ghostty"));
    ConfigFixture::writeFile(
        theme, QByteArrayLiteral("command = shell:theme-command\n"));
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QStringLiteral("theme = %1\n").arg(theme).toUtf8());
    result = load();
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.ordinaryCommand.has_value());
    QCOMPARE(result->values.ordinaryCommand->shellCommand,
             QByteArrayLiteral("theme-command"));
    QVERIFY(!result->values.ordinaryCommand->defaultShell);

    QByteArray rawArgument;
    rawArgument.append(char(0x80));
    rawArgument.append(char(0xff));
    QByteArray rawShell = QByteArrayLiteral("printf initial-");
    rawShell.append(char(0xfe));
    rawShell.append(char(0x81));
    QByteArray includeContents =
        QByteArrayLiteral("command = direct:/bin/printf alpha  ");
    includeContents.append(rawArgument);
    includeContents.append(QByteArrayLiteral("\ninitial-command = shell:"));
    includeContents.append(rawShell);
    includeContents.append(QByteArrayLiteral("\nwait-after-command = true\n"));
    ConfigFixture::writeFile(included, includeContents);
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QStringLiteral("command = shell:root command\n"
                       "initial-command = direct:/bin/root initial\n"
                       "wait-after-command = false\n"
                       "config-file = %1\n")
            .arg(included)
            .toUtf8());

    result = load({
        QStringLiteral("--command=shell:CLI command"),
        QStringLiteral("--initial-command=direct:/bin/cli initial"),
        QStringLiteral("--wait-after-command=false"),
    });
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.ordinaryCommand.has_value());
    QCOMPARE(result->values.ordinaryCommand->kind, TerminalCommandKind::Direct);
    const QVector<QByteArray> expectedArguments{
        QByteArrayLiteral("/bin/printf"),
        QByteArrayLiteral("alpha"),
        QByteArray{},
        rawArgument,
    };
    QVERIFY(result->values.ordinaryCommand->directArguments
            == expectedArguments);
    QVERIFY(result->values.ordinaryCommand->shellCommand.isEmpty());
    QVERIFY(!result->values.ordinaryCommand->defaultShell);
    QVERIFY(result->values.initialCommand.has_value());
    QCOMPARE(result->values.initialCommand->kind, TerminalCommandKind::Shell);
    QCOMPARE(result->values.initialCommand->shellCommand, rawShell);
    QVERIFY(result->values.initialCommand->directArguments.isEmpty());
    QVERIFY(!result->values.initialCommand->defaultShell);
    QVERIFY(result->values.waitAfterCommand);

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QStringLiteral("theme = %1\n"
                       "command = shell:configured\n"
                       "command =\n"
                       "initial-command = direct:/bin/configured\n"
                       "initial-command =\n"
                       "wait-after-command = true\n"
                       "wait-after-command =\n")
            .arg(theme)
            .toUtf8());
    result = load();
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.ordinaryCommand.has_value());
    QCOMPARE(result->values.ordinaryCommand->kind, TerminalCommandKind::Shell);
    QCOMPARE(result->values.ordinaryCommand->shellCommand, defaultShell);
    QVERIFY(result->values.ordinaryCommand->defaultShell);
    QVERIFY(!result->values.initialCommand.has_value());
    QVERIFY(!result->values.waitAfterCommand);
}

void GhosttyConfigProcessLoaderTest::
    realHelperExportsAbnormalCommandExitRuntime()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(fixture.preferredPath, {});

    auto result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.abnormalCommandExitRuntimeMilliseconds,
             quint32{250});

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("abnormal-command-exit-runtime = 1234\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.abnormalCommandExitRuntimeMilliseconds,
             quint32{1'234});

    result = queryRealConfigExport(
        helperPath, fixture,
        {QStringLiteral("--abnormal-command-exit-runtime=987")});
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.abnormalCommandExitRuntimeMilliseconds,
             quint32{987});

    result = queryRealConfigExport(
        helperPath, fixture,
        {QStringLiteral("--abnormal-command-exit-runtime=")});
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.abnormalCommandExitRuntimeMilliseconds,
             quint32{250});

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("abnormal-command-exit-runtime = 1234\n"
                          "abnormal-command-exit-runtime =\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.abnormalCommandExitRuntimeMilliseconds,
             quint32{250});

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("abnormal-command-exit-runtime = 0\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.abnormalCommandExitRuntimeMilliseconds, quint32{0});

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("abnormal-command-exit-runtime = 4294967295\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.abnormalCommandExitRuntimeMilliseconds,
             std::numeric_limits<quint32>::max());
}

void GhosttyConfigProcessLoaderTest::realHelperExportsScrollbackCompression()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(fixture.preferredPath, {});

    auto result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.scrollbackCompression);

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("scrollback-compression = false\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(!result->values.scrollbackCompression);

    result = queryRealConfigExport(
        helperPath, fixture, {QStringLiteral("--scrollback-compression=true")});
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.scrollbackCompression);

    result = queryRealConfigExport(
        helperPath, fixture, {QStringLiteral("--scrollback-compression=")});
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.scrollbackCompression);

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("scrollback-compression = false\n"
                          "scrollback-compression =\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.scrollbackCompression);
}

void GhosttyConfigProcessLoaderTest::realHelperExportsBellFeatures()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral(
            "bell-features = system,audio,no-attention,no-title,border\n"
            "bell-audio-path = ?sounds/bell.oga\n"
            "bell-audio-volume = 0.625\n"));

    auto result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.bellFeatures.system);
    QVERIFY(result->values.bellFeatures.audio);
    QVERIFY(!result->values.bellFeatures.attention);
    QVERIFY(!result->values.bellFeatures.title);
    QVERIFY(result->values.bellFeatures.border);
    QVERIFY(result->values.bellAudioPath.has_value());
    QCOMPARE(result->values.bellAudioPath->path,
             fixture.filePath(QStringLiteral("sounds/bell.oga")));
    QVERIFY(result->values.bellAudioPath->optional);
    QCOMPARE(result->values.bellAudioVolume, 0.625);

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("bell-audio-path = required-bell.oga\n"
                          "bell-audio-volume = -0.25\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.bellAudioPath.has_value());
    QCOMPARE(result->values.bellAudioPath->path,
             fixture.filePath(QStringLiteral("required-bell.oga")));
    QVERIFY(!result->values.bellAudioPath->optional);
    QCOMPARE(result->values.bellAudioVolume, -0.25);

    ConfigFixture::writeFile(fixture.preferredPath, {});
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(!result->values.bellFeatures.system);
    QVERIFY(!result->values.bellFeatures.audio);
    QVERIFY(result->values.bellFeatures.attention);
    QVERIFY(result->values.bellFeatures.title);
    QVERIFY(!result->values.bellFeatures.border);
    QVERIFY(!result->values.bellAudioPath.has_value());
    QCOMPARE(result->values.bellAudioVolume, 0.5);
}

void GhosttyConfigProcessLoaderTest::realHelperFinalizesMouseScrollMultiplier()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral(
            "mouse-scroll-multiplier = precision:0,discrete:20000\n"));

    auto result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.mouseScrollMultiplier.precision, 0.01);
    QCOMPARE(result->values.mouseScrollMultiplier.discrete, 10'000.0);

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral(
            "mouse-scroll-multiplier = precision:0.25,discrete:7.5\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.mouseScrollMultiplier.precision, 0.25);
    QCOMPARE(result->values.mouseScrollMultiplier.discrete, 7.5);

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("mouse-scroll-multiplier = 2.25\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.mouseScrollMultiplier.precision, 2.25);
    QCOMPARE(result->values.mouseScrollMultiplier.discrete, 2.25);

    ConfigFixture::writeFile(fixture.preferredPath, {});
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.mouseScrollMultiplier.precision, 1.0);
    QCOMPARE(result->values.mouseScrollMultiplier.discrete, 3.0);
}

void GhosttyConfigProcessLoaderTest::realHelperExportsMouseHideWhileTyping()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("mouse-hide-while-typing = true\n"));

    auto result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.mouseHideWhileTyping);

    ConfigFixture::writeFile(fixture.preferredPath, {});
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(!result->values.mouseHideWhileTyping);
}

void GhosttyConfigProcessLoaderTest::realHelperExportsFocusFollowsMouse()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(fixture.preferredPath,
                             QByteArrayLiteral("focus-follows-mouse = true\n"));

    auto result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.focusFollowsMouse);

    ConfigFixture::writeFile(fixture.preferredPath, {});
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(!result->values.focusFollowsMouse);
}

void GhosttyConfigProcessLoaderTest::realHelperExportsVtKamAllowed()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(fixture.preferredPath,
                             QByteArrayLiteral("vt-kam-allowed = true\n"));

    auto result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.vtKamAllowed);

    ConfigFixture::writeFile(fixture.preferredPath, {});
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(!result->values.vtKamAllowed);
}

void GhosttyConfigProcessLoaderTest::realHelperExportsSelectionWordChars()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("selection-word-chars = A\\u{2502}\\u{1F642}\n"));

    auto result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.selectionWordChars,
             QVector<quint32>({0, quint32('A'), 0x2502, 0x1f642}));

    ConfigFixture::writeFile(fixture.preferredPath, {});
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.selectionWordChars,
             QVector<quint32>({0,   ' ', '\t', '\'', '"', 0x2502, '`',
                               '|', ':', ';',  ',',  '(', ')',    '[',
                               ']', '{', '}',  '<',  '>', '$'}));
}

void GhosttyConfigProcessLoaderTest::realHelperExportsClickRepeatInterval()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("click-repeat-interval = 731\n"));

    auto result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.clickRepeatIntervalMilliseconds, quint32{731});

    ConfigFixture::writeFile(fixture.preferredPath,
                             QByteArrayLiteral("click-repeat-interval = 0\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.clickRepeatIntervalMilliseconds, quint32{500});

    ConfigFixture::writeFile(fixture.preferredPath, {});
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.clickRepeatIntervalMilliseconds, quint32{500});
}

void GhosttyConfigProcessLoaderTest::realHelperExportsClipboardWrite()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});

    for (const auto &[spelling, expected] :
         std::to_array<std::pair<QByteArray, TerminalClipboardAccess>>({
             {QByteArrayLiteral("ask"), TerminalClipboardAccess::Ask},
             {QByteArrayLiteral("allow"), TerminalClipboardAccess::Allow},
             {QByteArrayLiteral("deny"), TerminalClipboardAccess::Deny},
         })) {
        ConfigFixture::writeFile(fixture.preferredPath,
                                 QByteArrayLiteral("clipboard-write = ")
                                     + spelling + '\n');
        const auto result = queryRealConfigExport(helperPath, fixture);
        QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
        QCOMPARE(result->values.clipboardWrite, expected);
    }

    ConfigFixture::writeFile(fixture.preferredPath, {});
    const auto result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.clipboardWrite, TerminalClipboardAccess::Allow);
}

void GhosttyConfigProcessLoaderTest::realHelperExportsEnquiryResponse()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});

    QByteArray expected = QByteArrayLiteral("literal\\x05:");
    expected.append(QByteArray::fromHex("050080ff"));
    ConfigFixture::writeFile(fixture.preferredPath,
                             QByteArrayLiteral("enquiry-response = ") + expected
                                 + '\n');

    auto result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.enquiryResponse, expected);

    ConfigFixture::writeFile(fixture.preferredPath, {});
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.enquiryResponse.isEmpty());
}

void GhosttyConfigProcessLoaderTest::realHelperExportsScrollToBottom()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("scroll-to-bottom = no-keystroke,output\n"));

    auto result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(!result->values.scrollToBottom.keystroke);
    QVERIFY(result->values.scrollToBottom.output);

    ConfigFixture::writeFile(fixture.preferredPath, {});
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.scrollToBottom.keystroke);
    QVERIFY(!result->values.scrollToBottom.output);
}

void GhosttyConfigProcessLoaderTest::realHelperExportsRightClickAction()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("right-click-action = copy-or-paste\n"));

    auto result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.rightClickAction, RightClickAction::CopyOrPaste);

    ConfigFixture::writeFile(fixture.preferredPath, {});
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.rightClickAction, RightClickAction::ContextMenu);
}

void GhosttyConfigProcessLoaderTest::realHelperExportsMouseShiftCapture()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});

    for (const auto &[spelling, expected] :
         std::to_array<std::pair<QByteArray, MouseShiftCapture>>({
             {QByteArrayLiteral("false"), MouseShiftCapture::False},
             {QByteArrayLiteral("true"), MouseShiftCapture::True},
             {QByteArrayLiteral("always"), MouseShiftCapture::Always},
             {QByteArrayLiteral("never"), MouseShiftCapture::Never},
         })) {
        ConfigFixture::writeFile(fixture.preferredPath,
                                 QByteArrayLiteral("mouse-shift-capture = ")
                                     + spelling + '\n');
        const auto result = queryRealConfigExport(helperPath, fixture);
        QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
        QCOMPARE(result->values.mouseShiftCapture, expected);
    }

    ConfigFixture::writeFile(fixture.preferredPath, {});
    const auto defaultResult = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(defaultResult.has_value(),
             qPrintable(errorMessage(defaultResult)));
    QCOMPARE(defaultResult->values.mouseShiftCapture, MouseShiftCapture::False);
}

void GhosttyConfigProcessLoaderTest::realHelperExportsConfigFileSources()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty())
        QSKIP("The pinned Ghostty config helper is disabled");

    ConfigFixture fixture;
    const QString included =
        fixture.filePath(QStringLiteral("included.ghostty"));
    const QString missing = fixture.filePath(QStringLiteral("missing.ghostty"));
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        included,
        QByteArrayLiteral("font-family = Included Regular\n"
                          "font-size = 19\n"));
    ConfigFixture::writeFile(fixture.preferredPath,
                             QStringLiteral("font-family = File Regular\n"
                                            "font-size = 16\n"
                                            "config-file = %1\n"
                                            "config-file = ?%2\n")
                                 .arg(included, missing)
                                 .toUtf8());

    auto options = realOptions(helperPath);
    options.configurationArguments = {
        QStringLiteral("--font-family=CLI Regular"),
        QStringLiteral("--font-size=17.25"),
    };
    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(std::move(options))(fixture.request());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    const QStringList finalizedFamilies{
        QStringLiteral("CLI Regular"),
        QStringLiteral("Included Regular"),
    };
    for (const TerminalFontRole role : {
             TerminalFontRole::Regular,
             TerminalFontRole::Bold,
             TerminalFontRole::Italic,
             TerminalFontRole::BoldItalic,
         }) {
        QCOMPARE(result->values.typography.face(role).families,
                 finalizedFamilies);
    }
    // Pinned recursive files load after CLI arguments.
    QCOMPARE(result->values.typography.pointSize, 19.0);
    QCOMPARE(result->values.configFiles.size(), 2);
    QCOMPARE(result->values.configFiles.at(0).path, included);
    QVERIFY(!result->values.configFiles.at(0).optional);
    QCOMPARE(result->values.configFiles.at(1).path, missing);
    QVERIFY(result->values.configFiles.at(1).optional);
    QCOMPARE(
        result->sourcePaths,
        QStringList({fixture.legacyPath, fixture.preferredPath, included}));
}

void GhosttyConfigProcessLoaderTest::
    realHelperExportsFinalizedStructuredKeybindings()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty())
        QSKIP("The pinned Ghostty config helper is disabled");

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QStringLiteral("keybind = clear\n"
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
                       "keybind = modeé/ctrl+h=resize_split:right,10\n")
            .toUtf8());

    const GhosttyConfigLoadResult result = makeGhosttyConfigProcessLoader(
        realOptions(helperPath))(fixture.request());
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
            return diagnostic.message.contains(
                       QStringLiteral("close_tab:other"))
                || diagnostic.message.contains(
                    QStringLiteral("close_tab:right"));
        }));
}

void GhosttyConfigProcessLoaderTest::
    realHelperCanonicalizesTerminalControlActionPayloads()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty())
        QSKIP("The pinned Ghostty config helper is disabled");

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    QByteArray config = QByteArrayLiteral("keybind = clear\n"
                                          "keybind = ctrl+a=text:\\x15\n"
                                          "keybind = ctrl+b=text:");
    config.append(QByteArray::fromHex("f09f91bb"));
    config.append(QByteArrayLiteral("\nkeybind = ctrl+c=csi:"));
    config.append(QByteArray::fromHex("c3a9"));
    config.append(QByteArrayLiteral("\nkeybind = ctrl+d=esc:\\x7f\n"
                                    "keybind = ctrl+e=text:\\q\n"));
    ConfigFixture::writeFile(fixture.preferredPath, config);

    const GhosttyConfigLoadResult result = makeGhosttyConfigProcessLoader(
        realOptions(helperPath))(fixture.request());
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
    QCOMPARE(actionFor('a'), QStringList({QStringLiteral(R"(text:\\x15)")}));
    QCOMPARE(actionFor('b'),
             QStringList({QStringLiteral(R"(text:\xf0\x9f\x91\xbb)")}));
    QCOMPARE(actionFor('c'), QStringList({QStringLiteral(R"(csi:\xc3\xa9)")}));
    QCOMPARE(actionFor('d'), QStringList({QStringLiteral(R"(esc:\\x7f)")}));
    QCOMPARE(actionFor('e'), QStringList({QStringLiteral(R"(text:\\q)")}));
}

QTEST_GUILESS_MAIN(GhosttyConfigProcessLoaderTest)

#include "test_ghostty_config_process_loader.moc"
