#include "ghostty_action_catalog.h"

#include <QChar>
#include <QLatin1StringView>
#include <QtCore/qnamespace.h>

#include <cmath>
#include <utility>

namespace {

using Error = GhosttyActionTranslationError;
using OptionalView = std::optional<QStringView>;

bool equals(QStringView value, QLatin1StringView expected)
{
    return value == expected;
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
        || equals(actionName, QLatin1StringView("quit"));
}

bool isCatalogAction(QStringView actionName)
{
    return isVoidAction(actionName)
        || equals(actionName, QLatin1StringView("close_tab"))
        || equals(actionName, QLatin1StringView("new_split"))
        || equals(actionName, QLatin1StringView("goto_split"));
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
    if (equals(actionName, QLatin1StringView("quit"))) {
        return accept(WorkspaceAction::RequestQuit,
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

    // goto_split has no default and uses a custom parser in Binding.zig. The
    // cardinal directions are supported here; previous/next require an
    // ordering-aware workspace operation that does not exist yet. Ghostty's
    // backwards-compatible top/bottom spellings remain accepted.
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
    } else if (equals(*parameter, QLatin1StringView("previous"))
               || equals(*parameter, QLatin1StringView("next"))) {
        return reject(Error::UnsupportedParameter, actionName, parameter);
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
