#include "ghostty_keybind_set.h"

#include <QChar>
#include <QHash>
#include <QLatin1StringView>

#include <algorithm>
#include <array>
#include <linux/input-event-codes.h>

namespace {

using Disposition = GhosttyKeybindEntryDisposition;
using Reason = GhosttyKeybindUnsupportedReason;

constexpr Qt::KeyboardModifiers RelevantModifiers =
    Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier
    | Qt::MetaModifier;

struct ParsedFlags {
    bool consumed = true;
    bool performable = false;
    bool nonLocal = false;
};

struct ParsedTrigger {
    bool physical = false;
    bool catchAll = false;
    int qtKey = Qt::Key_unknown;
    quint32 nativeScanCode = 0;
    bool keypad = false;
    QString physicalName;
    QString unicode;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
};

struct PhysicalKey {
    int qtKey = Qt::Key_unknown;
    quint32 nativeScanCode = 0;
    bool keypad = false;
};

GhosttyKeybindParseRecord record(QStringView input,
                                 Disposition disposition,
                                 Reason reason = Reason::None,
                                 QString detail = {})
{
    return {
        .input = input.toString(),
        .disposition = disposition,
        .reason = reason,
        .detail = std::move(detail),
    };
}

Qt::KeyboardModifiers normalizedModifiers(Qt::KeyboardModifiers modifiers)
{
    return modifiers & RelevantModifiers;
}

// Binding.Parser.init must distinguish the action separator from an equals
// key. An '=' followed by '+' or '=' is part of the trigger; the next equals
// is the delimiter. This covers `=+ctrl=...`, `ctrl+==...`, and keeps the
// ordinary `ctrl++=...` form unambiguous.
std::optional<qsizetype> actionDelimiter(QStringView input,
                                         qsizetype start = 0)
{
    for (qsizetype i = std::max<qsizetype>(0, start); i < input.size(); ++i) {
        if (input.at(i) != u'=') {
            continue;
        }
        if (i + 1 < input.size()
            && (input.at(i + 1) == u'+' || input.at(i + 1) == u'=')) {
            continue;
        }
        return i;
    }
    return std::nullopt;
}

bool isKeyTableInput(QStringView input)
{
    // Config.Keybinds detects tables before Binding.Parser sees the trigger.
    // Limit the slash search to the left side so `ctrl+a=text:/tmp` remains a
    // normal root binding. A leading slash and a prefix containing '+' or '>'
    // are trigger syntax, not table names.
    const qsizetype firstEquals = input.indexOf(u'=');
    const QStringView left = firstEquals < 0 ? input : input.first(firstEquals);
    const qsizetype slash = left.indexOf(u'/');
    if (slash <= 0) {
        return false;
    }
    const QStringView possibleTable = left.first(slash);
    return !possibleTable.contains(u'+') && !possibleTable.contains(u'>');
}

QString w3cToSnake(QStringView name)
{
    QString lowered = name.toString().toLower();
    // Ghostty first tries the fully lower-cased name, which handles F1, F2,
    // etc. without producing the otherwise incorrect `f_1` spelling.
    if (lowered.size() >= 2 && lowered.front() == u'f') {
        bool digits = true;
        for (qsizetype i = 1; i < lowered.size(); ++i) {
            digits = digits && lowered.at(i).isDigit();
        }
        if (digits) {
            return lowered;
        }
    }

    QString result;
    result.reserve(name.size() * 2);
    for (qsizetype i = 0; i < name.size(); ++i) {
        const QChar character = name.at(i);
        if (i > 0 && (character.isUpper() || character.isDigit())) {
            result.append(u'_');
        }
        result.append(character.toLower());
    }
    return result;
}

quint32 xkbKeycode(unsigned int evdevCode)
{
    // wl_keyboard sends Linux evdev codes. Qt Wayland adds the XKB offset
    // before placing the value in QKeyEvent::nativeScanCode(); X11 already
    // uses the same XKB keycode space.
    return static_cast<quint32>(evdevCode + 8U);
}

std::optional<PhysicalKey> functionKey(QStringView name)
{
    if (name.size() < 2 || name.front() != u'f') {
        return std::nullopt;
    }
    bool valid = false;
    const int number = name.sliced(1).toInt(&valid);
    if (!valid || number < 1 || number > 35) {
        return std::nullopt;
    }

    static constexpr std::array<unsigned int, 24> evdevCodes = {
        KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
        KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
        KEY_F13, KEY_F14, KEY_F15, KEY_F16, KEY_F17, KEY_F18,
        KEY_F19, KEY_F20, KEY_F21, KEY_F22, KEY_F23, KEY_F24,
    };
    return PhysicalKey{
        .qtKey = Qt::Key_F1 + number - 1,
        // Linux has standard evdev codes through F24. Qt can represent F25+
        // logically, so those retain the compatibility fallback.
        .nativeScanCode = number <= static_cast<int>(evdevCodes.size())
            ? xkbKeycode(evdevCodes.at(static_cast<std::size_t>(number - 1)))
            : 0,
    };
}

std::optional<PhysicalKey> physicalKey(QStringView rawName)
{
    const QString name = rawName.contains(u'_') || rawName == rawName.toString().toLower()
        ? rawName.toString()
        : w3cToSnake(rawName);

    if (const auto key = functionKey(name)) {
        return key;
    }

    if (name.startsWith(QLatin1StringView("key_")) && name.size() == 5) {
        const QChar letter = name.back();
        if (letter >= u'a' && letter <= u'z') {
            static constexpr std::array<unsigned int, 26> evdevCodes = {
                KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G,
                KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N,
                KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U,
                KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
            };
            const std::size_t index = static_cast<std::size_t>(
                letter.unicode() - u'a');
            return PhysicalKey{
                .qtKey = Qt::Key_A + static_cast<int>(index),
                .nativeScanCode = xkbKeycode(evdevCodes.at(index)),
            };
        }
    }
    if (name.startsWith(QLatin1StringView("digit_")) && name.size() == 7) {
        const QChar digit = name.back();
        if (digit >= u'0' && digit <= u'9') {
            static constexpr std::array<unsigned int, 10> evdevCodes = {
                KEY_0, KEY_1, KEY_2, KEY_3, KEY_4,
                KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
            };
            const std::size_t index = static_cast<std::size_t>(
                digit.unicode() - u'0');
            return PhysicalKey{
                .qtKey = Qt::Key_0 + static_cast<int>(index),
                .nativeScanCode = xkbKeycode(evdevCodes.at(index)),
            };
        }
    }

    const auto key = [](int qtKey, unsigned int evdevCode,
                        bool keypad = false) {
        return PhysicalKey{qtKey, xkbKeycode(evdevCode), keypad};
    };
    static const QHash<QString, PhysicalKey> keys = {
        {QStringLiteral("backquote"), key(Qt::Key_QuoteLeft, KEY_GRAVE)},
        {QStringLiteral("backslash"), key(Qt::Key_Backslash, KEY_BACKSLASH)},
        {QStringLiteral("bracket_left"), key(Qt::Key_BracketLeft, KEY_LEFTBRACE)},
        {QStringLiteral("bracket_right"), key(Qt::Key_BracketRight, KEY_RIGHTBRACE)},
        {QStringLiteral("comma"), key(Qt::Key_Comma, KEY_COMMA)},
        {QStringLiteral("equal"), key(Qt::Key_Equal, KEY_EQUAL)},
        {QStringLiteral("minus"), key(Qt::Key_Minus, KEY_MINUS)},
        {QStringLiteral("period"), key(Qt::Key_Period, KEY_DOT)},
        {QStringLiteral("quote"), key(Qt::Key_Apostrophe, KEY_APOSTROPHE)},
        {QStringLiteral("semicolon"), key(Qt::Key_Semicolon, KEY_SEMICOLON)},
        {QStringLiteral("slash"), key(Qt::Key_Slash, KEY_SLASH)},
        {QStringLiteral("alt_left"), key(Qt::Key_Alt, KEY_LEFTALT)},
        {QStringLiteral("alt_right"), key(Qt::Key_AltGr, KEY_RIGHTALT)},
        {QStringLiteral("backspace"), key(Qt::Key_Backspace, KEY_BACKSPACE)},
        {QStringLiteral("caps_lock"), key(Qt::Key_CapsLock, KEY_CAPSLOCK)},
        {QStringLiteral("context_menu"), key(Qt::Key_Menu, KEY_MENU)},
        {QStringLiteral("control_left"), key(Qt::Key_Control, KEY_LEFTCTRL)},
        {QStringLiteral("control_right"), key(Qt::Key_Control, KEY_RIGHTCTRL)},
        {QStringLiteral("enter"), key(Qt::Key_Return, KEY_ENTER)},
        {QStringLiteral("meta_left"), key(Qt::Key_Meta, KEY_LEFTMETA)},
        {QStringLiteral("meta_right"), key(Qt::Key_Meta, KEY_RIGHTMETA)},
        {QStringLiteral("shift_left"), key(Qt::Key_Shift, KEY_LEFTSHIFT)},
        {QStringLiteral("shift_right"), key(Qt::Key_Shift, KEY_RIGHTSHIFT)},
        {QStringLiteral("space"), key(Qt::Key_Space, KEY_SPACE)},
        {QStringLiteral("tab"), key(Qt::Key_Tab, KEY_TAB)},
        {QStringLiteral("delete"), key(Qt::Key_Delete, KEY_DELETE)},
        {QStringLiteral("end"), key(Qt::Key_End, KEY_END)},
        {QStringLiteral("help"), key(Qt::Key_Help, KEY_HELP)},
        {QStringLiteral("home"), key(Qt::Key_Home, KEY_HOME)},
        {QStringLiteral("insert"), key(Qt::Key_Insert, KEY_INSERT)},
        {QStringLiteral("page_down"), key(Qt::Key_PageDown, KEY_PAGEDOWN)},
        {QStringLiteral("page_up"), key(Qt::Key_PageUp, KEY_PAGEUP)},
        {QStringLiteral("arrow_down"), key(Qt::Key_Down, KEY_DOWN)},
        {QStringLiteral("arrow_left"), key(Qt::Key_Left, KEY_LEFT)},
        {QStringLiteral("arrow_right"), key(Qt::Key_Right, KEY_RIGHT)},
        {QStringLiteral("arrow_up"), key(Qt::Key_Up, KEY_UP)},
        // Ghostty 1.1 compatibility spellings retained by the pinned parser.
        {QStringLiteral("down"), key(Qt::Key_Down, KEY_DOWN)},
        {QStringLiteral("left"), key(Qt::Key_Left, KEY_LEFT)},
        {QStringLiteral("right"), key(Qt::Key_Right, KEY_RIGHT)},
        {QStringLiteral("up"), key(Qt::Key_Up, KEY_UP)},
        {QStringLiteral("num_lock"), key(Qt::Key_NumLock, KEY_NUMLOCK, true)},
        {QStringLiteral("numpad_add"), key(Qt::Key_Plus, KEY_KPPLUS, true)},
        {QStringLiteral("numpad_decimal"), key(Qt::Key_Period, KEY_KPDOT, true)},
        {QStringLiteral("numpad_divide"), key(Qt::Key_Slash, KEY_KPSLASH, true)},
        {QStringLiteral("numpad_enter"), key(Qt::Key_Enter, KEY_KPENTER, true)},
        {QStringLiteral("numpad_equal"), key(Qt::Key_Equal, KEY_KPEQUAL, true)},
        {QStringLiteral("numpad_multiply"), key(Qt::Key_Asterisk, KEY_KPASTERISK, true)},
        {QStringLiteral("numpad_subtract"), key(Qt::Key_Minus, KEY_KPMINUS, true)},
        {QStringLiteral("escape"), key(Qt::Key_Escape, KEY_ESC)},
        {QStringLiteral("print_screen"), key(Qt::Key_Print, KEY_SYSRQ)},
        {QStringLiteral("scroll_lock"), key(Qt::Key_ScrollLock, KEY_SCROLLLOCK)},
        {QStringLiteral("pause"), key(Qt::Key_Pause, KEY_PAUSE)},
        {QStringLiteral("copy"), key(Qt::Key_Copy, KEY_COPY)},
        {QStringLiteral("cut"), key(Qt::Key_Cut, KEY_CUT)},
        {QStringLiteral("paste"), key(Qt::Key_Paste, KEY_PASTE)},
    };

    if (name.startsWith(QLatin1StringView("numpad_")) && name.size() == 8) {
        const QChar digit = name.back();
        if (digit >= u'0' && digit <= u'9') {
            static constexpr std::array<unsigned int, 10> evdevCodes = {
                KEY_KP0, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP4,
                KEY_KP5, KEY_KP6, KEY_KP7, KEY_KP8, KEY_KP9,
            };
            const std::size_t index = static_cast<std::size_t>(
                digit.unicode() - u'0');
            return PhysicalKey{
                .qtKey = Qt::Key_0 + static_cast<int>(index),
                .nativeScanCode = xkbKeycode(evdevCodes.at(index)),
                .keypad = true,
            };
        }
    }

    const auto found = keys.constFind(name);
    return found == keys.cend()
        ? std::nullopt
        : std::optional<PhysicalKey>(*found);
}

bool parseFlags(QStringView input,
                qsizetype *triggerStart,
                ParsedFlags *flags,
                QString *error)
{
    qsizetype start = 0;
    bool seenConsumed = false;
    bool seenPerformable = false;
    bool seenAll = false;
    bool seenGlobal = false;

    while (start < input.size()) {
        const qsizetype colon = input.indexOf(u':', start);
        if (colon < 0) {
            break;
        }
        const QStringView prefix = input.sliced(start, colon - start);
        bool recognized = true;
        if (prefix == QLatin1StringView("unconsumed")) {
            if (seenConsumed) {
                *error = QStringLiteral("duplicate unconsumed flag");
                return false;
            }
            seenConsumed = true;
            flags->consumed = false;
        } else if (prefix == QLatin1StringView("performable")) {
            if (seenPerformable) {
                *error = QStringLiteral("duplicate performable flag");
                return false;
            }
            seenPerformable = true;
            flags->performable = true;
        } else if (prefix == QLatin1StringView("all")) {
            if (seenAll) {
                *error = QStringLiteral("duplicate all flag");
                return false;
            }
            seenAll = true;
            flags->nonLocal = true;
        } else if (prefix == QLatin1StringView("global")) {
            if (seenGlobal) {
                *error = QStringLiteral("duplicate global flag");
                return false;
            }
            seenGlobal = true;
            flags->nonLocal = true;
        } else {
            recognized = false;
        }

        if (!recognized) {
            break;
        }
        start = colon + 1;
    }

    *triggerStart = start;
    return true;
}

std::optional<Qt::KeyboardModifier> modifierFor(QStringView part)
{
    if (part == QLatin1StringView("super")
        || part == QLatin1StringView("cmd")
        || part == QLatin1StringView("command")) {
        return Qt::MetaModifier;
    }
    if (part == QLatin1StringView("ctrl")
        || part == QLatin1StringView("control")) {
        return Qt::ControlModifier;
    }
    if (part == QLatin1StringView("alt")
        || part == QLatin1StringView("opt")
        || part == QLatin1StringView("option")) {
        return Qt::AltModifier;
    }
    if (part == QLatin1StringView("shift")) {
        return Qt::ShiftModifier;
    }
    return std::nullopt;
}

bool isSingleCodepoint(QStringView value)
{
    return value.toString().toUcs4().size() == 1;
}

bool parseTrigger(QStringView input, ParsedTrigger *trigger, QString *error)
{
    if (input.isEmpty()) {
        *error = QStringLiteral("empty trigger");
        return false;
    }

    bool hasKey = false;
    qsizetype start = 0;
    while (start < input.size()) {
        const qsizetype plus = input.indexOf(u'+', start);
        const qsizetype end = plus < 0 ? input.size() : plus;
        const QStringView part = input.sliced(start, end - start);
        start = plus < 0 ? input.size() : plus + 1;

        if (const auto modifier = modifierFor(part)) {
            if (trigger->modifiers.testFlag(*modifier)) {
                *error = QStringLiteral("duplicate modifier");
                return false;
            }
            trigger->modifiers |= *modifier;
            continue;
        }

        if (hasKey) {
            *error = QStringLiteral("trigger contains more than one key");
            return false;
        }
        hasKey = true;

        // An empty part is Ghostty's spelling for a literal plus key. A
        // trailing plus is not iterated a second time, matching Trigger.parse.
        if (part.isEmpty()) {
            trigger->unicode = QStringLiteral("+");
            continue;
        }
        if (part == QLatin1StringView("catch_all")) {
            trigger->catchAll = true;
            continue;
        }
        if (const auto key = physicalKey(part)) {
            trigger->physical = true;
            trigger->qtKey = key->qtKey;
            trigger->nativeScanCode = key->nativeScanCode;
            trigger->keypad = key->keypad;
            trigger->physicalName =
                part.contains(u'_') || part == part.toString().toLower()
                ? part.toString().toLower()
                : w3cToSnake(part);
            continue;
        }
        if (isSingleCodepoint(part)) {
            trigger->unicode = part.toString();
            continue;
        }

        *error = QStringLiteral("unknown key name");
        return false;
    }

    if (!hasKey) {
        *error = QStringLiteral("trigger has no key");
        return false;
    }
    return true;
}

bool sameUnicode(QStringView left, QStringView right)
{
    return left.toString().toCaseFolded() == right.toString().toCaseFolded();
}

QString unicodeFromQtEvent(int qtKey, QStringView text)
{
    const QList<uint> codepoints = text.toString().toUcs4();
    if (codepoints.size() == 1
        && codepoints.constFirst() >= 0x20U
        && codepoints.constFirst() != 0x7fU) {
        return text.toString();
    }

    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) {
        return QString(QChar(u'a' + qtKey - Qt::Key_A));
    }
    if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9) {
        return QString(QChar(u'0' + qtKey - Qt::Key_0));
    }

    static const QHash<int, QChar> punctuation = {
        {Qt::Key_Space, u' '},       {Qt::Key_Equal, u'='},
        {Qt::Key_Plus, u'+'},        {Qt::Key_Minus, u'-'},
        {Qt::Key_Comma, u','},       {Qt::Key_Period, u'.'},
        {Qt::Key_Slash, u'/'},       {Qt::Key_Backslash, u'\\'},
        {Qt::Key_Semicolon, u';'},   {Qt::Key_Apostrophe, u'\''},
        {Qt::Key_BracketLeft, u'['}, {Qt::Key_BracketRight, u']'},
        {Qt::Key_QuoteLeft, u'`'},
    };
    const auto found = punctuation.constFind(qtKey);
    return found == punctuation.cend() ? QString{} : QString(*found);
}

bool couldBeKeypadKey(int qtKey)
{
    return (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9)
        || qtKey == Qt::Key_Plus || qtKey == Qt::Key_Minus
        || qtKey == Qt::Key_Period || qtKey == Qt::Key_Slash
        || qtKey == Qt::Key_Equal || qtKey == Qt::Key_Asterisk;
}

bool physicalMatches(int configured,
                     quint32 configuredScanCode,
                     bool configuredKeypad,
                     int eventKey,
                     quint32 eventScanCode,
                     Qt::KeyboardModifiers eventModifiers)
{
    // A nonzero native code is authoritative. Never fall back to the layout-
    // derived Qt key when both sides have native identities: that would make
    // KeyA fire from whichever physical key the active layout maps to A.
    if (configuredScanCode != 0 && eventScanCode != 0) {
        return configuredScanCode == eventScanCode;
    }

    // Synthetic QKeyEvents often omit native information. Preserve logical
    // matching, while using KeypadModifier to disambiguate keypad locations
    // wherever Qt gives us enough information.
    const bool eventKeypad = eventModifiers.testFlag(Qt::KeypadModifier);
    if (configuredKeypad != eventKeypad
        && (configuredKeypad || couldBeKeypadKey(eventKey))) {
        return false;
    }

    // Qt reports Shift+Tab as Backtab even though both represent the same
    // physical Tab key in Ghostty's model.
    if (configured == Qt::Key_Tab && eventKey == Qt::Key_Backtab) {
        return true;
    }
    return configured == eventKey;
}

Qt::KeyboardModifiers withoutTriggeredModifier(
    int configuredKey,
    Qt::KeyboardModifiers modifiers)
{
    switch (configuredKey) {
    case Qt::Key_Control:
        return modifiers & ~Qt::ControlModifier;
    case Qt::Key_Shift:
        return modifiers & ~Qt::ShiftModifier;
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
        return modifiers & ~Qt::AltModifier;
    case Qt::Key_Meta:
        return modifiers & ~Qt::MetaModifier;
    default:
        return modifiers;
    }
}

quint8 configModifiers(Qt::KeyboardModifiers modifiers)
{
    quint8 result = 0;
    if (modifiers.testFlag(Qt::ShiftModifier)) result |= GhosttyKeybindShift;
    if (modifiers.testFlag(Qt::ControlModifier)) result |= GhosttyKeybindCtrl;
    if (modifiers.testFlag(Qt::AltModifier)) result |= GhosttyKeybindAlt;
    if (modifiers.testFlag(Qt::MetaModifier)) result |= GhosttyKeybindSuper;
    return result;
}

Qt::KeyboardModifiers qtModifiers(quint8 modifiers)
{
    Qt::KeyboardModifiers result = Qt::NoModifier;
    if ((modifiers & GhosttyKeybindShift) != 0U) result |= Qt::ShiftModifier;
    if ((modifiers & GhosttyKeybindCtrl) != 0U) result |= Qt::ControlModifier;
    if ((modifiers & GhosttyKeybindAlt) != 0U) result |= Qt::AltModifier;
    if ((modifiers & GhosttyKeybindSuper) != 0U) result |= Qt::MetaModifier;
    return result;
}

bool isUnicodeScalar(quint32 codepoint)
{
    return codepoint <= 0x10ffffU
        && !(codepoint >= 0xd800U && codepoint <= 0xdfffU);
}

bool isModifierKey(int key)
{
    return key == Qt::Key_Control || key == Qt::Key_Shift
        || key == Qt::Key_Alt || key == Qt::Key_AltGr
        || key == Qt::Key_Meta;
}

bool isIgnoreAction(QStringView action)
{
    const qsizetype colon = action.indexOf(u':');
    return (colon < 0 ? action : action.first(colon))
        == QLatin1StringView("ignore");
}

} // namespace

int GhosttyKeybindLoadReport::count(
    GhosttyKeybindEntryDisposition disposition) const
{
    return int(std::count_if(records.cbegin(), records.cend(),
                             [disposition](const auto &entry) {
                                 return entry.disposition == disposition;
                             }));
}

QStringList GhosttyKeybindSet::serializedActions() const
{
    QStringList result;
    if (nodes_.isEmpty()) {
        return result;
    }

    const auto visit = [this, &result](auto &&self, quint32 node) -> void {
        if (node >= static_cast<quint32>(nodes_.size())) {
            return;
        }
        for (const Entry &entry : nodes_.at(static_cast<qsizetype>(node)).entries) {
            if (entry.kind == EntryKind::Leader) {
                self(self, entry.child);
            } else {
                result.append(entry.actions);
            }
        }
    };
    visit(visit, 0);
    return result;
}

void GhosttyKeybindSet::clear() noexcept
{
    nodes_.clear();
    nodes_.append(Node{});
    bindingCount_ = 0;
    resetSequence();
}

GhosttyKeybindLoadReport GhosttyKeybindSet::load(const QStringList &values)
{
    GhosttyKeybindLoadReport report;
    report.records.reserve(values.size());
    GhosttyKeybindConfig config;
    qsizetype chainTarget = -1;

    for (const QString &value : values) {
        const QStringView input(value);
        if (input == QLatin1StringView("clear")) {
            report.records.append(record(input, Disposition::Unsupported,
                                         Reason::ClearDirective));
            chainTarget = -1;
            continue;
        }
        if (input.isEmpty()) {
            report.records.append(record(input, Disposition::Invalid,
                                         Reason::None,
                                         QStringLiteral("empty binding")));
            chainTarget = -1;
            continue;
        }
        if (isKeyTableInput(input)) {
            report.records.append(record(input, Disposition::Unsupported,
                                         Reason::KeyTable));
            chainTarget = -1;
            continue;
        }

        const auto delimiter = actionDelimiter(input);
        if (!delimiter.has_value()) {
            report.records.append(record(input, Disposition::Invalid,
                                         Reason::None,
                                         QStringLiteral("missing action delimiter")));
            chainTarget = -1;
            continue;
        }

        const QStringView left = input.first(*delimiter);
        const QString action = input.sliced(*delimiter + 1).toString();
        if (action.isEmpty()) {
            report.records.append(record(input, Disposition::Invalid,
                                         Reason::None,
                                         QStringLiteral("empty action")));
            chainTarget = -1;
            continue;
        }
        if (left == QLatin1StringView("chain")) {
            if (chainTarget < 0 || chainTarget >= config.root.size()) {
                report.records.append(record(input, Disposition::Unsupported,
                                             Reason::OrphanChain,
                                             QStringLiteral("chain has no adjacent supported binding")));
            } else {
                config.root[chainTarget].actions.append(action);
                report.records.append(record(input, Disposition::Chained));
            }
            continue;
        }

        chainTarget = -1;
        ParsedFlags flags;
        qsizetype triggerStart = 0;
        QString error;
        if (!parseFlags(left, &triggerStart, &flags, &error)) {
            report.records.append(record(input, Disposition::Invalid,
                                         Reason::None, std::move(error)));
            continue;
        }
        if (flags.nonLocal) {
            report.records.append(record(input, Disposition::Unsupported,
                                         Reason::NonLocal));
            continue;
        }

        const QString triggerText = left.sliced(triggerStart).toString();
        const QStringList parts = triggerText.split(u'>', Qt::KeepEmptyParts);
        GhosttyKeybindDefinition definition;
        definition.actions.append(action);
        definition.flags.consumed = flags.consumed;
        definition.flags.performable = flags.performable;
        bool valid = !parts.isEmpty();
        for (const QString &part : parts) {
            ParsedTrigger parsed;
            if (!parseTrigger(part, &parsed, &error)) {
                valid = false;
                break;
            }
            GhosttyKeybindTrigger trigger;
            trigger.modifiers = configModifiers(parsed.modifiers);
            if (parsed.catchAll) {
                trigger.kind = GhosttyKeybindKeyKind::CatchAll;
            } else if (parsed.physical) {
                trigger.kind = GhosttyKeybindKeyKind::Physical;
                trigger.physicalName = parsed.physicalName;
            } else {
                const QList<uint> codepoints = parsed.unicode.toUcs4();
                if (codepoints.size() != 1) {
                    error = QStringLiteral("Unicode trigger is not one scalar");
                    valid = false;
                    break;
                }
                trigger.kind = GhosttyKeybindKeyKind::Unicode;
                trigger.unicodeCodepoint = codepoints.constFirst();
            }
            definition.sequence.append(std::move(trigger));
        }
        if (!valid || definition.sequence.isEmpty()) {
            report.records.append(record(input, Disposition::Invalid,
                                         Reason::None,
                                         error.isEmpty()
                                             ? QStringLiteral("empty sequence")
                                             : std::move(error)));
            continue;
        }

        config.root.append(std::move(definition));
        chainTarget = config.root.size() - 1;
        report.records.append(record(input, Disposition::Installed));
    }

    GhosttyKeybindSet candidate;
    (void) candidate.load(config);
    *this = std::move(candidate);
    return report;
}

GhosttyKeybindLoadReport GhosttyKeybindSet::load(
    const GhosttyKeybindConfig &config)
{
    GhosttyKeybindLoadReport report;
    QVector<Node> newNodes{Node{}};

    if (config.schemaVersion != GhosttyKeybindConfig::CurrentSchemaVersion) {
        report.records.append(record(
            QStringLiteral("structured keybindings"), Disposition::Invalid,
            Reason::None, QStringLiteral("unsupported structured schema version")));
        clear();
        return report;
    }

    for (const GhosttyKeybindTable &table : config.tables) {
        report.records.append(record(
            table.name, Disposition::Unsupported, Reason::KeyTable,
            QStringLiteral("named key table is retained but not active")));
    }

    const auto sameTrigger = [](const Binding &left, const Binding &right) {
        if (left.keyKind != right.keyKind
            || left.modifiers != right.modifiers) {
            return false;
        }
        if (left.keyKind == KeyKind::CatchAll) {
            return true;
        }
        if (left.keyKind == KeyKind::Unicode) {
            return sameUnicode(left.unicode, right.unicode);
        }
        if (left.nativeScanCode != 0 && right.nativeScanCode != 0) {
            return left.nativeScanCode == right.nativeScanCode;
        }
        return left.qtKey == right.qtKey && left.keypad == right.keypad;
    };

    for (qsizetype definitionIndex = 0;
         definitionIndex < config.root.size(); ++definitionIndex) {
        const GhosttyKeybindDefinition &definition =
            config.root.at(definitionIndex);
        const QString label = QStringLiteral("root binding %1")
                                  .arg(definitionIndex);
        if (definition.sequence.isEmpty() || definition.actions.isEmpty()) {
            report.records.append(record(
                label, Disposition::Invalid, Reason::None,
                QStringLiteral("binding sequence and actions must be non-empty")));
            continue;
        }
        if (definition.flags.all || definition.flags.global) {
            report.records.append(record(label, Disposition::Unsupported,
                                         Reason::NonLocal));
            continue;
        }

        QVector<Binding> sequence;
        sequence.reserve(definition.sequence.size());
        QString error;
        bool supported = true;
        for (const GhosttyKeybindTrigger &source : definition.sequence) {
            Binding trigger;
            trigger.modifiers = normalizedModifiers(qtModifiers(source.modifiers));
            switch (source.kind) {
            case GhosttyKeybindKeyKind::Physical: {
                const auto key = physicalKey(source.physicalName);
                if (!key.has_value()) {
                    error = QStringLiteral("unsupported physical key '%1'")
                                .arg(source.physicalName);
                    supported = false;
                    break;
                }
                trigger.keyKind = KeyKind::Physical;
                trigger.qtKey = key->qtKey;
                trigger.nativeScanCode = key->nativeScanCode;
                trigger.keypad = key->keypad;
                break;
            }
            case GhosttyKeybindKeyKind::Unicode: {
                if (!isUnicodeScalar(source.unicodeCodepoint)
                    || source.unicodeCodepoint == 0U) {
                    error = QStringLiteral("invalid Unicode trigger scalar");
                    supported = false;
                    break;
                }
                trigger.keyKind = KeyKind::Unicode;
                const char32_t codepoint =
                    static_cast<char32_t>(source.unicodeCodepoint);
                trigger.unicode = QString::fromUcs4(&codepoint, 1);
                break;
            }
            case GhosttyKeybindKeyKind::CatchAll:
                trigger.keyKind = KeyKind::CatchAll;
                break;
            }
            if (!supported) {
                break;
            }
            sequence.append(std::move(trigger));
        }
        if (!supported) {
            report.records.append(record(label, Disposition::Unsupported,
                                         Reason::None, std::move(error)));
            continue;
        }

        quint32 node = 0;
        for (qsizetype index = 0; index < sequence.size(); ++index) {
            const bool final = index + 1 == sequence.size();
            const Binding &trigger = sequence.at(index);
            QVector<Entry> &entries =
                newNodes[static_cast<qsizetype>(node)].entries;
            qsizetype entryIndex = -1;
            for (qsizetype candidate = 0; candidate < entries.size(); ++candidate) {
                if (sameTrigger(entries.at(candidate).trigger, trigger)) {
                    entryIndex = candidate;
                    break;
                }
            }

            if (final) {
                Entry leaf;
                leaf.trigger = trigger;
                leaf.kind = EntryKind::Leaf;
                leaf.actions = definition.actions;
                leaf.consumed = definition.flags.consumed;
                leaf.performable = definition.flags.performable;
                if (entryIndex < 0) {
                    entries.append(std::move(leaf));
                } else {
                    entries[entryIndex] = std::move(leaf);
                }
                continue;
            }

            if (entryIndex >= 0
                && entries.at(entryIndex).kind == EntryKind::Leader) {
                node = entries.at(entryIndex).child;
                continue;
            }

            const quint32 child = static_cast<quint32>(newNodes.size());
            // Appending a node may reallocate newNodes, so never retain a
            // reference to its parent's entries across this operation.
            newNodes.append(Node{});
            Entry leader;
            leader.trigger = trigger;
            leader.kind = EntryKind::Leader;
            leader.child = child;
            QVector<Entry> &parentEntries =
                newNodes[static_cast<qsizetype>(node)].entries;
            if (entryIndex < 0) {
                parentEntries.append(std::move(leader));
            } else {
                parentEntries[entryIndex] = std::move(leader);
            }
            node = child;
        }
        report.records.append(record(label, Disposition::Installed));
    }

    qsizetype count = 0;
    const auto countLeaves = [&newNodes, &count](auto &&self,
                                                 quint32 node) -> void {
        if (node >= static_cast<quint32>(newNodes.size())) {
            return;
        }
        for (const Entry &entry :
             newNodes.at(static_cast<qsizetype>(node)).entries) {
            if (entry.kind == EntryKind::Leader) self(self, entry.child);
            else ++count;
        }
    };
    countLeaves(countLeaves, 0);

    nodes_ = std::move(newNodes);
    bindingCount_ = count;
    resetSequence();
    return report;
}

GhosttyKeybindSet::Lookup GhosttyKeybindSet::lookup(
    quint32 node,
    const GhosttyKeybindEvent &event) const
{
    if (node >= static_cast<quint32>(nodes_.size())) {
        return {};
    }
    const QVector<Entry> &entries =
        nodes_.at(static_cast<qsizetype>(node)).entries;
    const Qt::KeyboardModifiers normalized =
        normalizedModifiers(event.modifiers);

    // Ghostty prioritizes physical identity at every trie level.
    for (const Entry &entry : entries) {
        const Binding &trigger = entry.trigger;
        const Qt::KeyboardModifiers withoutSelf =
            withoutTriggeredModifier(trigger.qtKey, normalized);
        if (trigger.keyKind == KeyKind::Physical
            && (trigger.modifiers == normalized
                || trigger.modifiers == withoutSelf)
            && physicalMatches(trigger.qtKey, trigger.nativeScanCode,
                               trigger.keypad, event.qtKey,
                               event.nativeScanCode, event.modifiers)) {
            return {&entry, true};
        }
    }

    QVector<QString> unicodeCandidates;
    const auto appendCandidate = [&unicodeCandidates](QString candidate) {
        if (candidate.isEmpty()) return;
        const bool duplicate = std::any_of(
            unicodeCandidates.cbegin(), unicodeCandidates.cend(),
            [&candidate](const QString &existing) {
                return sameUnicode(existing, candidate);
            });
        if (!duplicate) unicodeCandidates.append(std::move(candidate));
    };
    appendCandidate(unicodeFromQtEvent(event.qtKey, event.text));
    if (event.unshiftedCodepoint != 0
        && isUnicodeScalar(event.unshiftedCodepoint)) {
        const char32_t codepoint =
            static_cast<char32_t>(event.unshiftedCodepoint);
        appendCandidate(QString::fromUcs4(&codepoint, 1));
    }
    for (const QString &candidate : unicodeCandidates) {
        for (const Entry &entry : entries) {
            const Binding &trigger = entry.trigger;
            if (trigger.keyKind == KeyKind::Unicode
                && trigger.modifiers == normalized
                && sameUnicode(trigger.unicode, candidate)) {
                return {&entry, false};
            }
        }
    }

    const auto findCatchAll = [&entries](Qt::KeyboardModifiers modifiers)
        -> const Entry * {
        for (const Entry &entry : entries) {
            if (entry.trigger.keyKind == KeyKind::CatchAll
                && entry.trigger.modifiers == modifiers) {
                return &entry;
            }
        }
        return nullptr;
    };
    if (const Entry *entry = findCatchAll(normalized)) {
        return {entry, false};
    }
    if (normalized != Qt::NoModifier) {
        if (const Entry *entry = findCatchAll(Qt::NoModifier)) {
            return {entry, false};
        }
    }
    return {};
}

bool GhosttyKeybindSet::rootCatchAllIgnores() const
{
    if (nodes_.isEmpty()) {
        return false;
    }
    for (const Entry &entry : nodes_.constFirst().entries) {
        if (entry.kind != EntryKind::Leaf
            || entry.trigger.keyKind != KeyKind::CatchAll
            || entry.trigger.modifiers != Qt::NoModifier) {
            continue;
        }
        return std::any_of(entry.actions.cbegin(), entry.actions.cend(),
                           [](const QString &action) {
                               return isIgnoreAction(action);
                           });
    }
    return false;
}

std::optional<GhosttyKeybindMatch> GhosttyKeybindSet::match(
    int qtKey,
    Qt::KeyboardModifiers modifiers,
    QStringView text) const
{
    return match(GhosttyKeybindEvent{
        .qtKey = qtKey,
        .modifiers = modifiers,
        .text = text.toString(),
    });
}

std::optional<GhosttyKeybindMatch> GhosttyKeybindSet::match(
    const GhosttyKeybindEvent &event) const
{
    const Lookup found = lookup(0, event);
    if (found.entry == nullptr || found.entry->kind != EntryKind::Leaf) {
        return std::nullopt;
    }
    return GhosttyKeybindMatch{
        .actions = found.entry->actions,
        .consumed = found.entry->consumed,
        .performable = found.entry->performable,
        .physical = found.physical,
    };
}

GhosttyKeybindStep GhosttyKeybindSet::advance(
    const GhosttyKeybindEvent &event)
{
    const bool continuing = activeNode_.has_value();
    const quint32 node = activeNode_.value_or(0);
    const Lookup found = lookup(node, event);
    if (found.entry == nullptr) {
        if (!continuing || isModifierKey(event.qtKey)) {
            return {};
        }

        GhosttyKeybindStep result;
        result.kind = rootCatchAllIgnores()
            ? GhosttyKeybindStepKind::IgnoredSequence
            : GhosttyKeybindStepKind::InvalidSequence;
        result.queuedEvents = queuedEvents_;
        resetSequence();
        return result;
    }

    if (found.entry->kind == EntryKind::Leader) {
        if (!continuing) {
            queuedEvents_.clear();
        }
        queuedEvents_.append(event);
        activeNode_ = found.entry->child;
        return {
            .kind = GhosttyKeybindStepKind::Leader,
            .match = {},
            .queuedEvents = queuedEvents_,
        };
    }

    GhosttyKeybindStep result{
        .kind = GhosttyKeybindStepKind::Binding,
        .match = {
            .actions = found.entry->actions,
            .consumed = found.entry->consumed,
            .performable = found.entry->performable,
            .physical = found.physical,
        },
        .queuedEvents = queuedEvents_,
    };
    resetSequence();
    return result;
}

void GhosttyKeybindSet::resetSequence() noexcept
{
    activeNode_.reset();
    queuedEvents_.clear();
}
