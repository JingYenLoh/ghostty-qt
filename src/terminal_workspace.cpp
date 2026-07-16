#include "terminal_workspace.h"

#include "terminal_pane.h"

#include <QKeyEvent>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

struct TerminalWorkspace::Node {
    explicit Node(TerminalPane *terminalPane)
        : pane(terminalPane)
    {
    }

    bool isLeaf() const { return pane != nullptr; }

    TerminalPane *pane = nullptr;
    Qt::Orientation orientation = Qt::Horizontal;
    qreal ratio = 0.5;
    QRectF geometry;
    std::unique_ptr<Node> first;
    std::unique_ptr<Node> second;
};

struct TerminalWorkspace::Tab {
    std::unique_ptr<Node> root;
    QPointer<TerminalPane> activePane;
};

LaunchOptions TerminalWorkspace::defaultOptions_;

TerminalWorkspace::TerminalWorkspace(QQuickItem *parent)
    : QQuickItem(parent)
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

QStringList TerminalWorkspace::tabTitles() const
{
    QStringList result;
    result.reserve(static_cast<qsizetype>(tabs_.size()));
    for (const std::unique_ptr<Tab> &tab : tabs_) {
        TerminalPane *pane = tab->activePane;
        if (pane == nullptr) {
            pane = firstPane(tab->root.get());
        }
        result.append(pane != nullptr ? pane->title() : QStringLiteral("Terminal"));
    }
    return result;
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

TerminalPane *TerminalWorkspace::createPane(const LaunchOptions &options)
{
    auto *pane = new TerminalPane(options, this);
    pane->setVisible(false);
    connect(pane, &TerminalPane::activated, this,
            [this](TerminalPane *activatedPane) { setActivePane(activatedPane); });
    connect(pane, &TerminalPane::titleChanged, this,
            [this] { Q_EMIT tabTitlesChanged(); });
    connect(pane, &TerminalPane::currentDirectoryChanged, this,
            [this] { Q_EMIT tabTitlesChanged(); });
    connect(pane, &TerminalPane::requestNewTab, this, &TerminalWorkspace::newTab);
    connect(pane, &TerminalPane::requestSplit, this,
            [this](int orientation) {
                splitActive(static_cast<Qt::Orientation>(orientation));
            });
    connect(pane, &TerminalPane::requestClose, this,
            [this, pane] { closePane(pane); });
    connect(pane, &TerminalPane::requestNavigate, this,
            [this, pane](int direction) { navigateFrom(pane, direction); });
    connect(pane, &TerminalPane::requestTabChange, this,
            &TerminalWorkspace::changeTabRelative);
    connect(pane, &TerminalPane::requestQuit, this, &TerminalWorkspace::requestQuit);
    connect(pane, &TerminalPane::unsafePasteRequested, this,
            &TerminalWorkspace::beginUnsafePaste);
    return pane;
}

void TerminalWorkspace::newTab()
{
    LaunchOptions options = defaultOptions_;
    if (initialTabCreated_) {
        options.program.clear();
        options.hold = false;
    }
    initialTabCreated_ = true;

    TerminalPane *pane = createPane(options);
    auto tab = std::make_unique<Tab>();
    tab->root = std::make_unique<Node>(pane);
    tab->activePane = pane;
    tabs_.push_back(std::move(tab));

    Q_EMIT tabTitlesChanged();
    setCurrentIndex(static_cast<int>(tabs_.size()) - 1);
}

void TerminalWorkspace::setCurrentIndex(int index)
{
    if (tabs_.empty()) {
        currentIndex_ = -1;
        return;
    }
    index = std::clamp(index, 0, static_cast<int>(tabs_.size()) - 1);
    if (currentIndex_ == index) {
        if (Tab *tab = currentTab(); tab != nullptr && tab->activePane != nullptr) {
            tab->activePane->focusTerminal();
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
    if (Tab *tab = currentTab(); tab != nullptr && tab->activePane != nullptr) {
        tab->activePane->focusTerminal();
    }
}

void TerminalWorkspace::changeTabRelative(int delta)
{
    if (tabs_.empty()) {
        return;
    }
    const int count = static_cast<int>(tabs_.size());
    const int target = (currentIndex_ + delta % count + count) % count;
    setCurrentIndex(target);
}

void TerminalWorkspace::splitRight()
{
    splitActive(Qt::Horizontal);
}

void TerminalWorkspace::splitDown()
{
    splitActive(Qt::Vertical);
}

void TerminalWorkspace::splitActive(Qt::Orientation orientation)
{
    Tab *tab = currentTab();
    if (tab == nullptr || tab->activePane == nullptr) {
        return;
    }
    Node *node = findNode(tab->root.get(), tab->activePane);
    if (node == nullptr || !node->isLeaf()) {
        return;
    }

    TerminalPane *oldPane = node->pane;
    TerminalPane *newPane = createPane(oldPane->splitLaunchOptions());
    node->pane = nullptr;
    node->orientation = orientation;
    node->ratio = 0.5;
    node->first = std::make_unique<Node>(oldPane);
    node->second = std::make_unique<Node>(newPane);
    tab->activePane = newPane;
    newPane->setVisible(true);
    layoutCurrentTab();
    newPane->focusTerminal();
    Q_EMIT tabTitlesChanged();
}

void TerminalWorkspace::setActivePane(TerminalPane *pane)
{
    const int tabIndex = tabIndexForPane(pane);
    if (tabIndex < 0) {
        return;
    }
    if (tabIndex != currentIndex_) {
        setCurrentIndex(tabIndex);
    }
    Tab *tab = currentTab();
    if (tab != nullptr && tab->activePane != pane) {
        tab->activePane = pane;
        Q_EMIT tabTitlesChanged();
    }
}

void TerminalWorkspace::closeActivePane()
{
    if (Tab *tab = currentTab(); tab != nullptr) {
        closePane(tab->activePane);
    }
}

void TerminalWorkspace::closePane(TerminalPane *pane, bool force)
{
    if (pane == nullptr) {
        return;
    }
    const int tabIndex = tabIndexForPane(pane);
    if (tabIndex < 0) {
        return;
    }
    if (!force && pane->isRunning()) {
        pendingClose_ = PendingClose::Pane;
        pendingPane_ = pane;
        Q_EMIT closeConfirmationRequested(
            QStringLiteral("A process is still running in this pane. Close it?"));
        return;
    }

    Tab *tab = tabs_[static_cast<size_t>(tabIndex)].get();
    removePaneFromNode(tab->root, pane);
    if (tab->root == nullptr) {
        removeTab(tabIndex);
        return;
    }
    tab->activePane = firstPane(tab->root.get());
    if (tabIndex == currentIndex_) {
        layoutCurrentTab();
        if (tab->activePane != nullptr) tab->activePane->focusTerminal();
    }
    Q_EMIT tabTitlesChanged();
}

bool TerminalWorkspace::removePaneFromNode(std::unique_ptr<Node> &node,
                                           TerminalPane *pane)
{
    if (node == nullptr) {
        return false;
    }
    if (node->isLeaf()) {
        if (node->pane != pane) {
            return false;
        }
        pane->setVisible(false);
        pane->deleteLater();
        node.reset();
        return true;
    }

    const bool removed = removePaneFromNode(node->first, pane)
        || removePaneFromNode(node->second, pane);
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
    const Tab *tab = currentTab();
    if (tab == nullptr) {
        return;
    }
    std::vector<TerminalPane *> panes;
    collectPanes(tab->root.get(), &panes);
    const bool running = std::any_of(panes.cbegin(), panes.cend(),
                                     [](TerminalPane *pane) { return pane->isRunning(); });
    if (running) {
        pendingClose_ = PendingClose::Tab;
        pendingTabIndex_ = currentIndex_;
        Q_EMIT closeConfirmationRequested(
            QStringLiteral("Processes are still running in this tab. Close the tab?"));
        return;
    }
    removeTab(currentIndex_);
}

void TerminalWorkspace::removeTab(int index)
{
    if (index < 0 || index >= static_cast<int>(tabs_.size())) {
        return;
    }
    std::vector<TerminalPane *> panes;
    collectPanes(tabs_[static_cast<size_t>(index)]->root.get(), &panes);
    for (TerminalPane *pane : panes) {
        pane->setVisible(false);
        pane->deleteLater();
    }
    tabs_.erase(tabs_.begin() + index);

    if (tabs_.empty()) {
        currentIndex_ = -1;
        Q_EMIT tabTitlesChanged();
        Q_EMIT currentIndexChanged();
        Q_EMIT quitApproved();
        return;
    }

    const int nextIndex = std::min(index, static_cast<int>(tabs_.size()) - 1);
    currentIndex_ = -1;
    Q_EMIT tabTitlesChanged();
    setCurrentIndex(nextIndex);
}

void TerminalWorkspace::requestQuit()
{
    if (pendingClose_ == PendingClose::Quit) {
        return;
    }
    if (hasRunningProcesses()) {
        pendingClose_ = PendingClose::Quit;
        pendingPane_.clear();
        pendingTabIndex_ = -1;
        Q_EMIT closeConfirmationRequested(
            QStringLiteral("Terminal processes are still running. Quit and terminate them?"));
        return;
    }
    Q_EMIT quitApproved();
}

void TerminalWorkspace::confirmClose()
{
    const PendingClose action = pendingClose_;
    QPointer<TerminalPane> pane = pendingPane_;
    const int tabIndex = pendingTabIndex_;
    pendingClose_ = PendingClose::None;
    pendingPane_.clear();
    pendingTabIndex_ = -1;

    if (action == PendingClose::Pane && pane != nullptr) {
        closePane(pane, true);
    } else if (action == PendingClose::Tab) {
        removeTab(tabIndex);
    } else if (action == PendingClose::Quit) {
        Q_EMIT quitApproved();
    }
}

void TerminalWorkspace::cancelClose()
{
    pendingClose_ = PendingClose::None;
    pendingPane_.clear();
    pendingTabIndex_ = -1;
}

void TerminalWorkspace::beginUnsafePaste(const QString &text, TerminalPane *pane)
{
    pendingPaste_ = text;
    pendingPastePane_ = pane;
    QString preview = text.left(240);
    preview.replace(QLatin1Char('\n'), QStringLiteral("↵\n"));
    if (text.size() > preview.size()) {
        preview.append(QStringLiteral("…"));
    }
    Q_EMIT unsafePasteConfirmationRequested(preview);
}

void TerminalWorkspace::confirmPaste()
{
    if (pendingPastePane_ != nullptr) {
        pendingPastePane_->pasteText(pendingPaste_);
    }
    cancelPaste();
}

void TerminalWorkspace::cancelPaste()
{
    pendingPaste_.clear();
    pendingPastePane_.clear();
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
                                                     TerminalPane *pane) const
{
    if (node == nullptr) return nullptr;
    if (node->isLeaf()) return node->pane == pane ? node : nullptr;
    if (Node *result = findNode(node->first.get(), pane)) return result;
    return findNode(node->second.get(), pane);
}

TerminalPane *TerminalWorkspace::firstPane(Node *node) const
{
    if (node == nullptr) return nullptr;
    if (node->isLeaf()) return node->pane;
    if (TerminalPane *pane = firstPane(node->first.get())) return pane;
    return firstPane(node->second.get());
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

void TerminalWorkspace::navigateFrom(TerminalPane *pane, int direction)
{
    const Tab *tab = currentTab();
    if (tab == nullptr || pane == nullptr) return;
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
        setActivePane(best);
        best->focusTerminal();
    }
}

bool TerminalWorkspace::hasRunningProcesses() const
{
    for (const std::unique_ptr<Tab> &tab : tabs_) {
        std::vector<TerminalPane *> panes;
        collectPanes(tab->root.get(), &panes);
        if (std::any_of(panes.cbegin(), panes.cend(),
                        [](TerminalPane *pane) { return pane->isRunning(); })) {
            return true;
        }
    }
    return false;
}

int TerminalWorkspace::tabIndexForPane(TerminalPane *pane) const
{
    for (int index = 0; index < static_cast<int>(tabs_.size()); ++index) {
        if (findNode(tabs_[static_cast<size_t>(index)]->root.get(), pane) != nullptr) {
            return index;
        }
    }
    return -1;
}
