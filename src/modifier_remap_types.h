#pragma once

enum class ModifierKey {
    Shift,
    Ctrl,
    Alt,
    Super,
};

enum class ModifierSide {
    Left,
    Right,
};

struct SidedModifier {
    ModifierKey key = ModifierKey::Shift;
    ModifierSide side = ModifierSide::Left;

    bool operator==(const SidedModifier &) const = default;
};

struct ModifierRemap {
    SidedModifier from;
    SidedModifier to;

    bool operator==(const ModifierRemap &) const = default;
};
