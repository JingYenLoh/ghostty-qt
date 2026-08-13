#pragma once

#include <QtGlobal>

enum class GhosttyShellIntegrationMode : quint8 {
    None,
    Detect,
    Bash,
    Elvish,
    Fish,
    Nushell,
    Zsh,
};
