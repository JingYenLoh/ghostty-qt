#include "ghostty_key_identity.h"

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

GhosttyKey ghosttyKeyFromQt(int key, Qt::KeyboardModifiers modifiers)
{
    if (modifiers.testFlag(Qt::KeypadModifier)) {
        if (key >= Qt::Key_0 && key <= Qt::Key_9) {
            return static_cast<GhosttyKey>(GHOSTTY_KEY_NUMPAD_0 + key
                                           - Qt::Key_0);
        }
        switch (key) {
        case Qt::Key_Plus: return GHOSTTY_KEY_NUMPAD_ADD;
        case Qt::Key_Comma: return GHOSTTY_KEY_NUMPAD_COMMA;
        case Qt::Key_Period: return GHOSTTY_KEY_NUMPAD_DECIMAL;
        case Qt::Key_Slash: return GHOSTTY_KEY_NUMPAD_DIVIDE;
        case Qt::Key_Enter:
        case Qt::Key_Return: return GHOSTTY_KEY_NUMPAD_ENTER;
        case Qt::Key_Equal: return GHOSTTY_KEY_NUMPAD_EQUAL;
        case Qt::Key_Asterisk: return GHOSTTY_KEY_NUMPAD_MULTIPLY;
        case Qt::Key_Minus: return GHOSTTY_KEY_NUMPAD_SUBTRACT;
        case Qt::Key_Up: return GHOSTTY_KEY_NUMPAD_UP;
        case Qt::Key_Down: return GHOSTTY_KEY_NUMPAD_DOWN;
        case Qt::Key_Right: return GHOSTTY_KEY_NUMPAD_RIGHT;
        case Qt::Key_Left: return GHOSTTY_KEY_NUMPAD_LEFT;
        case Qt::Key_Home: return GHOSTTY_KEY_NUMPAD_HOME;
        case Qt::Key_End: return GHOSTTY_KEY_NUMPAD_END;
        case Qt::Key_Insert: return GHOSTTY_KEY_NUMPAD_INSERT;
        case Qt::Key_Delete: return GHOSTTY_KEY_NUMPAD_DELETE;
        case Qt::Key_PageUp: return GHOSTTY_KEY_NUMPAD_PAGE_UP;
        case Qt::Key_PageDown: return GHOSTTY_KEY_NUMPAD_PAGE_DOWN;
        default: break;
        }
    }

    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return static_cast<GhosttyKey>(GHOSTTY_KEY_A + key - Qt::Key_A);
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return static_cast<GhosttyKey>(GHOSTTY_KEY_DIGIT_0 + key - Qt::Key_0);
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F25) {
        return static_cast<GhosttyKey>(GHOSTTY_KEY_F1 + key - Qt::Key_F1);
    }

    switch (key) {
    case Qt::Key_QuoteLeft:
    case Qt::Key_AsciiTilde: return GHOSTTY_KEY_BACKQUOTE;
    case Qt::Key_Backslash:
    case Qt::Key_Bar: return GHOSTTY_KEY_BACKSLASH;
    case Qt::Key_BracketLeft:
    case Qt::Key_BraceLeft: return GHOSTTY_KEY_BRACKET_LEFT;
    case Qt::Key_BracketRight:
    case Qt::Key_BraceRight: return GHOSTTY_KEY_BRACKET_RIGHT;
    case Qt::Key_Comma:
    case Qt::Key_Less: return GHOSTTY_KEY_COMMA;
    case Qt::Key_Equal:
    case Qt::Key_Plus: return GHOSTTY_KEY_EQUAL;
    case Qt::Key_Minus:
    case Qt::Key_Underscore: return GHOSTTY_KEY_MINUS;
    case Qt::Key_Period:
    case Qt::Key_Greater: return GHOSTTY_KEY_PERIOD;
    case Qt::Key_Apostrophe:
    case Qt::Key_QuoteDbl: return GHOSTTY_KEY_QUOTE;
    case Qt::Key_Semicolon:
    case Qt::Key_Colon: return GHOSTTY_KEY_SEMICOLON;
    case Qt::Key_Slash:
    case Qt::Key_Question: return GHOSTTY_KEY_SLASH;
    case Qt::Key_Alt: return GHOSTTY_KEY_ALT_LEFT;
    case Qt::Key_AltGr: return GHOSTTY_KEY_ALT_RIGHT;
    case Qt::Key_Backspace: return GHOSTTY_KEY_BACKSPACE;
    case Qt::Key_CapsLock: return GHOSTTY_KEY_CAPS_LOCK;
    case Qt::Key_Menu: return GHOSTTY_KEY_CONTEXT_MENU;
    case Qt::Key_Control: return GHOSTTY_KEY_CONTROL_LEFT;
    case Qt::Key_Return:
    case Qt::Key_Enter: return GHOSTTY_KEY_ENTER;
    case Qt::Key_Meta: return GHOSTTY_KEY_META_LEFT;
    case Qt::Key_Shift: return GHOSTTY_KEY_SHIFT_LEFT;
    case Qt::Key_Space: return GHOSTTY_KEY_SPACE;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return GHOSTTY_KEY_TAB;
    case Qt::Key_Delete: return GHOSTTY_KEY_DELETE;
    case Qt::Key_End: return GHOSTTY_KEY_END;
    case Qt::Key_Help: return GHOSTTY_KEY_HELP;
    case Qt::Key_Home: return GHOSTTY_KEY_HOME;
    case Qt::Key_Insert: return GHOSTTY_KEY_INSERT;
    case Qt::Key_PageDown: return GHOSTTY_KEY_PAGE_DOWN;
    case Qt::Key_PageUp: return GHOSTTY_KEY_PAGE_UP;
    case Qt::Key_Down: return GHOSTTY_KEY_ARROW_DOWN;
    case Qt::Key_Left: return GHOSTTY_KEY_ARROW_LEFT;
    case Qt::Key_Right: return GHOSTTY_KEY_ARROW_RIGHT;
    case Qt::Key_Up: return GHOSTTY_KEY_ARROW_UP;
    case Qt::Key_NumLock: return GHOSTTY_KEY_NUM_LOCK;
    case Qt::Key_Escape: return GHOSTTY_KEY_ESCAPE;
    case Qt::Key_Pause: return GHOSTTY_KEY_PAUSE;
    case Qt::Key_Print: return GHOSTTY_KEY_PRINT_SCREEN;
    case Qt::Key_ScrollLock: return GHOSTTY_KEY_SCROLL_LOCK;
    case Qt::Key_Copy: return GHOSTTY_KEY_COPY;
    case Qt::Key_Cut: return GHOSTTY_KEY_CUT;
    case Qt::Key_Paste: return GHOSTTY_KEY_PASTE;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
}

GhosttyKey ghosttyKeyFromNativeScanCode(quint32 nativeScanCode)
{
    if (nativeScanCode < 8U) return GHOSTTY_KEY_UNIDENTIFIED;

    switch (nativeScanCode - 8U) {
    case KEY_GRAVE: return GHOSTTY_KEY_BACKQUOTE;
    case KEY_BACKSLASH: return GHOSTTY_KEY_BACKSLASH;
    case KEY_LEFTBRACE: return GHOSTTY_KEY_BRACKET_LEFT;
    case KEY_RIGHTBRACE: return GHOSTTY_KEY_BRACKET_RIGHT;
    case KEY_COMMA: return GHOSTTY_KEY_COMMA;
    case KEY_0: return GHOSTTY_KEY_DIGIT_0;
    case KEY_1: return GHOSTTY_KEY_DIGIT_1;
    case KEY_2: return GHOSTTY_KEY_DIGIT_2;
    case KEY_3: return GHOSTTY_KEY_DIGIT_3;
    case KEY_4: return GHOSTTY_KEY_DIGIT_4;
    case KEY_5: return GHOSTTY_KEY_DIGIT_5;
    case KEY_6: return GHOSTTY_KEY_DIGIT_6;
    case KEY_7: return GHOSTTY_KEY_DIGIT_7;
    case KEY_8: return GHOSTTY_KEY_DIGIT_8;
    case KEY_9: return GHOSTTY_KEY_DIGIT_9;
    case KEY_EQUAL: return GHOSTTY_KEY_EQUAL;
    case KEY_102ND: return GHOSTTY_KEY_INTL_BACKSLASH;
    case KEY_RO: return GHOSTTY_KEY_INTL_RO;
    case KEY_YEN: return GHOSTTY_KEY_INTL_YEN;
    case KEY_A: return GHOSTTY_KEY_A;
    case KEY_B: return GHOSTTY_KEY_B;
    case KEY_C: return GHOSTTY_KEY_C;
    case KEY_D: return GHOSTTY_KEY_D;
    case KEY_E: return GHOSTTY_KEY_E;
    case KEY_F: return GHOSTTY_KEY_F;
    case KEY_G: return GHOSTTY_KEY_G;
    case KEY_H: return GHOSTTY_KEY_H;
    case KEY_I: return GHOSTTY_KEY_I;
    case KEY_J: return GHOSTTY_KEY_J;
    case KEY_K: return GHOSTTY_KEY_K;
    case KEY_L: return GHOSTTY_KEY_L;
    case KEY_M: return GHOSTTY_KEY_M;
    case KEY_N: return GHOSTTY_KEY_N;
    case KEY_O: return GHOSTTY_KEY_O;
    case KEY_P: return GHOSTTY_KEY_P;
    case KEY_Q: return GHOSTTY_KEY_Q;
    case KEY_R: return GHOSTTY_KEY_R;
    case KEY_S: return GHOSTTY_KEY_S;
    case KEY_T: return GHOSTTY_KEY_T;
    case KEY_U: return GHOSTTY_KEY_U;
    case KEY_V: return GHOSTTY_KEY_V;
    case KEY_W: return GHOSTTY_KEY_W;
    case KEY_X: return GHOSTTY_KEY_X;
    case KEY_Y: return GHOSTTY_KEY_Y;
    case KEY_Z: return GHOSTTY_KEY_Z;
    case KEY_MINUS: return GHOSTTY_KEY_MINUS;
    case KEY_DOT: return GHOSTTY_KEY_PERIOD;
    case KEY_APOSTROPHE: return GHOSTTY_KEY_QUOTE;
    case KEY_SEMICOLON: return GHOSTTY_KEY_SEMICOLON;
    case KEY_SLASH: return GHOSTTY_KEY_SLASH;
    case KEY_LEFTALT: return GHOSTTY_KEY_ALT_LEFT;
    case KEY_RIGHTALT: return GHOSTTY_KEY_ALT_RIGHT;
    case KEY_BACKSPACE: return GHOSTTY_KEY_BACKSPACE;
    case KEY_CAPSLOCK: return GHOSTTY_KEY_CAPS_LOCK;
    case KEY_MENU: return GHOSTTY_KEY_CONTEXT_MENU;
    case KEY_LEFTCTRL: return GHOSTTY_KEY_CONTROL_LEFT;
    case KEY_RIGHTCTRL: return GHOSTTY_KEY_CONTROL_RIGHT;
    case KEY_ENTER: return GHOSTTY_KEY_ENTER;
    case KEY_LEFTMETA: return GHOSTTY_KEY_META_LEFT;
    case KEY_RIGHTMETA: return GHOSTTY_KEY_META_RIGHT;
    case KEY_LEFTSHIFT: return GHOSTTY_KEY_SHIFT_LEFT;
    case KEY_RIGHTSHIFT: return GHOSTTY_KEY_SHIFT_RIGHT;
    case KEY_SPACE: return GHOSTTY_KEY_SPACE;
    case KEY_TAB: return GHOSTTY_KEY_TAB;
    case KEY_DELETE: return GHOSTTY_KEY_DELETE;
    case KEY_END: return GHOSTTY_KEY_END;
    case KEY_HOME: return GHOSTTY_KEY_HOME;
    case KEY_INSERT: return GHOSTTY_KEY_INSERT;
    case KEY_PAGEDOWN: return GHOSTTY_KEY_PAGE_DOWN;
    case KEY_PAGEUP: return GHOSTTY_KEY_PAGE_UP;
    case KEY_DOWN: return GHOSTTY_KEY_ARROW_DOWN;
    case KEY_LEFT: return GHOSTTY_KEY_ARROW_LEFT;
    case KEY_RIGHT: return GHOSTTY_KEY_ARROW_RIGHT;
    case KEY_UP: return GHOSTTY_KEY_ARROW_UP;
    case KEY_NUMLOCK: return GHOSTTY_KEY_NUM_LOCK;
    case KEY_KP0: return GHOSTTY_KEY_NUMPAD_0;
    case KEY_KP1: return GHOSTTY_KEY_NUMPAD_1;
    case KEY_KP2: return GHOSTTY_KEY_NUMPAD_2;
    case KEY_KP3: return GHOSTTY_KEY_NUMPAD_3;
    case KEY_KP4: return GHOSTTY_KEY_NUMPAD_4;
    case KEY_KP5: return GHOSTTY_KEY_NUMPAD_5;
    case KEY_KP6: return GHOSTTY_KEY_NUMPAD_6;
    case KEY_KP7: return GHOSTTY_KEY_NUMPAD_7;
    case KEY_KP8: return GHOSTTY_KEY_NUMPAD_8;
    case KEY_KP9: return GHOSTTY_KEY_NUMPAD_9;
    case KEY_KPPLUS: return GHOSTTY_KEY_NUMPAD_ADD;
    case KEY_KPDOT: return GHOSTTY_KEY_NUMPAD_DECIMAL;
    case KEY_KPSLASH: return GHOSTTY_KEY_NUMPAD_DIVIDE;
    case KEY_KPENTER: return GHOSTTY_KEY_NUMPAD_ENTER;
    case KEY_KPEQUAL: return GHOSTTY_KEY_NUMPAD_EQUAL;
    case KEY_KPASTERISK: return GHOSTTY_KEY_NUMPAD_MULTIPLY;
    case KEY_KPMINUS: return GHOSTTY_KEY_NUMPAD_SUBTRACT;
    case KEY_ESC: return GHOSTTY_KEY_ESCAPE;
    case KEY_PAUSE: return GHOSTTY_KEY_PAUSE;
    case KEY_SYSRQ: return GHOSTTY_KEY_PRINT_SCREEN;
    case KEY_SCROLLLOCK: return GHOSTTY_KEY_SCROLL_LOCK;
    case KEY_COPY: return GHOSTTY_KEY_COPY;
    case KEY_CUT: return GHOSTTY_KEY_CUT;
    case KEY_PASTE: return GHOSTTY_KEY_PASTE;
    case KEY_F1: return GHOSTTY_KEY_F1;
    case KEY_F2: return GHOSTTY_KEY_F2;
    case KEY_F3: return GHOSTTY_KEY_F3;
    case KEY_F4: return GHOSTTY_KEY_F4;
    case KEY_F5: return GHOSTTY_KEY_F5;
    case KEY_F6: return GHOSTTY_KEY_F6;
    case KEY_F7: return GHOSTTY_KEY_F7;
    case KEY_F8: return GHOSTTY_KEY_F8;
    case KEY_F9: return GHOSTTY_KEY_F9;
    case KEY_F10: return GHOSTTY_KEY_F10;
    case KEY_F11: return GHOSTTY_KEY_F11;
    case KEY_F12: return GHOSTTY_KEY_F12;
    case KEY_F13: return GHOSTTY_KEY_F13;
    case KEY_F14: return GHOSTTY_KEY_F14;
    case KEY_F15: return GHOSTTY_KEY_F15;
    case KEY_F16: return GHOSTTY_KEY_F16;
    case KEY_F17: return GHOSTTY_KEY_F17;
    case KEY_F18: return GHOSTTY_KEY_F18;
    case KEY_F19: return GHOSTTY_KEY_F19;
    case KEY_F20: return GHOSTTY_KEY_F20;
    case KEY_F21: return GHOSTTY_KEY_F21;
    case KEY_F22: return GHOSTTY_KEY_F22;
    case KEY_F23: return GHOSTTY_KEY_F23;
    case KEY_F24: return GHOSTTY_KEY_F24;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
}

GhosttyKey ghosttyKeyFromXkbKeysym(quint32 keysym)
{
    if (keysym >= XKB_KEY_a && keysym <= XKB_KEY_z) {
        return static_cast<GhosttyKey>(GHOSTTY_KEY_A + keysym - XKB_KEY_a);
    }
    if (keysym >= XKB_KEY_0 && keysym <= XKB_KEY_9) {
        return static_cast<GhosttyKey>(GHOSTTY_KEY_DIGIT_0 + keysym
                                       - XKB_KEY_0);
    }
    if (keysym >= XKB_KEY_F1 && keysym <= XKB_KEY_F25) {
        return static_cast<GhosttyKey>(GHOSTTY_KEY_F1 + keysym - XKB_KEY_F1);
    }
    if (keysym >= XKB_KEY_KP_0 && keysym <= XKB_KEY_KP_9) {
        return static_cast<GhosttyKey>(GHOSTTY_KEY_NUMPAD_0 + keysym
                                       - XKB_KEY_KP_0);
    }

    switch (keysym) {
    case XKB_KEY_semicolon: return GHOSTTY_KEY_SEMICOLON;
    case XKB_KEY_space: return GHOSTTY_KEY_SPACE;
    case XKB_KEY_apostrophe: return GHOSTTY_KEY_QUOTE;
    case XKB_KEY_comma: return GHOSTTY_KEY_COMMA;
    case XKB_KEY_grave: return GHOSTTY_KEY_BACKQUOTE;
    case XKB_KEY_period: return GHOSTTY_KEY_PERIOD;
    case XKB_KEY_slash: return GHOSTTY_KEY_SLASH;
    case XKB_KEY_minus: return GHOSTTY_KEY_MINUS;
    case XKB_KEY_equal: return GHOSTTY_KEY_EQUAL;
    case XKB_KEY_bracketleft: return GHOSTTY_KEY_BRACKET_LEFT;
    case XKB_KEY_bracketright: return GHOSTTY_KEY_BRACKET_RIGHT;
    case XKB_KEY_backslash: return GHOSTTY_KEY_BACKSLASH;
    case XKB_KEY_Up: return GHOSTTY_KEY_ARROW_UP;
    case XKB_KEY_Down: return GHOSTTY_KEY_ARROW_DOWN;
    case XKB_KEY_Right: return GHOSTTY_KEY_ARROW_RIGHT;
    case XKB_KEY_Left: return GHOSTTY_KEY_ARROW_LEFT;
    case XKB_KEY_Home: return GHOSTTY_KEY_HOME;
    case XKB_KEY_End: return GHOSTTY_KEY_END;
    case XKB_KEY_Insert: return GHOSTTY_KEY_INSERT;
    case XKB_KEY_Delete: return GHOSTTY_KEY_DELETE;
    case XKB_KEY_Caps_Lock: return GHOSTTY_KEY_CAPS_LOCK;
    case XKB_KEY_Scroll_Lock: return GHOSTTY_KEY_SCROLL_LOCK;
    case XKB_KEY_Num_Lock: return GHOSTTY_KEY_NUM_LOCK;
    case XKB_KEY_Page_Up: return GHOSTTY_KEY_PAGE_UP;
    case XKB_KEY_Page_Down: return GHOSTTY_KEY_PAGE_DOWN;
    case XKB_KEY_Escape: return GHOSTTY_KEY_ESCAPE;
    case XKB_KEY_Return: return GHOSTTY_KEY_ENTER;
    case XKB_KEY_Tab: return GHOSTTY_KEY_TAB;
    case XKB_KEY_BackSpace: return GHOSTTY_KEY_BACKSPACE;
    case XKB_KEY_Print: return GHOSTTY_KEY_PRINT_SCREEN;
    case XKB_KEY_Pause: return GHOSTTY_KEY_PAUSE;
    case XKB_KEY_KP_Decimal: return GHOSTTY_KEY_NUMPAD_DECIMAL;
    case XKB_KEY_KP_Divide: return GHOSTTY_KEY_NUMPAD_DIVIDE;
    case XKB_KEY_KP_Multiply: return GHOSTTY_KEY_NUMPAD_MULTIPLY;
    case XKB_KEY_KP_Subtract: return GHOSTTY_KEY_NUMPAD_SUBTRACT;
    case XKB_KEY_KP_Add: return GHOSTTY_KEY_NUMPAD_ADD;
    case XKB_KEY_KP_Enter: return GHOSTTY_KEY_NUMPAD_ENTER;
    case XKB_KEY_KP_Equal: return GHOSTTY_KEY_NUMPAD_EQUAL;
    case XKB_KEY_KP_Separator: return GHOSTTY_KEY_NUMPAD_SEPARATOR;
    case XKB_KEY_KP_Left: return GHOSTTY_KEY_NUMPAD_LEFT;
    case XKB_KEY_KP_Right: return GHOSTTY_KEY_NUMPAD_RIGHT;
    case XKB_KEY_KP_Up: return GHOSTTY_KEY_NUMPAD_UP;
    case XKB_KEY_KP_Down: return GHOSTTY_KEY_NUMPAD_DOWN;
    case XKB_KEY_KP_Page_Up: return GHOSTTY_KEY_NUMPAD_PAGE_UP;
    case XKB_KEY_KP_Page_Down: return GHOSTTY_KEY_NUMPAD_PAGE_DOWN;
    case XKB_KEY_KP_Home: return GHOSTTY_KEY_NUMPAD_HOME;
    case XKB_KEY_KP_End: return GHOSTTY_KEY_NUMPAD_END;
    case XKB_KEY_KP_Insert: return GHOSTTY_KEY_NUMPAD_INSERT;
    case XKB_KEY_KP_Delete: return GHOSTTY_KEY_NUMPAD_DELETE;
    case XKB_KEY_KP_Begin: return GHOSTTY_KEY_NUMPAD_BEGIN;
    case XKB_KEY_XF86Copy: return GHOSTTY_KEY_COPY;
    case XKB_KEY_XF86Cut: return GHOSTTY_KEY_CUT;
    case XKB_KEY_XF86Paste: return GHOSTTY_KEY_PASTE;
    case XKB_KEY_Shift_L: return GHOSTTY_KEY_SHIFT_LEFT;
    case XKB_KEY_Control_L: return GHOSTTY_KEY_CONTROL_LEFT;
    case XKB_KEY_Alt_L: return GHOSTTY_KEY_ALT_LEFT;
    case XKB_KEY_Super_L: return GHOSTTY_KEY_META_LEFT;
    case XKB_KEY_Shift_R: return GHOSTTY_KEY_SHIFT_RIGHT;
    case XKB_KEY_Control_R: return GHOSTTY_KEY_CONTROL_RIGHT;
    case XKB_KEY_Alt_R: return GHOSTTY_KEY_ALT_RIGHT;
    case XKB_KEY_Super_R: return GHOSTTY_KEY_META_RIGHT;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
}

bool ghosttyKeyShouldBeRemappable(GhosttyKey key) noexcept
{
    switch (key) {
    case GHOSTTY_KEY_BACKQUOTE:
    case GHOSTTY_KEY_BACKSLASH:
    case GHOSTTY_KEY_BRACKET_LEFT:
    case GHOSTTY_KEY_BRACKET_RIGHT:
    case GHOSTTY_KEY_COMMA:
    case GHOSTTY_KEY_DIGIT_0:
    case GHOSTTY_KEY_DIGIT_1:
    case GHOSTTY_KEY_DIGIT_2:
    case GHOSTTY_KEY_DIGIT_3:
    case GHOSTTY_KEY_DIGIT_4:
    case GHOSTTY_KEY_DIGIT_5:
    case GHOSTTY_KEY_DIGIT_6:
    case GHOSTTY_KEY_DIGIT_7:
    case GHOSTTY_KEY_DIGIT_8:
    case GHOSTTY_KEY_DIGIT_9:
    case GHOSTTY_KEY_EQUAL:
    case GHOSTTY_KEY_INTL_BACKSLASH:
    case GHOSTTY_KEY_INTL_RO:
    case GHOSTTY_KEY_INTL_YEN:
    case GHOSTTY_KEY_A:
    case GHOSTTY_KEY_B:
    case GHOSTTY_KEY_C:
    case GHOSTTY_KEY_D:
    case GHOSTTY_KEY_E:
    case GHOSTTY_KEY_F:
    case GHOSTTY_KEY_G:
    case GHOSTTY_KEY_H:
    case GHOSTTY_KEY_I:
    case GHOSTTY_KEY_J:
    case GHOSTTY_KEY_K:
    case GHOSTTY_KEY_L:
    case GHOSTTY_KEY_M:
    case GHOSTTY_KEY_N:
    case GHOSTTY_KEY_O:
    case GHOSTTY_KEY_P:
    case GHOSTTY_KEY_Q:
    case GHOSTTY_KEY_R:
    case GHOSTTY_KEY_S:
    case GHOSTTY_KEY_T:
    case GHOSTTY_KEY_U:
    case GHOSTTY_KEY_V:
    case GHOSTTY_KEY_W:
    case GHOSTTY_KEY_X:
    case GHOSTTY_KEY_Y:
    case GHOSTTY_KEY_Z:
    case GHOSTTY_KEY_MINUS:
    case GHOSTTY_KEY_PERIOD:
    case GHOSTTY_KEY_QUOTE:
    case GHOSTTY_KEY_SEMICOLON:
    case GHOSTTY_KEY_SLASH: return false;
    default: return true;
    }
}

GhosttyKey ghosttyEffectiveKey(quint32 nativeScanCode, quint32 resolvedKeysym,
                               int qtKey, Qt::KeyboardModifiers modifiers)
{
    const GhosttyKey physical = ghosttyKeyFromNativeScanCode(nativeScanCode);
    const GhosttyKey resolved = ghosttyKeyFromXkbKeysym(resolvedKeysym);
    if (resolved != GHOSTTY_KEY_UNIDENTIFIED
        && (ghosttyKeyShouldBeRemappable(physical)
            || ghosttyKeyShouldBeRemappable(resolved))) {
        return resolved;
    }
    if (physical != GHOSTTY_KEY_UNIDENTIFIED) return physical;
    if (nativeScanCode == 0 && resolvedKeysym == 0) {
        return ghosttyKeyFromQt(qtKey, modifiers);
    }
    return GHOSTTY_KEY_UNIDENTIFIED;
}

bool ghosttyKeyIsModifier(GhosttyKey key) noexcept
{
    switch (key) {
    case GHOSTTY_KEY_SHIFT_LEFT:
    case GHOSTTY_KEY_SHIFT_RIGHT:
    case GHOSTTY_KEY_CONTROL_LEFT:
    case GHOSTTY_KEY_CONTROL_RIGHT:
    case GHOSTTY_KEY_ALT_LEFT:
    case GHOSTTY_KEY_ALT_RIGHT:
    case GHOSTTY_KEY_META_LEFT:
    case GHOSTTY_KEY_META_RIGHT: return true;
    default: return false;
    }
}
