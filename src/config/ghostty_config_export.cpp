#include "config/ghostty_config_export.h"

#include <QColor>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <concepts>
#include <limits>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace {

template <typename Value> using ParseResult = std::expected<Value, QString>;

using Fields = std::span<const QLatin1StringView>;

constexpr auto RootFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("version"),
    QLatin1StringView("values"),
    QLatin1StringView("keybindings"),
    QLatin1StringView("default-keybindings"),
});

constexpr auto ValueFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("term"),
    QLatin1StringView("enquiry-response"),
    QLatin1StringView("command"),
    QLatin1StringView("initial-command"),
    QLatin1StringView("input"),
    QLatin1StringView("key-remap"),
    QLatin1StringView("wait-after-command"),
    QLatin1StringView("abnormal-command-exit-runtime"),
    QLatin1StringView("env"),
    QLatin1StringView("shell-integration"),
    QLatin1StringView("shell-integration-features"),
    QLatin1StringView("linux-cgroup"),
    QLatin1StringView("linux-cgroup-memory-limit"),
    QLatin1StringView("linux-cgroup-processes-limit"),
    QLatin1StringView("linux-cgroup-hard-fail"),
    QLatin1StringView("working-directory"),
    QLatin1StringView("title"),
    QLatin1StringView("class"),
    QLatin1StringView("font-family"),
    QLatin1StringView("font-family-bold"),
    QLatin1StringView("font-family-italic"),
    QLatin1StringView("font-family-bold-italic"),
    QLatin1StringView("font-size"),
    QLatin1StringView("font-style"),
    QLatin1StringView("font-style-bold"),
    QLatin1StringView("font-style-italic"),
    QLatin1StringView("font-style-bold-italic"),
    QLatin1StringView("font-feature"),
    QLatin1StringView("font-variation"),
    QLatin1StringView("font-variation-bold"),
    QLatin1StringView("font-variation-italic"),
    QLatin1StringView("font-variation-bold-italic"),
    QLatin1StringView("font-codepoint-map"),
    QLatin1StringView("font-synthetic-style"),
    QLatin1StringView("font-shaping-break"),
    QLatin1StringView("freetype-load-flags"),
    QLatin1StringView("adjust-cell-width"),
    QLatin1StringView("adjust-cell-height"),
    QLatin1StringView("adjust-font-baseline"),
    QLatin1StringView("adjust-underline-position"),
    QLatin1StringView("adjust-underline-thickness"),
    QLatin1StringView("adjust-strikethrough-position"),
    QLatin1StringView("adjust-strikethrough-thickness"),
    QLatin1StringView("adjust-overline-position"),
    QLatin1StringView("adjust-overline-thickness"),
    QLatin1StringView("adjust-cursor-thickness"),
    QLatin1StringView("adjust-cursor-height"),
    QLatin1StringView("metric-modifier-order"),
    QLatin1StringView("foreground"),
    QLatin1StringView("background"),
    QLatin1StringView("alpha-blending"),
    QLatin1StringView("background-opacity"),
    QLatin1StringView("background-opacity-cells"),
    QLatin1StringView("background-blur"),
    QLatin1StringView("background-image"),
    QLatin1StringView("background-image-opacity"),
    QLatin1StringView("background-image-position"),
    QLatin1StringView("background-image-fit"),
    QLatin1StringView("background-image-repeat"),
    QLatin1StringView("custom-shader"),
    QLatin1StringView("custom-shader-animation"),
    QLatin1StringView("unfocused-split-opacity"),
    QLatin1StringView("unfocused-split-fill"),
    QLatin1StringView("split-divider-color"),
    QLatin1StringView("split-inherit-working-directory"),
    QLatin1StringView("split-preserve-zoom"),
    QLatin1StringView("tab-inherit-working-directory"),
    QLatin1StringView("window-inherit-working-directory"),
    QLatin1StringView("window-inherit-font-size"),
    QLatin1StringView("window-new-tab-position"),
    QLatin1StringView("window-show-tab-bar"),
    QLatin1StringView("drag-handle"),
    QLatin1StringView("window-decoration"),
    QLatin1StringView("window-theme"),
    QLatin1StringView("window-title-font-family"),
    QLatin1StringView("window-titlebar-background"),
    QLatin1StringView("window-titlebar-foreground"),
    QLatin1StringView("window-subtitle"),
    QLatin1StringView("window-width"),
    QLatin1StringView("window-height"),
    QLatin1StringView("window-padding-x"),
    QLatin1StringView("window-padding-y"),
    QLatin1StringView("window-padding-balance"),
    QLatin1StringView("window-padding-color"),
    QLatin1StringView("maximize"),
    QLatin1StringView("fullscreen"),
    QLatin1StringView("palette"),
    QLatin1StringView("selection-foreground"),
    QLatin1StringView("selection-background"),
    QLatin1StringView("search-foreground"),
    QLatin1StringView("search-background"),
    QLatin1StringView("search-selected-foreground"),
    QLatin1StringView("search-selected-background"),
    QLatin1StringView("cursor-color"),
    QLatin1StringView("cursor-opacity"),
    QLatin1StringView("cursor-style"),
    QLatin1StringView("cursor-style-blink"),
    QLatin1StringView("cursor-text"),
    QLatin1StringView("bold-color"),
    QLatin1StringView("faint-opacity"),
    QLatin1StringView("minimum-contrast"),
    QLatin1StringView("vt-kam-allowed"),
    QLatin1StringView("grapheme-width-method"),
    QLatin1StringView("title-report"),
    QLatin1StringView("scrollback-limit-bytes"),
    QLatin1StringView("scrollback-limit-lines"),
    QLatin1StringView("image-storage-limit"),
    QLatin1StringView("scrollback-compression"),
    QLatin1StringView("scrollbar"),
    QLatin1StringView("desktop-notifications"),
    QLatin1StringView("progress-style"),
    QLatin1StringView("bell-features"),
    QLatin1StringView("bell-audio-path"),
    QLatin1StringView("bell-audio-volume"),
    QLatin1StringView("confirm-close-surface"),
    QLatin1StringView("clipboard-trim-trailing-spaces"),
    QLatin1StringView("clipboard-codepoint-map"),
    QLatin1StringView("clipboard-read"),
    QLatin1StringView("clipboard-write"),
    QLatin1StringView("clipboard-write-limit-bytes"),
    QLatin1StringView("clipboard-paste-protection"),
    QLatin1StringView("clipboard-paste-bracketed-safe"),
    QLatin1StringView("copy-on-select"),
    QLatin1StringView("selection-clear-on-typing"),
    QLatin1StringView("selection-clear-on-copy"),
    QLatin1StringView("selection-word-chars"),
    QLatin1StringView("click-repeat-interval"),
    QLatin1StringView("right-click-action"),
    QLatin1StringView("middle-click-action"),
    QLatin1StringView("mouse-reporting"),
    QLatin1StringView("mouse-shift-capture"),
    QLatin1StringView("mouse-hide-while-typing"),
    QLatin1StringView("scroll-to-bottom"),
    QLatin1StringView("focus-follows-mouse"),
    QLatin1StringView("mouse-scroll-multiplier"),
    QLatin1StringView("link-url"),
    QLatin1StringView("link-osc8"),
    QLatin1StringView("link-previews"),
    QLatin1StringView("config-file"),
    QLatin1StringView("config-default-files"),
    QLatin1StringView("theme-files"),
    QLatin1StringView("quit-after-last-window-closed"),
    QLatin1StringView("quit-after-last-window-closed-delay"),
    QLatin1StringView("initial-window"),
    QLatin1StringView("resize-overlay"),
    QLatin1StringView("resize-overlay-position"),
    QLatin1StringView("resize-overlay-duration"),
    QLatin1StringView("gtk-single-instance"),
    QLatin1StringView("quick-terminal-position"),
    QLatin1StringView("quick-terminal-size"),
    QLatin1StringView("quick-terminal-screen"),
    QLatin1StringView("quick-terminal-autohide"),
    QLatin1StringView("quick-terminal-keyboard-interactivity"),
    QLatin1StringView("command-palette-entry"),
    QLatin1StringView("app-notifications"),
});

constexpr auto KeybindingFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("root"),
    QLatin1StringView("tables"),
});
constexpr auto TableFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("name"),
    QLatin1StringView("bindings"),
});
constexpr auto DefinitionFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("sequence"),
    QLatin1StringView("actions"),
    QLatin1StringView("flags"),
});
constexpr auto FlagFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("consumed"),
    QLatin1StringView("all"),
    QLatin1StringView("global"),
    QLatin1StringView("performable"),
});
constexpr auto PhysicalTriggerFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("kind"),
    QLatin1StringView("key"),
    QLatin1StringView("mods"),
});
constexpr auto UnicodeTriggerFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("kind"),
    QLatin1StringView("codepoint"),
    QLatin1StringView("mods"),
});
constexpr auto CatchAllTriggerFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("kind"),
    QLatin1StringView("mods"),
});
constexpr auto FontStyleFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("kind"),
});
constexpr auto NamedFontStyleFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("kind"),
    QLatin1StringView("name"),
});
constexpr auto OpenTypeFeatureFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("tag"),
    QLatin1StringView("value"),
});
constexpr auto FontVariationFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("tag"),
    QLatin1StringView("value-bits"),
});
constexpr auto CodepointFontMapFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("first"),
    QLatin1StringView("last"),
    QLatin1StringView("family"),
});
constexpr auto ClipboardCodepointMapFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("first"),
    QLatin1StringView("last"),
    QLatin1StringView("replacement"),
});
constexpr auto ClipboardCodepointReplacementFields =
    std::to_array<QLatin1StringView>({
        QLatin1StringView("kind"),
        QLatin1StringView("value"),
    });
constexpr auto SyntheticStyleFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("bold"),
    QLatin1StringView("italic"),
    QLatin1StringView("bold-italic"),
});
constexpr auto ShapingBreakFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("cursor"),
});
constexpr auto FreetypeLoadFlagFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("hinting"),
    QLatin1StringView("force-autohint"),
    QLatin1StringView("monochrome"),
    QLatin1StringView("autohint"),
    QLatin1StringView("light"),
});
constexpr auto MetricModifierFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("kind"),
    QLatin1StringView("value"),
});
constexpr auto BellFeatureFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("system"),
    QLatin1StringView("audio"),
    QLatin1StringView("attention"),
    QLatin1StringView("title"),
    QLatin1StringView("border"),
});
constexpr auto ScrollToBottomFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("keystroke"),
    QLatin1StringView("output"),
});
constexpr auto ConfigPathFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("path"),
    QLatin1StringView("optional"),
});
constexpr auto EnvironmentEntryFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("key"),
    QLatin1StringView("value"),
});
constexpr auto ShellIntegrationFeatureFields =
    std::to_array<QLatin1StringView>({
        QLatin1StringView("cursor"),
        QLatin1StringView("sudo"),
        QLatin1StringView("title"),
        QLatin1StringView("ssh-env"),
        QLatin1StringView("ssh-terminfo"),
        QLatin1StringView("path"),
    });
constexpr auto ShellCommandFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("kind"),
    QLatin1StringView("value"),
    QLatin1StringView("default-shell"),
});
constexpr auto DirectCommandFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("kind"),
    QLatin1StringView("argv"),
    QLatin1StringView("default-shell"),
});
constexpr auto MouseScrollMultiplierFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("precision"),
    QLatin1StringView("discrete"),
});
constexpr auto InitialInputFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("kind"),
    QLatin1StringView("value"),
});
constexpr auto ModifierRemapFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("from"),
    QLatin1StringView("to"),
});
constexpr auto SidedModifierFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("modifier"),
    QLatin1StringView("side"),
});
constexpr auto QuickTerminalSizeFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("primary"),
    QLatin1StringView("secondary"),
});
constexpr auto QuickTerminalPixelsFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("kind"),
    QLatin1StringView("value"),
});
constexpr auto QuickTerminalPercentageFields =
    std::to_array<QLatin1StringView>({
        QLatin1StringView("kind"),
        QLatin1StringView("value-bits"),
    });
constexpr auto CommandPaletteEntryFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("title"),
    QLatin1StringView("description"),
    QLatin1StringView("action-key"),
    QLatin1StringView("action"),
});
constexpr auto AppNotificationFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("clipboard-copy"),
    QLatin1StringView("config-reload"),
});

constexpr auto FontFamilyFields =
    std::to_array<std::pair<QLatin1StringView, TerminalFontRole>>({
        {QLatin1StringView("font-family"), TerminalFontRole::Regular},
        {QLatin1StringView("font-family-bold"), TerminalFontRole::Bold},
        {QLatin1StringView("font-family-italic"), TerminalFontRole::Italic},
        {QLatin1StringView("font-family-bold-italic"),
         TerminalFontRole::BoldItalic},
    });
constexpr auto FontStyleValueFields =
    std::to_array<std::pair<QLatin1StringView, TerminalFontRole>>({
        {QLatin1StringView("font-style"), TerminalFontRole::Regular},
        {QLatin1StringView("font-style-bold"), TerminalFontRole::Bold},
        {QLatin1StringView("font-style-italic"), TerminalFontRole::Italic},
        {QLatin1StringView("font-style-bold-italic"),
         TerminalFontRole::BoldItalic},
    });
constexpr auto FontVariationValueFields =
    std::to_array<std::pair<QLatin1StringView, TerminalFontRole>>({
        {QLatin1StringView("font-variation"), TerminalFontRole::Regular},
        {QLatin1StringView("font-variation-bold"), TerminalFontRole::Bold},
        {QLatin1StringView("font-variation-italic"), TerminalFontRole::Italic},
        {QLatin1StringView("font-variation-bold-italic"),
         TerminalFontRole::BoldItalic},
    });
constexpr auto MetricModifierValueFields =
    std::to_array<std::pair<QLatin1StringView, TerminalMetric>>({
        {QLatin1StringView("adjust-cell-width"), TerminalMetric::CellWidth},
        {QLatin1StringView("adjust-cell-height"), TerminalMetric::CellHeight},
        {QLatin1StringView("adjust-font-baseline"),
         TerminalMetric::FontBaseline},
        {QLatin1StringView("adjust-underline-position"),
         TerminalMetric::UnderlinePosition},
        {QLatin1StringView("adjust-underline-thickness"),
         TerminalMetric::UnderlineThickness},
        {QLatin1StringView("adjust-strikethrough-position"),
         TerminalMetric::StrikethroughPosition},
        {QLatin1StringView("adjust-strikethrough-thickness"),
         TerminalMetric::StrikethroughThickness},
        {QLatin1StringView("adjust-overline-position"),
         TerminalMetric::OverlinePosition},
        {QLatin1StringView("adjust-overline-thickness"),
         TerminalMetric::OverlineThickness},
        {QLatin1StringView("adjust-cursor-thickness"),
         TerminalMetric::CursorThickness},
        {QLatin1StringView("adjust-cursor-height"),
         TerminalMetric::CursorHeight},
    });

QString childContext(const QString &parent, QLatin1StringView child)
{
    return parent + u'.' + child;
}

ParseResult<QJsonObject> readExactObject(const QJsonValue &value,
                                         const QString &context,
                                         Fields expected)
{
    if (!value.isObject()) {
        return std::unexpected(
            QStringLiteral("%1 must be an object").arg(context));
    }

    const QJsonObject object = value.toObject();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const bool recognized =
            std::ranges::any_of(expected, [&it](QLatin1StringView field) {
                return it.key() == field;
            });
        if (!recognized) {
            return std::unexpected(
                QStringLiteral("%1 has unexpected field '%2'")
                    .arg(context, it.key()));
        }
    }
    for (const QLatin1StringView field : expected) {
        if (!object.contains(field)) {
            return std::unexpected(
                QStringLiteral("%1 is missing field '%2'").arg(context, field));
        }
    }
    return object;
}

ParseResult<QJsonArray> readArray(const QJsonValue &value,
                                  const QString &context)
{
    if (!value.isArray()) {
        return std::unexpected(
            QStringLiteral("%1 must be an array").arg(context));
    }
    return value.toArray();
}

ParseResult<bool> readBoolean(const QJsonValue &value, const QString &context)
{
    if (!value.isBool()) {
        return std::unexpected(
            QStringLiteral("%1 must be a boolean").arg(context));
    }
    return value.toBool();
}

template <typename Value, std::size_t Size>
ParseResult<Value> readBooleanObject(
    const QJsonValue &value, const QString &context, Fields expected,
    const std::array<std::pair<QLatin1StringView, bool Value::*>, Size> &fields)
{
    auto object = readExactObject(value, context, expected);
    if (!object) return std::unexpected(std::move(object.error()));

    Value result;
    for (const auto &[name, member] : fields) {
        auto parsed =
            readBoolean(object->value(name), childContext(context, name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.*member = *parsed;
    }
    return result;
}

ParseResult<BellFeatures> readBellFeatures(const QJsonValue &value,
                                           const QString &context)
{
    return readBooleanObject<BellFeatures>(
        value, context, BellFeatureFields,
        std::to_array<std::pair<QLatin1StringView, bool BellFeatures::*>>({
            {QLatin1StringView("system"), &BellFeatures::system},
            {QLatin1StringView("audio"), &BellFeatures::audio},
            {QLatin1StringView("attention"), &BellFeatures::attention},
            {QLatin1StringView("title"), &BellFeatures::title},
            {QLatin1StringView("border"), &BellFeatures::border},
        }));
}

ParseResult<GhosttyShellIntegrationFeatures>
readShellIntegrationFeatures(const QJsonValue &value, const QString &context)
{
    return readBooleanObject<GhosttyShellIntegrationFeatures>(
        value, context, ShellIntegrationFeatureFields,
        std::to_array<std::pair<QLatin1StringView,
                                bool GhosttyShellIntegrationFeatures::*>>({
            {QLatin1StringView("cursor"),
             &GhosttyShellIntegrationFeatures::cursor},
            {QLatin1StringView("sudo"), &GhosttyShellIntegrationFeatures::sudo},
            {QLatin1StringView("title"),
             &GhosttyShellIntegrationFeatures::title},
            {QLatin1StringView("ssh-env"),
             &GhosttyShellIntegrationFeatures::sshEnvironment},
            {QLatin1StringView("ssh-terminfo"),
             &GhosttyShellIntegrationFeatures::sshTerminfo},
            {QLatin1StringView("path"), &GhosttyShellIntegrationFeatures::path},
        }));
}

ParseResult<TerminalScrollToBottomOptions>
readScrollToBottom(const QJsonValue &value, const QString &context)
{
    return readBooleanObject<TerminalScrollToBottomOptions>(
        value, context, ScrollToBottomFields,
        std::to_array<std::pair<QLatin1StringView,
                                bool TerminalScrollToBottomOptions::*>>({
            {QLatin1StringView("keystroke"),
             &TerminalScrollToBottomOptions::keystroke},
            {QLatin1StringView("output"),
             &TerminalScrollToBottomOptions::output},
        }));
}

ParseResult<QString> readString(const QJsonValue &value, const QString &context)
{
    if (!value.isString()) {
        return std::unexpected(
            QStringLiteral("%1 must be a string").arg(context));
    }
    return value.toString();
}

ParseResult<QString> readNonEmptyString(const QJsonValue &value,
                                        const QString &context)
{
    auto result = readString(value, context);
    if (result && result->isEmpty()) {
        return std::unexpected(
            QStringLiteral("%1 must be a non-empty string").arg(context));
    }
    return result;
}

ParseResult<std::optional<QString>> readOptionalString(const QJsonValue &value,
                                                       const QString &context,
                                                       bool allowEmpty)
{
    if (value.isNull()) return std::nullopt;
    auto result = allowEmpty ? readString(value, context)
                             : readNonEmptyString(value, context);
    if (!result) return std::unexpected(std::move(result.error()));
    return std::move(*result);
}

ParseResult<GhosttyConfigPath> readConfigPath(const QJsonValue &value,
                                              const QString &context)
{
    auto object = readExactObject(value, context, ConfigPathFields);
    if (!object) return std::unexpected(std::move(object.error()));
    auto path =
        readNonEmptyString(object->value(QLatin1StringView("path")),
                           childContext(context, QLatin1StringView("path")));
    if (!path) return std::unexpected(std::move(path.error()));
    if (!QDir::isAbsolutePath(*path)) {
        return std::unexpected(
            QStringLiteral("%1.path must be a finalized absolute path")
                .arg(context));
    }
    auto optional =
        readBoolean(object->value(QLatin1StringView("optional")),
                    childContext(context, QLatin1StringView("optional")));
    if (!optional) return std::unexpected(std::move(optional.error()));

    return GhosttyConfigPath{
        .path = std::move(*path),
        .optional = *optional,
    };
}

ParseResult<std::optional<GhosttyConfigPath>>
readOptionalConfigPath(const QJsonValue &value, const QString &context)
{
    if (value.isNull()) return std::nullopt;
    auto parsed = readConfigPath(value, context);
    if (!parsed) return std::unexpected(std::move(parsed.error()));
    return std::move(*parsed);
}

ParseResult<QVector<GhosttyConfigPath>> readConfigPaths(const QJsonValue &value,
                                                        const QString &context)
{
    auto array = readArray(value, context);
    if (!array) return std::unexpected(std::move(array.error()));

    QVector<GhosttyConfigPath> result;
    result.reserve(array->size());
    for (qsizetype index = 0; index < array->size(); ++index) {
        const QString entryContext =
            QStringLiteral("%1[%2]").arg(context).arg(index);
        auto path = readConfigPath(array->at(index), entryContext);
        if (!path) return std::unexpected(std::move(path.error()));
        result.append(std::move(*path));
    }
    return result;
}

ParseResult<double>
readFiniteDouble(const QJsonValue &value, const QString &context,
                 double minimum = -std::numeric_limits<double>::infinity(),
                 double maximum = std::numeric_limits<double>::infinity())
{
    if (!value.isDouble()) {
        return std::unexpected(
            QStringLiteral("%1 must be a finite number").arg(context));
    }
    const double result = value.toDouble();
    if (!std::isfinite(result) || result < minimum || result > maximum) {
        return std::unexpected(
            QStringLiteral("%1 is outside its supported range").arg(context));
    }
    return result;
}

ParseResult<MouseScrollMultiplier>
readMouseScrollMultiplier(const QJsonValue &value, const QString &context)
{
    auto object = readExactObject(value, context, MouseScrollMultiplierFields);
    if (!object) return std::unexpected(std::move(object.error()));

    constexpr double Minimum = 0.01;
    constexpr double Maximum = 10'000.0;
    MouseScrollMultiplier result;
    for (const auto [name, destination] :
         std::to_array<std::pair<QLatin1StringView, double *>>({
             {QLatin1StringView("precision"), &result.precision},
             {QLatin1StringView("discrete"), &result.discrete},
         })) {
        auto parsed = readFiniteDouble(
            object->value(name), childContext(context, name), Minimum, Maximum);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        *destination = *parsed;
    }
    return result;
}

ParseResult<qint16> readBackgroundBlur(const QJsonValue &value,
                                       const QString &context)
{
    if (!value.isDouble()) {
        return std::unexpected(
            QStringLiteral("%1 must be an integer").arg(context));
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::trunc(number) != number) {
        return std::unexpected(
            QStringLiteral("%1 must be an integer").arg(context));
    }
    if (number < -2.0 || number > 255.0) {
        return std::unexpected(
            QStringLiteral(
                "%1 must be -2, -1, or an integer from 0 through 255")
                .arg(context));
    }
    return static_cast<qint16>(number);
}

template <std::unsigned_integral Integer>
ParseResult<Integer>
readUnsignedInteger(const QJsonValue &value, const QString &context,
                    Integer maximum = std::numeric_limits<Integer>::max())
{
    if (!value.isDouble()) {
        return std::unexpected(
            QStringLiteral("%1 must be an unsigned integer").arg(context));
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0.0 || std::trunc(number) != number
        || number > static_cast<double>(maximum)) {
        return std::unexpected(
            QStringLiteral("%1 must be an unsigned integer in range")
                .arg(context));
    }
    return static_cast<Integer>(number);
}

ParseResult<TerminalPaddingAxis>
readTerminalPaddingAxis(const QJsonValue &value, const QString &context)
{
    auto array = readArray(value, context);
    if (!array) return std::unexpected(std::move(array.error()));
    if (array->size() != 2) {
        return std::unexpected(
            QStringLiteral("%1 must contain exactly two point values")
                .arg(context));
    }

    auto leading = readUnsignedInteger<quint32>(
        array->at(0), QStringLiteral("%1[0]").arg(context));
    if (!leading) return std::unexpected(std::move(leading.error()));
    auto trailing = readUnsignedInteger<quint32>(
        array->at(1), QStringLiteral("%1[1]").arg(context));
    if (!trailing) return std::unexpected(std::move(trailing.error()));
    return TerminalPaddingAxis{
        .leadingPoints = *leading,
        .trailingPoints = *trailing,
    };
}

ParseResult<QByteArray> readByteArray(const QJsonValue &value,
                                      const QString &context)
{
    auto array = readArray(value, context);
    if (!array) return std::unexpected(std::move(array.error()));

    QByteArray result;
    result.reserve(array->size());
    for (qsizetype index = 0; index < array->size(); ++index) {
        const QString entryContext =
            QStringLiteral("%1[%2]").arg(context).arg(index);
        auto byte = readUnsignedInteger<quint8>(array->at(index), entryContext);
        if (!byte) return std::unexpected(std::move(byte.error()));
        result.append(static_cast<char>(*byte));
    }
    return result;
}

ParseResult<QByteArray> readNonEmptyByteArray(const QJsonValue &value,
                                              const QString &context)
{
    auto result = readByteArray(value, context);
    if (result && result->isEmpty()) {
        return std::unexpected(
            QStringLiteral("%1 must be a non-empty byte array").arg(context));
    }
    return result;
}

ParseResult<QByteArray> readCommandBytes(const QJsonValue &value,
                                         const QString &context)
{
    auto result = readByteArray(value, context);
    if (result && result->contains('\0')) {
        return std::unexpected(
            QStringLiteral("%1 must not contain NUL").arg(context));
    }
    return result;
}

ParseResult<std::optional<TerminalCommand>>
readOptionalCommand(const QJsonValue &value, const QString &context,
                    bool allowDefaultShell)
{
    if (value.isNull()) return std::optional<TerminalCommand>{};
    if (!value.isObject()) {
        return std::unexpected(
            QStringLiteral("%1 must be an object or null").arg(context));
    }

    const QJsonObject candidate = value.toObject();
    auto kind = readString(candidate.value(QLatin1StringView("kind")),
                           childContext(context, QLatin1StringView("kind")));
    if (!kind) return std::unexpected(std::move(kind.error()));

    const Fields fields = *kind == QLatin1StringView("shell")
        ? Fields(ShellCommandFields)
        : *kind == QLatin1StringView("direct") ? Fields(DirectCommandFields)
                                               : Fields{};
    if (fields.empty()) {
        return std::unexpected(
            QStringLiteral("%1.kind has unsupported value '%2'")
                .arg(context, *kind));
    }
    auto object = readExactObject(value, context, fields);
    if (!object) return std::unexpected(std::move(object.error()));

    auto defaultShell =
        readBoolean(object->value(QLatin1StringView("default-shell")),
                    childContext(context, QLatin1StringView("default-shell")));
    if (!defaultShell) {
        return std::unexpected(std::move(defaultShell.error()));
    }
    if (*defaultShell
        && (!allowDefaultShell || *kind != QLatin1StringView("shell"))) {
        return std::unexpected(
            QStringLiteral("%1.default-shell is invalid for this command")
                .arg(context));
    }

    if (*kind == QLatin1StringView("shell")) {
        auto shell =
            readCommandBytes(object->value(QLatin1StringView("value")),
                             childContext(context, QLatin1StringView("value")));
        if (!shell) return std::unexpected(std::move(shell.error()));
        return TerminalCommand::shell(std::move(*shell), *defaultShell);
    }

    auto argv = readArray(object->value(QLatin1StringView("argv")),
                          childContext(context, QLatin1StringView("argv")));
    if (!argv) return std::unexpected(std::move(argv.error()));
    if (argv->isEmpty()) {
        return std::unexpected(
            QStringLiteral("%1.argv must contain at least argv[0]")
                .arg(context));
    }
    QVector<QByteArray> arguments;
    arguments.reserve(argv->size());
    for (qsizetype index = 0; index < argv->size(); ++index) {
        const QString argumentContext =
            QStringLiteral("%1.argv[%2]").arg(context).arg(index);
        auto argument = readCommandBytes(argv->at(index), argumentContext);
        if (!argument) return std::unexpected(std::move(argument.error()));
        arguments.push_back(std::move(*argument));
    }
    return TerminalCommand::direct(std::move(arguments));
}

ParseResult<TerminalEnvironment> readEnvironment(const QJsonValue &value,
                                                 const QString &context)
{
    auto array = readArray(value, context);
    if (!array) return std::unexpected(std::move(array.error()));

    TerminalEnvironment result;
    result.reserve(array->size());
    for (qsizetype index = 0; index < array->size(); ++index) {
        const QString entryContext =
            QStringLiteral("%1[%2]").arg(context).arg(index);
        auto object = readExactObject(array->at(index), entryContext,
                                      EnvironmentEntryFields);
        if (!object) return std::unexpected(std::move(object.error()));

        auto key =
            readByteArray(object->value(QLatin1StringView("key")),
                          childContext(entryContext, QLatin1StringView("key")));
        if (!key) return std::unexpected(std::move(key.error()));
        if (key->contains('=')) {
            return std::unexpected(QStringLiteral("%1.key must not contain '='")
                                       .arg(entryContext));
        }
        if (key->contains('\0')) {
            return std::unexpected(QStringLiteral("%1.key must not contain NUL")
                                       .arg(entryContext));
        }
        auto entryValue = readNonEmptyByteArray(
            object->value(QLatin1StringView("value")),
            childContext(entryContext, QLatin1StringView("value")));
        if (!entryValue) {
            return std::unexpected(std::move(entryValue.error()));
        }
        if (entryValue->contains('\0')) {
            return std::unexpected(
                QStringLiteral("%1.value must not contain NUL")
                    .arg(entryContext));
        }
        if (std::ranges::any_of(result, [&key](const auto &entry) {
                return entry.key == *key;
            })) {
            return std::unexpected(
                QStringLiteral("%1 contains duplicate key").arg(entryContext));
        }
        result.push_back({
            .key = std::move(*key),
            .value = std::move(*entryValue),
        });
    }
    return result;
}

ParseResult<QVector<quint32>> readUnicodeScalarList(const QJsonValue &value,
                                                    const QString &context)
{
    auto array = readArray(value, context);
    if (!array) return std::unexpected(std::move(array.error()));

    QVector<quint32> result;
    result.reserve(array->size());
    for (qsizetype index = 0; index < array->size(); ++index) {
        const QString entryContext =
            QStringLiteral("%1[%2]").arg(context).arg(index);
        auto codepoint = readUnsignedInteger<quint32>(array->at(index),
                                                      entryContext, 0x10ffffU);
        if (!codepoint || (*codepoint >= 0xd800U && *codepoint <= 0xdfffU)) {
            return std::unexpected(
                QStringLiteral("%1 must be a Unicode scalar value")
                    .arg(entryContext));
        }
        result.append(*codepoint);
    }
    if (result.isEmpty() || result.front() != 0U) {
        return std::unexpected(
            QStringLiteral("%1 must begin with U+0000").arg(context));
    }
    return result;
}

ParseResult<quint32> readFinalizedClickRepeatInterval(const QJsonValue &value,
                                                      const QString &context)
{
    auto result = readUnsignedInteger<quint32>(value, context);
    if (!result) return std::unexpected(std::move(result.error()));
    if (*result == 0) {
        return std::unexpected(
            QStringLiteral("%1 must be a nonzero unsigned integer")
                .arg(context));
    }
    return result;
}

template <std::signed_integral Integer>
ParseResult<Integer> readSignedInteger(const QJsonValue &value,
                                       const QString &context)
{
    if (!value.isDouble()) {
        return std::unexpected(
            QStringLiteral("%1 must be a signed integer").arg(context));
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::trunc(number) != number
        || number < static_cast<double>(std::numeric_limits<Integer>::min())
        || number > static_cast<double>(std::numeric_limits<Integer>::max())) {
        return std::unexpected(
            QStringLiteral("%1 must be a signed integer in range")
                .arg(context));
    }
    return static_cast<Integer>(number);
}

ParseResult<quint64> readDecimalUint64(const QJsonValue &value,
                                       const QString &context)
{
    auto text = readString(value, context);
    if (!text) return std::unexpected(std::move(text.error()));
    if (text->isEmpty() || (text->size() > 1 && text->startsWith(u'0'))
        || !std::ranges::all_of(*text, [](QChar character) {
               return character >= u'0' && character <= u'9';
           })) {
        return std::unexpected(
            QStringLiteral("%1 must be a canonical unsigned decimal string")
                .arg(context));
    }
    bool valid = false;
    const quint64 result = text->toULongLong(&valid, 10);
    if (!valid) {
        return std::unexpected(
            QStringLiteral("%1 exceeds the uint64 range").arg(context));
    }
    return result;
}

ParseResult<std::optional<quint64>>
readOptionalDecimalUint64(const QJsonValue &value, const QString &context)
{
    if (value.isNull()) return std::optional<quint64>{};
    auto parsed = readDecimalUint64(value, context);
    if (!parsed) return std::unexpected(std::move(parsed.error()));
    return std::optional<quint64>{*parsed};
}

ParseResult<QVector<TerminalFontFeature>>
readFontFeatures(const QJsonValue &value, const QString &context)
{
    auto array = readArray(value, context);
    if (!array) return std::unexpected(std::move(array.error()));

    QVector<TerminalFontFeature> result;
    result.reserve(array->size());
    for (qsizetype index = 0; index < array->size(); ++index) {
        const QString entryContext =
            QStringLiteral("%1[%2]").arg(context).arg(index);
        auto object = readExactObject(array->at(index), entryContext,
                                      OpenTypeFeatureFields);
        if (!object) return std::unexpected(std::move(object.error()));
        auto tag = readUnsignedInteger<quint32>(
            object->value(QLatin1StringView("tag")),
            childContext(entryContext, QLatin1StringView("tag")));
        if (!tag) return std::unexpected(std::move(tag.error()));
        auto featureValue = readUnsignedInteger<quint32>(
            object->value(QLatin1StringView("value")),
            childContext(entryContext, QLatin1StringView("value")));
        if (!featureValue) {
            return std::unexpected(std::move(featureValue.error()));
        }
        result.append({
            .tag = *tag,
            .value = *featureValue,
        });
    }
    return result;
}

ParseResult<QVector<TerminalFontVariation>>
readFontVariations(const QJsonValue &value, const QString &context)
{
    auto array = readArray(value, context);
    if (!array) return std::unexpected(std::move(array.error()));

    QVector<TerminalFontVariation> result;
    result.reserve(array->size());
    for (qsizetype index = 0; index < array->size(); ++index) {
        const QString entryContext =
            QStringLiteral("%1[%2]").arg(context).arg(index);
        auto object = readExactObject(array->at(index), entryContext,
                                      FontVariationFields);
        if (!object) return std::unexpected(std::move(object.error()));
        auto tag = readUnsignedInteger<quint32>(
            object->value(QLatin1StringView("tag")),
            childContext(entryContext, QLatin1StringView("tag")));
        if (!tag) return std::unexpected(std::move(tag.error()));
        auto valueBits = readDecimalUint64(
            object->value(QLatin1StringView("value-bits")),
            childContext(entryContext, QLatin1StringView("value-bits")));
        if (!valueBits) {
            return std::unexpected(std::move(valueBits.error()));
        }
        result.append({
            .tag = *tag,
            .valueBits = *valueBits,
        });
    }
    return result;
}

ParseResult<QVector<TerminalCodepointFontMap>>
readCodepointFontMap(const QJsonValue &value, const QString &context)
{
    auto array = readArray(value, context);
    if (!array) return std::unexpected(std::move(array.error()));

    constexpr quint32 MaximumU21 = 0x1fffffU;
    QVector<TerminalCodepointFontMap> result;
    result.reserve(array->size());
    for (qsizetype index = 0; index < array->size(); ++index) {
        const QString entryContext =
            QStringLiteral("%1[%2]").arg(context).arg(index);
        auto object = readExactObject(array->at(index), entryContext,
                                      CodepointFontMapFields);
        if (!object) return std::unexpected(std::move(object.error()));
        auto first = readUnsignedInteger<quint32>(
            object->value(QLatin1StringView("first")),
            childContext(entryContext, QLatin1StringView("first")), MaximumU21);
        if (!first) return std::unexpected(std::move(first.error()));
        auto last = readUnsignedInteger<quint32>(
            object->value(QLatin1StringView("last")),
            childContext(entryContext, QLatin1StringView("last")), MaximumU21);
        if (!last) return std::unexpected(std::move(last.error()));
        if (*first > *last) {
            return std::unexpected(
                QStringLiteral("%1.first must not exceed last")
                    .arg(entryContext));
        }
        auto family =
            readString(object->value(QLatin1StringView("family")),
                       childContext(entryContext, QLatin1StringView("family")));
        if (!family) return std::unexpected(std::move(family.error()));
        result.append({
            .first = *first,
            .last = *last,
            .family = std::move(*family),
        });
    }
    return result;
}

ParseResult<TerminalClipboardCodepointReplacement>
readClipboardCodepointReplacement(const QJsonValue &value,
                                  const QString &context)
{
    auto object =
        readExactObject(value, context, ClipboardCodepointReplacementFields);
    if (!object) return std::unexpected(std::move(object.error()));

    auto kind = readString(object->value(QLatin1StringView("kind")),
                           childContext(context, QLatin1StringView("kind")));
    if (!kind) return std::unexpected(std::move(kind.error()));
    if (*kind == QLatin1StringView("codepoint")) {
        constexpr quint32 MaximumU21 = 0x1fffffU;
        auto codepoint = readUnsignedInteger<quint32>(
            object->value(QLatin1StringView("value")),
            childContext(context, QLatin1StringView("value")), MaximumU21);
        if (!codepoint) {
            return std::unexpected(std::move(codepoint.error()));
        }
        return TerminalClipboardCodepointReplacement{*codepoint};
    }
    if (*kind == QLatin1StringView("text")) {
        auto text =
            readString(object->value(QLatin1StringView("value")),
                       childContext(context, QLatin1StringView("value")));
        if (!text) return std::unexpected(std::move(text.error()));
        return TerminalClipboardCodepointReplacement{std::move(*text)};
    }
    return std::unexpected(QStringLiteral("%1.kind has unsupported value '%2'")
                               .arg(context, *kind));
}

ParseResult<TerminalClipboardCodepointMap>
readClipboardCodepointMap(const QJsonValue &value, const QString &context)
{
    auto array = readArray(value, context);
    if (!array) return std::unexpected(std::move(array.error()));

    constexpr quint32 MaximumU21 = 0x1fffffU;
    TerminalClipboardCodepointMap result;
    result.reserve(array->size());
    for (qsizetype index = 0; index < array->size(); ++index) {
        const QString entryContext =
            QStringLiteral("%1[%2]").arg(context).arg(index);
        auto object = readExactObject(array->at(index), entryContext,
                                      ClipboardCodepointMapFields);
        if (!object) return std::unexpected(std::move(object.error()));
        auto first = readUnsignedInteger<quint32>(
            object->value(QLatin1StringView("first")),
            childContext(entryContext, QLatin1StringView("first")), MaximumU21);
        if (!first) return std::unexpected(std::move(first.error()));
        auto last = readUnsignedInteger<quint32>(
            object->value(QLatin1StringView("last")),
            childContext(entryContext, QLatin1StringView("last")), MaximumU21);
        if (!last) return std::unexpected(std::move(last.error()));
        if (*first > *last) {
            return std::unexpected(
                QStringLiteral("%1.first must not exceed last")
                    .arg(entryContext));
        }
        auto replacement = readClipboardCodepointReplacement(
            object->value(QLatin1StringView("replacement")),
            childContext(entryContext, QLatin1StringView("replacement")));
        if (!replacement) {
            return std::unexpected(std::move(replacement.error()));
        }
        result.append({
            .first = *first,
            .last = *last,
            .replacement = std::move(*replacement),
        });
    }
    return result;
}

ParseResult<TerminalSyntheticStyle> readSyntheticStyle(const QJsonValue &value,
                                                       const QString &context)
{
    return readBooleanObject<TerminalSyntheticStyle>(
        value, context, SyntheticStyleFields,
        std::to_array<
            std::pair<QLatin1StringView, bool TerminalSyntheticStyle::*>>({
            {QLatin1StringView("bold"), &TerminalSyntheticStyle::bold},
            {QLatin1StringView("italic"), &TerminalSyntheticStyle::italic},
            {QLatin1StringView("bold-italic"),
             &TerminalSyntheticStyle::boldItalic},
        }));
}

ParseResult<TerminalFreetypeLoadFlags>
readFreetypeLoadFlags(const QJsonValue &value, const QString &context)
{
    return readBooleanObject<TerminalFreetypeLoadFlags>(
        value, context, FreetypeLoadFlagFields,
        std::to_array<
            std::pair<QLatin1StringView, bool TerminalFreetypeLoadFlags::*>>({
            {QLatin1StringView("hinting"), &TerminalFreetypeLoadFlags::hinting},
            {QLatin1StringView("force-autohint"),
             &TerminalFreetypeLoadFlags::forceAutohint},
            {QLatin1StringView("monochrome"),
             &TerminalFreetypeLoadFlags::monochrome},
            {QLatin1StringView("autohint"),
             &TerminalFreetypeLoadFlags::autohint},
            {QLatin1StringView("light"), &TerminalFreetypeLoadFlags::light},
        }));
}

int hexDigit(QChar value)
{
    if (value >= u'0' && value <= u'9') return value.unicode() - u'0';
    if (value >= u'a' && value <= u'f') return value.unicode() - u'a' + 10;
    return -1;
}

ParseResult<QColor> readRgbColor(const QJsonValue &value,
                                 const QString &context)
{
    auto text = readString(value, context);
    if (!text) return std::unexpected(std::move(text.error()));
    if (text->size() != 7 || text->front() != u'#') {
        return std::unexpected(
            QStringLiteral("%1 must be a canonical #rrggbb color")
                .arg(context));
    }
    std::array<int, 6> digits{};
    for (qsizetype index = 0; index < 6; ++index) {
        digits[static_cast<std::size_t>(index)] = hexDigit(text->at(index + 1));
        if (digits[static_cast<std::size_t>(index)] < 0) {
            return std::unexpected(
                QStringLiteral("%1 must be a canonical #rrggbb color")
                    .arg(context));
        }
    }
    return QColor((digits[0] << 4) | digits[1], (digits[2] << 4) | digits[3],
                  (digits[4] << 4) | digits[5]);
}

ParseResult<QStringList> readStringList(const QJsonValue &value,
                                        const QString &context)
{
    auto array = readArray(value, context);
    if (!array) return std::unexpected(std::move(array.error()));
    QStringList result;
    result.reserve(array->size());
    for (qsizetype index = 0; index < array->size(); ++index) {
        auto entry = readString(
            array->at(index), QStringLiteral("%1[%2]").arg(context).arg(index));
        if (!entry) return std::unexpected(std::move(entry.error()));
        result.append(std::move(*entry));
    }
    return result;
}

ParseResult<QStringList> readNonEmptyStringList(const QJsonValue &value,
                                                const QString &context)
{
    auto result = readStringList(value, context);
    if (!result) return std::unexpected(std::move(result.error()));
    for (qsizetype index = 0; index < result->size(); ++index) {
        if (result->at(index).isEmpty()) {
            return std::unexpected(
                QStringLiteral("%1[%2] must be a non-empty string")
                    .arg(context)
                    .arg(index));
        }
    }
    return result;
}

ParseResult<QStringList> readAbsolutePathList(const QJsonValue &value,
                                              const QString &context)
{
    auto paths = readNonEmptyStringList(value, context);
    if (!paths) return std::unexpected(std::move(paths.error()));

    QStringList result;
    result.reserve(paths->size());
    for (qsizetype index = 0; index < paths->size(); ++index) {
        QString path = std::move((*paths)[index]);
        if (!QDir::isAbsolutePath(path)) {
            return std::unexpected(
                QStringLiteral("%1[%2] must be an absolute path")
                    .arg(context)
                    .arg(index));
        }
        if (result.contains(path)) {
            return std::unexpected(
                QStringLiteral("%1 contains duplicate path '%2'")
                    .arg(context, path));
        }
        result.append(std::move(path));
    }
    return result;
}

ParseResult<QVector<GhosttyConfigFile>> readConfigFiles(const QJsonValue &value,
                                                        const QString &context)
{
    auto paths = readStringList(value, context);
    if (!paths) return std::unexpected(std::move(paths.error()));

    QVector<GhosttyConfigFile> result;
    result.reserve(paths->size());
    for (QString &encoded : *paths) {
        const bool optional = encoded.startsWith(u'?');
        if (optional) encoded.remove(0, 1);
        if (encoded.isEmpty()) {
            return std::unexpected(
                QStringLiteral("%1 entries must contain a path").arg(context));
        }
        result.append({
            .path = std::move(encoded),
            .optional = optional,
        });
    }
    return result;
}

template <typename Enum, std::size_t Size>
ParseResult<Enum>
readEnum(const QJsonValue &value, const QString &context,
         const std::array<std::pair<QLatin1StringView, Enum>, Size> &allowed)
{
    auto text = readString(value, context);
    if (!text) return std::unexpected(std::move(text.error()));
    const auto match =
        std::ranges::find_if(allowed, [&text](const auto &candidate) {
            return *text == candidate.first;
        });
    if (match == allowed.cend()) {
        return std::unexpected(QStringLiteral("%1 has unsupported value '%2'")
                                   .arg(context, *text));
    }
    return match->second;
}

ParseResult<QVector<TerminalInitialInput>>
readInitialInput(const QJsonValue &value, const QString &context)
{
    auto array = readArray(value, context);
    if (!array) return std::unexpected(std::move(array.error()));

    QVector<TerminalInitialInput> result;
    result.reserve(array->size());
    for (qsizetype index = 0; index < array->size(); ++index) {
        const QString entryContext =
            QStringLiteral("%1[%2]").arg(context).arg(index);
        auto object =
            readExactObject(array->at(index), entryContext, InitialInputFields);
        if (!object) return std::unexpected(std::move(object.error()));

        auto kind =
            readString(object->value(QLatin1StringView("kind")),
                       childContext(entryContext, QLatin1StringView("kind")));
        if (!kind) return std::unexpected(std::move(kind.error()));
        auto bytes = readByteArray(
            object->value(QLatin1StringView("value")),
            childContext(entryContext, QLatin1StringView("value")));
        if (!bytes) return std::unexpected(std::move(bytes.error()));

        if (*kind == QLatin1StringView("raw")) {
            result.append(TerminalInitialInputs::Raw{
                .bytes = std::move(*bytes),
            });
        } else if (*kind == QLatin1StringView("path")) {
            result.append(TerminalInitialInputs::Path{
                .path = std::move(*bytes),
            });
        } else {
            return std::unexpected(
                QStringLiteral("%1.kind has unsupported value '%2'")
                    .arg(entryContext, *kind));
        }
    }
    return result;
}

ParseResult<SidedModifier> readSidedModifier(const QJsonValue &value,
                                             const QString &context)
{
    auto object = readExactObject(value, context, SidedModifierFields);
    if (!object) return std::unexpected(std::move(object.error()));

    constexpr auto Keys =
        std::to_array<std::pair<QLatin1StringView, ModifierKey>>({
            {QLatin1StringView("shift"), ModifierKey::Shift},
            {QLatin1StringView("ctrl"), ModifierKey::Ctrl},
            {QLatin1StringView("alt"), ModifierKey::Alt},
            {QLatin1StringView("super"), ModifierKey::Super},
        });
    constexpr auto Sides =
        std::to_array<std::pair<QLatin1StringView, ModifierSide>>({
            {QLatin1StringView("left"), ModifierSide::Left},
            {QLatin1StringView("right"), ModifierSide::Right},
        });

    auto key =
        readEnum(object->value(QLatin1StringView("modifier")),
                 childContext(context, QLatin1StringView("modifier")), Keys);
    if (!key) return std::unexpected(std::move(key.error()));
    auto side =
        readEnum(object->value(QLatin1StringView("side")),
                 childContext(context, QLatin1StringView("side")), Sides);
    if (!side) return std::unexpected(std::move(side.error()));
    return SidedModifier{.key = *key, .side = *side};
}

ParseResult<QVector<ModifierRemap>> readModifierRemaps(const QJsonValue &value,
                                                       const QString &context)
{
    auto array = readArray(value, context);
    if (!array) return std::unexpected(std::move(array.error()));

    QVector<ModifierRemap> result;
    result.reserve(array->size());
    for (qsizetype index = 0; index < array->size(); ++index) {
        const QString entryContext =
            QStringLiteral("%1[%2]").arg(context).arg(index);
        auto object = readExactObject(array->at(index), entryContext,
                                      ModifierRemapFields);
        if (!object) return std::unexpected(std::move(object.error()));

        auto from = readSidedModifier(
            object->value(QLatin1StringView("from")),
            childContext(entryContext, QLatin1StringView("from")));
        if (!from) return std::unexpected(std::move(from.error()));
        auto to = readSidedModifier(
            object->value(QLatin1StringView("to")),
            childContext(entryContext, QLatin1StringView("to")));
        if (!to) return std::unexpected(std::move(to.error()));
        if (std::ranges::any_of(result, [&from](const ModifierRemap &remap) {
                return remap.from == *from;
            })) {
            return std::unexpected(
                QStringLiteral("%1 contains duplicate source modifier")
                    .arg(entryContext));
        }
        result.append({.from = *from, .to = *to});
    }
    return result;
}

ParseResult<QVector<CommandPaletteEntry>>
readCommandPalette(const QJsonValue &value, const QString &context)
{
    auto array = readArray(value, context);
    if (!array) return std::unexpected(std::move(array.error()));

    QVector<CommandPaletteEntry> result;
    result.reserve(array->size());
    for (qsizetype index = 0; index < array->size(); ++index) {
        const QString entryContext =
            QStringLiteral("%1[%2]").arg(context).arg(index);
        auto object = readExactObject(array->at(index), entryContext,
                                      CommandPaletteEntryFields);
        if (!object) return std::unexpected(std::move(object.error()));

        CommandPaletteEntry entry;
        for (const auto &[name, destination, allowEmpty] :
             std::to_array<std::tuple<QLatin1StringView, QString *, bool>>({
                 {QLatin1StringView("title"), &entry.title, true},
                 {QLatin1StringView("description"), &entry.description, true},
                 {QLatin1StringView("action-key"), &entry.actionKey, false},
                 {QLatin1StringView("action"), &entry.action, false},
             })) {
            auto parsed = allowEmpty
                ? readString(object->value(name),
                             childContext(entryContext, name))
                : readNonEmptyString(object->value(name),
                                     childContext(entryContext, name));
            if (!parsed) return std::unexpected(std::move(parsed.error()));
            *destination = std::move(*parsed);
        }
        result.append(std::move(entry));
    }
    return result;
}

ParseResult<AppNotificationOptions>
readAppNotifications(const QJsonValue &value, const QString &context)
{
    return readBooleanObject<AppNotificationOptions>(
        value, context, AppNotificationFields,
        std::to_array<
            std::pair<QLatin1StringView, bool AppNotificationOptions::*>>({
            {QLatin1StringView("clipboard-copy"),
             &AppNotificationOptions::clipboardCopy},
            {QLatin1StringView("config-reload"),
             &AppNotificationOptions::configReload},
        }));
}

ParseResult<std::optional<QuickTerminalExtent>>
readOptionalQuickTerminalExtent(const QJsonValue &value, const QString &context)
{
    if (value.isNull()) return std::optional<QuickTerminalExtent>{};
    if (!value.isObject()) {
        return std::unexpected(
            QStringLiteral("%1 must be an object or null").arg(context));
    }

    const QJsonObject candidate = value.toObject();
    auto kind = readString(candidate.value(QLatin1StringView("kind")),
                           childContext(context, QLatin1StringView("kind")));
    if (!kind) return std::unexpected(std::move(kind.error()));

    if (*kind == QLatin1StringView("pixels")) {
        auto object =
            readExactObject(value, context, QuickTerminalPixelsFields);
        if (!object) return std::unexpected(std::move(object.error()));
        auto pixels = readUnsignedInteger<quint32>(
            object->value(QLatin1StringView("value")),
            childContext(context, QLatin1StringView("value")));
        if (!pixels) return std::unexpected(std::move(pixels.error()));
        return QuickTerminalExtent{QuickTerminalPixels{.value = *pixels}};
    }
    if (*kind == QLatin1StringView("percentage")) {
        auto object =
            readExactObject(value, context, QuickTerminalPercentageFields);
        if (!object) return std::unexpected(std::move(object.error()));
        auto bits = readDecimalUint64(
            object->value(QLatin1StringView("value-bits")),
            childContext(context, QLatin1StringView("value-bits")));
        if (!bits) return std::unexpected(std::move(bits.error()));
        if (*bits > std::numeric_limits<quint32>::max()) {
            return std::unexpected(
                QStringLiteral("%1.value-bits exceeds the uint32 range")
                    .arg(context));
        }
        const float percentage =
            std::bit_cast<float>(static_cast<quint32>(*bits));
        if (!std::isfinite(percentage) || percentage < 0.0F) {
            return std::unexpected(
                QStringLiteral("%1.value-bits is not a finite nonnegative f32")
                    .arg(context));
        }
        return QuickTerminalExtent{
            QuickTerminalPercentage{.value = percentage}};
    }
    return std::unexpected(QStringLiteral("%1.kind has unsupported value '%2'")
                               .arg(context, *kind));
}

ParseResult<QuickTerminalSize> readQuickTerminalSize(const QJsonValue &value,
                                                     const QString &context)
{
    auto object = readExactObject(value, context, QuickTerminalSizeFields);
    if (!object) return std::unexpected(std::move(object.error()));

    QuickTerminalSize result;
    for (const auto [name, destination] :
         std::to_array<std::pair<QLatin1StringView,
                                 std::optional<QuickTerminalExtent> *>>({
             {QLatin1StringView("primary"), &result.primary},
             {QLatin1StringView("secondary"), &result.secondary},
         })) {
        auto parsed = readOptionalQuickTerminalExtent(
            object->value(name), childContext(context, name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        *destination = std::move(*parsed);
    }
    return result;
}

ParseResult<std::optional<QColor>> readOptionalRgb(const QJsonValue &value,
                                                   const QString &context)
{
    if (value.isNull()) return std::nullopt;
    auto color = readRgbColor(value, context);
    if (!color) return std::unexpected(std::move(color.error()));
    return *color;
}

ParseResult<TerminalColorValue> readTerminalColor(const QJsonValue &value,
                                                  const QString &context)
{
    if (value.isNull()) {
        return std::unexpected(
            QStringLiteral("%1 must not be null").arg(context));
    }
    if (value.isString()) {
        const QString sentinel = value.toString();
        if (sentinel == QStringLiteral("cell-foreground")) {
            return TerminalColorValue{
                .kind = TerminalColorKind::CellForeground,
                .color = {},
            };
        }
        if (sentinel == QStringLiteral("cell-background")) {
            return TerminalColorValue{
                .kind = TerminalColorKind::CellBackground,
                .color = {},
            };
        }
    }
    auto color = readRgbColor(value, context);
    if (!color) return std::unexpected(std::move(color.error()));
    return TerminalColorValue::fromColor(*color);
}

ParseResult<TerminalColorValue>
readOptionalTerminalColor(const QJsonValue &value, const QString &context)
{
    if (value.isNull()) return TerminalColorValue{};
    return readTerminalColor(value, context);
}

ParseResult<std::optional<bool>> readOptionalBoolean(const QJsonValue &value,
                                                     const QString &context)
{
    if (value.isNull()) return std::nullopt;
    auto result = readBoolean(value, context);
    if (!result) return std::unexpected(std::move(result.error()));
    return *result;
}

ParseResult<TerminalBoldColor> readBoldColor(const QJsonValue &value,
                                             const QString &context)
{
    if (value.isNull()) return TerminalBoldColor{};
    if (value.isString() && value.toString() == QStringLiteral("bright")) {
        return TerminalBoldColor{
            .kind = TerminalBoldColorKind::Bright,
            .color = {},
        };
    }
    auto color = readRgbColor(value, context);
    if (!color) return std::unexpected(std::move(color.error()));
    return TerminalBoldColor{
        .kind = TerminalBoldColorKind::Color,
        .color = *color,
    };
}

ParseResult<QVector<QColor>> readPalette(const QJsonValue &value,
                                         const QString &context)
{
    auto array = readArray(value, context);
    if (!array) return std::unexpected(std::move(array.error()));
    if (array->size() != 256) {
        return std::unexpected(
            QStringLiteral("%1 must contain exactly 256 colors").arg(context));
    }
    QVector<QColor> result;
    result.reserve(array->size());
    for (qsizetype index = 0; index < array->size(); ++index) {
        auto color = readRgbColor(
            array->at(index), QStringLiteral("%1[%2]").arg(context).arg(index));
        if (!color) return std::unexpected(std::move(color.error()));
        result.append(*color);
    }
    return result;
}

ParseResult<TerminalFontStyle> readFontStyle(const QJsonValue &value,
                                             const QString &context)
{
    if (!value.isObject()) {
        return std::unexpected(
            QStringLiteral("%1 must be an object").arg(context));
    }

    const QJsonObject unvalidated = value.toObject();
    constexpr QLatin1StringView Kind("kind");
    if (!unvalidated.contains(Kind)) {
        return std::unexpected(
            QStringLiteral("%1 is missing field 'kind'").arg(context));
    }
    auto kind =
        readString(unvalidated.value(Kind), childContext(context, Kind));
    if (!kind) return std::unexpected(std::move(kind.error()));

    Fields fields;
    if (*kind == QLatin1StringView("automatic")
        || *kind == QLatin1StringView("disabled")) {
        fields = FontStyleFields;
    } else if (*kind == QLatin1StringView("named")) {
        fields = NamedFontStyleFields;
    } else {
        return std::unexpected(
            QStringLiteral("%1.kind has unsupported value '%2'")
                .arg(context, *kind));
    }

    auto object = readExactObject(value, context, fields);
    if (!object) return std::unexpected(std::move(object.error()));
    if (*kind == QLatin1StringView("automatic")) {
        return TerminalFontStyles::Automatic{};
    }
    if (*kind == QLatin1StringView("disabled")) {
        return TerminalFontStyles::Disabled{};
    }

    constexpr QLatin1StringView Name("name");
    auto name =
        readNonEmptyString(object->value(Name), childContext(context, Name));
    if (!name) return std::unexpected(std::move(name.error()));
    return TerminalFontStyles::Named{.name = std::move(*name)};
}

ParseResult<TerminalMetricModifierSet::Value>
readMetricModifier(const QJsonValue &value, const QString &context)
{
    if (value.isNull()) return TerminalMetricModifierSet::Value{};

    auto object = readExactObject(value, context, MetricModifierFields);
    if (!object) return std::unexpected(std::move(object.error()));

    constexpr QLatin1StringView Kind("kind");
    constexpr QLatin1StringView Value("value");
    auto kind = readString(object->value(Kind), childContext(context, Kind));
    if (!kind) return std::unexpected(std::move(kind.error()));

    if (*kind == QLatin1StringView("absolute")) {
        auto pixels = readSignedInteger<qint32>(object->value(Value),
                                                childContext(context, Value));
        if (!pixels) return std::unexpected(std::move(pixels.error()));
        return TerminalMetricModifier{
            TerminalMetricModifiers::Absolute{.pixels = *pixels}};
    }
    if (*kind == QLatin1StringView("percentage")) {
        auto multiplier = readFiniteDouble(object->value(Value),
                                           childContext(context, Value), 0.0);
        if (!multiplier) {
            return std::unexpected(std::move(multiplier.error()));
        }
        return TerminalMetricModifier{
            TerminalMetricModifiers::Percentage{.multiplier = *multiplier}};
    }
    return std::unexpected(QStringLiteral("%1.kind has unsupported value '%2'")
                               .arg(context, *kind));
}

ParseResult<std::vector<TerminalMetric>>
readMetricModifierOrder(const QJsonValue &value, const QString &context,
                        const TerminalMetricModifierSet &modifiers)
{
    auto array = readArray(value, context);
    if (!array) return std::unexpected(std::move(array.error()));

    std::vector<TerminalMetric> result;
    result.reserve(static_cast<std::size_t>(array->size()));
    std::array<bool, terminalEnumIndex(TerminalMetric::Count)> seen{};
    for (qsizetype index = 0; index < array->size(); ++index) {
        const QString entryContext =
            QStringLiteral("%1[%2]").arg(context).arg(index);
        auto name = readString(array->at(index), entryContext);
        if (!name) return std::unexpected(std::move(name.error()));

        const auto descriptor = std::ranges::find_if(
            MetricModifierValueFields, [&name](const auto &candidate) {
                return *name == candidate.first;
            });
        if (descriptor == MetricModifierValueFields.cend()) {
            return std::unexpected(
                QStringLiteral("%1 has unsupported value '%2'")
                    .arg(entryContext, *name));
        }

        const TerminalMetric metric = descriptor->second;
        bool &alreadySeen = seen[terminalEnumIndex(metric)];
        if (alreadySeen) {
            return std::unexpected(
                QStringLiteral("%1 contains duplicate modifier '%2'")
                    .arg(context, *name));
        }
        if (!modifiers[metric]) {
            return std::unexpected(
                QStringLiteral("%1 refers to unset modifier '%2'")
                    .arg(entryContext, *name));
        }
        alreadySeen = true;
        result.push_back(metric);
    }

    for (const auto &[name, metric] : MetricModifierValueFields) {
        if (modifiers[metric] && !seen[terminalEnumIndex(metric)]) {
            return std::unexpected(
                QStringLiteral("%1 does not include active modifier '%2'")
                    .arg(context, name));
        }
    }
    return result;
}

ParseResult<GhosttyConfigValues> readValues(const QJsonValue &value)
{
    constexpr QLatin1StringView Context("values");
    auto object = readExactObject(value, Context.toString(), ValueFields);
    if (!object) return std::unexpected(std::move(object.error()));

    GhosttyConfigValues result;
    const auto fieldValue = [&object](QLatin1StringView name) {
        return object->value(name);
    };
    const auto context = [](QLatin1StringView name) {
        return childContext(QStringLiteral("values"), name);
    };
    const auto assign = [&](QLatin1StringView name, auto &destination,
                            auto reader) -> ParseResult<void> {
        auto parsed = reader(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        destination = std::move(*parsed);
        return {};
    };
    const auto assignBoolean = [&](QLatin1StringView name,
                                   bool &destination) -> ParseResult<void> {
        return assign(name, destination, readBoolean);
    };

    if (auto parsed = assign(QLatin1StringView("term"), result.term,
                             readNonEmptyByteArray);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed = assign(QLatin1StringView("enquiry-response"),
                             result.enquiryResponse, readByteArray);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    {
        constexpr QLatin1StringView name("command");
        auto parsed =
            readOptionalCommand(fieldValue(name), context(name), true);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.ordinaryCommand = std::move(*parsed);
    }
    {
        constexpr QLatin1StringView name("initial-command");
        auto parsed =
            readOptionalCommand(fieldValue(name), context(name), false);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.initialCommand = std::move(*parsed);
    }
    if (auto parsed = assign(QLatin1StringView("input"), result.initialInput,
                             readInitialInput);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed = assign(QLatin1StringView("key-remap"),
                             result.modifierRemaps, readModifierRemaps);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed = assignBoolean(QLatin1StringView("wait-after-command"),
                                    result.waitAfterCommand);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    {
        constexpr QLatin1StringView name("abnormal-command-exit-runtime");
        auto parsed =
            readUnsignedInteger<quint32>(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.abnormalCommandExitRuntimeMilliseconds = *parsed;
    }
    if (auto parsed = assign(QLatin1StringView("env"), result.environment,
                             readEnvironment);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    {
        constexpr QLatin1StringView name("working-directory");
        const QJsonValue encoded = fieldValue(name);
        if (encoded.isString()
            && encoded.toString() == QLatin1StringView("inherit")) {
            result.workingDirectoryPath.reset();
        } else {
            auto parsed = readByteArray(encoded, context(name));
            if (!parsed) return std::unexpected(std::move(parsed.error()));
            result.workingDirectoryPath = std::move(*parsed);
        }
    }
    {
        constexpr QLatin1StringView name("title");
        auto parsed = readOptionalString(fieldValue(name), context(name), true);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.title = std::move(*parsed);
    }
    {
        constexpr QLatin1StringView name("class");
        const QJsonValue encoded = fieldValue(name);
        if (encoded.isNull()) {
            result.applicationClass.reset();
        } else {
            auto parsed = readByteArray(encoded, context(name));
            if (!parsed) return std::unexpected(std::move(parsed.error()));
            result.applicationClass = std::move(*parsed);
        }
    }
    for (const auto &[name, role] : FontFamilyFields) {
        if (auto parsed = assign(name, result.typography.face(role).families,
                                 readNonEmptyStringList);
            !parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
    }
    for (const auto &[name, role] : FontStyleValueFields) {
        if (auto parsed =
                assign(name, result.typography.face(role).style, readFontStyle);
            !parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
    }
    if (auto parsed = assign(QLatin1StringView("font-feature"),
                             result.typography.features, readFontFeatures);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    for (const auto &[name, role] : FontVariationValueFields) {
        if (auto parsed = assign(name, result.typography.face(role).variations,
                                 readFontVariations);
            !parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
    }
    if (auto parsed =
            assign(QLatin1StringView("font-codepoint-map"),
                   result.typography.codepointMap, readCodepointFontMap);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed =
            assign(QLatin1StringView("font-synthetic-style"),
                   result.typography.syntheticStyle, readSyntheticStyle);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    {
        constexpr QLatin1StringView name("font-shaping-break");
        auto shaping = readExactObject(fieldValue(name), context(name),
                                       ShapingBreakFields);
        if (!shaping) {
            return std::unexpected(std::move(shaping.error()));
        }
        auto cursor = readBoolean(
            shaping->value(QLatin1StringView("cursor")),
            childContext(context(name), QLatin1StringView("cursor")));
        if (!cursor) return std::unexpected(std::move(cursor.error()));
        result.typography.shapingBreakCursor = *cursor;
    }
    if (auto parsed =
            assign(QLatin1StringView("freetype-load-flags"),
                   result.typography.freetypeLoadFlags, readFreetypeLoadFlags);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    for (const auto &[name, metric] : MetricModifierValueFields) {
        if (auto parsed =
                assign(name, result.typography.metricModifiers[metric],
                       readMetricModifier);
            !parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
    }
    {
        constexpr QLatin1StringView name("metric-modifier-order");
        auto parsed = readMetricModifierOrder(
            fieldValue(name), context(name), result.typography.metricModifiers);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.typography.metricModifiers.applicationOrder = std::move(*parsed);
    }
    if (auto parsed = assign(QLatin1StringView("config-file"),
                             result.configFiles, readConfigFiles);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assign(QLatin1StringView("bell-features"),
                             result.bellFeatures, readBellFeatures);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assign(QLatin1StringView("bell-audio-path"),
                             result.bellAudioPath, readOptionalConfigPath);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    const auto readDouble = [](const QJsonValue &entry,
                               const QString &entryContext) {
        return readFiniteDouble(entry, entryContext);
    };
    const auto readUint32 = [](const QJsonValue &entry,
                               const QString &entryContext) {
        return readUnsignedInteger<quint32>(entry, entryContext);
    };
    if (auto parsed = assign(QLatin1StringView("font-size"),
                             result.typography.pointSize, readDouble);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assign(QLatin1StringView("bell-audio-volume"),
                             result.bellAudioVolume, readDouble);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed =
            assign(QLatin1StringView("mouse-scroll-multiplier"),
                   result.mouseScrollMultiplier, readMouseScrollMultiplier);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed = assign(QLatin1StringView("scroll-to-bottom"),
                             result.scrollToBottom, readScrollToBottom);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed = assign(QLatin1StringView("selection-word-chars"),
                             result.selectionWordChars, readUnicodeScalarList);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed = assign(QLatin1StringView("click-repeat-interval"),
                             result.clickRepeatIntervalMilliseconds,
                             readFinalizedClickRepeatInterval);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }

    for (const auto [name, destination] :
         std::to_array<std::pair<QLatin1StringView, QColor *>>({
             {QLatin1StringView("foreground"),
              &result.appearance.foregroundColor},
             {QLatin1StringView("background"),
              &result.appearance.backgroundColor},
         })) {
        if (auto parsed = assign(name, *destination, readRgbColor); !parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
    }
    {
        constexpr QLatin1StringView name("background-opacity");
        auto parsed = readFiniteDouble(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.background.opacity = std::clamp(*parsed, 0.0, 1.0);
    }
    if (auto parsed =
            assignBoolean(QLatin1StringView("background-opacity-cells"),
                          result.background.opacityCells);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed = assign(QLatin1StringView("background-blur"),
                             result.backgroundBlur, readBackgroundBlur);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed =
            assign(QLatin1StringView("background-image"),
                   result.background.image.path, readOptionalConfigPath);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed = assign(QLatin1StringView("background-image-opacity"),
                             result.background.image.opacity, readDouble);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    {
        constexpr QLatin1StringView name("background-image-position");
        constexpr auto allowed = std::to_array<
            std::pair<QLatin1StringView, TerminalBackgroundImagePosition>>({
            {QLatin1StringView("top-left"),
             TerminalBackgroundImagePosition::TopLeft},
            {QLatin1StringView("top-center"),
             TerminalBackgroundImagePosition::TopCenter},
            {QLatin1StringView("top-right"),
             TerminalBackgroundImagePosition::TopRight},
            {QLatin1StringView("center-left"),
             TerminalBackgroundImagePosition::CenterLeft},
            {QLatin1StringView("center"),
             TerminalBackgroundImagePosition::Center},
            {QLatin1StringView("center-center"),
             TerminalBackgroundImagePosition::Center},
            {QLatin1StringView("center-right"),
             TerminalBackgroundImagePosition::CenterRight},
            {QLatin1StringView("bottom-left"),
             TerminalBackgroundImagePosition::BottomLeft},
            {QLatin1StringView("bottom-center"),
             TerminalBackgroundImagePosition::BottomCenter},
            {QLatin1StringView("bottom-right"),
             TerminalBackgroundImagePosition::BottomRight},
        });
        auto parsed = readEnum(fieldValue(name), context(name), allowed);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.background.image.position = *parsed;
    }
    {
        constexpr QLatin1StringView name("background-image-fit");
        constexpr auto allowed = std::to_array<
            std::pair<QLatin1StringView, TerminalBackgroundImageFit>>({
            {QLatin1StringView("contain"), TerminalBackgroundImageFit::Contain},
            {QLatin1StringView("cover"), TerminalBackgroundImageFit::Cover},
            {QLatin1StringView("stretch"), TerminalBackgroundImageFit::Stretch},
            {QLatin1StringView("none"), TerminalBackgroundImageFit::None},
        });
        auto parsed = readEnum(fieldValue(name), context(name), allowed);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.background.image.fit = *parsed;
    }
    if (auto parsed =
            assignBoolean(QLatin1StringView("background-image-repeat"),
                          result.background.image.repeat);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed = assign(QLatin1StringView("custom-shader"),
                             result.customShaders.sources, readConfigPaths);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    {
        constexpr QLatin1StringView name("custom-shader-animation");
        constexpr auto allowed = std::to_array<
            std::pair<QLatin1StringView, TerminalCustomShaderAnimation>>({
            {QLatin1StringView("false"), TerminalCustomShaderAnimation::Never},
            {QLatin1StringView("true"), TerminalCustomShaderAnimation::Focused},
            {QLatin1StringView("always"),
             TerminalCustomShaderAnimation::Always},
        });
        auto parsed = readEnum(fieldValue(name), context(name), allowed);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.customShaders.animation = *parsed;
    }
    {
        constexpr QLatin1StringView name("unfocused-split-opacity");
        auto parsed =
            readFiniteDouble(fieldValue(name), context(name), 0.15, 1.0);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.splitAppearance.unfocusedOpacity = *parsed;
    }
    if (auto parsed =
            assign(QLatin1StringView("unfocused-split-fill"),
                   result.splitAppearance.unfocusedFill, readOptionalRgb);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed =
            assign(QLatin1StringView("split-divider-color"),
                   result.splitAppearance.dividerColor, readOptionalRgb);
        !parsed)
        return std::unexpected(std::move(parsed.error()));

    if (auto parsed = assign(QLatin1StringView("clipboard-codepoint-map"),
                             result.selectionClipboard.codepointMap,
                             readClipboardCodepointMap);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }

    for (const auto [name, destination] :
         std::to_array<std::pair<QLatin1StringView, bool *>>({
             {QLatin1StringView("split-inherit-working-directory"),
              &result.splitInheritWorkingDirectory},
             {QLatin1StringView("split-preserve-zoom"),
              &result.splitPreserveZoom},
             {QLatin1StringView("tab-inherit-working-directory"),
              &result.tabInheritWorkingDirectory},
             {QLatin1StringView("window-inherit-working-directory"),
              &result.windowInheritWorkingDirectory},
             {QLatin1StringView("window-inherit-font-size"),
              &result.windowInheritFontSize},
             {QLatin1StringView("maximize"), &result.maximize},
             {QLatin1StringView("desktop-notifications"),
              &result.desktopNotifications},
             {QLatin1StringView("progress-style"), &result.progressStyle},
             {QLatin1StringView("clipboard-trim-trailing-spaces"),
              &result.selectionClipboard.trimTrailingSpaces},
             {QLatin1StringView("clipboard-paste-protection"),
              &result.clipboardPaste.protection},
             {QLatin1StringView("clipboard-paste-bracketed-safe"),
              &result.clipboardPaste.bracketedSafe},
             {QLatin1StringView("selection-clear-on-typing"),
              &result.selectionClipboard.clearOnTyping},
             {QLatin1StringView("selection-clear-on-copy"),
              &result.selectionClipboard.clearOnCopy},
             {QLatin1StringView("mouse-reporting"), &result.mouseReporting},
             {QLatin1StringView("mouse-hide-while-typing"),
              &result.mouseHideWhileTyping},
             {QLatin1StringView("focus-follows-mouse"),
              &result.focusFollowsMouse},
             {QLatin1StringView("vt-kam-allowed"), &result.vtKamAllowed},
             {QLatin1StringView("title-report"), &result.titleReport},
             {QLatin1StringView("link-url"), &result.linkUrl},
             {QLatin1StringView("link-osc8"), &result.linkOsc8},
             {QLatin1StringView("quit-after-last-window-closed"),
              &result.quitAfterLastWindowClosed},
             {QLatin1StringView("initial-window"), &result.initialWindow},
             {QLatin1StringView("config-default-files"),
              &result.configDefaultFiles},
             {QLatin1StringView("quick-terminal-autohide"),
              &result.applicationShell.quickTerminal.autohide},
             {QLatin1StringView("linux-cgroup-hard-fail"),
              &result.linuxCgroup.hardFail},
         })) {
        if (auto parsed = assignBoolean(name, *destination); !parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
    }

    if (auto parsed =
            assign(QLatin1StringView("window-padding-x"),
                   result.padding.horizontal, readTerminalPaddingAxis);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed = assign(QLatin1StringView("window-padding-y"),
                             result.padding.vertical, readTerminalPaddingAxis);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    {
        constexpr QLatin1StringView name("window-padding-balance");
        constexpr auto allowed =
            std::to_array<std::pair<QLatin1StringView, TerminalPaddingBalance>>(
                {
                    {QLatin1StringView("false"),
                     TerminalPaddingBalance::Disabled},
                    {QLatin1StringView("true"),
                     TerminalPaddingBalance::Balanced},
                    {QLatin1StringView("equal"), TerminalPaddingBalance::Equal},
                });
        auto parsed = readEnum(fieldValue(name), context(name), allowed);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.padding.balance = *parsed;
    }
    {
        constexpr QLatin1StringView name("window-padding-color");
        constexpr auto allowed =
            std::to_array<std::pair<QLatin1StringView, TerminalPaddingColor>>({
                {QLatin1StringView("background"),
                 TerminalPaddingColor::Background},
                {QLatin1StringView("extend"), TerminalPaddingColor::Extend},
                {QLatin1StringView("extend-always"),
                 TerminalPaddingColor::ExtendAlways},
            });
        auto parsed = readEnum(fieldValue(name), context(name), allowed);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.padding.color = *parsed;
    }

    constexpr auto NewTabPositions =
        std::to_array<std::pair<QLatin1StringView, WindowNewTabPosition>>({
            {QLatin1StringView("current"), WindowNewTabPosition::Current},
            {QLatin1StringView("end"), WindowNewTabPosition::End},
        });
    constexpr auto TabBarModes =
        std::to_array<std::pair<QLatin1StringView, WindowShowTabBar>>({
            {QLatin1StringView("always"), WindowShowTabBar::Always},
            {QLatin1StringView("auto"), WindowShowTabBar::Auto},
            {QLatin1StringView("never"), WindowShowTabBar::Never},
        });
    constexpr auto DragHandleModes =
        std::to_array<std::pair<QLatin1StringView, DragHandleMode>>({
            {QLatin1StringView("always"), DragHandleMode::Always},
            {QLatin1StringView("auto"), DragHandleMode::Auto},
            {QLatin1StringView("never"), DragHandleMode::Never},
        });
    constexpr auto GraphemeWidthMethods =
        std::to_array<std::pair<QLatin1StringView, GraphemeWidthMethod>>({
            {QLatin1StringView("legacy"), GraphemeWidthMethod::Legacy},
            {QLatin1StringView("unicode"), GraphemeWidthMethod::Unicode},
        });
    constexpr auto WindowDecorations =
        std::to_array<std::pair<QLatin1StringView, WindowDecorationMode>>({
            {QLatin1StringView("auto"), WindowDecorationMode::Auto},
            {QLatin1StringView("client"), WindowDecorationMode::Client},
            {QLatin1StringView("server"), WindowDecorationMode::Server},
            {QLatin1StringView("none"), WindowDecorationMode::None},
        });
    constexpr auto WindowThemes =
        std::to_array<std::pair<QLatin1StringView, WindowTheme>>({
            {QLatin1StringView("auto"), WindowTheme::Auto},
            {QLatin1StringView("system"), WindowTheme::System},
            {QLatin1StringView("light"), WindowTheme::Light},
            {QLatin1StringView("dark"), WindowTheme::Dark},
            {QLatin1StringView("ghostty"), WindowTheme::Ghostty},
        });
    constexpr auto AlphaBlendingModes =
        std::to_array<std::pair<QLatin1StringView, TerminalAlphaBlending>>({
            {QLatin1StringView("native"), TerminalAlphaBlending::Native},
            {QLatin1StringView("linear"), TerminalAlphaBlending::Linear},
            {QLatin1StringView("linear-corrected"),
             TerminalAlphaBlending::LinearCorrected},
        });
    constexpr auto WindowSubtitles =
        std::to_array<std::pair<QLatin1StringView, WindowSubtitleMode>>({
            {QLatin1StringView("false"), WindowSubtitleMode::Disabled},
            {QLatin1StringView("working-directory"),
             WindowSubtitleMode::WorkingDirectory},
        });
    constexpr auto FullscreenModes =
        std::to_array<std::pair<QLatin1StringView, GhosttyFullscreenMode>>({
            {QLatin1StringView("false"), GhosttyFullscreenMode::Disabled},
            {QLatin1StringView("true"), GhosttyFullscreenMode::Enabled},
            {QLatin1StringView("non-native"), GhosttyFullscreenMode::NonNative},
            {QLatin1StringView("non-native-visible-menu"),
             GhosttyFullscreenMode::NonNativeVisibleMenu},
            {QLatin1StringView("non-native-padded-notch"),
             GhosttyFullscreenMode::NonNativePaddedNotch},
        });
    constexpr auto CursorStyles =
        std::to_array<std::pair<QLatin1StringView, TerminalCursorStyle>>({
            {QLatin1StringView("block"), TerminalCursorStyle::Block},
            {QLatin1StringView("bar"), TerminalCursorStyle::Bar},
            {QLatin1StringView("underline"), TerminalCursorStyle::Underline},
            {QLatin1StringView("block_hollow"),
             TerminalCursorStyle::BlockHollow},
        });
    constexpr auto ConfirmCloseModes =
        std::to_array<std::pair<QLatin1StringView, ConfirmCloseMode>>({
            {QLatin1StringView("false"), ConfirmCloseMode::Never},
            {QLatin1StringView("true"), ConfirmCloseMode::RunningProcesses},
            {QLatin1StringView("always"), ConfirmCloseMode::Always},
        });
    constexpr auto CopyOnSelectModes =
        std::to_array<std::pair<QLatin1StringView, TerminalCopyOnSelectMode>>({
            {QLatin1StringView("false"), TerminalCopyOnSelectMode::Disabled},
            {QLatin1StringView("true"), TerminalCopyOnSelectMode::Primary},
            {QLatin1StringView("clipboard"),
             TerminalCopyOnSelectMode::PrimaryAndClipboard},
        });
    constexpr auto ClipboardAccessModes =
        std::to_array<std::pair<QLatin1StringView, TerminalClipboardAccess>>({
            {QLatin1StringView("ask"), TerminalClipboardAccess::Ask},
            {QLatin1StringView("allow"), TerminalClipboardAccess::Allow},
            {QLatin1StringView("deny"), TerminalClipboardAccess::Deny},
        });
    constexpr auto MiddleClickActions =
        std::to_array<std::pair<QLatin1StringView, MiddleClickAction>>({
            {QLatin1StringView("primary-paste"),
             MiddleClickAction::PrimaryPaste},
            {QLatin1StringView("ignore"), MiddleClickAction::Ignore},
        });
    constexpr auto RightClickActions =
        std::to_array<std::pair<QLatin1StringView, RightClickAction>>({
            {QLatin1StringView("context-menu"), RightClickAction::ContextMenu},
            {QLatin1StringView("paste"), RightClickAction::Paste},
            {QLatin1StringView("copy"), RightClickAction::Copy},
            {QLatin1StringView("copy-or-paste"), RightClickAction::CopyOrPaste},
            {QLatin1StringView("ignore"), RightClickAction::Ignore},
        });
    constexpr auto MouseShiftCaptureModes =
        std::to_array<std::pair<QLatin1StringView, MouseShiftCapture>>({
            {QLatin1StringView("false"), MouseShiftCapture::False},
            {QLatin1StringView("true"), MouseShiftCapture::True},
            {QLatin1StringView("always"), MouseShiftCapture::Always},
            {QLatin1StringView("never"), MouseShiftCapture::Never},
        });
    constexpr auto LinkPreviewModes =
        std::to_array<std::pair<QLatin1StringView, LinkPreviewMode>>({
            {QLatin1StringView("false"), LinkPreviewMode::Never},
            {QLatin1StringView("true"), LinkPreviewMode::Always},
            {QLatin1StringView("osc8"), LinkPreviewMode::Osc8},
        });
    constexpr auto ScrollbarPolicies =
        std::to_array<std::pair<QLatin1StringView, ScrollbarPolicy>>({
            {QLatin1StringView("system"), ScrollbarPolicy::System},
            {QLatin1StringView("never"), ScrollbarPolicy::Never},
        });
    constexpr auto ResizeOverlayModes =
        std::to_array<std::pair<QLatin1StringView, ResizeOverlayMode>>({
            {QLatin1StringView("always"), ResizeOverlayMode::Always},
            {QLatin1StringView("never"), ResizeOverlayMode::Never},
            {QLatin1StringView("after-first"), ResizeOverlayMode::AfterFirst},
        });
    constexpr auto ResizeOverlayPositions =
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
        });
    constexpr auto SingleInstanceModes =
        std::to_array<std::pair<QLatin1StringView, SingleInstanceMode>>({
            {QLatin1StringView("false"), SingleInstanceMode::Disabled},
            {QLatin1StringView("true"), SingleInstanceMode::Enabled},
            {QLatin1StringView("detect"), SingleInstanceMode::Detect},
        });
    constexpr auto LinuxCgroupModes =
        std::to_array<std::pair<QLatin1StringView, LinuxCgroupMode>>({
            {QLatin1StringView("never"), LinuxCgroupMode::Never},
            {QLatin1StringView("always"), LinuxCgroupMode::Always},
            {QLatin1StringView("single-instance"),
             LinuxCgroupMode::SingleInstance},
        });
    constexpr auto QuickTerminalPositions =
        std::to_array<std::pair<QLatin1StringView, QuickTerminalPosition>>({
            {QLatin1StringView("top"), QuickTerminalPosition::Top},
            {QLatin1StringView("bottom"), QuickTerminalPosition::Bottom},
            {QLatin1StringView("left"), QuickTerminalPosition::Left},
            {QLatin1StringView("right"), QuickTerminalPosition::Right},
            {QLatin1StringView("center"), QuickTerminalPosition::Center},
        });
    constexpr auto QuickTerminalScreens =
        std::to_array<std::pair<QLatin1StringView, QuickTerminalScreen>>({
            {QLatin1StringView("main"), QuickTerminalScreen::Main},
            {QLatin1StringView("mouse"), QuickTerminalScreen::Mouse},
            {QLatin1StringView("macos-menu-bar"),
             QuickTerminalScreen::MacosMenuBar},
        });
    constexpr auto QuickTerminalKeyboardModes = std::to_array<
        std::pair<QLatin1StringView, QuickTerminalKeyboardInteractivity>>({
        {QLatin1StringView("none"), QuickTerminalKeyboardInteractivity::None},
        {QLatin1StringView("on-demand"),
         QuickTerminalKeyboardInteractivity::OnDemand},
        {QLatin1StringView("exclusive"),
         QuickTerminalKeyboardInteractivity::Exclusive},
    });
    constexpr auto ShellIntegrationModes = std::to_array<
        std::pair<QLatin1StringView, GhosttyShellIntegrationMode>>({
        {QLatin1StringView("none"), GhosttyShellIntegrationMode::None},
        {QLatin1StringView("detect"), GhosttyShellIntegrationMode::Detect},
        {QLatin1StringView("bash"), GhosttyShellIntegrationMode::Bash},
        {QLatin1StringView("elvish"), GhosttyShellIntegrationMode::Elvish},
        {QLatin1StringView("fish"), GhosttyShellIntegrationMode::Fish},
        {QLatin1StringView("nushell"), GhosttyShellIntegrationMode::Nushell},
        {QLatin1StringView("zsh"), GhosttyShellIntegrationMode::Zsh},
    });

    const auto assignEnum = [&](QLatin1StringView name, auto &destination,
                                const auto &allowed) -> ParseResult<void> {
        auto parsed = readEnum(fieldValue(name), context(name), allowed);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        destination = *parsed;
        return {};
    };
    if (auto parsed = assignEnum(QLatin1StringView("window-new-tab-position"),
                                 result.windowNewTabPosition, NewTabPositions);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("window-show-tab-bar"),
                                 result.windowShowTabBar, TabBarModes);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("drag-handle"),
                                 result.dragHandle, DragHandleModes);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed =
            assignEnum(QLatin1StringView("grapheme-width-method"),
                       result.graphemeWidthMethod, GraphemeWidthMethods);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("window-decoration"),
                                 result.windowDecoration, WindowDecorations);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("window-theme"),
                                 result.windowAppearance.theme, WindowThemes);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed = assignEnum(QLatin1StringView("alpha-blending"),
                                 result.alphaBlending, AlphaBlendingModes);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed =
            assignEnum(QLatin1StringView("window-subtitle"),
                       result.windowAppearance.subtitle, WindowSubtitles);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed = assignEnum(QLatin1StringView("fullscreen"),
                                 result.fullscreen, FullscreenModes);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("cursor-style"),
                                 result.appearance.cursorStyle, CursorStyles);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("confirm-close-surface"),
                                 result.confirmCloseMode, ConfirmCloseModes);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("copy-on-select"),
                                 result.selectionClipboard.copyOnSelect,
                                 CopyOnSelectModes);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("clipboard-write"),
                                 result.clipboardWrite, ClipboardAccessModes);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("clipboard-read"),
                                 result.clipboardRead, ClipboardAccessModes);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed =
            assign(QLatin1StringView("clipboard-write-limit-bytes"),
                   result.clipboardWriteLimitBytes, readOptionalDecimalUint64);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("right-click-action"),
                                 result.rightClickAction, RightClickActions);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("middle-click-action"),
                                 result.middleClickAction, MiddleClickActions);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed =
            assignEnum(QLatin1StringView("mouse-shift-capture"),
                       result.mouseShiftCapture, MouseShiftCaptureModes);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("link-previews"),
                                 result.linkPreviews, LinkPreviewModes);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("scrollbar"),
                                 result.scrollbar, ScrollbarPolicies);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("resize-overlay"),
                                 result.resizeOverlay.mode, ResizeOverlayModes);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed =
            assignEnum(QLatin1StringView("resize-overlay-position"),
                       result.resizeOverlay.position, ResizeOverlayPositions);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed =
            assignEnum(QLatin1StringView("gtk-single-instance"),
                       result.singleInstanceMode, SingleInstanceModes);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("linux-cgroup"),
                                 result.linuxCgroup.mode, LinuxCgroupModes);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("quick-terminal-position"),
                                 result.applicationShell.quickTerminal.position,
                                 QuickTerminalPositions);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed = assignEnum(QLatin1StringView("quick-terminal-screen"),
                                 result.applicationShell.quickTerminal.screen,
                                 QuickTerminalScreens);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed = assignEnum(
            QLatin1StringView("quick-terminal-keyboard-interactivity"),
            result.applicationShell.quickTerminal.keyboardInteractivity,
            QuickTerminalKeyboardModes);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed =
            assignEnum(QLatin1StringView("shell-integration"),
                       result.shellIntegration, ShellIntegrationModes);
        !parsed)
        return std::unexpected(std::move(parsed.error()));

    if (auto parsed = assign(QLatin1StringView("window-width"),
                             result.windowWidth, readUint32);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assign(QLatin1StringView("window-height"),
                             result.windowHeight, readUint32);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assign(QLatin1StringView("palette"),
                             result.appearance.palette, readPalette);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assign(QLatin1StringView("quick-terminal-size"),
                             result.applicationShell.quickTerminal.size,
                             readQuickTerminalSize);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    {
        constexpr QLatin1StringView name("window-title-font-family");
        auto parsed =
            readOptionalString(fieldValue(name), context(name), false);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.windowAppearance.titleFontFamily = std::move(*parsed);
    }
    if (auto parsed =
            assign(QLatin1StringView("window-titlebar-background"),
                   result.windowAppearance.titlebarBackground, readOptionalRgb);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed =
            assign(QLatin1StringView("window-titlebar-foreground"),
                   result.windowAppearance.titlebarForeground, readOptionalRgb);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }

    for (const auto [name, destination] :
         std::to_array<std::pair<QLatin1StringView, TerminalColorValue *>>({
             {QLatin1StringView("selection-foreground"),
              &result.appearance.selectionForeground},
             {QLatin1StringView("selection-background"),
              &result.appearance.selectionBackground},
             {QLatin1StringView("cursor-color"),
              &result.appearance.cursorColor},
             {QLatin1StringView("cursor-text"),
              &result.appearance.cursorTextColor},
         })) {
        auto parsed =
            readOptionalTerminalColor(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        *destination = *parsed;
    }
    for (const auto [name, destination] :
         std::to_array<std::pair<QLatin1StringView, TerminalColorValue *>>({
             {QLatin1StringView("search-foreground"),
              &result.appearance.searchForeground},
             {QLatin1StringView("search-background"),
              &result.appearance.searchBackground},
             {QLatin1StringView("search-selected-foreground"),
              &result.appearance.searchSelectedForeground},
             {QLatin1StringView("search-selected-background"),
              &result.appearance.searchSelectedBackground},
         })) {
        auto parsed = readTerminalColor(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        *destination = *parsed;
    }
    if (auto parsed =
            assign(QLatin1StringView("cursor-style-blink"),
                   result.appearance.cursorBlink, readOptionalBoolean);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    {
        constexpr QLatin1StringView name("cursor-opacity");
        auto parsed = readFiniteDouble(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.appearance.cursorOpacity = std::clamp(*parsed, 0.0, 1.0);
    }
    if (auto parsed = assign(QLatin1StringView("bold-color"),
                             result.appearance.boldColor, readBoldColor);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    {
        constexpr QLatin1StringView name("faint-opacity");
        auto parsed =
            readFiniteDouble(fieldValue(name), context(name), 0.0, 1.0);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.appearance.faintOpacity = *parsed;
    }
    {
        constexpr QLatin1StringView name("minimum-contrast");
        auto parsed =
            readFiniteDouble(fieldValue(name), context(name), 1.0, 21.0);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.appearance.minimumContrast = *parsed;
    }
    if (auto parsed =
            assign(QLatin1StringView("scrollback-limit-bytes"),
                   result.scrollbackLimitBytes, readOptionalDecimalUint64);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed =
            assign(QLatin1StringView("scrollback-limit-lines"),
                   result.scrollbackLimitLines, readOptionalDecimalUint64);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assign(QLatin1StringView("image-storage-limit"),
                             result.kittyImageStorageLimitBytes, readUint32);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignBoolean(QLatin1StringView("scrollback-compression"),
                                    result.scrollbackCompression);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assign(QLatin1StringView("linux-cgroup-memory-limit"),
                             result.linuxCgroup.memoryLimitBytes,
                             readOptionalDecimalUint64);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assign(QLatin1StringView("linux-cgroup-processes-limit"),
                             result.linuxCgroup.processesLimit,
                             readOptionalDecimalUint64);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assign(QLatin1StringView("shell-integration-features"),
                             result.shellIntegrationFeatures,
                             readShellIntegrationFeatures);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assign(QLatin1StringView("theme-files"),
                             result.themeFiles, readAbsolutePathList);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed =
            assign(QLatin1StringView("command-palette-entry"),
                   result.applicationShell.commandPalette, readCommandPalette);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed =
            assign(QLatin1StringView("app-notifications"),
                   result.applicationShell.notifications, readAppNotifications);
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }

    {
        constexpr QLatin1StringView name("quit-after-last-window-closed-delay");
        const QJsonValue delay = fieldValue(name);
        if (delay.isNull()) {
            result.quitAfterLastWindowClosedDelay.reset();
        } else {
            auto parsed = readUnsignedInteger<quint32>(delay, context(name));
            if (!parsed) return std::unexpected(std::move(parsed.error()));
            result.quitAfterLastWindowClosedDelay =
                std::chrono::milliseconds(*parsed);
        }
    }
    {
        constexpr QLatin1StringView name("resize-overlay-duration");
        auto parsed =
            readUnsignedInteger<quint32>(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.resizeOverlay.duration =
            std::chrono::milliseconds(std::max(*parsed, quint32{250}));
    }

    return result;
}

ParseResult<GhosttyKeybindTrigger> readTrigger(const QJsonValue &value,
                                               const QString &context)
{
    if (!value.isObject()) {
        return std::unexpected(
            QStringLiteral("%1 must be an object").arg(context));
    }
    const QJsonObject unvalidated = value.toObject();
    auto kind = readString(unvalidated.value(QStringLiteral("kind")),
                           childContext(context, QLatin1StringView("kind")));
    if (!kind) return std::unexpected(std::move(kind.error()));

    Fields fields;
    GhosttyKeybindKeyKind keyKind = GhosttyKeybindKeyKind::CatchAll;
    if (*kind == QStringLiteral("physical")) {
        fields = PhysicalTriggerFields;
        keyKind = GhosttyKeybindKeyKind::Physical;
    } else if (*kind == QStringLiteral("unicode")) {
        fields = UnicodeTriggerFields;
        keyKind = GhosttyKeybindKeyKind::Unicode;
    } else if (*kind == QStringLiteral("catch_all")) {
        fields = CatchAllTriggerFields;
    } else {
        return std::unexpected(
            QStringLiteral("%1.kind has unsupported value '%2'")
                .arg(context, *kind));
    }
    auto object = readExactObject(value, context, fields);
    if (!object) return std::unexpected(std::move(object.error()));

    auto modifiers = readUnsignedInteger<quint8>(
        object->value(QStringLiteral("mods")),
        childContext(context, QLatin1StringView("mods")),
        GhosttyKeybindShift | GhosttyKeybindCtrl | GhosttyKeybindAlt
            | GhosttyKeybindSuper);
    if (!modifiers) return std::unexpected(std::move(modifiers.error()));

    GhosttyKeybindTrigger result{
        .kind = keyKind,
        .physicalName = {},
        .unicodeCodepoint = 0,
        .modifiers = *modifiers,
    };
    if (keyKind == GhosttyKeybindKeyKind::Physical) {
        auto key =
            readNonEmptyString(object->value(QStringLiteral("key")),
                               childContext(context, QLatin1StringView("key")));
        if (!key) return std::unexpected(std::move(key.error()));
        result.physicalName = std::move(*key);
    } else if (keyKind == GhosttyKeybindKeyKind::Unicode) {
        auto codepoint = readUnsignedInteger<quint32>(
            object->value(QStringLiteral("codepoint")),
            childContext(context, QLatin1StringView("codepoint")), 0x10ffffU);
        if (!codepoint) return std::unexpected(std::move(codepoint.error()));
        if (*codepoint == 0U
            || (*codepoint >= 0xd800U && *codepoint <= 0xdfffU)) {
            return std::unexpected(
                QStringLiteral("%1.codepoint must be a nonzero Unicode scalar")
                    .arg(context));
        }
        result.unicodeCodepoint = *codepoint;
    }
    return result;
}

ParseResult<GhosttyKeybindFlags> readFlags(const QJsonValue &value,
                                           const QString &context)
{
    auto object = readExactObject(value, context, FlagFields);
    if (!object) return std::unexpected(std::move(object.error()));
    GhosttyKeybindFlags result;
    const auto read = [&object, &context](QLatin1StringView name) {
        return readBoolean(object->value(name), childContext(context, name));
    };
    auto consumed = read(QLatin1StringView("consumed"));
    if (!consumed) return std::unexpected(std::move(consumed.error()));
    auto all = read(QLatin1StringView("all"));
    if (!all) return std::unexpected(std::move(all.error()));
    auto global = read(QLatin1StringView("global"));
    if (!global) return std::unexpected(std::move(global.error()));
    auto performable = read(QLatin1StringView("performable"));
    if (!performable) return std::unexpected(std::move(performable.error()));
    result.consumed = *consumed;
    result.all = *all;
    result.global = *global;
    result.performable = *performable;
    return result;
}

ParseResult<GhosttyKeybindDefinition> readDefinition(const QJsonValue &value,
                                                     const QString &context)
{
    auto object = readExactObject(value, context, DefinitionFields);
    if (!object) return std::unexpected(std::move(object.error()));
    auto sequence =
        readArray(object->value(QStringLiteral("sequence")),
                  childContext(context, QLatin1StringView("sequence")));
    if (!sequence) return std::unexpected(std::move(sequence.error()));
    if (sequence->isEmpty()) {
        return std::unexpected(
            QStringLiteral("%1.sequence must be a non-empty array")
                .arg(context));
    }
    auto actions =
        readArray(object->value(QStringLiteral("actions")),
                  childContext(context, QLatin1StringView("actions")));
    if (!actions) return std::unexpected(std::move(actions.error()));
    if (actions->isEmpty()) {
        return std::unexpected(
            QStringLiteral("%1.actions must be a non-empty array")
                .arg(context));
    }

    GhosttyKeybindDefinition result;
    result.sequence.reserve(sequence->size());
    for (qsizetype index = 0; index < sequence->size(); ++index) {
        auto trigger = readTrigger(
            sequence->at(index),
            QStringLiteral("%1.sequence[%2]").arg(context).arg(index));
        if (!trigger) return std::unexpected(std::move(trigger.error()));
        result.sequence.append(std::move(*trigger));
    }
    result.actions.reserve(actions->size());
    for (qsizetype index = 0; index < actions->size(); ++index) {
        auto action = readNonEmptyString(
            actions->at(index),
            QStringLiteral("%1.actions[%2]").arg(context).arg(index));
        if (!action) return std::unexpected(std::move(action.error()));
        result.actions.append(std::move(*action));
    }
    auto flags = readFlags(object->value(QStringLiteral("flags")),
                           childContext(context, QLatin1StringView("flags")));
    if (!flags) return std::unexpected(std::move(flags.error()));
    if ((flags->all || flags->global) && result.sequence.size() != 1) {
        return std::unexpected(
            QStringLiteral(
                "%1 all/global binding must contain exactly one trigger")
                .arg(context));
    }
    result.flags = *flags;
    return result;
}

ParseResult<QVector<GhosttyKeybindDefinition>>
readDefinitions(const QJsonValue &value, const QString &context)
{
    auto array = readArray(value, context);
    if (!array) return std::unexpected(std::move(array.error()));
    QVector<GhosttyKeybindDefinition> result;
    result.reserve(array->size());
    for (qsizetype index = 0; index < array->size(); ++index) {
        auto definition = readDefinition(
            array->at(index), QStringLiteral("%1[%2]").arg(context).arg(index));
        if (!definition) {
            return std::unexpected(std::move(definition.error()));
        }
        const bool duplicate = std::ranges::any_of(
            result, [&definition](const GhosttyKeybindDefinition &existing) {
                return existing.sequence == definition->sequence;
            });
        if (duplicate) {
            return std::unexpected(
                QStringLiteral("%1 contains a duplicate trigger sequence")
                    .arg(context));
        }
        result.append(std::move(*definition));
    }
    return result;
}

ParseResult<GhosttyKeybindConfig> readKeybindings(const QJsonValue &value,
                                                  const QString &context)
{
    auto object = readExactObject(value, context, KeybindingFields);
    if (!object) return std::unexpected(std::move(object.error()));
    auto root =
        readDefinitions(object->value(QStringLiteral("root")),
                        childContext(context, QLatin1StringView("root")));
    if (!root) return std::unexpected(std::move(root.error()));
    auto tables = readArray(object->value(QStringLiteral("tables")),
                            childContext(context, QLatin1StringView("tables")));
    if (!tables) return std::unexpected(std::move(tables.error()));

    GhosttyKeybindConfig result;
    result.root = std::move(*root);
    result.tables.reserve(tables->size());
    for (qsizetype index = 0; index < tables->size(); ++index) {
        const QString tableContext =
            QStringLiteral("%1.tables[%2]").arg(context).arg(index);
        auto table =
            readExactObject(tables->at(index), tableContext, TableFields);
        if (!table) return std::unexpected(std::move(table.error()));
        auto name = readNonEmptyString(
            table->value(QStringLiteral("name")),
            childContext(tableContext, QLatin1StringView("name")));
        if (!name) return std::unexpected(std::move(name.error()));
        if (std::ranges::any_of(result.tables, [&name](const auto &existing) {
                return existing.name == *name;
            })) {
            return std::unexpected(
                QStringLiteral("Duplicate Ghostty keybinding table '%1'")
                    .arg(*name));
        }
        auto bindings = readDefinitions(
            table->value(QStringLiteral("bindings")),
            childContext(tableContext, QLatin1StringView("bindings")));
        if (!bindings) return std::unexpected(std::move(bindings.error()));
        result.tables.append({
            .name = std::move(*name),
            .bindings = std::move(*bindings),
        });
    }
    return result;
}

} // namespace

std::expected<GhosttyConfigExport, QString>
parseGhosttyConfigExportJson(const QByteArray &json)
{
    QJsonParseError jsonError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &jsonError);
    if (jsonError.error != QJsonParseError::NoError) {
        return std::unexpected(
            QStringLiteral(
                "Invalid Ghostty structured config JSON at offset %1: %2")
                .arg(jsonError.offset)
                .arg(jsonError.errorString()));
    }
    if (!document.isObject()) {
        return std::unexpected(QStringLiteral(
            "Ghostty structured config JSON root must be an object"));
    }

    const QString rootContext =
        QStringLiteral("Ghostty structured config JSON root");
    auto root = readExactObject(document.object(), rootContext, RootFields);
    if (!root) return std::unexpected(std::move(root.error()));
    auto version = readUnsignedInteger<quint32>(
        root->value(QStringLiteral("version")),
        childContext(rootContext, QLatin1StringView("version")));
    if (!version || *version != GhosttyConfigExport::CurrentSchemaVersion) {
        return std::unexpected(QStringLiteral(
            "Unsupported Ghostty structured config JSON schema version"));
    }

    auto values = readValues(root->value(QStringLiteral("values")));
    if (!values) return std::unexpected(std::move(values.error()));
    auto keybindings =
        readKeybindings(root->value(QStringLiteral("keybindings")),
                        QStringLiteral("keybindings"));
    if (!keybindings) {
        return std::unexpected(std::move(keybindings.error()));
    }
    auto defaultKeybindings =
        readKeybindings(root->value(QStringLiteral("default-keybindings")),
                        QStringLiteral("default-keybindings"));
    if (!defaultKeybindings) {
        return std::unexpected(std::move(defaultKeybindings.error()));
    }

    return GhosttyConfigExport(std::move(*values), std::move(*keybindings),
                               std::move(*defaultKeybindings));
}
