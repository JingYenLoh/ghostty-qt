#pragma once

#include <ghostty/vt.h>

#include <Qt>
#include <QtGlobal>

// Translate the three identities carried by a Qt/XKB event into Ghostty's
// physical-key model. The effective selector mirrors the pinned GTK frontend:
// a known XKB-resolved key wins when either it or the raw keycode-derived key
// is outside the W3C Writing System key set.
[[nodiscard]] GhosttyKey
ghosttyKeyFromQt(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
[[nodiscard]] GhosttyKey ghosttyKeyFromNativeScanCode(quint32 nativeScanCode);
[[nodiscard]] GhosttyKey ghosttyKeyFromXkbKeysym(quint32 keysym);
[[nodiscard]] bool ghosttyKeyShouldBeRemappable(GhosttyKey key) noexcept;
[[nodiscard]] GhosttyKey
ghosttyEffectiveKey(quint32 nativeScanCode, quint32 resolvedKeysym, int qtKey,
                    Qt::KeyboardModifiers modifiers = Qt::NoModifier);
[[nodiscard]] bool ghosttyKeyIsModifier(GhosttyKey key) noexcept;
