#pragma once

#include "terminal_typography.h"

#include <QFont>
#include <QRawFont>
#include <QStringView>
#include <QVector>

#include <array>

struct TerminalMappedFont {
    TerminalCodepointFontMap range;
    QFont font;
    QRawFont rawFont;

    bool operator==(const TerminalMappedFont &) const = default;
};

struct TerminalResolvedFonts {
    // Metric fonts contain face, variation, synthesis, and hinting choices.
    // Shaping-only OpenType features are applied to `fonts` afterward so a
    // feature such as `pwid` cannot redefine the terminal grid.
    std::array<QFont, terminalEnumIndex(TerminalFontRole::Count)> metricFonts;
    std::array<QFont, terminalEnumIndex(TerminalFontRole::Count)> fonts;
    QVector<TerminalMappedFont> mappedFonts;

    bool operator==(const TerminalResolvedFonts &) const = default;
};

// Uses Qt's GUI-thread font database. Resolve once per typography generation;
// renderer-side cell lookup uses a sorted, disjoint interval table.
[[nodiscard]] TerminalResolvedFonts
resolveTerminalFonts(const TerminalTypography &typography);

// Applies Ghostty's later-entry-wins range policy with logarithmic lookup. A
// mapped face is returned only when its primary raw face supports the complete
// cell grapheme.
[[nodiscard]] const QFont &
terminalFontForText(const TerminalResolvedFonts &fonts, TerminalFontRole role,
                    QStringView text) noexcept;

[[nodiscard]] const QFont &terminalFontForText(
    const std::array<QFont, terminalEnumIndex(TerminalFontRole::Count)> &fonts,
    const QVector<TerminalMappedFont> &mappedFonts, TerminalFontRole role,
    QStringView text) noexcept;
