#include "terminal_workspace.h"

#include "ghostty_action_catalog.h"
#include "terminal_pane.h"

#include <QDebug>
#include <QKeyEvent>
#include <QPointer>
#include <QQmlComponent>
#include <QScopedValueRollback>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

constexpr qreal splitGap = 2.0;

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

    PaneId paneId;
    TerminalPane *pane = nullptr;
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
            std::vector<TerminalPane *> panes;
            collectPanes(tab->root.get(), &panes);
            for (TerminalPane *pane : panes) {
                createSearchOverlay(pane);
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
    effectiveOptions_ = applyGhosttyConfigSnapshot(defaultOptions_, snapshot);
    for (const std::unique_ptr<Tab> &tab : tabs_) {
        std::vector<TerminalPane *> panes;
        collectPanes(tab->root.get(), &panes);
        for (TerminalPane *pane : panes) {
            pane->applyRuntimeOptions(effectiveOptions_);
        }
    }
    reevaluatePendingClose();
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
        std::vector<TerminalPane *> rawPanes;
        collectPanes(tab->root.get(), &rawPanes);
        panes.reserve(panes.size() + rawPanes.size());
        for (TerminalPane *pane : rawPanes) {
            panes.emplace_back(pane);
        }
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
    case WorkspaceAction::NewTab:
        createNewTab();
        return true;
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
    case WorkspaceAction::SplitRight:
    case WorkspaceAction::SplitDown:
        if (paneForId(request.context.paneId) == nullptr
            || !contextMatchesPane()) return false;
        splitPane(request.context.paneId,
                  request.action == WorkspaceAction::SplitRight
                      ? Qt::Horizontal
                      : Qt::Vertical);
        return true;
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
                case WorkspaceAction::SplitRight:
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
            [this, paneId](TerminalPane *, int, int) {
                refreshTab(tabIdForPane(paneId));
            });
    connect(pane, &TerminalPane::processStateChanged, this,
            [this, paneId] {
                refreshTab(tabIdForPane(paneId));
                reevaluatePendingClose();
            });
    connect(pane, &TerminalPane::requestNewTab, this,
            [this] { dispatchAction({WorkspaceAction::NewTab, {}}); });
    connect(pane, &TerminalPane::requestSplit, this,
            [this, paneId](int orientation) {
                dispatchAction({orientation == Qt::Horizontal
                                    ? WorkspaceAction::SplitRight
                                    : WorkspaceAction::SplitDown,
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
            [this, paneId](const QString &text, TerminalPane *) {
                beginUnsafePaste(text, paneId);
            });
    createSearchOverlay(pane);
    return {paneId, pane};
}

void TerminalWorkspace::newTab()
{
    dispatchAction({WorkspaceAction::NewTab, {}});
}

void TerminalWorkspace::createNewTab()
{
    LaunchOptions options = effectiveOptions_;
    if (initialTabCreated_) {
        options.program.clear();
        options.hold = false;
    }
    initialTabCreated_ = true;

    const PaneHandle pane = createPane(options);
    auto tab = std::make_unique<Tab>();
    tab->id = TabId(nextTabId_++);
    tab->root = std::make_unique<Node>(pane);
    tab->activePaneId = pane.id;
    const TabId tabId = tab->id;
    tabs_.push_back(std::move(tab));

    tabModel_.append(tabListEntry(*tabs_.back()));
    Q_EMIT tabTitlesChanged();
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

void TerminalWorkspace::splitPane(PaneId paneId, Qt::Orientation orientation)
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
    const PaneHandle newPane = createPane(oldPane->splitLaunchOptions());
    // Ghostty resets split zoom whenever the tree structure is split.
    tab->zoomedPaneId = {};
    const PaneHandle oldHandle{node->paneId, oldPane};
    node->paneId = {};
    node->pane = nullptr;
    node->orientation = orientation;
    node->ratio = 0.5;
    node->first = std::make_unique<Node>(oldHandle);
    node->second = std::make_unique<Node>(newPane);
    tab->activePaneId = newPane.id;
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
    resolvePendingPaneRemoval(paneId);
    removePaneFromNode(tab->root, paneId);
    if (tab->root == nullptr) {
        removeTab(tabId);
        return;
    }
    if (removedActivePane || paneForId(tab->activePaneId) == nullptr) {
        tab->activePaneId = firstPaneId(tab->root.get());
    }
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
    const bool removedCurrentTab = index == currentIndex_;
    std::vector<TerminalPane *> panes;
    collectPanes(tabs_[static_cast<size_t>(index)]->root.get(), &panes);
    for (TerminalPane *pane : panes) {
        pane->beginShutdown();
        pane->setVisible(false);
        pane->deleteLater();
    }
    tabs_.erase(tabs_.begin() + index);
    tabModel_.remove(tabId);
    Q_EMIT tabTitlesChanged();
    resolvePendingTabRemoval(tabId);

    if (tabs_.empty()) {
        currentIndex_ = -1;
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

void TerminalWorkspace::resolvePendingPaneRemoval(PaneId paneId)
{
    if (pendingClose_ != PendingClose::Pane || pendingPaneId_ != paneId) {
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
        std::vector<TerminalPane *> panes;
        collectPanes(tab->root.get(), &panes);
        for (TerminalPane *pane : panes) {
            pane->beginShutdown();
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

void TerminalWorkspace::beginUnsafePaste(const QString &text, PaneId paneId)
{
    // A broad paste can reach multiple surfaces during one action fanout. Use
    // one confirmation for the identical clipboard payload and retain every
    // target instead of repeatedly overwriting a single pending pane.
    if (!pendingPaste_.isEmpty() && pendingPaste_ == text) {
        if (!pendingPastePaneIds_.contains(paneId)) {
            pendingPastePaneIds_.append(paneId);
        }
        return;
    }
    pendingPaste_ = text;
    pendingPastePaneIds_ = {paneId};
    QString preview = text.left(240);
    preview.replace(QLatin1Char('\n'), QStringLiteral("↵\n"));
    if (text.size() > preview.size()) {
        preview.append(QStringLiteral("…"));
    }
    Q_EMIT unsafePasteConfirmationRequested(preview);
}

void TerminalWorkspace::confirmPaste()
{
    const QString text = pendingPaste_;
    const QVector<PaneId> paneIds = pendingPastePaneIds_;
    for (PaneId paneId : paneIds) {
        if (TerminalPane *pane = paneForId(paneId); pane != nullptr) {
            pane->pasteText(text);
        }
    }
    cancelPaste();
}

void TerminalWorkspace::cancelPaste()
{
    pendingPaste_.clear();
    pendingPastePaneIds_.clear();
}

TabListEntry TerminalWorkspace::tabListEntry(const Tab &tab) const
{
    TerminalPane *activePane = paneForId(tab.activePaneId);
    if (activePane == nullptr) {
        activePane = firstPane(tab.root.get());
    }

    std::vector<TerminalPane *> panes;
    collectPanes(tab.root.get(), &panes);
    const bool running = std::any_of(panes.cbegin(), panes.cend(),
                                     [](TerminalPane *pane) {
                                         return pane->isRunning();
                                     });

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

void TerminalWorkspace::layoutCurrentTab()
{
    Tab *tab = currentTab();
    if (tab == nullptr || tab->root == nullptr) {
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

void TerminalWorkspace::collectPanes(Node *node,
                                     std::vector<TerminalPane *> *panes) const
{
    if (node == nullptr || panes == nullptr) return;
    if (node->isLeaf()) {
        panes->push_back(node->pane);
        return;
    }
    collectPanes(node->first.get(), panes);
    collectPanes(node->second.get(), panes);
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

    const auto sourceSlot = std::find_if(
        slots.cbegin(), slots.cend(),
        [paneId](const auto &slot) { return slot.first == paneId; });
    if (sourceSlot == slots.cend()) return false;

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
    std::vector<TerminalPane *> panes;
    collectPanes(tab->root.get(), &panes);
    if (panes.size() <= 1) return false;

    const auto current = std::find_if(
        panes.cbegin(), panes.cend(),
        [this, paneId](TerminalPane *pane) {
            return paneIdForPane(pane) == paneId;
        });
    if (current == panes.cend()) return false;

    const qint64 count = static_cast<qint64>(panes.size());
    const qint64 source = std::distance(panes.cbegin(), current);
    const qint64 offset = delta % count;
    const size_t destination = static_cast<size_t>(
        (source + offset + count) % count);
    if (destination == static_cast<size_t>(source)) return false;

    if (tab->zoomedPaneId.isValid()) {
        tab->zoomedPaneId = {};
        refreshTab(tab->id);
        if (tab->id == currentTabId()) layoutCurrentTab();
    }
    activatePane(paneIdForPane(panes[destination]));
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
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        if (!(*it)->isLeaf() && (*it)->orientation == orientation) {
            split = *it;
            break;
        }
    }
    if (split != nullptr) {
        const qreal extent = splitExtent(split->geometry, orientation);
        if (extent > 0.0) {
            split->ratio = std::clamp(
                split->ratio + sign * static_cast<qreal>(amount) / extent,
                0.0,
                1.0);
        }
        if (tab->id == currentTabId()) {
            layoutCurrentTab();
        } else {
            updateNodeGeometry(tab->root.get(), boundingRect());
        }
    }

    // Ghostty reports a resize as performed for any nontrivial tree even
    // when the active leaf has no ancestor split on the requested axis.
    return true;
}

int TerminalWorkspace::equalizeWeight(const Node *node,
                                      Qt::Orientation orientation)
{
    if (node == nullptr || node->isLeaf()
        || node->orientation != orientation) {
        return 1;
    }
    return equalizeWeight(node->first.get(), orientation)
        + equalizeWeight(node->second.get(), orientation);
}

void TerminalWorkspace::equalizeNode(Node *node)
{
    if (node == nullptr || node->isLeaf()) return;
    const int firstWeight = equalizeWeight(node->first.get(),
                                           node->orientation);
    const int secondWeight = equalizeWeight(node->second.get(),
                                            node->orientation);
    node->ratio = static_cast<qreal>(firstWeight)
        / static_cast<qreal>(firstWeight + secondWeight);
    equalizeNode(node->first.get());
    equalizeNode(node->second.get());
}

bool TerminalWorkspace::equalizeSplits(TabId tabId)
{
    Tab *tab = tabById(tabId);
    if (tab == nullptr || tab->root == nullptr) return false;
    equalizeNode(tab->root.get());
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
    std::vector<TerminalPane *> panes;
    collectPanes(tab.root.get(), &panes);
    const bool childRunning = std::any_of(
        panes.cbegin(), panes.cend(),
        [](TerminalPane *pane) { return pane->isRunning(); });
    const bool activeProcess = std::any_of(
        panes.cbegin(), panes.cend(),
        [](TerminalPane *pane) { return pane->hasActiveProcess(); });
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

PaneId TerminalWorkspace::paneIdForPane(TerminalPane *pane) const
{
    if (pane == nullptr) {
        return {};
    }
    const auto findPaneId = [pane](auto &&self, Node *node) -> PaneId {
        if (node == nullptr) return {};
        if (node->isLeaf()) return node->pane == pane ? node->paneId : PaneId{};
        const PaneId first = self(self, node->first.get());
        return first.isValid() ? first : self(self, node->second.get());
    };
    for (const std::unique_ptr<Tab> &tab : tabs_) {
        const PaneId paneId = findPaneId(findPaneId, tab->root.get());
        if (paneId.isValid()) {
            return paneId;
        }
    }
    return {};
}
