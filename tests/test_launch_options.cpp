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

QString errorMessage(const std::expected<LaunchOptions, QString> &result)
{
    return result ? QString{} : result.error();
}

QVector<QColor> testPalette()
{
    QVector<QColor> palette;
    palette.reserve(256);
    for (int index = 0; index < 256; ++index) {
        palette.append(QColor::fromRgb(index, 255 - index, index / 2));
    }
    return palette;
}

TerminalTypography completeTypography()
{
    TerminalTypography typography;
    typography.face(TerminalFontRole::Regular) = {
        .families =
            {
                QStringLiteral("Config Primary"),
                QStringLiteral("Config Fallback"),
            },
        .style = TerminalFontStyles::Named{QStringLiteral("Book")},
    };
    typography.face(TerminalFontRole::Bold) = {
        .families =
            {
                QStringLiteral("Config Bold"),
                QStringLiteral("Config Bold Fallback"),
            },
        .style = TerminalFontStyles::Disabled{},
    };
    typography.face(TerminalFontRole::Italic) = {
        .families =
            {
                QStringLiteral("Config Italic"),
            },
        .style = TerminalFontStyles::Automatic{},
    };
    typography.face(TerminalFontRole::BoldItalic) = {
        .families =
            {
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
    values.term = QByteArrayLiteral("ghostty-qt-configured");
    values.enquiryResponse = QByteArray::fromHex("000580ff");
    values.ordinaryCommand =
        TerminalCommand::shell(QByteArrayLiteral("printf 'ordinary command'"));
    values.initialCommand = TerminalCommand::direct({
        QByteArrayLiteral("/bin/printf"),
        QByteArray::fromHex("ff80617267"),
        QByteArray{},
    });
    values.abnormalCommandExitRuntimeMilliseconds = 731;
    values.waitAfterCommand = true;
    values.environment = {
        {
            .key = QByteArrayLiteral("GHOSTTY_QT_ENV"),
            .value = QByteArrayLiteral("configured"),
        },
        {
            .key = QByteArrayLiteral("RAW_VALUE"),
            .value = QByteArray::fromHex("ff8061"),
        },
    };
    values.shellIntegration = GhosttyShellIntegrationMode::Nushell;
    values.shellIntegrationFeatures = {
        .cursor = false,
        .sudo = true,
        .title = false,
        .sshEnvironment = true,
        .sshTerminfo = true,
        .path = false,
    };
    values.linuxCgroup = {
        .mode = LinuxCgroupMode::Always,
        .memoryLimitBytes = std::numeric_limits<quint64>::max(),
        .processesLimit = quint64{0},
        .hardFail = true,
    };
    values.workingDirectoryPath = QStringLiteral("/work/ghostty");
    values.typography = completeTypography();
    values.configDefaultFiles = false;
    values.title = QStringLiteral("Configured title");

    values.appearance = {
        .foregroundColor = QColor(QStringLiteral("#112233")),
        .backgroundColor = QColor(QStringLiteral("#445566")),
        .palette = testPalette(),
        .selectionForeground = {.kind = TerminalColorKind::CellForeground},
        .selectionBackground =
            TerminalColorValue::fromColor(QColor(QStringLiteral("#223344"))),
        .searchForeground = {.kind = TerminalColorKind::CellBackground},
        .searchBackground =
            TerminalColorValue::fromColor(QColor(QStringLiteral("#123456"))),
        .searchSelectedForeground = {.kind = TerminalColorKind::CellForeground},
        .searchSelectedBackground =
            TerminalColorValue::fromColor(QColor(QStringLiteral("#654321"))),
        .cursorColor = {.kind = TerminalColorKind::CellBackground},
        .cursorStyle = TerminalCursorStyle::BlockHollow,
        .cursorBlink = false,
        .cursorOpacity = 0.625,
        .cursorTextColor = {.kind = TerminalColorKind::CellForeground},
        .boldColor =
            {
                .kind = TerminalBoldColorKind::Color,
                .color = QColor(QStringLiteral("#abcdef")),
            },
        .faintOpacity = 0.375,
        .minimumContrast = 4.25,
    };
    values.background = {
        .opacity = 0.375,
        .opacityCells = true,
        .image =
            {
                .path =
                    GhosttyConfigPath{
                        .path = QStringLiteral("/work/background.png"),
                        .optional = true,
                    },
                .opacity = 1.25,
                .position = TerminalBackgroundImagePosition::BottomRight,
                .fit = TerminalBackgroundImageFit::Cover,
                .repeat = true,
            },
    };
    values.backgroundBlur = 42;
    values.customShaders = {
        .sources =
            {
                {.path = QStringLiteral("/work/shaders/first.glsl")},
                {.path = QStringLiteral("/work/shaders/second.glsl"),
                 .optional = true},
            },
        .animation = TerminalCustomShaderAnimation::Always,
    };
    values.padding = {
        .horizontal = {.leadingPoints = 3, .trailingPoints = 5},
        .vertical = {.leadingPoints = 7, .trailingPoints = 11},
        .balance = TerminalPaddingBalance::Equal,
        .color = TerminalPaddingColor::ExtendAlways,
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
    values.windowAppearance = {
        .theme = WindowTheme::Ghostty,
        .titleFontFamily = QStringLiteral("Window Font"),
        .titlebarBackground = QColor(QStringLiteral("#102030")),
        .titlebarForeground = QColor(QStringLiteral("#f0e0d0")),
        .subtitle = WindowSubtitleMode::WorkingDirectory,
    };
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
    values.kittyImageStorageLimitBytes = 123'456'789;
    values.scrollbackCompression = false;
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
        .codepointMap =
            {
                {.first = 0x2500, .last = 0x257f, .replacement = quint32{'-'}},
                {.first = 0x2500,
                 .last = 0x2500,
                 .replacement = QStringLiteral("line")},
            },
    };
    values.clipboardPaste = {
        .protection = false,
        .bracketedSafe = true,
    };
    values.clipboardWrite = TerminalClipboardAccess::Ask;
    values.scrollToBottom = {
        .keystroke = false,
        .output = true,
    };
    values.rightClickAction = RightClickAction::CopyOrPaste;
    values.middleClickAction = MiddleClickAction::Ignore;
    values.mouseReporting = false;
    values.mouseShiftCapture = MouseShiftCapture::Never;
    values.mouseHideWhileTyping = true;
    values.focusFollowsMouse = true;
    values.selectionWordChars = {0, 0x20, 0x2502, 0x1f642};
    values.clickRepeatIntervalMilliseconds = 731;
    values.mouseScrollMultiplier = {
        .precision = 0.75,
        .discrete = 4.5,
    };
    values.vtKamAllowed = true;
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
    void parsesDesktopTerminalLaunchOptions();
    void parsesGhosttyDashECommand();
    void preservesDashEAfterDoubleDashAndInInlineValues();
    void rejectsInvalidDashEUsage();
    void preservesSymlinkSensitiveExplicitWorkingDirectory();
    void rejectsInvalidWorkingDirectory();
    void rejectsFileAsWorkingDirectory();
    void rejectsInvalidFontSize_data();
    void rejectsInvalidFontSize();
    void normalizesFontSizeToGhosttyPrecision();
    void buildsGhosttyConfigurationArguments();
    void rejectsInvalidScrollbackLines_data();
    void rejectsInvalidScrollbackLines();
    void rejectsUnknownOption();
    void rejectsMissingApplicationName();
    void appliesFinalizedGhosttyTypography();
    void mapsScrollbarPolicy();
    void mapsBellFeatures();
    void mapsBellAudio();
    void mapsBackdropAndPaddingSnapshots();
    void mapsMouseHideWhileTyping();
    void mapsFocusFollowsMouse();
    void mapsSelectionWordChars();
    void mapsClickRepeatInterval();
    void mapsMouseScrollMultiplier();
    void mapsScrollToBottom();
    void mapsLinkPreviewModes();
    void mapsLinkPreviewModes_data();
    void mapsClipboardModes();
    void mapsWorkingDirectoryAndSurfaceInheritance();
    void mapsSplitPreserveZoom();
    void mapsNewTabPosition();
    void mapsWindowShowTabBar();
    void mapsWindowDecoration();
    void mapsTitleAndWindowAppearance();
    void mapsWindowCellDimensions();
    void mapsResizeOverlay();
    void mapsStartupWindowState();
    void mapsApplicationLifetime();
    void mapsSingleInstancePolicy();
    void mapsFrontendConfigurationPrecedence();
    void mapsUnfocusedSplitAppearance();
    void restoresNullableAppearanceDefaults();
    void removesOnlyTheInitialCommand();
    void materializesMissingFinalizedCommandFallback();
    void projectsTerminalSessionOptions();
    void convertsLegacyLineCapacityToLibghosttyBytes();
    void mapsCloseConfirmationModes();
    void keepsBackdropGuiOwned();
};

void LaunchOptionsTest::defaults()
{
    const auto result = parseLaunchOptions({QStringLiteral("ghostty-qt")});
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    const LaunchOptions &options = *result;
    QCOMPARE(options.term, QByteArrayLiteral("xterm-ghostty"));
    QVERIFY(options.enquiryResponse.isEmpty());
    QVERIFY(!options.ordinaryCommand.has_value());
    QVERIFY(!options.initialCommand.has_value());
    QCOMPARE(options.abnormalCommandExitRuntimeMilliseconds, quint32{250});
    QVERIFY(!options.waitAfterCommand);
    QVERIFY(!options.waitAfterCommandExplicit);
    QVERIFY(options.environment.isEmpty());
    QCOMPARE(options.shellIntegration, GhosttyShellIntegrationMode::None);
    QVERIFY(options.shellIntegrationFeatures.cursor);
    QVERIFY(!options.shellIntegrationFeatures.sudo);
    QVERIFY(options.shellIntegrationFeatures.title);
    QVERIFY(!options.shellIntegrationFeatures.sshEnvironment);
    QVERIFY(!options.shellIntegrationFeatures.sshTerminfo);
    QVERIFY(options.shellIntegrationFeatures.path);
    QVERIFY(!options.shellIntegrationAvailable);
    QCOMPARE(options.linuxCgroup.mode, LinuxCgroupMode::SingleInstance);
    QVERIFY(!options.linuxCgroup.memoryLimitBytes.has_value());
    QVERIFY(!options.linuxCgroup.processesLimit.has_value());
    QVERIFY(!options.linuxCgroup.hardFail);
    QVERIFY(!options.processUsesSingleInstance);
    QCOMPARE(options.workingDirectory, QDir::currentPath());
    QVERIFY(options.inheritWorkingDirectory);
    QVERIFY(!options.workingDirectoryExplicit);
    QVERIFY(regularFamilies(options).isEmpty());
    QCOMPARE(options.typography.pointSize, 12.0);
    for (const TerminalFontFace &face : options.typography.faces) {
        QVERIFY(face.families.isEmpty());
        QVERIFY(
            std::holds_alternative<TerminalFontStyles::Automatic>(face.style));
    }
    QVERIFY(std::ranges::all_of(
        options.typography.metricModifiers.values,
        [](const TerminalMetricModifierSet::Value &modifier) {
            return !modifier.has_value();
        }));
    QVERIFY(!options.fontFamilyExplicit);
    QVERIFY(!options.fontSizeExplicit);
    QVERIFY(options.configDefaultFiles);
    QVERIFY(!options.configDefaultFilesExplicit);
    QCOMPARE(options.colorScheme, TerminalColorScheme::Light);
    QVERIFY(!options.configuredTitle.has_value());
    QVERIFY(!options.configuredTitleExplicit);
    QVERIFY(!options.applicationClass.has_value());
    QVERIFY(!options.applicationClassExplicit);
    QCOMPARE(options.windowAppearance.theme, WindowTheme::Auto);
    QVERIFY(!options.windowAppearance.titleFontFamily.has_value());
    QVERIFY(!options.windowAppearance.titlebarBackground.has_value());
    QVERIFY(!options.windowAppearance.titlebarForeground.has_value());
    QCOMPARE(options.windowAppearance.subtitle, WindowSubtitleMode::Disabled);
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
    QCOMPARE(options.appearance.minimumContrast, 1.0);
    QCOMPARE(options.background.opacity, 1.0);
    QVERIFY(!options.background.opacityCells);
    QVERIFY(!options.background.image.path.has_value());
    QCOMPARE(options.background.image.opacity, 1.0);
    QCOMPARE(options.background.image.position,
             TerminalBackgroundImagePosition::Center);
    QCOMPARE(options.background.image.fit, TerminalBackgroundImageFit::Contain);
    QVERIFY(!options.background.image.repeat);
    QVERIFY(options.customShaders.sources.isEmpty());
    QCOMPARE(options.customShaders.animation,
             TerminalCustomShaderAnimation::Focused);
    QCOMPARE(options.backgroundBlur, qint16{0});
    QCOMPARE(options.padding.horizontal.leadingPoints, quint32(0));
    QCOMPARE(options.padding.horizontal.trailingPoints, quint32(0));
    QCOMPARE(options.padding.vertical.leadingPoints, quint32(0));
    QCOMPARE(options.padding.vertical.trailingPoints, quint32(0));
    QCOMPARE(options.padding.balance, TerminalPaddingBalance::Disabled);
    QCOMPARE(options.padding.color, TerminalPaddingColor::Background);
    QCOMPARE(options.scrollbackLimit.value, quint64(10'000));
    QCOMPARE(options.scrollbackLimit.unit, ScrollbackLimitUnit::Lines);
    QVERIFY(!options.scrollbackLimitExplicit);
    QVERIFY(options.scrollbackCompression);
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
    QCOMPARE(options.clickRepeatIntervalMilliseconds, quint32{500});
    QCOMPARE(options.mouseScrollMultiplier.precision, 1.0);
    QCOMPARE(options.mouseScrollMultiplier.discrete, 3.0);
    QVERIFY(!options.vtKamAllowed);
    QCOMPARE(options.confirmCloseMode, ConfirmCloseMode::RunningProcesses);
    QVERIFY(options.selectionClipboard.trimTrailingSpaces);
    QCOMPARE(options.selectionClipboard.copyOnSelect,
             TerminalCopyOnSelectMode::Primary);
    QVERIFY(options.selectionClipboard.clearOnTyping);
    QVERIFY(!options.selectionClipboard.clearOnCopy);
    QVERIFY(options.clipboardPaste.protection);
    QVERIFY(options.clipboardPaste.bracketedSafe);
    QCOMPARE(options.clipboardWrite, TerminalClipboardAccess::Allow);
    QVERIFY(options.scrollToBottom.keystroke);
    QVERIFY(!options.scrollToBottom.output);
    QCOMPARE(options.splitAppearance.unfocusedOpacity, 0.7);
    QVERIFY(!options.splitAppearance.unfocusedFill.has_value());
    QVERIFY(!options.splitAppearance.dividerColor.has_value());
    QVERIFY(options.splitInheritWorkingDirectory);
    QVERIFY(!options.splitPreserveZoomNavigation);
    QVERIFY(options.tabInheritWorkingDirectory);
    QVERIFY(options.windowInheritWorkingDirectory);
    QVERIFY(options.windowInheritFontSize);
    QCOMPARE(options.windowNewTabPosition, WindowNewTabPosition::Current);
    QCOMPARE(options.windowShowTabBar, WindowShowTabBar::Auto);
    QCOMPARE(options.tabsLocation, TabsLocation::Top);
    QVERIFY(options.wideTabs);
    QVERIFY(options.horizontalTabScroll);
    QCOMPARE(options.quickTerminalLayerShell.layer, QuickTerminalLayer::Top);
    QCOMPARE(options.quickTerminalLayerShell.layerNamespace,
             QStringLiteral("ghostty-quick-terminal"));
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
    QVERIFY(!options.hasUnforwardedLaunchPayload());
    QCOMPARE(options.singleInstanceMode, SingleInstanceMode::Detect);
    QVERIFY(!options.singleInstanceModeExplicit);
    QCOMPARE(options.rightClickAction, RightClickAction::ContextMenu);
    QCOMPARE(options.middleClickAction, MiddleClickAction::PrimaryPaste);
    QCOMPARE(options.mouseShiftCapture, MouseShiftCapture::False);
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
    QVERIFY(!service->hasUnforwardedLaunchPayload());
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
        const auto invalid =
            parseLaunchOptions({QStringLiteral("ghostty-qt"), argument});
        QVERIFY(!invalid.has_value());
        QVERIFY(invalid.error().contains(QStringLiteral("Invalid")));
    }

    const auto payload = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--single-instance=true"),
        QStringLiteral("--font-size=14"),
    });
    QVERIFY2(payload.has_value(), qPrintable(errorMessage(payload)));
    QVERIFY(payload->hasUnforwardedLaunchPayload());
    QVERIFY(!shouldUseSingleInstance(*payload, QByteArrayView{}));

    const auto noDefaultConfig = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--config-default-files=true"),
        QStringLiteral("--config-default-files=f"),
    });
    QVERIFY2(noDefaultConfig.has_value(),
             qPrintable(errorMessage(noDefaultConfig)));
    QVERIFY(!noDefaultConfig->configDefaultFiles);
    QVERIFY(noDefaultConfig->configDefaultFilesExplicit);
    QVERIFY(noDefaultConfig->hasUnforwardedLaunchPayload());
    QCOMPARE(ghosttyConfigurationArguments(*noDefaultConfig),
             QStringList{QStringLiteral("--config-default-files=false")});

    const auto explicitDefaultConfig = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--config-default-files=T"),
    });
    QVERIFY2(explicitDefaultConfig.has_value(),
             qPrintable(errorMessage(explicitDefaultConfig)));
    QVERIFY(explicitDefaultConfig->configDefaultFiles);
    QVERIFY(explicitDefaultConfig->configDefaultFilesExplicit);
    QVERIFY(explicitDefaultConfig->hasUnforwardedLaunchPayload());
    QCOMPARE(ghosttyConfigurationArguments(*explicitDefaultConfig),
             QStringList{QStringLiteral("--config-default-files=true")});

    const auto invalidDefaultConfig = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--config-default-files=yes"),
    });
    QVERIFY(!invalidDefaultConfig.has_value());
    QVERIFY(invalidDefaultConfig.error().contains(
        QStringLiteral("Invalid config-default-files")));
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
        QString::fromUtf8("--class=com.example.\xE5\xAE\xB6\xF0\x9F\x91\xBB"),
        QStringLiteral("--config-default-files=false"),
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
    QCOMPARE(options.applicationClass,
             std::optional<QByteArray>(QByteArray::fromHex(
                 "636f6d2e6578616d706c652ee5aeb6f09f91bb")));
    QVERIFY(options.applicationClassExplicit);
    QVERIFY(ghosttyConfigurationArguments(options).contains(
        QString::fromUtf8("--class=com.example.\xE5\xAE\xB6\xF0\x9F\x91\xBB")));
    QVERIFY(!options.configDefaultFiles);
    QVERIFY(options.configDefaultFilesExplicit);
    QCOMPARE(options.scrollbackLimit.value, quint64(250'000));
    QCOMPARE(options.scrollbackLimit.unit, ScrollbackLimitUnit::Lines);
    QVERIFY(options.scrollbackLimitExplicit);
    QVERIFY(options.hold);
    QVERIFY(options.hasUnforwardedLaunchPayload());
    QCOMPARE(options.program,
             QStringList({QStringLiteral("/bin/sh"), QStringLiteral("-lc"),
                          QStringLiteral("printf hello")}));
}

void LaunchOptionsTest::parsesDesktopTerminalLaunchOptions()
{
    const QString spacedTitle = QStringLiteral("  terminal title  ");
    const auto result = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--title=ignored"),
        QStringLiteral("--title=") + spacedTitle,
        QStringLiteral("--wait-after-command"),
    });
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->configuredTitle, std::optional<QString>(spacedTitle));
    QVERIFY(result->configuredTitleExplicit);
    QVERIFY(result->waitAfterCommand);
    QVERIFY(result->waitAfterCommandExplicit);
    QVERIFY(result->hasUnforwardedLaunchPayload());
    QVERIFY(!shouldUseSingleInstance(*result, QByteArrayView{}));
    QCOMPARE(ghosttyConfigurationArguments(*result),
             QStringList({QStringLiteral("--title=") + spacedTitle,
                          QStringLiteral("--wait-after-command=true")}));

    GhosttyConfigSnapshot snapshot = completeSnapshot();
    const LaunchOptions configured =
        applyGhosttyConfigSnapshot(*result, snapshot);
    QVERIFY(configured.configuredTitleExplicit);
    QVERIFY(configured.waitAfterCommandExplicit);

    const auto emptyTitle = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--title=previous"),
        QStringLiteral("--title="),
    });
    QVERIFY2(emptyTitle.has_value(), qPrintable(errorMessage(emptyTitle)));
    QVERIFY(!emptyTitle->configuredTitle.has_value());
    QVERIFY(emptyTitle->configuredTitleExplicit);
    QVERIFY(!emptyTitle->waitAfterCommand);
    QVERIFY(!emptyTitle->waitAfterCommandExplicit);
    QVERIFY(emptyTitle->hasUnforwardedLaunchPayload());
    QCOMPARE(ghosttyConfigurationArguments(*emptyTitle),
             QStringList{QStringLiteral("--title=")});

    const auto disabledWait = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--wait-after-command"),
        QStringLiteral("--wait-after-command=false"),
    });
    QVERIFY2(disabledWait.has_value(), qPrintable(errorMessage(disabledWait)));
    QVERIFY(!disabledWait->waitAfterCommand);
    QVERIFY(disabledWait->waitAfterCommandExplicit);
    QVERIFY(disabledWait->hasUnforwardedLaunchPayload());
    QCOMPARE(ghosttyConfigurationArguments(*disabledWait),
             QStringList{QStringLiteral("--wait-after-command=false")});

    const auto invalidWait = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--wait-after-command=yes"),
    });
    QVERIFY(!invalidWait.has_value());
    QVERIFY(invalidWait.error().contains(
        QStringLiteral("Invalid wait-after-command")));
}

void LaunchOptionsTest::parsesGhosttyDashECommand()
{
    const auto result = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--font-size=15.5"),
        QStringLiteral("-e"),
        QStringLiteral("/bin/sh"),
        QStringLiteral("--help"),
        QStringLiteral("--version"),
        QStringLiteral("--hold"),
        QStringLiteral("--working-directory=/ignored"),
        QStringLiteral("--single-instance=true"),
        QStringLiteral("--initial-window=false"),
        QStringLiteral("--title=opaque title"),
        QStringLiteral("--wait-after-command"),
        QStringLiteral("-e"),
        QStringLiteral("+help"),
        QStringLiteral("--"),
        QString{},
    });
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->typography.pointSize, 15.5);
    QCOMPARE(result->program,
             QStringList({QStringLiteral("/bin/sh"), QStringLiteral("--help"),
                          QStringLiteral("--version"), QStringLiteral("--hold"),
                          QStringLiteral("--working-directory=/ignored"),
                          QStringLiteral("--single-instance=true"),
                          QStringLiteral("--initial-window=false"),
                          QStringLiteral("--title=opaque title"),
                          QStringLiteral("--wait-after-command"),
                          QStringLiteral("-e"), QStringLiteral("+help"),
                          QStringLiteral("--"), QString{}}));
    QVERIFY(!result->showHelp);
    QVERIFY(!result->showVersion);
    QVERIFY(!result->hold);
    QVERIFY(!result->configuredTitle.has_value());
    QVERIFY(!result->configuredTitleExplicit);
    QVERIFY(!result->waitAfterCommand);
    QVERIFY(!result->waitAfterCommandExplicit);
    QCOMPARE(result->workingDirectory, QDir::currentPath());
    QVERIFY(!result->workingDirectoryExplicit);
    QCOMPARE(result->singleInstanceMode, SingleInstanceMode::Detect);
    QVERIFY(!result->singleInstanceModeExplicit);
    QVERIFY(result->initialWindow);
    QVERIFY(!result->initialWindowExplicit);
    QVERIFY(result->hasUnforwardedLaunchPayload());
    QVERIFY(!shouldUseSingleInstance(*result, QByteArrayView{}));
}

void LaunchOptionsTest::preservesDashEAfterDoubleDashAndInInlineValues()
{
    const auto inlineValue = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--font-family=-e"),
        QStringLiteral("-e"),
        QStringLiteral("/bin/true"),
    });
    QVERIFY2(inlineValue.has_value(), qPrintable(errorMessage(inlineValue)));
    QCOMPARE(regularFamilies(*inlineValue), QStringList{QStringLiteral("-e")});
    QCOMPARE(inlineValue->program, QStringList{QStringLiteral("/bin/true")});

    const auto afterDoubleDash = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--"),
        QStringLiteral("-e"),
        QStringLiteral("--wait-after-command"),
        QStringLiteral("literal-argument"),
    });
    QVERIFY2(afterDoubleDash.has_value(),
             qPrintable(errorMessage(afterDoubleDash)));
    QCOMPARE(afterDoubleDash->program,
             QStringList({QStringLiteral("-e"),
                          QStringLiteral("--wait-after-command"),
                          QStringLiteral("literal-argument")}));

    const auto reservedBoundary = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--font-family"),
        QStringLiteral("-e"),
        QStringLiteral("/bin/true"),
    });
    QVERIFY(!reservedBoundary.has_value());
    QVERIFY(reservedBoundary.error().contains(QStringLiteral("font-family")));

    const auto explicitSingleInstance = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--single-instance=true"),
        QStringLiteral("-e"),
        QStringLiteral("/bin/true"),
    });
    QVERIFY2(explicitSingleInstance.has_value(),
             qPrintable(errorMessage(explicitSingleInstance)));
    QCOMPARE(explicitSingleInstance->singleInstanceMode,
             SingleInstanceMode::Enabled);
    QVERIFY(explicitSingleInstance->singleInstanceModeExplicit);
    QVERIFY(
        !shouldUseSingleInstance(*explicitSingleInstance, QByteArrayView{}));
}

void LaunchOptionsTest::rejectsInvalidDashEUsage()
{
    const auto missing = parseLaunchOptions(
        {QStringLiteral("ghostty-qt"), QStringLiteral("-e")});
    QVERIFY(!missing.has_value());
    QCOMPARE(missing.error(), QStringLiteral("Missing command after -e."));

    const auto emptyExecutable = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("-e"),
        QString{},
        QStringLiteral("tail"),
    });
    QVERIFY2(emptyExecutable.has_value(),
             qPrintable(errorMessage(emptyExecutable)));
    QCOMPARE(emptyExecutable->program,
             QStringList({QString{}, QStringLiteral("tail")}));

    const auto positionalBeforeBoundary = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("unexpected"),
        QStringLiteral("-e"),
        QStringLiteral("/bin/true"),
    });
    QVERIFY(!positionalBeforeBoundary.has_value());
    QCOMPARE(
        positionalBeforeBoundary.error(),
        QStringLiteral("Unexpected positional argument before -e: unexpected"));

    for (const QString &nonBoundary :
         {QStringLiteral("-efoo"), QStringLiteral("-e=foo")}) {
        const auto result =
            parseLaunchOptions({QStringLiteral("ghostty-qt"), nonBoundary});
        QVERIFY(!result.has_value());
        QVERIFY(result.error() != QStringLiteral("Missing command after -e."));
    }
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
    const QString actualTarget = root.filePath(QStringLiteral("other/target"));
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
        {QStringLiteral("ghostty-qt"), QStringLiteral("--working-directory"),
         missingPath});
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

    const auto result =
        parseLaunchOptions({QStringLiteral("ghostty-qt"),
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
        {QStringLiteral("ghostty-qt"), QStringLiteral("--font-size"), value});
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
    QCOMPARE(ghosttyConfigurationArguments(*result),
             QStringList{QStringLiteral("--font-size=13.123457")});
}

void LaunchOptionsTest::buildsGhosttyConfigurationArguments()
{
    QCOMPARE(ghosttyConfigurationArguments({}), QStringList{});

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
    QCOMPARE(ghosttyConfigurationArguments(*family),
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
    QCOMPARE(
        regularFamilies(*repeatedFamilies),
        QStringList({QStringLiteral("Primary"), QStringLiteral("Fallback")}));
    QCOMPARE(ghosttyConfigurationArguments(*repeatedFamilies),
             QStringList({QStringLiteral("--font-family=Primary"),
                          QStringLiteral("--font-family=Fallback")}));

    const auto emptyFamily = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--font-family="),
    });
    QVERIFY2(emptyFamily.has_value(), qPrintable(errorMessage(emptyFamily)));
    QVERIFY(regularFamilies(*emptyFamily).isEmpty());
    QCOMPARE(ghosttyConfigurationArguments(*emptyFamily),
             QStringList{QStringLiteral("--font-family=")});

    const auto resetClass = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--class=com.example.before"),
        QStringLiteral("--class="),
    });
    QVERIFY2(resetClass.has_value(), qPrintable(errorMessage(resetClass)));
    QVERIFY(!resetClass->applicationClass.has_value());
    QVERIFY(resetClass->applicationClassExplicit);
    QVERIFY(resetClass->hasUnforwardedLaunchPayload());
    QCOMPARE(ghosttyConfigurationArguments(*resetClass),
             QStringList{QStringLiteral("--class=")});

    LaunchOptions defensive;
    defensive.fontFamilyExplicit = true;
    defensive.typography.face(TerminalFontRole::Regular).families = {
        QStringLiteral("First"),
        QStringLiteral("Second"),
    };
    defensive.fontSizeExplicit = true;
    defensive.typography.pointSize = 15.5;
    defensive.applicationClass =
        QByteArray::fromHex("636f6d2e6578616d706c652ee5aeb6");
    defensive.applicationClassExplicit = true;
    defensive.configDefaultFiles = false;
    defensive.configDefaultFilesExplicit = true;
    QCOMPARE(ghosttyConfigurationArguments(defensive),
             QStringList({
                 QStringLiteral("--font-family=First"),
                 QStringLiteral("--font-family=Second"),
                 QStringLiteral("--font-size=15.5"),
                 QString::fromUtf8("--class=com.example.\xE5\xAE\xB6"),
                 QStringLiteral("--config-default-files=false"),
             }));

    defensive.typography.face(TerminalFontRole::Regular).families.clear();
    defensive.fontSizeExplicit = false;
    QCOMPARE(ghosttyConfigurationArguments(defensive),
             QStringList({
                 QStringLiteral("--font-family="),
                 QString::fromUtf8("--class=com.example.\xE5\xAE\xB6"),
                 QStringLiteral("--config-default-files=false"),
             }));
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

    const auto result =
        parseLaunchOptions({QStringLiteral("ghostty-qt"),
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
    QCOMPARE(
        result.error(),
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
    base.processUsesSingleInstance = true;
    base.environment = {{
        .key = QByteArrayLiteral("STALE"),
        .value = QByteArrayLiteral("launch-value"),
    }};

    const GhosttyConfigSnapshot snapshot = completeSnapshot();

    const LaunchOptions cliResult = applyGhosttyConfigSnapshot(base, snapshot);
    // The process helper has already applied explicit CLI arguments,
    // recursive includes, and styled-family finalization. The broad snapshot
    // projection must trust that complete value instead of rebuilding a
    // hybrid from the original frontend flags.
    QCOMPARE(cliResult.term, QByteArrayLiteral("ghostty-qt-configured"));
    QCOMPARE(cliResult.enquiryResponse, snapshot.values.enquiryResponse);
    QVERIFY(cliResult.ordinaryCommand == snapshot.values.ordinaryCommand);
    QVERIFY(cliResult.initialCommand == snapshot.values.initialCommand);
    QCOMPARE(cliResult.abnormalCommandExitRuntimeMilliseconds, quint32{731});
    QVERIFY(cliResult.waitAfterCommand);
    QCOMPARE(cliResult.environment, snapshot.values.environment);
    QCOMPARE(cliResult.shellIntegration, snapshot.values.shellIntegration);
    QCOMPARE(cliResult.shellIntegrationFeatures,
             snapshot.values.shellIntegrationFeatures);
    QVERIFY(cliResult.shellIntegrationAvailable);
    QCOMPARE(cliResult.linuxCgroup, snapshot.values.linuxCgroup);
    QVERIFY(cliResult.processUsesSingleInstance);
    QVERIFY(cliResult.typography == completeTypography());
    QCOMPARE(cliResult.applicationClass, snapshot.values.applicationClass);
    QVERIFY(!cliResult.configDefaultFiles);
    QCOMPARE(cliResult.configuredTitle, snapshot.values.title);
    QCOMPARE(cliResult.windowAppearance, snapshot.values.windowAppearance);
    QCOMPARE(cliResult.appearance.foregroundColor,
             QColor(QStringLiteral("#112233")));
    QCOMPARE(cliResult.appearance.backgroundColor,
             QColor(QStringLiteral("#445566")));
    QCOMPARE(cliResult.background.opacity, 0.375);
    QVERIFY(cliResult.background.opacityCells);
    QVERIFY(cliResult.background.image.path.has_value());
    QCOMPARE(cliResult.background.image.path->path,
             QStringLiteral("/work/background.png"));
    QVERIFY(cliResult.background.image.path->optional);
    QCOMPARE(cliResult.background.image.opacity, 1.25);
    QCOMPARE(cliResult.background.image.position,
             TerminalBackgroundImagePosition::BottomRight);
    QCOMPARE(cliResult.background.image.fit, TerminalBackgroundImageFit::Cover);
    QVERIFY(cliResult.background.image.repeat);
    QCOMPARE(cliResult.customShaders, snapshot.values.customShaders);
    QCOMPARE(cliResult.backgroundBlur, qint16{42});
    QCOMPARE(cliResult.padding.horizontal.leadingPoints, quint32(3));
    QCOMPARE(cliResult.padding.horizontal.trailingPoints, quint32(5));
    QCOMPARE(cliResult.padding.vertical.leadingPoints, quint32(7));
    QCOMPARE(cliResult.padding.vertical.trailingPoints, quint32(11));
    QCOMPARE(cliResult.padding.balance, TerminalPaddingBalance::Equal);
    QCOMPARE(cliResult.padding.color, TerminalPaddingColor::ExtendAlways);
    QCOMPARE(cliResult.splitAppearance.unfocusedOpacity, 0.42);
    QCOMPARE(cliResult.splitAppearance.unfocusedFill,
             std::optional<QColor>(QColor(QStringLiteral("#778899"))));
    QCOMPARE(cliResult.splitAppearance.dividerColor,
             std::optional<QColor>(QColor(QStringLiteral("#a1b2c3"))));
    QCOMPARE(cliResult.appearance.palette.size(), 256);
    QCOMPARE(cliResult.appearance.palette.at(42), QColor::fromRgb(42, 213, 21));
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
    QCOMPARE(cliResult.appearance.boldColor.kind, TerminalBoldColorKind::Color);
    QCOMPARE(cliResult.appearance.boldColor.color,
             QColor(QStringLiteral("#abcdef")));
    QCOMPARE(cliResult.appearance.faintOpacity, 0.375);
    QCOMPARE(cliResult.appearance.minimumContrast, 4.25);
    QCOMPARE(cliResult.scrollbackLimit.value, quint64(25'000));
    QCOMPARE(cliResult.scrollbackLimit.unit, ScrollbackLimitUnit::Lines);
    QVERIFY(!cliResult.scrollbackCompression);
    QCOMPARE(cliResult.confirmCloseMode, ConfirmCloseMode::Always);
    QVERIFY(!cliResult.selectionClipboard.trimTrailingSpaces);
    QCOMPARE(cliResult.selectionClipboard.copyOnSelect,
             TerminalCopyOnSelectMode::PrimaryAndClipboard);
    QVERIFY(!cliResult.selectionClipboard.clearOnTyping);
    QVERIFY(cliResult.selectionClipboard.clearOnCopy);
    QCOMPARE(cliResult.selectionClipboard.codepointMap.size(), qsizetype{2});
    QCOMPARE(std::get<QString>(
                 cliResult.selectionClipboard.codepointMap.at(1).replacement),
             QStringLiteral("line"));
    QVERIFY(!cliResult.clipboardPaste.protection);
    QVERIFY(cliResult.clipboardPaste.bracketedSafe);
    QCOMPARE(cliResult.clipboardWrite, TerminalClipboardAccess::Ask);
    QVERIFY(!cliResult.scrollToBottom.keystroke);
    QVERIFY(cliResult.scrollToBottom.output);
    QCOMPARE(cliResult.rightClickAction, RightClickAction::CopyOrPaste);
    QCOMPARE(cliResult.middleClickAction, MiddleClickAction::Ignore);
    QVERIFY(!cliResult.mouseReporting);
    QCOMPARE(cliResult.mouseShiftCapture, MouseShiftCapture::Never);
    QCOMPARE(cliResult.selectionWordChars,
             QVector<quint32>({0, 0x20, 0x2502, 0x1f642}));
    QCOMPARE(cliResult.clickRepeatIntervalMilliseconds, quint32{731});
    QVERIFY(cliResult.vtKamAllowed);
    QVERIFY(!cliResult.linkUrl);
    QCOMPARE(cliResult.linkPreviews, LinkPreviewMode::Osc8);
    QVERIFY(cliResult.keybindSource.isAvailable());
    QVERIFY(cliResult.keybindSource.text() == nullptr);
    QVERIFY(cliResult.keybindSource.structured() != nullptr);
    QCOMPARE(*cliResult.keybindSource.structured(), snapshot.keybindings);

    base.scrollbackLimitExplicit = false;
    const LaunchOptions configResult =
        applyGhosttyConfigSnapshot(base, snapshot);
    QVERIFY(configResult.typography == completeTypography());
    QCOMPARE(configResult.scrollbackLimit.value, quint64(50'000'000));
    QCOMPARE(configResult.scrollbackLimit.unit, ScrollbackLimitUnit::Bytes);
    QCOMPARE(configResult.kittyImageStorageLimitBytes, quint32{123'456'789});

    for (const double unusableSize : {0.0, -2.0}) {
        GhosttyConfigSnapshot unusable = snapshot;
        unusable.values.typography.pointSize = unusableSize;
        const LaunchOptions result = applyGhosttyConfigSnapshot(base, unusable);
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

void LaunchOptionsTest::mapsBackdropAndPaddingSnapshots()
{
    LaunchOptions base;
    base.background = {
        .opacity = 0.25,
        .opacityCells = false,
        .image =
            {
                .path =
                    GhosttyConfigPath{
                        .path = QStringLiteral("/base/stale.png"),
                        .optional = false,
                    },
                .opacity = 0.125,
                .position = TerminalBackgroundImagePosition::TopLeft,
                .fit = TerminalBackgroundImageFit::None,
                .repeat = false,
            },
    };
    base.backgroundBlur = -1;
    base.padding = {
        .horizontal = {.leadingPoints = 1, .trailingPoints = 2},
        .vertical = {.leadingPoints = 3, .trailingPoints = 4},
        .balance = TerminalPaddingBalance::Disabled,
        .color = TerminalPaddingColor::Background,
    };
    GhosttyConfigSnapshot snapshot = completeSnapshot();

    const LaunchOptions configured = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(configured.background.opacity, 0.375);
    QVERIFY(configured.background.opacityCells);
    QVERIFY(configured.background == snapshot.values.background);
    QCOMPARE(configured.backgroundBlur, qint16{42});
    QVERIFY(configured.padding == snapshot.values.padding);

    snapshot.values.background = {
        .opacity = 0.75,
        .opacityCells = false,
        .image =
            {
                .path = std::nullopt,
                .opacity = 0.5,
                .position = TerminalBackgroundImagePosition::CenterLeft,
                .fit = TerminalBackgroundImageFit::Stretch,
                .repeat = false,
            },
    };
    snapshot.values.backgroundBlur = 0;
    snapshot.values.padding = {
        .horizontal = {.leadingPoints = 13, .trailingPoints = 17},
        .vertical = {.leadingPoints = 19, .trailingPoints = 23},
        .balance = TerminalPaddingBalance::Balanced,
        .color = TerminalPaddingColor::Extend,
    };
    const LaunchOptions reloaded =
        applyGhosttyConfigSnapshot(configured, snapshot);
    QCOMPARE(reloaded.background.opacity, 0.75);
    QVERIFY(!reloaded.background.opacityCells);
    QVERIFY(reloaded.background == snapshot.values.background);
    QCOMPARE(reloaded.backgroundBlur, qint16{0});
    // The authoritative launch snapshot replaces x/y as well as the live
    // balance/color pair. Existing panes retain their constructed x/y in the
    // pane layer, while future panes consume these newest dimensions.
    QVERIFY(reloaded.padding == snapshot.values.padding);
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

void LaunchOptionsTest::mapsScrollToBottom()
{
    LaunchOptions base;
    base.scrollToBottom = {
        .keystroke = true,
        .output = false,
    };
    GhosttyConfigSnapshot snapshot = completeSnapshot();

    const LaunchOptions configured = applyGhosttyConfigSnapshot(base, snapshot);
    QVERIFY(!configured.scrollToBottom.keystroke);
    QVERIFY(configured.scrollToBottom.output);

    snapshot.values.scrollToBottom = {};
    const LaunchOptions defaults =
        applyGhosttyConfigSnapshot(configured, snapshot);
    QVERIFY(defaults.scrollToBottom.keystroke);
    QVERIFY(!defaults.scrollToBottom.output);
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

void LaunchOptionsTest::mapsClickRepeatInterval()
{
    LaunchOptions base;
    base.clickRepeatIntervalMilliseconds = 1;
    GhosttyConfigSnapshot snapshot = completeSnapshot();

    const LaunchOptions configured = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(configured.clickRepeatIntervalMilliseconds, quint32{731});

    snapshot.values.clickRepeatIntervalMilliseconds =
        std::numeric_limits<quint32>::max();
    const LaunchOptions reloaded =
        applyGhosttyConfigSnapshot(configured, snapshot);
    QCOMPARE(reloaded.clickRepeatIntervalMilliseconds,
             std::numeric_limits<quint32>::max());
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

    for (const RightClickAction configured : {
             RightClickAction::ContextMenu,
             RightClickAction::Paste,
             RightClickAction::Copy,
             RightClickAction::CopyOrPaste,
             RightClickAction::Ignore,
         }) {
        GhosttyConfigSnapshot snapshot = completeSnapshot();
        snapshot.values.rightClickAction = configured;
        QCOMPARE(applyGhosttyConfigSnapshot({}, snapshot).rightClickAction,
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

    for (const MouseShiftCapture configured : {
             MouseShiftCapture::False,
             MouseShiftCapture::True,
             MouseShiftCapture::Always,
             MouseShiftCapture::Never,
         }) {
        GhosttyConfigSnapshot snapshot = completeSnapshot();
        snapshot.values.mouseShiftCapture = configured;
        QCOMPARE(applyGhosttyConfigSnapshot({}, snapshot).mouseShiftCapture,
                 configured);
    }

    LaunchOptions baseline;
    LaunchOptions changed = baseline;
    QVERIFY(changed == baseline);
    changed.rightClickAction = RightClickAction::Ignore;
    QVERIFY(changed != baseline);
    changed = baseline;
    changed.mouseShiftCapture = MouseShiftCapture::Always;
    QVERIFY(changed != baseline);
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
    QCOMPARE(result.workingDirectory, QStringLiteral("/base/link/../target"));
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
    QVERIFY(
        applyGhosttyConfigSnapshot(base, snapshot).splitPreserveZoomNavigation);

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

void LaunchOptionsTest::mapsTitleAndWindowAppearance()
{
    LaunchOptions base;
    base.configuredTitle = QStringLiteral("stale title");
    base.windowAppearance = {
        .theme = WindowTheme::Light,
        .titleFontFamily = QStringLiteral("Stale Font"),
        .titlebarBackground = QColor(QStringLiteral("#010203")),
        .titlebarForeground = QColor(QStringLiteral("#fefdfc")),
        .subtitle = WindowSubtitleMode::Disabled,
    };

    GhosttyConfigSnapshot snapshot = completeSnapshot();
    const LaunchOptions configured = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(configured.configuredTitle, snapshot.values.title);
    QCOMPARE(configured.windowAppearance, snapshot.values.windowAppearance);

    snapshot.values.title.reset();
    snapshot.values.windowAppearance = {};
    const LaunchOptions cleared =
        applyGhosttyConfigSnapshot(configured, snapshot);
    QVERIFY(!cleared.configuredTitle.has_value());
    QCOMPARE(cleared.windowAppearance, WindowAppearanceOptions{});
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
    QCOMPARE(result.resizeOverlay.position, ResizeOverlayPosition::BottomRight);
    QCOMPARE(result.resizeOverlay.duration, std::chrono::milliseconds(1'234));

    snapshot.values.resizeOverlay = {
        .mode = ResizeOverlayMode::AfterFirst,
        .position = ResizeOverlayPosition::TopCenter,
        .duration = std::chrono::milliseconds(250),
    };
    result = applyGhosttyConfigSnapshot(base, snapshot);
    QCOMPARE(result.resizeOverlay.mode, ResizeOverlayMode::AfterFirst);
    QCOMPARE(result.resizeOverlay.position, ResizeOverlayPosition::TopCenter);
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
    options.workingDirectoryExplicit = true;
    QVERIFY(!shouldUseSingleInstance(options, QByteArrayView{}));
    options.workingDirectoryExplicit = false;

    snapshot.values.singleInstanceMode = SingleInstanceMode::Disabled;
    options = applyFrontendConfigSnapshot(base, snapshot);
    QCOMPARE(options.singleInstanceMode, SingleInstanceMode::Disabled);
    QVERIFY(!shouldUseSingleInstance(options, QByteArrayView{}));

    snapshot.values.singleInstanceMode = SingleInstanceMode::Detect;
    options = applyFrontendConfigSnapshot(base, snapshot);
    QCOMPARE(options.singleInstanceMode, SingleInstanceMode::Detect);
    QVERIFY(shouldUseSingleInstance(options, QByteArrayView{}));
    QVERIFY(!shouldUseSingleInstance(options, QByteArrayView("ghostty")));
    options.workingDirectoryExplicit = true;
    QVERIFY(!shouldUseSingleInstance(options, QByteArrayView{}));
}

void LaunchOptionsTest::mapsFrontendConfigurationPrecedence()
{
    const auto parsed = parseLaunchOptions({QStringLiteral("ghostty-qt")});
    QVERIFY2(parsed.has_value(), qPrintable(errorMessage(parsed)));

    const LaunchOptions builtIns =
        resolveLaunchOptions(*parsed, nullptr, nullptr);
    QCOMPARE(builtIns.tabsLocation, TabsLocation::Top);
    QVERIFY(builtIns.wideTabs);
    QVERIFY(builtIns.horizontalTabScroll);
    QCOMPARE(builtIns.quickTerminalLayerShell.layer, QuickTerminalLayer::Top);
    QCOMPARE(builtIns.quickTerminalLayerShell.layerNamespace,
             QStringLiteral("ghostty-quick-terminal"));
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
    QVERIFY(sharedOnly.wideTabs);
    QVERIFY(sharedOnly.horizontalTabScroll);
    QCOMPARE(sharedOnly.quickTerminalLayerShell,
             QuickTerminalLayerShellOptions{});
    QCOMPARE(sharedOnly.singleInstanceMode, SingleInstanceMode::Detect);

    FrontendConfigSnapshot frontend;
    frontend.values.tabsLocation = TabsLocation::Bottom;
    frontend.values.wideTabs = false;
    frontend.values.horizontalTabScroll = false;
    frontend.values.quickTerminalLayerShell = {
        .layer = QuickTerminalLayer::Overlay,
        .layerNamespace = QStringLiteral("configured-quick-terminal"),
    };
    frontend.values.singleInstanceMode = SingleInstanceMode::Disabled;
    const LaunchOptions configured =
        resolveLaunchOptions(*parsed, &shared, &frontend);
    QCOMPARE(configured.windowDecoration, WindowDecorationMode::None);
    QCOMPARE(configured.tabsLocation, TabsLocation::Bottom);
    QVERIFY(!configured.wideTabs);
    QVERIFY(!configured.horizontalTabScroll);
    QCOMPARE(configured.quickTerminalLayerShell,
             frontend.values.quickTerminalLayerShell);
    QCOMPARE(configured.singleInstanceMode, SingleInstanceMode::Disabled);

    const auto explicitCli = parseLaunchOptions({
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--single-instance=true"),
    });
    QVERIFY2(explicitCli.has_value(), qPrintable(errorMessage(explicitCli)));
    const LaunchOptions overridden =
        resolveLaunchOptions(*explicitCli, &shared, &frontend);
    QCOMPARE(overridden.tabsLocation, TabsLocation::Bottom);
    QVERIFY(!overridden.wideTabs);
    QVERIFY(!overridden.horizontalTabScroll);
    QCOMPARE(overridden.quickTerminalLayerShell,
             frontend.values.quickTerminalLayerShell);
    QCOMPARE(overridden.singleInstanceMode, SingleInstanceMode::Enabled);
    QVERIFY(overridden.singleInstanceModeExplicit);

    frontend.values.tabsLocation = TabsLocation::Top;
    frontend.values.wideTabs = true;
    frontend.values.horizontalTabScroll = true;
    const LaunchOptions reapplied =
        applyFrontendConfigSnapshot(*parsed, frontend);
    QCOMPARE(reapplied.tabsLocation, TabsLocation::Top);
    QVERIFY(reapplied.wideTabs);
    QVERIFY(reapplied.horizontalTabScroll);
    QCOMPARE(reapplied.quickTerminalLayerShell,
             frontend.values.quickTerminalLayerShell);
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
    snapshot.values.appearance.selectionForeground = {};
    snapshot.values.appearance.selectionBackground = {};
    snapshot.values.appearance.cursorColor = {};
    snapshot.values.appearance.cursorBlink.reset();
    snapshot.values.appearance.cursorTextColor = {};
    snapshot.values.appearance.boldColor = {};
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
    options.term = QByteArrayLiteral("ghostty-qt-session");
    options.enquiryResponse = QByteArray::fromHex("000580ff");
    options.environment = {
        {
            .key = QByteArrayLiteral("SESSION_ASCII"),
            .value = QByteArrayLiteral("projected"),
        },
        {
            .key = QByteArrayLiteral("SESSION_RAW"),
            .value = QByteArray::fromHex("fe817f"),
        },
    };
    options.shellIntegration = GhosttyShellIntegrationMode::Zsh;
    options.shellIntegrationFeatures = {
        .cursor = false,
        .sudo = true,
        .title = false,
        .sshEnvironment = true,
        .sshTerminfo = false,
        .path = false,
    };
    options.shellIntegrationAvailable = true;
    options.linuxCgroup = {
        .mode = LinuxCgroupMode::SingleInstance,
        .memoryLimitBytes = quint64{8'589'934'592},
        .processesLimit = quint64{512},
        .hardFail = true,
    };
    options.processUsesSingleInstance = true;
    options.workingDirectory = QStringLiteral("/session/working-directory");
    options.workingDirectoryExplicit = true;
    options.ordinaryCommand = TerminalCommand::shell(
        QByteArray::fromHex("7072696e74662027ff27"), true);
    options.initialCommand = TerminalCommand::direct({
        QByteArrayLiteral("/bin/initial"),
        QByteArray{},
    });
    options.abnormalCommandExitRuntimeMilliseconds =
        std::numeric_limits<quint32>::max();
    options.waitAfterCommand = true;
    options.program = {QStringLiteral("/bin/program"), QStringLiteral("arg")};
    options.scrollbackLimit = {
        .value = 42'000,
        .unit = ScrollbackLimitUnit::Bytes,
    };
    options.scrollbackCompression = false;
    options.hold = true;
    options.appearance.foregroundColor = QColor(QStringLiteral("#123456"));
    options.appearance.palette = {QColor(QStringLiteral("#abcdef"))};
    options.colorScheme = TerminalColorScheme::Dark;
    options.configuredTitle = QStringLiteral("configured title");
    options.selectionClipboard = {
        .trimTrailingSpaces = false,
        .copyOnSelect = TerminalCopyOnSelectMode::PrimaryAndClipboard,
        .clearOnTyping = false,
        .clearOnCopy = true,
        .codepointMap =
            {
                {.first = 'x',
                 .last = 'z',
                 .replacement = QStringLiteral("mapped")},
            },
    };
    options.selectionWordChars = {0, 0x20, 0x2502, 0x1f642};
    options.clickRepeatIntervalMilliseconds =
        std::numeric_limits<quint32>::max();
    options.clipboardPaste = {
        .protection = false,
        .bracketedSafe = true,
    };
    options.clipboardWrite = TerminalClipboardAccess::Deny;
    options.scrollToBottom = {
        .keystroke = false,
        .output = true,
    };
    options.rightClickAction = RightClickAction::CopyOrPaste;
    options.middleClickAction = MiddleClickAction::Ignore;
    options.vtKamAllowed = true;
    options.appearance.minimumContrast = 7.25;
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
    QCOMPARE(runtime.appearance.minimumContrast, 7.25);
    QCOMPARE(runtime.colorScheme, TerminalColorScheme::Dark);
    QCOMPARE(runtime.enquiryResponse, options.enquiryResponse);
    QCOMPARE(runtime.selectionClipboard, options.selectionClipboard);
    QCOMPARE(runtime.selectionWordChars, options.selectionWordChars);
    QCOMPARE(runtime.clickRepeatIntervalMilliseconds,
             options.clickRepeatIntervalMilliseconds);
    QCOMPARE(runtime.clipboardPaste, options.clipboardPaste);
    QCOMPARE(runtime.clipboardWrite, options.clipboardWrite);
    QCOMPARE(runtime.scrollToBottom, options.scrollToBottom);
    QCOMPARE(runtime.rightClickAction, options.rightClickAction);
    QCOMPARE(runtime.vtKamAllowed, options.vtKamAllowed);
    QCOMPARE(runtime.linkUrl, options.linkUrl);
    QCOMPARE(runtime.scrollbackCompression, options.scrollbackCompression);
    QCOMPARE(runtime.abnormalCommandExitRuntimeMilliseconds,
             options.abnormalCommandExitRuntimeMilliseconds);
    QVERIFY(runtime.waitAfterCommand);
    QCOMPARE(launch.workingDirectory, options.workingDirectory);
    QCOMPARE(launch.term, options.term);
    QCOMPARE(launch.environment, options.environment);
    QCOMPARE(launch.shellIntegration, options.shellIntegration);
    QCOMPARE(launch.shellIntegrationFeatures, options.shellIntegrationFeatures);
    QVERIFY(launch.shellIntegrationAvailable);
    QCOMPARE(launch.linuxCgroup, options.linuxCgroup);
    QCOMPARE(launch.processUsesSingleInstance,
             options.processUsesSingleInstance);
    QCOMPARE(launch.inheritWorkingDirectory, options.inheritWorkingDirectory);
    QCOMPARE(launch.configuredTitle, options.configuredTitle);
    QVERIFY(launch.command == options.ordinaryCommand);
    QCOMPARE(launch.program, options.program);
    QCOMPARE(launch.scrollbackLimit, options.scrollbackLimit);
    QCOMPARE(launch.hold, options.hold);
    QVERIFY(!launch.initialGeometry.has_value());
    QCOMPARE(launch.runtime, runtime);

    QVERIFY(QMetaType::fromType<TerminalSessionLaunchOptions>().isValid());
    QVERIFY(QMetaType::fromType<TerminalSessionRuntimeOptions>().isValid());
    QVERIFY(
        qvariant_cast<TerminalSessionLaunchOptions>(QVariant::fromValue(launch))
        == launch);
    QVERIFY(qvariant_cast<TerminalSessionRuntimeOptions>(
                QVariant::fromValue(runtime))
            == runtime);

    LaunchOptions workerPolicyChanged = options;
    workerPolicyChanged.rightClickAction = RightClickAction::Ignore;
    QVERIFY(toTerminalSessionRuntimeOptions(workerPolicyChanged) != runtime);
    QVERIFY(toTerminalSessionLaunchOptions(workerPolicyChanged) != launch);

    LaunchOptions enquiryResponseChanged = options;
    enquiryResponseChanged.enquiryResponse = QByteArrayLiteral("changed");
    QVERIFY(toTerminalSessionRuntimeOptions(enquiryResponseChanged) != runtime);
    QVERIFY(toTerminalSessionLaunchOptions(enquiryResponseChanged) != launch);

    LaunchOptions configuredTitleChanged = options;
    configuredTitleChanged.configuredTitle = QStringLiteral("replacement");
    QCOMPARE(toTerminalSessionRuntimeOptions(configuredTitleChanged), runtime);
    QVERIFY(toTerminalSessionLaunchOptions(configuredTitleChanged) != launch);

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
    frontendOnlyChanged.windowNewTabPosition = WindowNewTabPosition::End;
    frontendOnlyChanged.windowShowTabBar = WindowShowTabBar::Never;
    frontendOnlyChanged.wideTabs = false;
    frontendOnlyChanged.horizontalTabScroll = false;
    frontendOnlyChanged.windowDecoration = WindowDecorationMode::None;
    frontendOnlyChanged.windowAppearance = {
        .theme = WindowTheme::Ghostty,
        .titleFontFamily = QStringLiteral("Frontend Window Font"),
        .titlebarBackground = QColor(QStringLiteral("#102030")),
        .titlebarForeground = QColor(QStringLiteral("#f0e0d0")),
        .subtitle = WindowSubtitleMode::WorkingDirectory,
    };
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
    frontendOnlyChanged.mouseShiftCapture = MouseShiftCapture::Always;
    frontendOnlyChanged.keybindSource =
        GhosttyKeybindSource::text({QStringLiteral("ctrl+x=ignore")});
    frontendOnlyChanged.showHelp = true;
    frontendOnlyChanged.showVersion = true;
    QCOMPARE(toTerminalSessionLaunchOptions(frontendOnlyChanged), launch);
    QCOMPARE(toTerminalSessionRuntimeOptions(frontendOnlyChanged), runtime);

    LaunchOptions terminalIdentityChanged = options;
    terminalIdentityChanged.term = QByteArrayLiteral("future-terminal");
    QVERIFY(toTerminalSessionLaunchOptions(terminalIdentityChanged) != launch);
    QCOMPARE(toTerminalSessionRuntimeOptions(terminalIdentityChanged), runtime);

    LaunchOptions commandChanged = options;
    commandChanged.ordinaryCommand = TerminalCommand::direct({
        QByteArrayLiteral("/bin/future-command"),
    });
    QVERIFY(toTerminalSessionLaunchOptions(commandChanged) != launch);
    QCOMPARE(toTerminalSessionRuntimeOptions(commandChanged), runtime);

    LaunchOptions initialCommandChanged = options;
    initialCommandChanged.initialCommand =
        TerminalCommand::shell(QByteArrayLiteral("initial-only"));
    QCOMPARE(toTerminalSessionLaunchOptions(initialCommandChanged), launch);
    QCOMPARE(toTerminalSessionRuntimeOptions(initialCommandChanged), runtime);

    LaunchOptions waitChanged = options;
    waitChanged.waitAfterCommand = false;
    QCOMPARE(toTerminalSessionLaunchOptions(waitChanged).hold, launch.hold);
    QVERIFY(toTerminalSessionRuntimeOptions(waitChanged) != runtime);

    LaunchOptions abnormalRuntimeChanged = options;
    abnormalRuntimeChanged.abnormalCommandExitRuntimeMilliseconds = 0;
    QCOMPARE(toTerminalSessionLaunchOptions(abnormalRuntimeChanged).hold,
             launch.hold);
    QVERIFY(toTerminalSessionRuntimeOptions(abnormalRuntimeChanged) != runtime);

    LaunchOptions scrollbackCompressionChanged = options;
    scrollbackCompressionChanged.scrollbackCompression = true;
    QVERIFY(toTerminalSessionRuntimeOptions(scrollbackCompressionChanged)
            != runtime);

    LaunchOptions scrollToBottomChanged = options;
    scrollToBottomChanged.scrollToBottom.keystroke = true;
    QVERIFY(toTerminalSessionRuntimeOptions(scrollToBottomChanged) != runtime);

    LaunchOptions vtKamChanged = options;
    vtKamChanged.vtKamAllowed = false;
    QVERIFY(toTerminalSessionRuntimeOptions(vtKamChanged) != runtime);

    LaunchOptions environmentChanged = options;
    environmentChanged.environment = {{
        .key = QByteArrayLiteral("SESSION_ASCII"),
        .value = QByteArrayLiteral("future-value"),
    }};
    QVERIFY(environmentChanged != options);
    QVERIFY(toTerminalSessionLaunchOptions(environmentChanged) != launch);
    QCOMPARE(toTerminalSessionRuntimeOptions(environmentChanged), runtime);

    LaunchOptions shellIntegrationChanged = options;
    shellIntegrationChanged.shellIntegration =
        GhosttyShellIntegrationMode::Bash;
    QVERIFY(toTerminalSessionLaunchOptions(shellIntegrationChanged) != launch);
    QCOMPARE(toTerminalSessionRuntimeOptions(shellIntegrationChanged), runtime);

    LaunchOptions shellIntegrationFeaturesChanged = options;
    shellIntegrationFeaturesChanged.shellIntegrationFeatures.cursor = true;
    QVERIFY(toTerminalSessionLaunchOptions(shellIntegrationFeaturesChanged)
            != launch);
    QCOMPARE(toTerminalSessionRuntimeOptions(shellIntegrationFeaturesChanged),
             runtime);

    LaunchOptions shellIntegrationAvailabilityChanged = options;
    shellIntegrationAvailabilityChanged.shellIntegrationAvailable = false;
    QVERIFY(toTerminalSessionLaunchOptions(shellIntegrationAvailabilityChanged)
            != launch);
    QCOMPARE(
        toTerminalSessionRuntimeOptions(shellIntegrationAvailabilityChanged),
        runtime);

    LaunchOptions inheritedDirectory = options;
    inheritedDirectory.inheritWorkingDirectory = true;
    QVERIFY(toTerminalSessionLaunchOptions(inheritedDirectory) != launch);
    QCOMPARE(toTerminalSessionRuntimeOptions(inheritedDirectory), runtime);

    options.workingDirectory.clear();
    options.enquiryResponse.clear();
    options.ordinaryCommand.reset();
    options.initialCommand.reset();
    options.abnormalCommandExitRuntimeMilliseconds = 0;
    options.waitAfterCommand = false;
    options.environment.clear();
    options.program.clear();
    options.scrollbackLimit = {};
    options.scrollbackCompression = true;
    options.hold = false;
    options.appearance = {};
    options.selectionClipboard = {};
    options.clipboardPaste = {};
    options.clipboardWrite = TerminalClipboardAccess::Allow;
    options.scrollToBottom = {};
    options.middleClickAction = MiddleClickAction::PrimaryPaste;
    options.vtKamAllowed = false;
    options.linkUrl = true;
    QCOMPARE(launch.workingDirectory,
             QStringLiteral("/session/working-directory"));
    QCOMPARE(
        launch.program,
        QStringList({QStringLiteral("/bin/program"), QStringLiteral("arg")}));
    QVERIFY(launch.command.has_value());
    QCOMPARE(launch.command->kind, TerminalCommandKind::Shell);
    QCOMPARE(launch.command->shellCommand,
             QByteArray::fromHex("7072696e74662027ff27"));
    QVERIFY(launch.command->defaultShell);
    QCOMPARE(launch.runtime.abnormalCommandExitRuntimeMilliseconds,
             std::numeric_limits<quint32>::max());
    QCOMPARE(launch.runtime.enquiryResponse, QByteArray::fromHex("000580ff"));
    QVERIFY(launch.runtime.waitAfterCommand);
    QCOMPARE(launch.environment,
             TerminalEnvironment({
                 {
                     .key = QByteArrayLiteral("SESSION_ASCII"),
                     .value = QByteArrayLiteral("projected"),
                 },
                 {
                     .key = QByteArrayLiteral("SESSION_RAW"),
                     .value = QByteArray::fromHex("fe817f"),
                 },
             }));
    QCOMPARE(launch.scrollbackLimit.value, quint64(42'000));
    QVERIFY(!launch.runtime.scrollbackCompression);
    QCOMPARE(launch.runtime.appearance.foregroundColor,
             QColor(QStringLiteral("#123456")));
    const TerminalSelectionClipboardOptions expectedClipboard{
        .trimTrailingSpaces = false,
        .copyOnSelect = TerminalCopyOnSelectMode::PrimaryAndClipboard,
        .clearOnTyping = false,
        .clearOnCopy = true,
        .codepointMap =
            {
                {.first = 'x',
                 .last = 'z',
                 .replacement = QStringLiteral("mapped")},
            },
    };
    QCOMPARE(launch.runtime.selectionClipboard, expectedClipboard);
    const TerminalClipboardPasteOptions expectedPaste{
        .protection = false,
        .bracketedSafe = true,
    };
    QCOMPARE(launch.runtime.clipboardPaste, expectedPaste);
    QCOMPARE(launch.runtime.clipboardWrite, TerminalClipboardAccess::Deny);
    const TerminalScrollToBottomOptions expectedScrollToBottom{
        .keystroke = false,
        .output = true,
    };
    QCOMPARE(launch.runtime.scrollToBottom, expectedScrollToBottom);
    QVERIFY(launch.runtime.vtKamAllowed);
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
    QCOMPARE(
        scrollbackLimitInBytes({.value = std::numeric_limits<quint64>::max(),
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
    options.ordinaryCommand =
        TerminalCommand::shell(QByteArrayLiteral("ordinary"), true);
    options.initialCommand = TerminalCommand::direct({
        QByteArrayLiteral("initial"),
    });
    options.waitAfterCommand = true;
    options.program = {
        QStringLiteral("command"),
        QStringLiteral("argument"),
    };
    options.hold = true;

    LaunchOptions expected = options;
    expected.initialCommand.reset();
    expected.program.clear();
    expected.hold = false;
    QVERIFY(withoutInitialCommand(options) == expected);
    QVERIFY(expected.ordinaryCommand == options.ordinaryCommand);
    QVERIFY(expected.waitAfterCommand);
}

void LaunchOptionsTest::materializesMissingFinalizedCommandFallback()
{
    GhosttyConfigSnapshot snapshot = completeSnapshot();
    snapshot.values.ordinaryCommand.reset();

    const LaunchOptions projected = applyGhosttyConfigSnapshot({}, snapshot);
    QVERIFY(projected.ordinaryCommand.has_value());
    QCOMPARE(projected.ordinaryCommand->kind, TerminalCommandKind::Shell);
    QCOMPARE(projected.ordinaryCommand->shellCommand, QByteArrayLiteral("sh"));
    QVERIFY(projected.ordinaryCommand->defaultShell);
    QVERIFY(projected.ordinaryCommand->directArguments.isEmpty());

    const TerminalSessionLaunchOptions session =
        toTerminalSessionLaunchOptions(projected);
    QVERIFY(session.command == projected.ordinaryCommand);
}

void LaunchOptionsTest::mapsCloseConfirmationModes()
{
    QVERIFY(!shouldConfirmClose(ConfirmCloseMode::Never, false, false));
    QVERIFY(!shouldConfirmClose(ConfirmCloseMode::Never, true, true));
    QVERIFY(
        !shouldConfirmClose(ConfirmCloseMode::RunningProcesses, false, false));
    QVERIFY(
        !shouldConfirmClose(ConfirmCloseMode::RunningProcesses, true, false));
    QVERIFY(shouldConfirmClose(ConfirmCloseMode::RunningProcesses, true, true));
    QVERIFY(!shouldConfirmClose(ConfirmCloseMode::Always, false, false));
    QVERIFY(shouldConfirmClose(ConfirmCloseMode::Always, true, false));
    QVERIFY(shouldConfirmClose(ConfirmCloseMode::Always, true, true));
}

void LaunchOptionsTest::keepsBackdropGuiOwned()
{
    LaunchOptions baseline;
    LaunchOptions changed = baseline;
    changed.background = {
        .opacity = 0.375,
        .opacityCells = true,
        .image =
            {
                .path =
                    GhosttyConfigPath{
                        .path = QStringLiteral("/gui/background.png"),
                        .optional = true,
                    },
                .opacity = 1.5,
                .position = TerminalBackgroundImagePosition::TopRight,
                .fit = TerminalBackgroundImageFit::Cover,
                .repeat = true,
            },
    };
    changed.backgroundBlur = 20;
    changed.padding = {
        .horizontal = {.leadingPoints = 3, .trailingPoints = 5},
        .vertical = {.leadingPoints = 7, .trailingPoints = 11},
        .balance = TerminalPaddingBalance::Equal,
        .color = TerminalPaddingColor::ExtendAlways,
    };

    QVERIFY(baseline != changed);
    QCOMPARE(toTerminalSessionRuntimeOptions(baseline),
             toTerminalSessionRuntimeOptions(changed));
    QCOMPARE(toTerminalSessionLaunchOptions(baseline),
             toTerminalSessionLaunchOptions(changed));
}

QTEST_APPLESS_MAIN(LaunchOptionsTest)

#include "test_launch_options.moc"
