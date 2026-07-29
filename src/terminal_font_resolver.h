#pragma once

#include "terminal_typography.h"

#include <QFont>
#include <QRawFont>
#include <QStringView>
#include <QVector>

#include <array>
#include <memory>

struct TerminalMappedFontFace {
    QFont font;
    QRawFont rawFont;

    bool operator==(const TerminalMappedFontFace &) const = default;
};

struct TerminalMappedFontInterval {
    quint32 first = 0;
    quint32 last = 0;
    // Negative means the latest owning declaration could not be resolved and
    // deliberately masks every earlier mapping.
    qsizetype faceIndex = -1;
    qsizetype sourceIndex = -1;

    bool operator==(const TerminalMappedFontInterval &) const = default;
};

struct TerminalFontProgram {
    // Metric fonts contain face, variation, synthesis, and hinting choices.
    // Shaping-only OpenType features are applied to `fonts` afterward so a
    // feature such as `pwid` cannot redefine the terminal grid.
    std::array<QFont, terminalEnumIndex(TerminalFontRole::Count)> metricFonts;
    std::array<QFont, terminalEnumIndex(TerminalFontRole::Count)> fonts;
    QVector<TerminalMappedFontFace> mappedFaces;
    QVector<TerminalMappedFontInterval> mappedIntervals;

    bool operator==(const TerminalFontProgram &) const = default;
};

// Uses Qt's GUI-thread font database. Equal font-affecting typography shares
// one immutable program across panes, DPRs, and metric-only reloads. The weak
// process cache is invalidated when Qt reports a font-database change.
[[nodiscard]] std::shared_ptr<const TerminalFontProgram>
terminalFontProgram(const TerminalTypography &typography);

// Applies Ghostty's later-entry-wins range policy with logarithmic lookup. A
// mapped face is returned only when its primary raw face supports the complete
// cell grapheme.
[[nodiscard]] const QFont &terminalFontForText(const TerminalFontProgram &fonts,
                                               TerminalFontRole role,
                                               QStringView text) noexcept;
