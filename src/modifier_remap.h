#pragma once

#include "key_event_snapshot.h"
#include "modifier_remap_types.h"

#include <QVector>

#include <span>

// Value representation of Ghostty's four bindable modifier keys plus their
// independently tracked sides. Qt collapses left and right modifiers in
// QKeyEvent::modifiers(), so rightSides retains the side information that is
// available for the physical modifier key which originated an event.
struct ModifierRemapState final {
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    quint8 rightSides = 0;

    [[nodiscard]] bool contains(ModifierKey key) const noexcept;
    [[nodiscard]] ModifierSide side(ModifierKey key) const noexcept;

    bool operator==(const ModifierRemapState &) const = default;
};

// Immutable-at-use modifier preprocessing matching Ghostty's finalized
// RemapSet semantics. Replacing mappings is a configuration operation; event
// application is allocation-free and scans at most the small finalized table.
class ModifierRemapEngine final {
public:
    ModifierRemapEngine() = default;
    explicit ModifierRemapEngine(
        std::span<const ModifierRemap> orderedMappings);

    void replaceMappings(std::span<const ModifierRemap> orderedMappings);

    // Apply exactly one finalized mapping. This overload keeps side state
    // available for a future terminal-encoding integration.
    [[nodiscard]] ModifierRemapState
    remap(ModifierRemapState state) const noexcept;

    // Derive Ghostty-style modifier state from one Qt event. Modifiers on an
    // ordinary event use Qt's generic (left-defaulted) state. When the event
    // itself is a physical modifier, its XKB scan code supplies the side and
    // its press/repeat/release action fixes that modifier's active bit.
    [[nodiscard]] ModifierRemapState
    remapModifiers(const KeyEventSnapshot &event) const noexcept;

    // Convenience for consumers which already pass owning snapshots through
    // a deferred input pipeline. Text and native key identity are deliberately
    // unchanged: key-remap affects modifiers, not keyboard layout.
    [[nodiscard]] KeyEventSnapshot remap(KeyEventSnapshot event) const noexcept;

private:
    QVector<ModifierRemap> mappings_;
    quint8 sourceKeys_ = 0;
    quint8 sourceLeftSides_ = 0;
    quint8 sourceRightSides_ = 0;
};

// Stateful Qt adapter for ModifierRemapEngine. Qt reports only the generic
// modifier bits on ordinary key events, so remember the side observed on the
// physical modifier press and reuse it for the rest of that chord. The pure
// engine above remains useful for finalized-value tests and non-event callers.
class ModifierRemapTracker final {
public:
    ModifierRemapTracker() = default;
    explicit ModifierRemapTracker(
        std::span<const ModifierRemap> orderedMappings);

    void replaceMappings(std::span<const ModifierRemap> orderedMappings);
    void resetState() noexcept;

    [[nodiscard]] KeyEventSnapshot remapEvent(KeyEventSnapshot event) noexcept;

private:
    ModifierRemapEngine engine_;
    quint8 rightSides_ = 0;
};
