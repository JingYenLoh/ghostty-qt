#pragma once

#include <QByteArrayView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <utility>

namespace GhosttyConfigExportFixture {

inline QJsonArray bytes(QByteArrayView value)
{
    QJsonArray result;
    for (const char byte : value) {
        result.append(static_cast<unsigned char>(byte));
    }
    return result;
}

inline QJsonObject shellCommand(QByteArrayView value, bool defaultShell = false)
{
    return {
        {QStringLiteral("kind"), QStringLiteral("shell")},
        {QStringLiteral("value"), bytes(value)},
        {QStringLiteral("default-shell"), defaultShell},
    };
}

inline QJsonObject directCommand(QJsonArray arguments,
                                 bool defaultShell = false)
{
    return {
        {QStringLiteral("kind"), QStringLiteral("direct")},
        {QStringLiteral("argv"), std::move(arguments)},
        {QStringLiteral("default-shell"), defaultShell},
    };
}

inline QJsonObject environmentEntry(QByteArrayView key, QByteArrayView value)
{
    return {
        {QStringLiteral("key"), bytes(key)},
        {QStringLiteral("value"), bytes(value)},
    };
}

inline QJsonObject initialInput(const QString &kind, QByteArrayView value)
{
    return {
        {QStringLiteral("kind"), kind},
        {QStringLiteral("value"), bytes(value)},
    };
}

inline QJsonObject sidedModifier(const QString &modifier, const QString &side)
{
    return {
        {QStringLiteral("modifier"), modifier},
        {QStringLiteral("side"), side},
    };
}

inline QJsonObject modifierRemap(QJsonObject from, QJsonObject to)
{
    return {
        {QStringLiteral("from"), std::move(from)},
        {QStringLiteral("to"), std::move(to)},
    };
}

inline QJsonObject quickTerminalPixels(quint32 value)
{
    return {
        {QStringLiteral("kind"), QStringLiteral("pixels")},
        {QStringLiteral("value"), static_cast<qint64>(value)},
    };
}

inline QJsonObject quickTerminalPercentage(const QString &valueBits)
{
    return {
        {QStringLiteral("kind"), QStringLiteral("percentage")},
        {QStringLiteral("value-bits"), valueBits},
    };
}

inline QJsonObject quickTerminalSize(QJsonValue primary = QJsonValue::Null,
                                     QJsonValue secondary = QJsonValue::Null)
{
    return {
        {QStringLiteral("primary"), std::move(primary)},
        {QStringLiteral("secondary"), std::move(secondary)},
    };
}

inline QJsonObject commandPaletteEntry(const QString &title,
                                       const QString &description,
                                       const QString &actionKey,
                                       const QString &action)
{
    return {
        {QStringLiteral("title"), title},
        {QStringLiteral("description"), description},
        {QStringLiteral("action-key"), actionKey},
        {QStringLiteral("action"), action},
    };
}

inline QJsonObject appNotifications(bool clipboardCopy, bool configReload)
{
    return {
        {QStringLiteral("clipboard-copy"), clipboardCopy},
        {QStringLiteral("config-reload"), configReload},
    };
}

inline QJsonObject flags(bool consumed = true, bool all = false,
                         bool global = false, bool performable = false)
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

inline QJsonObject binding(QJsonArray sequence, const QStringList &actions,
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
        {unicodeTrigger('t', 3)}, {QStringLiteral("toggle_command_palette")});
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

inline QJsonObject fontFeature(quint32 tag, quint32 value)
{
    return {
        {QStringLiteral("tag"), static_cast<qint64>(tag)},
        {QStringLiteral("value"), static_cast<qint64>(value)},
    };
}

inline QJsonObject fontVariation(quint32 tag, const QString &valueBits)
{
    return {
        {QStringLiteral("tag"), static_cast<qint64>(tag)},
        {QStringLiteral("value-bits"), valueBits},
    };
}

inline QJsonObject codepointFontMap(quint32 first, quint32 last,
                                    const QString &family)
{
    return {
        {QStringLiteral("first"), static_cast<qint64>(first)},
        {QStringLiteral("last"), static_cast<qint64>(last)},
        {QStringLiteral("family"), family},
    };
}

inline QJsonObject clipboardCodepointReplacement(quint32 codepoint)
{
    return {
        {QStringLiteral("kind"), QStringLiteral("codepoint")},
        {QStringLiteral("value"), static_cast<qint64>(codepoint)},
    };
}

inline QJsonObject clipboardTextReplacement(const QString &text)
{
    return {
        {QStringLiteral("kind"), QStringLiteral("text")},
        {QStringLiteral("value"), text},
    };
}

inline QJsonObject clipboardCodepointMap(quint32 first, quint32 last,
                                         QJsonObject replacement)
{
    return {
        {QStringLiteral("first"), static_cast<qint64>(first)},
        {QStringLiteral("last"), static_cast<qint64>(last)},
        {QStringLiteral("replacement"), std::move(replacement)},
    };
}

inline QJsonObject syntheticStyle(bool bold, bool italic, bool boldItalic)
{
    return {
        {QStringLiteral("bold"), bold},
        {QStringLiteral("italic"), italic},
        {QStringLiteral("bold-italic"), boldItalic},
    };
}

inline QJsonObject freetypeLoadFlags(bool hinting, bool forceAutohint,
                                     bool monochrome, bool autohint, bool light)
{
    return {
        {QStringLiteral("hinting"), hinting},
        {QStringLiteral("force-autohint"), forceAutohint},
        {QStringLiteral("monochrome"), monochrome},
        {QStringLiteral("autohint"), autohint},
        {QStringLiteral("light"), light},
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

inline QJsonObject scrollToBottom(bool keystroke = true, bool output = false)
{
    return {
        {QStringLiteral("keystroke"), keystroke},
        {QStringLiteral("output"), output},
    };
}

inline QJsonObject
shellIntegrationFeatures(bool cursor = false, bool sudo = true,
                         bool title = false, bool sshEnvironment = true,
                         bool sshTerminfo = true, bool path = false)
{
    return {
        {QStringLiteral("cursor"), cursor},
        {QStringLiteral("sudo"), sudo},
        {QStringLiteral("title"), title},
        {QStringLiteral("ssh-env"), sshEnvironment},
        {QStringLiteral("ssh-terminfo"), sshTerminfo},
        {QStringLiteral("path"), path},
    };
}

inline QJsonObject values()
{
    QJsonArray palette;
    for (int index = 0; index < 256; ++index) {
        palette.append(
            QStringLiteral("#%1%1%1").arg(index, 2, 16, QLatin1Char('0')));
    }

    return {
        {QStringLiteral("term"), bytes(QByteArrayLiteral("ghostty-qt-test"))},
        {QStringLiteral("enquiry-response"),
         bytes(QByteArray::fromHex("000580ff"))},
        {QStringLiteral("command"),
         shellCommand(QByteArrayLiteral("/bin/fixture-shell"), true)},
        {QStringLiteral("initial-command"),
         directCommand({bytes(QByteArrayLiteral("/bin/printf")),
                        bytes(QByteArray::fromHex("80ff")),
                        bytes(QByteArrayView{})})},
        {QStringLiteral("input"),
         QJsonArray{
             initialInput(QStringLiteral("raw"),
                          QByteArray::fromHex("68656c6c6f0a00ff")),
             initialInput(QStringLiteral("path"),
                          QByteArray::fromHex("2f776f726b2f696e707574ff")),
         }},
        {QStringLiteral("key-remap"),
         QJsonArray{
             modifierRemap(
                 sidedModifier(QStringLiteral("ctrl"), QStringLiteral("right")),
                 sidedModifier(QStringLiteral("super"),
                               QStringLiteral("left"))),
             modifierRemap(
                 sidedModifier(QStringLiteral("ctrl"), QStringLiteral("left")),
                 sidedModifier(QStringLiteral("alt"), QStringLiteral("right"))),
             modifierRemap(
                 sidedModifier(QStringLiteral("shift"), QStringLiteral("left")),
                 sidedModifier(QStringLiteral("ctrl"),
                               QStringLiteral("right"))),
         }},
        {QStringLiteral("wait-after-command"), true},
        {QStringLiteral("abnormal-command-exit-runtime"), 731},
        {QStringLiteral("shell-integration"), QStringLiteral("fish")},
        {QStringLiteral("shell-integration-features"),
         shellIntegrationFeatures()},
        {QStringLiteral("env"),
         QJsonArray{
             environmentEntry(QByteArrayLiteral("GHOSTTY_QT_TEST"),
                              QByteArrayLiteral("alpha=beta")),
             environmentEntry(QByteArray::fromHex("80ff"),
                              QByteArray::fromHex("fe8176616c7565")),
             environmentEntry(QByteArrayView{},
                              QByteArrayLiteral("empty-key-value")),
         }},
        {QStringLiteral("linux-cgroup"), QStringLiteral("always")},
        {QStringLiteral("linux-cgroup-memory-limit"),
         QStringLiteral("18446744073709551615")},
        {QStringLiteral("linux-cgroup-processes-limit"), QStringLiteral("0")},
        {QStringLiteral("linux-cgroup-hard-fail"), true},
        {QStringLiteral("working-directory"),
         bytes(QByteArrayLiteral("/work/ghostty"))},
        {QStringLiteral("title"), QString{}},
        {QStringLiteral("class"),
         bytes(QByteArray::fromHex("636f6d2e6578616d706c652eff"))},
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
        {QStringLiteral("font-feature"),
         QJsonArray{
             fontFeature(1818847073U, 1U),
             fontFeature(1667329140U, 0U),
             fontFeature(1668689969U, 2U),
             fontFeature(1667329140U, 1U),
         }},
        {QStringLiteral("font-variation"),
         QJsonArray{
             fontVariation(2003265652U, QStringLiteral("4636737291354636288")),
             fontVariation(2003265652U, QStringLiteral("4641240890982006784")),
         }},
        {QStringLiteral("font-variation-bold"),
         QJsonArray{fontVariation(2003265652U,
                                  QStringLiteral("4649368480934526976"))}},
        {QStringLiteral("font-variation-italic"),
         QJsonArray{fontVariation(1936486004U,
                                  QStringLiteral("13846598529327300608"))}},
        {QStringLiteral("font-variation-bold-italic"), QJsonArray{}},
        {QStringLiteral("font-codepoint-map"),
         QJsonArray{
             codepointFontMap(0x2500U, 0x257fU, QStringLiteral("Symbols One")),
             codepointFontMap(0x2500U, 0x2500U,
                              QStringLiteral("Symbols Override")),
             codepointFontMap(0x110000U, 0x1fffffU, QString{}),
         }},
        {QStringLiteral("font-synthetic-style"),
         syntheticStyle(false, true, false)},
        {QStringLiteral("font-shaping-break"),
         QJsonObject{{QStringLiteral("cursor"), false}}},
        {QStringLiteral("freetype-load-flags"),
         freetypeLoadFlags(false, true, true, false, false)},
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
        {QStringLiteral("alpha-blending"), QStringLiteral("linear")},
        {QStringLiteral("background-opacity"), 0.375},
        {QStringLiteral("background-opacity-cells"), true},
        {QStringLiteral("background-blur"), -2},
        {QStringLiteral("background-image"),
         QJsonObject{
             {QStringLiteral("path"),
              QStringLiteral("/fixture/background.png")},
             {QStringLiteral("optional"), true},
         }},
        {QStringLiteral("background-image-opacity"), 1.25},
        {QStringLiteral("background-image-position"),
         QStringLiteral("bottom-right")},
        {QStringLiteral("background-image-fit"), QStringLiteral("cover")},
        {QStringLiteral("background-image-repeat"), true},
        {QStringLiteral("custom-shader"),
         QJsonArray{
             finalizedConfigPath(QStringLiteral("/work/shaders/first.glsl")),
             finalizedConfigPath(QStringLiteral("/work/shaders/optional.glsl"),
                                 true),
         }},
        {QStringLiteral("custom-shader-animation"), QStringLiteral("always")},
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
        {QStringLiteral("window-theme"), QStringLiteral("ghostty")},
        {QStringLiteral("window-title-font-family"),
         QStringLiteral("Window Sans")},
        {QStringLiteral("window-titlebar-background"),
         QStringLiteral("#123456")},
        {QStringLiteral("window-titlebar-foreground"),
         QStringLiteral("#fedcba")},
        {QStringLiteral("window-subtitle"),
         QStringLiteral("working-directory")},
        {QStringLiteral("window-width"), 120},
        {QStringLiteral("window-height"), 40},
        {QStringLiteral("window-padding-x"), QJsonArray{3, 5}},
        {QStringLiteral("window-padding-y"), QJsonArray{7, 11}},
        {QStringLiteral("window-padding-balance"), QStringLiteral("equal")},
        {QStringLiteral("window-padding-color"),
         QStringLiteral("extend-always")},
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
        {QStringLiteral("minimum-contrast"), 4.25},
        {QStringLiteral("vt-kam-allowed"), true},
        {QStringLiteral("scrollback-limit-bytes"),
         QStringLiteral("18446744073709551615")},
        {QStringLiteral("scrollback-limit-lines"),
         QStringLiteral("9876543210")},
        {QStringLiteral("image-storage-limit"), 123456789},
        {QStringLiteral("scrollback-compression"), false},
        {QStringLiteral("scrollbar"), QStringLiteral("never")},
        {QStringLiteral("bell-features"),
         bellFeatures(true, true, false, false, true)},
        {QStringLiteral("bell-audio-path"),
         finalizedConfigPath(QStringLiteral("/work/bell.oga"))},
        {QStringLiteral("bell-audio-volume"), 0.625},
        {QStringLiteral("confirm-close-surface"), QStringLiteral("always")},
        {QStringLiteral("clipboard-trim-trailing-spaces"), false},
        {QStringLiteral("clipboard-codepoint-map"),
         QJsonArray{
             clipboardCodepointMap(
                 0x2500U, 0x257fU,
                 clipboardCodepointReplacement(static_cast<quint32>('-'))),
             clipboardCodepointMap(
                 0x2500U, 0x2500U,
                 clipboardTextReplacement(QStringLiteral("line"))),
             clipboardCodepointMap(0x1f642U, 0x1f642U,
                                   clipboardCodepointReplacement(0x1f47bU)),
             clipboardCodepointMap(0x200bU, 0x200bU,
                                   clipboardTextReplacement(QString{})),
             clipboardCodepointMap(0x110000U, 0x1fffffU,
                                   clipboardCodepointReplacement(0x1fffffU)),
         }},
        {QStringLiteral("clipboard-write"), QStringLiteral("ask")},
        {QStringLiteral("clipboard-paste-protection"), false},
        {QStringLiteral("clipboard-paste-bracketed-safe"), true},
        {QStringLiteral("copy-on-select"), QStringLiteral("clipboard")},
        {QStringLiteral("selection-clear-on-typing"), false},
        {QStringLiteral("selection-clear-on-copy"), true},
        {QStringLiteral("selection-word-chars"),
         QJsonArray{0, 0x20, 0x2502, 0x1f642}},
        {QStringLiteral("click-repeat-interval"), 731},
        {QStringLiteral("right-click-action"), QStringLiteral("copy-or-paste")},
        {QStringLiteral("middle-click-action"), QStringLiteral("ignore")},
        {QStringLiteral("mouse-reporting"), false},
        {QStringLiteral("mouse-shift-capture"), QStringLiteral("never")},
        {QStringLiteral("mouse-hide-while-typing"), true},
        {QStringLiteral("scroll-to-bottom"), scrollToBottom(false, true)},
        {QStringLiteral("focus-follows-mouse"), true},
        {QStringLiteral("mouse-scroll-multiplier"),
         mouseScrollMultiplier(0.75, 4.5)},
        {QStringLiteral("link-url"), false},
        {QStringLiteral("link-previews"), QStringLiteral("osc8")},
        {QStringLiteral("config-file"),
         QJsonArray{QStringLiteral("/work/include.ghostty"),
                    QStringLiteral("?/work/optional.ghostty")}},
        {QStringLiteral("config-default-files"), false},
        {QStringLiteral("theme-files"),
         QJsonArray{QStringLiteral("/work/themes/light"),
                    QStringLiteral("/work/themes/dark")}},
        {QStringLiteral("quit-after-last-window-closed"), false},
        {QStringLiteral("quit-after-last-window-closed-delay"),
         QJsonValue::Null},
        {QStringLiteral("initial-window"), false},
        {QStringLiteral("resize-overlay"), QStringLiteral("always")},
        {QStringLiteral("resize-overlay-position"),
         QStringLiteral("bottom-right")},
        {QStringLiteral("resize-overlay-duration"), 1250},
        {QStringLiteral("gtk-single-instance"), QStringLiteral("detect")},
        {QStringLiteral("quick-terminal-position"), QStringLiteral("center")},
        {QStringLiteral("quick-terminal-size"),
         quickTerminalSize(
             quickTerminalPercentage(QStringLiteral("1108738048")),
             quickTerminalPixels(640))},
        {QStringLiteral("quick-terminal-screen"), QStringLiteral("mouse")},
        {QStringLiteral("quick-terminal-autohide"), true},
        {QStringLiteral("quick-terminal-keyboard-interactivity"),
         QStringLiteral("exclusive")},
        {QStringLiteral("command-palette-entry"),
         QJsonArray{
             commandPaletteEntry(
                 QStringLiteral("Reset"), QStringLiteral("Reset everything"),
                 QStringLiteral("reset"), QStringLiteral("reset")),
             commandPaletteEntry(QStringLiteral("Say 👻"), QString{},
                                 QStringLiteral("text"),
                                 QStringLiteral("text:hello, world")),
         }},
        {QStringLiteral("app-notifications"), appNotifications(false, true)},
    };
}

inline QJsonObject object()
{
    const QJsonObject baseline = keybindings();
    return {
        {QStringLiteral("version"), 2},
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
