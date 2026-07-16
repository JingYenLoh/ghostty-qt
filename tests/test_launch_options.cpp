#include "launch_options.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <limits>

class LaunchOptionsTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void defaults();
    void parsesEveryOptionAndProgramArguments();
    void rejectsInvalidWorkingDirectory();
    void rejectsFileAsWorkingDirectory();
    void rejectsInvalidFontSize_data();
    void rejectsInvalidFontSize();
    void rejectsInvalidScrollbackLines_data();
    void rejectsInvalidScrollbackLines();
    void rejectsUnknownOption();
    void preservesOutputOnFailure();
    void overlaysGhosttySnapshotAndPreservesCliFonts();
    void ignoresUnavailableAndMalformedSnapshotValues();
    void convertsLegacyLineCapacityToLibghosttyBytes();
    void mapsCloseConfirmationModes();
};

void LaunchOptionsTest::defaults()
{
    LaunchOptions options;
    QString error;

    QVERIFY2(parseLaunchOptions({QStringLiteral("ghostty-qt")}, &options, &error),
             qPrintable(error));
    QCOMPARE(options.workingDirectory, QDir::currentPath());
    QVERIFY(options.fontFamily.isEmpty());
    QCOMPARE(options.fontSize, 12.0);
    QVERIFY(!options.fontFamilyExplicit);
    QVERIFY(!options.fontSizeExplicit);
    QCOMPARE(options.foregroundColor, QColor(QStringLiteral("#d8dee9")));
    QCOMPARE(options.backgroundColor, QColor(QStringLiteral("#1e222a")));
    QVERIFY(!options.cursorColor.isValid());
    QCOMPARE(options.scrollbackLimit.value, quint64(10'000));
    QCOMPARE(options.scrollbackLimit.unit, ScrollbackLimitUnit::Lines);
    QVERIFY(!options.scrollbackLimitExplicit);
    QCOMPARE(options.confirmCloseMode, ConfirmCloseMode::RunningProcesses);
    QVERIFY(!options.hold);
    QVERIFY(options.program.isEmpty());
    QVERIFY(error.isEmpty());
}

void LaunchOptionsTest::parsesEveryOptionAndProgramArguments()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    LaunchOptions options;
    QString error;
    const QStringList arguments{
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--working-directory"),
        directory.path(),
        QStringLiteral("--font-family"),
        QStringLiteral("Iosevka Term"),
        QStringLiteral("--font-size=15.5"),
        QStringLiteral("--scrollback-lines"),
        QStringLiteral("250000"),
        QStringLiteral("--hold"),
        QStringLiteral("--"),
        QStringLiteral("/bin/sh"),
        QStringLiteral("-lc"),
        QStringLiteral("printf hello"),
    };

    QVERIFY2(parseLaunchOptions(arguments, &options, &error), qPrintable(error));
    QCOMPARE(options.workingDirectory, QDir::cleanPath(directory.path()));
    QCOMPARE(options.fontFamily, QStringLiteral("Iosevka Term"));
    QCOMPARE(options.fontSize, 15.5);
    QVERIFY(options.fontFamilyExplicit);
    QVERIFY(options.fontSizeExplicit);
    QCOMPARE(options.scrollbackLimit.value, quint64(250'000));
    QCOMPARE(options.scrollbackLimit.unit, ScrollbackLimitUnit::Lines);
    QVERIFY(options.scrollbackLimitExplicit);
    QVERIFY(options.hold);
    QCOMPARE(options.program,
             QStringList({QStringLiteral("/bin/sh"), QStringLiteral("-lc"),
                          QStringLiteral("printf hello")}));
}

void LaunchOptionsTest::rejectsInvalidWorkingDirectory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString missingPath = directory.filePath(QStringLiteral("missing"));

    LaunchOptions options;
    QString error;
    QVERIFY(!parseLaunchOptions({QStringLiteral("ghostty-qt"),
                                 QStringLiteral("--working-directory"), missingPath},
                                &options, &error));
    QVERIFY(error.contains(QStringLiteral("does not exist or is not a directory")));
}

void LaunchOptionsTest::rejectsFileAsWorkingDirectory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString filePath = directory.filePath(QStringLiteral("regular-file"));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    LaunchOptions options;
    QString error;
    QVERIFY(!parseLaunchOptions({QStringLiteral("ghostty-qt"),
                                 QStringLiteral("--working-directory"), filePath},
                                &options, &error));
    QVERIFY(error.contains(QStringLiteral("does not exist or is not a directory")));
}

void LaunchOptionsTest::rejectsInvalidFontSize_data()
{
    QTest::addColumn<QString>("value");

    QTest::newRow("zero") << QStringLiteral("0");
    QTest::newRow("negative") << QStringLiteral("-2");
    QTest::newRow("not-a-number") << QStringLiteral("large");
    QTest::newRow("infinity") << QStringLiteral("inf");
}

void LaunchOptionsTest::rejectsInvalidFontSize()
{
    QFETCH(QString, value);

    LaunchOptions options;
    QString error;
    QVERIFY(!parseLaunchOptions({QStringLiteral("ghostty-qt"), QStringLiteral("--font-size"),
                                 value},
                                &options, &error));
    QVERIFY(error.contains(QStringLiteral("Invalid font size")));
}

void LaunchOptionsTest::rejectsInvalidScrollbackLines_data()
{
    QTest::addColumn<QString>("value");

    QTest::newRow("negative") << QStringLiteral("-1");
    QTest::newRow("too-large") << QStringLiteral("10000001");
    QTest::newRow("fractional") << QStringLiteral("1.5");
    QTest::newRow("not-a-number") << QStringLiteral("many");
}

void LaunchOptionsTest::rejectsInvalidScrollbackLines()
{
    QFETCH(QString, value);

    LaunchOptions options;
    QString error;
    QVERIFY(!parseLaunchOptions({QStringLiteral("ghostty-qt"),
                                 QStringLiteral("--scrollback-lines"), value},
                                &options, &error));
    QVERIFY(error.contains(QStringLiteral("Invalid scrollback line count")));
}

void LaunchOptionsTest::rejectsUnknownOption()
{
    LaunchOptions options;
    QString error;
    QVERIFY(!parseLaunchOptions(
        {QStringLiteral("ghostty-qt"), QStringLiteral("--not-an-option")}, &options, &error));
    QVERIFY(!error.isEmpty());
}

void LaunchOptionsTest::preservesOutputOnFailure()
{
    LaunchOptions options;
    options.fontFamily = QStringLiteral("sentinel");
    options.fontSize = 42.0;

    QVERIFY(!parseLaunchOptions(
        {QStringLiteral("ghostty-qt"), QStringLiteral("--font-size=0")}, &options));
    QCOMPARE(options.fontFamily, QStringLiteral("sentinel"));
    QCOMPARE(options.fontSize, 42.0);
}

void LaunchOptionsTest::overlaysGhosttySnapshotAndPreservesCliFonts()
{
    LaunchOptions base;
    base.fontFamily = QStringLiteral("CLI Family");
    base.fontSize = 17.0;
    base.fontFamilyExplicit = true;
    base.fontSizeExplicit = true;
    base.scrollbackLimit = {.value = 25'000,
                            .unit = ScrollbackLimitUnit::Lines};
    base.scrollbackLimitExplicit = true;

    GhosttyConfigSnapshot snapshot;
    snapshot.availability = GhosttyConfigAvailability::Available;
    snapshot.values.insert(
        QStringLiteral("font-family"),
        QStringList({QStringLiteral("Config Primary"),
                     QStringLiteral("Config Fallback")}));
    snapshot.values.insert(QStringLiteral("font-size"), 14.5);
    snapshot.values.insert(QStringLiteral("foreground"), QColor(QStringLiteral("#112233")));
    snapshot.values.insert(QStringLiteral("background"), QColor(QStringLiteral("#445566")));
    snapshot.values.insert(QStringLiteral("cursor-color"),
                           QStringLiteral("cell-background"));
    snapshot.values.insert(QStringLiteral("scrollback-limit"), qint64(50'000'000));
    snapshot.values.insert(QStringLiteral("confirm-close-surface"),
                           QStringLiteral("always"));
    snapshot.values.insert(
        QStringLiteral("keybind"),
        QStringList({QStringLiteral("alt+n=new_tab")}));

    const LaunchOptions cliResult = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(cliResult.fontFamily, QStringLiteral("CLI Family"));
    QCOMPARE(cliResult.fontSize, 17.0);
    QCOMPARE(cliResult.foregroundColor, QColor(QStringLiteral("#112233")));
    QCOMPARE(cliResult.backgroundColor, QColor(QStringLiteral("#445566")));
    QCOMPARE(cliResult.cursorColor, QColor(QStringLiteral("#445566")));
    QCOMPARE(cliResult.scrollbackLimit.value, quint64(25'000));
    QCOMPARE(cliResult.scrollbackLimit.unit, ScrollbackLimitUnit::Lines);
    QCOMPARE(cliResult.confirmCloseMode, ConfirmCloseMode::Always);
    QCOMPARE(cliResult.keybindings,
             QStringList({QStringLiteral("alt+n=new_tab")}));
    QVERIFY(cliResult.keybindingsConfigured);

    base.fontFamilyExplicit = false;
    base.fontSizeExplicit = false;
    base.scrollbackLimitExplicit = false;
    const LaunchOptions configResult = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(configResult.fontFamily, QStringLiteral("Config Primary"));
    QCOMPARE(configResult.fontSize, 14.5);
    QCOMPARE(configResult.scrollbackLimit.value, quint64(50'000'000));
    QCOMPARE(configResult.scrollbackLimit.unit, ScrollbackLimitUnit::Bytes);
}

void LaunchOptionsTest::ignoresUnavailableAndMalformedSnapshotValues()
{
    LaunchOptions base;
    base.fontFamily = QStringLiteral("Base Family");
    base.fontSize = 13.0;
    base.scrollbackLimit = {.value = 900, .unit = ScrollbackLimitUnit::Lines};

    GhosttyConfigSnapshot snapshot;
    snapshot.values.insert(QStringLiteral("font-size"), -2.0);
    snapshot.values.insert(QStringLiteral("foreground"), QStringLiteral("not-a-color"));
    snapshot.values.insert(QStringLiteral("scrollback-limit"), qint64(-1));
    snapshot.values.insert(QStringLiteral("confirm-close-surface"),
                           QStringLiteral("sometimes"));
    QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot).fontFamily, base.fontFamily);

    snapshot.availability = GhosttyConfigAvailability::Available;
    const LaunchOptions result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.fontSize, base.fontSize);
    QCOMPARE(result.foregroundColor, base.foregroundColor);
    QCOMPARE(result.scrollbackLimit, base.scrollbackLimit);
    QCOMPARE(result.confirmCloseMode, base.confirmCloseMode);
}

void LaunchOptionsTest::convertsLegacyLineCapacityToLibghosttyBytes()
{
    QCOMPARE(scrollbackLimitInBytes(
                 {.value = 10'000, .unit = ScrollbackLimitUnit::Lines}, 80),
             quint64(12'800'000));
    QCOMPARE(scrollbackLimitInBytes(
                 {.value = 123, .unit = ScrollbackLimitUnit::Bytes}, 240),
             quint64(123));
    QCOMPARE(scrollbackLimitInBytes(
                 {.value = std::numeric_limits<quint64>::max(),
                  .unit = ScrollbackLimitUnit::Lines},
                 80),
             std::numeric_limits<quint64>::max());
}

void LaunchOptionsTest::mapsCloseConfirmationModes()
{
    QVERIFY(!shouldConfirmClose(ConfirmCloseMode::Never, false, false));
    QVERIFY(!shouldConfirmClose(ConfirmCloseMode::Never, true, true));
    QVERIFY(!shouldConfirmClose(ConfirmCloseMode::RunningProcesses,
                                false, false));
    QVERIFY(!shouldConfirmClose(ConfirmCloseMode::RunningProcesses,
                                true, false));
    QVERIFY(shouldConfirmClose(ConfirmCloseMode::RunningProcesses,
                               true, true));
    QVERIFY(!shouldConfirmClose(ConfirmCloseMode::Always, false, false));
    QVERIFY(shouldConfirmClose(ConfirmCloseMode::Always, true, false));
    QVERIFY(shouldConfirmClose(ConfirmCloseMode::Always, true, true));
}

QTEST_APPLESS_MAIN(LaunchOptionsTest)

#include "test_launch_options.moc"
