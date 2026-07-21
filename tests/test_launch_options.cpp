#include "launch_options.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <limits>

namespace {

QString errorMessage(
    const std::expected<LaunchOptions, QString> &result)
{
    return result ? QString{} : result.error();
}

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
    void parsesActivationBootstrapOptions();
    void parsesEveryOptionAndProgramArguments();
    void preservesSymlinkSensitiveExplicitWorkingDirectory();
    void rejectsInvalidWorkingDirectory();
    void rejectsFileAsWorkingDirectory();
    void rejectsInvalidFontSize_data();
    void rejectsInvalidFontSize();
    void rejectsInvalidScrollbackLines_data();
    void rejectsInvalidScrollbackLines();
    void rejectsUnknownOption();
    void rejectsMissingApplicationName();
    void overlaysGhosttySnapshotAndPreservesCliFonts();
    void mapsLinkPreviewModes();
    void mapsLinkPreviewModes_data();
    void mapsClipboardModes();
    void mapsWorkingDirectoryAndSurfaceInheritance();
    void mapsSplitPreserveZoom();
    void mapsNewTabPosition();
    void mapsWindowShowTabBar();
    void mapsStartupWindowState();
    void mapsApplicationLifetime();
    void mapsSingleInstancePolicy();
    void mapsUnfocusedSplitAppearance();
    void restoresNullableAppearanceDefaults();
    void ignoresUnavailableAndMalformedSnapshotValues();
    void projectsTerminalSessionOptions();
    void convertsLegacyLineCapacityToLibghosttyBytes();
    void mapsCloseConfirmationModes();
};

void LaunchOptionsTest::defaults()
{
    const auto result =
        parseLaunchOptions({QStringLiteral("ghostty-qt")});
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    const LaunchOptions &options = *result;
    QCOMPARE(options.workingDirectory, QDir::currentPath());
    QVERIFY(options.inheritWorkingDirectory);
    QVERIFY(!options.workingDirectoryExplicit);
    QVERIFY(options.fontFamily.isEmpty());
    QCOMPARE(options.fontSize, 12.0);
    QVERIFY(!options.fontFamilyExplicit);
    QVERIFY(!options.fontSizeExplicit);
    QCOMPARE(options.appearance.foregroundColor,
             QColor(QStringLiteral("#d8dee9")));
    QCOMPARE(options.appearance.backgroundColor,
             QColor(QStringLiteral("#1e222a")));
    QVERIFY(options.appearance.palette.isEmpty());
    QCOMPARE(options.appearance.searchForeground.kind,
             TerminalColorKind::Color);
    QCOMPARE(options.appearance.searchForeground.color,
             QColor(QStringLiteral("#000000")));
    QCOMPARE(options.appearance.searchBackground.kind,
             TerminalColorKind::Color);
    QCOMPARE(options.appearance.searchBackground.color,
             QColor(QStringLiteral("#FFE082")));
    QCOMPARE(options.appearance.searchSelectedForeground.kind,
             TerminalColorKind::Color);
    QCOMPARE(options.appearance.searchSelectedForeground.color,
             QColor(QStringLiteral("#000000")));
    QCOMPARE(options.appearance.searchSelectedBackground.kind,
             TerminalColorKind::Color);
    QCOMPARE(options.appearance.searchSelectedBackground.color,
             QColor(QStringLiteral("#F2A57E")));
    QCOMPARE(options.appearance.cursorColor.kind, TerminalColorKind::Unset);
    QCOMPARE(options.appearance.cursorStyle, TerminalCursorStyle::Block);
    QVERIFY(!options.appearance.cursorBlink.has_value());
    QCOMPARE(options.appearance.cursorOpacity, 1.0);
    QCOMPARE(options.appearance.faintOpacity, 0.5);
    QCOMPARE(options.scrollbackLimit.value, quint64(10'000));
    QCOMPARE(options.scrollbackLimit.unit, ScrollbackLimitUnit::Lines);
    QVERIFY(!options.scrollbackLimitExplicit);
    QCOMPARE(options.confirmCloseMode, ConfirmCloseMode::RunningProcesses);
    QVERIFY(options.selectionClipboard.trimTrailingSpaces);
    QCOMPARE(options.selectionClipboard.copyOnSelect,
             TerminalCopyOnSelectMode::Primary);
    QVERIFY(options.selectionClipboard.clearOnTyping);
    QVERIFY(!options.selectionClipboard.clearOnCopy);
    QVERIFY(options.clipboardPaste.protection);
    QVERIFY(options.clipboardPaste.bracketedSafe);
    QCOMPARE(options.splitAppearance.unfocusedOpacity, 0.7);
    QVERIFY(!options.splitAppearance.unfocusedFill.has_value());
    QVERIFY(!options.splitAppearance.dividerColor.has_value());
    QVERIFY(options.splitInheritWorkingDirectory);
    QVERIFY(!options.splitPreserveZoomNavigation);
    QVERIFY(options.tabInheritWorkingDirectory);
    QVERIFY(options.windowInheritWorkingDirectory);
    QVERIFY(options.windowInheritFontSize);
    QCOMPARE(options.windowNewTabPosition,
             WindowNewTabPosition::Current);
    QCOMPARE(options.windowShowTabBar, WindowShowTabBar::Auto);
    QVERIFY(!options.maximize);
    QVERIFY(!options.fullscreen);
    QVERIFY(options.quitAfterLastWindowClosed);
    QVERIFY(!options.quitAfterLastWindowClosedDelay.has_value());
    QVERIFY(options.initialWindow);
    QVERIFY(!options.initialWindowExplicit);
    QVERIFY(!options.hasUnforwardedLaunchPayload);
    QCOMPARE(options.singleInstanceMode, SingleInstanceMode::Detect);
    QVERIFY(!options.singleInstanceModeExplicit);
    QCOMPARE(options.middleClickAction, MiddleClickAction::PrimaryPaste);
    QVERIFY(options.linkUrl);
    QCOMPARE(options.linkPreviews, LinkPreviewMode::Always);
    QVERIFY(!options.hold);
    QVERIFY(options.program.isEmpty());
}

void LaunchOptionsTest::parsesActivationBootstrapOptions()
{
    const auto service = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--gtk-single-instance=true"),
        QStringLiteral("--initial-window=false"),
    });
    QVERIFY2(service.has_value(), qPrintable(errorMessage(service)));
    QCOMPARE(service->singleInstanceMode, SingleInstanceMode::Enabled);
    QVERIFY(service->singleInstanceModeExplicit);
    QVERIFY(!service->initialWindow);
    QVERIFY(service->initialWindowExplicit);
    QVERIFY(!service->hasUnforwardedLaunchPayload);
    QVERIFY(service->program.isEmpty());
    QVERIFY(shouldUseSingleInstance(*service, QByteArrayView("ghostty")));

    GhosttyConfigSnapshot contradictory;
    contradictory.availability = GhosttyConfigAvailability::Available;
    contradictory.values.insert(
        QStringLiteral("gtk-single-instance"), QStringLiteral("false"));
    contradictory.values.insert(QStringLiteral("initial-window"), true);
    QCOMPARE(applyGhosttyConfigSnapshot(*service, contradictory), *service);

    const auto detect = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--gtk-single-instance=detect"),
        QStringLiteral("--initial-window=T"),
    });
    QVERIFY2(detect.has_value(), qPrintable(errorMessage(detect)));
    QVERIFY(detect->initialWindow);
    QVERIFY(shouldUseSingleInstance(*detect, QByteArrayView{}));
    QVERIFY(!shouldUseSingleInstance(*detect, QByteArrayView("ghostty")));

    const auto disabled = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--gtk-single-instance=false"),
        QStringLiteral("--initial-window=0"),
    });
    QVERIFY2(disabled.has_value(), qPrintable(errorMessage(disabled)));
    QVERIFY(!shouldUseSingleInstance(*disabled, QByteArrayView{}));
    QVERIFY(!disabled->initialWindow);

    for (const QString &argument : {
             QStringLiteral("--gtk-single-instance=yes"),
             QStringLiteral("--initial-window=yes"),
         }) {
        const auto invalid = parseLaunchOptions(
            {QStringLiteral("ghostty-qt"), argument});
        QVERIFY(!invalid.has_value());
        QVERIFY(invalid.error().contains(QStringLiteral("Invalid")));
    }

    const auto payload = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--gtk-single-instance=true"),
        QStringLiteral("--font-size=14"),
    });
    QVERIFY2(payload.has_value(), qPrintable(errorMessage(payload)));
    QVERIFY(payload->hasUnforwardedLaunchPayload);
    QVERIFY(!shouldUseSingleInstance(*payload, QByteArrayView{}));
}

void LaunchOptionsTest::parsesEveryOptionAndProgramArguments()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

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

    const auto result = parseLaunchOptions(arguments);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    const LaunchOptions &options = *result;
    QCOMPARE(options.workingDirectory, QDir::cleanPath(directory.path()));
    QVERIFY(!options.inheritWorkingDirectory);
    QVERIFY(options.workingDirectoryExplicit);
    QCOMPARE(options.fontFamily, QStringLiteral("Iosevka Term"));
    QCOMPARE(options.fontSize, 15.5);
    QVERIFY(options.fontFamilyExplicit);
    QVERIFY(options.fontSizeExplicit);
    QCOMPARE(options.scrollbackLimit.value, quint64(250'000));
    QCOMPARE(options.scrollbackLimit.unit, ScrollbackLimitUnit::Lines);
    QVERIFY(options.scrollbackLimitExplicit);
    QVERIFY(options.hold);
    QVERIFY(options.hasUnforwardedLaunchPayload);
    QCOMPARE(options.program,
             QStringList({QStringLiteral("/bin/sh"), QStringLiteral("-lc"),
                          QStringLiteral("printf hello")}));
}

void LaunchOptionsTest::preservesSymlinkSensitiveExplicitWorkingDirectory()
{
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/explicit-working-directory-XXXXXX")));
    QVERIFY(directory.isValid());
    const QDir root(directory.path());
    const QString base = root.filePath(QStringLiteral("base"));
    const QString linkedDirectory =
        root.filePath(QStringLiteral("other/directory"));
    const QString actualTarget =
        root.filePath(QStringLiteral("other/target"));
    for (const QString &path : {base, linkedDirectory, actualTarget}) {
        QVERIFY(QDir().mkpath(path));
    }
    QVERIFY(QFile::link(linkedDirectory,
                        QDir(base).filePath(QStringLiteral("link"))));
    const QString requested =
        QDir(base).filePath(QStringLiteral("link/../target"));

    const auto result = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--working-directory"),
        requested,
    });
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->workingDirectory, requested);
    QVERIFY(!result->inheritWorkingDirectory);
    QVERIFY(result->workingDirectoryExplicit);
}

void LaunchOptionsTest::rejectsInvalidWorkingDirectory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString missingPath = directory.filePath(QStringLiteral("missing"));

    const auto result = parseLaunchOptions(
        {QStringLiteral("ghostty-qt"),
         QStringLiteral("--working-directory"), missingPath});
    QVERIFY(!result.has_value());
    QVERIFY(result.error().contains(
        QStringLiteral("does not exist or is not a directory")));
}

void LaunchOptionsTest::rejectsFileAsWorkingDirectory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString filePath = directory.filePath(QStringLiteral("regular-file"));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    const auto result = parseLaunchOptions(
        {QStringLiteral("ghostty-qt"),
         QStringLiteral("--working-directory"), filePath});
    QVERIFY(!result.has_value());
    QVERIFY(result.error().contains(
        QStringLiteral("does not exist or is not a directory")));
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

    const auto result = parseLaunchOptions(
        {QStringLiteral("ghostty-qt"), QStringLiteral("--font-size"),
         value});
    QVERIFY(!result.has_value());
    QVERIFY(result.error().contains(QStringLiteral("Invalid font size")));
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

    const auto result = parseLaunchOptions(
        {QStringLiteral("ghostty-qt"),
         QStringLiteral("--scrollback-lines"), value});
    QVERIFY(!result.has_value());
    QVERIFY(result.error().contains(
        QStringLiteral("Invalid scrollback line count")));
}

void LaunchOptionsTest::rejectsUnknownOption()
{
    const auto result = parseLaunchOptions(
        {QStringLiteral("ghostty-qt"), QStringLiteral("--not-an-option")});
    QVERIFY(!result.has_value());
    QVERIFY(!result.error().isEmpty());
}

void LaunchOptionsTest::rejectsMissingApplicationName()
{
    const auto result = parseLaunchOptions({});
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(),
             QStringLiteral("The argument list must include the application name."));
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
    snapshot.values.insert(QStringLiteral("unfocused-split-opacity"), 0.42);
    snapshot.values.insert(QStringLiteral("unfocused-split-fill"),
                           QColor(QStringLiteral("#778899")));
    snapshot.values.insert(QStringLiteral("split-divider-color"),
                           QColor(QStringLiteral("#a1b2c3")));
    snapshot.values.insert(QStringLiteral("palette"), testPalette());
    snapshot.values.insert(QStringLiteral("selection-foreground"),
                           QStringLiteral("cell-foreground"));
    snapshot.values.insert(QStringLiteral("selection-background"),
                           QColor(QStringLiteral("#223344")));
    snapshot.values.insert(QStringLiteral("search-foreground"),
                           QStringLiteral("cell-background"));
    snapshot.values.insert(QStringLiteral("search-background"),
                           QColor(QStringLiteral("#123456")));
    snapshot.values.insert(QStringLiteral("search-selected-foreground"),
                           QStringLiteral("cell-foreground"));
    snapshot.values.insert(QStringLiteral("search-selected-background"),
                           QColor(QStringLiteral("#654321")));
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
    snapshot.values.insert(QStringLiteral("clipboard-trim-trailing-spaces"),
                           false);
    snapshot.values.insert(QStringLiteral("clipboard-paste-protection"), false);
    snapshot.values.insert(QStringLiteral("clipboard-paste-bracketed-safe"),
                           true);
    snapshot.values.insert(QStringLiteral("copy-on-select"),
                           QStringLiteral("clipboard"));
    snapshot.values.insert(QStringLiteral("selection-clear-on-typing"), false);
    snapshot.values.insert(QStringLiteral("selection-clear-on-copy"), true);
    snapshot.values.insert(QStringLiteral("middle-click-action"),
                           QStringLiteral("ignore"));
    snapshot.values.insert(QStringLiteral("mouse-reporting"), false);
    snapshot.values.insert(QStringLiteral("link-url"), false);
    snapshot.values.insert(QStringLiteral("link-previews"),
                           QStringLiteral("osc8"));
    snapshot.values.insert(
        QStringLiteral("keybind"),
        QStringList({QStringLiteral("alt+n=new_tab")}));
    snapshot.keybindConfig = GhosttyKeybindConfig{
        .schemaVersion = GhosttyKeybindConfig::CurrentSchemaVersion,
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
    QCOMPARE(cliResult.splitAppearance.unfocusedOpacity, 0.42);
    QCOMPARE(cliResult.splitAppearance.unfocusedFill,
             std::optional<QColor>(QColor(QStringLiteral("#778899"))));
    QCOMPARE(cliResult.splitAppearance.dividerColor,
             std::optional<QColor>(QColor(QStringLiteral("#a1b2c3"))));
    QCOMPARE(cliResult.appearance.palette.size(), 256);
    QCOMPARE(cliResult.appearance.palette.at(42),
             QColor::fromRgb(42, 213, 21));
    QCOMPARE(cliResult.appearance.selectionForeground.kind,
             TerminalColorKind::CellForeground);
    QCOMPARE(cliResult.appearance.selectionBackground.kind,
             TerminalColorKind::Color);
    QCOMPARE(cliResult.appearance.selectionBackground.color,
             QColor(QStringLiteral("#223344")));
    QCOMPARE(cliResult.appearance.searchForeground.kind,
             TerminalColorKind::CellBackground);
    QCOMPARE(cliResult.appearance.searchBackground.kind,
             TerminalColorKind::Color);
    QCOMPARE(cliResult.appearance.searchBackground.color,
             QColor(QStringLiteral("#123456")));
    QCOMPARE(cliResult.appearance.searchSelectedForeground.kind,
             TerminalColorKind::CellForeground);
    QCOMPARE(cliResult.appearance.searchSelectedBackground.kind,
             TerminalColorKind::Color);
    QCOMPARE(cliResult.appearance.searchSelectedBackground.color,
             QColor(QStringLiteral("#654321")));
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
    QVERIFY(!cliResult.selectionClipboard.trimTrailingSpaces);
    QCOMPARE(cliResult.selectionClipboard.copyOnSelect,
             TerminalCopyOnSelectMode::PrimaryAndClipboard);
    QVERIFY(!cliResult.selectionClipboard.clearOnTyping);
    QVERIFY(cliResult.selectionClipboard.clearOnCopy);
    QVERIFY(!cliResult.clipboardPaste.protection);
    QVERIFY(cliResult.clipboardPaste.bracketedSafe);
    QCOMPARE(cliResult.middleClickAction, MiddleClickAction::Ignore);
    QVERIFY(!cliResult.mouseReporting);
    QVERIFY(!cliResult.linkUrl);
    QCOMPARE(cliResult.linkPreviews, LinkPreviewMode::Osc8);
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

void LaunchOptionsTest::mapsLinkPreviewModes_data()
{
    QTest::addColumn<QString>("canonical");
    QTest::addColumn<LinkPreviewMode>("expected");

    QTest::newRow("false") << QStringLiteral("false")
                            << LinkPreviewMode::Never;
    QTest::newRow("true") << QStringLiteral("true")
                           << LinkPreviewMode::Always;
    QTest::newRow("osc8") << QStringLiteral("osc8")
                           << LinkPreviewMode::Osc8;
}

void LaunchOptionsTest::mapsLinkPreviewModes()
{
    QFETCH(QString, canonical);
    QFETCH(LinkPreviewMode, expected);

    LaunchOptions base;
    base.linkPreviews = LinkPreviewMode::Never;
    GhosttyConfigSnapshot snapshot;
    snapshot.availability = GhosttyConfigAvailability::Available;
    snapshot.values.insert(QStringLiteral("link-previews"), canonical);

    QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot).linkPreviews, expected);
}

void LaunchOptionsTest::mapsClipboardModes()
{
    const struct {
        QString canonical;
        TerminalCopyOnSelectMode expected;
    } copyModes[] = {
        {QStringLiteral("false"), TerminalCopyOnSelectMode::Disabled},
        {QStringLiteral("true"), TerminalCopyOnSelectMode::Primary},
        {QStringLiteral("clipboard"),
         TerminalCopyOnSelectMode::PrimaryAndClipboard},
    };
    for (const auto &testCase : copyModes) {
        GhosttyConfigSnapshot snapshot;
        snapshot.availability = GhosttyConfigAvailability::Available;
        snapshot.values.insert(QStringLiteral("copy-on-select"),
                               testCase.canonical);
        QCOMPARE(applyGhosttyConfigSnapshot({}, snapshot)
                     .selectionClipboard.copyOnSelect,
                 testCase.expected);
    }

    const struct {
        QString canonical;
        MiddleClickAction expected;
    } middleClickActions[] = {
        {QStringLiteral("primary-paste"), MiddleClickAction::PrimaryPaste},
        {QStringLiteral("ignore"), MiddleClickAction::Ignore},
    };
    for (const auto &testCase : middleClickActions) {
        GhosttyConfigSnapshot snapshot;
        snapshot.availability = GhosttyConfigAvailability::Available;
        snapshot.values.insert(QStringLiteral("middle-click-action"),
                               testCase.canonical);
        QCOMPARE(applyGhosttyConfigSnapshot({}, snapshot).middleClickAction,
                 testCase.expected);
    }

    for (const bool enabled : {false, true}) {
        LaunchOptions base;
        base.mouseReporting = !enabled;
        GhosttyConfigSnapshot snapshot;
        snapshot.availability = GhosttyConfigAvailability::Available;
        snapshot.values.insert(QStringLiteral("mouse-reporting"), enabled);
        QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot).mouseReporting,
                 enabled);
    }
}

void LaunchOptionsTest::mapsWorkingDirectoryAndSurfaceInheritance()
{
    LaunchOptions base;
    base.workingDirectory = QStringLiteral("/launch-directory");
    base.splitInheritWorkingDirectory = true;
    base.tabInheritWorkingDirectory = true;
    base.windowInheritWorkingDirectory = true;
    base.windowInheritFontSize = true;

    GhosttyConfigSnapshot snapshot;
    snapshot.availability = GhosttyConfigAvailability::Available;
    snapshot.values.insert(QStringLiteral("working-directory"),
                           QStringLiteral("/base/link/../target"));
    snapshot.values.insert(
        QStringLiteral("split-inherit-working-directory"), false);
    snapshot.values.insert(
        QStringLiteral("tab-inherit-working-directory"), false);
    snapshot.values.insert(
        QStringLiteral("window-inherit-working-directory"), false);
    snapshot.values.insert(QStringLiteral("window-inherit-font-size"), false);

    LaunchOptions result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.workingDirectory,
             QStringLiteral("/base/link/../target"));
    QVERIFY(!result.inheritWorkingDirectory);
    QVERIFY(!result.splitInheritWorkingDirectory);
    QVERIFY(!result.tabInheritWorkingDirectory);
    QVERIFY(!result.windowInheritWorkingDirectory);
    QVERIFY(!result.windowInheritFontSize);

    base.workingDirectoryExplicit = true;
    result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.workingDirectory, QStringLiteral("/launch-directory"));
    QVERIFY(!result.inheritWorkingDirectory);
    QVERIFY(!result.splitInheritWorkingDirectory);
    QVERIFY(!result.tabInheritWorkingDirectory);
    QVERIFY(!result.windowInheritWorkingDirectory);
    QVERIFY(!result.windowInheritFontSize);

    base.workingDirectoryExplicit = false;
    for (const QString &fallback : {
             QString{}, QStringLiteral("inherit"),
         }) {
        snapshot.values.insert(QStringLiteral("working-directory"), fallback);
        result = applyGhosttyConfigSnapshot(base, snapshot);
        QCOMPARE(result.workingDirectory,
                 QStringLiteral("/launch-directory"));
        QVERIFY(result.inheritWorkingDirectory);
    }

    snapshot.values.insert(QStringLiteral("working-directory"), false);
    snapshot.values.insert(
        QStringLiteral("split-inherit-working-directory"),
        QStringLiteral("false"));
    snapshot.values.insert(
        QStringLiteral("tab-inherit-working-directory"),
        QStringLiteral("false"));
    snapshot.values.insert(
        QStringLiteral("window-inherit-working-directory"),
        QStringLiteral("false"));
    snapshot.values.insert(
        QStringLiteral("window-inherit-font-size"),
        QStringLiteral("false"));
    result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.workingDirectory, QStringLiteral("/launch-directory"));
    QVERIFY(!result.inheritWorkingDirectory);
    QVERIFY(result.splitInheritWorkingDirectory);
    QVERIFY(result.tabInheritWorkingDirectory);
    QVERIFY(result.windowInheritWorkingDirectory);
    QVERIFY(result.windowInheritFontSize);

    snapshot.availability = GhosttyConfigAvailability::Unavailable;
    QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot), base);
}

void LaunchOptionsTest::mapsSplitPreserveZoom()
{
    LaunchOptions base;
    base.splitPreserveZoomNavigation = false;

    GhosttyConfigSnapshot snapshot;
    snapshot.availability = GhosttyConfigAvailability::Available;
    snapshot.values.insert(QStringLiteral("split-preserve-zoom"), true);
    QVERIFY(applyGhosttyConfigSnapshot(base, snapshot)
                .splitPreserveZoomNavigation);

    snapshot.values.insert(QStringLiteral("split-preserve-zoom"), false);
    QVERIFY(!applyGhosttyConfigSnapshot(base, snapshot)
                 .splitPreserveZoomNavigation);

    base.splitPreserveZoomNavigation = true;
    snapshot.values.insert(QStringLiteral("split-preserve-zoom"),
                           QStringLiteral("navigation"));
    QVERIFY(applyGhosttyConfigSnapshot(base, snapshot)
                .splitPreserveZoomNavigation);

    snapshot.values.remove(QStringLiteral("split-preserve-zoom"));
    QVERIFY(applyGhosttyConfigSnapshot(base, snapshot)
                .splitPreserveZoomNavigation);

    snapshot.availability = GhosttyConfigAvailability::Unavailable;
    QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot), base);
}

void LaunchOptionsTest::mapsNewTabPosition()
{
    LaunchOptions base;
    base.windowNewTabPosition = WindowNewTabPosition::End;

    GhosttyConfigSnapshot snapshot;
    snapshot.availability = GhosttyConfigAvailability::Available;
    snapshot.values.insert(QStringLiteral("window-new-tab-position"),
                           QStringLiteral("current"));
    QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot).windowNewTabPosition,
             WindowNewTabPosition::Current);

    snapshot.values.insert(QStringLiteral("window-new-tab-position"),
                           QStringLiteral("end"));
    QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot).windowNewTabPosition,
             WindowNewTabPosition::End);

    for (const QVariant &invalid : {
             QVariant(QStringLiteral("after")), QVariant(false),
         }) {
        snapshot.values.insert(QStringLiteral("window-new-tab-position"),
                               invalid);
        QCOMPARE(
            applyGhosttyConfigSnapshot(base, snapshot).windowNewTabPosition,
            WindowNewTabPosition::End);
    }

    snapshot.availability = GhosttyConfigAvailability::Unavailable;
    QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot), base);
}

void LaunchOptionsTest::mapsWindowShowTabBar()
{
    const struct {
        QString canonical;
        WindowShowTabBar expected;
    } modes[] = {
        {QStringLiteral("always"), WindowShowTabBar::Always},
        {QStringLiteral("auto"), WindowShowTabBar::Auto},
        {QStringLiteral("never"), WindowShowTabBar::Never},
    };

    LaunchOptions base;
    base.windowShowTabBar = WindowShowTabBar::Never;
    GhosttyConfigSnapshot snapshot;
    snapshot.availability = GhosttyConfigAvailability::Available;
    for (const auto &mode : modes) {
        snapshot.values.insert(QStringLiteral("window-show-tab-bar"),
                               mode.canonical);
        QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot).windowShowTabBar,
                 mode.expected);
    }

    for (const QVariant &invalid : {
             QVariant(QStringLiteral("sometimes")), QVariant(true),
         }) {
        snapshot.values.insert(QStringLiteral("window-show-tab-bar"),
                               invalid);
        QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot).windowShowTabBar,
                 WindowShowTabBar::Never);
    }

    snapshot.availability = GhosttyConfigAvailability::Unavailable;
    QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot), base);
}

void LaunchOptionsTest::mapsStartupWindowState()
{
    LaunchOptions base;
    base.maximize = true;
    base.fullscreen = true;

    GhosttyConfigSnapshot snapshot;
    snapshot.availability = GhosttyConfigAvailability::Available;
    snapshot.values.insert(QStringLiteral("maximize"), false);
    snapshot.values.insert(QStringLiteral("fullscreen"),
                           QStringLiteral("false"));
    LaunchOptions result = applyGhosttyConfigSnapshot(base, snapshot);
    QVERIFY(!result.maximize);
    QVERIFY(!result.fullscreen);

    snapshot.values.insert(QStringLiteral("maximize"), true);
    for (const QString &mode : {
             QStringLiteral("true"),
             QStringLiteral("non-native"),
             QStringLiteral("non-native-visible-menu"),
             QStringLiteral("non-native-padded-notch"),
         }) {
        snapshot.values.insert(QStringLiteral("fullscreen"), mode);
        result = applyGhosttyConfigSnapshot(base, snapshot);
        QVERIFY(result.maximize);
        QVERIFY(result.fullscreen);
    }

    LaunchOptions unchanged;
    snapshot.values.insert(QStringLiteral("maximize"),
                           QStringLiteral("true"));
    snapshot.values.insert(QStringLiteral("fullscreen"), true);
    QCOMPARE(applyGhosttyConfigSnapshot(unchanged, snapshot), unchanged);

    snapshot.values.insert(QStringLiteral("maximize"), true);
    snapshot.values.insert(QStringLiteral("fullscreen"),
                           QStringLiteral("native"));
    result = applyGhosttyConfigSnapshot(unchanged, snapshot);
    QVERIFY(result.maximize);
    QVERIFY(!result.fullscreen);

    snapshot.availability = GhosttyConfigAvailability::Unavailable;
    QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot), base);
}

void LaunchOptionsTest::mapsApplicationLifetime()
{
    LaunchOptions base;
    GhosttyConfigSnapshot snapshot;
    snapshot.availability = GhosttyConfigAvailability::Available;
    snapshot.values.insert(
        QStringLiteral("quit-after-last-window-closed"), false);
    snapshot.values.insert(
        QStringLiteral("quit-after-last-window-closed-delay"),
        QVariant::fromValue<quint32>(1'500));
    snapshot.values.insert(QStringLiteral("initial-window"), false);

    LaunchOptions result = applyGhosttyConfigSnapshot(base, snapshot);
    QVERIFY(!result.quitAfterLastWindowClosed);
    QCOMPARE(result.quitAfterLastWindowClosedDelay,
             std::optional(std::chrono::milliseconds(1'500)));
    QVERIFY(!result.initialWindow);

    snapshot.values.insert(
        QStringLiteral("quit-after-last-window-closed-delay"),
        QVariant::fromValue<quint32>(0));
    result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.quitAfterLastWindowClosedDelay,
             std::optional(std::chrono::milliseconds::zero()));

    snapshot.values.insert(
        QStringLiteral("quit-after-last-window-closed-delay"), QVariant{});
    result = applyGhosttyConfigSnapshot(base, snapshot);
    QVERIFY(!result.quitAfterLastWindowClosedDelay.has_value());

    base.quitAfterLastWindowClosed = false;
    base.quitAfterLastWindowClosedDelay = std::chrono::milliseconds(75);
    snapshot.values.insert(
        QStringLiteral("quit-after-last-window-closed"),
        QStringLiteral("false"));
    snapshot.values.insert(
        QStringLiteral("quit-after-last-window-closed-delay"),
        QStringLiteral("1500"));
    snapshot.values.insert(QStringLiteral("initial-window"),
                           QStringLiteral("false"));
    QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot), base);

    base.program = {QStringLiteral("/bin/true")};
    result = applyGhosttyConfigSnapshot(base, snapshot);
    QVERIFY(result.quitAfterLastWindowClosed);
    QVERIFY(!result.quitAfterLastWindowClosedDelay.has_value());
    QVERIFY(result.initialWindow);

    base.initialWindow = false;
    snapshot.availability = GhosttyConfigAvailability::Unavailable;
    result = applyGhosttyConfigSnapshot(base, snapshot);
    QVERIFY(result.quitAfterLastWindowClosed);
    QVERIFY(!result.quitAfterLastWindowClosedDelay.has_value());
    QVERIFY(!result.initialWindow);
}

void LaunchOptionsTest::mapsSingleInstancePolicy()
{
    LaunchOptions base;
    GhosttyConfigSnapshot snapshot;
    snapshot.availability = GhosttyConfigAvailability::Available;

    snapshot.values.insert(QStringLiteral("gtk-single-instance"),
                           QStringLiteral("true"));
    LaunchOptions options = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(options.singleInstanceMode, SingleInstanceMode::Enabled);
    QVERIFY(shouldUseSingleInstance(options, QByteArrayView("ghostty")));
    // Forwarding command-line payloads is deliberately outside this first
    // protocol even when the config explicitly enables process uniqueness.
    options.hasUnforwardedLaunchPayload = true;
    QVERIFY(!shouldUseSingleInstance(options, QByteArrayView{}));
    options.hasUnforwardedLaunchPayload = false;

    snapshot.values.insert(QStringLiteral("gtk-single-instance"),
                           QStringLiteral("false"));
    options = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(options.singleInstanceMode, SingleInstanceMode::Disabled);
    QVERIFY(!shouldUseSingleInstance(options, QByteArrayView{}));

    snapshot.values.insert(QStringLiteral("gtk-single-instance"),
                           QStringLiteral("detect"));
    options = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(options.singleInstanceMode, SingleInstanceMode::Detect);
    QVERIFY(shouldUseSingleInstance(options, QByteArrayView{}));
    QVERIFY(!shouldUseSingleInstance(options, QByteArrayView("ghostty")));
    options.hasUnforwardedLaunchPayload = true;
    QVERIFY(!shouldUseSingleInstance(options, QByteArrayView{}));

    snapshot.values.insert(QStringLiteral("gtk-single-instance"),
                           QStringLiteral("invalid"));
    QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot).singleInstanceMode,
             SingleInstanceMode::Detect);
}

void LaunchOptionsTest::mapsUnfocusedSplitAppearance()
{
    LaunchOptions base;
    base.splitAppearance = {
        .unfocusedOpacity = 0.4,
        .unfocusedFill = QColor(Qt::red),
    };

    GhosttyConfigSnapshot snapshot;
    snapshot.availability = GhosttyConfigAvailability::Available;
    snapshot.values.insert(QStringLiteral("unfocused-split-opacity"), -2.0);
    snapshot.values.insert(QStringLiteral("unfocused-split-fill"),
                           QColor(QStringLiteral("#123456")));
    LaunchOptions result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.splitAppearance.unfocusedOpacity, 0.15);
    QCOMPARE(result.splitAppearance.unfocusedFill,
             std::optional<QColor>(QColor(QStringLiteral("#123456"))));

    snapshot.values.insert(QStringLiteral("unfocused-split-opacity"), 2.0);
    snapshot.values.insert(QStringLiteral("unfocused-split-fill"), QString());
    result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.splitAppearance.unfocusedOpacity, 1.0);
    QVERIFY(!result.splitAppearance.unfocusedFill.has_value());

    snapshot.values.insert(QStringLiteral("unfocused-split-opacity"),
                           QStringLiteral("not-a-number"));
    snapshot.values.insert(QStringLiteral("unfocused-split-fill"),
                           QStringLiteral("not-a-color"));
    result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.splitAppearance, base.splitAppearance);

    snapshot.values.insert(
        QStringLiteral("unfocused-split-opacity"),
        std::numeric_limits<double>::quiet_NaN());
    result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.splitAppearance, base.splitAppearance);
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
    base.splitAppearance.unfocusedFill = QColor(Qt::cyan);
    base.splitAppearance.dividerColor = QColor(Qt::blue);

    GhosttyConfigSnapshot snapshot;
    snapshot.availability = GhosttyConfigAvailability::Available;
    snapshot.values.insert(QStringLiteral("selection-foreground"), QString());
    snapshot.values.insert(QStringLiteral("selection-background"), QString());
    snapshot.values.insert(QStringLiteral("cursor-color"), QString());
    snapshot.values.insert(QStringLiteral("cursor-style-blink"), QString());
    snapshot.values.insert(QStringLiteral("cursor-text"), QString());
    snapshot.values.insert(QStringLiteral("bold-color"), QString());
    snapshot.values.insert(QStringLiteral("unfocused-split-fill"), QString());
    snapshot.values.insert(QStringLiteral("split-divider-color"), QString());

    const TerminalAppearance appearance =
        applyGhosttyConfigSnapshot(base, snapshot).appearance;
    QCOMPARE(appearance.selectionForeground.kind, TerminalColorKind::Unset);
    QCOMPARE(appearance.selectionBackground.kind, TerminalColorKind::Unset);
    QCOMPARE(appearance.cursorColor.kind, TerminalColorKind::Unset);
    QVERIFY(!appearance.cursorBlink.has_value());
    QCOMPARE(appearance.cursorTextColor.kind, TerminalColorKind::Unset);
    QCOMPARE(appearance.boldColor.kind, TerminalBoldColorKind::Unset);
    QVERIFY(!applyGhosttyConfigSnapshot(base, snapshot)
                 .splitAppearance.unfocusedFill.has_value());
    QVERIFY(!applyGhosttyConfigSnapshot(base, snapshot)
                 .splitAppearance.dividerColor.has_value());
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
    snapshot.values.insert(QStringLiteral("search-background"), QString());
    snapshot.values.insert(QStringLiteral("search-selected-foreground"),
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
    snapshot.values.insert(QStringLiteral("link-previews"),
                           true);
    snapshot.values.insert(QStringLiteral("clipboard-trim-trailing-spaces"),
                           QStringLiteral("false"));
    snapshot.values.insert(QStringLiteral("clipboard-paste-protection"),
                           QStringLiteral("false"));
    snapshot.values.insert(QStringLiteral("clipboard-paste-bracketed-safe"),
                           QStringLiteral("false"));
    snapshot.values.insert(QStringLiteral("copy-on-select"), true);
    snapshot.values.insert(QStringLiteral("selection-clear-on-typing"),
                           QStringLiteral("false"));
    snapshot.values.insert(QStringLiteral("selection-clear-on-copy"),
                           QStringLiteral("true"));
    snapshot.values.insert(QStringLiteral("middle-click-action"), false);
    snapshot.values.insert(QStringLiteral("mouse-reporting"),
                           QStringLiteral("false"));
    snapshot.values.insert(QStringLiteral("window-show-tab-bar"),
                           QStringLiteral("sometimes"));
    snapshot.values.insert(QStringLiteral("split-divider-color"),
                           QStringLiteral("not-a-color"));
    snapshot.values.insert(QStringLiteral("unfocused-split-opacity"),
                           QStringLiteral("not-a-number"));
    snapshot.values.insert(QStringLiteral("unfocused-split-fill"),
                           QStringLiteral("not-a-color"));
    QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot).fontFamily, base.fontFamily);

    snapshot.availability = GhosttyConfigAvailability::Available;
    const LaunchOptions result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.fontSize, base.fontSize);
    QCOMPARE(result.appearance, base.appearance);
    QCOMPARE(result.scrollbackLimit, base.scrollbackLimit);
    QCOMPARE(result.confirmCloseMode, base.confirmCloseMode);
    QCOMPARE(result.linkUrl, base.linkUrl);
    QCOMPARE(result.linkPreviews, base.linkPreviews);
    QCOMPARE(result.selectionClipboard, base.selectionClipboard);
    QCOMPARE(result.clipboardPaste, base.clipboardPaste);
    QCOMPARE(result.middleClickAction, base.middleClickAction);
    QCOMPARE(result.mouseReporting, base.mouseReporting);
    QCOMPARE(result.windowShowTabBar, base.windowShowTabBar);
    QCOMPARE(result.splitAppearance, base.splitAppearance);
}

void LaunchOptionsTest::projectsTerminalSessionOptions()
{
    LaunchOptions options;
    options.workingDirectory = QStringLiteral("/session/working-directory");
    options.workingDirectoryExplicit = true;
    options.program = {QStringLiteral("/bin/program"), QStringLiteral("arg")};
    options.scrollbackLimit = {
        .value = 42'000,
        .unit = ScrollbackLimitUnit::Bytes,
    };
    options.hold = true;
    options.appearance.foregroundColor = QColor(QStringLiteral("#123456"));
    options.appearance.palette = {QColor(QStringLiteral("#abcdef"))};
    options.selectionClipboard = {
        .trimTrailingSpaces = false,
        .copyOnSelect = TerminalCopyOnSelectMode::PrimaryAndClipboard,
        .clearOnTyping = false,
        .clearOnCopy = true,
    };
    options.clipboardPaste = {
        .protection = false,
        .bracketedSafe = true,
    };
    options.middleClickAction = MiddleClickAction::Ignore;
    options.linkUrl = false;
    options.splitAppearance = {
        .unfocusedOpacity = 0.35,
        .unfocusedFill = QColor(QStringLiteral("#123456")),
    };

    const TerminalSessionRuntimeOptions runtime =
        toTerminalSessionRuntimeOptions(options);
    const TerminalSessionLaunchOptions launch =
        toTerminalSessionLaunchOptions(options);

    QCOMPARE(runtime.appearance, options.appearance);
    QCOMPARE(runtime.selectionClipboard, options.selectionClipboard);
    QCOMPARE(runtime.clipboardPaste, options.clipboardPaste);
    QCOMPARE(runtime.linkUrl, options.linkUrl);
    QCOMPARE(launch.workingDirectory, options.workingDirectory);
    QCOMPARE(launch.inheritWorkingDirectory,
             options.inheritWorkingDirectory);
    QCOMPARE(launch.program, options.program);
    QCOMPARE(launch.scrollbackLimit, options.scrollbackLimit);
    QCOMPARE(launch.hold, options.hold);
    QCOMPARE(launch.runtime, runtime);

    QVERIFY(QMetaType::fromType<TerminalSessionLaunchOptions>().isValid());
    QVERIFY(QMetaType::fromType<TerminalSessionRuntimeOptions>().isValid());
    QVERIFY(qvariant_cast<TerminalSessionLaunchOptions>(
                QVariant::fromValue(launch)) == launch);
    QVERIFY(qvariant_cast<TerminalSessionRuntimeOptions>(
                QVariant::fromValue(runtime)) == runtime);

    LaunchOptions frontendOnlyChanged = options;
    frontendOnlyChanged.fontFamily = QStringLiteral("Frontend Font");
    frontendOnlyChanged.fontSize = 19.0;
    frontendOnlyChanged.fontFamilyExplicit = true;
    frontendOnlyChanged.fontSizeExplicit = true;
    frontendOnlyChanged.confirmCloseMode = ConfirmCloseMode::Always;
    frontendOnlyChanged.workingDirectoryExplicit = false;
    frontendOnlyChanged.splitInheritWorkingDirectory = false;
    frontendOnlyChanged.splitPreserveZoomNavigation = true;
    frontendOnlyChanged.tabInheritWorkingDirectory = false;
    frontendOnlyChanged.windowInheritWorkingDirectory = false;
    frontendOnlyChanged.windowInheritFontSize = false;
    frontendOnlyChanged.windowNewTabPosition =
        WindowNewTabPosition::End;
    frontendOnlyChanged.windowShowTabBar = WindowShowTabBar::Never;
    frontendOnlyChanged.maximize = true;
    frontendOnlyChanged.fullscreen = true;
    frontendOnlyChanged.quitAfterLastWindowClosed = false;
    frontendOnlyChanged.quitAfterLastWindowClosedDelay =
        std::chrono::milliseconds(250);
    frontendOnlyChanged.splitAppearance = {
        .unfocusedOpacity = 0.9,
        .unfocusedFill = QColor(QStringLiteral("#fedcba")),
    };
    frontendOnlyChanged.splitAppearance.dividerColor =
        QColor(QStringLiteral("#abcdef"));
    frontendOnlyChanged.linkPreviews = LinkPreviewMode::Never;
    frontendOnlyChanged.middleClickAction = MiddleClickAction::PrimaryPaste;
    frontendOnlyChanged.keybindings = {QStringLiteral("ctrl+x=ignore")};
    frontendOnlyChanged.keybindingsConfigured = true;
    frontendOnlyChanged.showHelp = true;
    frontendOnlyChanged.showVersion = true;
    QCOMPARE(toTerminalSessionLaunchOptions(frontendOnlyChanged), launch);
    QCOMPARE(toTerminalSessionRuntimeOptions(frontendOnlyChanged), runtime);

    LaunchOptions inheritedDirectory = options;
    inheritedDirectory.inheritWorkingDirectory = true;
    QVERIFY(toTerminalSessionLaunchOptions(inheritedDirectory) != launch);
    QCOMPARE(toTerminalSessionRuntimeOptions(inheritedDirectory), runtime);

    options.workingDirectory.clear();
    options.program.clear();
    options.scrollbackLimit = {};
    options.hold = false;
    options.appearance = {};
    options.selectionClipboard = {};
    options.clipboardPaste = {};
    options.middleClickAction = MiddleClickAction::PrimaryPaste;
    options.linkUrl = true;
    QCOMPARE(launch.workingDirectory,
             QStringLiteral("/session/working-directory"));
    QCOMPARE(launch.program,
             QStringList({QStringLiteral("/bin/program"),
                          QStringLiteral("arg")}));
    QCOMPARE(launch.scrollbackLimit.value, quint64(42'000));
    QCOMPARE(launch.runtime.appearance.foregroundColor,
             QColor(QStringLiteral("#123456")));
    const TerminalSelectionClipboardOptions expectedClipboard{
        .trimTrailingSpaces = false,
        .copyOnSelect = TerminalCopyOnSelectMode::PrimaryAndClipboard,
        .clearOnTyping = false,
        .clearOnCopy = true,
    };
    QCOMPARE(launch.runtime.selectionClipboard, expectedClipboard);
    const TerminalClipboardPasteOptions expectedPaste{
        .protection = false,
        .bracketedSafe = true,
    };
    QCOMPARE(launch.runtime.clipboardPaste, expectedPaste);
    QVERIFY(!launch.runtime.linkUrl);
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
