#pragma once

#include <QByteArrayView>
#include <QKeyEvent>
#include <Qt>
#include <QtGlobal>

#include <cstdint>
#include <memory>
#include <optional>

struct KeyboardLayoutTranslation final {
    std::uint32_t unshiftedCodepoint = 0;
    Qt::KeyboardModifiers consumedModifiers = Qt::NoModifier;
    bool capsLock = false;
    bool numLock = false;
    bool consumedCapsLock = false;
    // Zero is a valid result for dead keys and non-text keys. Callers must use
    // this flag, rather than the codepoint, to decide whether XKB answered.
    bool authoritative = false;
};

// Testable XKB keymap/state core. The Wayland adapter below feeds this from
// compositor events; keeping protocol ownership out of this class makes
// layout semantics deterministic under the offscreen test platform.
class XkbKeyboardLayout final {
public:
    XkbKeyboardLayout();
    ~XkbKeyboardLayout();

    XkbKeyboardLayout(const XkbKeyboardLayout &) = delete;
    XkbKeyboardLayout &operator=(const XkbKeyboardLayout &) = delete;

    [[nodiscard]] bool installKeymap(QByteArrayView keymap);
    void clear();
    void updateModifiers(quint32 depressed, quint32 latched, quint32 locked,
                         quint32 group);
    void resetModifiers();
    [[nodiscard]] KeyboardLayoutTranslation
    translate(quint32 nativeScanCode,
              std::optional<quint32> effectiveModifiers = std::nullopt,
              std::optional<quint32> nativeVirtualKey = std::nullopt) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Process-owned bridge to the compositor keymap. Construct this after
// QApplication and before the first window, and destroy it before QApplication.
class WaylandKeyboardLayout final {
public:
    WaylandKeyboardLayout();
    ~WaylandKeyboardLayout();

    WaylandKeyboardLayout(const WaylandKeyboardLayout &) = delete;
    WaylandKeyboardLayout &operator=(const WaylandKeyboardLayout &) = delete;

    [[nodiscard]] KeyboardLayoutTranslation translate(const QKeyEvent &event);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Returns compositor-derived data when available, otherwise the conservative
// Qt-key fallback used by synthetic/offscreen events.
[[nodiscard]] KeyboardLayoutTranslation
translateKeyboardLayout(const QKeyEvent &event);

// Replayed QKeyEvents cannot carry frontend metadata. This small GUI-thread
// scope lets nested application/pane filters retrieve the event-time result.
class ScopedKeyboardLayoutTranslation final {
public:
    ScopedKeyboardLayoutTranslation(const QKeyEvent &event,
                                    KeyboardLayoutTranslation translation);
    ~ScopedKeyboardLayoutTranslation();

    ScopedKeyboardLayoutTranslation(const ScopedKeyboardLayoutTranslation &) =
        delete;
    ScopedKeyboardLayoutTranslation &
    operator=(const ScopedKeyboardLayoutTranslation &) = delete;

private:
    const QKeyEvent *previousEvent_ = nullptr;
    KeyboardLayoutTranslation previousTranslation_;
    bool hadPrevious_ = false;
};
