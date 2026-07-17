#include "launch_options.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <limits>

namespace {

QVariantList testPalette()
{
    QVariantList palette;
    palette.reserve(256);
    for (int index = 0; index < 256; ++index) {
        palette.append(QColor::fromRgb(index, 255 - index, index / 2));
    }
    return palette;
}

} // namespace

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
    void restoresNullableAppearanceDefaults();
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
    QCOMPARE(options.appearance.foregroundColor,
             QColor(QStringLiteral("#d8dee9")));
    QCOMPARE(options.appearance.backgroundColor,
             QColor(QStringLiteral("#1e222a")));
    QVERIFY(options.appearance.palette.isEmpty());
    QCOMPARE(options.appearance.cursorColor.kind, TerminalColorKind::Unset);
    QCOMPARE(options.appearance.cursorStyle, TerminalCursorStyle::Block);
    QVERIFY(!options.appearance.cursorBlink.has_value());
    QCOMPARE(options.appearance.cursorOpacity, 1.0);
    QCOMPARE(options.appearance.faintOpacity, 0.5);
    QCOMPARE(options.scrollbackLimit.value, quint64(10'000));
    QCOMPARE(options.scrollbackLimit.unit, ScrollbackLimitUnit::Lines);
    QVERIFY(!options.scrollbackLimitExplicit);
    QCOMPARE(options.confirmCloseMode, ConfirmCloseMode::RunningProcesses);
    QVERIFY(options.linkUrl);
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
    snapshot.values.insert(QStringLiteral("palette"), testPalette());
    snapshot.values.insert(QStringLiteral("selection-foreground"),
                           QStringLiteral("cell-foreground"));
    snapshot.values.insert(QStringLiteral("selection-background"),
                           QColor(QStringLiteral("#223344")));
    snapshot.values.insert(QStringLiteral("cursor-color"),
                           QStringLiteral("cell-background"));
    snapshot.values.insert(QStringLiteral("cursor-opacity"), 0.625);
    snapshot.values.insert(QStringLiteral("cursor-style"),
                           QStringLiteral("block_hollow"));
    snapshot.values.insert(QStringLiteral("cursor-style-blink"), false);
    snapshot.values.insert(QStringLiteral("cursor-text"),
                           QStringLiteral("cell-foreground"));
    snapshot.values.insert(QStringLiteral("bold-color"),
                           QColor(QStringLiteral("#abcdef")));
    snapshot.values.insert(QStringLiteral("faint-opacity"), 0.375);
    snapshot.values.insert(QStringLiteral("scrollback-limit"), qint64(50'000'000));
    snapshot.values.insert(QStringLiteral("confirm-close-surface"),
                           QStringLiteral("always"));
    snapshot.values.insert(QStringLiteral("link-url"), false);
    snapshot.values.insert(
        QStringLiteral("keybind"),
        QStringList({QStringLiteral("alt+n=new_tab")}));
    snapshot.keybindConfig = GhosttyKeybindConfig{
        .schemaVersion = 1,
        .root = {GhosttyKeybindDefinition{
            .sequence = {GhosttyKeybindTrigger{
                .kind = GhosttyKeybindKeyKind::Unicode,
                .unicodeCodepoint = quint32('n'),
                .modifiers = GhosttyKeybindAlt,
            }},
            .actions = {QStringLiteral("new_tab")},
        }},
    };

    const LaunchOptions cliResult = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(cliResult.fontFamily, QStringLiteral("CLI Family"));
    QCOMPARE(cliResult.fontSize, 17.0);
    QCOMPARE(cliResult.appearance.foregroundColor,
             QColor(QStringLiteral("#112233")));
    QCOMPARE(cliResult.appearance.backgroundColor,
             QColor(QStringLiteral("#445566")));
    QCOMPARE(cliResult.appearance.palette.size(), 256);
    QCOMPARE(cliResult.appearance.palette.at(42),
             QColor::fromRgb(42, 213, 21));
    QCOMPARE(cliResult.appearance.selectionForeground.kind,
             TerminalColorKind::CellForeground);
    QCOMPARE(cliResult.appearance.selectionBackground.kind,
             TerminalColorKind::Color);
    QCOMPARE(cliResult.appearance.selectionBackground.color,
             QColor(QStringLiteral("#223344")));
    QCOMPARE(cliResult.appearance.cursorColor.kind,
             TerminalColorKind::CellBackground);
    QCOMPARE(cliResult.appearance.cursorStyle,
             TerminalCursorStyle::BlockHollow);
    QCOMPARE(cliResult.appearance.cursorBlink, std::optional<bool>(false));
    QCOMPARE(cliResult.appearance.cursorOpacity, 0.625);
    QCOMPARE(cliResult.appearance.cursorTextColor.kind,
             TerminalColorKind::CellForeground);
    QCOMPARE(cliResult.appearance.boldColor.kind,
             TerminalBoldColorKind::Color);
    QCOMPARE(cliResult.appearance.boldColor.color,
             QColor(QStringLiteral("#abcdef")));
    QCOMPARE(cliResult.appearance.faintOpacity, 0.375);
    QCOMPARE(cliResult.scrollbackLimit.value, quint64(25'000));
    QCOMPARE(cliResult.scrollbackLimit.unit, ScrollbackLimitUnit::Lines);
    QCOMPARE(cliResult.confirmCloseMode, ConfirmCloseMode::Always);
    QVERIFY(!cliResult.linkUrl);
    QVERIFY(cliResult.keybindings.isEmpty());
    QVERIFY(cliResult.keybindingsConfigured);
    QCOMPARE(cliResult.keybindConfig, *snapshot.keybindConfig);

    base.fontFamilyExplicit = false;
    base.fontSizeExplicit = false;
    base.scrollbackLimitExplicit = false;
    const LaunchOptions configResult = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(configResult.fontFamily, QStringLiteral("Config Primary"));
    QCOMPARE(configResult.fontSize, 14.5);
    QCOMPARE(configResult.scrollbackLimit.value, quint64(50'000'000));
    QCOMPARE(configResult.scrollbackLimit.unit, ScrollbackLimitUnit::Bytes);
}

void LaunchOptionsTest::restoresNullableAppearanceDefaults()
{
    LaunchOptions base;
    base.appearance.selectionForeground =
        TerminalColorValue::fromColor(QColor(Qt::red));
    base.appearance.selectionBackground = {
        .kind = TerminalColorKind::CellForeground,
    };
    base.appearance.cursorColor =
        TerminalColorValue::fromColor(QColor(Qt::green));
    base.appearance.cursorBlink = true;
    base.appearance.cursorTextColor = {
        .kind = TerminalColorKind::CellBackground,
    };
    base.appearance.boldColor = {
        .kind = TerminalBoldColorKind::Bright,
    };

    GhosttyConfigSnapshot snapshot;
    snapshot.availability = GhosttyConfigAvailability::Available;
    snapshot.values.insert(QStringLiteral("selection-foreground"), QString());
    snapshot.values.insert(QStringLiteral("selection-background"), QString());
    snapshot.values.insert(QStringLiteral("cursor-color"), QString());
    snapshot.values.insert(QStringLiteral("cursor-style-blink"), QString());
    snapshot.values.insert(QStringLiteral("cursor-text"), QString());
    snapshot.values.insert(QStringLiteral("bold-color"), QString());

    const TerminalAppearance appearance =
        applyGhosttyConfigSnapshot(base, snapshot).appearance;
    QCOMPARE(appearance.selectionForeground.kind, TerminalColorKind::Unset);
    QCOMPARE(appearance.selectionBackground.kind, TerminalColorKind::Unset);
    QCOMPARE(appearance.cursorColor.kind, TerminalColorKind::Unset);
    QVERIFY(!appearance.cursorBlink.has_value());
    QCOMPARE(appearance.cursorTextColor.kind, TerminalColorKind::Unset);
    QCOMPARE(appearance.boldColor.kind, TerminalBoldColorKind::Unset);
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
    snapshot.values.insert(QStringLiteral("palette"), QVariantList{QColor(Qt::red)});
    snapshot.values.insert(QStringLiteral("selection-background"),
                           QStringLiteral("not-an-alias"));
    snapshot.values.insert(QStringLiteral("cursor-opacity"), 2.0);
    snapshot.values.insert(QStringLiteral("cursor-style"),
                           QStringLiteral("beam"));
    snapshot.values.insert(QStringLiteral("cursor-style-blink"),
                           QStringLiteral("sometimes"));
    snapshot.values.insert(QStringLiteral("bold-color"),
                           QStringLiteral("dim"));
    snapshot.values.insert(QStringLiteral("faint-opacity"), -0.5);
    snapshot.values.insert(QStringLiteral("scrollback-limit"), qint64(-1));
    snapshot.values.insert(QStringLiteral("confirm-close-surface"),
                           QStringLiteral("sometimes"));
    snapshot.values.insert(QStringLiteral("link-url"),
                           QStringLiteral("false"));
    QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot).fontFamily, base.fontFamily);

    snapshot.availability = GhosttyConfigAvailability::Available;
    const LaunchOptions result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.fontSize, base.fontSize);
    QCOMPARE(result.appearance, base.appearance);
    QCOMPARE(result.scrollbackLimit, base.scrollbackLimit);
    QCOMPARE(result.confirmCloseMode, base.confirmCloseMode);
    QCOMPARE(result.linkUrl, base.linkUrl);
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
