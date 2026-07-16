#include "ghostty_action_catalog.h"

#include <QChar>
#include <QLatin1StringView>
#include <QtCore/qnamespace.h>

#include <cmath>
#include <limits>
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

QStringView actionName(QStringView serializedAction)
{
    const qsizetype colon = serializedAction.indexOf(u':');
    return colon < 0 ? serializedAction : serializedAction.first(colon);
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

GhosttyActionTranslation GhosttyActionCatalog::translate(
    QStringView serializedAction,
    WorkspaceActionContext context)
{
    const qsizetype colonIndex = serializedAction.indexOf(QChar(u':'));
    const QStringView actionName = colonIndex < 0
        ? serializedAction
        : serializedAction.first(colonIndex);
    const OptionalView parameter = colonIndex < 0
        ? OptionalView{}
        : OptionalView{serializedAction.sliced(colonIndex + 1)};

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

bool GhosttyActionCatalog::isImplemented(QStringView serializedAction)
{
    if (translate(serializedAction).accepted()) {
        return true;
    }

    const qsizetype colon = serializedAction.indexOf(u':');
    const QStringView name = colon < 0
        ? serializedAction
        : serializedAction.first(colon);
    const OptionalView parameter = colon < 0
        ? OptionalView{}
        : OptionalView{serializedAction.sliced(colon + 1)};

    if (name == QLatin1StringView("copy_to_clipboard")) {
        return !parameter.has_value()
            || *parameter == QLatin1StringView("plain")
            || *parameter == QLatin1StringView("mixed");
    }
    if (name == QLatin1StringView("paste_from_clipboard")
        || name == QLatin1StringView("paste_from_selection")
        || name == QLatin1StringView("reset_font_size")
        || name == QLatin1StringView("scroll_page_up")
        || name == QLatin1StringView("scroll_page_down")
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
    return false;
}

GhosttyActionScope GhosttyActionCatalog::scope(
    QStringView serializedAction)
{
    return isApplicationAction(actionName(serializedAction))
        ? GhosttyActionScope::Application
        : GhosttyActionScope::Surface;
}
