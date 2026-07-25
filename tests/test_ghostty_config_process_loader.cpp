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

struct HelperResult {
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    int exitCode = -1;
    QByteArray standardOutput;
    QByteArray standardError;

    bool operator==(const HelperResult &) const = default;
};

std::expected<HelperResult, QString> runRealHelper(
    const QString &helperPath,
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
        return std::unexpected(QStringLiteral("structured helper did not start"));
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
    const QString &helperPath,
    const ConfigFixture &fixture,
    QStringList configurationArguments = {})
{
    configurationArguments.prepend(QStringLiteral("+show-config-json"));
    auto process = runRealHelper(
        helperPath, fixture, configurationArguments);
    if (!process) return std::unexpected(std::move(process.error()));
    if (process->exitStatus != QProcess::NormalExit
        || process->exitCode != 0) {
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
    void invokesStableFourProcessTransaction();
    void forwardsConfigurationArgumentsToEveryConfigQuery();
    void publishesTypedSnapshotAndSourcePaths();
    void diagnosesOnlyNonDefaultUnsupportedActions();
    void rejectsQueryFailuresAndMalformedData();
    void rejectsConfigThatBecomesInvalidDuringQueries();
    void rejectsConfigThatChangesValidlyDuringQueries();
    void preservesSuccessfulHelperWarnings();
    void reportsValidationFailureDeterministically();
    void reportsTimeoutCrashAndStartFailureDeterministically();
    void realHelperAppliesConfigurationArgumentPrecedence();
    void realHelperRejectsInvalidConfigurationArgumentsDeterministically();
    void realHelperFinalizesSurfaceValues();
    void realHelperFinalizesAppearanceAndUnbinds();
    void realHelperExportsApplicationLifetime();
    void realHelperExportsBellFeatures();
    void realHelperExportsMouseHideWhileTyping();
    void realHelperExportsFocusFollowsMouse();
    void realHelperExportsSelectionWordChars();
    void realHelperFinalizesMouseScrollMultiplier();
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
    QCOMPARE(result->values.typography.pointSize, 13.5);
    QCOMPARE(result->keybindings.root.size(), 1);
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
    };

    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));

    const QByteArray suffix =
        QByteArrayLiteral(" --font-family= --font-family=")
        + QStringLiteral("等号=👻").toUtf8()
        + QByteArrayLiteral(" --font-size=17.25\n");
    QCOMPARE(invocationLog(logPath),
             QByteArrayLiteral("+validate-config\n")
                 + QByteArrayLiteral("+show-config-json") + suffix
                 + QByteArrayLiteral("+validate-config\n")
                 + QByteArrayLiteral("+show-config-json") + suffix);
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
    };

    auto direct = queryRealConfigExport(helperPath, fixture, arguments);
    QVERIFY2(direct.has_value(), qPrintable(errorMessage(direct)));

    auto options = realOptions(helperPath);
    options.configurationArguments = arguments;
    const GhosttyConfigLoadResult loaded =
        makeGhosttyConfigProcessLoader(std::move(options))(
            fixture.candidates());
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
    QCOMPARE(
        std::get<TerminalFontStyles::Named>(
            typography.face(TerminalFontRole::Italic).style)
            .name,
        QStringLiteral("File Italic Style"));
    QVERIFY(std::holds_alternative<TerminalFontStyles::Automatic>(
        typography.face(TerminalFontRole::BoldItalic).style));
    QCOMPARE(typography.pointSize, 17.25);

    const auto expectedModifiers = std::to_array<
        std::pair<TerminalMetric, TerminalMetricModifier>>({
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

    auto validation = runRealHelper(
        helperPath, fixture, {QStringLiteral("+validate-config")});
    QVERIFY2(validation.has_value(), qPrintable(errorMessage(validation)));
    QCOMPARE(validation->exitStatus, QProcess::NormalExit);
    QCOMPARE(validation->exitCode, 0);
    QVERIFY(validation->standardError.isEmpty());
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
    const GhosttyConfigLoadResult first = load(fixture.candidates());
    const GhosttyConfigLoadResult second = load(fixture.candidates());
    QVERIFY(!first);
    QVERIFY(!second);
    QCOMPARE(first.error(), second.error());
    QVERIFY(first.error().startsWith(QStringLiteral(
        "Ghostty config helper failed during config query with exit code 1")));
    QVERIFY(first.error().contains(QStringLiteral("font-size")));

    const QStringList invalidPrivateArguments{
        QStringLiteral("+show-config-json"),
        QStringLiteral("--font-size=not-a-number"),
    };
    auto privateFirst = runRealHelper(
        helperPath, fixture, invalidPrivateArguments);
    auto privateSecond = runRealHelper(
        helperPath, fixture, invalidPrivateArguments);
    QVERIFY2(privateFirst.has_value(), qPrintable(errorMessage(privateFirst)));
    QVERIFY2(privateSecond.has_value(), qPrintable(errorMessage(privateSecond)));
    QCOMPARE(privateFirst->exitStatus, QProcess::NormalExit);
    QCOMPARE(privateFirst->exitCode, 1);
    QVERIFY(*privateFirst == *privateSecond);
    QVERIFY(privateFirst->standardOutput.isEmpty());
    QVERIFY(privateFirst->standardError.contains(
        QByteArrayLiteral("font-size")));

    auto multiple = runRealHelper(
        helperPath, fixture,
        {QStringLiteral("+show-config-json"),
         QStringLiteral("+validate-config")});
    QVERIFY2(multiple.has_value(), qPrintable(errorMessage(multiple)));
    QCOMPARE(multiple->exitStatus, QProcess::NormalExit);
    QCOMPARE(multiple->exitCode, 64);
    QVERIFY(multiple->standardOutput.isEmpty());
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
                       "window-decoration = server\n"
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
    QCOMPARE(result->values.windowDecoration, WindowDecorationMode::Server);
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
    QCOMPARE(result->values.scrollbar, ScrollbarPolicy::System);
    QVERIFY(!result->defaultKeybindings.root.isEmpty());
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

void GhosttyConfigProcessLoaderTest::realHelperExportsConfigFileSources()
{
    const QString helperPath = QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) QSKIP("The pinned Ghostty config helper is disabled");

    ConfigFixture fixture;
    const QString included = fixture.filePath(QStringLiteral("included.ghostty"));
    const QString missing = fixture.filePath(QStringLiteral("missing.ghostty"));
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        included,
        QByteArrayLiteral(
            "font-family = Included Regular\n"
            "font-size = 19\n"));
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QStringLiteral(
            "font-family = File Regular\n"
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
    const GhosttyConfigLoadResult result = makeGhosttyConfigProcessLoader(
        std::move(options))(fixture.candidates());
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
