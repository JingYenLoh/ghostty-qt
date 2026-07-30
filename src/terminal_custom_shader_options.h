#pragma once

#include "ghostty_config_path.h"

#include <QVector>

// Ghostty spells these values `false`, `true`, and `always`. `Focused`
// intentionally names the behavior of the `true` value at the frontend
// boundary instead of carrying a boolean whose meaning is easy to invert.
enum class TerminalCustomShaderAnimation {
    Never,
    Focused,
    Always,
};

struct TerminalCustomShaderOptions {
    // Shader order is part of the rendering contract: every shader consumes
    // the previous shader's output through iChannel0.
    QVector<GhosttyConfigPath> sources;
    TerminalCustomShaderAnimation animation =
        TerminalCustomShaderAnimation::Focused;

    bool operator==(const TerminalCustomShaderOptions &) const = default;
};
