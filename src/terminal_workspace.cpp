#include "terminal_workspace.h"

#include "ghostty_action_catalog.h"
#include "terminal_pane.h"

#include <QCursor>
#include <QDebug>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QScopedValueRollback>
#include <QSGSimpleRectNode>
#include <QTimer>

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <limits>
#include <ranges>
#include <utility>

namespace {

constexpr qreal splitGap = 2.0;
constexpr qreal splitDividerZ = 1.0;

struct AxisWeights {
    [[nodiscard]] std::size_t along(
        Qt::Orientation orientation) const noexcept
    {
        return orientation == Qt::Horizontal ? horizontal : vertical;
    }

    std::size_t horizontal = 1;
    std::size_t vertical = 1;
};

qreal splitExtent(const QRectF &geometry, Qt::Orientation orientation)
{
    const qreal extent = orientation == Qt::Horizontal
        ? geometry.width()
        : geometry.height();
    return std::max(0.0, extent - splitGap);
}

} // namespace

struct TerminalWorkspace::PaneHandle {
    PaneId id;
    TerminalPane *pane = nullptr;
};

struct TerminalWorkspace::Node {
    explicit Node(PaneHandle handle)
        : paneId(handle.id)
        , pane(handle.pane)
    {
    }

    bool isLeaf() const { return pane != nullptr; }

    void collectPanes(std::vector<PaneHandle> &panes) const
    {
        if (isLeaf()) {
            panes.push_back({paneId, pane});
            return;
        }
        if (first != nullptr) first->collectPanes(panes);
        if (second != nullptr) second->collectPanes(panes);
    }

    [[nodiscard]] AxisWeights equalize()
    {
        if (isLeaf()) return {};

        const AxisWeights firstWeights = first != nullptr
            ? first->equalize() : AxisWeights{};
        const AxisWeights secondWeights = second != nullptr
            ? second->equalize() : AxisWeights{};
        const std::size_t firstWeight = firstWeights.along(orientation);
        const std::size_t secondWeight = secondWeights.along(orientation);
        const std::size_t combinedWeight = firstWeight + secondWeight;
        ratio = static_cast<qreal>(firstWeight)
            / static_cast<qreal>(combinedWeight);

        return orientation == Qt::Horizontal
            ? AxisWeights{combinedWeight, 1}
            : AxisWeights{1, combinedWeight};
    }

    PaneId paneId;
    TerminalPane *pane = nullptr;
    quint64 splitId = 0;
    Qt::Orientation orientation = Qt::Horizontal;
    qreal ratio = 0.5;
    QRectF geometry;
    std::unique_ptr<Node> first;
    std::unique_ptr<Node> second;
};

struct TerminalWorkspace::Tab {
    TabId id;
    std::unique_ptr<Node> root;
    PaneId activePaneId;
    PaneId zoomedPaneId;
};

class TerminalWorkspace::SplitDividerItem final : public QQuickItem {
public:
    SplitDividerItem(quint64 splitId, TerminalWorkspace *workspace)
        : QQuickItem(workspace)
        , splitId_(splitId)
        , workspace_(workspace)
    {
        setObjectName(QStringLiteral("_ghosttyQtSplitDivider"));
        setAcceptedMouseButtons(Qt::LeftButton);
        setAcceptHoverEvents(true);
        setFocusPolicy(Qt::NoFocus);
        setZ(splitDividerZ);
        setFlag(ItemHasContents);
    }

    void setOrientation(Qt::Orientation orientation)
    {
        setCursor(orientation == Qt::Horizontal
            ? Qt::SplitHCursor : Qt::SplitVCursor);
    }

    void markCurrent(quint64 generation)
    {
        generation_ = generation;
    }

    void setColor(std::optional<QColor> color)
    {
        if (color.has_value() && !color->isValid()) {
            color.reset();
        }
        if (color_ == color) {
            return;
        }
        color_ = std::move(color);
        update();
    }

    [[nodiscard]] bool isCurrent(quint64 generation) const
    {
        return generation_ == generation;
    }

protected:
    void geometryChange(const QRectF &newGeometry,
                        const QRectF &oldGeometry) override
    {
        QQuickItem::geometryChange(newGeometry, oldGeometry);
        if (newGeometry.size() != oldGeometry.size()) {
            update();
        }
    }

    QSGNode *updatePaintNode(QSGNode *oldNode,
                             UpdatePaintNodeData *) override
    {
        if (!color_.has_value()) {
            delete oldNode;
            return nullptr;
        }
        auto *node = oldNode != nullptr
            ? static_cast<QSGSimpleRectNode *>(oldNode)
            : new QSGSimpleRectNode;
        node->setRect(boundingRect());
        node->setColor(*color_);
        return node;
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || workspace_ == nullptr) {
            event->ignore();
            return;
        }
        const std::optional<SplitDividerDrag> drag =
            workspace_->beginSplitDividerDrag(
                splitId_, mapToItem(workspace_, event->position()));
        if (!drag.has_value()) {
            event->ignore();
            return;
        }
        drag_ = *drag;
        dragging_ = true;
        setKeepMouseGrab(true);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!dragging_) {
            event->ignore();
            return;
        }
        if (workspace_ == nullptr || !(event->buttons() & Qt::LeftButton)
            || !workspace_->dragSplitDivider(
                splitId_, mapToItem(workspace_, event->position()),
                drag_)) {
            cancelDragging();
        }
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!dragging_ || event->button() != Qt::LeftButton) {
            event->ignore();
            return;
        }
        if (workspace_ != nullptr) {
            (void) workspace_->dragSplitDivider(
                splitId_, mapToItem(workspace_, event->position()),
                drag_);
        }
        clearDragging();
        event->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        mousePressEvent(event);
    }

    void mouseUngrabEvent() override
    {
        clearDragging();
    }

private:
    void clearDragging()
    {
        dragging_ = false;
        setKeepMouseGrab(false);
    }

    void cancelDragging()
    {
        clearDragging();
        ungrabMouse();
    }

    quint64 splitId_ = 0;
    TerminalWorkspace *workspace_ = nullptr;
    SplitDividerDrag drag_;
    std::optional<QColor> color_;
    quint64 generation_ = 0;
    bool dragging_ = false;
};

LaunchOptions TerminalWorkspace::defaultOptions_;

TerminalWorkspace::TerminalWorkspace(QQuickItem *parent)
    : QQuickItem(parent)
    , effectiveOptions_(defaultOptions_)
    , actionDispatcher_([this](const WorkspaceActionRequest &request) {
        return executeAction(request);
    })
{
    setClip(true);
    QTimer::singleShot(0, this, [this] {
        if (tabs_.empty()) {
            newTab();
        }
    });
}

TerminalWorkspace::~TerminalWorkspace() = default;

void TerminalWorkspace::setSearchOverlayComponent(QQmlComponent *component)
{
    if (searchOverlayComponent_ == component) {
        return;
    }

    searchOverlayComponent_ = component;
    if (component != nullptr) {
        connect(component, &QObject::destroyed, this, [this, component] {
            if (searchOverlayComponent_ == component) {
                searchOverlayComponent_ = nullptr;
                Q_EMIT searchOverlayComponentChanged();
            }
        });

        for (const std::unique_ptr<Tab> &tab : tabs_) {
            std::vector<PaneHandle> panes;
            tab->root->collectPanes(panes);
            for (const PaneHandle &handle : panes) {
                createSearchOverlay(handle.pane);
            }
        }
    }

    Q_EMIT searchOverlayComponentChanged();
}

void TerminalWorkspace::createSearchOverlay(TerminalPane *pane)
{
    constexpr auto attachedProperty = "_ghosttyQtSearchOverlayAttached";
    if (pane == nullptr || searchOverlayComponent_ == nullptr
        || pane->property(attachedProperty).toBool()) {
        return;
    }

    QObject *overlay = searchOverlayComponent_->createWithInitialProperties({
        {QStringLiteral("terminalPane"),
         QVariant::fromValue(static_cast<QObject *>(pane))},
    });
    if (overlay == nullptr) {
        qWarning().noquote()
            << "Could not create terminal search overlay:"
            << searchOverlayComponent_->errorString();
        return;
    }

    auto *overlayItem = qobject_cast<QQuickItem *>(overlay);
    if (overlayItem == nullptr) {
        qWarning() << "Terminal search overlay component did not create a QQuickItem";
        delete overlay;
        return;
    }

    overlay->setParent(pane);
    overlayItem->setParentItem(pane);
    pane->setProperty(attachedProperty, true);
}

void TerminalWorkspace::setDefaultLaunchOptions(const LaunchOptions &options)
{
    defaultOptions_ = options;
}

void TerminalWorkspace::applyConfigSnapshot(const GhosttyConfigSnapshot &snapshot)
{
    const bool wasTabBarVisible = tabBarVisible();
    effectiveOptions_ = applyGhosttyConfigSnapshot(defaultOptions_, snapshot);
    if (tabBarVisible() != wasTabBarVisible) {
        Q_EMIT tabBarVisibleChanged();
    }
    for (SplitDividerItem *divider : std::as_const(splitDividers_)) {
        divider->setColor(effectiveOptions_.splitAppearance.dividerColor);
    }
    for (const std::unique_ptr<Tab> &tab : tabs_) {
        std::vector<PaneHandle> panes;
        tab->root->collectPanes(panes);
        for (const PaneHandle &handle : panes) {
            handle.pane->applyRuntimeOptions(effectiveOptions_);
        }
    }
    reevaluatePendingClose();
}

bool TerminalWorkspace::tabBarVisible() const
{
    switch (effectiveOptions_.windowShowTabBar) {
    case WindowShowTabBar::Always:
        return true;
    case WindowShowTabBar::Auto:
        return tabs_.size() >= 2;
    case WindowShowTabBar::Never:
        return false;
    }
    Q_UNREACHABLE_RETURN(false);
}

QStringList TerminalWorkspace::tabTitles() const
{
    QStringList result;
    result.reserve(tabModel_.count());
    for (int index = 0; index < tabModel_.count(); ++index) {
        result.append(tabModel_.data(tabModel_.index(index, 0),
                                     TabListModel::TitleRole).toString());
    }
    return result;
}

QString TerminalWorkspace::currentTitle() const
{
    const TabListEntry *entry = tabModel_.entryAt(currentIndex_);
    if (entry == nullptr) {
        return {};
    }
    return entry->titleOverride.isEmpty() ? entry->title : entry->titleOverride;
}

bool TerminalWorkspace::dispatchAction(const WorkspaceActionRequest &request)
{
    return actionDispatcher_.dispatch(request);
}

bool TerminalWorkspace::executeApplicationConfiguredAction(QStringView action)
{
    if (!GhosttyActionCatalog::isImplemented(action)) {
        return false;
    }
    const GhosttySerializedActionView parsed =
        GhosttyActionCatalog::parseSerializedAction(action);
    const QStringView name = parsed.name;
    if (name == QLatin1StringView("ignore")) {
        return true;
    }
    if (name == QLatin1StringView("reload_config")) {
        Q_EMIT configReloadRequested();
        return true;
    }
    if (name == QLatin1StringView("quit")) {
        requestQuitImpl();
        return true;
    }
    return false;
}

bool TerminalWorkspace::executeSurfaceActionOnAllPanes(QStringView action)
{
    // Actions may remove panes or tabs. QPointer keeps this stable snapshot
    // safe while preserving Ghostty's action-major fanout order at the
    // process-level caller.
    std::vector<QPointer<TerminalPane>> panes;
    for (const std::unique_ptr<Tab> &tab : tabs_) {
        std::vector<PaneHandle> tabPanes;
        tab->root->collectPanes(tabPanes);
        panes.reserve(panes.size() + tabPanes.size());
        for (const PaneHandle &handle : tabPanes) {
            panes.emplace_back(handle.pane);
        }
    }

    // Ghostty scopes fullscreen to a source surface, but every Qt pane in a
    // workspace ultimately mutates the same host window. Applying the toggle
    // synchronously once per pane would cancel itself for an even pane count.
    // Keep broad dispatch once per registered workspace/window while leaving
    // all other surface actions on the ordinary per-pane fanout path.
    const GhosttyActionTranslation translated =
        GhosttyActionCatalog::translate(action);
    if (translated.accepted()
        && translated.request->action == WorkspaceAction::ToggleFullscreen) {
        return !panes.empty() && dispatchAction(*translated.request);
    }

    QScopedValueRollback<bool> broadFanout(broadActionFanout_, true);
    bool performed = false;
    for (const QPointer<TerminalPane> &pane : panes) {
        if (pane != nullptr) {
            performed = pane->executeConfiguredAction(action) || performed;
        }
    }
    return performed;
}

bool TerminalWorkspace::executeAction(const WorkspaceActionRequest &request)
{
    const auto contextMatchesPane = [this, &request] {
        return !request.context.tabId.isValid()
            || tabIdForPane(request.context.paneId) == request.context.tabId;
    };
    switch (request.action) {
    case WorkspaceAction::NewTab: {
        PaneId sourcePaneId;
        if (request.context.paneId.isValid()) {
            if (paneForId(request.context.paneId) == nullptr
                || !contextMatchesPane()) {
                return false;
            }
            sourcePaneId = request.context.paneId;
        } else if (request.context.tabId.isValid()) {
            const Tab *tab = tabById(request.context.tabId);
            if (tab == nullptr) return false;
            sourcePaneId = tab->activePaneId;
        }
        createNewTab(sourcePaneId);
        return true;
    }
    case WorkspaceAction::ActivateTab:
        if (tabById(request.context.tabId) == nullptr) return false;
        activateTab(request.context.tabId);
        return true;
    case WorkspaceAction::ActivatePane:
        if (paneForId(request.context.paneId) == nullptr
            || !contextMatchesPane()) return false;
        activatePane(request.context.paneId);
        return true;
    case WorkspaceAction::CloseTab:
        if (tabById(request.context.tabId) == nullptr) return false;
        closeTab(request.context.tabId);
        return true;
    case WorkspaceAction::ClosePane:
        if (paneForId(request.context.paneId) == nullptr
            || !contextMatchesPane()) return false;
        closePane(request.context.paneId);
        return true;
    case WorkspaceAction::SplitLeft:
    case WorkspaceAction::SplitRight:
    case WorkspaceAction::SplitUp:
    case WorkspaceAction::SplitDown:
    case WorkspaceAction::SplitAuto: {
        TerminalPane *sourcePane = paneForId(request.context.paneId);
        if (sourcePane == nullptr || !contextMatchesPane()) return false;
        WorkspaceAction direction = request.action;
        if (direction == WorkspaceAction::SplitAuto) {
            const qreal devicePixelRatio = sourcePane->window() != nullptr
                ? sourcePane->window()->devicePixelRatio()
                : 1.0;
            const int surfaceWidth = std::max(
                1, qRound(sourcePane->width() * devicePixelRatio));
            const int surfaceHeight = std::max(
                1, qRound(sourcePane->height() * devicePixelRatio));
            direction = surfaceWidth > surfaceHeight
                ? WorkspaceAction::SplitRight
                : WorkspaceAction::SplitDown;
        }
        const bool horizontal = direction == WorkspaceAction::SplitLeft
            || direction == WorkspaceAction::SplitRight;
        const bool placeNewPaneFirst = direction == WorkspaceAction::SplitLeft
            || direction == WorkspaceAction::SplitUp;
        splitPane(request.context.paneId,
                  horizontal ? Qt::Horizontal : Qt::Vertical,
                  placeNewPaneFirst);
        return true;
    }
    case WorkspaceAction::NavigatePane:
        if (paneForId(request.context.paneId) == nullptr
            || !contextMatchesPane()) return false;
        return navigateFrom(request.context.paneId,
                            static_cast<int>(request.context.value));
    case WorkspaceAction::NavigatePaneRelative:
        if (paneForId(request.context.paneId) == nullptr
            || !contextMatchesPane()) return false;
        return navigateRelative(request.context.paneId,
                                request.context.value);
    case WorkspaceAction::ChangeTabRelative:
        if (request.context.tabId.isValid()
            && tabById(request.context.tabId) == nullptr) return false;
        return changeTabRelativeImpl(static_cast<int>(request.context.value),
                                     request.context.tabId);
    case WorkspaceAction::ActivateTabByIndex:
        return activateTabByIndex(request.context.value);
    case WorkspaceAction::ActivateLastTab:
        return activateTabByIndex(static_cast<qint64>(tabs_.size()));
    case WorkspaceAction::MoveTab:
        if ((request.context.tabId.isValid()
             && tabById(request.context.tabId) == nullptr)
            || (request.context.paneId.isValid()
                && (paneForId(request.context.paneId) == nullptr
                    || !contextMatchesPane()))) {
            return false;
        }
        return moveTab(request.context.tabId.isValid()
                           ? request.context.tabId
                           : tabIdForPane(request.context.paneId),
                       request.context.value);
    case WorkspaceAction::ResizeSplit:
        if (paneForId(request.context.paneId) == nullptr
            || !contextMatchesPane()) return false;
        return resizeSplit(request.context.paneId,
                           static_cast<int>(request.context.value),
                           request.context.amount);
    case WorkspaceAction::EqualizeSplits: {
        if (request.context.paneId.isValid() && !contextMatchesPane()) {
            return false;
        }
        const TabId tabId = request.context.tabId.isValid()
            ? request.context.tabId
            : (request.context.paneId.isValid()
                   ? tabIdForPane(request.context.paneId)
                   : currentTabId());
        return equalizeSplits(tabId);
    }
    case WorkspaceAction::ToggleSplitZoom: {
        if (request.context.paneId.isValid() && !contextMatchesPane()) {
            return false;
        }
        const TabId tabId = request.context.tabId.isValid()
            ? request.context.tabId
            : (request.context.paneId.isValid()
                   ? tabIdForPane(request.context.paneId)
                   : currentTabId());
        return toggleSplitZoom(tabId);
    }
    case WorkspaceAction::ToggleFullscreen:
        if ((request.context.paneId.isValid()
             && (paneForId(request.context.paneId) == nullptr
                 || !contextMatchesPane()))
            || (request.context.tabId.isValid()
                && tabById(request.context.tabId) == nullptr)
            || window() == nullptr) {
            return false;
        }
        Q_EMIT toggleFullscreenRequested();
        return true;
    case WorkspaceAction::RequestQuit:
        requestQuitImpl();
        return true;
    }
    return false;
}

TerminalWorkspace::Tab *TerminalWorkspace::currentTab()
{
    if (currentIndex_ < 0 || currentIndex_ >= static_cast<int>(tabs_.size())) {
        return nullptr;
    }
    return tabs_[static_cast<size_t>(currentIndex_)].get();
}

const TerminalWorkspace::Tab *TerminalWorkspace::currentTab() const
{
    if (currentIndex_ < 0 || currentIndex_ >= static_cast<int>(tabs_.size())) {
        return nullptr;
    }
    return tabs_[static_cast<size_t>(currentIndex_)].get();
}

TerminalWorkspace::Tab *TerminalWorkspace::tabById(TabId id)
{
    const int index = tabIndexForId(id);
    return index >= 0 ? tabs_[static_cast<size_t>(index)].get() : nullptr;
}

const TerminalWorkspace::Tab *TerminalWorkspace::tabById(TabId id) const
{
    const int index = tabIndexForId(id);
    return index >= 0 ? tabs_[static_cast<size_t>(index)].get() : nullptr;
}

TabId TerminalWorkspace::currentTabId() const
{
    const Tab *tab = currentTab();
    return tab != nullptr ? tab->id : TabId{};
}

PaneId TerminalWorkspace::currentPaneId() const
{
    const Tab *tab = currentTab();
    return tab != nullptr ? tab->activePaneId : PaneId{};
}

TerminalWorkspace::PaneHandle TerminalWorkspace::createPane(
    const LaunchOptions &options)
{
    const PaneId paneId(nextPaneId_++);
    auto *pane = new TerminalPane(options, this);
    pane->setVisible(false);
    pane->setWorkspaceActionHandler(
        [this, paneId](WorkspaceActionRequest request) {
            request.context.tabId = tabIdForPane(paneId);
            request.context.paneId = paneId;
            if (broadActionFanout_) {
                switch (request.action) {
                case WorkspaceAction::SplitLeft:
                case WorkspaceAction::SplitRight:
                case WorkspaceAction::SplitUp:
                case WorkspaceAction::SplitDown:
                case WorkspaceAction::NavigatePane:
                case WorkspaceAction::NavigatePaneRelative:
                case WorkspaceAction::ResizeSplit:
                    if (const Tab *tab = tabById(request.context.tabId);
                        tab != nullptr) {
                        // Pinned GTK resolves these container operations from
                        // the split tree's current active surface, even while
                        // an all/global fanout is visiting another surface.
                        request.context.paneId = tab->activePaneId;
                    }
                    break;
                default:
                    break;
                }
            }
            return dispatchAction(request);
        });
    connect(pane, &TerminalPane::activated, this,
            [this, paneId](TerminalPane *) {
                dispatchAction({WorkspaceAction::ActivatePane,
                                {TabId{}, paneId, 0}});
            });
    connect(pane, &TerminalPane::titleChanged, this,
            [this, paneId] { refreshTab(tabIdForPane(paneId)); });
    connect(pane, &TerminalPane::currentDirectoryChanged, this,
            [this, paneId] { refreshTab(tabIdForPane(paneId)); });
    connect(pane, &TerminalPane::sessionEnded, this,
            [this, paneId, pane](TerminalPane *, int, int) {
                removePendingPastesForPane({paneId, pane});
                refreshTab(tabIdForPane(paneId));
            });
    connect(pane, &TerminalPane::processStateChanged, this,
            [this, paneId] {
                refreshTab(tabIdForPane(paneId));
                reevaluatePendingClose();
            });
    connect(pane, &TerminalPane::requestNewTab, this,
            [this, paneId] {
                dispatchAction({WorkspaceAction::NewTab,
                                {tabIdForPane(paneId), paneId, 0}});
            });
    connect(pane, &TerminalPane::requestSplit, this,
            [this, paneId](WorkspaceAction action) {
                dispatchAction({action,
                                {tabIdForPane(paneId), paneId, 0}});
            });
    connect(pane, &TerminalPane::requestClose, this,
            [this, paneId] {
                dispatchAction({WorkspaceAction::ClosePane,
                                {tabIdForPane(paneId), paneId, 0}});
            });
    connect(pane, &TerminalPane::requestCloseTab, this,
            [this, paneId] {
                const TabId tabId = tabIdForPane(paneId);
                dispatchAction({WorkspaceAction::CloseTab,
                                {tabId, PaneId{}, 0}});
            });
    connect(pane, &TerminalPane::requestNavigate, this,
            [this, paneId](int direction) {
                dispatchAction({WorkspaceAction::NavigatePane,
                                {tabIdForPane(paneId), paneId, direction}});
            });
    connect(pane, &TerminalPane::requestTabChange, this,
            [this, paneId](int delta) {
        dispatchAction({WorkspaceAction::ChangeTabRelative,
                        {tabIdForPane(paneId), paneId, delta}});
    });
    connect(pane, &TerminalPane::requestQuit, this, [this] {
        dispatchAction({WorkspaceAction::RequestQuit, {}});
    });
    connect(pane, &TerminalPane::requestConfigReload,
            this, &TerminalWorkspace::configReloadRequested);
    connect(pane, &TerminalPane::broadActionsRequested,
            this, &TerminalWorkspace::broadActionsRequested);
    connect(pane, &TerminalPane::unsafePasteRequested, this,
            [this, paneId, pane](quint64 requestId, const QString &text,
                                 TerminalPane *) {
                if (paneForId(paneId) != pane) {
                    pane->cancelPaste(requestId);
                    return;
                }
                beginUnsafePaste(requestId, text, paneId);
            });
    createSearchOverlay(pane);
    return {paneId, pane};
}

void TerminalWorkspace::newTab()
{
    dispatchAction({WorkspaceAction::NewTab, {}});
}

void TerminalWorkspace::createNewTab(PaneId sourcePaneId)
{
    const bool wasTabBarVisible = tabBarVisible();
    LaunchOptions options = effectiveOptions_;
    if (initialTabCreated_) {
        TerminalPane *sourcePane = paneForId(sourcePaneId);
        if (sourcePane == nullptr) {
            sourcePane = paneForId(currentPaneId());
        }
        if (sourcePane != nullptr) {
            options = sourcePane->tabLaunchOptions(effectiveOptions_);
        } else {
            options.program.clear();
            options.hold = false;
        }
    }
    initialTabCreated_ = true;

    const int insertionIndex =
        options.windowNewTabPosition == WindowNewTabPosition::Current
            && currentIndex_ >= 0
        ? currentIndex_ + 1
        : static_cast<int>(tabs_.size());

    const PaneHandle pane = createPane(options);
    auto tab = std::make_unique<Tab>();
    tab->id = TabId(nextTabId_++);
    tab->root = std::make_unique<Node>(pane);
    tab->activePaneId = pane.id;
    const TabId tabId = tab->id;
    const TabListEntry entry = tabListEntry(*tab);
    tabs_.insert(tabs_.begin() + insertionIndex, std::move(tab));

    const bool modelInserted = tabModel_.insert(insertionIndex, entry);
    Q_ASSERT(modelInserted);
    Q_UNUSED(modelInserted);
    Q_EMIT tabTitlesChanged();
    if (tabBarVisible() != wasTabBarVisible) {
        Q_EMIT tabBarVisibleChanged();
    }
    activateTab(tabId);
}

void TerminalWorkspace::setCurrentIndex(int index)
{
    if (tabs_.empty()) {
        currentIndex_ = -1;
        return;
    }
    index = std::clamp(index, 0, static_cast<int>(tabs_.size()) - 1);
    dispatchAction({WorkspaceAction::ActivateTab,
                    {tabs_[static_cast<size_t>(index)]->id, PaneId{}, 0}});
}

void TerminalWorkspace::activateTab(TabId id)
{
    const int index = tabIndexForId(id);
    if (index < 0) {
        return;
    }
    if (currentIndex_ == index) {
        if (TerminalPane *pane = paneForId(currentPaneId()); pane != nullptr) {
            pane->focusTerminal();
        }
        return;
    }

    if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(tabs_.size())) {
        updateTabVisibility(*tabs_[static_cast<size_t>(currentIndex_)], false);
    }
    currentIndex_ = index;
    updateTabVisibility(*tabs_[static_cast<size_t>(currentIndex_)], true);
    layoutCurrentTab();
    Q_EMIT currentIndexChanged();
    Q_EMIT currentTitleChanged();
    if (TerminalPane *pane = paneForId(currentPaneId()); pane != nullptr) {
        pane->focusTerminal();
    }
}

bool TerminalWorkspace::activateTabByIndex(qint64 oneBasedIndex)
{
    // The binding stores usize, but Ghostty's frontend action ABI and GTK tab
    // model narrow numeric destinations to c_int before clamping.
    if (tabs_.empty() || oneBasedIndex <= 0
        || oneBasedIndex > std::numeric_limits<int>::max()) {
        return false;
    }
    const qint64 last = static_cast<qint64>(tabs_.size()) - 1;
    const int target = static_cast<int>(std::min(oneBasedIndex - 1, last));
    if (target == currentIndex_) {
        return false;
    }
    activateTab(tabs_[static_cast<size_t>(target)]->id);
    return true;
}

bool TerminalWorkspace::moveTab(TabId tabId, qint64 delta)
{
    if (tabs_.size() <= 1) {
        return false;
    }
    const int source = tabId.isValid()
        ? tabIndexForId(tabId)
        : currentIndex_;
    if (source < 0) {
        return false;
    }

    const qint64 count = static_cast<qint64>(tabs_.size());
    const qint64 offset = delta % count;
    const int destination = static_cast<int>(
        (static_cast<qint64>(source) + offset + count) % count);
    if (source == destination) {
        return false;
    }

    const TabId sourceId = tabs_[static_cast<size_t>(source)]->id;
    const TabId selected = currentTabId();
    std::unique_ptr<Tab> moved = std::move(tabs_[static_cast<size_t>(source)]);
    tabs_.erase(tabs_.begin() + source);
    tabs_.insert(tabs_.begin() + destination, std::move(moved));
    const bool modelMoved = tabModel_.move(sourceId, destination);
    Q_ASSERT(modelMoved);
    Q_UNUSED(modelMoved);

    const int previousCurrentIndex = currentIndex_;
    currentIndex_ = tabIndexForId(selected);
    Q_EMIT tabTitlesChanged();
    if (currentIndex_ != previousCurrentIndex) {
        Q_EMIT currentIndexChanged();
    }
    return true;
}

bool TerminalWorkspace::changeTabRelativeImpl(int delta, TabId origin)
{
    if (tabs_.empty()) {
        return false;
    }
    const int count = static_cast<int>(tabs_.size());
    const int base = origin.isValid() ? tabIndexForId(origin) : currentIndex_;
    if (base < 0) {
        return false;
    }
    const int target = (base + delta % count + count) % count;
    if (target == currentIndex_) {
        return false;
    }
    setCurrentIndex(target);
    return true;
}

void TerminalWorkspace::splitRight()
{
    dispatchAction({WorkspaceAction::SplitRight,
                    {TabId{}, currentPaneId(), 0}});
}

void TerminalWorkspace::splitDown()
{
    dispatchAction({WorkspaceAction::SplitDown,
                    {TabId{}, currentPaneId(), 0}});
}

void TerminalWorkspace::splitPane(PaneId paneId, Qt::Orientation orientation,
                                  bool placeNewPaneFirst)
{
    const TabId tabId = tabIdForPane(paneId);
    Tab *tab = tabById(tabId);
    if (tab == nullptr) {
        return;
    }
    Node *node = findNode(tab->root.get(), paneId);
    if (node == nullptr || !node->isLeaf()) {
        return;
    }

    TerminalPane *oldPane = node->pane;
    const PaneHandle newPane = createPane(
        oldPane->splitLaunchOptions(effectiveOptions_));
    // Ghostty resets split zoom whenever the tree structure is split.
    tab->zoomedPaneId = {};
    const PaneHandle oldHandle{node->paneId, oldPane};
    node->paneId = {};
    node->pane = nullptr;
    do {
        node->splitId = nextSplitId_++;
    } while (node->splitId == 0);
    node->orientation = orientation;
    node->ratio = 0.5;
    node->first = std::make_unique<Node>(
        placeNewPaneFirst ? newPane : oldHandle);
    node->second = std::make_unique<Node>(
        placeNewPaneFirst ? oldHandle : newPane);
    tab->activePaneId = newPane.id;
    updateSplitMembership(*tab);
    const bool targetIsCurrent = tabId == currentTabId();
    if (targetIsCurrent) {
        layoutCurrentTab();
        newPane.pane->focusTerminal();
    } else {
        updateTabVisibility(*tab, false);
    }
    refreshTab(tabId);
}

void TerminalWorkspace::activatePane(PaneId paneId)
{
    const int tabIndex = tabIndexForPane(paneId);
    if (tabIndex < 0) {
        return;
    }
    Tab *targetTab = tabs_[static_cast<size_t>(tabIndex)].get();
    if (targetTab->zoomedPaneId.isValid()
        && targetTab->zoomedPaneId != paneId) {
        // A tab must never advertise a hidden active pane. Default Ghostty
        // behavior clears zoom when focus moves to another split.
        targetTab->zoomedPaneId = {};
        refreshTab(targetTab->id);
        if (tabIndex == currentIndex_) {
            layoutCurrentTab();
        }
    }
    if (tabIndex != currentIndex_) {
        activateTab(targetTab->id);
    }
    Tab *tab = currentTab();
    if (tab != nullptr && tab->activePaneId != paneId) {
        tab->activePaneId = paneId;
        refreshTab(tab->id);
    }
    if (TerminalPane *pane = paneForId(paneId); pane != nullptr) {
        pane->focusTerminal();
    }
}

void TerminalWorkspace::closeActivePane()
{
    dispatchAction({WorkspaceAction::ClosePane,
                    {TabId{}, currentPaneId(), 0}});
}

void TerminalWorkspace::closePane(PaneId paneId, bool force)
{
    TerminalPane *pane = paneForId(paneId);
    if (pane == nullptr) {
        return;
    }
    const int tabIndex = tabIndexForPane(paneId);
    if (tabIndex < 0) {
        return;
    }
    if (!force && shouldConfirmClose(effectiveOptions_.confirmCloseMode,
                                     pane->isRunning(),
                                     pane->hasActiveProcess())) {
        pendingClose_ = PendingClose::Pane;
        pendingPaneId_ = paneId;
        pendingTabId_ = tabs_[static_cast<size_t>(tabIndex)]->id;
        Q_EMIT closeConfirmationRequested(
            QStringLiteral("A process is still running in this pane. Close it?"));
        return;
    }

    Tab *tab = tabs_[static_cast<size_t>(tabIndex)].get();
    const TabId tabId = tab->id;
    const bool removedActivePane = tab->activePaneId == paneId;
    if (tab->zoomedPaneId == paneId) {
        tab->zoomedPaneId = {};
    }
    resolvePendingPaneRemoval({paneId, pane});
    removePaneFromNode(tab->root, paneId);
    if (tab->root == nullptr) {
        removeTab(tabId);
        return;
    }
    if (removedActivePane || paneForId(tab->activePaneId) == nullptr) {
        tab->activePaneId = firstPaneId(tab->root.get());
    }
    updateSplitMembership(*tab);
    if (tabIndex == currentIndex_) {
        layoutCurrentTab();
        if (TerminalPane *activePane = paneForId(tab->activePaneId);
            activePane != nullptr) {
            activePane->focusTerminal();
        }
    }
    refreshTab(tabId);
}

bool TerminalWorkspace::removePaneFromNode(std::unique_ptr<Node> &node,
                                           PaneId paneId)
{
    if (node == nullptr) {
        return false;
    }
    if (node->isLeaf()) {
        if (node->paneId != paneId) {
            return false;
        }
        node->pane->beginShutdown();
        node->pane->setVisible(false);
        node->pane->deleteLater();
        node.reset();
        return true;
    }

    const bool removed = removePaneFromNode(node->first, paneId)
        || removePaneFromNode(node->second, paneId);
    if (!removed || node == nullptr) {
        return removed;
    }
    if (node->first == nullptr) {
        node = std::move(node->second);
    } else if (node->second == nullptr) {
        node = std::move(node->first);
    }
    return true;
}

void TerminalWorkspace::closeCurrentTab()
{
    dispatchAction({WorkspaceAction::CloseTab,
                    {currentTabId(), PaneId{}, 0}});
}

void TerminalWorkspace::closeTab(TabId tabId, bool force)
{
    const Tab *tab = tabById(tabId);
    if (tab == nullptr) {
        return;
    }
    if (!force && shouldConfirmTabClose(*tab)) {
        pendingClose_ = PendingClose::Tab;
        pendingPaneId_ = {};
        pendingTabId_ = tabId;
        Q_EMIT closeConfirmationRequested(
            QStringLiteral("Processes are still running in this tab. Close the tab?"));
        return;
    }
    removeTab(tabId);
}

void TerminalWorkspace::removeTab(TabId tabId)
{
    const int index = tabIndexForId(tabId);
    if (index < 0) {
        return;
    }
    const bool wasTabBarVisible = tabBarVisible();
    const bool removedCurrentTab = index == currentIndex_;
    std::vector<PaneHandle> panes;
    if (const Node *root = tabs_[static_cast<size_t>(index)]->root.get();
        root != nullptr) {
        root->collectPanes(panes);
    }
    for (const PaneHandle &handle : panes) {
        resolvePendingPaneRemoval(handle);
        handle.pane->beginShutdown();
        handle.pane->setVisible(false);
        handle.pane->deleteLater();
    }
    tabs_.erase(tabs_.begin() + index);
    tabModel_.remove(tabId);
    Q_EMIT tabTitlesChanged();
    if (tabBarVisible() != wasTabBarVisible) {
        Q_EMIT tabBarVisibleChanged();
    }
    resolvePendingTabRemoval(tabId);

    if (tabs_.empty()) {
        currentIndex_ = -1;
        updateSplitDividers(nullptr);
        Q_EMIT currentIndexChanged();
        Q_EMIT currentTitleChanged();
        approveQuit();
        return;
    }

    if (pendingClose_ == PendingClose::Quit && !shouldConfirmWorkspaceClose()) {
        pendingClose_ = PendingClose::None;
        pendingPaneId_ = {};
        pendingTabId_ = {};
        Q_EMIT closeConfirmationResolved();
        approveQuit();
        return;
    }

    if (removedCurrentTab) {
        const int nextIndex = std::min(index, static_cast<int>(tabs_.size()) - 1);
        currentIndex_ = -1;
        activateTab(tabs_[static_cast<size_t>(nextIndex)]->id);
    } else if (index < currentIndex_) {
        --currentIndex_;
        Q_EMIT currentIndexChanged();
    }
}

void TerminalWorkspace::resolvePendingPaneRemoval(PaneHandle handle)
{
    removePendingPastesForPane(handle);
    if (pendingClose_ != PendingClose::Pane || pendingPaneId_ != handle.id) {
        return;
    }
    pendingClose_ = PendingClose::None;
    pendingPaneId_ = {};
    pendingTabId_ = {};
    Q_EMIT closeConfirmationResolved();
}

void TerminalWorkspace::resolvePendingTabRemoval(TabId tabId)
{
    if (pendingClose_ != PendingClose::Tab || pendingTabId_ != tabId) {
        return;
    }
    pendingClose_ = PendingClose::None;
    pendingPaneId_ = {};
    pendingTabId_ = {};
    Q_EMIT closeConfirmationResolved();
}

void TerminalWorkspace::reevaluatePendingClose()
{
    if (pendingClose_ == PendingClose::None) {
        return;
    }

    const PendingClose action = pendingClose_;
    const PaneId paneId = pendingPaneId_;
    const TabId tabId = pendingTabId_;
    if (action == PendingClose::Pane) {
        TerminalPane *pane = paneForId(paneId);
        if (pane != nullptr
            && shouldConfirmClose(effectiveOptions_.confirmCloseMode,
                                  pane->isRunning(),
                                  pane->hasActiveProcess())) {
            return;
        }
    } else if (action == PendingClose::Tab) {
        const Tab *tab = tabById(tabId);
        if (tab != nullptr && shouldConfirmTabClose(*tab)) {
            return;
        }
    } else if (action == PendingClose::Quit
               && shouldConfirmWorkspaceClose()) {
        return;
    }

    pendingClose_ = PendingClose::None;
    pendingPaneId_ = {};
    pendingTabId_ = {};
    Q_EMIT closeConfirmationResolved();

    if (action == PendingClose::Pane && paneForId(paneId) != nullptr) {
        closePane(paneId, true);
    } else if (action == PendingClose::Tab && tabById(tabId) != nullptr) {
        closeTab(tabId, true);
    } else if (action == PendingClose::Quit) {
        approveQuit();
    }
}

void TerminalWorkspace::requestQuit()
{
    dispatchAction({WorkspaceAction::RequestQuit, {}});
}

void TerminalWorkspace::requestQuitImpl()
{
    if (pendingClose_ == PendingClose::Quit) {
        return;
    }
    if (shouldConfirmWorkspaceClose()) {
        pendingClose_ = PendingClose::Quit;
        pendingPaneId_ = {};
        pendingTabId_ = {};
        Q_EMIT closeConfirmationRequested(
            QStringLiteral("Terminal processes are still running. Quit and terminate them?"));
        return;
    }
    approveQuit();
}

void TerminalWorkspace::approveQuit()
{
    if (quitApprovedEmitted_) {
        return;
    }
    quitApprovedEmitted_ = true;
    // Start every worker shutdown before QObject hierarchy teardown. The
    // per-process grace periods then overlap on their independent threads
    // instead of accumulating serially on the GUI thread.
    for (const std::unique_ptr<Tab> &tab : tabs_) {
        std::vector<PaneHandle> panes;
        tab->root->collectPanes(panes);
        for (const PaneHandle &handle : panes) {
            handle.pane->beginShutdown();
        }
    }
    Q_EMIT quitApproved();
}

void TerminalWorkspace::confirmClose()
{
    const PendingClose action = pendingClose_;
    const PaneId paneId = pendingPaneId_;
    const TabId tabId = pendingTabId_;
    pendingClose_ = PendingClose::None;
    pendingPaneId_ = {};
    pendingTabId_ = {};

    if (action == PendingClose::Pane && tabIdForPane(paneId) == tabId) {
        closePane(paneId, true);
    } else if (action == PendingClose::Tab) {
        closeTab(tabId, true);
    } else if (action == PendingClose::Quit) {
        approveQuit();
    }
}

void TerminalWorkspace::cancelClose()
{
    pendingClose_ = PendingClose::None;
    pendingPaneId_ = {};
    pendingTabId_ = {};
}

void TerminalWorkspace::beginUnsafePaste(quint64 requestId,
                                         const QString &text,
                                         PaneId paneId)
{
    if (requestId == 0 || text.isEmpty() || !paneId.isValid()) {
        return;
    }

    const auto matchesTarget = [paneId, requestId](
                                   const PendingPasteTarget &target) {
        return target.paneId == paneId && target.requestId == requestId;
    };
    for (const PendingPaste &pending : std::as_const(pendingPastes_)) {
        if (std::ranges::any_of(pending.targets, matchesTarget)) {
            return;
        }
    }

    const bool firstRequest = pendingPastes_.isEmpty();
    if (!firstRequest && pendingPastes_.front().text == text) {
        PendingPaste &active = pendingPastes_.front();
        const bool alreadyTargetsPane = std::ranges::contains(
            std::as_const(active.targets), paneId,
            &PendingPasteTarget::paneId);
        if (!alreadyTargetsPane) {
            active.targets.append({paneId, requestId});
            return;
        }
    }

    pendingPastes_.append({text, {{paneId, requestId}}});
    if (firstRequest) {
        showPendingPastePreview();
    }
}

QString TerminalWorkspace::pastePreview(const QString &text)
{
    constexpr qsizetype limit = 240;
    const bool truncated = text.size() > limit;
    QString preview = text.left(limit);
    preview.replace(QChar(0x1b), QStringLiteral("␛"));
    preview.replace(QLatin1Char('\n'), QStringLiteral("↵\n"));
    if (truncated) {
        preview.append(QStringLiteral("…"));
    }
    return preview;
}

void TerminalWorkspace::showPendingPastePreview()
{
    if (pendingPastes_.isEmpty() || activePasteConfirmationId_ != 0) {
        return;
    }
    do {
        ++nextPasteConfirmationId_;
    } while (nextPasteConfirmationId_ == 0);
    activePasteConfirmationId_ = nextPasteConfirmationId_;
    Q_EMIT unsafePasteConfirmationRequested(
        activePasteConfirmationId_,
        pastePreview(pendingPastes_.front().text));
}

void TerminalWorkspace::schedulePendingPastePreview()
{
    if (pendingPastes_.isEmpty() || pendingPastePreviewScheduled_) {
        return;
    }
    pendingPastePreviewScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        pendingPastePreviewScheduled_ = false;
        showPendingPastePreview();
    });
}

void TerminalWorkspace::confirmPaste(quint64 confirmationId)
{
    finishPendingPaste(confirmationId, true);
}

void TerminalWorkspace::cancelPaste(quint64 confirmationId)
{
    finishPendingPaste(confirmationId, false);
}

void TerminalWorkspace::finishPendingPaste(quint64 confirmationId,
                                           bool confirmed)
{
    if (pendingPastes_.isEmpty() || confirmationId == 0
        || confirmationId != activePasteConfirmationId_) {
        return;
    }

    activePasteConfirmationId_ = 0;
    PendingPaste pending = std::move(pendingPastes_.front());
    pendingPastes_.removeFirst();
    for (const PendingPasteTarget &target : std::as_const(pending.targets)) {
        if (TerminalPane *pane = paneForId(target.paneId); pane != nullptr) {
            if (confirmed) {
                pane->confirmPaste(target.requestId);
            } else {
                pane->cancelPaste(target.requestId);
            }
        }
    }
    Q_EMIT unsafePasteConfirmationResolved(confirmationId);
    schedulePendingPastePreview();
}

void TerminalWorkspace::removePendingPastesForPane(PaneHandle handle)
{
    bool removedActiveRequest = false;
    for (qsizetype pendingIndex = pendingPastes_.size();
         pendingIndex-- > 0;) {
        PendingPaste &pending = pendingPastes_[pendingIndex];
        for (qsizetype targetIndex = pending.targets.size();
             targetIndex-- > 0;) {
            const PendingPasteTarget target = pending.targets[targetIndex];
            if (target.paneId != handle.id) {
                continue;
            }
            if (handle.pane != nullptr) {
                handle.pane->cancelPaste(target.requestId);
            }
            pending.targets.removeAt(targetIndex);
        }
        if (pending.targets.isEmpty()) {
            removedActiveRequest = removedActiveRequest || pendingIndex == 0;
            pendingPastes_.removeAt(pendingIndex);
        }
    }
    if (removedActiveRequest) {
        const quint64 confirmationId = activePasteConfirmationId_;
        activePasteConfirmationId_ = 0;
        if (confirmationId != 0) {
            Q_EMIT unsafePasteConfirmationResolved(confirmationId);
        }
        schedulePendingPastePreview();
    }
}

TabListEntry TerminalWorkspace::tabListEntry(const Tab &tab) const
{
    TerminalPane *activePane = paneForId(tab.activePaneId);
    if (activePane == nullptr) {
        activePane = firstPane(tab.root.get());
    }

    std::vector<PaneHandle> panes;
    tab.root->collectPanes(panes);
    const bool running = std::ranges::any_of(
        panes,
        [](const PaneHandle &handle) { return handle.pane->isRunning(); });

    TabListEntry entry;
    entry.id = tab.id;
    entry.activePaneId = tab.activePaneId;
    entry.zoomed = tab.zoomedPaneId.isValid();
    entry.title = activePane != nullptr
        ? activePane->title()
        : QStringLiteral("Terminal");
    entry.currentDirectory = activePane != nullptr
        ? activePane->currentDirectory()
        : QString{};
    entry.running = running;
    return entry;
}

void TerminalWorkspace::updateSplitMembership(Tab &tab)
{
    const bool split = tab.root != nullptr && !tab.root->isLeaf();
    std::vector<PaneHandle> panes;
    if (tab.root != nullptr) {
        tab.root->collectPanes(panes);
    }
    for (const PaneHandle &handle : panes) {
        handle.pane->setSplit(split);
    }
}

void TerminalWorkspace::refreshTab(TabId tabId)
{
    const Tab *tab = tabById(tabId);
    if (tab == nullptr) {
        return;
    }
    tabModel_.replace(tabId, tabListEntry(*tab));
    Q_EMIT tabTitlesChanged();
    if (tabId == currentTabId()) {
        Q_EMIT currentTitleChanged();
    }
}

void TerminalWorkspace::geometryChange(const QRectF &newGeometry,
                                       const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        layoutCurrentTab();
    }
}

void TerminalWorkspace::itemChange(ItemChange change,
                                   const ItemChangeData &value)
{
    if (change == ItemSceneChange) {
        // QQuickItem::ungrabMouse() follows the item's current scene. If this
        // workspace moves between windows while a divider owns the old
        // scene's grab, only destroying that handle reliably releases both
        // delivery agents through Qt's public API. Stable split IDs recreate
        // it below before the base emits windowChanged().
        updateSplitDividers(nullptr);
        if (value.window != nullptr) {
            layoutCurrentTab();
        }
    }
    QQuickItem::itemChange(change, value);
}

void TerminalWorkspace::layoutCurrentTab()
{
    Tab *tab = currentTab();
    if (tab == nullptr || tab->root == nullptr) {
        updateSplitDividers(nullptr);
        return;
    }
    // Keep logical geometry current while zoomed, but don't resize hidden
    // PTYs or briefly resize the zoomed pane twice.
    updateNodeGeometry(tab->root.get(), boundingRect());
    updateTabVisibility(*tab, true);
    if (Node *zoomed = findNode(tab->root.get(), tab->zoomedPaneId);
        zoomed != nullptr && zoomed->isLeaf()) {
        zoomed->pane->setPosition(boundingRect().topLeft());
        zoomed->pane->setSize(boundingRect().size());
    } else {
        applyNodeGeometry(tab->root.get());
    }
    updateSplitDividers(tab);
}

void TerminalWorkspace::updateNodeGeometry(Node *node,
                                           const QRectF &geometry)
{
    if (node == nullptr) {
        return;
    }
    node->geometry = geometry;
    if (node->isLeaf()) {
        return;
    }

    if (node->orientation == Qt::Horizontal) {
        const qreal available = splitExtent(geometry, node->orientation);
        const qreal firstWidth = std::floor(available * node->ratio);
        updateNodeGeometry(
            node->first.get(),
            QRectF(geometry.x(), geometry.y(), firstWidth, geometry.height()));
        updateNodeGeometry(
            node->second.get(),
            QRectF(geometry.x() + firstWidth + splitGap, geometry.y(),
                   available - firstWidth, geometry.height()));
    } else {
        const qreal available = splitExtent(geometry, node->orientation);
        const qreal firstHeight = std::floor(available * node->ratio);
        updateNodeGeometry(
            node->first.get(),
            QRectF(geometry.x(), geometry.y(), geometry.width(), firstHeight));
        updateNodeGeometry(
            node->second.get(),
            QRectF(geometry.x(), geometry.y() + firstHeight + splitGap,
                   geometry.width(), available - firstHeight));
    }
}

void TerminalWorkspace::applyNodeGeometry(Node *node)
{
    if (node == nullptr) {
        return;
    }
    if (node->isLeaf()) {
        node->pane->setPosition(node->geometry.topLeft());
        node->pane->setSize(node->geometry.size());
        return;
    }
    applyNodeGeometry(node->first.get());
    applyNodeGeometry(node->second.get());
}

TerminalWorkspace::Node *TerminalWorkspace::findSplitNode(
    Node *node, quint64 splitId) const
{
    if (node == nullptr || node->isLeaf()) {
        return nullptr;
    }
    if (node->splitId == splitId) {
        return node;
    }
    if (Node *match = findSplitNode(node->first.get(), splitId);
        match != nullptr) {
        return match;
    }
    return findSplitNode(node->second.get(), splitId);
}

void TerminalWorkspace::updateSplitDividers(const Tab *tab)
{
    ++splitDividerGeneration_;
    if (tab != nullptr && tab->root != nullptr
        && !tab->zoomedPaneId.isValid()) {
        updateSplitDividers(tab->root.get(), splitDividerGeneration_);
    }

    for (auto divider = splitDividers_.begin();
         divider != splitDividers_.end();) {
        if (divider.value()->isCurrent(splitDividerGeneration_)) {
            ++divider;
            continue;
        }
        delete divider.value();
        divider = splitDividers_.erase(divider);
    }
}

void TerminalWorkspace::updateSplitDividers(Node *node, quint64 generation)
{
    if (node == nullptr || node->isLeaf()) {
        return;
    }

    const qreal perpendicular = node->orientation == Qt::Horizontal
        ? node->geometry.height() : node->geometry.width();
    if (splitExtent(node->geometry, node->orientation) > 0.0
        && perpendicular > 0.0) {
        SplitDividerItem *divider = splitDividers_.value(node->splitId);
        if (divider == nullptr) {
            divider = new SplitDividerItem(node->splitId, this);
            divider->setColor(effectiveOptions_.splitAppearance.dividerColor);
            splitDividers_.insert(node->splitId, divider);
        }
        divider->markCurrent(generation);
        QRectF geometry;
        if (node->orientation == Qt::Horizontal) {
            geometry = QRectF(
                node->first->geometry.right(), node->geometry.top(),
                splitGap, node->geometry.height());
        } else {
            geometry = QRectF(
                node->geometry.left(), node->first->geometry.bottom(),
                node->geometry.width(), splitGap);
        }
        divider->setOrientation(node->orientation);
        divider->setPosition(geometry.topLeft());
        divider->setSize(geometry.size());
        divider->setVisible(true);
    }

    updateSplitDividers(node->first.get(), generation);
    updateSplitDividers(node->second.get(), generation);
}

std::optional<TerminalWorkspace::SplitDividerDrag>
TerminalWorkspace::beginSplitDividerDrag(
    quint64 splitId, const QPointF &position) const
{
    const Tab *tab = currentTab();
    if (tab == nullptr || tab->root == nullptr
        || tab->zoomedPaneId.isValid()) {
        return std::nullopt;
    }
    const Node *split = findSplitNode(tab->root.get(), splitId);
    if (split == nullptr) {
        return std::nullopt;
    }
    const qreal available = splitExtent(split->geometry, split->orientation);
    if (available <= 0.0) {
        return std::nullopt;
    }
    const qreal pointer = split->orientation == Qt::Horizontal
        ? position.x() : position.y();
    if (!std::isfinite(pointer)) {
        return std::nullopt;
    }
    return SplitDividerDrag{pointer, split->ratio};
}

bool TerminalWorkspace::dragSplitDivider(
    quint64 splitId, const QPointF &position,
    const SplitDividerDrag &drag)
{
    Tab *tab = currentTab();
    if (tab == nullptr || tab->root == nullptr
        || tab->zoomedPaneId.isValid()) {
        return false;
    }
    Node *split = findSplitNode(tab->root.get(), splitId);
    if (split == nullptr) {
        return false;
    }
    const qreal available = splitExtent(split->geometry, split->orientation);
    if (available <= 0.0) {
        return false;
    }
    const qreal pointer = split->orientation == Qt::Horizontal
        ? position.x() : position.y();
    if (!std::isfinite(pointer) || !std::isfinite(drag.pointer)
        || !std::isfinite(drag.ratio)) {
        return false;
    }
    const qreal ratio = std::clamp(
        drag.ratio + (pointer - drag.pointer) / available, 0.0, 1.0);
    if (ratio > 0.0 && ratio < 1.0
        && std::floor(available * ratio)
            == std::floor(available * split->ratio)) {
        return true;
    }
    setSplitRatio(*tab, *split, ratio);
    return true;
}

void TerminalWorkspace::setSplitRatio(Tab &tab, Node &split, qreal ratio)
{
    if (!std::isfinite(ratio)) {
        return;
    }
    ratio = std::clamp(ratio, 0.0, 1.0);
    if (ratio == split.ratio) {
        return;
    }
    split.ratio = ratio;
    if (tab.id == currentTabId()) {
        layoutCurrentTab();
    } else {
        updateNodeGeometry(tab.root.get(), boundingRect());
    }
}

void TerminalWorkspace::updateTabVisibility(Tab &tab, bool visible)
{
    if (!visible) {
        setNodeVisibility(tab.root.get(), false);
        return;
    }

    Node *zoomed = findNode(tab.root.get(), tab.zoomedPaneId);
    if (zoomed == nullptr || !zoomed->isLeaf()) {
        setNodeVisibility(tab.root.get(), true);
        return;
    }
    setNodeVisibility(tab.root.get(), false);
    zoomed->pane->setVisible(true);
}

void TerminalWorkspace::setNodeVisibility(Node *node, bool visible)
{
    if (node == nullptr) {
        return;
    }
    if (node->isLeaf()) {
        node->pane->setVisible(visible);
        return;
    }
    setNodeVisibility(node->first.get(), visible);
    setNodeVisibility(node->second.get(), visible);
}

TerminalWorkspace::Node *TerminalWorkspace::findNode(Node *node,
                                                     PaneId paneId) const
{
    if (node == nullptr) return nullptr;
    if (node->isLeaf()) return node->paneId == paneId ? node : nullptr;
    if (Node *result = findNode(node->first.get(), paneId)) return result;
    return findNode(node->second.get(), paneId);
}

TerminalPane *TerminalWorkspace::firstPane(Node *node) const
{
    if (node == nullptr) return nullptr;
    if (node->isLeaf()) return node->pane;
    if (TerminalPane *pane = firstPane(node->first.get())) return pane;
    return firstPane(node->second.get());
}

PaneId TerminalWorkspace::firstPaneId(Node *node) const
{
    if (node == nullptr) return {};
    if (node->isLeaf()) return node->paneId;
    const PaneId first = firstPaneId(node->first.get());
    return first.isValid() ? first : firstPaneId(node->second.get());
}

TerminalPane *TerminalWorkspace::paneForId(PaneId paneId) const
{
    if (!paneId.isValid()) {
        return nullptr;
    }
    for (const std::unique_ptr<Tab> &tab : tabs_) {
        if (Node *node = findNode(tab->root.get(), paneId); node != nullptr) {
            return node->pane;
        }
    }
    return nullptr;
}

bool TerminalWorkspace::navigateFrom(PaneId paneId, int direction)
{
    Tab *tab = tabById(tabIdForPane(paneId));
    if (tab == nullptr || tab->root == nullptr) return false;

    // Ghostty's spatial tree is normalized to a unit square and excludes the
    // visual divider. This keeps direction choices independent of window
    // aspect ratio and divider thickness.
    std::vector<std::pair<PaneId, QRectF>> slots;
    const auto collectSlots = [&slots](auto &&self, Node *node,
                                       const QRectF &geometry) -> void {
        if (node == nullptr) return;
        if (node->isLeaf()) {
            slots.emplace_back(node->paneId, geometry);
            return;
        }
        if (node->orientation == Qt::Horizontal) {
            const qreal firstWidth = geometry.width() * node->ratio;
            self(self, node->first.get(),
                 QRectF(geometry.x(), geometry.y(), firstWidth,
                        geometry.height()));
            self(self, node->second.get(),
                 QRectF(geometry.x() + firstWidth, geometry.y(),
                        geometry.width() - firstWidth, geometry.height()));
        } else {
            const qreal firstHeight = geometry.height() * node->ratio;
            self(self, node->first.get(),
                 QRectF(geometry.x(), geometry.y(), geometry.width(),
                        firstHeight));
            self(self, node->second.get(),
                 QRectF(geometry.x(), geometry.y() + firstHeight,
                        geometry.width(), geometry.height() - firstHeight));
        }
    };
    collectSlots(collectSlots, tab->root.get(), QRectF(0.0, 0.0, 1.0, 1.0));
    if (slots.size() <= 1) return false;

    const auto sourceSlot = std::ranges::find_if(
        slots,
        [paneId](const auto &slot) { return slot.first == paneId; });
    if (sourceSlot == slots.end()) return false;

    const auto findNearest = [&slots, paneId, direction](
                                 const QRectF &source) -> PaneId {
        PaneId best;
        qreal bestDistance = std::numeric_limits<qreal>::max();
        for (const auto &[candidateId, candidate] : slots) {
            if (candidateId == paneId) continue;
            const bool valid =
                (direction == Qt::Key_Left
                 && candidate.right() <= source.left())
                || (direction == Qt::Key_Right
                    && candidate.left() >= source.right())
                || (direction == Qt::Key_Up
                    && candidate.bottom() <= source.top())
                || (direction == Qt::Key_Down
                    && candidate.top() >= source.bottom());
            if (!valid) continue;

            const qreal dx = candidate.x() - source.x();
            const qreal dy = candidate.y() - source.y();
            const qreal distance = std::sqrt(dx * dx + dy * dy);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = candidateId;
            }
        }
        return best;
    };

    if (direction != Qt::Key_Left && direction != Qt::Key_Right
        && direction != Qt::Key_Up && direction != Qt::Key_Down) {
        return false;
    }

    QRectF source = sourceSlot->second;
    PaneId target = findNearest(source);
    if (!target.isValid()) {
        if (direction == Qt::Key_Left) {
            source.translate(1.0, 0.0);
        } else if (direction == Qt::Key_Right) {
            source.translate(-1.0, 0.0);
        } else if (direction == Qt::Key_Up) {
            source.translate(0.0, 1.0);
        } else {
            source.translate(0.0, -1.0);
        }
        target = findNearest(source);
    }
    if (!target.isValid()) return false;

    if (tab->zoomedPaneId.isValid()) {
        tab->zoomedPaneId = {};
        refreshTab(tab->id);
        if (tab->id == currentTabId()) layoutCurrentTab();
    }
    activatePane(target);
    return true;
}

bool TerminalWorkspace::navigateRelative(PaneId paneId, qint64 delta)
{
    Tab *tab = tabById(tabIdForPane(paneId));
    if (tab == nullptr) return false;
    std::vector<PaneHandle> panes;
    tab->root->collectPanes(panes);
    if (panes.size() <= 1) return false;

    const auto current = std::ranges::find(panes, paneId, &PaneHandle::id);
    if (current == panes.end()) return false;

    const qint64 count = static_cast<qint64>(panes.size());
    const qint64 source = std::ranges::distance(panes.begin(), current);
    const qint64 offset = delta % count;
    const size_t destination = static_cast<size_t>(
        (source + offset + count) % count);
    if (destination == static_cast<size_t>(source)) return false;

    if (tab->zoomedPaneId.isValid()) {
        tab->zoomedPaneId = {};
        refreshTab(tab->id);
        if (tab->id == currentTabId()) layoutCurrentTab();
    }
    activatePane(panes[destination].id);
    return true;
}

bool TerminalWorkspace::findNodePath(Node *node, PaneId paneId,
                                     std::vector<Node *> *path) const
{
    if (node == nullptr || path == nullptr) return false;
    path->push_back(node);
    if (node->isLeaf()) {
        if (node->paneId == paneId) return true;
        path->pop_back();
        return false;
    }
    if (findNodePath(node->first.get(), paneId, path)
        || findNodePath(node->second.get(), paneId, path)) {
        return true;
    }
    path->pop_back();
    return false;
}

bool TerminalWorkspace::resizeSplit(PaneId paneId, int direction, int amount)
{
    Tab *tab = tabById(tabIdForPane(paneId));
    if (tab == nullptr || tab->root == nullptr || tab->root->isLeaf()
        || amount <= 0 || boundingRect().width() <= 0.0
        || boundingRect().height() <= 0.0) {
        return false;
    }

    Qt::Orientation orientation;
    qreal sign;
    if (direction == Qt::Key_Left) {
        orientation = Qt::Horizontal;
        sign = -1.0;
    } else if (direction == Qt::Key_Right) {
        orientation = Qt::Horizontal;
        sign = 1.0;
    } else if (direction == Qt::Key_Up) {
        orientation = Qt::Vertical;
        sign = -1.0;
    } else if (direction == Qt::Key_Down) {
        orientation = Qt::Vertical;
        sign = 1.0;
    } else {
        return false;
    }

    updateNodeGeometry(tab->root.get(), boundingRect());
    std::vector<Node *> path;
    if (!findNodePath(tab->root.get(), paneId, &path)) return false;

    Node *split = nullptr;
    for (Node *node : path | std::views::reverse) {
        if (!node->isLeaf() && node->orientation == orientation) {
            split = node;
            break;
        }
    }
    if (split != nullptr) {
        const qreal extent = splitExtent(split->geometry, orientation);
        if (extent > 0.0) {
            setSplitRatio(
                *tab, *split,
                split->ratio + sign * static_cast<qreal>(amount) / extent);
        }
    }

    // Ghostty reports a resize as performed for any nontrivial tree even
    // when the active leaf has no ancestor split on the requested axis.
    return true;
}

bool TerminalWorkspace::equalizeSplits(TabId tabId)
{
    Tab *tab = tabById(tabId);
    if (tab == nullptr || tab->root == nullptr) return false;
    (void) tab->root->equalize();
    if (tab->id == currentTabId()) {
        layoutCurrentTab();
    } else {
        updateNodeGeometry(tab->root.get(), boundingRect());
    }
    return true;
}

bool TerminalWorkspace::toggleSplitZoom(TabId tabId)
{
    Tab *tab = tabById(tabId);
    if (tab == nullptr || tab->root == nullptr || tab->root->isLeaf()) {
        return false;
    }
    tab->zoomedPaneId = tab->zoomedPaneId.isValid()
        ? PaneId{}
        : tab->activePaneId;
    refreshTab(tab->id);
    if (tab->id == currentTabId()) {
        layoutCurrentTab();
    } else {
        updateTabVisibility(*tab, false);
    }
    return true;
}

bool TerminalWorkspace::shouldConfirmTabClose(const Tab &tab) const
{
    std::vector<PaneHandle> panes;
    tab.root->collectPanes(panes);
    const bool childRunning = std::ranges::any_of(
        panes,
        [](const PaneHandle &handle) { return handle.pane->isRunning(); });
    const bool activeProcess = std::ranges::any_of(
        panes,
        [](const PaneHandle &handle) {
            return handle.pane->hasActiveProcess();
        });
    return shouldConfirmClose(effectiveOptions_.confirmCloseMode,
                              childRunning, activeProcess);
}

bool TerminalWorkspace::shouldConfirmWorkspaceClose() const
{
    for (const std::unique_ptr<Tab> &tab : tabs_) {
        if (shouldConfirmTabClose(*tab)) {
            return true;
        }
    }
    return false;
}

int TerminalWorkspace::tabIndexForId(TabId tabId) const
{
    for (int index = 0; index < static_cast<int>(tabs_.size()); ++index) {
        if (tabs_[static_cast<size_t>(index)]->id == tabId) {
            return index;
        }
    }
    return -1;
}

int TerminalWorkspace::tabIndexForPane(PaneId paneId) const
{
    for (int index = 0; index < static_cast<int>(tabs_.size()); ++index) {
        if (findNode(tabs_[static_cast<size_t>(index)]->root.get(), paneId) != nullptr) {
            return index;
        }
    }
    return -1;
}

TabId TerminalWorkspace::tabIdForPane(PaneId paneId) const
{
    const int index = tabIndexForPane(paneId);
    return index >= 0 ? tabs_[static_cast<size_t>(index)]->id : TabId{};
}
