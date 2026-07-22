#include "ghostty_config_export.h"

#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <limits>
#include <ranges>
#include <span>
#include <utility>

namespace {

template<typename Value>
using ParseResult = std::expected<Value, QString>;

using Fields = std::span<const QLatin1StringView>;

constexpr auto RootFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("version"),
    QLatin1StringView("values"),
    QLatin1StringView("keybindings"),
    QLatin1StringView("default-keybindings"),
});

constexpr auto ValueFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("working-directory"),
    QLatin1StringView("font-family"),
    QLatin1StringView("font-size"),
    QLatin1StringView("foreground"),
    QLatin1StringView("background"),
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
    QLatin1StringView("window-width"),
    QLatin1StringView("window-height"),
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
    QLatin1StringView("scrollback-limit"),
    QLatin1StringView("confirm-close-surface"),
    QLatin1StringView("clipboard-trim-trailing-spaces"),
    QLatin1StringView("clipboard-paste-protection"),
    QLatin1StringView("clipboard-paste-bracketed-safe"),
    QLatin1StringView("copy-on-select"),
    QLatin1StringView("selection-clear-on-typing"),
    QLatin1StringView("selection-clear-on-copy"),
    QLatin1StringView("middle-click-action"),
    QLatin1StringView("mouse-reporting"),
    QLatin1StringView("link-url"),
    QLatin1StringView("link-previews"),
    QLatin1StringView("config-file"),
    QLatin1StringView("quit-after-last-window-closed"),
    QLatin1StringView("quit-after-last-window-closed-delay"),
    QLatin1StringView("initial-window"),
    QLatin1StringView("resize-overlay"),
    QLatin1StringView("resize-overlay-position"),
    QLatin1StringView("resize-overlay-duration"),
    QLatin1StringView("gtk-single-instance"),
});

constexpr auto KeybindingFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("root"), QLatin1StringView("tables"),
});
constexpr auto TableFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("name"), QLatin1StringView("bindings"),
});
constexpr auto DefinitionFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("sequence"), QLatin1StringView("actions"),
    QLatin1StringView("flags"),
});
constexpr auto FlagFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("consumed"), QLatin1StringView("all"),
    QLatin1StringView("global"), QLatin1StringView("performable"),
});
constexpr auto PhysicalTriggerFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("kind"), QLatin1StringView("key"),
    QLatin1StringView("mods"),
});
constexpr auto UnicodeTriggerFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("kind"), QLatin1StringView("codepoint"),
    QLatin1StringView("mods"),
});
constexpr auto CatchAllTriggerFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("kind"), QLatin1StringView("mods"),
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
        const bool recognized = std::ranges::any_of(
            expected, [&it](QLatin1StringView field) {
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
                QStringLiteral("%1 is missing field '%2'")
                    .arg(context, field));
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

ParseResult<bool> readBoolean(const QJsonValue &value,
                              const QString &context)
{
    if (!value.isBool()) {
        return std::unexpected(
            QStringLiteral("%1 must be a boolean").arg(context));
    }
    return value.toBool();
}

ParseResult<QString> readString(const QJsonValue &value,
                                const QString &context)
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

ParseResult<double> readFiniteDouble(const QJsonValue &value,
                                     const QString &context,
                                     double minimum =
                                         -std::numeric_limits<double>::infinity(),
                                     double maximum =
                                         std::numeric_limits<double>::infinity())
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

template<std::unsigned_integral Integer>
ParseResult<Integer> readUnsignedInteger(const QJsonValue &value,
                                         const QString &context,
                                         Integer maximum =
                                             std::numeric_limits<Integer>::max())
{
    if (!value.isDouble()) {
        return std::unexpected(
            QStringLiteral("%1 must be an unsigned integer").arg(context));
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0.0
        || std::trunc(number) != number
        || number > static_cast<double>(maximum)) {
        return std::unexpected(
            QStringLiteral("%1 must be an unsigned integer in range")
                .arg(context));
    }
    return static_cast<Integer>(number);
}

ParseResult<quint64> readDecimalUint64(const QJsonValue &value,
                                       const QString &context)
{
    auto text = readString(value, context);
    if (!text) return std::unexpected(std::move(text.error()));
    if (text->isEmpty()
        || (text->size() > 1 && text->startsWith(u'0'))
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
    return QColor((digits[0] << 4) | digits[1],
                  (digits[2] << 4) | digits[3],
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
            array->at(index),
            QStringLiteral("%1[%2]").arg(context).arg(index));
        if (!entry) return std::unexpected(std::move(entry.error()));
        result.append(std::move(*entry));
    }
    return result;
}

ParseResult<QString> readEnum(const QJsonValue &value,
                              const QString &context,
                              Fields allowed)
{
    auto result = readString(value, context);
    if (!result) return std::unexpected(std::move(result.error()));
    if (std::ranges::none_of(allowed, [&result](QLatin1StringView candidate) {
            return *result == candidate;
        })) {
        return std::unexpected(
            QStringLiteral("%1 has unsupported value '%2'")
                .arg(context, *result));
    }
    return result;
}

ParseResult<QVariant> readOptionalRgb(const QJsonValue &value,
                                      const QString &context)
{
    if (value.isNull()) return QVariant(QString{});
    auto color = readRgbColor(value, context);
    if (!color) return std::unexpected(std::move(color.error()));
    return QVariant::fromValue(*color);
}

ParseResult<QVariant> readTerminalColor(const QJsonValue &value,
                                        const QString &context,
                                        bool nullable)
{
    if (value.isNull()) {
        if (nullable) return QVariant(QString{});
        return std::unexpected(
            QStringLiteral("%1 must not be null").arg(context));
    }
    if (value.isString()) {
        const QString sentinel = value.toString();
        if (sentinel == QStringLiteral("cell-foreground")
            || sentinel == QStringLiteral("cell-background")) {
            return QVariant(sentinel);
        }
    }
    auto color = readRgbColor(value, context);
    if (!color) return std::unexpected(std::move(color.error()));
    return QVariant::fromValue(*color);
}

ParseResult<QVariant> readOptionalBoolean(const QJsonValue &value,
                                          const QString &context)
{
    if (value.isNull()) return QVariant(QString{});
    auto result = readBoolean(value, context);
    if (!result) return std::unexpected(std::move(result.error()));
    return QVariant(*result);
}

ParseResult<QVariant> readBoldColor(const QJsonValue &value,
                                    const QString &context)
{
    if (value.isNull()) return QVariant(QString{});
    if (value.isString() && value.toString() == QStringLiteral("bright")) {
        return QVariant(QStringLiteral("bright"));
    }
    auto color = readRgbColor(value, context);
    if (!color) return std::unexpected(std::move(color.error()));
    return QVariant::fromValue(*color);
}

ParseResult<QVariantList> readPalette(const QJsonValue &value,
                                      const QString &context)
{
    auto array = readArray(value, context);
    if (!array) return std::unexpected(std::move(array.error()));
    if (array->size() != 256) {
        return std::unexpected(
            QStringLiteral("%1 must contain exactly 256 colors")
                .arg(context));
    }
    QVariantList result;
    result.reserve(array->size());
    for (qsizetype index = 0; index < array->size(); ++index) {
        auto color = readRgbColor(
            array->at(index),
            QStringLiteral("%1[%2]").arg(context).arg(index));
        if (!color) return std::unexpected(std::move(color.error()));
        result.append(QVariant::fromValue(*color));
    }
    return result;
}

ParseResult<QVariantMap> readValues(const QJsonValue &value)
{
    constexpr QLatin1StringView Context("values");
    auto object = readExactObject(value, Context.toString(), ValueFields);
    if (!object) return std::unexpected(std::move(object.error()));

    QVariantMap result;
    const auto fieldValue = [&object](QLatin1StringView name) {
        return object->value(name);
    };
    const auto context = [](QLatin1StringView name) {
        return childContext(QStringLiteral("values"), name);
    };
    const auto insert = [&result](QLatin1StringView name, auto &&parsed) {
        result.insert(name.toString(),
                      QVariant::fromValue(std::forward<decltype(parsed)>(parsed)));
    };

    for (const QLatin1StringView name : std::to_array<QLatin1StringView>({
             QLatin1StringView("working-directory"),
         })) {
        auto parsed = readString(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        insert(name, std::move(*parsed));
    }
    for (const QLatin1StringView name : std::to_array<QLatin1StringView>({
             QLatin1StringView("font-family"),
             QLatin1StringView("config-file"),
         })) {
        auto parsed = readStringList(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        insert(name, std::move(*parsed));
    }
    for (const QLatin1StringView name : std::to_array<QLatin1StringView>({
             QLatin1StringView("font-size"),
             QLatin1StringView("cursor-opacity"),
         })) {
        auto parsed = readFiniteDouble(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        insert(name, *parsed);
    }
    {
        constexpr QLatin1StringView name("unfocused-split-opacity");
        auto parsed = readFiniteDouble(fieldValue(name), context(name), 0.15, 1.0);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        insert(name, *parsed);
    }
    {
        constexpr QLatin1StringView name("faint-opacity");
        auto parsed = readFiniteDouble(fieldValue(name), context(name), 0.0, 1.0);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        insert(name, *parsed);
    }
    for (const QLatin1StringView name : std::to_array<QLatin1StringView>({
             QLatin1StringView("foreground"),
             QLatin1StringView("background"),
         })) {
        auto parsed = readRgbColor(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        insert(name, *parsed);
    }
    for (const QLatin1StringView name : std::to_array<QLatin1StringView>({
             QLatin1StringView("unfocused-split-fill"),
             QLatin1StringView("split-divider-color"),
         })) {
        auto parsed = readOptionalRgb(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.insert(name.toString(), std::move(*parsed));
    }
    for (const QLatin1StringView name : std::to_array<QLatin1StringView>({
             QLatin1StringView("split-inherit-working-directory"),
             QLatin1StringView("split-preserve-zoom"),
             QLatin1StringView("tab-inherit-working-directory"),
             QLatin1StringView("window-inherit-working-directory"),
             QLatin1StringView("window-inherit-font-size"),
             QLatin1StringView("maximize"),
             QLatin1StringView("clipboard-trim-trailing-spaces"),
             QLatin1StringView("clipboard-paste-protection"),
             QLatin1StringView("clipboard-paste-bracketed-safe"),
             QLatin1StringView("selection-clear-on-typing"),
             QLatin1StringView("selection-clear-on-copy"),
             QLatin1StringView("mouse-reporting"),
             QLatin1StringView("link-url"),
             QLatin1StringView("quit-after-last-window-closed"),
             QLatin1StringView("initial-window"),
         })) {
        auto parsed = readBoolean(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        insert(name, *parsed);
    }

    const auto addEnum = [&](QLatin1StringView name,
                             Fields allowed) -> ParseResult<void> {
        auto parsed = readEnum(fieldValue(name), context(name), allowed);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        insert(name, std::move(*parsed));
        return {};
    };
    constexpr auto NewTabPositions = std::to_array<QLatin1StringView>({
        QLatin1StringView("current"), QLatin1StringView("end"),
    });
    constexpr auto TabBarModes = std::to_array<QLatin1StringView>({
        QLatin1StringView("always"), QLatin1StringView("auto"),
        QLatin1StringView("never"),
    });
    constexpr auto FullscreenModes = std::to_array<QLatin1StringView>({
        QLatin1StringView("false"), QLatin1StringView("true"),
        QLatin1StringView("non-native"),
        QLatin1StringView("non-native-visible-menu"),
        QLatin1StringView("non-native-padded-notch"),
    });
    constexpr auto CursorStyles = std::to_array<QLatin1StringView>({
        QLatin1StringView("block"), QLatin1StringView("bar"),
        QLatin1StringView("underline"), QLatin1StringView("block_hollow"),
    });
    constexpr auto ConfirmCloseModes = std::to_array<QLatin1StringView>({
        QLatin1StringView("false"), QLatin1StringView("true"),
        QLatin1StringView("always"),
    });
    constexpr auto CopyOnSelectModes = std::to_array<QLatin1StringView>({
        QLatin1StringView("false"), QLatin1StringView("true"),
        QLatin1StringView("clipboard"),
    });
    constexpr auto MiddleClickActions = std::to_array<QLatin1StringView>({
        QLatin1StringView("primary-paste"), QLatin1StringView("ignore"),
    });
    constexpr auto LinkPreviewModes = std::to_array<QLatin1StringView>({
        QLatin1StringView("false"), QLatin1StringView("true"),
        QLatin1StringView("osc8"),
    });
    constexpr auto ResizeOverlayModes = std::to_array<QLatin1StringView>({
        QLatin1StringView("always"), QLatin1StringView("never"),
        QLatin1StringView("after-first"),
    });
    constexpr auto ResizeOverlayPositions = std::to_array<QLatin1StringView>({
        QLatin1StringView("center"), QLatin1StringView("top-left"),
        QLatin1StringView("top-center"), QLatin1StringView("top-right"),
        QLatin1StringView("bottom-left"), QLatin1StringView("bottom-center"),
        QLatin1StringView("bottom-right"),
    });
    constexpr auto SingleInstanceModes = std::to_array<QLatin1StringView>({
        QLatin1StringView("false"), QLatin1StringView("true"),
        QLatin1StringView("detect"),
    });
    if (auto parsed = addEnum(QLatin1StringView("window-new-tab-position"),
                              NewTabPositions);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = addEnum(QLatin1StringView("window-show-tab-bar"),
                              TabBarModes);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = addEnum(QLatin1StringView("fullscreen"), FullscreenModes);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = addEnum(QLatin1StringView("cursor-style"), CursorStyles);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = addEnum(QLatin1StringView("confirm-close-surface"),
                              ConfirmCloseModes);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = addEnum(QLatin1StringView("copy-on-select"),
                              CopyOnSelectModes);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = addEnum(QLatin1StringView("middle-click-action"),
                              MiddleClickActions);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = addEnum(QLatin1StringView("link-previews"),
                              LinkPreviewModes);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = addEnum(QLatin1StringView("resize-overlay"),
                              ResizeOverlayModes);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = addEnum(QLatin1StringView("resize-overlay-position"),
                              ResizeOverlayPositions);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = addEnum(QLatin1StringView("gtk-single-instance"),
                              SingleInstanceModes);
        !parsed) return std::unexpected(std::move(parsed.error()));

    for (const QLatin1StringView name : std::to_array<QLatin1StringView>({
             QLatin1StringView("window-width"),
             QLatin1StringView("window-height"),
             QLatin1StringView("resize-overlay-duration"),
         })) {
        auto parsed = readUnsignedInteger<quint32>(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        insert(name, *parsed);
    }
    {
        constexpr QLatin1StringView name("palette");
        auto parsed = readPalette(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        insert(name, std::move(*parsed));
    }
    for (const QLatin1StringView name : std::to_array<QLatin1StringView>({
             QLatin1StringView("selection-foreground"),
             QLatin1StringView("selection-background"),
             QLatin1StringView("cursor-color"),
             QLatin1StringView("cursor-text"),
         })) {
        auto parsed = readTerminalColor(fieldValue(name), context(name), true);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.insert(name.toString(), std::move(*parsed));
    }
    for (const QLatin1StringView name : std::to_array<QLatin1StringView>({
             QLatin1StringView("search-foreground"),
             QLatin1StringView("search-background"),
             QLatin1StringView("search-selected-foreground"),
             QLatin1StringView("search-selected-background"),
         })) {
        auto parsed = readTerminalColor(fieldValue(name), context(name), false);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.insert(name.toString(), std::move(*parsed));
    }
    {
        constexpr QLatin1StringView name("cursor-style-blink");
        auto parsed = readOptionalBoolean(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.insert(name.toString(), std::move(*parsed));
    }
    {
        constexpr QLatin1StringView name("bold-color");
        auto parsed = readBoldColor(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.insert(name.toString(), std::move(*parsed));
    }
    {
        constexpr QLatin1StringView name("scrollback-limit");
        auto parsed = readDecimalUint64(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        insert(name, *parsed);
    }
    {
        constexpr QLatin1StringView name(
            "quit-after-last-window-closed-delay");
        const QJsonValue delay = fieldValue(name);
        if (delay.isNull()) {
            result.insert(name.toString(), QVariant{});
        } else {
            auto parsed = readUnsignedInteger<quint32>(delay, context(name));
            if (!parsed) return std::unexpected(std::move(parsed.error()));
            insert(name, *parsed);
        }
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
        auto key = readNonEmptyString(
            object->value(QStringLiteral("key")),
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
                QStringLiteral(
                    "%1.codepoint must be a nonzero Unicode scalar")
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

ParseResult<GhosttyKeybindDefinition> readDefinition(
    const QJsonValue &value, const QString &context)
{
    auto object = readExactObject(value, context, DefinitionFields);
    if (!object) return std::unexpected(std::move(object.error()));
    auto sequence = readArray(object->value(QStringLiteral("sequence")),
                              childContext(context,
                                           QLatin1StringView("sequence")));
    if (!sequence) return std::unexpected(std::move(sequence.error()));
    if (sequence->isEmpty()) {
        return std::unexpected(
            QStringLiteral("%1.sequence must be a non-empty array")
                .arg(context));
    }
    auto actions = readArray(object->value(QStringLiteral("actions")),
                             childContext(context,
                                          QLatin1StringView("actions")));
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

ParseResult<QVector<GhosttyKeybindDefinition>> readDefinitions(
    const QJsonValue &value, const QString &context)
{
    auto array = readArray(value, context);
    if (!array) return std::unexpected(std::move(array.error()));
    QVector<GhosttyKeybindDefinition> result;
    result.reserve(array->size());
    for (qsizetype index = 0; index < array->size(); ++index) {
        auto definition = readDefinition(
            array->at(index),
            QStringLiteral("%1[%2]").arg(context).arg(index));
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
    auto root = readDefinitions(object->value(QStringLiteral("root")),
                                childContext(context,
                                             QLatin1StringView("root")));
    if (!root) return std::unexpected(std::move(root.error()));
    auto tables = readArray(object->value(QStringLiteral("tables")),
                            childContext(context,
                                         QLatin1StringView("tables")));
    if (!tables) return std::unexpected(std::move(tables.error()));

    GhosttyKeybindConfig result;
    result.root = std::move(*root);
    result.tables.reserve(tables->size());
    for (qsizetype index = 0; index < tables->size(); ++index) {
        const QString tableContext =
            QStringLiteral("%1.tables[%2]").arg(context).arg(index);
        auto table = readExactObject(tables->at(index), tableContext,
                                     TableFields);
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
            QStringLiteral("Invalid Ghostty structured config JSON at offset %1: %2")
                .arg(jsonError.offset)
                .arg(jsonError.errorString()));
    }
    if (!document.isObject()) {
        return std::unexpected(
            QStringLiteral("Ghostty structured config JSON root must be an object"));
    }

    const QString rootContext =
        QStringLiteral("Ghostty structured config JSON root");
    auto root = readExactObject(document.object(), rootContext, RootFields);
    if (!root) return std::unexpected(std::move(root.error()));
    auto version = readUnsignedInteger<quint32>(
        root->value(QStringLiteral("version")),
        childContext(rootContext, QLatin1StringView("version")));
    if (!version
        || *version != GhosttyConfigExport::CurrentSchemaVersion) {
        return std::unexpected(
            QStringLiteral("Unsupported Ghostty structured config JSON schema version"));
    }

    auto values = readValues(root->value(QStringLiteral("values")));
    if (!values) return std::unexpected(std::move(values.error()));
    auto keybindings = readKeybindings(
        root->value(QStringLiteral("keybindings")),
        QStringLiteral("keybindings"));
    if (!keybindings) {
        return std::unexpected(std::move(keybindings.error()));
    }
    auto defaultKeybindings = readKeybindings(
        root->value(QStringLiteral("default-keybindings")),
        QStringLiteral("default-keybindings"));
    if (!defaultKeybindings) {
        return std::unexpected(std::move(defaultKeybindings.error()));
    }

    return GhosttyConfigExport{
        .values = std::move(*values),
        .keybindings = std::move(*keybindings),
        .defaultKeybindings = std::move(*defaultKeybindings),
    };
}
