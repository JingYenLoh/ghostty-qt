#include "ghostty_action_catalog.h"

#include <QChar>
#include <QLatin1StringView>
#include <QtCore/qnamespace.h>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <locale.h>
#include <string>
#include <utility>

namespace {

using Error = GhosttyActionTranslationError;
using OptionalView = std::optional<QStringView>;

bool equals(QStringView value, QLatin1StringView expected)
{
    return value == expected;
}

std::optional<quint64> parseUnsignedInteger(QStringView value)
{
    // Binding.Action.parse ultimately delegates integer fields to
    // std.fmt.parseInt with base 10. Match its optional sign and interior
    // underscore handling rather than QString's more permissive conversion.
    if (value.isEmpty()) return std::nullopt;

    bool negative = false;
    if (value.front() == u'+' || value.front() == u'-') {
        negative = value.front() == u'-';
        value = value.sliced(1);
    }
    if (value.isEmpty() || value.front() == u'_'
        || value.back() == u'_') {
        return std::nullopt;
    }

    constexpr quint64 limit =
        static_cast<quint64>(std::numeric_limits<quintptr>::max());
    quint64 result = 0;
    for (const QChar character : value) {
        if (character == u'_') continue;
        if (character < u'0' || character > u'9') return std::nullopt;

        const quint64 digit = character.unicode() - u'0';
        if (result > (limit - digit) / 10) {
            return std::nullopt;
        }
        result = result * 10 + digit;
    }

    // Zig accepts -0 for unsigned parseInt fields, but no other negative
    // value can be represented by usize/u16.
    if (negative && result != 0) return std::nullopt;
    return result;
}

std::optional<qint64> parseSignedInteger(QStringView value)
{
    if (value.isEmpty()) return std::nullopt;

    bool negative = false;
    if (value.front() == u'+' || value.front() == u'-') {
        negative = value.front() == u'-';
        value = value.sliced(1);
    }
    if (value.isEmpty() || value.front() == u'_'
        || value.back() == u'_') {
        return std::nullopt;
    }

    constexpr quint64 positiveLimit =
        static_cast<quint64>(std::numeric_limits<qintptr>::max());
    constexpr quint64 negativeLimit = positiveLimit + 1;
    const quint64 limit = negative ? negativeLimit : positiveLimit;
    quint64 magnitude = 0;
    for (const QChar character : value) {
        if (character == u'_') continue;
        if (character < u'0' || character > u'9') return std::nullopt;

        const quint64 digit = character.unicode() - u'0';
        if (magnitude > (limit - digit) / 10) return std::nullopt;
        magnitude = magnitude * 10 + digit;
    }

    if (!negative) return static_cast<qint64>(magnitude);
    constexpr quint64 qint64NegativeLimit =
        static_cast<quint64>(std::numeric_limits<qint64>::max()) + 1;
    if (magnitude == qint64NegativeLimit) {
        return std::numeric_limits<qint64>::min();
    }
    return -static_cast<qint64>(magnitude);
}

bool isDigitForBase(QChar character, int base)
{
    if (character >= u'0' && character <= u'9') {
        return character.unicode() - u'0' < base;
    }
    if (base == 16) {
        const QChar lower = character.toLower();
        return lower >= u'a' && lower <= u'f';
    }
    return false;
}

bool consumeFloatDigits(QStringView value, qsizetype *index, int base,
                        std::string *normalized, bool *consumedAny)
{
    if (index == nullptr || normalized == nullptr || consumedAny == nullptr) {
        return false;
    }

    while (*index < value.size()) {
        const QChar character = value.at(*index);
        if (isDigitForBase(character, base)) {
            normalized->push_back(static_cast<char>(character.unicode()));
            *consumedAny = true;
            ++*index;
            continue;
        }
        if (character != u'_') break;

        // Zig's float parser permits underscores only when both neighbors are
        // digits in the active mantissa/exponent base. In particular, unlike
        // parseInt, consecutive underscores are invalid.
        if (*index == 0 || *index + 1 >= value.size()
            || !isDigitForBase(value.at(*index - 1), base)
            || !isDigitForBase(value.at(*index + 1), base)) {
            return false;
        }
        ++*index;
    }
    return true;
}

std::optional<float> parseFiniteFloat32(QStringView value)
{
    if (value.isEmpty()) return std::nullopt;

    qsizetype index = 0;
    bool negative = false;
    if (value.front() == u'+' || value.front() == u'-') {
        negative = value.front() == u'-';
        ++index;
    }
    if (index >= value.size()) return std::nullopt;

    const bool hexadecimal = index + 1 < value.size()
        && value.at(index) == u'0'
        && (value.at(index + 1) == u'x' || value.at(index + 1) == u'X');
    const int mantissaBase = hexadecimal ? 16 : 10;

    std::string normalized;
    normalized.reserve(static_cast<size_t>(value.size()));
    if (negative) normalized.push_back('-');
    if (hexadecimal) {
        normalized.append("0x");
        index += 2;
    }

    bool hasMantissaDigit = false;
    if (!consumeFloatDigits(value, &index, mantissaBase,
                            &normalized, &hasMantissaDigit)) {
        return std::nullopt;
    }
    if (index < value.size() && value.at(index) == u'.') {
        normalized.push_back('.');
        ++index;
        if (!consumeFloatDigits(value, &index, mantissaBase,
                                &normalized, &hasMantissaDigit)) {
            return std::nullopt;
        }
    }
    if (!hasMantissaDigit) return std::nullopt;

    const QChar exponentMarker = hexadecimal ? u'p' : u'e';
    if (index < value.size()
        && value.at(index).toLower() == exponentMarker) {
        normalized.push_back(static_cast<char>(value.at(index).unicode()));
        ++index;
        if (index < value.size()
            && (value.at(index) == u'+' || value.at(index) == u'-')) {
            normalized.push_back(static_cast<char>(value.at(index).unicode()));
            ++index;
        }
        bool hasExponentDigit = false;
        if (!consumeFloatDigits(value, &index, 10,
                                &normalized, &hasExponentDigit)
            || !hasExponentDigit) {
            return std::nullopt;
        }
    }
    if (index != value.size()) return std::nullopt;

    // The project is Linux-only, so use the POSIX locale-specific conversion
    // to obtain the same binary32 rounding independent of the process locale.
    // A strict lexer above prevents strtof_l's whitespace and NaN-payload
    // extensions from widening the accepted action grammar.
    locale_t numericLocale = newlocale(LC_NUMERIC_MASK, "C", nullptr);
    if (numericLocale == nullptr) return std::nullopt;
    char *end = nullptr;
    const float result = strtof_l(normalized.c_str(), &end, numericLocale);
    freelocale(numericLocale);
    if (end != normalized.data() + normalized.size()
        || !std::isfinite(result)) {
        return std::nullopt;
    }
    return result;
}

bool fitsSignedPointer(float value)
{
    if (!std::isfinite(value)) return false;
    value = std::trunc(value);

    // Converting intptr max to f32 rounds up to the next power of two on the
    // supported 32/64-bit targets. Step down once to obtain the largest f32
    // value whose integer conversion is representable.
    const float minimum =
        static_cast<float>(std::numeric_limits<qintptr>::min());
    const float maximum = std::nextafter(
        static_cast<float>(std::numeric_limits<qintptr>::max()), 0.0F);
    return value >= minimum && value <= maximum;
}

GhosttyActionTranslation reject(Error error,
                                 QStringView actionName,
                                 OptionalView parameter)
{
    GhosttyActionTranslation result;
    result.error = error;
    result.actionName = actionName.toString();
    if (parameter.has_value()) {
        result.parameter = parameter->toString();
    }
    return result;
}

GhosttyActionTranslation accept(WorkspaceAction action,
                                 WorkspaceActionContext context,
                                 QStringView actionName,
                                 OptionalView parameter)
{
    GhosttyActionTranslation result;
    result.request = WorkspaceActionRequest{action, context};
    result.error = Error::None;
    result.actionName = actionName.toString();
    if (parameter.has_value()) {
        result.parameter = parameter->toString();
    }
    return result;
}

bool isVoidAction(QStringView actionName)
{
    return equals(actionName, QLatin1StringView("new_tab"))
        || equals(actionName, QLatin1StringView("close_surface"))
        || equals(actionName, QLatin1StringView("previous_tab"))
        || equals(actionName, QLatin1StringView("next_tab"))
        || equals(actionName, QLatin1StringView("last_tab"))
        || equals(actionName, QLatin1StringView("toggle_split_zoom"))
        || equals(actionName, QLatin1StringView("equalize_splits"))
        || equals(actionName, QLatin1StringView("quit"));
}

bool isCatalogAction(QStringView actionName)
{
    return isVoidAction(actionName)
        || equals(actionName, QLatin1StringView("close_tab"))
        || equals(actionName, QLatin1StringView("new_split"))
        || equals(actionName, QLatin1StringView("goto_split"))
        || equals(actionName, QLatin1StringView("goto_tab"))
        || equals(actionName, QLatin1StringView("move_tab"))
        || equals(actionName, QLatin1StringView("resize_split"));
}

bool isApplicationAction(QStringView name)
{
    // Keep this list synchronized with Binding.Action.scope(). `unbind` does
    // not survive finalized configuration, but classifying it is harmless and
    // makes the mapping complete.
    return equals(name, QLatin1StringView("ignore"))
        || equals(name, QLatin1StringView("unbind"))
        || equals(name, QLatin1StringView("open_config"))
        || equals(name, QLatin1StringView("reload_config"))
        || equals(name, QLatin1StringView("close_all_windows"))
        || equals(name, QLatin1StringView("quit"))
        || equals(name, QLatin1StringView("toggle_quick_terminal"))
        || equals(name, QLatin1StringView("toggle_visibility"))
        || equals(name, QLatin1StringView("check_for_updates"))
        || equals(name, QLatin1StringView("show_gtk_inspector"))
        || equals(name, QLatin1StringView("new_window"))
        || equals(name, QLatin1StringView("undo"))
        || equals(name, QLatin1StringView("redo"));
}

} // namespace

GhosttySerializedActionView GhosttyActionCatalog::parseSerializedAction(
    QStringView serializedAction)
{
    const qsizetype colon = serializedAction.indexOf(u':');
    GhosttySerializedActionView result;
    result.name = colon < 0
        ? serializedAction
        : serializedAction.first(colon);
    if (colon >= 0) {
        result.parameter = serializedAction.sliced(colon + 1);
    }
    return result;
}

GhosttyActionTranslation GhosttyActionCatalog::translate(
    QStringView serializedAction,
    WorkspaceActionContext context)
{
    const GhosttySerializedActionView parsed =
        parseSerializedAction(serializedAction);
    const QStringView actionName = parsed.name;
    const OptionalView parameter = parsed.parameter;

    // Binding.Action.parse reports an empty action name as InvalidFormat.
    if (actionName.isEmpty()) {
        return reject(Error::InvalidFormat, actionName, parameter);
    }

    if (!isCatalogAction(actionName)) {
        return reject(Error::UnsupportedAction, actionName, parameter);
    }

    // Ghostty's void actions reject the presence of a colon, including a
    // trailing colon with an empty value.
    if (isVoidAction(actionName) && parameter.has_value()) {
        return reject(Error::InvalidFormat, actionName, parameter);
    }

    if (equals(actionName, QLatin1StringView("new_tab"))) {
        return accept(WorkspaceAction::NewTab, context, actionName, parameter);
    }
    if (equals(actionName, QLatin1StringView("close_surface"))) {
        return accept(WorkspaceAction::ClosePane, context, actionName, parameter);
    }
    if (equals(actionName, QLatin1StringView("previous_tab"))) {
        context.value = -1;
        return accept(WorkspaceAction::ChangeTabRelative,
                      context,
                      actionName,
                      parameter);
    }
    if (equals(actionName, QLatin1StringView("next_tab"))) {
        context.value = 1;
        return accept(WorkspaceAction::ChangeTabRelative,
                      context,
                      actionName,
                      parameter);
    }
    if (equals(actionName, QLatin1StringView("last_tab"))) {
        return accept(WorkspaceAction::ActivateLastTab,
                      context,
                      actionName,
                      parameter);
    }
    if (equals(actionName, QLatin1StringView("toggle_split_zoom"))) {
        return accept(WorkspaceAction::ToggleSplitZoom,
                      context,
                      actionName,
                      parameter);
    }
    if (equals(actionName, QLatin1StringView("equalize_splits"))) {
        return accept(WorkspaceAction::EqualizeSplits,
                      context,
                      actionName,
                      parameter);
    }
    if (equals(actionName, QLatin1StringView("quit"))) {
        return accept(WorkspaceAction::RequestQuit,
                      context,
                      actionName,
                      parameter);
    }

    if (equals(actionName, QLatin1StringView("goto_tab"))) {
        if (!parameter.has_value()) {
            return reject(Error::InvalidFormat, actionName, parameter);
        }
        const std::optional<quint64> index =
            parseUnsignedInteger(*parameter);
        if (!index.has_value()) {
            return reject(Error::InvalidFormat, actionName, parameter);
        }
        // Preserve successful usize parsing across qint64 storage. The
        // execution layer rejects values above Ghostty's c_int boundary, so
        // qint64::max is an unambiguous rejection sentinel for the upper half
        // of Linux's usize range.
        context.value = *index > static_cast<quint64>(
                                      std::numeric_limits<qint64>::max())
            ? std::numeric_limits<qint64>::max()
            : static_cast<qint64>(*index);
        return accept(WorkspaceAction::ActivateTabByIndex,
                      context,
                      actionName,
                      parameter);
    }

    if (equals(actionName, QLatin1StringView("move_tab"))) {
        if (!parameter.has_value()) {
            return reject(Error::InvalidFormat, actionName, parameter);
        }
        const std::optional<qint64> offset = parseSignedInteger(*parameter);
        if (!offset.has_value()) {
            return reject(Error::InvalidFormat, actionName, parameter);
        }
        context.value = *offset;
        return accept(WorkspaceAction::MoveTab,
                      context,
                      actionName,
                      parameter);
    }

    if (equals(actionName, QLatin1StringView("close_tab"))) {
        // CloseTabMode.default is .this in Binding.zig.
        if (!parameter.has_value()
            || equals(*parameter, QLatin1StringView("this"))) {
            return accept(WorkspaceAction::CloseTab,
                          context,
                          actionName,
                          parameter);
        }
        if (equals(*parameter, QLatin1StringView("other"))
            || equals(*parameter, QLatin1StringView("right"))) {
            return reject(Error::UnsupportedParameter, actionName, parameter);
        }
        return reject(Error::InvalidFormat, actionName, parameter);
    }

    if (equals(actionName, QLatin1StringView("new_split"))) {
        // SplitDirection.default is .auto. It is valid Ghostty syntax but the
        // current workspace can only place a new split to the right or down.
        if (!parameter.has_value()) {
            return reject(Error::UnsupportedParameter, actionName, parameter);
        }
        if (equals(*parameter, QLatin1StringView("right"))) {
            return accept(WorkspaceAction::SplitRight,
                          context,
                          actionName,
                          parameter);
        }
        if (equals(*parameter, QLatin1StringView("down"))) {
            return accept(WorkspaceAction::SplitDown,
                          context,
                          actionName,
                          parameter);
        }
        if (equals(*parameter, QLatin1StringView("left"))
            || equals(*parameter, QLatin1StringView("up"))
            || equals(*parameter, QLatin1StringView("auto"))) {
            return reject(Error::UnsupportedParameter, actionName, parameter);
        }
        return reject(Error::InvalidFormat, actionName, parameter);
    }

    if (equals(actionName, QLatin1StringView("resize_split"))) {
        if (!parameter.has_value()) {
            return reject(Error::InvalidFormat, actionName, parameter);
        }
        const qsizetype comma = parameter->indexOf(u',');
        if (comma < 0 || parameter->indexOf(u',', comma + 1) >= 0) {
            return reject(Error::InvalidFormat, actionName, parameter);
        }

        const QStringView direction = parameter->first(comma);
        if (equals(direction, QLatin1StringView("left"))) {
            context.value = Qt::Key_Left;
        } else if (equals(direction, QLatin1StringView("right"))) {
            context.value = Qt::Key_Right;
        } else if (equals(direction, QLatin1StringView("up"))) {
            context.value = Qt::Key_Up;
        } else if (equals(direction, QLatin1StringView("down"))) {
            context.value = Qt::Key_Down;
        } else {
            return reject(Error::InvalidFormat, actionName, parameter);
        }

        const std::optional<quint64> amount =
            parseUnsignedInteger(parameter->sliced(comma + 1));
        if (!amount.has_value()
            || *amount > std::numeric_limits<quint16>::max()) {
            return reject(Error::InvalidFormat, actionName, parameter);
        }
        context.amount = static_cast<int>(*amount);
        return accept(WorkspaceAction::ResizeSplit,
                      context,
                      actionName,
                      parameter);
    }

    // goto_split has no default and uses a custom parser in Binding.zig.
    // Ghostty's backwards-compatible top/bottom spellings remain accepted.
    if (!parameter.has_value()) {
        return reject(Error::InvalidFormat, actionName, parameter);
    }

    if (equals(*parameter, QLatin1StringView("left"))) {
        context.value = Qt::Key_Left;
    } else if (equals(*parameter, QLatin1StringView("right"))) {
        context.value = Qt::Key_Right;
    } else if (equals(*parameter, QLatin1StringView("up"))
               || equals(*parameter, QLatin1StringView("top"))) {
        context.value = Qt::Key_Up;
    } else if (equals(*parameter, QLatin1StringView("down"))
               || equals(*parameter, QLatin1StringView("bottom"))) {
        context.value = Qt::Key_Down;
    } else if (equals(*parameter, QLatin1StringView("previous"))) {
        context.value = -1;
        return accept(WorkspaceAction::NavigatePaneRelative,
                      context,
                      actionName,
                      parameter);
    } else if (equals(*parameter, QLatin1StringView("next"))) {
        context.value = 1;
        return accept(WorkspaceAction::NavigatePaneRelative,
                      context,
                      actionName,
                      parameter);
    } else {
        return reject(Error::InvalidFormat, actionName, parameter);
    }

    return accept(WorkspaceAction::NavigatePane,
                  context,
                  actionName,
                  parameter);
}

std::optional<GhosttyPaneAction> GhosttyActionCatalog::parsePaneAction(
    QStringView serializedAction)
{
    const GhosttySerializedActionView parsed =
        parseSerializedAction(serializedAction);
    const QStringView name = parsed.name;
    const OptionalView parameter = parsed.parameter;

    const auto viewportAction = [](TerminalViewportRequest::Kind kind) {
        GhosttyPaneAction action;
        action.kind = GhosttyPaneActionKind::ScrollViewport;
        action.viewport.kind = kind;
        return action;
    };

    if (name == QLatin1StringView("scroll_to_top")
        || name == QLatin1StringView("scroll_to_bottom")
        || name == QLatin1StringView("scroll_to_selection")) {
        if (parameter.has_value()) return std::nullopt;
        if (name == QLatin1StringView("scroll_to_top")) {
            return viewportAction(TerminalViewportRequest::Kind::Top);
        }
        if (name == QLatin1StringView("scroll_to_bottom")) {
            return viewportAction(TerminalViewportRequest::Kind::Bottom);
        }
        return viewportAction(TerminalViewportRequest::Kind::Selection);
    }

    if (name == QLatin1StringView("scroll_to_row")) {
        if (!parameter.has_value()) return std::nullopt;
        const std::optional<quint64> row = parseUnsignedInteger(*parameter);
        if (!row.has_value()) return std::nullopt;
        GhosttyPaneAction action =
            viewportAction(TerminalViewportRequest::Kind::Row);
        action.viewport.row = *row;
        return action;
    }

    if (name == QLatin1StringView("scroll_page_up")
        || name == QLatin1StringView("scroll_page_down")) {
        if (parameter.has_value()) return std::nullopt;
        GhosttyPaneAction action;
        action.kind = name == QLatin1StringView("scroll_page_up")
            ? GhosttyPaneActionKind::ScrollPageUp
            : GhosttyPaneActionKind::ScrollPageDown;
        return action;
    }

    if (name == QLatin1StringView("scroll_page_fractional")) {
        if (!parameter.has_value()) return std::nullopt;
        const std::optional<float> fraction = parseFiniteFloat32(*parameter);
        // The terminal always has at least one row. A fraction that cannot be
        // converted safely even at that minimum can never be executable.
        if (!fraction.has_value() || !fitsSignedPointer(*fraction)) {
            return std::nullopt;
        }
        GhosttyPaneAction action;
        action.kind = GhosttyPaneActionKind::ScrollPageFractional;
        action.pageFraction = *fraction;
        return action;
    }

    if (name == QLatin1StringView("scroll_page_lines")) {
        if (!parameter.has_value()) return std::nullopt;
        const std::optional<qint64> lines = parseSignedInteger(*parameter);
        if (!lines.has_value()
            || *lines < std::numeric_limits<qint16>::min()
            || *lines > std::numeric_limits<qint16>::max()) {
            return std::nullopt;
        }
        GhosttyPaneAction action =
            viewportAction(TerminalViewportRequest::Kind::Delta);
        action.viewport.delta = *lines;
        return action;
    }

    if (name == QLatin1StringView("select_all")) {
        if (parameter.has_value()) return std::nullopt;
        GhosttyPaneAction action;
        action.kind = GhosttyPaneActionKind::SelectAll;
        return action;
    }

    if (name == QLatin1StringView("start_search")
        || name == QLatin1StringView("end_search")
        || name == QLatin1StringView("search_selection")) {
        // These are void Binding.Action fields, so even an explicitly empty
        // parameter is invalid.
        if (parameter.has_value()) return std::nullopt;
        GhosttyPaneAction action;
        if (name == QLatin1StringView("start_search")) {
            action.kind = GhosttyPaneActionKind::StartSearch;
        } else if (name == QLatin1StringView("end_search")) {
            action.kind = GhosttyPaneActionKind::EndSearch;
        } else {
            action.kind = GhosttyPaneActionKind::SearchSelection;
        }
        return action;
    }

    if (name == QLatin1StringView("search")) {
        // Search is a []const u8 field. Its colon is required, its payload may
        // be empty, and only the first colon separates the action name.
        if (!parameter.has_value()) return std::nullopt;
        GhosttyPaneAction action;
        action.kind = GhosttyPaneActionKind::Search;
        action.payload = parameter->toString();
        return action;
    }

    if (name == QLatin1StringView("navigate_search")) {
        if (!parameter.has_value()) return std::nullopt;
        GhosttyPaneAction action;
        action.kind = GhosttyPaneActionKind::NavigateSearch;
        if (*parameter == QLatin1StringView("previous")) {
            action.searchDirection = TerminalSearchDirection::Previous;
        } else if (*parameter == QLatin1StringView("next")) {
            action.searchDirection = TerminalSearchDirection::Next;
        } else {
            return std::nullopt;
        }
        return action;
    }

    if (name == QLatin1StringView("csi")
        || name == QLatin1StringView("esc")
        || name == QLatin1StringView("text")) {
        // All three fields are []const u8 in Binding.Action, so the colon is
        // required but the byte string after it may be empty. Split only at
        // the first colon; later colons are part of the payload.
        if (!parameter.has_value()) return std::nullopt;
        GhosttyPaneAction action;
        if (name == QLatin1StringView("csi")) {
            action.kind = GhosttyPaneActionKind::Csi;
        } else if (name == QLatin1StringView("esc")) {
            action.kind = GhosttyPaneActionKind::Esc;
        } else {
            action.kind = GhosttyPaneActionKind::Text;
        }
        action.payload = parameter->toString();
        return action;
    }

    if (name == QLatin1StringView("reset")) {
        // Reset is a void Binding.Action field, therefore even `reset:` is
        // invalid rather than an empty-parameter spelling of reset.
        if (parameter.has_value()) return std::nullopt;
        GhosttyPaneAction action;
        action.kind = GhosttyPaneActionKind::Reset;
        return action;
    }

    if (name != QLatin1StringView("adjust_selection")) {
        return std::nullopt;
    }
    if (!parameter.has_value()) return std::nullopt;

    GhosttyPaneAction action;
    action.kind = GhosttyPaneActionKind::AdjustSelection;
    if (*parameter == QLatin1StringView("left")) {
        action.selectionAdjustment = TerminalSelectionAdjustment::Left;
    } else if (*parameter == QLatin1StringView("right")) {
        action.selectionAdjustment = TerminalSelectionAdjustment::Right;
    } else if (*parameter == QLatin1StringView("up")) {
        action.selectionAdjustment = TerminalSelectionAdjustment::Up;
    } else if (*parameter == QLatin1StringView("down")) {
        action.selectionAdjustment = TerminalSelectionAdjustment::Down;
    } else if (*parameter == QLatin1StringView("page_up")) {
        action.selectionAdjustment = TerminalSelectionAdjustment::PageUp;
    } else if (*parameter == QLatin1StringView("page_down")) {
        action.selectionAdjustment = TerminalSelectionAdjustment::PageDown;
    } else if (*parameter == QLatin1StringView("home")) {
        action.selectionAdjustment = TerminalSelectionAdjustment::Home;
    } else if (*parameter == QLatin1StringView("end")) {
        action.selectionAdjustment = TerminalSelectionAdjustment::End;
    } else if (*parameter == QLatin1StringView("beginning_of_line")) {
        action.selectionAdjustment =
            TerminalSelectionAdjustment::BeginningOfLine;
    } else if (*parameter == QLatin1StringView("end_of_line")) {
        action.selectionAdjustment = TerminalSelectionAdjustment::EndOfLine;
    } else {
        return std::nullopt;
    }
    return action;
}

bool GhosttyActionCatalog::isImplemented(QStringView serializedAction)
{
    if (parsePaneAction(serializedAction).has_value()) {
        return true;
    }

    const GhosttySerializedActionView parsed =
        parseSerializedAction(serializedAction);
    const QStringView name = parsed.name;
    const OptionalView parameter = parsed.parameter;

    if (name == QLatin1StringView("copy_to_clipboard")) {
        return !parameter.has_value()
            || *parameter == QLatin1StringView("plain")
            || *parameter == QLatin1StringView("mixed");
    }
    if (name == QLatin1StringView("paste_from_clipboard")
        || name == QLatin1StringView("paste_from_selection")
        || name == QLatin1StringView("copy_url_to_clipboard")
        || name == QLatin1StringView("reset_font_size")
        || name == QLatin1StringView("reload_config")
        || name == QLatin1StringView("end_key_sequence")
        || name == QLatin1StringView("close_window")
        || name == QLatin1StringView("ignore")) {
        return !parameter.has_value();
    }
    if (name == QLatin1StringView("activate_key_table")
        || name == QLatin1StringView("activate_key_table_once")) {
        // Binding.Action.parse accepts an empty string parameter. Whether the
        // named table exists is a pane-state question handled at execution.
        return parameter.has_value();
    }
    if (name == QLatin1StringView("deactivate_key_table")
        || name == QLatin1StringView("deactivate_all_key_tables")) {
        return !parameter.has_value();
    }
    if (name == QLatin1StringView("increase_font_size")
        || name == QLatin1StringView("decrease_font_size")) {
        if (!parameter.has_value()) {
            return true;
        }
        bool valid = false;
        const double amount = parameter->toString().toDouble(&valid);
        return valid && std::isfinite(amount) && amount > 0.0;
    }
    return translate(serializedAction).accepted();
}

GhosttyActionScope GhosttyActionCatalog::scope(
    QStringView serializedAction)
{
    return isApplicationAction(parseSerializedAction(serializedAction).name)
        ? GhosttyActionScope::Application
        : GhosttyActionScope::Surface;
}
