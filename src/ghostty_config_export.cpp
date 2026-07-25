#include "ghostty_config_export.h"

#include <QColor>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <concepts>
#include <limits>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

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
    QLatin1StringView("font-family-bold"),
    QLatin1StringView("font-family-italic"),
    QLatin1StringView("font-family-bold-italic"),
    QLatin1StringView("font-size"),
    QLatin1StringView("font-style"),
    QLatin1StringView("font-style-bold"),
    QLatin1StringView("font-style-italic"),
    QLatin1StringView("font-style-bold-italic"),
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
    QLatin1StringView("window-decoration"),
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
    QLatin1StringView("scrollbar"),
    QLatin1StringView("bell-features"),
    QLatin1StringView("bell-audio-path"),
    QLatin1StringView("bell-audio-volume"),
    QLatin1StringView("confirm-close-surface"),
    QLatin1StringView("clipboard-trim-trailing-spaces"),
    QLatin1StringView("clipboard-paste-protection"),
    QLatin1StringView("clipboard-paste-bracketed-safe"),
    QLatin1StringView("copy-on-select"),
    QLatin1StringView("selection-clear-on-typing"),
    QLatin1StringView("selection-clear-on-copy"),
    QLatin1StringView("middle-click-action"),
    QLatin1StringView("mouse-reporting"),
    QLatin1StringView("mouse-hide-while-typing"),
    QLatin1StringView("mouse-scroll-multiplier"),
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
constexpr auto FontStyleFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("kind"),
});
constexpr auto NamedFontStyleFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("kind"), QLatin1StringView("name"),
});
constexpr auto MetricModifierFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("kind"), QLatin1StringView("value"),
});
constexpr auto BellFeatureFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("system"),
    QLatin1StringView("audio"),
    QLatin1StringView("attention"),
    QLatin1StringView("title"),
    QLatin1StringView("border"),
});
constexpr auto ConfigPathFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("path"),
    QLatin1StringView("optional"),
});
constexpr auto MouseScrollMultiplierFields = std::to_array<QLatin1StringView>({
    QLatin1StringView("precision"),
    QLatin1StringView("discrete"),
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

ParseResult<BellFeatures> readBellFeatures(const QJsonValue &value,
                                           const QString &context)
{
    auto object = readExactObject(value, context, BellFeatureFields);
    if (!object) return std::unexpected(std::move(object.error()));

    BellFeatures result;
    for (const auto [name, destination] :
         std::to_array<std::pair<QLatin1StringView, bool *>>({
             {QLatin1StringView("system"), &result.system},
             {QLatin1StringView("audio"), &result.audio},
             {QLatin1StringView("attention"), &result.attention},
             {QLatin1StringView("title"), &result.title},
             {QLatin1StringView("border"), &result.border},
         })) {
        auto parsed =
            readBoolean(object->value(name), childContext(context, name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        *destination = *parsed;
    }
    return result;
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

ParseResult<std::optional<GhosttyConfigPath>>
readOptionalConfigPath(const QJsonValue &value, const QString &context)
{
    if (value.isNull()) return std::nullopt;

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

template<std::signed_integral Integer>
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

ParseResult<QVector<GhosttyConfigFile>> readConfigFiles(
    const QJsonValue &value, const QString &context)
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

template<typename Enum, std::size_t Size>
ParseResult<Enum> readEnum(
    const QJsonValue &value,
    const QString &context,
    const std::array<std::pair<QLatin1StringView, Enum>, Size> &allowed)
{
    auto text = readString(value, context);
    if (!text) return std::unexpected(std::move(text.error()));
    const auto match =
        std::ranges::find_if(allowed, [&text](const auto &candidate) {
            return *text == candidate.first;
        });
    if (match == allowed.cend()) {
        return std::unexpected(
            QStringLiteral("%1 has unsupported value '%2'")
                .arg(context, *text));
    }
    return match->second;
}

ParseResult<std::optional<QColor>> readOptionalRgb(
    const QJsonValue &value, const QString &context)
{
    if (value.isNull()) return std::nullopt;
    auto color = readRgbColor(value, context);
    if (!color) return std::unexpected(std::move(color.error()));
    return *color;
}

ParseResult<GhosttyTerminalColor> readTerminalColor(
    const QJsonValue &value, const QString &context)
{
    if (value.isNull()) {
        return std::unexpected(
            QStringLiteral("%1 must not be null").arg(context));
    }
    if (value.isString()) {
        const QString sentinel = value.toString();
        if (sentinel == QStringLiteral("cell-foreground")) {
            return GhosttyCellRelativeColor::Foreground;
        }
        if (sentinel == QStringLiteral("cell-background")) {
            return GhosttyCellRelativeColor::Background;
        }
    }
    auto color = readRgbColor(value, context);
    if (!color) return std::unexpected(std::move(color.error()));
    return *color;
}

ParseResult<std::optional<GhosttyTerminalColor>> readOptionalTerminalColor(
    const QJsonValue &value, const QString &context)
{
    if (value.isNull()) return std::nullopt;
    auto color = readTerminalColor(value, context);
    if (!color) return std::unexpected(std::move(color.error()));
    return std::move(*color);
}

ParseResult<std::optional<bool>> readOptionalBoolean(
    const QJsonValue &value, const QString &context)
{
    if (value.isNull()) return std::nullopt;
    auto result = readBoolean(value, context);
    if (!result) return std::unexpected(std::move(result.error()));
    return *result;
}

ParseResult<std::optional<GhosttyBoldColor>> readBoldColor(
    const QJsonValue &value, const QString &context)
{
    if (value.isNull()) return std::nullopt;
    if (value.isString() && value.toString() == QStringLiteral("bright")) {
        return GhosttyBoldColor{GhosttyBoldBrightness::Bright};
    }
    auto color = readRgbColor(value, context);
    if (!color) return std::unexpected(std::move(color.error()));
    return GhosttyBoldColor{*color};
}

ParseResult<std::array<QColor, 256>> readPalette(
    const QJsonValue &value, const QString &context)
{
    auto array = readArray(value, context);
    if (!array) return std::unexpected(std::move(array.error()));
    if (array->size() != 256) {
        return std::unexpected(
            QStringLiteral("%1 must contain exactly 256 colors")
                .arg(context));
    }
    std::array<QColor, 256> result;
    for (qsizetype index = 0; index < array->size(); ++index) {
        auto color = readRgbColor(
            array->at(index),
            QStringLiteral("%1[%2]").arg(context).arg(index));
        if (!color) return std::unexpected(std::move(color.error()));
        result[static_cast<std::size_t>(index)] = *color;
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
    auto kind = readString(unvalidated.value(Kind),
                           childContext(context, Kind));
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
    auto name = readNonEmptyString(object->value(Name),
                                   childContext(context, Name));
    if (!name) return std::unexpected(std::move(name.error()));
    return TerminalFontStyles::Named{.name = std::move(*name)};
}

ParseResult<TerminalMetricModifierSet::Value> readMetricModifier(
    const QJsonValue &value, const QString &context)
{
    if (value.isNull()) return TerminalMetricModifierSet::Value{};

    auto object = readExactObject(value, context, MetricModifierFields);
    if (!object) return std::unexpected(std::move(object.error()));

    constexpr QLatin1StringView Kind("kind");
    constexpr QLatin1StringView Value("value");
    auto kind = readString(object->value(Kind), childContext(context, Kind));
    if (!kind) return std::unexpected(std::move(kind.error()));

    if (*kind == QLatin1StringView("absolute")) {
        auto pixels = readSignedInteger<qint32>(
            object->value(Value), childContext(context, Value));
        if (!pixels) return std::unexpected(std::move(pixels.error()));
        return TerminalMetricModifier{
            TerminalMetricModifiers::Absolute{.pixels = *pixels}};
    }
    if (*kind == QLatin1StringView("percentage")) {
        auto multiplier = readFiniteDouble(
            object->value(Value), childContext(context, Value), 0.0);
        if (!multiplier) {
            return std::unexpected(std::move(multiplier.error()));
        }
        return TerminalMetricModifier{
            TerminalMetricModifiers::Percentage{
                .multiplier = *multiplier}};
    }
    return std::unexpected(
        QStringLiteral("%1.kind has unsupported value '%2'")
            .arg(context, *kind));
}

ParseResult<std::vector<TerminalMetric>> readMetricModifierOrder(
    const QJsonValue &value,
    const QString &context,
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
            MetricModifierValueFields,
            [&name](const auto &candidate) {
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
    const auto assign = [&](QLatin1StringView name,
                            auto &destination,
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

    {
        constexpr QLatin1StringView name("working-directory");
        auto parsed = readString(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        if (parsed->isEmpty() || *parsed == QLatin1StringView("home")) {
            return std::unexpected(
                QStringLiteral("%1 must be 'inherit' or a finalized path")
                    .arg(context(name)));
        }
        if (*parsed == QLatin1StringView("inherit")) {
            result.workingDirectoryPath.reset();
        } else {
            result.workingDirectoryPath = std::move(*parsed);
        }
    }
    for (const auto &[name, role] : FontFamilyFields) {
        if (auto parsed = assign(name,
                                 result.typography.face(role).families,
                                 readNonEmptyStringList);
            !parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
    }
    for (const auto &[name, role] : FontStyleValueFields) {
        if (auto parsed = assign(name,
                                 result.typography.face(role).style,
                                 readFontStyle);
            !parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
    }
    for (const auto &[name, metric] : MetricModifierValueFields) {
        if (auto parsed = assign(name,
                                 result.typography.metricModifiers[metric],
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
        result.typography.metricModifiers.applicationOrder =
            std::move(*parsed);
    }
    if (auto parsed = assign(QLatin1StringView("config-file"),
                             result.configFiles, readConfigFiles);
        !parsed) return std::unexpected(std::move(parsed.error()));
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
        !parsed) return std::unexpected(std::move(parsed.error()));
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

    for (const auto [name, destination] :
         std::to_array<std::pair<QLatin1StringView, QColor *>>({
             {QLatin1StringView("foreground"),
              &result.appearance.foreground},
             {QLatin1StringView("background"),
              &result.appearance.background},
         })) {
        if (auto parsed = assign(name, *destination, readRgbColor); !parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
    }
    {
        constexpr QLatin1StringView name("unfocused-split-opacity");
        auto parsed = readFiniteDouble(
            fieldValue(name), context(name), 0.15, 1.0);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.splitAppearance.unfocusedOpacity = *parsed;
    }
    if (auto parsed = assign(QLatin1StringView("unfocused-split-fill"),
                             result.splitAppearance.unfocusedFill,
                             readOptionalRgb);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assign(QLatin1StringView("split-divider-color"),
                             result.splitAppearance.dividerColor,
                             readOptionalRgb);
        !parsed) return std::unexpected(std::move(parsed.error()));

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
             {QLatin1StringView("link-url"), &result.linkUrl},
             {QLatin1StringView("quit-after-last-window-closed"),
              &result.quitAfterLastWindowClosed},
             {QLatin1StringView("initial-window"), &result.initialWindow},
         })) {
        if (auto parsed = assignBoolean(name, *destination); !parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
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
    constexpr auto WindowDecorations =
        std::to_array<std::pair<QLatin1StringView, WindowDecorationMode>>({
            {QLatin1StringView("auto"), WindowDecorationMode::Auto},
            {QLatin1StringView("client"), WindowDecorationMode::Client},
            {QLatin1StringView("server"), WindowDecorationMode::Server},
            {QLatin1StringView("none"), WindowDecorationMode::None},
        });
    constexpr auto FullscreenModes =
        std::to_array<std::pair<QLatin1StringView, GhosttyFullscreenMode>>({
            {QLatin1StringView("false"), GhosttyFullscreenMode::Disabled},
            {QLatin1StringView("true"), GhosttyFullscreenMode::Enabled},
            {QLatin1StringView("non-native"),
             GhosttyFullscreenMode::NonNative},
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
            {QLatin1StringView("true"),
             ConfirmCloseMode::RunningProcesses},
            {QLatin1StringView("always"), ConfirmCloseMode::Always},
        });
    constexpr auto CopyOnSelectModes =
        std::to_array<std::pair<QLatin1StringView,
                                TerminalCopyOnSelectMode>>({
            {QLatin1StringView("false"),
             TerminalCopyOnSelectMode::Disabled},
            {QLatin1StringView("true"),
             TerminalCopyOnSelectMode::Primary},
            {QLatin1StringView("clipboard"),
             TerminalCopyOnSelectMode::PrimaryAndClipboard},
        });
    constexpr auto MiddleClickActions =
        std::to_array<std::pair<QLatin1StringView, MiddleClickAction>>({
            {QLatin1StringView("primary-paste"),
             MiddleClickAction::PrimaryPaste},
            {QLatin1StringView("ignore"), MiddleClickAction::Ignore},
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
            {QLatin1StringView("after-first"),
             ResizeOverlayMode::AfterFirst},
        });
    constexpr auto ResizeOverlayPositions =
        std::to_array<std::pair<QLatin1StringView,
                                ResizeOverlayPosition>>({
            {QLatin1StringView("center"), ResizeOverlayPosition::Center},
            {QLatin1StringView("top-left"),
             ResizeOverlayPosition::TopLeft},
            {QLatin1StringView("top-center"),
             ResizeOverlayPosition::TopCenter},
            {QLatin1StringView("top-right"),
             ResizeOverlayPosition::TopRight},
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

    const auto assignEnum = [&](QLatin1StringView name,
                                auto &destination,
                                const auto &allowed) -> ParseResult<void> {
        auto parsed = readEnum(fieldValue(name), context(name), allowed);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        destination = *parsed;
        return {};
    };
    if (auto parsed = assignEnum(QLatin1StringView("window-new-tab-position"),
                                 result.windowNewTabPosition,
                                 NewTabPositions);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("window-show-tab-bar"),
                                 result.windowShowTabBar,
                                 TabBarModes);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("window-decoration"),
                                 result.windowDecoration, WindowDecorations);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("fullscreen"),
                                 result.fullscreen,
                                 FullscreenModes);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("cursor-style"),
                                 result.appearance.cursorStyle,
                                 CursorStyles);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("confirm-close-surface"),
                                 result.confirmCloseMode,
                                 ConfirmCloseModes);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("copy-on-select"),
                                 result.selectionClipboard.copyOnSelect,
                                 CopyOnSelectModes);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("middle-click-action"),
                                 result.middleClickAction,
                                 MiddleClickActions);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("link-previews"),
                                 result.linkPreviews,
                                 LinkPreviewModes);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("scrollbar"),
                                 result.scrollbar, ScrollbarPolicies);
        !parsed)
        return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("resize-overlay"),
                                 result.resizeOverlay.mode,
                                 ResizeOverlayModes);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("resize-overlay-position"),
                                 result.resizeOverlay.position,
                                 ResizeOverlayPositions);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assignEnum(QLatin1StringView("gtk-single-instance"),
                                 result.singleInstanceMode,
                                 SingleInstanceModes);
        !parsed) return std::unexpected(std::move(parsed.error()));

    if (auto parsed = assign(QLatin1StringView("window-width"),
                             result.windowWidth, readUint32);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assign(QLatin1StringView("window-height"),
                             result.windowHeight, readUint32);
        !parsed) return std::unexpected(std::move(parsed.error()));
    if (auto parsed = assign(QLatin1StringView("palette"),
                             result.appearance.palette, readPalette);
        !parsed) return std::unexpected(std::move(parsed.error()));

    for (const auto [name, destination] :
         std::to_array<std::pair<
             QLatin1StringView,
             std::optional<GhosttyTerminalColor> *>>({
             {QLatin1StringView("selection-foreground"),
              &result.appearance.selectionForeground},
             {QLatin1StringView("selection-background"),
              &result.appearance.selectionBackground},
             {QLatin1StringView("cursor-color"),
              &result.appearance.cursorColor},
             {QLatin1StringView("cursor-text"),
              &result.appearance.cursorText},
         })) {
        auto parsed = readOptionalTerminalColor(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        *destination = *parsed;
    }
    for (const auto [name, destination] :
         std::to_array<std::pair<QLatin1StringView,
                                 GhosttyTerminalColor *>>({
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
    if (auto parsed = assign(QLatin1StringView("cursor-style-blink"),
                             result.appearance.cursorBlink,
                             readOptionalBoolean);
        !parsed) return std::unexpected(std::move(parsed.error()));
    {
        constexpr QLatin1StringView name("cursor-opacity");
        auto parsed = readFiniteDouble(fieldValue(name), context(name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.appearance.cursorOpacity = std::clamp(*parsed, 0.0, 1.0);
    }
    if (auto parsed = assign(QLatin1StringView("bold-color"),
                             result.appearance.boldColor, readBoldColor);
        !parsed) return std::unexpected(std::move(parsed.error()));
    {
        constexpr QLatin1StringView name("faint-opacity");
        auto parsed = readFiniteDouble(
            fieldValue(name), context(name), 0.0, 1.0);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        result.appearance.faintOpacity = *parsed;
    }
    if (auto parsed = assign(QLatin1StringView("scrollback-limit"),
                             result.scrollbackLimitBytes,
                             readDecimalUint64);
        !parsed) return std::unexpected(std::move(parsed.error()));

    {
        constexpr QLatin1StringView name(
            "quit-after-last-window-closed-delay");
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
        auto parsed = readUnsignedInteger<quint32>(
            fieldValue(name), context(name));
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

    return GhosttyConfigExport(std::move(*values), std::move(*keybindings),
                               std::move(*defaultKeybindings));
}
