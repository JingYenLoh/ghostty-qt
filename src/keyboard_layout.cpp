#include "keyboard_layout.h"

#include "unique_file_descriptor.h"

#include <QByteArray>
#include <QGuiApplication>
#include <QtGui/qguiapplication_platform.h>

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon-names.h>
#include <xkbcommon/xkbcommon.h>

#include <sys/mman.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace {

constexpr std::size_t kMaximumKeymapBytes = 16U * 1024U * 1024U;

struct XkbContextDeleter final {
    void operator()(xkb_context *context) const noexcept
    {
        if (context != nullptr) xkb_context_unref(context);
    }
};

struct XkbKeymapDeleter final {
    void operator()(xkb_keymap *keymap) const noexcept
    {
        if (keymap != nullptr) xkb_keymap_unref(keymap);
    }
};

struct XkbStateDeleter final {
    void operator()(xkb_state *state) const noexcept
    {
        if (state != nullptr) xkb_state_unref(state);
    }
};

using XkbContextPtr = std::unique_ptr<xkb_context, XkbContextDeleter>;
using XkbKeymapPtr = std::unique_ptr<xkb_keymap, XkbKeymapDeleter>;
using XkbStatePtr = std::unique_ptr<xkb_state, XkbStateDeleter>;

bool isUnicodeScalar(std::uint32_t codepoint)
{
    return codepoint <= 0x10ffffU
        && !(codepoint >= 0xd800U && codepoint <= 0xdfffU);
}

std::uint32_t fallbackUnshiftedCodepoint(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return static_cast<std::uint32_t>('a' + key - Qt::Key_A);
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return static_cast<std::uint32_t>('0' + key - Qt::Key_0);
    }
    switch (key) {
    case Qt::Key_Space: return ' ';
    case Qt::Key_QuoteLeft:
    case Qt::Key_AsciiTilde: return '`';
    case Qt::Key_Backslash:
    case Qt::Key_Bar: return '\\';
    case Qt::Key_BracketLeft:
    case Qt::Key_BraceLeft: return '[';
    case Qt::Key_BracketRight:
    case Qt::Key_BraceRight: return ']';
    case Qt::Key_Comma:
    case Qt::Key_Less: return ',';
    case Qt::Key_Equal:
    case Qt::Key_Plus: return '=';
    case Qt::Key_Minus:
    case Qt::Key_Underscore: return '-';
    case Qt::Key_Period:
    case Qt::Key_Greater: return '.';
    case Qt::Key_Apostrophe:
    case Qt::Key_QuoteDbl: return '\'';
    case Qt::Key_Semicolon:
    case Qt::Key_Colon: return ';';
    case Qt::Key_Slash:
    case Qt::Key_Question: return '/';
    default: return 0;
    }
}

std::optional<char32_t> singlePrintableCodepoint(QStringView text)
{
    char32_t codepoint = 0;
    if (text.size() == 1 && !text.front().isSurrogate()) {
        codepoint = text.front().unicode();
    } else if (text.size() == 2 && text.front().isHighSurrogate()
               && text.back().isLowSurrogate()) {
        codepoint = QChar::surrogateToUcs4(text.front(), text.back());
    } else {
        return std::nullopt;
    }

    if (codepoint < 0x20U || codepoint == 0x7fU) return std::nullopt;
    return codepoint;
}

KeyboardLayoutTranslation fallbackTranslation(const QKeyEvent &event)
{
    KeyboardLayoutTranslation result{
        .unshiftedCodepoint = fallbackUnshiftedCodepoint(event.key()),
        .resolvedKeysym = event.nativeVirtualKey(),
    };
    const Qt::KeyboardModifiers modifiers =
        event.modifiers() & ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
    if (!modifiers.testFlag(Qt::ShiftModifier)) return result;

    const std::optional<char32_t> produced =
        singlePrintableCodepoint(event.text());
    if (result.unshiftedCodepoint != 0 && produced.has_value()
        && static_cast<std::uint32_t>(*produced) != result.unshiftedCodepoint) {
        result.consumedModifiers = Qt::ShiftModifier;
    }
    return result;
}

WaylandKeyboardLayout *activeWaylandLayout = nullptr;
thread_local const QKeyEvent *overrideEvent = nullptr;
thread_local KeyboardLayoutTranslation overrideTranslation;
thread_local bool overrideActive = false;

} // namespace

class XkbKeyboardLayout::Impl final {
public:
    Impl()
        : context(xkb_context_new(XKB_CONTEXT_NO_FLAGS))
    {}

    [[nodiscard]] xkb_mod_mask_t modifierMask(const char *name) const
    {
        if (keymap == nullptr) return 0;
        const xkb_mod_index_t index =
            xkb_keymap_mod_get_index(keymap.get(), name);
        if (index == XKB_MOD_INVALID
            || index >= std::numeric_limits<xkb_mod_mask_t>::digits) {
            return 0;
        }
        return xkb_mod_mask_t{1} << index;
    }

    [[nodiscard]] Qt::KeyboardModifiers
    qtConsumedModifiers(xkb_mod_mask_t consumed, xkb_mod_mask_t active) const
    {
        consumed &= active;
        Qt::KeyboardModifiers result = Qt::NoModifier;
        const auto append = [this, consumed, &result](const char *name,
                                                      auto modifier) {
            if ((consumed & modifierMask(name)) != 0) result |= modifier;
        };
        append(XKB_MOD_NAME_SHIFT, Qt::ShiftModifier);
        append(XKB_MOD_NAME_CTRL, Qt::ControlModifier);
        append(XKB_MOD_NAME_MOD1, Qt::AltModifier);
        append(XKB_MOD_NAME_MOD4, Qt::MetaModifier);
        // Qt represents ISO_Level3_Shift as GroupSwitch, not synthetic
        // Ctrl+Alt. Ghostty has no AltGr bit and therefore correctly ignores
        // this consumed layout modifier when encoding terminal modifiers.
        append(XKB_MOD_NAME_MOD5, Qt::GroupSwitchModifier);
        return result;
    }

    void applyModifiers(xkb_state *target, quint32 effective,
                        quint32 activeGroup) const
    {
        if (target == nullptr) return;
        (void)xkb_state_update_mask(target, effective, 0, 0, 0, 0, activeGroup);
    }

    XkbContextPtr context;
    XkbKeymapPtr keymap;
    XkbStatePtr state;
    mutable XkbStatePtr queryState;
    quint32 depressed = 0;
    quint32 latched = 0;
    quint32 locked = 0;
    quint32 group = 0;
};

XkbKeyboardLayout::XkbKeyboardLayout()
    : impl_(std::make_unique<Impl>())
{}

XkbKeyboardLayout::~XkbKeyboardLayout() = default;

bool XkbKeyboardLayout::installKeymap(QByteArrayView keymap)
{
    if (impl_->context == nullptr || keymap.isEmpty()) return false;

    QByteArray terminated(keymap.data(), keymap.size());
    if (terminated.back() != '\0') terminated.append('\0');
    XkbKeymapPtr replacement(xkb_keymap_new_from_string(
        impl_->context.get(), terminated.constData(), XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS));
    if (replacement == nullptr) return false;

    XkbStatePtr replacementState(xkb_state_new(replacement.get()));
    XkbStatePtr replacementQueryState(xkb_state_new(replacement.get()));
    if (replacementState == nullptr || replacementQueryState == nullptr) {
        return false;
    }

    (void)xkb_state_update_mask(replacementState.get(), impl_->depressed,
                                impl_->latched, impl_->locked, 0, 0,
                                impl_->group);
    impl_->applyModifiers(replacementQueryState.get(),
                          impl_->depressed | impl_->latched | impl_->locked,
                          impl_->group);
    impl_->keymap = std::move(replacement);
    impl_->state = std::move(replacementState);
    impl_->queryState = std::move(replacementQueryState);
    return true;
}

void XkbKeyboardLayout::clear()
{
    impl_->queryState.reset();
    impl_->state.reset();
    impl_->keymap.reset();
    impl_->depressed = 0;
    impl_->latched = 0;
    impl_->locked = 0;
    impl_->group = 0;
}

void XkbKeyboardLayout::updateModifiers(quint32 depressed, quint32 latched,
                                        quint32 locked, quint32 group)
{
    impl_->depressed = depressed;
    impl_->latched = latched;
    impl_->locked = locked;
    impl_->group = group;
    if (impl_->state != nullptr) {
        (void)xkb_state_update_mask(impl_->state.get(), depressed, latched,
                                    locked, 0, 0, group);
    }
}

void XkbKeyboardLayout::resetModifiers()
{
    updateModifiers(0, 0, 0, 0);
}

KeyboardLayoutTranslation
XkbKeyboardLayout::translate(quint32 nativeScanCode,
                             std::optional<quint32> effectiveModifiers,
                             std::optional<quint32> nativeVirtualKey) const
{
    if (impl_->keymap == nullptr || impl_->state == nullptr
        || nativeScanCode == 0
        || nativeScanCode < xkb_keymap_min_keycode(impl_->keymap.get())
        || nativeScanCode > xkb_keymap_max_keycode(impl_->keymap.get())) {
        return {};
    }

    xkb_mod_mask_t active = impl_->depressed | impl_->latched | impl_->locked;
    if (effectiveModifiers.has_value()) active = *effectiveModifiers;

    xkb_state *state = impl_->state.get();
    if (effectiveModifiers.has_value() || nativeVirtualKey.has_value()) {
        quint32 selectedGroup = impl_->group;
        impl_->applyModifiers(impl_->queryState.get(), active, selectedGroup);
        if (nativeVirtualKey.value_or(0) != 0
            && xkb_state_key_get_one_sym(impl_->queryState.get(),
                                         nativeScanCode)
                != *nativeVirtualKey) {
            bool matched = false;
            const xkb_layout_index_t layoutCount =
                xkb_keymap_num_layouts_for_key(impl_->keymap.get(),
                                               nativeScanCode);
            for (xkb_layout_index_t candidate = 0; candidate < layoutCount;
                 ++candidate) {
                impl_->applyModifiers(impl_->queryState.get(), active,
                                      candidate);
                if (xkb_state_key_get_one_sym(impl_->queryState.get(),
                                              nativeScanCode)
                    == *nativeVirtualKey) {
                    selectedGroup = candidate;
                    matched = true;
                    break;
                }
            }
            if (!matched) return {};
            impl_->applyModifiers(impl_->queryState.get(), active,
                                  selectedGroup);
        }
        state = impl_->queryState.get();
    }

    const xkb_layout_index_t layout =
        xkb_state_key_get_layout(state, nativeScanCode);
    if (layout == XKB_LAYOUT_INVALID) return {};

    const xkb_mod_mask_t consumed = xkb_state_key_get_consumed_mods2(
        state, nativeScanCode, XKB_CONSUMED_MODE_GTK);
    const bool capsLock = xkb_state_mod_name_is_active(
        state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_EFFECTIVE);
    const bool numLock = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_NUM,
                                                      XKB_STATE_MODS_EFFECTIVE);
    const bool consumedCapsLock =
        (consumed & active & impl_->modifierMask(XKB_MOD_NAME_CAPS)) != 0;
    const quint32 resolvedKeysym =
        xkb_state_key_get_one_sym(state, nativeScanCode);
    const auto result = [&](std::uint32_t codepoint) {
        return KeyboardLayoutTranslation{
            .unshiftedCodepoint = codepoint,
            .resolvedKeysym = resolvedKeysym,
            .consumedModifiers = impl_->qtConsumedModifiers(consumed, active),
            .capsLock = capsLock,
            .numLock = numLock,
            .consumedCapsLock = consumedCapsLock,
            .authoritative = true,
        };
    };

    const xkb_keysym_t *symbols = nullptr;
    const int symbolCount = xkb_keymap_key_get_syms_by_level(
        impl_->keymap.get(), nativeScanCode, layout, 0, &symbols);
    if (symbolCount <= 0 || symbols == nullptr) {
        return result(0);
    }

    std::uint32_t codepoint = 0;
    for (int index = 0; index < symbolCount && codepoint == 0; ++index) {
        const std::uint32_t candidate = xkb_keysym_to_utf32(symbols[index]);
        if (isUnicodeScalar(candidate)) codepoint = candidate;
    }
    return result(codepoint);
}

class WaylandKeyboardLayout::Impl final {
public:
    ~Impl() { releaseKeyboard(); }

    void ensureBound()
    {
        if (qGuiApp == nullptr) return;
        auto *native =
            qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        if (native == nullptr) return;

        wl_seat *const seat = native->seat();
        if (native->keyboard() == nullptr) {
            releaseKeyboard();
            return;
        }
        if (keyboard != nullptr && seat == boundSeat) return;
        if (seat == nullptr) return;

        releaseKeyboard();
        boundSeat = seat;
        keyboard = wl_seat_get_keyboard(boundSeat);
        if (keyboard == nullptr) {
            boundSeat = nullptr;
            return;
        }
        if (wl_keyboard_add_listener(keyboard, &listener, this) != 0) {
            releaseKeyboard();
        }
    }

    void releaseKeyboard()
    {
        layout.clear();
        if (keyboard != nullptr) {
            if (wl_proxy_get_version(reinterpret_cast<wl_proxy *>(keyboard))
                >= WL_KEYBOARD_RELEASE_SINCE_VERSION) {
                wl_keyboard_release(keyboard);
            } else {
                wl_keyboard_destroy(keyboard);
            }
        }
        keyboard = nullptr;
        boundSeat = nullptr;
    }

    static void keymap(void *data, wl_keyboard *, std::uint32_t format, int fd,
                       std::uint32_t size)
    {
        UniqueFileDescriptor descriptor(fd);
        auto &self = *static_cast<Impl *>(data);
        if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0
            || size > kMaximumKeymapBytes) {
            self.layout.clear();
            return;
        }

        void *const mapping =
            mmap(nullptr, size, PROT_READ, MAP_PRIVATE, descriptor.get(), 0);
        if (mapping == MAP_FAILED) {
            self.layout.clear();
            return;
        }
        const QByteArrayView bytes(static_cast<const char *>(mapping),
                                   static_cast<qsizetype>(size));
        if (!self.layout.installKeymap(bytes)) self.layout.clear();
        (void)munmap(mapping, size);
    }

    static void enter(void *, wl_keyboard *, std::uint32_t, wl_surface *,
                      wl_array *)
    {}
    static void leave(void *data, wl_keyboard *, std::uint32_t, wl_surface *)
    {
        static_cast<Impl *>(data)->layout.resetModifiers();
    }
    static void key(void *, wl_keyboard *, std::uint32_t, std::uint32_t,
                    std::uint32_t, std::uint32_t)
    {}
    static void modifiers(void *data, wl_keyboard *, std::uint32_t,
                          std::uint32_t depressed, std::uint32_t latched,
                          std::uint32_t locked, std::uint32_t group)
    {
        static_cast<Impl *>(data)->layout.updateModifiers(depressed, latched,
                                                          locked, group);
    }
    static void repeatInfo(void *, wl_keyboard *, std::int32_t, std::int32_t) {}

    inline static const wl_keyboard_listener listener{
        .keymap = keymap,
        .enter = enter,
        .leave = leave,
        .key = key,
        .modifiers = modifiers,
        .repeat_info = repeatInfo,
    };

    XkbKeyboardLayout layout;
    wl_seat *boundSeat = nullptr;
    wl_keyboard *keyboard = nullptr;
};

WaylandKeyboardLayout::WaylandKeyboardLayout()
    : impl_(std::make_unique<Impl>())
{
    Q_ASSERT(activeWaylandLayout == nullptr);
    activeWaylandLayout = this;
    impl_->ensureBound();
}

WaylandKeyboardLayout::~WaylandKeyboardLayout()
{
    Q_ASSERT(activeWaylandLayout == this);
    activeWaylandLayout = nullptr;
}

KeyboardLayoutTranslation
WaylandKeyboardLayout::translate(const QKeyEvent &event)
{
    impl_->ensureBound();
    if (qGuiApp == nullptr) return {};
    auto *native =
        qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (native == nullptr
        || (native->lastInputSeat() != nullptr
            && native->lastInputSeat() != impl_->boundSeat)) {
        return {};
    }
    return impl_->layout.translate(event.nativeScanCode(),
                                   event.nativeModifiers(),
                                   event.nativeVirtualKey());
}

KeyboardLayoutTranslation translateKeyboardLayout(const QKeyEvent &event)
{
    if (overrideActive && overrideEvent == &event) return overrideTranslation;
    if (activeWaylandLayout != nullptr) {
        KeyboardLayoutTranslation result =
            activeWaylandLayout->translate(event);
        if (result.authoritative) return result;
    }
    return fallbackTranslation(event);
}

ScopedKeyboardLayoutTranslation::ScopedKeyboardLayoutTranslation(
    const QKeyEvent &event, KeyboardLayoutTranslation translation)
    : previousEvent_(overrideEvent)
    , previousTranslation_(overrideTranslation)
    , hadPrevious_(overrideActive)
{
    overrideEvent = &event;
    overrideTranslation = translation;
    overrideActive = true;
}

ScopedKeyboardLayoutTranslation::~ScopedKeyboardLayoutTranslation()
{
    overrideEvent = previousEvent_;
    overrideTranslation = previousTranslation_;
    overrideActive = hadPrevious_;
}
