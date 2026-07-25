#include "launch_options.h"

#include "ghostty_config_snapshot_fixture.h"

#include <QDir>
#include <QFile>
#include <QLocale>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <array>
#include <limits>

namespace {

QString errorMessage(
    const std::expected<LaunchOptions, QString> &result)
{
    return result ? QString{} : result.error();
}

std::array<QColor, 256> testPalette()
{
    std::array<QColor, 256> palette;
    for (int index = 0; index < 256; ++index) {
        palette[static_cast<std::size_t>(index)] =
            QColor::fromRgb(index, 255 - index, index / 2);
    }
    return palette;
}

TerminalTypography completeTypography()
{
    TerminalTypography typography;
    typography.face(TerminalFontRole::Regular) = {
        .families = {
            QStringLiteral("Config Primary"),
            QStringLiteral("Config Fallback"),
        },
        .style = TerminalFontStyles::Named{QStringLiteral("Book")},
    };
    typography.face(TerminalFontRole::Bold) = {
        .families = {
            QStringLiteral("Config Bold"),
            QStringLiteral("Config Bold Fallback"),
        },
        .style = TerminalFontStyles::Disabled{},
    };
    typography.face(TerminalFontRole::Italic) = {
        .families = {
            QStringLiteral("Config Italic"),
        },
        .style = TerminalFontStyles::Automatic{},
    };
    typography.face(TerminalFontRole::BoldItalic) = {
        .families = {
            QStringLiteral("Config Bold Italic"),
        },
        .style =
            TerminalFontStyles::Named{QStringLiteral("Extra Black Italic")},
    };
    typography.pointSize = 14.5;
    typography.metricModifiers[TerminalMetric::CellWidth] =
        TerminalMetricModifiers::Percentage{1.125};
    typography.metricModifiers[TerminalMetric::FontBaseline] =
        TerminalMetricModifiers::Absolute{-2};
    typography.metricModifiers[TerminalMetric::CursorHeight] =
        TerminalMetricModifiers::Percentage{0.75};
    return typography;
}

const QStringList &regularFamilies(const LaunchOptions &options)
{
    return options.typography.face(TerminalFontRole::Regular).families;
}

GhosttyConfigSnapshot completeSnapshot()
{
    GhosttyConfigSnapshot snapshot = GhosttyConfigSnapshotFixture::snapshot();
    GhosttyConfigValues &values = snapshot.values;
    values.workingDirectoryPath = QStringLiteral("/work/ghostty");
    values.typography = completeTypography();

    values.appearance = {
        .foreground = QColor(QStringLiteral("#112233")),
        .background = QColor(QStringLiteral("#445566")),
        .palette = testPalette(),
        .selectionForeground =
            GhosttyTerminalColor{GhosttyCellRelativeColor::Foreground},
        .selectionBackground =
            GhosttyTerminalColor{QColor(QStringLiteral("#223344"))},
        .searchForeground =
            GhosttyTerminalColor{GhosttyCellRelativeColor::Background},
        .searchBackground =
            GhosttyTerminalColor{QColor(QStringLiteral("#123456"))},
        .searchSelectedForeground =
            GhosttyTerminalColor{GhosttyCellRelativeColor::Foreground},
        .searchSelectedBackground =
            GhosttyTerminalColor{QColor(QStringLiteral("#654321"))},
        .cursorColor =
            GhosttyTerminalColor{GhosttyCellRelativeColor::Background},
        .cursorStyle = TerminalCursorStyle::BlockHollow,
        .cursorBlink = false,
        .cursorOpacity = 0.625,
        .cursorText =
            GhosttyTerminalColor{GhosttyCellRelativeColor::Foreground},
        .boldColor = GhosttyBoldColor{QColor(QStringLiteral("#abcdef"))},
        .faintOpacity = 0.375,
    };
    values.splitAppearance = {
        .unfocusedOpacity = 0.42,
        .unfocusedFill = QColor(QStringLiteral("#778899")),
        .dividerColor = QColor(QStringLiteral("#a1b2c3")),
    };

    values.splitInheritWorkingDirectory = false;
    values.splitPreserveZoom = true;
    values.tabInheritWorkingDirectory = false;
    values.windowInheritWorkingDirectory = false;
    values.windowInheritFontSize = false;
    values.windowNewTabPosition = WindowNewTabPosition::End;
    values.windowShowTabBar = WindowShowTabBar::Always;
    values.windowDecoration = WindowDecorationMode::Server;
    values.windowWidth = 120;
    values.windowHeight = 40;
    values.maximize = true;
    values.fullscreen = GhosttyFullscreenMode::NonNative;
    values.resizeOverlay = {
        .mode = ResizeOverlayMode::Always,
        .position = ResizeOverlayPosition::BottomRight,
        .duration = std::chrono::milliseconds(1'250),
    };

    values.scrollbackLimitBytes = 50'000'000;
    values.bellAudioPath = GhosttyConfigPath{
        .path = QStringLiteral("/work/bell.oga"),
        .optional = true,
    };
    values.bellAudioVolume = 0.625;
    values.confirmCloseMode = ConfirmCloseMode::Always;
    values.selectionClipboard = {
        .trimTrailingSpaces = false,
        .copyOnSelect = TerminalCopyOnSelectMode::PrimaryAndClipboard,
        .clearOnTyping = false,
        .clearOnCopy = true,
    };
    values.clipboardPaste = {
        .protection = false,
        .bracketedSafe = true,
    };
    values.middleClickAction = MiddleClickAction::Ignore;
    values.mouseReporting = false;
    values.mouseHideWhileTyping = true;
    values.focusFollowsMouse = true;
    values.selectionWordChars = {0, 0x20, 0x2502, 0x1f642};
    values.mouseScrollMultiplier = {
        .precision = 0.75,
        .discrete = 4.5,
    };
    values.linkUrl = false;
    values.linkPreviews = LinkPreviewMode::Osc8;
    values.configFiles = {
        {.path = QStringLiteral("/work/include.ghostty")},
        {.path = QStringLiteral("/work/optional.ghostty"), .optional = true},
    };
    values.quitAfterLastWindowClosed = false;
    values.quitAfterLastWindowClosedDelay.reset();
    values.initialWindow = false;
    values.singleInstanceMode = SingleInstanceMode::Detect;

    snapshot.keybindings = {
        .root = {GhosttyKeybindDefinition{
            .sequence = {GhosttyKeybindTrigger{
                .kind = GhosttyKeybindKeyKind::Unicode,
                .unicodeCodepoint = quint32('n'),
                .modifiers = GhosttyKeybindAlt,
            }},
            .actions = {QStringLiteral("new_tab")},
        }},
    };
    return snapshot;
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
    void normalizesFontSizeToGhosttyPrecision();
    void buildsGhosttyFontCliArguments();
    void rejectsInvalidScrollbackLines_data();
    void rejectsInvalidScrollbackLines();
    void rejectsUnknownOption();
    void rejectsMissingApplicationName();
    void appliesFinalizedGhosttyTypography();
    void mapsScrollbarPolicy();
    void mapsBellFeatures();
    void mapsBellAudio();
    void mapsMouseHideWhileTyping();
    void mapsFocusFollowsMouse();
    void mapsSelectionWordChars();
    void mapsMouseScrollMultiplier();
    void mapsLinkPreviewModes();
    void mapsLinkPreviewModes_data();
    void mapsClipboardModes();
    void mapsWorkingDirectoryAndSurfaceInheritance();
    void mapsSplitPreserveZoom();
    void mapsNewTabPosition();
    void mapsWindowShowTabBar();
    void mapsWindowDecoration();
    void mapsWindowCellDimensions();
    void mapsResizeOverlay();
    void mapsStartupWindowState();
    void mapsApplicationLifetime();
    void mapsSingleInstancePolicy();
    void mapsFrontendConfigurationPrecedence();
    void mapsUnfocusedSplitAppearance();
    void restoresNullableAppearanceDefaults();
    void removesOnlyTheInitialCommand();
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
    QVERIFY(regularFamilies(options).isEmpty());
    QCOMPARE(options.typography.pointSize, 12.0);
    for (const TerminalFontFace &face : options.typography.faces) {
        QVERIFY(face.families.isEmpty());
        QVERIFY(std::holds_alternative<TerminalFontStyles::Automatic>(
            face.style));
    }
    QVERIFY(std::ranges::all_of(
        options.typography.metricModifiers.values,
        [](const TerminalMetricModifierSet::Value &modifier) {
            return !modifier.has_value();
        }));
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
    QCOMPARE(options.scrollbar, ScrollbarPolicy::System);
    QVERIFY(!options.bellFeatures.system);
    QVERIFY(!options.bellFeatures.audio);
    QVERIFY(options.bellFeatures.attention);
    QVERIFY(options.bellFeatures.title);
    QVERIFY(!options.bellFeatures.border);
    QVERIFY(!options.bellAudioPath.has_value());
    QCOMPARE(options.bellAudioVolume, 0.5);
    QVERIFY(!options.mouseHideWhileTyping);
    QVERIFY(!options.focusFollowsMouse);
    QVERIFY(options.selectionWordChars.isEmpty());
    QCOMPARE(options.mouseScrollMultiplier.precision, 1.0);
    QCOMPARE(options.mouseScrollMultiplier.discrete, 3.0);
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
    QCOMPARE(options.tabsLocation, TabsLocation::Top);
    QCOMPARE(options.windowDecoration, WindowDecorationMode::Auto);
    QCOMPARE(options.windowWidth, quint32(0));
    QCOMPARE(options.windowHeight, quint32(0));
    QCOMPARE(options.resizeOverlay.mode, ResizeOverlayMode::AfterFirst);
    QCOMPARE(options.resizeOverlay.position, ResizeOverlayPosition::Center);
    QCOMPARE(options.resizeOverlay.duration, std::chrono::milliseconds(750));
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
    QVERIFY(!options.keybindSource.isAvailable());
    QVERIFY(!options.hold);
    QVERIFY(options.program.isEmpty());
}

void LaunchOptionsTest::parsesActivationBootstrapOptions()
{
    const auto service = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--single-instance=true"),
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

    GhosttyConfigSnapshot contradictory = completeSnapshot();
    contradictory.values.singleInstanceMode = SingleInstanceMode::Disabled;
    contradictory.values.initialWindow = true;
    FrontendConfigSnapshot frontend;
    frontend.values.singleInstanceMode = SingleInstanceMode::Disabled;
    frontend.values.tabsLocation = TabsLocation::Bottom;
    const LaunchOptions configured =
        resolveLaunchOptions(*service, &contradictory, &frontend);
    QCOMPARE(configured.singleInstanceMode, SingleInstanceMode::Enabled);
    QCOMPARE(configured.tabsLocation, TabsLocation::Bottom);
    QVERIFY(!configured.initialWindow);

    const auto detect = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--single-instance=detect"),
        QStringLiteral("--initial-window=T"),
    });
    QVERIFY2(detect.has_value(), qPrintable(errorMessage(detect)));
    QVERIFY(detect->initialWindow);
    QVERIFY(shouldUseSingleInstance(*detect, QByteArrayView{}));
    QVERIFY(!shouldUseSingleInstance(*detect, QByteArrayView("ghostty")));

    const auto disabled = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--single-instance=false"),
        QStringLiteral("--initial-window=0"),
    });
    QVERIFY2(disabled.has_value(), qPrintable(errorMessage(disabled)));
    QVERIFY(!shouldUseSingleInstance(*disabled, QByteArrayView{}));
    QVERIFY(!disabled->initialWindow);

    const auto legacyAlias = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--gtk-single-instance=false"),
    });
    QVERIFY2(legacyAlias.has_value(), qPrintable(errorMessage(legacyAlias)));
    QCOMPARE(legacyAlias->singleInstanceMode, SingleInstanceMode::Disabled);
    QVERIFY(legacyAlias->singleInstanceModeExplicit);

    const auto conflictingAliases = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--single-instance=true"),
        QStringLiteral("--gtk-single-instance=false"),
    });
    QVERIFY(!conflictingAliases.has_value());
    QVERIFY(conflictingAliases.error().contains(
        QStringLiteral("--single-instance")));
    QVERIFY(conflictingAliases.error().contains(
        QStringLiteral("--gtk-single-instance")));

    for (const QString &argument : {
             QStringLiteral("--single-instance=yes"),
             QStringLiteral("--initial-window=yes"),
         }) {
        const auto invalid = parseLaunchOptions(
            {QStringLiteral("ghostty-qt"), argument});
        QVERIFY(!invalid.has_value());
        QVERIFY(invalid.error().contains(QStringLiteral("Invalid")));
    }

    const auto payload = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--single-instance=true"),
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
    QCOMPARE(regularFamilies(options),
             QStringList{QStringLiteral("Iosevka Term")});
    QCOMPARE(regularFamilies(options).size(), qsizetype(1));
    QCOMPARE(options.typography.pointSize, 15.5);
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
    QTest::newRow("overflows-f32") << QStringLiteral("3.5e38");
    QTest::newRow("underflows-f32") << QStringLiteral("1e-1000");
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

void LaunchOptionsTest::normalizesFontSizeToGhosttyPrecision()
{
    const QLocale previousLocale;
    struct RestoreLocale {
        QLocale locale;
        ~RestoreLocale() { QLocale::setDefault(locale); }
    } restore{previousLocale};
    QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));

    const auto result = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--font-size=13.123456789"),
    });
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));

    const double expected =
        static_cast<double>(static_cast<float>(13.123456789));
    QCOMPARE(result->typography.pointSize, expected);
    QCOMPARE(
        ghosttyConfigCliFontArguments(*result),
        QStringList{QStringLiteral("--font-size=13.123457")});
}

void LaunchOptionsTest::buildsGhosttyFontCliArguments()
{
    QCOMPARE(ghosttyConfigCliFontArguments({}), QStringList{});

    const QString unicodeFamily =
        QString::fromUtf8("家族 = --font \"with spaces\" \xE2\x98\x83");
    const auto family = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--font-family"),
        unicodeFamily,
    });
    QVERIFY2(family.has_value(), qPrintable(errorMessage(family)));
    QCOMPARE(regularFamilies(*family), QStringList{unicodeFamily});
    QCOMPARE(regularFamilies(*family).size(), qsizetype(1));
    QCOMPARE(ghosttyConfigCliFontArguments(*family),
             QStringList{QStringLiteral("--font-family=") + unicodeFamily});

    const auto repeatedFamilies = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--font-family=Before Reset"),
        QStringLiteral("--font-family="),
        QStringLiteral("--font-family=Primary"),
        QStringLiteral("--font-family=Fallback"),
    });
    QVERIFY2(repeatedFamilies.has_value(),
             qPrintable(errorMessage(repeatedFamilies)));
    QCOMPARE(regularFamilies(*repeatedFamilies),
             QStringList({QStringLiteral("Primary"),
                          QStringLiteral("Fallback")}));
    QCOMPARE(
        ghosttyConfigCliFontArguments(*repeatedFamilies),
        QStringList({QStringLiteral("--font-family=Primary"),
                     QStringLiteral("--font-family=Fallback")}));

    const auto emptyFamily = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--font-family="),
    });
    QVERIFY2(emptyFamily.has_value(), qPrintable(errorMessage(emptyFamily)));
    QVERIFY(regularFamilies(*emptyFamily).isEmpty());
    QCOMPARE(ghosttyConfigCliFontArguments(*emptyFamily),
             QStringList{QStringLiteral("--font-family=")});

    LaunchOptions defensive;
    defensive.fontFamilyExplicit = true;
    defensive.typography.face(TerminalFontRole::Regular).families = {
        QStringLiteral("First"),
        QStringLiteral("Second"),
    };
    defensive.fontSizeExplicit = true;
    defensive.typography.pointSize = 15.5;
    QCOMPARE(ghosttyConfigCliFontArguments(defensive),
             QStringList({
                 QStringLiteral("--font-family=First"),
                 QStringLiteral("--font-family=Second"),
                 QStringLiteral("--font-size=15.5"),
             }));

    defensive.typography.face(TerminalFontRole::Regular).families.clear();
    defensive.fontSizeExplicit = false;
    QCOMPARE(ghosttyConfigCliFontArguments(defensive),
             QStringList{QStringLiteral("--font-family=")});
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

void LaunchOptionsTest::appliesFinalizedGhosttyTypography()
{
    LaunchOptions base;
    base.typography.face(TerminalFontRole::Regular).families = {
        QStringLiteral("CLI Family"),
    };
    base.typography.face(TerminalFontRole::Bold).families = {
        QStringLiteral("Stale Launch Bold"),
    };
    base.typography.face(TerminalFontRole::Italic).style =
        TerminalFontStyles::Disabled{};
    base.typography.pointSize = 17.0;
    base.typography.metricModifiers[TerminalMetric::CellHeight] =
        TerminalMetricModifiers::Absolute{99};
    base.fontFamilyExplicit = true;
    base.fontSizeExplicit = true;
    base.scrollbackLimit = {.value = 25'000,
                            .unit = ScrollbackLimitUnit::Lines};
    base.scrollbackLimitExplicit = true;

    const GhosttyConfigSnapshot snapshot = completeSnapshot();

    const LaunchOptions cliResult = applyGhosttyConfigSnapshot(base, snapshot);
    // The process helper has already applied explicit CLI arguments,
    // recursive includes, and styled-family finalization. The broad snapshot
    // projection must trust that complete value instead of rebuilding a
    // hybrid from the original frontend flags.
    QVERIFY(cliResult.typography == completeTypography());
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
    QCOMPARE(cliResult.selectionWordChars,
             QVector<quint32>({0, 0x20, 0x2502, 0x1f642}));
    QVERIFY(!cliResult.linkUrl);
    QCOMPARE(cliResult.linkPreviews, LinkPreviewMode::Osc8);
    QVERIFY(cliResult.keybindSource.isAvailable());
    QVERIFY(cliResult.keybindSource.text() == nullptr);
    QVERIFY(cliResult.keybindSource.structured() != nullptr);
    QCOMPARE(*cliResult.keybindSource.structured(), snapshot.keybindings);

    base.scrollbackLimitExplicit = false;
    const LaunchOptions configResult = applyGhosttyConfigSnapshot(base, snapshot);
    QVERIFY(configResult.typography == completeTypography());
    QCOMPARE(configResult.scrollbackLimit.value, quint64(50'000'000));
    QCOMPARE(configResult.scrollbackLimit.unit, ScrollbackLimitUnit::Bytes);

    for (const double unusableSize : {0.0, -2.0}) {
        GhosttyConfigSnapshot unusable = snapshot;
        unusable.values.typography.pointSize = unusableSize;
        const LaunchOptions result =
            applyGhosttyConfigSnapshot(base, unusable);
        TerminalTypography expected = completeTypography();
        expected.pointSize = base.typography.pointSize;
        QVERIFY(result.typography == expected);
    }
}

void LaunchOptionsTest::mapsScrollbarPolicy()
{
    for (const ScrollbarPolicy configured : {
             ScrollbarPolicy::System,
             ScrollbarPolicy::Never,
         }) {
        LaunchOptions base;
        base.scrollbar = configured == ScrollbarPolicy::System
            ? ScrollbarPolicy::Never
            : ScrollbarPolicy::System;
        GhosttyConfigSnapshot snapshot = completeSnapshot();
        snapshot.values.scrollbar = configured;

        QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot).scrollbar,
                 configured);
    }
}

void LaunchOptionsTest::mapsBellFeatures()
{
    LaunchOptions base;
    base.bellFeatures = {
        .system = false,
        .audio = false,
        .attention = true,
        .title = true,
        .border = false,
    };
    GhosttyConfigSnapshot snapshot = completeSnapshot();
    snapshot.values.bellFeatures = {
        .system = true,
        .audio = true,
        .attention = false,
        .title = false,
        .border = true,
    };

    QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot).bellFeatures,
             snapshot.values.bellFeatures);
}

void LaunchOptionsTest::mapsBellAudio()
{
    LaunchOptions base;
    base.bellAudioPath = GhosttyConfigPath{
        .path = QStringLiteral("/base/bell.wav"),
        .optional = false,
    };
    base.bellAudioVolume = 0.25;
    GhosttyConfigSnapshot snapshot = completeSnapshot();
    snapshot.values.bellAudioPath = GhosttyConfigPath{
        .path = QStringLiteral("/config/bell.oga"),
        .optional = true,
    };
    snapshot.values.bellAudioVolume = 1.75;

    const LaunchOptions configured = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(configured.bellAudioPath, snapshot.values.bellAudioPath);
    QCOMPARE(configured.bellAudioVolume, 1.75);

    snapshot.values.bellAudioPath.reset();
    snapshot.values.bellAudioVolume = 0.5;
    const LaunchOptions defaults =
        applyGhosttyConfigSnapshot(configured, snapshot);
    QVERIFY(!defaults.bellAudioPath.has_value());
    QCOMPARE(defaults.bellAudioVolume, 0.5);
}

void LaunchOptionsTest::mapsMouseScrollMultiplier()
{
    LaunchOptions base;
    base.mouseScrollMultiplier = {
        .precision = 0.25,
        .discrete = 0.5,
    };
    GhosttyConfigSnapshot snapshot = completeSnapshot();

    const LaunchOptions configured = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(configured.mouseScrollMultiplier.precision, 0.75);
    QCOMPARE(configured.mouseScrollMultiplier.discrete, 4.5);

    snapshot.values.mouseScrollMultiplier = {};
    const LaunchOptions defaults =
        applyGhosttyConfigSnapshot(configured, snapshot);
    QCOMPARE(defaults.mouseScrollMultiplier.precision, 1.0);
    QCOMPARE(defaults.mouseScrollMultiplier.discrete, 3.0);
}

void LaunchOptionsTest::mapsMouseHideWhileTyping()
{
    for (const bool enabled : {false, true}) {
        LaunchOptions base;
        base.mouseHideWhileTyping = !enabled;
        GhosttyConfigSnapshot snapshot = completeSnapshot();
        snapshot.values.mouseHideWhileTyping = enabled;

        QCOMPARE(
            applyGhosttyConfigSnapshot(base, snapshot).mouseHideWhileTyping,
            enabled);
    }
}

void LaunchOptionsTest::mapsFocusFollowsMouse()
{
    for (const bool enabled : {false, true}) {
        LaunchOptions base;
        base.focusFollowsMouse = !enabled;
        GhosttyConfigSnapshot snapshot = completeSnapshot();
        snapshot.values.focusFollowsMouse = enabled;

        QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot).focusFollowsMouse,
                 enabled);
    }
}

void LaunchOptionsTest::mapsSelectionWordChars()
{
    LaunchOptions base;
    base.selectionWordChars = {0, quint32('x')};
    GhosttyConfigSnapshot snapshot = completeSnapshot();

    const LaunchOptions configured = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(configured.selectionWordChars,
             QVector<quint32>({0, 0x20, 0x2502, 0x1f642}));

    snapshot.values.selectionWordChars = {0, quint32('y')};
    const LaunchOptions reloaded =
        applyGhosttyConfigSnapshot(configured, snapshot);
    QCOMPARE(reloaded.selectionWordChars, QVector<quint32>({0, quint32('y')}));
}

void LaunchOptionsTest::mapsLinkPreviewModes_data()
{
    QTest::addColumn<LinkPreviewMode>("configured");

    QTest::newRow("never") << LinkPreviewMode::Never;
    QTest::newRow("always") << LinkPreviewMode::Always;
    QTest::newRow("osc8") << LinkPreviewMode::Osc8;
}

void LaunchOptionsTest::mapsLinkPreviewModes()
{
    QFETCH(LinkPreviewMode, configured);

    LaunchOptions base;
    base.linkPreviews = LinkPreviewMode::Never;
    GhosttyConfigSnapshot snapshot = completeSnapshot();
    snapshot.values.linkPreviews = configured;

    QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot).linkPreviews,
             configured);
}

void LaunchOptionsTest::mapsClipboardModes()
{
    for (const TerminalCopyOnSelectMode configured : {
             TerminalCopyOnSelectMode::Disabled,
             TerminalCopyOnSelectMode::Primary,
             TerminalCopyOnSelectMode::PrimaryAndClipboard,
         }) {
        GhosttyConfigSnapshot snapshot = completeSnapshot();
        snapshot.values.selectionClipboard.copyOnSelect = configured;
        QCOMPARE(applyGhosttyConfigSnapshot({}, snapshot)
                     .selectionClipboard.copyOnSelect,
                 configured);
    }

    for (const MiddleClickAction configured : {
             MiddleClickAction::PrimaryPaste,
             MiddleClickAction::Ignore,
         }) {
        GhosttyConfigSnapshot snapshot = completeSnapshot();
        snapshot.values.middleClickAction = configured;
        QCOMPARE(applyGhosttyConfigSnapshot({}, snapshot).middleClickAction,
                 configured);
    }

    for (const bool enabled : {false, true}) {
        LaunchOptions base;
        base.mouseReporting = !enabled;
        GhosttyConfigSnapshot snapshot = completeSnapshot();
        snapshot.values.mouseReporting = enabled;
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

    GhosttyConfigSnapshot snapshot = completeSnapshot();
    snapshot.values.workingDirectoryPath =
        QStringLiteral("/base/link/../target");
    snapshot.values.splitInheritWorkingDirectory = false;
    snapshot.values.tabInheritWorkingDirectory = false;
    snapshot.values.windowInheritWorkingDirectory = false;
    snapshot.values.windowInheritFontSize = false;

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
    snapshot.values.workingDirectoryPath.reset();
    result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.workingDirectory, QStringLiteral("/launch-directory"));
    QVERIFY(result.inheritWorkingDirectory);
}

void LaunchOptionsTest::mapsSplitPreserveZoom()
{
    LaunchOptions base;
    base.splitPreserveZoomNavigation = false;

    GhosttyConfigSnapshot snapshot = completeSnapshot();
    snapshot.values.splitPreserveZoom = true;
    QVERIFY(applyGhosttyConfigSnapshot(base, snapshot)
                .splitPreserveZoomNavigation);

    snapshot.values.splitPreserveZoom = false;
    QVERIFY(!applyGhosttyConfigSnapshot(base, snapshot)
                 .splitPreserveZoomNavigation);
}

void LaunchOptionsTest::mapsNewTabPosition()
{
    LaunchOptions base;
    base.windowNewTabPosition = WindowNewTabPosition::End;

    GhosttyConfigSnapshot snapshot = completeSnapshot();
    snapshot.values.windowNewTabPosition = WindowNewTabPosition::Current;
    QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot).windowNewTabPosition,
             WindowNewTabPosition::Current);

    snapshot.values.windowNewTabPosition = WindowNewTabPosition::End;
    QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot).windowNewTabPosition,
             WindowNewTabPosition::End);
}

void LaunchOptionsTest::mapsWindowShowTabBar()
{
    LaunchOptions base;
    base.windowShowTabBar = WindowShowTabBar::Never;
    GhosttyConfigSnapshot snapshot = completeSnapshot();
    for (const WindowShowTabBar mode : {
             WindowShowTabBar::Always,
             WindowShowTabBar::Auto,
             WindowShowTabBar::Never,
         }) {
        snapshot.values.windowShowTabBar = mode;
        QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot).windowShowTabBar,
                 mode);
    }
}

void LaunchOptionsTest::mapsWindowDecoration()
{
    LaunchOptions base;
    base.windowDecoration = WindowDecorationMode::None;
    GhosttyConfigSnapshot snapshot = completeSnapshot();
    for (const WindowDecorationMode mode : {
             WindowDecorationMode::Auto,
             WindowDecorationMode::Client,
             WindowDecorationMode::Server,
             WindowDecorationMode::None,
         }) {
        snapshot.values.windowDecoration = mode;
        QCOMPARE(applyGhosttyConfigSnapshot(base, snapshot).windowDecoration,
                 mode);
    }
}

void LaunchOptionsTest::mapsWindowCellDimensions()
{
    LaunchOptions base;
    base.windowWidth = 91;
    base.windowHeight = 31;

    GhosttyConfigSnapshot snapshot = completeSnapshot();
    snapshot.values.windowWidth = 120;
    snapshot.values.windowHeight = 40;
    LaunchOptions result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.windowWidth, quint32(120));
    QCOMPARE(result.windowHeight, quint32(40));

    snapshot.values.windowWidth = 0;
    snapshot.values.windowHeight = 0;
    result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.windowWidth, quint32(0));
    QCOMPARE(result.windowHeight, quint32(0));

    snapshot.values.windowWidth = 10;
    snapshot.values.windowHeight = 4;
    result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.windowWidth, quint32(10));
    QCOMPARE(result.windowHeight, quint32(4));

    snapshot.values.windowWidth = std::numeric_limits<quint32>::max();
    snapshot.values.windowHeight = std::numeric_limits<quint32>::max();
    result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.windowWidth, std::numeric_limits<quint32>::max());
    QCOMPARE(result.windowHeight, std::numeric_limits<quint32>::max());
}

void LaunchOptionsTest::mapsResizeOverlay()
{
    LaunchOptions base;
    base.resizeOverlay = {
        .mode = ResizeOverlayMode::Never,
        .position = ResizeOverlayPosition::TopLeft,
        .duration = std::chrono::milliseconds(900),
    };

    GhosttyConfigSnapshot snapshot = completeSnapshot();
    snapshot.values.resizeOverlay = {
        .mode = ResizeOverlayMode::Always,
        .position = ResizeOverlayPosition::BottomRight,
        .duration = std::chrono::milliseconds(1'234),
    };
    LaunchOptions result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.resizeOverlay.mode, ResizeOverlayMode::Always);
    QCOMPARE(result.resizeOverlay.position,
             ResizeOverlayPosition::BottomRight);
    QCOMPARE(result.resizeOverlay.duration,
             std::chrono::milliseconds(1'234));

    snapshot.values.resizeOverlay = {
        .mode = ResizeOverlayMode::AfterFirst,
        .position = ResizeOverlayPosition::TopCenter,
        .duration = std::chrono::milliseconds(250),
    };
    result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.resizeOverlay.mode, ResizeOverlayMode::AfterFirst);
    QCOMPARE(result.resizeOverlay.position,
             ResizeOverlayPosition::TopCenter);
    QCOMPARE(result.resizeOverlay.duration, std::chrono::milliseconds(250));
}

void LaunchOptionsTest::mapsStartupWindowState()
{
    LaunchOptions base;
    base.maximize = true;
    base.fullscreen = true;

    GhosttyConfigSnapshot snapshot = completeSnapshot();
    snapshot.values.maximize = false;
    snapshot.values.fullscreen = GhosttyFullscreenMode::Disabled;
    LaunchOptions result = applyGhosttyConfigSnapshot(base, snapshot);
    QVERIFY(!result.maximize);
    QVERIFY(!result.fullscreen);

    snapshot.values.maximize = true;
    for (const GhosttyFullscreenMode mode : {
             GhosttyFullscreenMode::Enabled,
             GhosttyFullscreenMode::NonNative,
             GhosttyFullscreenMode::NonNativeVisibleMenu,
             GhosttyFullscreenMode::NonNativePaddedNotch,
         }) {
        snapshot.values.fullscreen = mode;
        result = applyGhosttyConfigSnapshot(base, snapshot);
        QVERIFY(result.maximize);
        QVERIFY(result.fullscreen);
    }
}

void LaunchOptionsTest::mapsApplicationLifetime()
{
    LaunchOptions base;
    GhosttyConfigSnapshot snapshot = completeSnapshot();
    snapshot.values.quitAfterLastWindowClosed = false;
    snapshot.values.quitAfterLastWindowClosedDelay =
        std::chrono::milliseconds(1'500);
    snapshot.values.initialWindow = false;

    LaunchOptions result = applyGhosttyConfigSnapshot(base, snapshot);
    QVERIFY(!result.quitAfterLastWindowClosed);
    QCOMPARE(result.quitAfterLastWindowClosedDelay,
             std::optional(std::chrono::milliseconds(1'500)));
    QVERIFY(!result.initialWindow);

    snapshot.values.quitAfterLastWindowClosedDelay =
        std::chrono::milliseconds::zero();
    result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.quitAfterLastWindowClosedDelay,
             std::optional(std::chrono::milliseconds::zero()));

    snapshot.values.quitAfterLastWindowClosedDelay.reset();
    result = applyGhosttyConfigSnapshot(base, snapshot);
    QVERIFY(!result.quitAfterLastWindowClosedDelay.has_value());

    base.initialWindow = true;
    base.initialWindowExplicit = true;
    base.program = {QStringLiteral("/bin/true")};
    snapshot.values.quitAfterLastWindowClosedDelay =
        std::chrono::milliseconds(75);
    result = applyGhosttyConfigSnapshot(base, snapshot);
    QVERIFY(result.quitAfterLastWindowClosed);
    QVERIFY(!result.quitAfterLastWindowClosedDelay.has_value());
    QVERIFY(result.initialWindow);
}

void LaunchOptionsTest::mapsSingleInstancePolicy()
{
    LaunchOptions base;
    FrontendConfigSnapshot snapshot;

    snapshot.values.singleInstanceMode = SingleInstanceMode::Enabled;
    LaunchOptions options = applyFrontendConfigSnapshot(base, snapshot);
    QCOMPARE(options.singleInstanceMode, SingleInstanceMode::Enabled);
    QVERIFY(shouldUseSingleInstance(options, QByteArrayView("ghostty")));
    // Forwarding command-line payloads is deliberately outside this first
    // protocol even when the config explicitly enables process uniqueness.
    options.hasUnforwardedLaunchPayload = true;
    QVERIFY(!shouldUseSingleInstance(options, QByteArrayView{}));
    options.hasUnforwardedLaunchPayload = false;

    snapshot.values.singleInstanceMode = SingleInstanceMode::Disabled;
    options = applyFrontendConfigSnapshot(base, snapshot);
    QCOMPARE(options.singleInstanceMode, SingleInstanceMode::Disabled);
    QVERIFY(!shouldUseSingleInstance(options, QByteArrayView{}));

    snapshot.values.singleInstanceMode = SingleInstanceMode::Detect;
    options = applyFrontendConfigSnapshot(base, snapshot);
    QCOMPARE(options.singleInstanceMode, SingleInstanceMode::Detect);
    QVERIFY(shouldUseSingleInstance(options, QByteArrayView{}));
    QVERIFY(!shouldUseSingleInstance(options, QByteArrayView("ghostty")));
    options.hasUnforwardedLaunchPayload = true;
    QVERIFY(!shouldUseSingleInstance(options, QByteArrayView{}));
}

void LaunchOptionsTest::mapsFrontendConfigurationPrecedence()
{
    const auto parsed = parseLaunchOptions({QStringLiteral("ghostty-qt")});
    QVERIFY2(parsed.has_value(), qPrintable(errorMessage(parsed)));

    const LaunchOptions builtIns =
        resolveLaunchOptions(*parsed, nullptr, nullptr);
    QCOMPARE(builtIns.tabsLocation, TabsLocation::Top);
    QCOMPARE(builtIns.singleInstanceMode, SingleInstanceMode::Detect);

    GhosttyConfigSnapshot shared = completeSnapshot();
    shared.values.windowDecoration = WindowDecorationMode::None;
    // This legacy shared value is intentionally ignored by the frontend
    // resolution seam.
    shared.values.singleInstanceMode = SingleInstanceMode::Enabled;
    const LaunchOptions sharedOnly =
        resolveLaunchOptions(*parsed, &shared, nullptr);
    QCOMPARE(sharedOnly.windowDecoration, WindowDecorationMode::None);
    QCOMPARE(sharedOnly.tabsLocation, TabsLocation::Top);
    QCOMPARE(sharedOnly.singleInstanceMode, SingleInstanceMode::Detect);

    FrontendConfigSnapshot frontend;
    frontend.values.tabsLocation = TabsLocation::Bottom;
    frontend.values.singleInstanceMode = SingleInstanceMode::Disabled;
    const LaunchOptions configured =
        resolveLaunchOptions(*parsed, &shared, &frontend);
    QCOMPARE(configured.windowDecoration, WindowDecorationMode::None);
    QCOMPARE(configured.tabsLocation, TabsLocation::Bottom);
    QCOMPARE(configured.singleInstanceMode, SingleInstanceMode::Disabled);

    const auto explicitCli = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--single-instance=true"),
    });
    QVERIFY2(explicitCli.has_value(), qPrintable(errorMessage(explicitCli)));
    const LaunchOptions overridden =
        resolveLaunchOptions(*explicitCli, &shared, &frontend);
    QCOMPARE(overridden.tabsLocation, TabsLocation::Bottom);
    QCOMPARE(overridden.singleInstanceMode, SingleInstanceMode::Enabled);
    QVERIFY(overridden.singleInstanceModeExplicit);

    frontend.values.tabsLocation = TabsLocation::Top;
    QCOMPARE(applyFrontendConfigSnapshot(*parsed, frontend).tabsLocation,
             TabsLocation::Top);
}

void LaunchOptionsTest::mapsUnfocusedSplitAppearance()
{
    LaunchOptions base;
    base.splitAppearance = {
        .unfocusedOpacity = 0.4,
        .unfocusedFill = QColor(Qt::red),
    };

    GhosttyConfigSnapshot snapshot = completeSnapshot();
    snapshot.values.splitAppearance = {
        .unfocusedOpacity = 0.15,
        .unfocusedFill = QColor(QStringLiteral("#123456")),
    };
    LaunchOptions result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.splitAppearance.unfocusedOpacity, 0.15);
    QCOMPARE(result.splitAppearance.unfocusedFill,
             std::optional<QColor>(QColor(QStringLiteral("#123456"))));

    snapshot.values.splitAppearance = {
        .unfocusedOpacity = 1.0,
        .unfocusedFill = std::nullopt,
    };
    result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.splitAppearance.unfocusedOpacity, 1.0);
    QVERIFY(!result.splitAppearance.unfocusedFill.has_value());
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

    GhosttyConfigSnapshot snapshot = completeSnapshot();
    snapshot.values.appearance.selectionForeground.reset();
    snapshot.values.appearance.selectionBackground.reset();
    snapshot.values.appearance.cursorColor.reset();
    snapshot.values.appearance.cursorBlink.reset();
    snapshot.values.appearance.cursorText.reset();
    snapshot.values.appearance.boldColor.reset();
    snapshot.values.splitAppearance.unfocusedFill.reset();
    snapshot.values.splitAppearance.dividerColor.reset();

    const LaunchOptions result = applyGhosttyConfigSnapshot(base, snapshot);
    const TerminalAppearance &appearance = result.appearance;
    QCOMPARE(appearance.selectionForeground.kind, TerminalColorKind::Unset);
    QCOMPARE(appearance.selectionBackground.kind, TerminalColorKind::Unset);
    QCOMPARE(appearance.cursorColor.kind, TerminalColorKind::Unset);
    QVERIFY(!appearance.cursorBlink.has_value());
    QCOMPARE(appearance.cursorTextColor.kind, TerminalColorKind::Unset);
    QCOMPARE(appearance.boldColor.kind, TerminalBoldColorKind::Unset);
    QVERIFY(!result.splitAppearance.unfocusedFill.has_value());
    QVERIFY(!result.splitAppearance.dividerColor.has_value());
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
    options.selectionWordChars = {0, 0x20, 0x2502, 0x1f642};
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
    QCOMPARE(runtime.selectionWordChars, options.selectionWordChars);
    QCOMPARE(runtime.clipboardPaste, options.clipboardPaste);
    QCOMPARE(runtime.linkUrl, options.linkUrl);
    QCOMPARE(launch.workingDirectory, options.workingDirectory);
    QCOMPARE(launch.inheritWorkingDirectory,
             options.inheritWorkingDirectory);
    QCOMPARE(launch.program, options.program);
    QCOMPARE(launch.scrollbackLimit, options.scrollbackLimit);
    QCOMPARE(launch.hold, options.hold);
    QVERIFY(!launch.initialGeometry.has_value());
    QCOMPARE(launch.runtime, runtime);

    QVERIFY(QMetaType::fromType<TerminalSessionLaunchOptions>().isValid());
    QVERIFY(QMetaType::fromType<TerminalSessionRuntimeOptions>().isValid());
    QVERIFY(qvariant_cast<TerminalSessionLaunchOptions>(
                QVariant::fromValue(launch)) == launch);
    QVERIFY(qvariant_cast<TerminalSessionRuntimeOptions>(
                QVariant::fromValue(runtime)) == runtime);

    LaunchOptions frontendOnlyChanged = options;
    frontendOnlyChanged.typography.face(TerminalFontRole::Regular).families = {
        QStringLiteral("Frontend Font"),
    };
    frontendOnlyChanged.typography.pointSize = 19.0;
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
    frontendOnlyChanged.windowDecoration = WindowDecorationMode::None;
    frontendOnlyChanged.windowWidth = 132;
    frontendOnlyChanged.windowHeight = 43;
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
    frontendOnlyChanged.keybindSource = GhosttyKeybindSource::text(
        {QStringLiteral("ctrl+x=ignore")});
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

void LaunchOptionsTest::removesOnlyTheInitialCommand()
{
    LaunchOptions options;
    options.workingDirectory = QStringLiteral("/configured");
    options.typography.pointSize = 17.0;
    options.maximize = true;
    options.program = {
        QStringLiteral("command"), QStringLiteral("argument"),
    };
    options.hold = true;

    LaunchOptions expected = options;
    expected.program.clear();
    expected.hold = false;
    QVERIFY(withoutInitialCommand(options) == expected);
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
