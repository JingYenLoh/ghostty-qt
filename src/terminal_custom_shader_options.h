#pragma once

#include "ghostty_config_path.h"

#include <QVector>

#include <chrono>

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
    // Lifecycle shaders are composed after the persistent Ghostty chain. The
    // compiler phase-gates them so they are exact pass-through stages outside
    // their matching transition.
    QVector<GhosttyConfigPath> paneEnterTransitionSources;
    QVector<GhosttyConfigPath> paneExitTransitionSources;
    TerminalCustomShaderAnimation animation =
        TerminalCustomShaderAnimation::Focused;
    // Qt-owned finite lifecycle animation windows. A zero duration preserves
    // Ghostty's ordinary immediate pane presentation and destruction.
    std::chrono::milliseconds paneEnterTransitionDuration{};
    std::chrono::milliseconds paneExitTransitionDuration{};

    bool operator==(const TerminalCustomShaderOptions &) const = default;
};
