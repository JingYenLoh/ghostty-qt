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

constexpr auto DefaultOutput =
    "font-family = \n"
    "font-size = 13\n"
    "foreground = #ffffff\n"
    "background = #282c34\n"
    "cursor-color = \n"
    "scrollback-limit = 50000000\n"
    "confirm-close-surface = true\n"
    "keybind = ctrl+shift+t=new_tab\n"
    "config-file = \n";

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
                       QString::fromLatin1(DefaultOutput));
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
    void realHelperPreservesEffectiveClearAndUnbindSemantics();
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
            "cursor-color = cell-background\r\n"
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
        parseGhosttyConfigShowOutputs(DefaultOutput, changes,
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
    QCOMPARE(snapshot.values.value(QStringLiteral("cursor-color")).toString(),
             QStringLiteral("cell-background"));
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
        QByteArrayLiteral("font-family = Monospace\n") + DefaultOutput;
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
        DefaultOutput, QByteArrayLiteral("foreground = not-a-color\n"),
        fixture.candidates());
    QVERIFY(!malformed.succeeded());
    QCOMPARE(malformed.errorMessage,
             QStringLiteral("Invalid foreground in Ghostty config output at line 1"));

    const GhosttyConfigLoadResult missing = parseGhosttyConfigShowOutputs(
        QByteArrayLiteral("font-size = 13\n"), {}, fixture.candidates());
    QVERIFY(!missing.succeeded());
    QCOMPARE(missing.errorMessage,
             QStringLiteral("Ghostty default config output is missing a required compatibility key"));

    const GhosttyConfigLoadResult maximum = parseGhosttyConfigShowOutputs(
        DefaultOutput,
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

void GhosttyConfigProcessLoaderTest::realHelperPreservesEffectiveClearAndUnbindSemantics()
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
