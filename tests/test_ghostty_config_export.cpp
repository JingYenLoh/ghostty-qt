#include "ghostty_config_export.h"

#include "ghostty_config_export_fixture.h"

#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <limits>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace GhosttyConfigExportFixture;

template <typename Value>
QString errorMessage(const std::expected<Value, QString> &result)
{
    return result ? QString{} : result.error();
}

QJsonObject withValue(QJsonObject root, const QString &name,
                      const QJsonValue &value)
{
    QJsonObject configValues = root.value(QStringLiteral("values")).toObject();
    configValues.insert(name, value);
    root.insert(QStringLiteral("values"), configValues);
    return root;
}

QJsonObject withoutValue(QJsonObject root, const QString &name)
{
    QJsonObject configValues = root.value(QStringLiteral("values")).toObject();
    configValues.remove(name);
    root.insert(QStringLiteral("values"), configValues);
    return root;
}

} // namespace

class GhosttyConfigExportTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void parsesEveryValueWithExactSemantics();
    void normalizesBoundaryValues();
    void parsesEveryEnumSpelling();
    void parsesEveryTypographyAlternative();
    void parsesStructuredBindingSets();
    void rejectsMalformedEnvelope();
    void rejectsInvalidValues_data();
    void rejectsInvalidValues();
    void rejectsInvalidBindings();
};

void GhosttyConfigExportTest::parsesEveryValueWithExactSemantics()
{
    const auto parsed = parseGhosttyConfigExportJson(json());
    QVERIFY2(parsed.has_value(), qPrintable(errorMessage(parsed)));
    const GhosttyConfigValues &values = parsed->values;

    QCOMPARE(values.term, QByteArrayLiteral("ghostty-qt-test"));
    QVERIFY(values.ordinaryCommand.has_value());
    QCOMPARE(values.ordinaryCommand->kind, TerminalCommandKind::Shell);
    QCOMPARE(values.ordinaryCommand->shellCommand,
             QByteArrayLiteral("/bin/fixture-shell"));
    QVERIFY(values.ordinaryCommand->directArguments.isEmpty());
    QVERIFY(values.ordinaryCommand->defaultShell);
    QVERIFY(values.initialCommand.has_value());
    QCOMPARE(values.initialCommand->kind, TerminalCommandKind::Direct);
    QCOMPARE(values.initialCommand->directArguments.size(), qsizetype{3});
    QCOMPARE(values.initialCommand->directArguments.at(0),
             QByteArrayLiteral("/bin/printf"));
    QCOMPARE(values.initialCommand->directArguments.at(1),
             QByteArray::fromHex("80ff"));
    QVERIFY(values.initialCommand->directArguments.at(2).isEmpty());
    QVERIFY(values.initialCommand->shellCommand.isEmpty());
    QVERIFY(!values.initialCommand->defaultShell);
    QVERIFY(values.waitAfterCommand);
    QCOMPARE(values.abnormalCommandExitRuntimeMilliseconds, quint32{731});
    QCOMPARE(values.shellIntegration, GhosttyShellIntegrationMode::Fish);
    QVERIFY(!values.shellIntegrationFeatures.cursor);
    QVERIFY(values.shellIntegrationFeatures.sudo);
    QVERIFY(!values.shellIntegrationFeatures.title);
    QVERIFY(values.shellIntegrationFeatures.sshEnvironment);
    QVERIFY(values.shellIntegrationFeatures.sshTerminfo);
    QVERIFY(!values.shellIntegrationFeatures.path);
    QCOMPARE(values.environment.size(), qsizetype{3});
    QCOMPARE(values.environment.at(0).key,
             QByteArrayLiteral("GHOSTTY_QT_TEST"));
    QCOMPARE(values.environment.at(0).value, QByteArrayLiteral("alpha=beta"));
    QCOMPARE(values.environment.at(1).key, QByteArray::fromHex("80ff"));
    QCOMPARE(values.environment.at(1).value,
             QByteArray::fromHex("fe8176616c7565"));
    QVERIFY(values.environment.at(2).key.isEmpty());
    QCOMPARE(values.environment.at(2).value,
             QByteArrayLiteral("empty-key-value"));
    QCOMPARE(values.linuxCgroup.mode, LinuxCgroupMode::Always);
    QCOMPARE(values.linuxCgroup.memoryLimitBytes,
             std::optional<quint64>(std::numeric_limits<quint64>::max()));
    QCOMPARE(values.linuxCgroup.processesLimit,
             std::optional<quint64>(quint64{0}));
    QVERIFY(values.linuxCgroup.hardFail);
    QVERIFY(values.workingDirectoryPath.has_value());
    QCOMPARE(*values.workingDirectoryPath, QStringLiteral("/work/ghostty"));
    const TerminalTypography &typography = values.typography;
    QCOMPARE(
        typography.face(TerminalFontRole::Regular).families,
        QStringList({QStringLiteral("Mono One"), QStringLiteral("Emoji")}));
    QCOMPARE(typography.face(TerminalFontRole::Bold).families,
             QStringList({QStringLiteral("Mono Bold"),
                          QStringLiteral("Bold Fallback")}));
    QCOMPARE(typography.face(TerminalFontRole::Italic).families,
             QStringList({QStringLiteral("Mono Italic")}));
    QVERIFY(typography.face(TerminalFontRole::BoldItalic).families.isEmpty());
    QCOMPARE(typography.pointSize, 13.5);

    QVERIFY(std::holds_alternative<TerminalFontStyles::Automatic>(
        typography.face(TerminalFontRole::Regular).style));
    QVERIFY(std::holds_alternative<TerminalFontStyles::Disabled>(
        typography.face(TerminalFontRole::Bold).style));
    const auto *italicStyle = std::get_if<TerminalFontStyles::Named>(
        &typography.face(TerminalFontRole::Italic).style);
    QVERIFY(italicStyle != nullptr);
    QCOMPARE(italicStyle->name, QStringLiteral("Book Italic"));
    const auto *boldItalicStyle = std::get_if<TerminalFontStyles::Named>(
        &typography.face(TerminalFontRole::BoldItalic).style);
    QVERIFY(boldItalicStyle != nullptr);
    QCOMPARE(boldItalicStyle->name, QStringLiteral("Extra Bold Italic"));

    const auto absoluteModifier = [&typography](TerminalMetric metric) {
        const auto &value = typography.metricModifiers[metric];
        return value ? std::get_if<TerminalMetricModifiers::Absolute>(&*value)
                     : nullptr;
    };
    const auto percentageModifier = [&typography](TerminalMetric metric) {
        const auto &value = typography.metricModifiers[metric];
        return value ? std::get_if<TerminalMetricModifiers::Percentage>(&*value)
                     : nullptr;
    };
    QVERIFY(absoluteModifier(TerminalMetric::CellWidth) != nullptr);
    QCOMPARE(absoluteModifier(TerminalMetric::CellWidth)->pixels, qint32{2});
    QVERIFY(absoluteModifier(TerminalMetric::CellHeight) != nullptr);
    QCOMPARE(absoluteModifier(TerminalMetric::CellHeight)->pixels, qint32{-3});
    QVERIFY(percentageModifier(TerminalMetric::FontBaseline) != nullptr);
    QCOMPARE(percentageModifier(TerminalMetric::FontBaseline)->multiplier,
             1.25);
    QVERIFY(percentageModifier(TerminalMetric::UnderlinePosition) != nullptr);
    QCOMPARE(percentageModifier(TerminalMetric::UnderlinePosition)->multiplier,
             0.8);
    QVERIFY(!typography.metricModifiers[TerminalMetric::UnderlineThickness]
                 .has_value());
    QVERIFY(absoluteModifier(TerminalMetric::StrikethroughPosition) != nullptr);
    QCOMPARE(absoluteModifier(TerminalMetric::StrikethroughPosition)->pixels,
             qint32{4});
    QVERIFY(absoluteModifier(TerminalMetric::StrikethroughThickness)
            != nullptr);
    QCOMPARE(absoluteModifier(TerminalMetric::StrikethroughThickness)->pixels,
             qint32{-2});
    QVERIFY(percentageModifier(TerminalMetric::OverlinePosition) != nullptr);
    QCOMPARE(percentageModifier(TerminalMetric::OverlinePosition)->multiplier,
             1.5);
    QVERIFY(!typography.metricModifiers[TerminalMetric::OverlineThickness]
                 .has_value());
    QVERIFY(percentageModifier(TerminalMetric::CursorThickness) != nullptr);
    QCOMPARE(percentageModifier(TerminalMetric::CursorThickness)->multiplier,
             0.5);
    QVERIFY(absoluteModifier(TerminalMetric::CursorHeight) != nullptr);
    QCOMPARE(absoluteModifier(TerminalMetric::CursorHeight)->pixels, qint32{6});
    const std::vector expectedModifierOrder{
        TerminalMetric::CursorHeight,
        TerminalMetric::CellWidth,
        TerminalMetric::OverlinePosition,
        TerminalMetric::StrikethroughPosition,
        TerminalMetric::CellHeight,
        TerminalMetric::CursorThickness,
        TerminalMetric::FontBaseline,
        TerminalMetric::StrikethroughThickness,
        TerminalMetric::UnderlinePosition,
    };
    QVERIFY(typography.metricModifiers.applicationOrder
            == expectedModifierOrder);

    const GhosttyAppearanceConfig &appearance = values.appearance;
    QCOMPARE(appearance.foreground, QColor(QStringLiteral("#112233")));
    QCOMPARE(appearance.background, QColor(QStringLiteral("#445566")));
    QCOMPARE(appearance.palette.size(), std::size_t{256});
    for (std::size_t index = 0; index < appearance.palette.size(); ++index) {
        const int component = static_cast<int>(index);
        QCOMPARE(appearance.palette[index],
                 QColor(component, component, component));
    }

    QVERIFY(!appearance.selectionForeground.has_value());
    QVERIFY(appearance.selectionBackground.has_value());
    const auto *selectionBackground =
        std::get_if<GhosttyCellRelativeColor>(&*appearance.selectionBackground);
    QVERIFY(selectionBackground != nullptr);
    QCOMPARE(*selectionBackground, GhosttyCellRelativeColor::Foreground);

    const auto *searchForeground =
        std::get_if<QColor>(&appearance.searchForeground);
    QVERIFY(searchForeground != nullptr);
    QCOMPARE(*searchForeground, QColor(QStringLiteral("#010203")));
    const auto *searchBackground =
        std::get_if<GhosttyCellRelativeColor>(&appearance.searchBackground);
    QVERIFY(searchBackground != nullptr);
    QCOMPARE(*searchBackground, GhosttyCellRelativeColor::Background);
    const auto *searchSelectedForeground =
        std::get_if<GhosttyCellRelativeColor>(
            &appearance.searchSelectedForeground);
    QVERIFY(searchSelectedForeground != nullptr);
    QCOMPARE(*searchSelectedForeground, GhosttyCellRelativeColor::Foreground);
    const auto *searchSelectedBackground =
        std::get_if<QColor>(&appearance.searchSelectedBackground);
    QVERIFY(searchSelectedBackground != nullptr);
    QCOMPARE(*searchSelectedBackground, QColor(QStringLiteral("#aabbcc")));

    QVERIFY(appearance.cursorColor.has_value());
    const auto *cursorColor = std::get_if<QColor>(&*appearance.cursorColor);
    QVERIFY(cursorColor != nullptr);
    QCOMPARE(*cursorColor, QColor(QStringLiteral("#abcdef")));
    QCOMPARE(appearance.cursorOpacity, 0.625);
    QCOMPARE(appearance.cursorStyle, TerminalCursorStyle::BlockHollow);
    QVERIFY(!appearance.cursorBlink.has_value());
    QVERIFY(appearance.cursorText.has_value());
    const auto *cursorText =
        std::get_if<GhosttyCellRelativeColor>(&*appearance.cursorText);
    QVERIFY(cursorText != nullptr);
    QCOMPARE(*cursorText, GhosttyCellRelativeColor::Background);
    QVERIFY(appearance.boldColor.has_value());
    const auto *boldColor =
        std::get_if<GhosttyBoldBrightness>(&*appearance.boldColor);
    QVERIFY(boldColor != nullptr);
    QCOMPARE(*boldColor, GhosttyBoldBrightness::Bright);
    QCOMPARE(appearance.faintOpacity, 0.375);

    QCOMPARE(values.splitAppearance.unfocusedOpacity, 0.7);
    QVERIFY(!values.splitAppearance.unfocusedFill.has_value());
    QCOMPARE(values.splitAppearance.dividerColor,
             std::optional<QColor>(QColor(QStringLiteral("#778899"))));
    QVERIFY(!values.splitInheritWorkingDirectory);
    QVERIFY(values.splitPreserveZoom);
    QVERIFY(!values.tabInheritWorkingDirectory);
    QVERIFY(values.windowInheritWorkingDirectory);
    QVERIFY(!values.windowInheritFontSize);
    QCOMPARE(values.windowNewTabPosition, WindowNewTabPosition::End);
    QCOMPARE(values.windowShowTabBar, WindowShowTabBar::Always);
    QCOMPARE(values.windowDecoration, WindowDecorationMode::Server);
    QCOMPARE(values.windowWidth, quint32{120});
    QCOMPARE(values.windowHeight, quint32{40});
    QVERIFY(values.maximize);
    QCOMPARE(values.fullscreen, GhosttyFullscreenMode::NonNative);
    QCOMPARE(values.resizeOverlay.mode, ResizeOverlayMode::Always);
    QCOMPARE(values.resizeOverlay.position, ResizeOverlayPosition::BottomRight);
    QCOMPARE(values.resizeOverlay.duration, std::chrono::milliseconds{1250});

    QCOMPARE(values.scrollbackLimitBytes, std::numeric_limits<quint64>::max());
    QVERIFY(!values.scrollbackCompression);
    QCOMPARE(values.scrollbar, ScrollbarPolicy::Never);
    QVERIFY(values.bellFeatures.system);
    QVERIFY(values.bellFeatures.audio);
    QVERIFY(!values.bellFeatures.attention);
    QVERIFY(!values.bellFeatures.title);
    QVERIFY(values.bellFeatures.border);
    QVERIFY(values.bellAudioPath.has_value());
    QCOMPARE(values.bellAudioPath->path, QStringLiteral("/work/bell.oga"));
    QVERIFY(!values.bellAudioPath->optional);
    QCOMPARE(values.bellAudioVolume, 0.625);
    QCOMPARE(values.confirmCloseMode, ConfirmCloseMode::Always);
    QVERIFY(!values.selectionClipboard.trimTrailingSpaces);
    QCOMPARE(values.selectionClipboard.copyOnSelect,
             TerminalCopyOnSelectMode::PrimaryAndClipboard);
    QVERIFY(!values.selectionClipboard.clearOnTyping);
    QVERIFY(values.selectionClipboard.clearOnCopy);
    QCOMPARE(values.selectionWordChars,
             QVector<quint32>({0, 0x20, 0x2502, 0x1f642}));
    QCOMPARE(values.clickRepeatIntervalMilliseconds, quint32{731});
    QCOMPARE(values.clipboardWrite, TerminalClipboardAccess::Ask);
    QVERIFY(!values.clipboardPaste.protection);
    QVERIFY(values.clipboardPaste.bracketedSafe);
    QCOMPARE(values.rightClickAction, RightClickAction::CopyOrPaste);
    QCOMPARE(values.middleClickAction, MiddleClickAction::Ignore);
    QVERIFY(!values.mouseReporting);
    QCOMPARE(values.mouseShiftCapture, MouseShiftCapture::Never);
    QVERIFY(values.mouseHideWhileTyping);
    QVERIFY(!values.scrollToBottom.keystroke);
    QVERIFY(values.scrollToBottom.output);
    QVERIFY(values.focusFollowsMouse);
    QCOMPARE(values.mouseScrollMultiplier.precision, 0.75);
    QCOMPARE(values.mouseScrollMultiplier.discrete, 4.5);
    QVERIFY(!values.linkUrl);
    QCOMPARE(values.linkPreviews, LinkPreviewMode::Osc8);

    QCOMPARE(values.configFiles.size(), qsizetype{2});
    QCOMPARE(values.configFiles.at(0).path,
             QStringLiteral("/work/include.ghostty"));
    QVERIFY(!values.configFiles.at(0).optional);
    QCOMPARE(values.configFiles.at(1).path,
             QStringLiteral("/work/optional.ghostty"));
    QVERIFY(values.configFiles.at(1).optional);
    QVERIFY(!values.quitAfterLastWindowClosed);
    QVERIFY(!values.quitAfterLastWindowClosedDelay.has_value());
    QVERIFY(!values.initialWindow);
    QCOMPARE(values.singleInstanceMode, SingleInstanceMode::Detect);

    // Exercise both states of every nullable value. The primary fixture mixes
    // null and configured values; this inverse fixture ensures neither state
    // is represented by a textual or QVariant sentinel after decoding.
    QJsonObject inverseNullability = object();
    inverseNullability = withValue(std::move(inverseNullability),
                                   QStringLiteral("unfocused-split-fill"),
                                   QStringLiteral("#102030"));
    for (const QString &name : {
             QStringLiteral("split-divider-color"),
             QStringLiteral("selection-background"),
             QStringLiteral("cursor-color"),
             QStringLiteral("cursor-text"),
             QStringLiteral("bold-color"),
         }) {
        inverseNullability =
            withValue(std::move(inverseNullability), name, QJsonValue::Null);
    }
    inverseNullability = withValue(std::move(inverseNullability),
                                   QStringLiteral("selection-foreground"),
                                   QStringLiteral("cell-background"));
    inverseNullability = withValue(std::move(inverseNullability),
                                   QStringLiteral("cursor-style-blink"), false);
    inverseNullability =
        withValue(std::move(inverseNullability),
                  QStringLiteral("quit-after-last-window-closed-delay"), 0);
    inverseNullability =
        withValue(std::move(inverseNullability),
                  QStringLiteral("bell-audio-path"), QJsonValue::Null);
    inverseNullability = withValue(std::move(inverseNullability),
                                   QStringLiteral("command"), QJsonValue::Null);
    inverseNullability =
        withValue(std::move(inverseNullability),
                  QStringLiteral("initial-command"), QJsonValue::Null);
    inverseNullability = withValue(std::move(inverseNullability),
                                   QStringLiteral("wait-after-command"), false);

    const auto inverse = parseGhosttyConfigExportJson(json(inverseNullability));
    QVERIFY2(inverse.has_value(), qPrintable(errorMessage(inverse)));
    QCOMPARE(inverse->values.splitAppearance.unfocusedFill,
             std::optional<QColor>(QColor(QStringLiteral("#102030"))));
    QVERIFY(!inverse->values.splitAppearance.dividerColor.has_value());
    QVERIFY(inverse->values.appearance.selectionForeground.has_value());
    const auto *inverseSelectionForeground =
        std::get_if<GhosttyCellRelativeColor>(
            &*inverse->values.appearance.selectionForeground);
    QVERIFY(inverseSelectionForeground != nullptr);
    QCOMPARE(*inverseSelectionForeground, GhosttyCellRelativeColor::Background);
    QVERIFY(!inverse->values.appearance.selectionBackground.has_value());
    QVERIFY(!inverse->values.appearance.cursorColor.has_value());
    QCOMPARE(inverse->values.appearance.cursorBlink,
             std::optional<bool>(false));
    QVERIFY(!inverse->values.appearance.cursorText.has_value());
    QVERIFY(!inverse->values.appearance.boldColor.has_value());
    QCOMPARE(inverse->values.quitAfterLastWindowClosedDelay,
             std::optional(std::chrono::milliseconds::zero()));
    QVERIFY(!inverse->values.bellAudioPath.has_value());
    QVERIFY(!inverse->values.ordinaryCommand.has_value());
    QVERIFY(!inverse->values.initialCommand.has_value());
    QVERIFY(!inverse->values.waitAfterCommand);
}

void GhosttyConfigExportTest::normalizesBoundaryValues()
{
    QJsonObject lowValues = object();
    lowValues =
        withValue(std::move(lowValues), QStringLiteral("working-directory"),
                  QStringLiteral("inherit"));
    lowValues =
        withValue(std::move(lowValues), QStringLiteral("cursor-opacity"), -2.0);
    lowValues = withValue(std::move(lowValues),
                          QStringLiteral("resize-overlay-duration"), 0);
    lowValues = withValue(std::move(lowValues),
                          QStringLiteral("bell-audio-volume"), -2.0);
    lowValues = withValue(std::move(lowValues),
                          QStringLiteral("mouse-scroll-multiplier"),
                          mouseScrollMultiplier(0.01, 0.01));
    lowValues = withValue(std::move(lowValues),
                          QStringLiteral("linux-cgroup-memory-limit"),
                          QJsonValue::Null);
    lowValues = withValue(std::move(lowValues),
                          QStringLiteral("linux-cgroup-processes-limit"),
                          QJsonValue::Null);
    lowValues =
        withValue(std::move(lowValues), QStringLiteral("env"), QJsonArray{});

    const auto low = parseGhosttyConfigExportJson(json(lowValues));
    QVERIFY2(low.has_value(), qPrintable(errorMessage(low)));
    QVERIFY(!low->values.workingDirectoryPath.has_value());
    QCOMPARE(low->values.appearance.cursorOpacity, 0.0);
    QCOMPARE(low->values.resizeOverlay.duration,
             std::chrono::milliseconds{250});
    QCOMPARE(low->values.bellAudioVolume, -2.0);
    QCOMPARE(low->values.mouseScrollMultiplier.precision, 0.01);
    QCOMPARE(low->values.mouseScrollMultiplier.discrete, 0.01);
    QVERIFY(!low->values.linuxCgroup.memoryLimitBytes.has_value());
    QVERIFY(!low->values.linuxCgroup.processesLimit.has_value());
    QVERIFY(low->values.environment.isEmpty());

    QJsonObject emptyCommands = object();
    emptyCommands =
        withValue(std::move(emptyCommands), QStringLiteral("command"),
                  shellCommand(QByteArrayView{}));
    emptyCommands =
        withValue(std::move(emptyCommands), QStringLiteral("initial-command"),
                  directCommand({bytes(QByteArrayView{})}));
    const auto emptyCommandValues =
        parseGhosttyConfigExportJson(json(emptyCommands));
    QVERIFY2(emptyCommandValues.has_value(),
             qPrintable(errorMessage(emptyCommandValues)));
    QVERIFY(emptyCommandValues->values.ordinaryCommand.has_value());
    QVERIFY(emptyCommandValues->values.ordinaryCommand->shellCommand.isEmpty());
    QVERIFY(emptyCommandValues->values.initialCommand.has_value());
    QCOMPARE(emptyCommandValues->values.initialCommand->directArguments.size(),
             qsizetype{1});
    QVERIFY(
        emptyCommandValues->values.initialCommand->directArguments.constFirst()
            .isEmpty());

    QJsonObject emptyKeyValues = object();
    emptyKeyValues =
        withValue(std::move(emptyKeyValues), QStringLiteral("env"),
                  QJsonArray{environmentEntry(QByteArrayView{},
                                              QByteArrayLiteral("accepted"))});
    const auto emptyKey = parseGhosttyConfigExportJson(json(emptyKeyValues));
    QVERIFY2(emptyKey.has_value(), qPrintable(errorMessage(emptyKey)));
    QCOMPARE(emptyKey->values.environment.size(), qsizetype{1});
    QVERIFY(emptyKey->values.environment.constFirst().key.isEmpty());
    QCOMPARE(emptyKey->values.environment.constFirst().value,
             QByteArrayLiteral("accepted"));

    QJsonObject onlyRequiredNull = object();
    onlyRequiredNull =
        withValue(std::move(onlyRequiredNull),
                  QStringLiteral("selection-word-chars"), QJsonArray{0});
    const auto minimalWordChars =
        parseGhosttyConfigExportJson(json(onlyRequiredNull));
    QVERIFY2(minimalWordChars.has_value(),
             qPrintable(errorMessage(minimalWordChars)));
    QCOMPARE(minimalWordChars->values.selectionWordChars,
             QVector<quint32>({0}));

    QJsonObject orderedWordChars = object();
    orderedWordChars = withValue(std::move(orderedWordChars),
                                 QStringLiteral("selection-word-chars"),
                                 QJsonArray{0, 65, 65, 0, 0x10ffff});
    const auto preservedWordChars =
        parseGhosttyConfigExportJson(json(orderedWordChars));
    QVERIFY2(preservedWordChars.has_value(),
             qPrintable(errorMessage(preservedWordChars)));
    QCOMPARE(preservedWordChars->values.selectionWordChars,
             QVector<quint32>({0, 65, 65, 0, 0x10ffff}));

    QJsonObject highValues = object();
    highValues =
        withValue(std::move(highValues), QStringLiteral("cursor-opacity"), 2.0);
    highValues = withValue(std::move(highValues),
                           QStringLiteral("bell-audio-volume"), 2.0);
    highValues = withValue(std::move(highValues),
                           QStringLiteral("mouse-scroll-multiplier"),
                           mouseScrollMultiplier(10'000.0, 10'000.0));
    highValues =
        withValue(std::move(highValues),
                  QStringLiteral("click-repeat-interval"), 4294967295.0);
    highValues = withValue(std::move(highValues),
                           QStringLiteral("abnormal-command-exit-runtime"),
                           4294967295.0);
    const auto high = parseGhosttyConfigExportJson(json(highValues));
    QVERIFY2(high.has_value(), qPrintable(errorMessage(high)));
    QCOMPARE(high->values.appearance.cursorOpacity, 1.0);
    QCOMPARE(high->values.bellAudioVolume, 2.0);
    QCOMPARE(high->values.mouseScrollMultiplier.precision, 10'000.0);
    QCOMPARE(high->values.mouseScrollMultiplier.discrete, 10'000.0);
    QCOMPARE(high->values.clickRepeatIntervalMilliseconds,
             std::numeric_limits<quint32>::max());
    QCOMPARE(high->values.abnormalCommandExitRuntimeMilliseconds,
             std::numeric_limits<quint32>::max());

    const auto zeroRuntime = parseGhosttyConfigExportJson(json(withValue(
        object(), QStringLiteral("abnormal-command-exit-runtime"), 0)));
    QVERIFY2(zeroRuntime.has_value(), qPrintable(errorMessage(zeroRuntime)));
    QCOMPARE(zeroRuntime->values.abnormalCommandExitRuntimeMilliseconds,
             quint32{0});
}

void GhosttyConfigExportTest::parsesEveryEnumSpelling()
{
    const auto verifyMappings = [](QLatin1StringView field,
                                   const auto &mappings, auto projection) {
        for (const auto &[spelling, expected] : mappings) {
            const auto parsed = parseGhosttyConfigExportJson(json(
                withValue(object(), field.toString(), spelling.toString())));
            QVERIFY2(parsed.has_value(), qPrintable(errorMessage(parsed)));
            QCOMPARE(projection(parsed->values), expected);
        }
    };

    verifyMappings(
        QLatin1StringView("shell-integration"),
        std::to_array<
            std::pair<QLatin1StringView, GhosttyShellIntegrationMode>>({
            {QLatin1StringView("none"), GhosttyShellIntegrationMode::None},
            {QLatin1StringView("detect"), GhosttyShellIntegrationMode::Detect},
            {QLatin1StringView("bash"), GhosttyShellIntegrationMode::Bash},
            {QLatin1StringView("elvish"), GhosttyShellIntegrationMode::Elvish},
            {QLatin1StringView("fish"), GhosttyShellIntegrationMode::Fish},
            {QLatin1StringView("nushell"),
             GhosttyShellIntegrationMode::Nushell},
            {QLatin1StringView("zsh"), GhosttyShellIntegrationMode::Zsh},
        }),
        [](const GhosttyConfigValues &values) {
            return values.shellIntegration;
        });
    verifyMappings(
        QLatin1StringView("window-new-tab-position"),
        std::to_array<std::pair<QLatin1StringView, WindowNewTabPosition>>({
            {QLatin1StringView("current"), WindowNewTabPosition::Current},
            {QLatin1StringView("end"), WindowNewTabPosition::End},
        }),
        [](const GhosttyConfigValues &values) {
            return values.windowNewTabPosition;
        });
    verifyMappings(
        QLatin1StringView("window-show-tab-bar"),
        std::to_array<std::pair<QLatin1StringView, WindowShowTabBar>>({
            {QLatin1StringView("always"), WindowShowTabBar::Always},
            {QLatin1StringView("auto"), WindowShowTabBar::Auto},
            {QLatin1StringView("never"), WindowShowTabBar::Never},
        }),
        [](const GhosttyConfigValues &values) {
            return values.windowShowTabBar;
        });
    verifyMappings(
        QLatin1StringView("window-decoration"),
        std::to_array<std::pair<QLatin1StringView, WindowDecorationMode>>({
            {QLatin1StringView("auto"), WindowDecorationMode::Auto},
            {QLatin1StringView("client"), WindowDecorationMode::Client},
            {QLatin1StringView("server"), WindowDecorationMode::Server},
            {QLatin1StringView("none"), WindowDecorationMode::None},
        }),
        [](const GhosttyConfigValues &values) {
            return values.windowDecoration;
        });
    verifyMappings(
        QLatin1StringView("fullscreen"),
        std::to_array<std::pair<QLatin1StringView, GhosttyFullscreenMode>>({
            {QLatin1StringView("false"), GhosttyFullscreenMode::Disabled},
            {QLatin1StringView("true"), GhosttyFullscreenMode::Enabled},
            {QLatin1StringView("non-native"), GhosttyFullscreenMode::NonNative},
            {QLatin1StringView("non-native-visible-menu"),
             GhosttyFullscreenMode::NonNativeVisibleMenu},
            {QLatin1StringView("non-native-padded-notch"),
             GhosttyFullscreenMode::NonNativePaddedNotch},
        }),
        [](const GhosttyConfigValues &values) { return values.fullscreen; });
    verifyMappings(
        QLatin1StringView("cursor-style"),
        std::to_array<std::pair<QLatin1StringView, TerminalCursorStyle>>({
            {QLatin1StringView("block"), TerminalCursorStyle::Block},
            {QLatin1StringView("bar"), TerminalCursorStyle::Bar},
            {QLatin1StringView("underline"), TerminalCursorStyle::Underline},
            {QLatin1StringView("block_hollow"),
             TerminalCursorStyle::BlockHollow},
        }),
        [](const GhosttyConfigValues &values) {
            return values.appearance.cursorStyle;
        });
    verifyMappings(
        QLatin1StringView("confirm-close-surface"),
        std::to_array<std::pair<QLatin1StringView, ConfirmCloseMode>>({
            {QLatin1StringView("false"), ConfirmCloseMode::Never},
            {QLatin1StringView("true"), ConfirmCloseMode::RunningProcesses},
            {QLatin1StringView("always"), ConfirmCloseMode::Always},
        }),
        [](const GhosttyConfigValues &values) {
            return values.confirmCloseMode;
        });
    verifyMappings(
        QLatin1StringView("copy-on-select"),
        std::to_array<std::pair<QLatin1StringView, TerminalCopyOnSelectMode>>({
            {QLatin1StringView("false"), TerminalCopyOnSelectMode::Disabled},
            {QLatin1StringView("true"), TerminalCopyOnSelectMode::Primary},
            {QLatin1StringView("clipboard"),
             TerminalCopyOnSelectMode::PrimaryAndClipboard},
        }),
        [](const GhosttyConfigValues &values) {
            return values.selectionClipboard.copyOnSelect;
        });
    verifyMappings(
        QLatin1StringView("clipboard-write"),
        std::to_array<std::pair<QLatin1StringView, TerminalClipboardAccess>>({
            {QLatin1StringView("ask"), TerminalClipboardAccess::Ask},
            {QLatin1StringView("allow"), TerminalClipboardAccess::Allow},
            {QLatin1StringView("deny"), TerminalClipboardAccess::Deny},
        }),
        [](const GhosttyConfigValues &values) {
            return values.clipboardWrite;
        });
    verifyMappings(
        QLatin1StringView("right-click-action"),
        std::to_array<std::pair<QLatin1StringView, RightClickAction>>({
            {QLatin1StringView("context-menu"), RightClickAction::ContextMenu},
            {QLatin1StringView("paste"), RightClickAction::Paste},
            {QLatin1StringView("copy"), RightClickAction::Copy},
            {QLatin1StringView("copy-or-paste"), RightClickAction::CopyOrPaste},
            {QLatin1StringView("ignore"), RightClickAction::Ignore},
        }),
        [](const GhosttyConfigValues &values) {
            return values.rightClickAction;
        });
    verifyMappings(
        QLatin1StringView("middle-click-action"),
        std::to_array<std::pair<QLatin1StringView, MiddleClickAction>>({
            {QLatin1StringView("primary-paste"),
             MiddleClickAction::PrimaryPaste},
            {QLatin1StringView("ignore"), MiddleClickAction::Ignore},
        }),
        [](const GhosttyConfigValues &values) {
            return values.middleClickAction;
        });
    verifyMappings(
        QLatin1StringView("mouse-shift-capture"),
        std::to_array<std::pair<QLatin1StringView, MouseShiftCapture>>({
            {QLatin1StringView("false"), MouseShiftCapture::False},
            {QLatin1StringView("true"), MouseShiftCapture::True},
            {QLatin1StringView("always"), MouseShiftCapture::Always},
            {QLatin1StringView("never"), MouseShiftCapture::Never},
        }),
        [](const GhosttyConfigValues &values) {
            return values.mouseShiftCapture;
        });
    verifyMappings(
        QLatin1StringView("link-previews"),
        std::to_array<std::pair<QLatin1StringView, LinkPreviewMode>>({
            {QLatin1StringView("false"), LinkPreviewMode::Never},
            {QLatin1StringView("true"), LinkPreviewMode::Always},
            {QLatin1StringView("osc8"), LinkPreviewMode::Osc8},
        }),
        [](const GhosttyConfigValues &values) { return values.linkPreviews; });
    verifyMappings(
        QLatin1StringView("scrollbar"),
        std::to_array<std::pair<QLatin1StringView, ScrollbarPolicy>>({
            {QLatin1StringView("system"), ScrollbarPolicy::System},
            {QLatin1StringView("never"), ScrollbarPolicy::Never},
        }),
        [](const GhosttyConfigValues &values) { return values.scrollbar; });
    verifyMappings(
        QLatin1StringView("resize-overlay"),
        std::to_array<std::pair<QLatin1StringView, ResizeOverlayMode>>({
            {QLatin1StringView("always"), ResizeOverlayMode::Always},
            {QLatin1StringView("never"), ResizeOverlayMode::Never},
            {QLatin1StringView("after-first"), ResizeOverlayMode::AfterFirst},
        }),
        [](const GhosttyConfigValues &values) {
            return values.resizeOverlay.mode;
        });
    verifyMappings(
        QLatin1StringView("resize-overlay-position"),
        std::to_array<std::pair<QLatin1StringView, ResizeOverlayPosition>>({
            {QLatin1StringView("center"), ResizeOverlayPosition::Center},
            {QLatin1StringView("top-left"), ResizeOverlayPosition::TopLeft},
            {QLatin1StringView("top-center"), ResizeOverlayPosition::TopCenter},
            {QLatin1StringView("top-right"), ResizeOverlayPosition::TopRight},
            {QLatin1StringView("bottom-left"),
             ResizeOverlayPosition::BottomLeft},
            {QLatin1StringView("bottom-center"),
             ResizeOverlayPosition::BottomCenter},
            {QLatin1StringView("bottom-right"),
             ResizeOverlayPosition::BottomRight},
        }),
        [](const GhosttyConfigValues &values) {
            return values.resizeOverlay.position;
        });
    verifyMappings(
        QLatin1StringView("gtk-single-instance"),
        std::to_array<std::pair<QLatin1StringView, SingleInstanceMode>>({
            {QLatin1StringView("false"), SingleInstanceMode::Disabled},
            {QLatin1StringView("true"), SingleInstanceMode::Enabled},
            {QLatin1StringView("detect"), SingleInstanceMode::Detect},
        }),
        [](const GhosttyConfigValues &values) {
            return values.singleInstanceMode;
        });
    verifyMappings(
        QLatin1StringView("linux-cgroup"),
        std::to_array<std::pair<QLatin1StringView, LinuxCgroupMode>>({
            {QLatin1StringView("never"), LinuxCgroupMode::Never},
            {QLatin1StringView("always"), LinuxCgroupMode::Always},
            {QLatin1StringView("single-instance"),
             LinuxCgroupMode::SingleInstance},
        }),
        [](const GhosttyConfigValues &values) {
            return values.linuxCgroup.mode;
        });
}

void GhosttyConfigExportTest::parsesEveryTypographyAlternative()
{
    const auto faces =
        std::to_array<std::pair<QLatin1StringView, TerminalFontRole>>({
            {QLatin1StringView("font-style"), TerminalFontRole::Regular},
            {QLatin1StringView("font-style-bold"), TerminalFontRole::Bold},
            {QLatin1StringView("font-style-italic"), TerminalFontRole::Italic},
            {QLatin1StringView("font-style-bold-italic"),
             TerminalFontRole::BoldItalic},
        });

    for (const auto &[field, role] : faces) {
        auto parsed = parseGhosttyConfigExportJson(
            json(withValue(object(), field.toString(), automaticFontStyle())));
        QVERIFY2(parsed.has_value(), qPrintable(errorMessage(parsed)));
        QVERIFY(std::holds_alternative<TerminalFontStyles::Automatic>(
            parsed->values.typography.face(role).style));

        parsed = parseGhosttyConfigExportJson(
            json(withValue(object(), field.toString(), disabledFontStyle())));
        QVERIFY2(parsed.has_value(), qPrintable(errorMessage(parsed)));
        QVERIFY(std::holds_alternative<TerminalFontStyles::Disabled>(
            parsed->values.typography.face(role).style));

        parsed = parseGhosttyConfigExportJson(
            json(withValue(object(), field.toString(),
                           namedFontStyle(QStringLiteral("Named Style")))));
        QVERIFY2(parsed.has_value(), qPrintable(errorMessage(parsed)));
        const auto *named = std::get_if<TerminalFontStyles::Named>(
            &parsed->values.typography.face(role).style);
        QVERIFY(named != nullptr);
        QCOMPARE(named->name, QStringLiteral("Named Style"));
    }

    const auto parseCellWidth = [](const QJsonValue &value) {
        QJsonObject root =
            withValue(object(), QStringLiteral("adjust-cell-width"), value);
        if (value.isNull()) {
            QJsonArray order = metricModifierOrder();
            for (qsizetype index = 0; index < order.size(); ++index) {
                if (order.at(index).toString()
                    == QStringLiteral("adjust-cell-width")) {
                    order.removeAt(index);
                    break;
                }
            }
            root = withValue(std::move(root),
                             QStringLiteral("metric-modifier-order"), order);
        }
        return parseGhosttyConfigExportJson(json(root));
    };
    auto parsed = parseCellWidth(QJsonValue::Null);
    QVERIFY2(parsed.has_value(), qPrintable(errorMessage(parsed)));
    QVERIFY(
        !parsed->values.typography.metricModifiers[TerminalMetric::CellWidth]
             .has_value());

    for (const int pixels : {-17, 23}) {
        parsed = parseCellWidth(absoluteMetricModifier(pixels));
        QVERIFY2(parsed.has_value(), qPrintable(errorMessage(parsed)));
        const auto &modifier = parsed->values.typography
                                   .metricModifiers[TerminalMetric::CellWidth];
        QVERIFY(modifier.has_value());
        const auto *absolute =
            std::get_if<TerminalMetricModifiers::Absolute>(&*modifier);
        QVERIFY(absolute != nullptr);
        QCOMPARE(absolute->pixels, qint32{pixels});
    }

    for (const double multiplier : {0.75, 1.25}) {
        parsed = parseCellWidth(percentageMetricModifier(multiplier));
        QVERIFY2(parsed.has_value(), qPrintable(errorMessage(parsed)));
        const auto &modifier = parsed->values.typography
                                   .metricModifiers[TerminalMetric::CellWidth];
        QVERIFY(modifier.has_value());
        const auto *percentage =
            std::get_if<TerminalMetricModifiers::Percentage>(&*modifier);
        QVERIFY(percentage != nullptr);
        QCOMPARE(percentage->multiplier, multiplier);
    }
}

void GhosttyConfigExportTest::parsesStructuredBindingSets()
{
    QJsonObject exportObject = object();
    const QJsonObject current{
        {QStringLiteral("root"),
         QJsonArray{
             binding({physicalTrigger(QStringLiteral("key_a"), 3),
                      unicodeTrigger(128578, 4), catchAllTrigger()},
                     {QStringLiteral("new_tab"), QStringLiteral("goto_tab:2")},
                     flags(false, false, false, true)),
             binding({unicodeTrigger('x')}, {QStringLiteral("ignore")},
                     flags(true, true)),
         }},
        {QStringLiteral("tables"),
         QJsonArray{QJsonObject{
             {QStringLiteral("name"), QStringLiteral("modeé")},
             {QStringLiteral("bindings"),
              QJsonArray{binding({unicodeTrigger('h', GhosttyKeybindCtrl)},
                                 {QStringLiteral("resize_split:left,10")})}},
         }}},
    };
    exportObject.insert(QStringLiteral("keybindings"), current);

    const auto parsed = parseGhosttyConfigExportJson(json(exportObject));
    QVERIFY2(parsed.has_value(), qPrintable(errorMessage(parsed)));
    QCOMPARE(parsed->keybindings.root.size(), 2);
    const GhosttyKeybindDefinition &root =
        parsed->keybindings.root.constFirst();
    QCOMPARE(root.sequence.size(), 3);
    QCOMPARE(root.sequence.at(0).kind, GhosttyKeybindKeyKind::Physical);
    QCOMPARE(root.sequence.at(0).physicalName, QStringLiteral("key_a"));
    QCOMPARE(root.sequence.at(1).unicodeCodepoint, quint32(128578));
    QCOMPARE(root.sequence.at(2).kind, GhosttyKeybindKeyKind::CatchAll);
    QCOMPARE(
        root.actions,
        QStringList({QStringLiteral("new_tab"), QStringLiteral("goto_tab:2")}));
    QVERIFY(!root.flags.consumed);
    QVERIFY(!root.flags.all);
    QVERIFY(root.flags.performable);
    QVERIFY(parsed->keybindings.root.at(1).flags.all);
    QCOMPARE(parsed->keybindings.tables.size(), 1);
    QCOMPARE(parsed->keybindings.tables.constFirst().name,
             QStringLiteral("modeé"));
    QCOMPARE(parsed->defaultKeybindings.root.size(), 1);
    QCOMPARE(parsed->defaultKeybindings.root.constFirst().actions,
             QStringList({QStringLiteral("toggle_command_palette")}));
}

void GhosttyConfigExportTest::rejectsMalformedEnvelope()
{
    auto parsed = parseGhosttyConfigExportJson(QByteArrayLiteral("{not-json"));
    QVERIFY(!parsed);
    QVERIFY(parsed.error().startsWith(
        QStringLiteral("Invalid Ghostty structured config JSON at offset ")));

    parsed = parseGhosttyConfigExportJson(QByteArrayLiteral("[]"));
    QVERIFY(!parsed);
    QCOMPARE(parsed.error(),
             QStringLiteral(
                 "Ghostty structured config JSON root must be an object"));

    QJsonObject malformed = object();
    malformed.insert(QStringLiteral("version"), 2);
    parsed = parseGhosttyConfigExportJson(json(malformed));
    QVERIFY(!parsed);
    QCOMPARE(parsed.error(),
             QStringLiteral(
                 "Unsupported Ghostty structured config JSON schema version"));

    malformed = object();
    malformed.remove(QStringLiteral("values"));
    malformed.insert(QStringLiteral("application"), QJsonObject{});
    parsed = parseGhosttyConfigExportJson(json(malformed));
    QVERIFY(!parsed);
    QVERIFY(parsed.error().contains(
        QStringLiteral("unexpected field 'application'")));

    malformed = object();
    malformed.insert(QStringLiteral("future"), true);
    parsed = parseGhosttyConfigExportJson(json(malformed));
    QVERIFY(!parsed);
    QVERIFY(
        parsed.error().contains(QStringLiteral("unexpected field 'future'")));
}

void GhosttyConfigExportTest::rejectsInvalidValues_data()
{
    QTest::addColumn<QJsonObject>("exportObject");
    QTest::addColumn<QString>("diagnostic");

    QTest::newRow("missing-term")
        << withoutValue(object(), QStringLiteral("term"))
        << QStringLiteral("values is missing field 'term'");
    QTest::newRow("term-type")
        << withValue(object(), QStringLiteral("term"), true)
        << QStringLiteral("values.term must be an array");
    QTest::newRow("term-empty")
        << withValue(object(), QStringLiteral("term"), QJsonArray{})
        << QStringLiteral("values.term must be a non-empty byte array");
    QTest::newRow("term-byte-range")
        << withValue(object(), QStringLiteral("term"), QJsonArray{256})
        << QStringLiteral(
               "values.term[0] must be an unsigned integer in range");
    QTest::newRow("missing-command")
        << withoutValue(object(), QStringLiteral("command"))
        << QStringLiteral("values is missing field 'command'");
    QTest::newRow("missing-initial-command")
        << withoutValue(object(), QStringLiteral("initial-command"))
        << QStringLiteral("values is missing field 'initial-command'");
    QTest::newRow("missing-wait-after-command")
        << withoutValue(object(), QStringLiteral("wait-after-command"))
        << QStringLiteral("values is missing field 'wait-after-command'");
    QTest::newRow("missing-abnormal-command-exit-runtime")
        << withoutValue(object(),
                        QStringLiteral("abnormal-command-exit-runtime"))
        << QStringLiteral(
               "values is missing field 'abnormal-command-exit-runtime'");
    QTest::newRow("command-type")
        << withValue(object(), QStringLiteral("command"),
                     QStringLiteral("/bin/sh"))
        << QStringLiteral("values.command must be an object or null");
    QTest::newRow("command-missing-kind")
        << withValue(object(), QStringLiteral("command"), QJsonObject{})
        << QStringLiteral("values.command.kind must be a string");
    QTest::newRow("command-kind-type")
        << withValue(object(), QStringLiteral("command"),
                     QJsonObject{{QStringLiteral("kind"), true}})
        << QStringLiteral("values.command.kind must be a string");
    QTest::newRow("command-kind-unsupported")
        << withValue(object(), QStringLiteral("command"),
                     QJsonObject{
                         {QStringLiteral("kind"), QStringLiteral("expanded")}})
        << QStringLiteral(
               "values.command.kind has unsupported value 'expanded'");
    QTest::newRow("shell-command-missing-value")
        << withValue(object(), QStringLiteral("command"),
                     QJsonObject{
                         {QStringLiteral("kind"), QStringLiteral("shell")},
                         {QStringLiteral("default-shell"), false},
                     })
        << QStringLiteral("values.command is missing field 'value'");
    QTest::newRow("shell-command-missing-default") << withValue(
        object(), QStringLiteral("command"),
        QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("shell")},
            {QStringLiteral("value"), bytes(QByteArrayLiteral("/bin/sh"))},
        }) << QStringLiteral("values.command is missing field 'default-shell'");
    QTest::newRow("shell-command-extra-field") << withValue(
        object(), QStringLiteral("command"),
        QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("shell")},
            {QStringLiteral("value"), bytes(QByteArrayLiteral("/bin/sh"))},
            {QStringLiteral("default-shell"), false},
            {QStringLiteral("argv"), QJsonArray{}},
        }) << QStringLiteral("values.command has unexpected field 'argv'");
    QTest::newRow("shell-command-value-type")
        << withValue(object(), QStringLiteral("command"),
                     QJsonObject{
                         {QStringLiteral("kind"), QStringLiteral("shell")},
                         {QStringLiteral("value"), true},
                         {QStringLiteral("default-shell"), false},
                     })
        << QStringLiteral("values.command.value must be an array");
    QTest::newRow("shell-command-value-nul")
        << withValue(object(), QStringLiteral("command"),
                     shellCommand(QByteArray("sh\0-c", 5)))
        << QStringLiteral("values.command.value must not contain NUL");
    QTest::newRow("shell-command-default-type") << withValue(
        object(), QStringLiteral("command"),
        QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("shell")},
            {QStringLiteral("value"), bytes(QByteArrayLiteral("/bin/sh"))},
            {QStringLiteral("default-shell"), QStringLiteral("false")},
        }) << QStringLiteral("values.command.default-shell must be a boolean");
    QTest::newRow("direct-command-missing-argv")
        << withValue(object(), QStringLiteral("command"),
                     QJsonObject{
                         {QStringLiteral("kind"), QStringLiteral("direct")},
                         {QStringLiteral("default-shell"), false},
                     })
        << QStringLiteral("values.command is missing field 'argv'");
    QTest::newRow("direct-command-zero-argv")
        << withValue(object(), QStringLiteral("command"),
                     directCommand(QJsonArray{}))
        << QStringLiteral("values.command.argv must contain at least argv[0]");
    QTest::newRow("direct-command-argv-type")
        << withValue(object(), QStringLiteral("command"),
                     QJsonObject{
                         {QStringLiteral("kind"), QStringLiteral("direct")},
                         {QStringLiteral("argv"), true},
                         {QStringLiteral("default-shell"), false},
                     })
        << QStringLiteral("values.command.argv must be an array");
    QTest::newRow("direct-command-argument-type")
        << withValue(object(), QStringLiteral("command"), directCommand({true}))
        << QStringLiteral("values.command.argv[0] must be an array");
    QTest::newRow("direct-command-argument-byte")
        << withValue(object(), QStringLiteral("command"),
                     directCommand({QJsonArray{256}}))
        << QStringLiteral(
               "values.command.argv[0][0] must be an unsigned integer in range");
    QTest::newRow("direct-command-argument-nul")
        << withValue(object(), QStringLiteral("command"),
                     directCommand({bytes(QByteArray("a\0b", 3))}))
        << QStringLiteral("values.command.argv[0] must not contain NUL");
    QTest::newRow("direct-command-default-shell")
        << withValue(
               object(), QStringLiteral("command"),
               directCommand({bytes(QByteArrayLiteral("/bin/direct"))}, true))
        << QStringLiteral(
               "values.command.default-shell is invalid for this command");
    QTest::newRow("initial-command-default-shell")
        << withValue(object(), QStringLiteral("initial-command"),
                     shellCommand(QByteArrayLiteral("/bin/sh"), true))
        << QStringLiteral(
               "values.initial-command.default-shell is invalid for this command");
    QTest::newRow("wait-after-command-type")
        << withValue(object(), QStringLiteral("wait-after-command"),
                     QStringLiteral("true"))
        << QStringLiteral("values.wait-after-command must be a boolean");
    QTest::newRow("abnormal-command-exit-runtime-type")
        << withValue(object(), QStringLiteral("abnormal-command-exit-runtime"),
                     QStringLiteral("250"))
        << QStringLiteral(
               "values.abnormal-command-exit-runtime must be an unsigned integer");
    QTest::newRow("abnormal-command-exit-runtime-negative")
        << withValue(object(), QStringLiteral("abnormal-command-exit-runtime"),
                     -1)
        << QStringLiteral(
               "values.abnormal-command-exit-runtime must be an unsigned integer");
    QTest::newRow("abnormal-command-exit-runtime-fractional")
        << withValue(object(), QStringLiteral("abnormal-command-exit-runtime"),
                     0.5)
        << QStringLiteral(
               "values.abnormal-command-exit-runtime must be an unsigned integer");
    QTest::newRow("abnormal-command-exit-runtime-overflow")
        << withValue(object(), QStringLiteral("abnormal-command-exit-runtime"),
                     4294967296.0)
        << QStringLiteral(
               "values.abnormal-command-exit-runtime must be an unsigned integer");
    QTest::newRow("missing-env")
        << withoutValue(object(), QStringLiteral("env"))
        << QStringLiteral("values is missing field 'env'");
    QTest::newRow("missing-shell-integration")
        << withoutValue(object(), QStringLiteral("shell-integration"))
        << QStringLiteral("values is missing field 'shell-integration'");
    QTest::newRow("missing-shell-integration-features")
        << withoutValue(object(), QStringLiteral("shell-integration-features"))
        << QStringLiteral(
               "values is missing field 'shell-integration-features'");
    QTest::newRow("shell-integration-features-type")
        << withValue(object(), QStringLiteral("shell-integration-features"),
                     true)
        << QStringLiteral(
               "values.shell-integration-features must be an object");
    QTest::newRow("shell-integration-features-missing-field")
        << withValue(object(), QStringLiteral("shell-integration-features"),
                     QJsonObject{
                         {QStringLiteral("sudo"), true},
                         {QStringLiteral("title"), false},
                         {QStringLiteral("ssh-env"), true},
                         {QStringLiteral("ssh-terminfo"), true},
                         {QStringLiteral("path"), false},
                     })
        << QStringLiteral(
               "values.shell-integration-features is missing field 'cursor'");
    QTest::newRow("shell-integration-features-extra-field")
        << withValue(object(), QStringLiteral("shell-integration-features"),
                     QJsonObject{
                         {QStringLiteral("cursor"), false},
                         {QStringLiteral("sudo"), true},
                         {QStringLiteral("title"), false},
                         {QStringLiteral("ssh-env"), true},
                         {QStringLiteral("ssh-terminfo"), true},
                         {QStringLiteral("path"), false},
                         {QStringLiteral("future"), true},
                     })
        << QStringLiteral(
               "values.shell-integration-features has unexpected field 'future'");
    QTest::newRow("shell-integration-features-field-type")
        << withValue(object(), QStringLiteral("shell-integration-features"),
                     QJsonObject{
                         {QStringLiteral("cursor"), false},
                         {QStringLiteral("sudo"), true},
                         {QStringLiteral("title"), false},
                         {QStringLiteral("ssh-env"), QStringLiteral("true")},
                         {QStringLiteral("ssh-terminfo"), true},
                         {QStringLiteral("path"), false},
                     })
        << QStringLiteral(
               "values.shell-integration-features.ssh-env must be a boolean");
    QTest::newRow("env-type")
        << withValue(object(), QStringLiteral("env"), true)
        << QStringLiteral("values.env must be an array");
    QTest::newRow("env-entry-type")
        << withValue(object(), QStringLiteral("env"), QJsonArray{true})
        << QStringLiteral("values.env[0] must be an object");
    QTest::newRow("env-entry-missing-key") << withValue(
        object(), QStringLiteral("env"),
        QJsonArray{QJsonObject{
            {QStringLiteral("value"), bytes(QByteArrayLiteral("value"))},
        }}) << QStringLiteral("values.env[0] is missing field 'key'");
    QTest::newRow("env-entry-missing-value") << withValue(
        object(), QStringLiteral("env"),
        QJsonArray{QJsonObject{
            {QStringLiteral("key"), bytes(QByteArrayLiteral("KEY"))},
        }}) << QStringLiteral("values.env[0] is missing field 'value'");
    QTest::newRow("env-entry-extra-field") << withValue(
        object(), QStringLiteral("env"),
        QJsonArray{QJsonObject{
            {QStringLiteral("key"), bytes(QByteArrayLiteral("KEY"))},
            {QStringLiteral("value"), bytes(QByteArrayLiteral("value"))},
            {QStringLiteral("future"), true},
        }}) << QStringLiteral("values.env[0] has unexpected field 'future'");
    QTest::newRow("env-key-type") << withValue(
        object(), QStringLiteral("env"),
        QJsonArray{QJsonObject{
            {QStringLiteral("key"), true},
            {QStringLiteral("value"), bytes(QByteArrayLiteral("value"))},
        }}) << QStringLiteral("values.env[0].key must be an array");
    QTest::newRow("env-value-type") << withValue(
        object(), QStringLiteral("env"),
        QJsonArray{QJsonObject{
            {QStringLiteral("key"), bytes(QByteArrayLiteral("KEY"))},
            {QStringLiteral("value"), true},
        }}) << QStringLiteral("values.env[0].value must be an array");
    QTest::newRow("env-key-byte-range")
        << withValue(
               object(), QStringLiteral("env"),
               QJsonArray{QJsonObject{
                   {QStringLiteral("key"), QJsonArray{256}},
                   {QStringLiteral("value"), bytes(QByteArrayLiteral("value"))},
               }})
        << QStringLiteral(
               "values.env[0].key[0] must be an unsigned integer in range");
    QTest::newRow("env-value-byte-range")
        << withValue(
               object(), QStringLiteral("env"),
               QJsonArray{QJsonObject{
                   {QStringLiteral("key"), bytes(QByteArrayLiteral("KEY"))},
                   {QStringLiteral("value"), QJsonArray{-1}},
               }})
        << QStringLiteral(
               "values.env[0].value[0] must be an unsigned integer in range");
    QTest::newRow("env-key-equals")
        << withValue(object(), QStringLiteral("env"),
                     QJsonArray{environmentEntry(QByteArrayLiteral("BAD=KEY"),
                                                 QByteArrayLiteral("value"))})
        << QStringLiteral("values.env[0].key must not contain '='");
    QTest::newRow("env-key-nul") << withValue(
        object(), QStringLiteral("env"),
        QJsonArray{QJsonObject{
            {QStringLiteral("key"), QJsonArray{'K', 0, 'Y'}},
            {QStringLiteral("value"), bytes(QByteArrayLiteral("value"))},
        }}) << QStringLiteral("values.env[0].key must not contain NUL");
    QTest::newRow("env-value-empty")
        << withValue(object(), QStringLiteral("env"),
                     QJsonArray{environmentEntry(QByteArrayLiteral("KEY"),
                                                 QByteArrayView{})})
        << QStringLiteral("values.env[0].value must be a non-empty byte array");
    QTest::newRow("env-value-nul") << withValue(
        object(), QStringLiteral("env"),
        QJsonArray{QJsonObject{
            {QStringLiteral("key"), bytes(QByteArrayLiteral("KEY"))},
            {QStringLiteral("value"), QJsonArray{'v', 0, 'e'}},
        }}) << QStringLiteral("values.env[0].value must not contain NUL");
    QTest::newRow("env-duplicate-key")
        << withValue(object(), QStringLiteral("env"),
                     QJsonArray{
                         environmentEntry(QByteArrayLiteral("KEY"),
                                          QByteArrayLiteral("first")),
                         environmentEntry(QByteArrayLiteral("KEY"),
                                          QByteArrayLiteral("second")),
                     })
        << QStringLiteral("values.env[1] contains duplicate key");
    QTest::newRow("missing-linux-cgroup")
        << withoutValue(object(), QStringLiteral("linux-cgroup"))
        << QStringLiteral("values is missing field 'linux-cgroup'");
    QTest::newRow("missing-linux-cgroup-memory-limit")
        << withoutValue(object(), QStringLiteral("linux-cgroup-memory-limit"))
        << QStringLiteral(
               "values is missing field 'linux-cgroup-memory-limit'");
    QTest::newRow("missing-linux-cgroup-processes-limit")
        << withoutValue(object(),
                        QStringLiteral("linux-cgroup-processes-limit"))
        << QStringLiteral(
               "values is missing field 'linux-cgroup-processes-limit'");
    QTest::newRow("missing-linux-cgroup-hard-fail")
        << withoutValue(object(), QStringLiteral("linux-cgroup-hard-fail"))
        << QStringLiteral("values is missing field 'linux-cgroup-hard-fail'");
    QTest::newRow("linux-cgroup-hard-fail-type")
        << withValue(object(), QStringLiteral("linux-cgroup-hard-fail"),
                     QStringLiteral("true"))
        << QStringLiteral("values.linux-cgroup-hard-fail must be a boolean");
    QTest::newRow("missing-field")
        << withoutValue(object(), QStringLiteral("font-size"))
        << QStringLiteral("values is missing field 'font-size'");
    QTest::newRow("missing-font-style")
        << withoutValue(object(), QStringLiteral("font-style-bold"))
        << QStringLiteral("values is missing field 'font-style-bold'");
    QTest::newRow("missing-metric-modifier")
        << withoutValue(object(), QStringLiteral("adjust-cursor-height"))
        << QStringLiteral("values is missing field 'adjust-cursor-height'");
    QTest::newRow("missing-metric-modifier-order")
        << withoutValue(object(), QStringLiteral("metric-modifier-order"))
        << QStringLiteral("values is missing field 'metric-modifier-order'");
    QTest::newRow("missing-scrollbar")
        << withoutValue(object(), QStringLiteral("scrollbar"))
        << QStringLiteral("values is missing field 'scrollbar'");
    QTest::newRow("missing-scrollback-compression")
        << withoutValue(object(), QStringLiteral("scrollback-compression"))
        << QStringLiteral("values is missing field 'scrollback-compression'");
    QTest::newRow("missing-bell-features")
        << withoutValue(object(), QStringLiteral("bell-features"))
        << QStringLiteral("values is missing field 'bell-features'");
    QTest::newRow("missing-bell-audio-path")
        << withoutValue(object(), QStringLiteral("bell-audio-path"))
        << QStringLiteral("values is missing field 'bell-audio-path'");
    QTest::newRow("missing-bell-audio-volume")
        << withoutValue(object(), QStringLiteral("bell-audio-volume"))
        << QStringLiteral("values is missing field 'bell-audio-volume'");
    QTest::newRow("missing-mouse-scroll-multiplier")
        << withoutValue(object(), QStringLiteral("mouse-scroll-multiplier"))
        << QStringLiteral("values is missing field 'mouse-scroll-multiplier'");
    QTest::newRow("missing-mouse-hide-while-typing")
        << withoutValue(object(), QStringLiteral("mouse-hide-while-typing"))
        << QStringLiteral("values is missing field 'mouse-hide-while-typing'");
    QTest::newRow("missing-scroll-to-bottom")
        << withoutValue(object(), QStringLiteral("scroll-to-bottom"))
        << QStringLiteral("values is missing field 'scroll-to-bottom'");
    QTest::newRow("missing-focus-follows-mouse")
        << withoutValue(object(), QStringLiteral("focus-follows-mouse"))
        << QStringLiteral("values is missing field 'focus-follows-mouse'");
    QTest::newRow("missing-selection-word-chars")
        << withoutValue(object(), QStringLiteral("selection-word-chars"))
        << QStringLiteral("values is missing field 'selection-word-chars'");
    QTest::newRow("missing-click-repeat-interval")
        << withoutValue(object(), QStringLiteral("click-repeat-interval"))
        << QStringLiteral("values is missing field 'click-repeat-interval'");
    QTest::newRow("missing-clipboard-write")
        << withoutValue(object(), QStringLiteral("clipboard-write"))
        << QStringLiteral("values is missing field 'clipboard-write'");
    QTest::newRow("missing-right-click-action")
        << withoutValue(object(), QStringLiteral("right-click-action"))
        << QStringLiteral("values is missing field 'right-click-action'");
    QTest::newRow("missing-mouse-shift-capture")
        << withoutValue(object(), QStringLiteral("mouse-shift-capture"))
        << QStringLiteral("values is missing field 'mouse-shift-capture'");
    QTest::newRow("bell-features-type")
        << withValue(object(), QStringLiteral("bell-features"), true)
        << QStringLiteral("values.bell-features must be an object");
    QTest::newRow("bell-features-missing-flag")
        << withValue(object(), QStringLiteral("bell-features"),
                     QJsonObject{
                         {QStringLiteral("audio"), false},
                         {QStringLiteral("attention"), true},
                         {QStringLiteral("title"), true},
                         {QStringLiteral("border"), false},
                     })
        << QStringLiteral("values.bell-features is missing field 'system'");
    QTest::newRow("bell-features-extra-flag")
        << withValue(object(), QStringLiteral("bell-features"),
                     QJsonObject{
                         {QStringLiteral("system"), false},
                         {QStringLiteral("audio"), false},
                         {QStringLiteral("attention"), true},
                         {QStringLiteral("title"), true},
                         {QStringLiteral("border"), false},
                         {QStringLiteral("future"), true},
                     })
        << QStringLiteral("values.bell-features has unexpected field 'future'");
    QTest::newRow("bell-features-flag-type")
        << withValue(object(), QStringLiteral("bell-features"),
                     QJsonObject{
                         {QStringLiteral("system"), false},
                         {QStringLiteral("audio"), QStringLiteral("false")},
                         {QStringLiteral("attention"), true},
                         {QStringLiteral("title"), true},
                         {QStringLiteral("border"), false},
                     })
        << QStringLiteral("values.bell-features.audio must be a boolean");
    QTest::newRow("bell-audio-path-type")
        << withValue(object(), QStringLiteral("bell-audio-path"),
                     QStringLiteral("/work/bell.oga"))
        << QStringLiteral("values.bell-audio-path must be an object");
    QTest::newRow("bell-audio-path-missing-path")
        << withValue(object(), QStringLiteral("bell-audio-path"),
                     QJsonObject{{QStringLiteral("optional"), false}})
        << QStringLiteral("values.bell-audio-path is missing field 'path'");
    QTest::newRow("bell-audio-path-missing-optional")
        << withValue(object(), QStringLiteral("bell-audio-path"),
                     QJsonObject{{QStringLiteral("path"),
                                  QStringLiteral("/work/bell.oga")}})
        << QStringLiteral("values.bell-audio-path is missing field 'optional'");
    QTest::newRow("bell-audio-path-extra-field")
        << withValue(
               object(), QStringLiteral("bell-audio-path"),
               QJsonObject{
                   {QStringLiteral("path"), QStringLiteral("/work/bell.oga")},
                   {QStringLiteral("optional"), false},
                   {QStringLiteral("future"), true},
               })
        << QStringLiteral(
               "values.bell-audio-path has unexpected field 'future'");
    QTest::newRow("bell-audio-path-empty")
        << withValue(object(), QStringLiteral("bell-audio-path"),
                     finalizedConfigPath(QString{}))
        << QStringLiteral(
               "values.bell-audio-path.path must be a non-empty string");
    QTest::newRow("bell-audio-path-relative")
        << withValue(object(), QStringLiteral("bell-audio-path"),
                     finalizedConfigPath(QStringLiteral("sounds/bell.oga")))
        << QStringLiteral(
               "values.bell-audio-path.path must be a finalized absolute path");
    QTest::newRow("bell-audio-path-optional-type")
        << withValue(
               object(), QStringLiteral("bell-audio-path"),
               QJsonObject{
                   {QStringLiteral("path"), QStringLiteral("/work/bell.oga")},
                   {QStringLiteral("optional"), QStringLiteral("false")},
               })
        << QStringLiteral("values.bell-audio-path.optional must be a boolean");
    QTest::newRow("bell-audio-volume-type")
        << withValue(object(), QStringLiteral("bell-audio-volume"), true)
        << QStringLiteral("values.bell-audio-volume must be a finite number");
    QTest::newRow("bell-audio-volume-nonfinite")
        << withValue(object(), QStringLiteral("bell-audio-volume"),
                     std::numeric_limits<double>::infinity())
        << QStringLiteral("values.bell-audio-volume must be a finite number");
    QTest::newRow("mouse-scroll-multiplier-type")
        << withValue(object(), QStringLiteral("mouse-scroll-multiplier"), true)
        << QStringLiteral("values.mouse-scroll-multiplier must be an object");
    QTest::newRow("mouse-scroll-multiplier-missing-precision")
        << withValue(object(), QStringLiteral("mouse-scroll-multiplier"),
                     QJsonObject{{QStringLiteral("discrete"), 3.0}})
        << QStringLiteral(
               "values.mouse-scroll-multiplier is missing field 'precision'");
    QTest::newRow("mouse-scroll-multiplier-missing-discrete")
        << withValue(object(), QStringLiteral("mouse-scroll-multiplier"),
                     QJsonObject{{QStringLiteral("precision"), 1.0}})
        << QStringLiteral(
               "values.mouse-scroll-multiplier is missing field 'discrete'");
    QTest::newRow("mouse-scroll-multiplier-extra-field")
        << withValue(object(), QStringLiteral("mouse-scroll-multiplier"),
                     QJsonObject{
                         {QStringLiteral("precision"), 1.0},
                         {QStringLiteral("discrete"), 3.0},
                         {QStringLiteral("future"), true},
                     })
        << QStringLiteral(
               "values.mouse-scroll-multiplier has unexpected field 'future'");
    QTest::newRow("mouse-scroll-multiplier-precision-type")
        << withValue(object(), QStringLiteral("mouse-scroll-multiplier"),
                     QJsonObject{
                         {QStringLiteral("precision"), QStringLiteral("1")},
                         {QStringLiteral("discrete"), 3.0},
                     })
        << QStringLiteral(
               "values.mouse-scroll-multiplier.precision must be a finite number");
    QTest::newRow("mouse-scroll-multiplier-discrete-nonfinite")
        << withValue(object(), QStringLiteral("mouse-scroll-multiplier"),
                     mouseScrollMultiplier(
                         1.0, std::numeric_limits<double>::infinity()))
        << QStringLiteral(
               "values.mouse-scroll-multiplier.discrete must be a finite number");
    QTest::newRow("mouse-scroll-multiplier-below-finalized-range")
        << withValue(object(), QStringLiteral("mouse-scroll-multiplier"),
                     mouseScrollMultiplier(0.009, 3.0))
        << QStringLiteral(
               "values.mouse-scroll-multiplier.precision is outside its supported range");
    QTest::newRow("mouse-scroll-multiplier-above-finalized-range")
        << withValue(object(), QStringLiteral("mouse-scroll-multiplier"),
                     mouseScrollMultiplier(1.0, 10'000.01))
        << QStringLiteral(
               "values.mouse-scroll-multiplier.discrete is outside its supported range");
    QTest::newRow("mouse-hide-while-typing-type")
        << withValue(object(), QStringLiteral("mouse-hide-while-typing"), 1)
        << QStringLiteral("values.mouse-hide-while-typing must be a boolean");
    QTest::newRow("scroll-to-bottom-type")
        << withValue(object(), QStringLiteral("scroll-to-bottom"), true)
        << QStringLiteral("values.scroll-to-bottom must be an object");
    QTest::newRow("scroll-to-bottom-missing-keystroke")
        << withValue(object(), QStringLiteral("scroll-to-bottom"),
                     QJsonObject{{QStringLiteral("output"), false}})
        << QStringLiteral(
               "values.scroll-to-bottom is missing field 'keystroke'");
    QTest::newRow("scroll-to-bottom-missing-output")
        << withValue(object(), QStringLiteral("scroll-to-bottom"),
                     QJsonObject{{QStringLiteral("keystroke"), true}})
        << QStringLiteral("values.scroll-to-bottom is missing field 'output'");
    QTest::newRow("scroll-to-bottom-extra-field")
        << withValue(object(), QStringLiteral("scroll-to-bottom"),
                     QJsonObject{
                         {QStringLiteral("keystroke"), true},
                         {QStringLiteral("output"), false},
                         {QStringLiteral("future"), true},
                     })
        << QStringLiteral(
               "values.scroll-to-bottom has unexpected field 'future'");
    QTest::newRow("scroll-to-bottom-keystroke-type")
        << withValue(object(), QStringLiteral("scroll-to-bottom"),
                     QJsonObject{
                         {QStringLiteral("keystroke"), QStringLiteral("true")},
                         {QStringLiteral("output"), false},
                     })
        << QStringLiteral(
               "values.scroll-to-bottom.keystroke must be a boolean");
    QTest::newRow("scroll-to-bottom-output-type")
        << withValue(object(), QStringLiteral("scroll-to-bottom"),
                     QJsonObject{
                         {QStringLiteral("keystroke"), true},
                         {QStringLiteral("output"), 0},
                     })
        << QStringLiteral("values.scroll-to-bottom.output must be a boolean");
    QTest::newRow("focus-follows-mouse-type")
        << withValue(object(), QStringLiteral("focus-follows-mouse"), 1)
        << QStringLiteral("values.focus-follows-mouse must be a boolean");
    QTest::newRow("selection-word-chars-type")
        << withValue(object(), QStringLiteral("selection-word-chars"), true)
        << QStringLiteral("values.selection-word-chars must be an array");
    QTest::newRow("selection-word-chars-negative")
        << withValue(object(), QStringLiteral("selection-word-chars"),
                     QJsonArray{0, -1})
        << QStringLiteral(
               "values.selection-word-chars[1] must be a Unicode scalar value");
    QTest::newRow("selection-word-chars-too-large")
        << withValue(object(), QStringLiteral("selection-word-chars"),
                     QJsonArray{0, 0x110000})
        << QStringLiteral(
               "values.selection-word-chars[1] must be a Unicode scalar value");
    QTest::newRow("selection-word-chars-surrogate")
        << withValue(object(), QStringLiteral("selection-word-chars"),
                     QJsonArray{0, 0xd800})
        << QStringLiteral(
               "values.selection-word-chars[1] must be a Unicode scalar value");
    QTest::newRow("selection-word-chars-fractional")
        << withValue(object(), QStringLiteral("selection-word-chars"),
                     QJsonArray{0, 1.5})
        << QStringLiteral(
               "values.selection-word-chars[1] must be a Unicode scalar value");
    QTest::newRow("selection-word-chars-nonnumeric")
        << withValue(object(), QStringLiteral("selection-word-chars"),
                     QJsonArray{0, QStringLiteral("65")})
        << QStringLiteral(
               "values.selection-word-chars[1] must be a Unicode scalar value");
    QTest::newRow("selection-word-chars-empty")
        << withValue(object(), QStringLiteral("selection-word-chars"),
                     QJsonArray{})
        << QStringLiteral("values.selection-word-chars must begin with U+0000");
    QTest::newRow("selection-word-chars-missing-leading-null")
        << withValue(object(), QStringLiteral("selection-word-chars"),
                     QJsonArray{65})
        << QStringLiteral("values.selection-word-chars must begin with U+0000");
    QTest::newRow("click-repeat-interval-zero")
        << withValue(object(), QStringLiteral("click-repeat-interval"), 0)
        << QStringLiteral(
               "values.click-repeat-interval must be a nonzero unsigned integer");
    QTest::newRow("click-repeat-interval-negative")
        << withValue(object(), QStringLiteral("click-repeat-interval"), -1)
        << QStringLiteral(
               "values.click-repeat-interval must be an unsigned integer");
    QTest::newRow("click-repeat-interval-fractional")
        << withValue(object(), QStringLiteral("click-repeat-interval"), 1.5)
        << QStringLiteral(
               "values.click-repeat-interval must be an unsigned integer");
    QTest::newRow("click-repeat-interval-overflow")
        << withValue(object(), QStringLiteral("click-repeat-interval"),
                     4294967296.0)
        << QStringLiteral(
               "values.click-repeat-interval must be an unsigned integer");
    QTest::newRow("click-repeat-interval-nonnumeric")
        << withValue(object(), QStringLiteral("click-repeat-interval"),
                     QStringLiteral("500"))
        << QStringLiteral(
               "values.click-repeat-interval must be an unsigned integer");
    QTest::newRow("working-directory-empty")
        << withValue(object(), QStringLiteral("working-directory"), QString{})
        << QStringLiteral(
               "values.working-directory must be 'inherit' or a finalized path");
    QTest::newRow("working-directory-unfinalized-home")
        << withValue(object(), QStringLiteral("working-directory"),
                     QStringLiteral("home"))
        << QStringLiteral(
               "values.working-directory must be 'inherit' or a finalized path");

    QJsonObject extra = object();
    QJsonObject extraValues = extra.value(QStringLiteral("values")).toObject();
    extraValues.insert(QStringLiteral("future"), true);
    extra.insert(QStringLiteral("values"), extraValues);
    QTest::newRow("extra-field")
        << extra << QStringLiteral("unexpected field 'future'");

    QTest::newRow("font-size-type")
        << withValue(object(), QStringLiteral("font-size"), true)
        << QStringLiteral("values.font-size must be a finite number");
    QTest::newRow("font-size-nonfinite")
        << withValue(object(), QStringLiteral("font-size"),
                     std::numeric_limits<double>::infinity())
        << QStringLiteral("values.font-size must be a finite number");
    QTest::newRow("font-family-type")
        << withValue(object(), QStringLiteral("font-family-bold"), true)
        << QStringLiteral("values.font-family-bold must be an array");
    QTest::newRow("font-family-empty-entry")
        << withValue(object(), QStringLiteral("font-family-bold"),
                     QJsonArray{QString{}})
        << QStringLiteral(
               "values.font-family-bold[0] must be a non-empty string");
    QTest::newRow("font-style-type")
        << withValue(object(), QStringLiteral("font-style"), true)
        << QStringLiteral("values.font-style must be an object");
    QTest::newRow("font-style-missing-kind")
        << withValue(object(), QStringLiteral("font-style"), QJsonObject{})
        << QStringLiteral("values.font-style is missing field 'kind'");
    QTest::newRow("font-style-kind-type")
        << withValue(object(), QStringLiteral("font-style"),
                     QJsonObject{{QStringLiteral("kind"), true}})
        << QStringLiteral("values.font-style.kind must be a string");
    QTest::newRow("font-style-unknown-kind")
        << withValue(object(), QStringLiteral("font-style"),
                     QJsonObject{{QStringLiteral("kind"),
                                  QStringLiteral("synthesized")}})
        << QStringLiteral(
               "values.font-style.kind has unsupported value 'synthesized'");
    QTest::newRow("font-style-unexpected-name")
        << withValue(object(), QStringLiteral("font-style"),
                     QJsonObject{
                         {QStringLiteral("kind"), QStringLiteral("automatic")},
                         {QStringLiteral("name"), QStringLiteral("Regular")},
                     })
        << QStringLiteral("values.font-style has unexpected field 'name'");
    QTest::newRow("font-style-named-missing-name")
        << withValue(
               object(), QStringLiteral("font-style"),
               QJsonObject{{QStringLiteral("kind"), QStringLiteral("named")}})
        << QStringLiteral("values.font-style is missing field 'name'");
    QTest::newRow("font-style-name-type")
        << withValue(object(), QStringLiteral("font-style"),
                     QJsonObject{
                         {QStringLiteral("kind"), QStringLiteral("named")},
                         {QStringLiteral("name"), 7},
                     })
        << QStringLiteral("values.font-style.name must be a string");
    QTest::newRow("font-style-empty-name")
        << withValue(object(), QStringLiteral("font-style"),
                     namedFontStyle(QString{}))
        << QStringLiteral("values.font-style.name must be a non-empty string");
    QTest::newRow("metric-modifier-type")
        << withValue(object(), QStringLiteral("adjust-cell-width"), true)
        << QStringLiteral("values.adjust-cell-width must be an object");
    QTest::newRow("metric-modifier-missing-value")
        << withValue(object(), QStringLiteral("adjust-cell-width"),
                     QJsonObject{
                         {QStringLiteral("kind"), QStringLiteral("absolute")}})
        << QStringLiteral("values.adjust-cell-width is missing field 'value'");
    QTest::newRow("metric-modifier-extra-field")
        << withValue(object(), QStringLiteral("adjust-cell-width"),
                     QJsonObject{
                         {QStringLiteral("kind"), QStringLiteral("absolute")},
                         {QStringLiteral("value"), 1},
                         {QStringLiteral("pixels"), 1},
                     })
        << QStringLiteral(
               "values.adjust-cell-width has unexpected field 'pixels'");
    QTest::newRow("metric-modifier-kind-type")
        << withValue(object(), QStringLiteral("adjust-cell-width"),
                     QJsonObject{
                         {QStringLiteral("kind"), true},
                         {QStringLiteral("value"), 1},
                     })
        << QStringLiteral("values.adjust-cell-width.kind must be a string");
    QTest::newRow("metric-modifier-unknown-kind")
        << withValue(object(), QStringLiteral("adjust-cell-width"),
                     QJsonObject{
                         {QStringLiteral("kind"), QStringLiteral("relative")},
                         {QStringLiteral("value"), 1},
                     })
        << QStringLiteral(
               "values.adjust-cell-width.kind has unsupported value");
    QTest::newRow("absolute-modifier-type")
        << withValue(object(), QStringLiteral("adjust-cell-width"),
                     QJsonObject{
                         {QStringLiteral("kind"), QStringLiteral("absolute")},
                         {QStringLiteral("value"), true},
                     })
        << QStringLiteral(
               "values.adjust-cell-width.value must be a signed integer");
    QTest::newRow("absolute-modifier-fraction")
        << withValue(object(), QStringLiteral("adjust-cell-width"),
                     QJsonObject{
                         {QStringLiteral("kind"), QStringLiteral("absolute")},
                         {QStringLiteral("value"), 1.5},
                     })
        << QStringLiteral(
               "values.adjust-cell-width.value must be a signed integer");
    QTest::newRow("absolute-modifier-range")
        << withValue(object(), QStringLiteral("adjust-cell-width"),
                     QJsonObject{
                         {QStringLiteral("kind"), QStringLiteral("absolute")},
                         {QStringLiteral("value"), 2147483648.0},
                     })
        << QStringLiteral(
               "values.adjust-cell-width.value must be a signed integer in range");
    QTest::newRow("percentage-modifier-type")
        << withValue(object(), QStringLiteral("adjust-cell-width"),
                     QJsonObject{
                         {QStringLiteral("kind"), QStringLiteral("percentage")},
                         {QStringLiteral("value"), true},
                     })
        << QStringLiteral(
               "values.adjust-cell-width.value must be a finite number");
    QTest::newRow("percentage-modifier-negative-multiplier")
        << withValue(object(), QStringLiteral("adjust-cell-width"),
                     percentageMetricModifier(-0.01))
        << QStringLiteral(
               "values.adjust-cell-width.value is outside its supported range");
    QTest::newRow("percentage-modifier-nonfinite")
        << withValue(object(), QStringLiteral("adjust-cell-width"),
                     QJsonObject{
                         {QStringLiteral("kind"), QStringLiteral("percentage")},
                         {QStringLiteral("value"),
                          std::numeric_limits<double>::infinity()},
                     })
        << QStringLiteral(
               "values.adjust-cell-width.value must be a finite number");
    QTest::newRow("metric-modifier-order-type")
        << withValue(object(), QStringLiteral("metric-modifier-order"), true)
        << QStringLiteral("values.metric-modifier-order must be an array");
    QTest::newRow("metric-modifier-order-entry-type")
        << withValue(object(), QStringLiteral("metric-modifier-order"),
                     QJsonArray{true})
        << QStringLiteral("values.metric-modifier-order[0] must be a string");
    QTest::newRow("metric-modifier-order-unsupported")
        << withValue(object(), QStringLiteral("metric-modifier-order"),
                     QJsonArray{QStringLiteral("adjust-box-thickness")})
        << QStringLiteral(
               "values.metric-modifier-order[0] has unsupported value");

    QJsonArray duplicateModifierOrder = metricModifierOrder();
    duplicateModifierOrder.append(QStringLiteral("adjust-cell-width"));
    QTest::newRow("metric-modifier-order-duplicate")
        << withValue(object(), QStringLiteral("metric-modifier-order"),
                     duplicateModifierOrder)
        << QStringLiteral(
               "values.metric-modifier-order contains duplicate modifier");

    QJsonArray unsetModifierOrder = metricModifierOrder();
    unsetModifierOrder.append(QStringLiteral("adjust-underline-thickness"));
    QTest::newRow("metric-modifier-order-unset")
        << withValue(object(), QStringLiteral("metric-modifier-order"),
                     unsetModifierOrder)
        << QStringLiteral(
               "refers to unset modifier 'adjust-underline-thickness'");

    QJsonArray incompleteModifierOrder = metricModifierOrder();
    incompleteModifierOrder.removeFirst();
    QTest::newRow("metric-modifier-order-incomplete")
        << withValue(object(), QStringLiteral("metric-modifier-order"),
                     incompleteModifierOrder)
        << QStringLiteral(
               "does not include active modifier 'adjust-cursor-height'");
    QTest::newRow("split-opacity-range")
        << withValue(object(), QStringLiteral("unfocused-split-opacity"), 0.1)
        << QStringLiteral("values.unfocused-split-opacity is outside");
    QTest::newRow("faint-opacity-range")
        << withValue(object(), QStringLiteral("faint-opacity"), 1.1)
        << QStringLiteral("values.faint-opacity is outside");
    QTest::newRow("canonical-color")
        << withValue(object(), QStringLiteral("foreground"),
                     QStringLiteral("#AABBCC"))
        << QStringLiteral("values.foreground must be a canonical #rrggbb");
    QTest::newRow("nullable-color")
        << withValue(object(), QStringLiteral("split-divider-color"), 7)
        << QStringLiteral("values.split-divider-color must be a string");
    QTest::newRow("required-terminal-color-null")
        << withValue(object(), QStringLiteral("search-foreground"),
                     QJsonValue::Null)
        << QStringLiteral("values.search-foreground must not be null");
    QTest::newRow("terminal-color-sentinel")
        << withValue(object(), QStringLiteral("cursor-color"),
                     QStringLiteral("window-foreground"))
        << QStringLiteral("values.cursor-color must be a canonical #rrggbb");
    QTest::newRow("cursor-blink-type")
        << withValue(object(), QStringLiteral("cursor-style-blink"), 1)
        << QStringLiteral("values.cursor-style-blink must be a boolean");
    QTest::newRow("bold-color")
        << withValue(object(), QStringLiteral("bold-color"),
                     QStringLiteral("dim"))
        << QStringLiteral("values.bold-color must be a canonical #rrggbb");
    QTest::newRow("uint-negative")
        << withValue(object(), QStringLiteral("window-width"), -1)
        << QStringLiteral("values.window-width must be an unsigned integer");
    QTest::newRow("uint-fraction")
        << withValue(object(), QStringLiteral("window-height"), 4.5)
        << QStringLiteral("values.window-height must be an unsigned integer");
    QTest::newRow("uint-overflow")
        << withValue(object(), QStringLiteral("resize-overlay-duration"),
                     4294967296.0)
        << QStringLiteral(
               "values.resize-overlay-duration must be an unsigned integer");
    QTest::newRow("scrollback-number")
        << withValue(object(), QStringLiteral("scrollback-limit"), 50000000)
        << QStringLiteral("values.scrollback-limit must be a string");
    QTest::newRow("scrollback-leading-zero")
        << withValue(object(), QStringLiteral("scrollback-limit"),
                     QStringLiteral("01"))
        << QStringLiteral("canonical unsigned decimal string");
    QTest::newRow("scrollback-overflow")
        << withValue(object(), QStringLiteral("scrollback-limit"),
                     QStringLiteral("18446744073709551616"))
        << QStringLiteral("exceeds the uint64 range");
    QTest::newRow("scrollback-compression-type")
        << withValue(object(), QStringLiteral("scrollback-compression"),
                     QStringLiteral("true"))
        << QStringLiteral("values.scrollback-compression must be a boolean");
    QTest::newRow("linux-cgroup-memory-number")
        << withValue(object(), QStringLiteral("linux-cgroup-memory-limit"), 1)
        << QStringLiteral("values.linux-cgroup-memory-limit must be a string");
    QTest::newRow("linux-cgroup-memory-leading-zero")
        << withValue(object(), QStringLiteral("linux-cgroup-memory-limit"),
                     QStringLiteral("01"))
        << QStringLiteral("canonical unsigned decimal string");
    QTest::newRow("linux-cgroup-processes-overflow")
        << withValue(object(), QStringLiteral("linux-cgroup-processes-limit"),
                     QStringLiteral("18446744073709551616"))
        << QStringLiteral("exceeds the uint64 range");
    QTest::newRow("linux-cgroup-processes-boolean")
        << withValue(object(), QStringLiteral("linux-cgroup-processes-limit"),
                     true)
        << QStringLiteral(
               "values.linux-cgroup-processes-limit must be a string");
    QTest::newRow("delay-negative")
        << withValue(object(),
                     QStringLiteral("quit-after-last-window-closed-delay"), -1)
        << QStringLiteral("must be an unsigned integer");
    QTest::newRow("font-family-member")
        << withValue(object(), QStringLiteral("font-family"), QJsonArray{true})
        << QStringLiteral("values.font-family[0] must be a string");
    QTest::newRow("config-file-empty-path")
        << withValue(object(), QStringLiteral("config-file"),
                     QJsonArray{QString{}})
        << QStringLiteral("values.config-file entries must contain a path");
    QTest::newRow("config-file-empty-optional-path")
        << withValue(object(), QStringLiteral("config-file"),
                     QJsonArray{QStringLiteral("?")})
        << QStringLiteral("values.config-file entries must contain a path");

    QJsonArray shortPalette =
        values().value(QStringLiteral("palette")).toArray();
    shortPalette.removeLast();
    QTest::newRow("palette-length")
        << withValue(object(), QStringLiteral("palette"), shortPalette)
        << QStringLiteral("must contain exactly 256 colors");
    QJsonArray badPalette = values().value(QStringLiteral("palette")).toArray();
    badPalette.replace(42, QStringLiteral("#xyzxyz"));
    QTest::newRow("palette-color")
        << withValue(object(), QStringLiteral("palette"), badPalette)
        << QStringLiteral("values.palette[42]");

    for (const QString &enumName : {
             QStringLiteral("window-new-tab-position"),
             QStringLiteral("window-show-tab-bar"),
             QStringLiteral("window-decoration"),
             QStringLiteral("fullscreen"),
             QStringLiteral("cursor-style"),
             QStringLiteral("confirm-close-surface"),
             QStringLiteral("copy-on-select"),
             QStringLiteral("clipboard-write"),
             QStringLiteral("right-click-action"),
             QStringLiteral("middle-click-action"),
             QStringLiteral("mouse-shift-capture"),
             QStringLiteral("link-previews"),
             QStringLiteral("scrollbar"),
             QStringLiteral("resize-overlay"),
             QStringLiteral("resize-overlay-position"),
             QStringLiteral("gtk-single-instance"),
             QStringLiteral("linux-cgroup"),
             QStringLiteral("shell-integration"),
         }) {
        const QByteArray row = QStringLiteral("enum-%1").arg(enumName).toUtf8();
        QTest::newRow(row.constData())
            << withValue(object(), enumName, QStringLiteral("unsupported"))
            << QStringLiteral("values.%1 has unsupported value").arg(enumName);
    }
}

void GhosttyConfigExportTest::rejectsInvalidValues()
{
    QFETCH(QJsonObject, exportObject);
    QFETCH(QString, diagnostic);
    const auto parsed = parseGhosttyConfigExportJson(json(exportObject));
    QVERIFY(parsed.has_value() == false);
    QVERIFY2(parsed.error().contains(diagnostic), qPrintable(parsed.error()));
}

void GhosttyConfigExportTest::rejectsInvalidBindings()
{
    const auto reject = [](QJsonObject exportObject,
                           const QString &diagnostic) {
        const auto parsed = parseGhosttyConfigExportJson(json(exportObject));
        QVERIFY(parsed.has_value() == false);
        QVERIFY2(parsed.error().contains(diagnostic),
                 qPrintable(parsed.error()));
    };

    QJsonObject exportObject = object();
    QJsonObject bindings = keybindings();
    bindings.insert(
        QStringLiteral("tables"),
        QJsonArray{
            QJsonObject{{QStringLiteral("name"), QStringLiteral("mode")},
                        {QStringLiteral("bindings"), QJsonArray{}}},
            QJsonObject{{QStringLiteral("name"), QStringLiteral("mode")},
                        {QStringLiteral("bindings"), QJsonArray{}}},
        });
    exportObject.insert(QStringLiteral("keybindings"), bindings);
    reject(exportObject,
           QStringLiteral("Duplicate Ghostty keybinding table 'mode'"));

    exportObject = object();
    bindings = keybindings();
    const QJsonObject duplicate =
        binding({unicodeTrigger('a')}, {QStringLiteral("ignore")});
    bindings.insert(QStringLiteral("root"), QJsonArray{duplicate, duplicate});
    exportObject.insert(QStringLiteral("keybindings"), bindings);
    reject(exportObject, QStringLiteral("duplicate trigger sequence"));

    exportObject = object();
    bindings = keybindings();
    bindings.insert(QStringLiteral("root"),
                    QJsonArray{binding({unicodeTrigger(0xd800)},
                                       {QStringLiteral("ignore")})});
    exportObject.insert(QStringLiteral("default-keybindings"), bindings);
    reject(exportObject,
           QStringLiteral("default-keybindings.root[0].sequence[0].codepoint"));

    exportObject = object();
    bindings = keybindings();
    bindings.insert(
        QStringLiteral("root"),
        QJsonArray{binding({unicodeTrigger(0)}, {QStringLiteral("ignore")})});
    exportObject.insert(QStringLiteral("keybindings"), bindings);
    reject(exportObject, QStringLiteral("nonzero Unicode scalar"));

    exportObject = object();
    bindings = keybindings();
    bindings.insert(
        QStringLiteral("root"),
        QJsonArray{binding({unicodeTrigger('a'), unicodeTrigger('b')},
                           {QStringLiteral("ignore")}, flags(true, true))});
    exportObject.insert(QStringLiteral("keybindings"), bindings);
    reject(
        exportObject,
        QStringLiteral("all/global binding must contain exactly one trigger"));

    exportObject = object();
    bindings = keybindings();
    bindings.insert(QStringLiteral("root"),
                    QJsonArray{binding({unicodeTrigger('a', 16)},
                                       {QStringLiteral("ignore")})});
    exportObject.insert(QStringLiteral("keybindings"), bindings);
    reject(exportObject, QStringLiteral("mods must be an unsigned integer"));

    exportObject = object();
    bindings = keybindings();
    bindings.insert(QStringLiteral("root"),
                    QJsonArray{binding({}, {QStringLiteral("ignore")})});
    exportObject.insert(QStringLiteral("keybindings"), bindings);
    reject(exportObject, QStringLiteral("sequence must be a non-empty array"));

    exportObject = object();
    bindings = keybindings();
    bindings.insert(QStringLiteral("root"),
                    QJsonArray{binding({unicodeTrigger('a')}, {})});
    exportObject.insert(QStringLiteral("keybindings"), bindings);
    reject(exportObject, QStringLiteral("actions must be a non-empty array"));
}

QTEST_GUILESS_MAIN(GhosttyConfigExportTest)

#include "test_ghostty_config_export.moc"
