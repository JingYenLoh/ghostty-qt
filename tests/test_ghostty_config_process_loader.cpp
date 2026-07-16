#include "ghostty_config_process_loader.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <limits>

#ifndef GHOSTTY_QT_FAKE_CONFIG_HELPER_PATH
#define GHOSTTY_QT_FAKE_CONFIG_HELPER_PATH ""
#endif

#ifndef GHOSTTY_QT_REAL_CONFIG_HELPER_PATH
#define GHOSTTY_QT_REAL_CONFIG_HELPER_PATH ""
#endif

namespace {

QByteArray defaultOutput()
{
    QByteArray output =
        QByteArrayLiteral("font-family = \n"
                          "font-size = 13\n"
                          "foreground = #ffffff\n"
                          "background = #282c34\n"
                          "selection-foreground = \n"
                          "selection-background = \n"
                          "cursor-color = \n"
                          "cursor-opacity = 1\n"
                          "cursor-style = block\n"
                          "cursor-style-blink = \n"
                          "cursor-text = \n"
                          "bold-color = \n"
                          "faint-opacity = 0.5\n"
                          "scrollback-limit = 50000000\n"
                          "confirm-close-surface = true\n"
                          "keybind = ctrl+shift+t=new_tab\n"
                          "config-file = \n");
    for (int index = 0; index < 256; ++index) {
        output.append(QStringLiteral("palette = %1=#%2%2%2\n")
                          .arg(index)
                          .arg(index, 2, 16, QLatin1Char('0'))
                          .toLatin1());
    }
    return output;
}

struct ConfigFixture {
    QTemporaryDir temporary;
    QString xdgHome;
    QString legacyPath;
    QString preferredPath;

    ConfigFixture()
    {
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

    QStringList candidates() const { return {legacyPath, preferredPath}; }

    static void writeFile(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qFatal("could not create test fixture file");
        }
        file.write(contents);
    }
};

GhosttyConfigProcessLoaderOptions fakeOptions(const ConfigFixture &fixture,
                                               const QString &mode = {})
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_EXPECT_XDG_CONFIG_HOME"),
                       fixture.xdgHome);
    environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_DEFAULT_OUTPUT"),
                       QString::fromLatin1(defaultOutput()));
    environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_CHANGES_OUTPUT"),
                       QStringLiteral("font-size = 17.25\n"));
    if (!mode.isEmpty()) {
        environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_MODE"), mode);
    }
    return {
        .helperPath = QString::fromUtf8(GHOSTTY_QT_FAKE_CONFIG_HELPER_PATH),
        .timeoutMilliseconds = 2'000,
        .environment = environment,
    };
}

} // namespace

class GhosttyConfigProcessLoaderTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void derivesXdgHomeFromEitherCandidateOrder();
    void mergesCanonicalOutputsIntoTypedSnapshot();
    void emptyRepeatableChangesResetDefaults();
    void rejectsMalformedCanonicalValues();
    void invokesValidationThenDefaultAndCurrentQueries();
    void rejectsConfigThatBecomesInvalidDuringQueries();
    void preservesSuccessfulHelperWarnings();
    void realHelperPreservesAppearanceAndEffectiveUnbindSemantics();
    void reportsValidationFailureDeterministically();
    void reportsTimeoutCrashAndStartFailureDeterministically();
};

void GhosttyConfigProcessLoaderTest::derivesXdgHomeFromEitherCandidateOrder()
{
    ConfigFixture fixture;
    QString error;
    QCOMPARE(ghosttyConfigXdgHome(fixture.candidates(), &error), fixture.xdgHome);
    QVERIFY(error.isEmpty());

    QCOMPARE(ghosttyConfigXdgHome(
                 {fixture.preferredPath, fixture.legacyPath}, &error),
             fixture.xdgHome);
    QVERIFY(error.isEmpty());

    QVERIFY(ghosttyConfigXdgHome({fixture.legacyPath}, &error).isEmpty());
    QCOMPARE(error,
             QStringLiteral("Ghostty config candidates must contain both config and config.ghostty"));
}

void GhosttyConfigProcessLoaderTest::mergesCanonicalOutputsIntoTypedSnapshot()
{
    ConfigFixture fixture;
    const QString includePath =
        QDir(fixture.temporary.path()).filePath(QStringLiteral("included.conf"));
    ConfigFixture::writeFile(includePath,
                             QByteArrayLiteral("foreground = #102030\n"));
    const QString missingOptional =
        QDir(fixture.temporary.path()).filePath(QStringLiteral("optional.conf"));

    const QByteArray changes = QString(
        QStringLiteral(
            "# canonical changed values\r\n"
            "font-family = JetBrains Mono\r\n"
            "font-family = Noto Color Emoji\r\n"
            "font-size = 15.5\r\n"
            "foreground = #102030\r\n"
            "palette = 1=#123456\r\n"
            "palette = 255=#fedcba\r\n"
            "selection-foreground = cell-background\r\n"
            "selection-background = #334455\r\n"
            "cursor-color = cell-background\r\n"
            "cursor-opacity = 0.375\r\n"
            "cursor-style = block_hollow\r\n"
            "cursor-style-blink = false\r\n"
            "cursor-text = cell-foreground\r\n"
            "bold-color = bright\r\n"
            "faint-opacity = 0.25\r\n"
            "scrollback-limit = 123456\r\n"
            "confirm-close-surface = always\r\n"
            "keybind = alt+n=new_tab\r\n"
            "keybind = chain=next_tab\r\n"
            "keybind = ctrl+x>ctrl+y=new_tab\r\n"
            "keybind = ctrl+f=toggle_fullscreen\r\n"
            "config-file = %1\r\n"
            "config-file = ?%2\r\n"))
                                   .arg(includePath, missingOptional)
                                   .toUtf8();

    const GhosttyConfigLoadResult result =
        parseGhosttyConfigShowOutputs(defaultOutput(), changes,
                                      fixture.candidates());
    QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
    const GhosttyConfigSnapshot &snapshot = *result.snapshot;
    QCOMPARE(snapshot.availability, GhosttyConfigAvailability::Available);
    QCOMPARE(snapshot.values.value(QStringLiteral("font-family")).toStringList(),
             QStringList({QStringLiteral("JetBrains Mono"),
                          QStringLiteral("Noto Color Emoji")}));
    QCOMPARE(snapshot.values.value(QStringLiteral("font-size")).toDouble(), 15.5);
    QCOMPARE(snapshot.values.value(QStringLiteral("foreground")).value<QColor>(),
             QColor(QStringLiteral("#102030")));
    QCOMPARE(snapshot.values.value(QStringLiteral("background")).value<QColor>(),
             QColor(QStringLiteral("#282c34")));
    const QVariantList palette =
        snapshot.values.value(QStringLiteral("palette")).toList();
    QCOMPARE(palette.size(), 256);
    QCOMPARE(palette.at(0).value<QColor>(), QColor(QStringLiteral("#000000")));
    QCOMPARE(palette.at(1).value<QColor>(), QColor(QStringLiteral("#123456")));
    QCOMPARE(palette.at(2).value<QColor>(), QColor(QStringLiteral("#020202")));
    QCOMPARE(palette.at(255).value<QColor>(), QColor(QStringLiteral("#fedcba")));
    QCOMPARE(snapshot.values.value(QStringLiteral("selection-foreground"))
                 .toString(),
             QStringLiteral("cell-background"));
    QCOMPARE(snapshot.values.value(QStringLiteral("selection-background"))
                 .value<QColor>(),
             QColor(QStringLiteral("#334455")));
    QCOMPARE(snapshot.values.value(QStringLiteral("cursor-color")).toString(),
             QStringLiteral("cell-background"));
    QCOMPARE(snapshot.values.value(QStringLiteral("cursor-opacity")).toDouble(),
             0.375);
    QCOMPARE(snapshot.values.value(QStringLiteral("cursor-style")).toString(),
             QStringLiteral("block_hollow"));
    QCOMPARE(snapshot.values.value(QStringLiteral("cursor-style-blink")).toBool(),
             false);
    QCOMPARE(snapshot.values.value(QStringLiteral("cursor-text")).toString(),
             QStringLiteral("cell-foreground"));
    QCOMPARE(snapshot.values.value(QStringLiteral("bold-color")).toString(),
             QStringLiteral("bright"));
    QCOMPARE(snapshot.values.value(QStringLiteral("faint-opacity")).toDouble(),
             0.25);
    QCOMPARE(snapshot.values.value(QStringLiteral("scrollback-limit")).toULongLong(),
             quint64(123456));
    QCOMPARE(snapshot.values.value(QStringLiteral("confirm-close-surface")).toString(),
             QStringLiteral("always"));
    QCOMPARE(snapshot.values.value(QStringLiteral("keybind")).toStringList(),
             QStringList({QStringLiteral("alt+n=new_tab"),
                          QStringLiteral("chain=next_tab"),
                          QStringLiteral("ctrl+x>ctrl+y=new_tab"),
                          QStringLiteral("ctrl+f=toggle_fullscreen")}));
    QCOMPARE(snapshot.diagnostics.size(), 2);
    QVERIFY(std::any_of(
        snapshot.diagnostics.cbegin(), snapshot.diagnostics.cend(),
        [](const GhosttyConfigDiagnostic &diagnostic) {
            return diagnostic.message.contains(
                QStringLiteral("key sequences are not implemented yet"));
        }));
    QVERIFY(std::any_of(
        snapshot.diagnostics.cbegin(), snapshot.diagnostics.cend(),
        [](const GhosttyConfigDiagnostic &diagnostic) {
            return diagnostic.message.contains(
                QStringLiteral("toggle_fullscreen"));
        }));
    QCOMPARE(snapshot.values.value(QStringLiteral("config-file")).toStringList(),
             QStringList({includePath, QStringLiteral("?") + missingOptional}));
    QCOMPARE(snapshot.sourcePaths,
             QStringList({fixture.legacyPath, fixture.preferredPath, includePath}));
}

void GhosttyConfigProcessLoaderTest::emptyRepeatableChangesResetDefaults()
{
    ConfigFixture fixture;
    const QByteArray defaults =
        QByteArrayLiteral("font-family = Monospace\n") + defaultOutput();
    const QByteArray changes =
        QByteArrayLiteral("font-family = \nkeybind = \nconfig-file = \n");

    const GhosttyConfigLoadResult result =
        parseGhosttyConfigShowOutputs(defaults, changes, fixture.candidates());
    QVERIFY(result.succeeded());
    QVERIFY(result.snapshot->values.value(QStringLiteral("font-family"))
                .toStringList()
                .isEmpty());
    QVERIFY(result.snapshot->values.value(QStringLiteral("config-file"))
                .toStringList()
                .isEmpty());
    QVERIFY(result.snapshot->values.value(QStringLiteral("keybind"))
                .toStringList()
                .isEmpty());
}

void GhosttyConfigProcessLoaderTest::rejectsMalformedCanonicalValues()
{
    ConfigFixture fixture;
    const GhosttyConfigLoadResult malformed = parseGhosttyConfigShowOutputs(
        defaultOutput(), QByteArrayLiteral("foreground = not-a-color\n"),
        fixture.candidates());
    QVERIFY(!malformed.succeeded());
    QCOMPARE(malformed.errorMessage,
             QStringLiteral("Invalid foreground in Ghostty config output at line 1"));

    const GhosttyConfigLoadResult malformedPalette =
        parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("palette = 256=#abcdef\n"),
            fixture.candidates());
    QVERIFY(!malformedPalette.succeeded());
    QCOMPARE(malformedPalette.errorMessage,
             QStringLiteral("Invalid palette in Ghostty config output at line 1"));

    const GhosttyConfigLoadResult malformedCursor =
        parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("cursor-style = beam\n"),
            fixture.candidates());
    QVERIFY(!malformedCursor.succeeded());
    QCOMPARE(malformedCursor.errorMessage,
             QStringLiteral("Invalid cursor-style in Ghostty config output at line 1"));

    // Ghostty accepts cursor opacity outside the nominal interval and clamps
    // it in the renderer rather than the config finalizer. Preserve that
    // canonical value here; LaunchOptions performs the renderer-side clamp.
    const GhosttyConfigLoadResult unboundedCursorOpacity =
        parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("cursor-opacity = 2\n"),
            fixture.candidates());
    QVERIFY(unboundedCursorOpacity.succeeded());
    QCOMPARE(unboundedCursorOpacity.snapshot->values
                 .value(QStringLiteral("cursor-opacity")).toDouble(),
             2.0);

    const GhosttyConfigLoadResult malformedOpacity =
        parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("faint-opacity = 1.1\n"),
            fixture.candidates());
    QVERIFY(!malformedOpacity.succeeded());
    QCOMPARE(malformedOpacity.errorMessage,
             QStringLiteral("Invalid faint-opacity in Ghostty config output at line 1"));

    const GhosttyConfigLoadResult missing = parseGhosttyConfigShowOutputs(
        QByteArrayLiteral("font-size = 13\n"), {}, fixture.candidates());
    QVERIFY(!missing.succeeded());
    QCOMPARE(missing.errorMessage,
             QStringLiteral("Ghostty default config output is missing a required compatibility key"));

    const GhosttyConfigLoadResult maximum = parseGhosttyConfigShowOutputs(
        defaultOutput(),
        QByteArrayLiteral("scrollback-limit = 18446744073709551615\n"),
        fixture.candidates());
    QVERIFY(maximum.succeeded());
    QCOMPARE(maximum.snapshot->values.value(QStringLiteral("scrollback-limit"))
                 .toULongLong(),
             std::numeric_limits<quint64>::max());
}

void GhosttyConfigProcessLoaderTest::invokesValidationThenDefaultAndCurrentQueries()
{
    ConfigFixture fixture;
    const QString invocationLog =
        QDir(fixture.temporary.path()).filePath(QStringLiteral("invocations"));
    GhosttyConfigProcessLoaderOptions options = fakeOptions(fixture);
    QVERIFY2(!options.helperPath.isEmpty(),
             "CMake must provide GHOSTTY_QT_FAKE_CONFIG_HELPER_PATH");
    options.environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_INVOCATION_LOG"),
                               invocationLog);

    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.candidates());
    QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
    QCOMPARE(result.snapshot->values.value(QStringLiteral("font-size")).toDouble(),
             17.25);

    QFile log(invocationLog);
    QVERIFY(log.open(QIODevice::ReadOnly));
    QCOMPARE(log.readAll(),
             QByteArrayLiteral("+validate-config\n"
                               "+show-config --default\n"
                               "+show-config\n"
                               "+validate-config\n"));
}

void GhosttyConfigProcessLoaderTest::rejectsConfigThatBecomesInvalidDuringQueries()
{
    ConfigFixture fixture;
    const QString invocationLog =
        QDir(fixture.temporary.path()).filePath(QStringLiteral("invocations"));
    GhosttyConfigProcessLoaderOptions options =
        fakeOptions(fixture, QStringLiteral("post-validation-failure"));
    options.environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_INVOCATION_LOG"),
                               invocationLog);

    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.candidates());
    QVERIFY(!result.succeeded());
    QCOMPARE(result.errorMessage,
             QStringLiteral(
                 "Ghostty config helper failed during post-query validation with exit code 1: "
                 "stdout: config changed during query"));
}

void GhosttyConfigProcessLoaderTest::preservesSuccessfulHelperWarnings()
{
    ConfigFixture fixture;
    GhosttyConfigProcessLoaderOptions options = fakeOptions(fixture);
    options.environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_SUCCESS_WARNING"),
                               QStringLiteral("both standard files exist\n"));

    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.candidates());
    QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
    QCOMPARE(result.snapshot->diagnostics.size(), 1);
    QCOMPARE(result.snapshot->diagnostics.constFirst().severity,
             GhosttyConfigDiagnosticSeverity::Warning);
    QCOMPARE(result.snapshot->diagnostics.constFirst().message,
             QStringLiteral(
                 "Ghostty config helper current query: both standard files exist"));
}

void GhosttyConfigProcessLoaderTest::realHelperPreservesAppearanceAndEffectiveUnbindSemantics()
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
            "palette = 42=#123456\n"
            "selection-foreground = cell-background\n"
            "selection-background = #334455\n"
            "cursor-color = #abcdef\n"
            "cursor-opacity = 0.4\n"
            "cursor-style = block_hollow\n"
            "cursor-style-blink = false\n"
            "cursor-text = cell-foreground\n"
            "bold-color = bright\n"
            "faint-opacity = 0.25\n"
            "keybind = clear\n"
            "keybind = ctrl+a=ignore\n"
            "keybind = ctrl+a=unbind\n"
            "keybind = ctrl+b=new_tab\n"));

    GhosttyConfigProcessLoaderOptions options{
        .helperPath = helperPath,
        .timeoutMilliseconds = 10'000,
        .environment = QProcessEnvironment::systemEnvironment(),
    };
    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.candidates());
    QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
    const QStringList keybinds =
        result.snapshot->values.value(QStringLiteral("keybind")).toStringList();
    QCOMPARE(keybinds, QStringList({QStringLiteral("ctrl+b=new_tab")}));
    const QVariantList palette =
        result.snapshot->values.value(QStringLiteral("palette")).toList();
    QCOMPARE(palette.size(), 256);
    QCOMPARE(palette.at(42).value<QColor>(), QColor(QStringLiteral("#123456")));
    QCOMPARE(result.snapshot->values.value(
                 QStringLiteral("selection-foreground")).toString(),
             QStringLiteral("cell-background"));
    QCOMPARE(result.snapshot->values.value(
                 QStringLiteral("selection-background")).value<QColor>(),
             QColor(QStringLiteral("#334455")));
    QCOMPARE(result.snapshot->values.value(QStringLiteral("cursor-color"))
                 .value<QColor>(),
             QColor(QStringLiteral("#abcdef")));
    QCOMPARE(result.snapshot->values.value(QStringLiteral("cursor-opacity"))
                 .toDouble(),
             0.4);
    QCOMPARE(result.snapshot->values.value(QStringLiteral("cursor-style"))
                 .toString(),
             QStringLiteral("block_hollow"));
    QCOMPARE(result.snapshot->values.value(QStringLiteral("cursor-style-blink"))
                 .toBool(),
             false);
    QCOMPARE(result.snapshot->values.value(QStringLiteral("cursor-text"))
                 .toString(),
             QStringLiteral("cell-foreground"));
    QCOMPARE(result.snapshot->values.value(QStringLiteral("bold-color"))
                 .toString(),
             QStringLiteral("bright"));
    QCOMPARE(result.snapshot->values.value(QStringLiteral("faint-opacity"))
                 .toDouble(),
             0.25);
}

void GhosttyConfigProcessLoaderTest::reportsValidationFailureDeterministically()
{
    ConfigFixture fixture;
    GhosttyConfigProcessLoaderOptions options =
        fakeOptions(fixture, QStringLiteral("validation-failure"));
    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.candidates());
    QVERIFY(!result.succeeded());
    QCOMPARE(result.errorMessage,
             QStringLiteral(
                 "Ghostty config helper failed during validation with exit code 1: "
                 "stdout: config.ghostty:2:1: invalid value"));
}

void GhosttyConfigProcessLoaderTest::reportsTimeoutCrashAndStartFailureDeterministically()
{
    ConfigFixture fixture;
    GhosttyConfigProcessLoaderOptions timeout =
        fakeOptions(fixture, QStringLiteral("validation-timeout"));
    timeout.timeoutMilliseconds = 25;
    GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(timeout)(fixture.candidates());
    QVERIFY(!result.succeeded());
    QCOMPARE(result.errorMessage,
             QStringLiteral(
                 "Ghostty config helper timed out during validation after 25 ms"));

    GhosttyConfigProcessLoaderOptions crash =
        fakeOptions(fixture, QStringLiteral("validation-crash"));
    result = makeGhosttyConfigProcessLoader(crash)(fixture.candidates());
    QVERIFY(!result.succeeded());
    QCOMPARE(result.errorMessage,
             QStringLiteral("Ghostty config helper crashed during validation"));

    GhosttyConfigProcessLoaderOptions missing = fakeOptions(fixture);
    missing.helperPath = QDir(fixture.temporary.path())
                             .filePath(QStringLiteral("does-not-exist"));
    missing.timeoutMilliseconds = 100;
    result = makeGhosttyConfigProcessLoader(missing)(fixture.candidates());
    QVERIFY(!result.succeeded());
    QCOMPARE(result.errorMessage,
             QStringLiteral(
                 "Ghostty config helper could not be started during validation"));
}

QTEST_GUILESS_MAIN(GhosttyConfigProcessLoaderTest)

#include "test_ghostty_config_process_loader.moc"
