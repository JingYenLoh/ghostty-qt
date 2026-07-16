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
    for (const Binding &binding : bindings_) {
        result.append(binding.actions);
    }
    return result;
}

GhosttyKeybindLoadReport GhosttyKeybindSet::load(const QStringList &values)
{
    bindings_.clear();
    GhosttyKeybindLoadReport report;
    report.records.reserve(values.size());

    // Chains must be immediately adjacent to the binding they extend.
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
            if (chainTarget < 0 || chainTarget >= bindings_.size()) {
                report.records.append(record(input, Disposition::Unsupported,
                                             Reason::OrphanChain,
                                             QStringLiteral("chain has no adjacent supported binding")));
            } else {
                bindings_[chainTarget].actions.append(action);
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

        const QStringView triggerText = left.sliced(triggerStart);
        if (triggerText.contains(u'>')) {
            report.records.append(record(input, Disposition::Unsupported,
                                         Reason::Sequence));
            continue;
        }
        if (flags.nonLocal) {
            report.records.append(record(input, Disposition::Unsupported,
                                         Reason::NonLocal));
            continue;
        }

        ParsedTrigger trigger;
        if (!parseTrigger(triggerText, &trigger, &error)) {
            report.records.append(record(input, Disposition::Invalid,
                                         Reason::None, std::move(error)));
            continue;
        }
        if (trigger.catchAll) {
            report.records.append(record(input, Disposition::Unsupported,
                                         Reason::CatchAll));
            continue;
        }

        Binding binding{
            .keyKind = trigger.physical ? KeyKind::Physical : KeyKind::Unicode,
            .qtKey = trigger.qtKey,
            .nativeScanCode = trigger.nativeScanCode,
            .keypad = trigger.keypad,
            .unicode = trigger.unicode,
            .modifiers = normalizedModifiers(trigger.modifiers),
            .actions = {action},
            .consumed = flags.consumed,
            .performable = flags.performable,
        };

        const auto duplicate = std::find_if(
            bindings_.begin(), bindings_.end(),
            [&binding](const Binding &existing) {
                if (existing.keyKind != binding.keyKind
                    || existing.modifiers != binding.modifiers) {
                    return false;
                }
                if (binding.keyKind == KeyKind::Unicode) {
                    return sameUnicode(existing.unicode, binding.unicode);
                }
                if (existing.nativeScanCode != 0
                    && binding.nativeScanCode != 0) {
                    return existing.nativeScanCode == binding.nativeScanCode;
                }
                return existing.qtKey == binding.qtKey
                    && existing.keypad == binding.keypad;
            });
        if (duplicate == bindings_.end()) {
            bindings_.append(std::move(binding));
            chainTarget = bindings_.size() - 1;
        } else {
            *duplicate = std::move(binding);
            chainTarget = std::distance(bindings_.begin(), duplicate);
        }
        report.records.append(record(input, Disposition::Installed));
    }

    return report;
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
    const Qt::KeyboardModifiers normalized =
        normalizedModifiers(event.modifiers);

    const auto makeMatch = [](const Binding &binding) {
        return GhosttyKeybindMatch{
            .actions = binding.actions,
            .consumed = binding.consumed,
            .performable = binding.performable,
            .physical = binding.keyKind == KeyKind::Physical,
        };
    };

    // Ghostty's Binding.Set::getEvent explicitly prioritizes the physical
    // key before UTF-8/unshifted Unicode lookup.
    for (const Binding &binding : bindings_) {
        // Depending on the QPA backend and event direction, modifiers() can
        // describe the state immediately before or after the event. Permit a
        // physical modifier trigger to ignore its own bit while preserving
        // every other configured modifier.
        const Qt::KeyboardModifiers withoutSelf =
            withoutTriggeredModifier(binding.qtKey, normalized);
        if (binding.keyKind == KeyKind::Physical
            && (binding.modifiers == normalized
                || binding.modifiers == withoutSelf)
            && physicalMatches(binding.qtKey,
                               binding.nativeScanCode,
                               binding.keypad,
                               event.qtKey,
                               event.nativeScanCode,
                               event.modifiers)) {
            return makeMatch(binding);
        }
    }

    QVector<QString> unicodeCandidates;
    const auto appendCandidate = [&unicodeCandidates](QString candidate) {
        if (candidate.isEmpty()) {
            return;
        }
        const bool duplicate = std::any_of(
            unicodeCandidates.cbegin(), unicodeCandidates.cend(),
            [&candidate](const QString &existing) {
                return sameUnicode(existing, candidate);
            });
        if (!duplicate) {
            unicodeCandidates.append(std::move(candidate));
        }
    };

    appendCandidate(unicodeFromQtEvent(event.qtKey, event.text));
    if (event.unshiftedCodepoint != 0
        && event.unshiftedCodepoint <= 0x10ffffU
        && !(event.unshiftedCodepoint >= 0xd800U
             && event.unshiftedCodepoint <= 0xdfffU)) {
        const char32_t codepoint =
            static_cast<char32_t>(event.unshiftedCodepoint);
        appendCandidate(QString::fromUcs4(&codepoint, 1));
    }

    const auto findUnicode = [this, &makeMatch](
                                 const QVector<QString> &candidates,
                                 Qt::KeyboardModifiers candidateModifiers)
        -> std::optional<GhosttyKeybindMatch> {
        for (const QString &unicode : candidates) {
            for (const Binding &binding : bindings_) {
                if (binding.keyKind == KeyKind::Unicode
                    && binding.modifiers == candidateModifiers
                    && sameUnicode(binding.unicode, unicode)) {
                    return makeMatch(binding);
                }
            }
        }
        return std::nullopt;
    };

    if (const auto exact = findUnicode(unicodeCandidates, normalized)) {
        return exact;
    }
    return std::nullopt;
}
