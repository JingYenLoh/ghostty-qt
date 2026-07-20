#pragma once

#include "workspace_ids.h"

#include <QString>
#include <QtTypes>

#include <functional>
#include <utility>

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
    SetTabTitle,
    ResizeSplit,
    EqualizeSplits,
    ToggleSplitZoom,
    ToggleFullscreen,
    RequestQuit,
};

struct WorkspaceActionContext {
    TabId tabId;
    PaneId paneId;
    qint64 value = 0;
    int amount = 0;

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

class WorkspaceActionDispatcher final {
public:
    using Handler = std::function<bool(const WorkspaceActionRequest &)>;

    WorkspaceActionDispatcher() = default;
    explicit WorkspaceActionDispatcher(Handler handler)
        : handler_(std::move(handler))
    {
    }

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
Q_DECLARE_METATYPE(WorkspaceActionContext)
Q_DECLARE_METATYPE(WorkspaceActionRequest)
