#include "terminal_workspace.h"

#include "terminal_pane.h"

#include <QKeyEvent>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

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
        return navigateFrom(request.context.paneId, request.context.value);
    case WorkspaceAction::ChangeTabRelative:
        if (request.context.tabId.isValid()
            && tabById(request.context.tabId) == nullptr) return false;
        return changeTabRelativeImpl(request.context.value,
                                     request.context.tabId);
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
    connect(pane, &TerminalPane::unsafePasteRequested, this,
            [this, paneId](const QString &text, TerminalPane *) {
                beginUnsafePaste(text, paneId);
            });
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
        setNodeVisibility(tabs_[static_cast<size_t>(currentIndex_)]->root.get(), false);
    }
    currentIndex_ = index;
    setNodeVisibility(tabs_[static_cast<size_t>(currentIndex_)]->root.get(), true);
    layoutCurrentTab();
    Q_EMIT currentIndexChanged();
    Q_EMIT currentTitleChanged();
    if (TerminalPane *pane = paneForId(currentPaneId()); pane != nullptr) {
        pane->focusTerminal();
    }
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
    const PaneHandle oldHandle{node->paneId, oldPane};
    node->paneId = {};
    node->pane = nullptr;
    node->orientation = orientation;
    node->ratio = 0.5;
    node->first = std::make_unique<Node>(oldHandle);
    node->second = std::make_unique<Node>(newPane);
    tab->activePaneId = newPane.id;
    const bool targetIsCurrent = tabId == currentTabId();
    newPane.pane->setVisible(targetIsCurrent);
    if (targetIsCurrent) {
        layoutCurrentTab();
        newPane.pane->focusTerminal();
    }
    refreshTab(tabId);
}

void TerminalWorkspace::activatePane(PaneId paneId)
{
    const int tabIndex = tabIndexForPane(paneId);
    if (tabIndex < 0) {
        return;
    }
    if (tabIndex != currentIndex_) {
        activateTab(tabs_[static_cast<size_t>(tabIndex)]->id);
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
    pendingPaste_ = text;
    pendingPastePaneId_ = paneId;
    QString preview = text.left(240);
    preview.replace(QLatin1Char('\n'), QStringLiteral("↵\n"));
    if (text.size() > preview.size()) {
        preview.append(QStringLiteral("…"));
    }
    Q_EMIT unsafePasteConfirmationRequested(preview);
}

void TerminalWorkspace::confirmPaste()
{
    if (TerminalPane *pane = paneForId(pendingPastePaneId_); pane != nullptr) {
        pane->pasteText(pendingPaste_);
    }
    cancelPaste();
}

void TerminalWorkspace::cancelPaste()
{
    pendingPaste_.clear();
    pendingPastePaneId_ = {};
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
    layoutNode(tab->root.get(), boundingRect());
}

void TerminalWorkspace::layoutNode(Node *node, const QRectF &geometry)
{
    if (node == nullptr) {
        return;
    }
    node->geometry = geometry;
    if (node->isLeaf()) {
        node->pane->setPosition(geometry.topLeft());
        node->pane->setSize(geometry.size());
        return;
    }

    constexpr qreal gap = 2.0;
    if (node->orientation == Qt::Horizontal) {
        const qreal available = std::max(0.0, geometry.width() - gap);
        const qreal firstWidth = std::floor(available * node->ratio);
        layoutNode(node->first.get(),
                   QRectF(geometry.x(), geometry.y(), firstWidth, geometry.height()));
        layoutNode(node->second.get(),
                   QRectF(geometry.x() + firstWidth + gap, geometry.y(),
                          available - firstWidth, geometry.height()));
    } else {
        const qreal available = std::max(0.0, geometry.height() - gap);
        const qreal firstHeight = std::floor(available * node->ratio);
        layoutNode(node->first.get(),
                   QRectF(geometry.x(), geometry.y(), geometry.width(), firstHeight));
        layoutNode(node->second.get(),
                   QRectF(geometry.x(), geometry.y() + firstHeight + gap,
                          geometry.width(), available - firstHeight));
    }
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
    TerminalPane *pane = paneForId(paneId);
    const Tab *tab = tabById(tabIdForPane(paneId));
    if (tab == nullptr || pane == nullptr) return false;
    std::vector<TerminalPane *> panes;
    collectPanes(tab->root.get(), &panes);
    const QPointF source = pane->boundingRect().center() + pane->position();
    TerminalPane *best = nullptr;
    qreal bestScore = std::numeric_limits<qreal>::max();

    for (TerminalPane *candidate : panes) {
        if (candidate == pane) continue;
        const QPointF target = candidate->boundingRect().center() + candidate->position();
        const qreal dx = target.x() - source.x();
        const qreal dy = target.y() - source.y();
        qreal primary = 0.0;
        qreal secondary = 0.0;
        bool valid = false;
        if (direction == Qt::Key_Left && dx < 0.0) {
            primary = -dx; secondary = std::abs(dy); valid = true;
        } else if (direction == Qt::Key_Right && dx > 0.0) {
            primary = dx; secondary = std::abs(dy); valid = true;
        } else if (direction == Qt::Key_Up && dy < 0.0) {
            primary = -dy; secondary = std::abs(dx); valid = true;
        } else if (direction == Qt::Key_Down && dy > 0.0) {
            primary = dy; secondary = std::abs(dx); valid = true;
        }
        const qreal score = primary * 1000.0 + secondary;
        if (valid && score < bestScore) {
            bestScore = score;
            best = candidate;
        }
    }
    if (best != nullptr) {
        activatePane(paneIdForPane(best));
        return true;
    }
    return false;
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
