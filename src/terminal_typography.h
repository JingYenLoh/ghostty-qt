#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <array>
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

struct TerminalFontFace {
    QStringList families;
    TerminalFontStyle style = TerminalFontStyles::Automatic{};

    bool operator==(const TerminalFontFace &) const = default;
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
