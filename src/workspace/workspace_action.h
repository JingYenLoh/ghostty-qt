#pragma once

#include "workspace/workspace_ids.h"

#include <QString>
#include <QtTypes>

#include <functional>
#include <utility>
#include <variant>

enum class CloseTabMode {
    This,
    Other,
    Right,
};

enum class WorkspaceAction {
    NewTab,
    ActivateTab,
    ActivatePane,
    CloseTab,
    ClosePane,
    SplitLeft,
    SplitRight,
    SplitUp,
    SplitDown,
    SplitAuto,
    NavigatePane,
    NavigatePaneRelative,
    ChangeTabRelative,
    ActivateTabByIndex,
    ActivateLastTab,
    MoveTab,
    MoveTabToNewWindow,
    SetSurfaceTitle,
    PromptSurfaceTitle,
    PromptTabTitle,
    SetTabTitle,
    PromptWindowTitle,
    SetWindowTitle,
    ResizeSplit,
    EqualizeSplits,
    ToggleSplitZoom,
    ToggleFullscreen,
    ToggleMaximize,
    ToggleWindowDecorations,
};

struct WorkspaceActionContext {
    TabId tabId;
    PaneId paneId;
    qint64 value = 0;
    int amount = 0;
    CloseTabMode closeTabMode = CloseTabMode::This;

    friend bool operator==(const WorkspaceActionContext &,
                           const WorkspaceActionContext &) = default;
};

struct WorkspaceActionRequest {
    WorkspaceAction action = WorkspaceAction::NewTab;
    WorkspaceActionContext context;
    // Owning text payload for workspace actions whose canonical Ghostty
    // representation carries arbitrary bytes. Translation validates and
    // converts those bytes before the request crosses a component boundary.
    QString payload = {};

    friend bool operator==(const WorkspaceActionRequest &,
                           const WorkspaceActionRequest &) = default;
};

// Frontend-owned surface actions are kept separate from topology actions.
// Every variant alternative owns exactly its valid payload, so parameterized
// actions cannot accidentally be dispatched with an unrelated context value
// or string payload.
namespace WorkspaceFrontendActions {

struct ToggleCommandPalette {
    bool operator==(const ToggleCommandPalette &) const = default;
};

struct ToggleTabOverview {
    bool operator==(const ToggleTabOverview &) const = default;
};

struct ShowOnScreenKeyboard {
    bool operator==(const ShowOnScreenKeyboard &) const = default;
};

enum class InspectorMode : quint8 {
    Toggle,
    Show,
    Hide,
};

struct Inspector {
    InspectorMode mode = InspectorMode::Toggle;
    bool operator==(const Inspector &) const = default;
};

enum class CrashTarget : quint8 {
    Main,
    Io,
    Render,
};

struct Crash {
    CrashTarget target = CrashTarget::Main;
    bool operator==(const Crash &) const = default;
};

} // namespace WorkspaceFrontendActions

using WorkspaceFrontendAction =
    std::variant<WorkspaceFrontendActions::ToggleCommandPalette,
                 WorkspaceFrontendActions::ToggleTabOverview,
                 WorkspaceFrontendActions::ShowOnScreenKeyboard,
                 WorkspaceFrontendActions::Inspector,
                 WorkspaceFrontendActions::Crash>;

struct WorkspaceFrontendActionRequest {
    WorkspaceFrontendAction action;
    WorkspaceActionContext context;

    friend bool operator==(const WorkspaceFrontendActionRequest &,
                           const WorkspaceFrontendActionRequest &) = default;

    template <typename Action>
    [[nodiscard]] const Action *getIf() const noexcept
    {
        return std::get_if<Action>(&action);
    }
};

class WorkspaceActionDispatcher final {
public:
    using Handler = std::function<bool(const WorkspaceActionRequest &)>;

    WorkspaceActionDispatcher() = default;
    explicit WorkspaceActionDispatcher(Handler handler)
        : handler_(std::move(handler))
    {}

    void setHandler(Handler handler) { handler_ = std::move(handler); }

    [[nodiscard]] bool dispatch(const WorkspaceActionRequest &request) const
    {
        return handler_ != nullptr && handler_(request);
    }

    [[nodiscard]] bool dispatch(WorkspaceAction action,
                                WorkspaceActionContext context = {}) const
    {
        return dispatch(WorkspaceActionRequest{action, context});
    }

private:
    Handler handler_;
};

Q_DECLARE_METATYPE(WorkspaceAction)
Q_DECLARE_METATYPE(CloseTabMode)
Q_DECLARE_METATYPE(WorkspaceActionContext)
Q_DECLARE_METATYPE(WorkspaceActionRequest)
Q_DECLARE_METATYPE(WorkspaceFrontendActions::InspectorMode)
Q_DECLARE_METATYPE(WorkspaceFrontendActions::CrashTarget)
Q_DECLARE_METATYPE(WorkspaceFrontendAction)
Q_DECLARE_METATYPE(WorkspaceFrontendActionRequest)
