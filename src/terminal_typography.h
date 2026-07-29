#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <array>
#include <bit>
#include <cstddef>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

enum class TerminalFontRole : quint8 {
    Regular,
    Bold,
    Italic,
    BoldItalic,
    Count,
};

namespace TerminalFontStyles {

struct Automatic {
    bool operator==(const Automatic &) const = default;
};

struct Disabled {
    bool operator==(const Disabled &) const = default;
};

struct Named {
    QString name;

    bool operator==(const Named &) const = default;
};

} // namespace TerminalFontStyles

using TerminalFontStyle =
    std::variant<TerminalFontStyles::Automatic, TerminalFontStyles::Disabled,
                 TerminalFontStyles::Named>;

template <typename Value> struct TerminalOpenTypeSetting {
    // OpenType tags use the same big-endian integer representation as
    // QFont::Tag: `wght` is 0x77676874.
    quint32 tag = 0;
    Value value{};

    bool operator==(const TerminalOpenTypeSetting &) const = default;
};

using TerminalFontFeature = TerminalOpenTypeSetting<quint32>;

struct TerminalFontVariation {
    quint32 tag = 0;
    // Store the finalized f64 bit pattern so -0, infinities, and NaNs can
    // cross the strict JSON schema without relying on non-standard numbers.
    quint64 valueBits = 0;

    [[nodiscard]] static TerminalFontVariation fromValue(quint32 settingTag,
                                                         double value) noexcept
    {
        return {
            .tag = settingTag,
            .valueBits = std::bit_cast<quint64>(value),
        };
    }

    [[nodiscard]] double value() const noexcept
    {
        return std::bit_cast<double>(valueBits);
    }

    bool operator==(const TerminalFontVariation &) const = default;
};

struct TerminalFontFace {
    QStringList families;
    TerminalFontStyle style = TerminalFontStyles::Automatic{};
    QVector<TerminalFontVariation> variations;

    bool operator==(const TerminalFontFace &) const = default;
};

struct TerminalCodepointFontMap {
    // Pinned Ghostty parses these as u21 ranges rather than restricting them
    // to Unicode scalar values. Values outside Unicode simply never match a
    // QString cell.
    quint32 first = 0;
    quint32 last = 0;
    QString family;

    bool operator==(const TerminalCodepointFontMap &) const = default;
};

struct TerminalSyntheticStyle {
    bool bold = true;
    bool italic = true;
    bool boldItalic = true;

    bool operator==(const TerminalSyntheticStyle &) const = default;
};

struct TerminalFreetypeLoadFlags {
    bool hinting = true;
    bool forceAutohint = false;
    bool monochrome = false;
    bool autohint = true;
    bool light = true;

    bool operator==(const TerminalFreetypeLoadFlags &) const = default;
};

namespace TerminalMetricModifiers {

struct Absolute {
    qint32 pixels = 0;

    bool operator==(const Absolute &) const = default;
};

// Ghostty parses a textual delta such as 20% into the multiplier 1.2.
struct Percentage {
    double multiplier = 1.0;

    bool operator==(const Percentage &) const = default;
};

} // namespace TerminalMetricModifiers

using TerminalMetricModifier =
    std::variant<TerminalMetricModifiers::Absolute,
                 TerminalMetricModifiers::Percentage>;

enum class TerminalMetric : quint8 {
    CellWidth,
    CellHeight,
    FontBaseline,
    UnderlinePosition,
    UnderlineThickness,
    StrikethroughPosition,
    StrikethroughThickness,
    OverlinePosition,
    OverlineThickness,
    CursorThickness,
    CursorHeight,
    Count,
};

template <typename Enum>
[[nodiscard]] constexpr std::size_t terminalEnumIndex(Enum value) noexcept
{
    return static_cast<std::size_t>(std::to_underlying(value));
}

struct TerminalMetricModifierSet {
    using Value = std::optional<TerminalMetricModifier>;
    std::array<Value, terminalEnumIndex(TerminalMetric::Count)> values;
    // Ghostty applies modifiers by iterating its sparse map. The config
    // helper exports that pinned order because cell-height recentering makes
    // it observable when combined with percentage position adjustments.
    // Directly constructed values may leave this empty and use enum order.
    std::vector<TerminalMetric> applicationOrder;

    [[nodiscard]] Value &operator[](TerminalMetric metric) noexcept
    {
        return values[terminalEnumIndex(metric)];
    }

    [[nodiscard]] const Value &operator[](TerminalMetric metric) const noexcept
    {
        return values[terminalEnumIndex(metric)];
    }

    bool operator==(const TerminalMetricModifierSet &) const = default;
};

struct TerminalTypography {
    std::array<TerminalFontFace, terminalEnumIndex(TerminalFontRole::Count)>
        faces;
    double pointSize = 12.0;
    QVector<TerminalFontFeature> features;
    QVector<TerminalCodepointFontMap> codepointMap;
    TerminalSyntheticStyle syntheticStyle;
    bool shapingBreakCursor = true;
    TerminalFreetypeLoadFlags freetypeLoadFlags;
    TerminalMetricModifierSet metricModifiers;

    [[nodiscard]] TerminalFontFace &face(TerminalFontRole role) noexcept
    {
        return faces[terminalEnumIndex(role)];
    }

    [[nodiscard]] const TerminalFontFace &
    face(TerminalFontRole role) const noexcept
    {
        return faces[terminalEnumIndex(role)];
    }

    bool operator==(const TerminalTypography &) const = default;
};
