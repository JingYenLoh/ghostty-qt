#include "terminal_cell_metrics.h"

#include <QByteArrayView>
#include <QFontMetricsF>
#include <QRawFont>

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <limits>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

using Pixel = quint32;
using SignedPixel = qint32;

constexpr auto kMetrics = std::to_array({
    TerminalMetric::CellWidth,
    TerminalMetric::CellHeight,
    TerminalMetric::FontBaseline,
    TerminalMetric::UnderlinePosition,
    TerminalMetric::UnderlineThickness,
    TerminalMetric::StrikethroughPosition,
    TerminalMetric::StrikethroughThickness,
    TerminalMetric::OverlinePosition,
    TerminalMetric::OverlineThickness,
    TerminalMetric::CursorThickness,
    TerminalMetric::CursorHeight,
});

[[nodiscard]] qreal normalizedDpr(qreal value) noexcept
{
    return std::isfinite(value) && value > 0.0 ? value : 1.0;
}

template <typename Integer>
[[nodiscard]] Integer roundedSaturated(double value) noexcept
{
    constexpr Integer minimum = std::numeric_limits<Integer>::lowest();
    constexpr Integer maximum = std::numeric_limits<Integer>::max();
    if (std::isnan(value)) {
        return {};
    }
    if (value <= static_cast<double>(minimum)) {
        return minimum;
    }
    if (value >= static_cast<double>(maximum)) {
        return maximum;
    }
    return static_cast<Integer>(std::round(value));
}

[[nodiscard]] Pixel roundedPixel(double value) noexcept
{
    return roundedSaturated<Pixel>(value);
}

[[nodiscard]] Pixel ceiledPixel(double value) noexcept
{
    if (std::isnan(value) || value <= 0.0) {
        return 0;
    }
    constexpr Pixel maximum = std::numeric_limits<Pixel>::max();
    if (value >= static_cast<double>(maximum)) {
        return maximum;
    }
    return static_cast<Pixel>(std::ceil(value));
}

[[nodiscard]] Pixel addSaturated(Pixel value, qint64 delta) noexcept
{
    constexpr Pixel maximum = std::numeric_limits<Pixel>::max();
    if (delta >= 0) {
        const quint64 increment = static_cast<quint64>(delta);
        return increment > static_cast<quint64>(maximum - value)
            ? maximum
            : value + static_cast<Pixel>(increment);
    }
    const quint64 decrement = static_cast<quint64>(-(delta + 1)) + 1U;
    return decrement > value ? 0U : value - static_cast<Pixel>(decrement);
}

[[nodiscard]] SignedPixel addSaturated(SignedPixel value, qint64 delta) noexcept
{
    const qint64 sum = static_cast<qint64>(value) + delta;
    return static_cast<SignedPixel>(std::clamp(
        sum, static_cast<qint64>(std::numeric_limits<SignedPixel>::lowest()),
        static_cast<qint64>(std::numeric_limits<SignedPixel>::max())));
}

template <typename Integer>
[[nodiscard]] Integer
applyModifier(Integer value, const TerminalMetricModifier &modifier) noexcept
{
    return std::visit(
        [value](const auto &typed) -> Integer {
            using Modifier = std::decay_t<decltype(typed)>;
            if constexpr (std::same_as<Modifier,
                                       TerminalMetricModifiers::Absolute>) {
                return addSaturated(value, static_cast<qint64>(typed.pixels));
            } else {
                const double multiplier = std::isnan(typed.multiplier)
                    ? 0.0
                    : std::max(0.0, typed.multiplier);
                return roundedSaturated<Integer>(static_cast<double>(value)
                                                 * multiplier);
            }
        },
        modifier);
}

[[nodiscard]] std::optional<quint16>
positiveBigEndianFword(QByteArrayView table, qsizetype offset) noexcept
{
    constexpr qsizetype width = 2;
    if (offset < 0 || table.size() < width || offset > table.size() - width) {
        return std::nullopt;
    }

    const auto high = static_cast<quint8>(table[offset]);
    const auto low = static_cast<quint8>(table[offset + 1]);
    const quint16 value =
        static_cast<quint16>((static_cast<quint16>(high) << 8U) | low);
    // FWORD is signed. Negative and zero stroke widths are unusable.
    return value > 0
            && value <= static_cast<quint16>(std::numeric_limits<qint16>::max())
        ? std::optional<quint16>{value}
        : std::nullopt;
}

[[nodiscard]] std::optional<double>
physicalTableThickness(const QRawFont &font, const char *tableName,
                       qsizetype offset, qreal devicePixelRatio)
{
    const qreal unitsPerEm = font.unitsPerEm();
    const qreal pixelSize = font.pixelSize();
    if (!font.isValid() || !std::isfinite(unitsPerEm) || unitsPerEm <= 0.0
        || !std::isfinite(pixelSize) || pixelSize <= 0.0) {
        return std::nullopt;
    }

    const QByteArray table = font.fontTable(tableName);
    const auto designUnits =
        positiveBigEndianFword(QByteArrayView(table), offset);
    if (!designUnits) {
        return std::nullopt;
    }

    const double result = static_cast<double>(*designUnits)
        * static_cast<double>(pixelSize) * static_cast<double>(devicePixelRatio)
        / static_cast<double>(unitsPerEm);
    return std::isfinite(result) && result > 0.0 ? std::optional<double>{result}
                                                 : std::nullopt;
}

struct PhysicalStrokeThicknesses {
    double underline = 1.0;
    double strikethrough = 1.0;
};

[[nodiscard]] PhysicalStrokeThicknesses
physicalStrokeThicknesses(const QFont &font, const QFontMetricsF &metrics,
                          qreal devicePixelRatio)
{
    // OpenType `post` stores underlineThickness at byte 10. OS/2 stores
    // yStrikeoutSize at byte 26. Both are signed FWORD design units.
    const QRawFont rawFont = QRawFont::fromFont(font);
    const double fallback =
        static_cast<double>(metrics.lineWidth() * devicePixelRatio);
    const double underline =
        physicalTableThickness(rawFont, "post", 10, devicePixelRatio)
            .value_or(fallback);
    return {
        .underline = underline,
        .strikethrough =
            physicalTableThickness(rawFont, "OS/2", 26, devicePixelRatio)
                .value_or(underline),
    };
}

struct PhysicalMetrics {
    Pixel cellWidth = 1;
    Pixel cellHeight = 1;
    Pixel cellBaseline = 0; // Distance from the bottom of the cell.
    Pixel underlinePosition = 0;
    Pixel underlineThickness = 1;
    Pixel strikethroughPosition = 0;
    Pixel strikethroughThickness = 1;
    SignedPixel overlinePosition = 0;
    Pixel overlineThickness = 1;
    Pixel cursorThickness = 1;
    Pixel cursorHeight = 1;
    double faceHeight = 1.0;
    double faceY = 0.0;
};

[[nodiscard]] PhysicalMetrics baseMetrics(const QFont &font,
                                          qreal devicePixelRatio)
{
    const QFontMetricsF metrics(font);
    const double dpr = devicePixelRatio;
    double faceWidth = 0.0;
    for (char value = 0x20; value <= 0x7e; ++value) {
        faceWidth = std::max(
            faceWidth,
            static_cast<double>(metrics.horizontalAdvance(QLatin1Char(value))));
    }
    faceWidth *= dpr;
    const double faceHeight = metrics.lineSpacing() * dpr;
    const Pixel cellWidth = std::max<Pixel>(1, roundedPixel(faceWidth));
    const Pixel cellHeight = std::max<Pixel>(1, roundedPixel(faceHeight));

    const double faceBaseline =
        (metrics.leading() / 2.0 + metrics.descent()) * dpr;
    const Pixel cellBaseline = roundedPixel(
        faceBaseline - (static_cast<double>(cellHeight) - faceHeight) / 2.0);
    const qint64 topToBaseline =
        static_cast<qint64>(cellHeight) - static_cast<qint64>(cellBaseline);
    const PhysicalStrokeThicknesses strokes =
        physicalStrokeThicknesses(font, metrics, devicePixelRatio);
    const Pixel underlineThickness =
        std::max<Pixel>(1, ceiledPixel(strokes.underline));
    const Pixel strikethroughThickness =
        std::max<Pixel>(1, ceiledPixel(strokes.strikethrough));

    return {
        .cellWidth = cellWidth,
        .cellHeight = cellHeight,
        .cellBaseline = cellBaseline,
        .underlinePosition = roundedPixel(static_cast<double>(topToBaseline)
                                          + metrics.underlinePos() * dpr),
        .underlineThickness = underlineThickness,
        .strikethroughPosition = roundedPixel(static_cast<double>(topToBaseline)
                                              - metrics.strikeOutPos() * dpr),
        .strikethroughThickness = strikethroughThickness,
        .overlinePosition = 0,
        .overlineThickness = underlineThickness,
        .cursorThickness = 1,
        .cursorHeight = cellHeight,
        .faceHeight = faceHeight,
        .faceY = static_cast<double>(cellBaseline) - faceBaseline,
    };
}

void adjustCellHeight(PhysicalMetrics &metrics,
                      const TerminalMetricModifier &modifier)
{
    const Pixel original = metrics.cellHeight;
    const Pixel adjusted =
        std::max<Pixel>(1, applyModifier(original, modifier));
    if (adjusted == original) {
        return;
    }

    const double difference =
        static_cast<double>(adjusted) - static_cast<double>(original);
    const double halfDifference = difference / 2.0;
    const double positionWithRespectToCenter = metrics.faceY
        - (static_cast<double>(original) - metrics.faceHeight) / 2.0;
    const qint64 differenceTop = static_cast<qint64>(
        positionWithRespectToCenter > 0.0 ? std::ceil(halfDifference)
                                          : std::floor(halfDifference));
    const qint64 differenceBottom = static_cast<qint64>(
        positionWithRespectToCenter > 0.0 ? std::floor(halfDifference)
                                          : std::ceil(halfDifference));

    metrics.cellHeight = adjusted;
    metrics.cellBaseline = addSaturated(metrics.cellBaseline, differenceBottom);
    metrics.faceY += static_cast<double>(differenceBottom);
    metrics.underlinePosition =
        addSaturated(metrics.underlinePosition, differenceTop);
    metrics.strikethroughPosition =
        addSaturated(metrics.strikethroughPosition, differenceTop);
    metrics.overlinePosition =
        addSaturated(metrics.overlinePosition, differenceTop);
}

void applyOne(PhysicalMetrics &metrics, TerminalMetric key,
              const TerminalMetricModifier &modifier)
{
    switch (key) {
    case TerminalMetric::CellWidth:
        metrics.cellWidth =
            std::max<Pixel>(1, applyModifier(metrics.cellWidth, modifier));
        break;
    case TerminalMetric::CellHeight: adjustCellHeight(metrics, modifier); break;
    case TerminalMetric::FontBaseline:
        metrics.cellBaseline = applyModifier(metrics.cellBaseline, modifier);
        break;
    case TerminalMetric::UnderlinePosition:
        metrics.underlinePosition =
            applyModifier(metrics.underlinePosition, modifier);
        break;
    case TerminalMetric::UnderlineThickness:
        metrics.underlineThickness =
            applyModifier(metrics.underlineThickness, modifier);
        break;
    case TerminalMetric::StrikethroughPosition:
        metrics.strikethroughPosition =
            applyModifier(metrics.strikethroughPosition, modifier);
        break;
    case TerminalMetric::StrikethroughThickness:
        metrics.strikethroughThickness =
            applyModifier(metrics.strikethroughThickness, modifier);
        break;
    case TerminalMetric::OverlinePosition:
        metrics.overlinePosition =
            applyModifier(metrics.overlinePosition, modifier);
        break;
    case TerminalMetric::OverlineThickness:
        metrics.overlineThickness =
            applyModifier(metrics.overlineThickness, modifier);
        break;
    case TerminalMetric::CursorThickness:
        metrics.cursorThickness =
            applyModifier(metrics.cursorThickness, modifier);
        break;
    case TerminalMetric::CursorHeight:
        metrics.cursorHeight = applyModifier(metrics.cursorHeight, modifier);
        break;
    case TerminalMetric::Count: break;
    }
}

void applyModifiers(PhysicalMetrics &metrics,
                    const TerminalMetricModifierSet &modifiers)
{
    std::array<bool, terminalEnumIndex(TerminalMetric::Count)> applied{};
    const auto apply = [&](TerminalMetric key) {
        const std::size_t index = terminalEnumIndex(key);
        if (index >= applied.size() || std::exchange(applied[index], true)) {
            return;
        }
        if (const auto &modifier = modifiers[key]) {
            applyOne(metrics, key, *modifier);
        }
    };

    if (!modifiers.applicationOrder.empty()) {
        std::ranges::for_each(modifiers.applicationOrder, apply);
    }
    std::ranges::for_each(kMetrics, apply);

    metrics.cellWidth = std::max<Pixel>(1, metrics.cellWidth);
    metrics.cellHeight = std::max<Pixel>(1, metrics.cellHeight);
    metrics.underlineThickness = std::max<Pixel>(1, metrics.underlineThickness);
    metrics.strikethroughThickness =
        std::max<Pixel>(1, metrics.strikethroughThickness);
    metrics.overlineThickness = std::max<Pixel>(1, metrics.overlineThickness);
    metrics.cursorThickness = std::max<Pixel>(1, metrics.cursorThickness);
    metrics.cursorHeight = std::max<Pixel>(1, metrics.cursorHeight);
}

[[nodiscard]] qreal logical(Pixel value, qreal devicePixelRatio)
{
    return static_cast<qreal>(value) / devicePixelRatio;
}

[[nodiscard]] qreal logical(SignedPixel value, qreal devicePixelRatio)
{
    return static_cast<qreal>(value) / devicePixelRatio;
}

[[nodiscard]] qreal logical(qint64 value, qreal devicePixelRatio)
{
    return static_cast<qreal>(value) / devicePixelRatio;
}

} // namespace

TerminalCellMetrics terminalCellMetrics(const TerminalTypography &typography,
                                        qreal devicePixelRatio)
{
    const qreal dpr = normalizedDpr(devicePixelRatio);
    TerminalResolvedFonts fonts = resolveTerminalFonts(typography);
    PhysicalMetrics physical = baseMetrics(
        fonts.metricFonts[terminalEnumIndex(TerminalFontRole::Regular)], dpr);
    applyModifiers(physical, typography.metricModifiers);

    const qint64 baselineFromTop = static_cast<qint64>(physical.cellHeight)
        - static_cast<qint64>(physical.cellBaseline);
    const qint64 cursorTop = (static_cast<qint64>(physical.cellHeight)
                              - static_cast<qint64>(physical.cursorHeight))
        / 2;
    const qint64 cursorBarLeft =
        -((static_cast<qint64>(physical.cursorThickness) + 1) / 2);
    const Pixel underlineLimitWithoutThickness = addSaturated(
        physical.cellHeight, static_cast<qint64>(physical.cellHeight / 4));
    const Pixel underlineMaximumPosition =
        addSaturated(underlineLimitWithoutThickness,
                     -static_cast<qint64>(physical.underlineThickness));
    const qint64 overlineMinimumPosition =
        -static_cast<qint64>(physical.cellHeight / 4);
    return {
        .fonts = std::move(fonts.fonts),
        .mappedFonts = std::move(fonts.mappedFonts),
        .shapingBreakCursor = typography.shapingBreakCursor,
        .cellWidth = logical(physical.cellWidth, dpr),
        .cellHeight = logical(physical.cellHeight, dpr),
        .baseline = static_cast<qreal>(baselineFromTop) / dpr,
        .underlinePosition = logical(physical.underlinePosition, dpr),
        .underlineThickness = logical(physical.underlineThickness, dpr),
        .strikethroughPosition = logical(physical.strikethroughPosition, dpr),
        .strikethroughThickness = logical(physical.strikethroughThickness, dpr),
        .overlinePosition = logical(physical.overlinePosition, dpr),
        .overlineThickness = logical(physical.overlineThickness, dpr),
        .cursorThickness = logical(physical.cursorThickness, dpr),
        .cursorHeight = logical(physical.cursorHeight, dpr),
        .cursorTop = logical(cursorTop, dpr),
        .cursorBarLeft = logical(cursorBarLeft, dpr),
        .underlineMaximumPosition = logical(underlineMaximumPosition, dpr),
        .overlineMinimumPosition = logical(overlineMinimumPosition, dpr),
    };
}
