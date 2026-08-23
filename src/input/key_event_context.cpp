#include "input/key_event_context.h"

namespace {

thread_local const QKeyEvent *overrideEvent = nullptr;
thread_local bool overrideComposing = false;
thread_local bool overrideActive = false;

} // namespace

std::optional<bool> keyEventCompositionOverride(const QKeyEvent &event) noexcept
{
    if (!overrideActive || overrideEvent != &event) return std::nullopt;
    return overrideComposing;
}

bool isCompositionModifierKey(int key) noexcept
{
    switch (key) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_Meta:
    case Qt::Key_CapsLock:
    case Qt::Key_NumLock: return true;
    default: return false;
    }
}

ScopedKeyEventComposition::ScopedKeyEventComposition(const QKeyEvent &event,
                                                     bool composing) noexcept
    : previousEvent_(overrideEvent)
    , previousComposing_(overrideComposing)
    , hadPrevious_(overrideActive)
{
    overrideEvent = &event;
    overrideComposing = composing;
    overrideActive = true;
}

ScopedKeyEventComposition::~ScopedKeyEventComposition()
{
    overrideEvent = previousEvent_;
    overrideComposing = previousComposing_;
    overrideActive = hadPrevious_;
}
