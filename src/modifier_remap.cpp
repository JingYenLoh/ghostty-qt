#include "modifier_remap.h"

#include "ghostty_key_identity.h"

#include <array>
#include <cstdint>
#include <linux/input-event-codes.h>
#include <optional>

namespace {

constexpr quint8 modifierBit(ModifierKey key) noexcept
{
    switch (key) {
    case ModifierKey::Shift: return 1U << 0U;
    case ModifierKey::Ctrl: return 1U << 1U;
    case ModifierKey::Alt: return 1U << 2U;
    case ModifierKey::Super: return 1U << 3U;
    }
    return 0;
}

constexpr Qt::KeyboardModifier qtModifier(ModifierKey key) noexcept
{
    switch (key) {
    case ModifierKey::Shift: return Qt::ShiftModifier;
    case ModifierKey::Ctrl: return Qt::ControlModifier;
    case ModifierKey::Alt: return Qt::AltModifier;
    case ModifierKey::Super: return Qt::MetaModifier;
    }
    return Qt::NoModifier;
}

constexpr Qt::KeyboardModifiers RelevantModifiers = Qt::ShiftModifier
    | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;

constexpr std::array ModifierKeys{
    ModifierKey::Shift,
    ModifierKey::Ctrl,
    ModifierKey::Alt,
    ModifierKey::Super,
};

constexpr quint32 xkbKeycode(std::uint32_t evdevCode) noexcept
{
    // Linux XKB keycodes are evdev codes plus the protocol's historical
    // eight-code offset. Qt's Wayland and X11 backends expose these through
    // QKeyEvent::nativeScanCode().
    return static_cast<quint32>(evdevCode + 8U);
}

std::optional<SidedModifier> physicalModifier(quint32 nativeScanCode) noexcept
{
    switch (nativeScanCode) {
    case xkbKeycode(KEY_LEFTSHIFT):
        return SidedModifier{
            .key = ModifierKey::Shift,
            .side = ModifierSide::Left,
        };
    case xkbKeycode(KEY_RIGHTSHIFT):
        return SidedModifier{
            .key = ModifierKey::Shift,
            .side = ModifierSide::Right,
        };
    case xkbKeycode(KEY_LEFTCTRL):
        return SidedModifier{
            .key = ModifierKey::Ctrl,
            .side = ModifierSide::Left,
        };
    case xkbKeycode(KEY_RIGHTCTRL):
        return SidedModifier{
            .key = ModifierKey::Ctrl,
            .side = ModifierSide::Right,
        };
    case xkbKeycode(KEY_LEFTALT):
        return SidedModifier{
            .key = ModifierKey::Alt,
            .side = ModifierSide::Left,
        };
    case xkbKeycode(KEY_RIGHTALT):
        return SidedModifier{
            .key = ModifierKey::Alt,
            .side = ModifierSide::Right,
        };
    case xkbKeycode(KEY_LEFTMETA):
        return SidedModifier{
            .key = ModifierKey::Super,
            .side = ModifierSide::Left,
        };
    case xkbKeycode(KEY_RIGHTMETA):
        return SidedModifier{
            .key = ModifierKey::Super,
            .side = ModifierSide::Right,
        };
    default: return std::nullopt;
    }
}

std::optional<SidedModifier> eventModifier(const KeyEventSnapshot &event,
                                           quint32 resolvedKeysym) noexcept
{
    if (resolvedKeysym != 0) {
        const GhosttyKey effective = ghosttyEffectiveKey(
            event.nativeScanCode, resolvedKeysym, event.key, event.modifiers);
        switch (effective) {
        case GHOSTTY_KEY_SHIFT_LEFT:
            return SidedModifier{ModifierKey::Shift, ModifierSide::Left};
        case GHOSTTY_KEY_SHIFT_RIGHT:
            return SidedModifier{ModifierKey::Shift, ModifierSide::Right};
        case GHOSTTY_KEY_CONTROL_LEFT:
            return SidedModifier{ModifierKey::Ctrl, ModifierSide::Left};
        case GHOSTTY_KEY_CONTROL_RIGHT:
            return SidedModifier{ModifierKey::Ctrl, ModifierSide::Right};
        case GHOSTTY_KEY_ALT_LEFT:
            return SidedModifier{ModifierKey::Alt, ModifierSide::Left};
        case GHOSTTY_KEY_ALT_RIGHT:
            return SidedModifier{ModifierKey::Alt, ModifierSide::Right};
        case GHOSTTY_KEY_META_LEFT:
            return SidedModifier{ModifierKey::Super, ModifierSide::Left};
        case GHOSTTY_KEY_META_RIGHT:
            return SidedModifier{ModifierKey::Super, ModifierSide::Right};
        default: return std::nullopt;
        }
    }

    if (const auto physical = physicalModifier(event.nativeScanCode)) {
        return physical;
    }

    // With no native identity, mirror Ghostty's default unsided value: left.
    const auto left = [](ModifierKey key) {
        return SidedModifier{
            .key = key,
            .side = ModifierSide::Left,
        };
    };
    switch (event.key) {
    case Qt::Key_Shift: return left(ModifierKey::Shift);
    case Qt::Key_Control: return left(ModifierKey::Ctrl);
    case Qt::Key_Alt:
    case Qt::Key_AltGr: return left(ModifierKey::Alt);
    case Qt::Key_Meta: return left(ModifierKey::Super);
    default: return std::nullopt;
    }
}

quint8 modifierKeys(Qt::KeyboardModifiers modifiers) noexcept
{
    quint8 result = 0;
    for (const ModifierKey key : ModifierKeys) {
        if (modifiers.testFlag(qtModifier(key))) {
            result |= modifierBit(key);
        }
    }
    return result;
}

Qt::KeyboardModifiers qtModifiers(quint8 keys) noexcept
{
    Qt::KeyboardModifiers result = Qt::NoModifier;
    for (const ModifierKey key : ModifierKeys) {
        if ((keys & modifierBit(key)) != 0) {
            result |= qtModifier(key);
        }
    }
    return result;
}

} // namespace

bool ModifierRemapState::contains(ModifierKey key) const noexcept
{
    return modifiers.testFlag(qtModifier(key));
}

ModifierSide ModifierRemapState::side(ModifierKey key) const noexcept
{
    return (rightSides & modifierBit(key)) != 0 ? ModifierSide::Right
                                                : ModifierSide::Left;
}

ModifierRemapEngine::ModifierRemapEngine(
    std::span<const ModifierRemap> orderedMappings)
{
    replaceMappings(orderedMappings);
}

void ModifierRemapEngine::replaceMappings(
    std::span<const ModifierRemap> orderedMappings)
{
    mappings_.assign(orderedMappings.begin(), orderedMappings.end());
    sourceKeys_ = 0;
    sourceLeftSides_ = 0;
    sourceRightSides_ = 0;
    for (const ModifierRemap &mapping : mappings_) {
        const quint8 source = modifierBit(mapping.from.key);
        sourceKeys_ |= source;
        if (mapping.from.side == ModifierSide::Right) {
            sourceRightSides_ |= source;
        } else {
            sourceLeftSides_ |= source;
        }
    }
}

ModifierRemapState
ModifierRemapEngine::remap(ModifierRemapState state) const noexcept
{
    quint8 keys = modifierKeys(state.modifiers);
    const quint8 activeSources = keys & sourceKeys_;
    const quint8 sideMatches =
        (static_cast<quint8>(~state.rightSides) & sourceLeftSides_)
        | (state.rightSides & sourceRightSides_);
    if ((activeSources & sideMatches) == 0) {
        return state;
    }

    for (const ModifierRemap &mapping : mappings_) {
        const quint8 source = modifierBit(mapping.from.key);
        if ((keys & source) == 0
            || state.side(mapping.from.key) != mapping.from.side) {
            continue;
        }

        // This deliberately mirrors Ghostty's integer `&~ from; | to`
        // operation. In particular, mapping to the left side does not clear a
        // right-side bit already held by the destination modifier.
        keys &= static_cast<quint8>(~source);
        if (mapping.from.side == ModifierSide::Right) {
            state.rightSides &= static_cast<quint8>(~source);
        }

        const quint8 destination = modifierBit(mapping.to.key);
        keys |= destination;
        if (mapping.to.side == ModifierSide::Right) {
            state.rightSides |= destination;
        }

        state.modifiers =
            (state.modifiers & ~RelevantModifiers) | qtModifiers(keys);
        return state;
    }

    return state;
}

ModifierRemapState
ModifierRemapEngine::remapModifiers(const KeyEventSnapshot &event,
                                    quint32 resolvedKeysym) const noexcept
{
    ModifierRemapState state{
        .modifiers = event.modifiers,
        .rightSides = 0,
    };
    if (const auto current = eventModifier(event, resolvedKeysym)) {
        const quint8 currentBit = modifierBit(current->key);
        if (event.pressed) {
            state.modifiers |= qtModifier(current->key);
        } else {
            state.modifiers &= ~Qt::KeyboardModifiers(qtModifier(current->key));
        }
        if (current->side == ModifierSide::Right) {
            state.rightSides |= currentBit;
        }
    }
    return remap(state);
}

KeyEventSnapshot
ModifierRemapEngine::remap(KeyEventSnapshot event,
                           quint32 resolvedKeysym) const noexcept
{
    event.modifiers = remapModifiers(event, resolvedKeysym).modifiers;
    return event;
}

ModifierRemapTracker::ModifierRemapTracker(
    std::span<const ModifierRemap> orderedMappings)
    : engine_(orderedMappings)
{}

void ModifierRemapTracker::replaceMappings(
    std::span<const ModifierRemap> orderedMappings)
{
    engine_.replaceMappings(orderedMappings);
    resetState();
}

void ModifierRemapTracker::resetState() noexcept
{
    rightSides_ = 0;
}

KeyEventSnapshot
ModifierRemapTracker::remapEvent(KeyEventSnapshot event,
                                 quint32 resolvedKeysym) noexcept
{
    ModifierRemapState state{
        .modifiers = event.modifiers,
        .rightSides = rightSides_,
    };
    if (const auto current = eventModifier(event, resolvedKeysym)) {
        const quint8 currentBit = modifierBit(current->key);
        if (event.pressed) {
            state.modifiers |= qtModifier(current->key);
            if (current->side == ModifierSide::Right) {
                rightSides_ |= currentBit;
            } else {
                rightSides_ &= static_cast<quint8>(~currentBit);
            }
        } else {
            state.modifiers &= ~Qt::KeyboardModifiers(qtModifier(current->key));
            rightSides_ &= static_cast<quint8>(~currentBit);
        }
    }

    // A missed release (for example, after focus leaves the surface) must not
    // retain a stale right-side classification. Qt's current generic mask is
    // authoritative for which modifier families remain active.
    rightSides_ &= modifierKeys(state.modifiers);
    state.rightSides = rightSides_;

    event.modifiers = engine_.remap(state).modifiers;
    return event;
}
