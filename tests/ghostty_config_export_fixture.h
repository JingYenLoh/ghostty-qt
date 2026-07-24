#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <utility>

namespace GhosttyConfigExportFixture {

inline QJsonObject flags(bool consumed = true,
                         bool all = false,
                         bool global = false,
                         bool performable = false)
{
    return {
        {QStringLiteral("consumed"), consumed},
        {QStringLiteral("all"), all},
        {QStringLiteral("global"), global},
        {QStringLiteral("performable"), performable},
    };
}

inline QJsonObject unicodeTrigger(int codepoint, int modifiers = 0)
{
    return {
        {QStringLiteral("kind"), QStringLiteral("unicode")},
        {QStringLiteral("codepoint"), codepoint},
        {QStringLiteral("mods"), modifiers},
    };
}

inline QJsonObject physicalTrigger(const QString &key, int modifiers = 0)
{
    return {
        {QStringLiteral("kind"), QStringLiteral("physical")},
        {QStringLiteral("key"), key},
        {QStringLiteral("mods"), modifiers},
    };
}

inline QJsonObject catchAllTrigger(int modifiers = 0)
{
    return {
        {QStringLiteral("kind"), QStringLiteral("catch_all")},
        {QStringLiteral("mods"), modifiers},
    };
}

inline QJsonObject binding(QJsonArray sequence,
                           const QStringList &actions,
                           QJsonObject bindingFlags = flags())
{
    QJsonArray serializedActions;
    for (const QString &action : actions) serializedActions.append(action);
    return {
        {QStringLiteral("sequence"), std::move(sequence)},
        {QStringLiteral("actions"), std::move(serializedActions)},
        {QStringLiteral("flags"), std::move(bindingFlags)},
    };
}

inline QJsonObject keybindings()
{
    const QJsonObject rootBinding = binding(
        {unicodeTrigger('t', 3)},
        {QStringLiteral("toggle_command_palette")});
    return {
        {QStringLiteral("root"), QJsonArray{rootBinding}},
        {QStringLiteral("tables"), QJsonArray{}},
    };
}

inline QJsonObject automaticFontStyle()
{
    return {{QStringLiteral("kind"), QStringLiteral("automatic")}};
}

inline QJsonObject disabledFontStyle()
{
    return {{QStringLiteral("kind"), QStringLiteral("disabled")}};
}

inline QJsonObject namedFontStyle(const QString &name)
{
    return {
        {QStringLiteral("kind"), QStringLiteral("named")},
        {QStringLiteral("name"), name},
    };
}

inline QJsonObject absoluteMetricModifier(int pixels)
{
    return {
        {QStringLiteral("kind"), QStringLiteral("absolute")},
        {QStringLiteral("value"), pixels},
    };
}

inline QJsonObject percentageMetricModifier(double multiplier)
{
    return {
        {QStringLiteral("kind"), QStringLiteral("percentage")},
        {QStringLiteral("value"), multiplier},
    };
}

inline QJsonArray metricModifierOrder()
{
    return {
        QStringLiteral("adjust-cursor-height"),
        QStringLiteral("adjust-cell-width"),
        QStringLiteral("adjust-overline-position"),
        QStringLiteral("adjust-strikethrough-position"),
        QStringLiteral("adjust-cell-height"),
        QStringLiteral("adjust-cursor-thickness"),
        QStringLiteral("adjust-font-baseline"),
        QStringLiteral("adjust-strikethrough-thickness"),
        QStringLiteral("adjust-underline-position"),
    };
}

inline QJsonObject bellFeatures(bool system = false, bool audio = false,
                                bool attention = true, bool title = true,
                                bool border = false)
{
    return {
        {QStringLiteral("system"), system},
        {QStringLiteral("audio"), audio},
        {QStringLiteral("attention"), attention},
        {QStringLiteral("title"), title},
        {QStringLiteral("border"), border},
    };
}

inline QJsonObject finalizedConfigPath(const QString &path,
                                       bool optional = false)
{
    return {
        {QStringLiteral("path"), path},
        {QStringLiteral("optional"), optional},
    };
}

inline QJsonObject mouseScrollMultiplier(double precision = 1.0,
                                         double discrete = 3.0)
{
    return {
        {QStringLiteral("precision"), precision},
        {QStringLiteral("discrete"), discrete},
    };
}

inline QJsonObject values()
{
    QJsonArray palette;
    for (int index = 0; index < 256; ++index) {
        palette.append(QStringLiteral("#%1%1%1")
                           .arg(index, 2, 16, QLatin1Char('0')));
    }

    return {
        {QStringLiteral("working-directory"), QStringLiteral("/work/ghostty")},
        {QStringLiteral("font-family"),
         QJsonArray{QStringLiteral("Mono One"), QStringLiteral("Emoji")}},
        {QStringLiteral("font-family-bold"),
         QJsonArray{QStringLiteral("Mono Bold"),
                    QStringLiteral("Bold Fallback")}},
        {QStringLiteral("font-family-italic"),
         QJsonArray{QStringLiteral("Mono Italic")}},
        {QStringLiteral("font-family-bold-italic"), QJsonArray{}},
        {QStringLiteral("font-size"), 13.5},
        {QStringLiteral("font-style"), automaticFontStyle()},
        {QStringLiteral("font-style-bold"), disabledFontStyle()},
        {QStringLiteral("font-style-italic"),
         namedFontStyle(QStringLiteral("Book Italic"))},
        {QStringLiteral("font-style-bold-italic"),
         namedFontStyle(QStringLiteral("Extra Bold Italic"))},
        {QStringLiteral("adjust-cell-width"), absoluteMetricModifier(2)},
        {QStringLiteral("adjust-cell-height"), absoluteMetricModifier(-3)},
        {QStringLiteral("adjust-font-baseline"),
         percentageMetricModifier(1.25)},
        {QStringLiteral("adjust-underline-position"),
         percentageMetricModifier(0.8)},
        {QStringLiteral("adjust-underline-thickness"), QJsonValue::Null},
        {QStringLiteral("adjust-strikethrough-position"),
         absoluteMetricModifier(4)},
        {QStringLiteral("adjust-strikethrough-thickness"),
         absoluteMetricModifier(-2)},
        {QStringLiteral("adjust-overline-position"),
         percentageMetricModifier(1.5)},
        {QStringLiteral("adjust-overline-thickness"), QJsonValue::Null},
        {QStringLiteral("adjust-cursor-thickness"),
         percentageMetricModifier(0.5)},
        {QStringLiteral("adjust-cursor-height"), absoluteMetricModifier(6)},
        {QStringLiteral("metric-modifier-order"), metricModifierOrder()},
        {QStringLiteral("foreground"), QStringLiteral("#112233")},
        {QStringLiteral("background"), QStringLiteral("#445566")},
        {QStringLiteral("unfocused-split-opacity"), 0.7},
        {QStringLiteral("unfocused-split-fill"), QJsonValue::Null},
        {QStringLiteral("split-divider-color"), QStringLiteral("#778899")},
        {QStringLiteral("split-inherit-working-directory"), false},
        {QStringLiteral("split-preserve-zoom"), true},
        {QStringLiteral("tab-inherit-working-directory"), false},
        {QStringLiteral("window-inherit-working-directory"), true},
        {QStringLiteral("window-inherit-font-size"), false},
        {QStringLiteral("window-new-tab-position"), QStringLiteral("end")},
        {QStringLiteral("window-show-tab-bar"), QStringLiteral("always")},
        {QStringLiteral("window-decoration"), QStringLiteral("server")},
        {QStringLiteral("window-width"), 120},
        {QStringLiteral("window-height"), 40},
        {QStringLiteral("maximize"), true},
        {QStringLiteral("fullscreen"), QStringLiteral("non-native")},
        {QStringLiteral("palette"), std::move(palette)},
        {QStringLiteral("selection-foreground"), QJsonValue::Null},
        {QStringLiteral("selection-background"),
         QStringLiteral("cell-foreground")},
        {QStringLiteral("search-foreground"), QStringLiteral("#010203")},
        {QStringLiteral("search-background"),
         QStringLiteral("cell-background")},
        {QStringLiteral("search-selected-foreground"),
         QStringLiteral("cell-foreground")},
        {QStringLiteral("search-selected-background"),
         QStringLiteral("#aabbcc")},
        {QStringLiteral("cursor-color"), QStringLiteral("#abcdef")},
        {QStringLiteral("cursor-opacity"), 0.625},
        {QStringLiteral("cursor-style"), QStringLiteral("block_hollow")},
        {QStringLiteral("cursor-style-blink"), QJsonValue::Null},
        {QStringLiteral("cursor-text"), QStringLiteral("cell-background")},
        {QStringLiteral("bold-color"), QStringLiteral("bright")},
        {QStringLiteral("faint-opacity"), 0.375},
        {QStringLiteral("scrollback-limit"),
         QStringLiteral("18446744073709551615")},
        {QStringLiteral("scrollbar"), QStringLiteral("never")},
        {QStringLiteral("bell-features"),
         bellFeatures(true, true, false, false, true)},
        {QStringLiteral("bell-audio-path"),
         finalizedConfigPath(QStringLiteral("/work/bell.oga"))},
        {QStringLiteral("bell-audio-volume"), 0.625},
        {QStringLiteral("confirm-close-surface"), QStringLiteral("always")},
        {QStringLiteral("clipboard-trim-trailing-spaces"), false},
        {QStringLiteral("clipboard-paste-protection"), false},
        {QStringLiteral("clipboard-paste-bracketed-safe"), true},
        {QStringLiteral("copy-on-select"), QStringLiteral("clipboard")},
        {QStringLiteral("selection-clear-on-typing"), false},
        {QStringLiteral("selection-clear-on-copy"), true},
        {QStringLiteral("middle-click-action"), QStringLiteral("ignore")},
        {QStringLiteral("mouse-reporting"), false},
        {QStringLiteral("mouse-scroll-multiplier"),
         mouseScrollMultiplier(0.75, 4.5)},
        {QStringLiteral("link-url"), false},
        {QStringLiteral("link-previews"), QStringLiteral("osc8")},
        {QStringLiteral("config-file"),
         QJsonArray{QStringLiteral("/work/include.ghostty"),
                    QStringLiteral("?/work/optional.ghostty")}},
        {QStringLiteral("quit-after-last-window-closed"), false},
        {QStringLiteral("quit-after-last-window-closed-delay"),
         QJsonValue::Null},
        {QStringLiteral("initial-window"), false},
        {QStringLiteral("resize-overlay"), QStringLiteral("always")},
        {QStringLiteral("resize-overlay-position"),
         QStringLiteral("bottom-right")},
        {QStringLiteral("resize-overlay-duration"), 1250},
        {QStringLiteral("gtk-single-instance"), QStringLiteral("detect")},
    };
}

inline QJsonObject object()
{
    const QJsonObject baseline = keybindings();
    return {
        {QStringLiteral("version"), 1},
        {QStringLiteral("values"), values()},
        {QStringLiteral("keybindings"), baseline},
        {QStringLiteral("default-keybindings"), baseline},
    };
}

inline QByteArray json(const QJsonObject &exportObject = object())
{
    return QJsonDocument(exportObject).toJson(QJsonDocument::Compact);
}

} // namespace GhosttyConfigExportFixture
