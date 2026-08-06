#pragma once

#include <QKeyEvent>

#include <optional>

// QKeyEvent cannot carry frontend-owned metadata. Deferred/replayed events use
// this GUI-thread scope so every filter observes the composition state from
// the time the original event arrived, rather than the pane's later state.
[[nodiscard]] std::optional<bool>
keyEventCompositionOverride(const QKeyEvent &event) noexcept;

// Ghostty's Kitty encoder continues to report these keys while composition is
// active. All other physical keys belong to the input method until commit.
[[nodiscard]] bool isCompositionModifierKey(int key) noexcept;

class ScopedKeyEventComposition final {
public:
    ScopedKeyEventComposition(const QKeyEvent &event, bool composing) noexcept;
    ~ScopedKeyEventComposition();

    ScopedKeyEventComposition(const ScopedKeyEventComposition &) = delete;
    ScopedKeyEventComposition &
    operator=(const ScopedKeyEventComposition &) = delete;

private:
    const QKeyEvent *previousEvent_ = nullptr;
    bool previousComposing_ = false;
    bool hadPrevious_ = false;
};
