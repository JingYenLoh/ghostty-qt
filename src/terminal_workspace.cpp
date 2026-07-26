#include "terminal_workspace.h"

#include "ghostty_action_catalog.h"
#include "terminal_cell_metrics.h"
#include "terminal_geometry.h"
#include "terminal_pane.h"

#include <QCursor>
#include <QDebug>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QSGSimpleRectNode>
#include <QScopeGuard>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <limits>
#include <ranges>
#include <type_traits>
#include <utility>

namespace {

constexpr qreal splitGap = 2.0;
constexpr qreal splitDividerZ = 1.0;
constexpr auto kSearchOverlayProperty = "_ghosttyQtSearchOverlay";
constexpr auto kReadOnlyOverlayProperty = "_ghosttyQtReadOnlyOverlay";
constexpr auto kResizeOverlayProperty = "_ghosttyQtResizeOverlay";
constexpr auto kScrollbarProperty = "_ghosttyQtScrollbar";
constexpr auto kBellBorderProperty = "_ghosttyQtBellBorder";

quint64 nextNonzeroId(quint64 &counter) noexcept
{
    do {
        ++counter;
    } while (counter == 0);
    return counter;
}

struct AxisWeights {
    [[nodiscard]] std::size_t along(Qt::Orientation orientation) const noexcept
    {
        return orientation == Qt::Horizontal ? horizontal : vertical;
    }

    std::size_t horizontal = 1;
    std::size_t vertical = 1;
};

qreal splitExtent(const QRectF &geometry, Qt::Orientation orientation)
{
    const qreal extent =
        orientation == Qt::Horizontal ? geometry.width() : geometry.height();
    return std::max(0.0, extent - splitGap);
}

} // namespace

struct TerminalWorkspace::PaneHandle {
    PaneId id;
    QPointer<TerminalPane> pane;

    [[nodiscard]] bool isValid() const noexcept
    {
        return id.isValid() && pane != nullptr;
    }
};

struct TerminalWorkspace::Node {
    explicit Node(PaneHandle handle)
        : paneId(handle.id)
        , pane(handle.pane.data())
    {}

    bool isLeaf() const { return pane != nullptr; }

    void collectPanes(std::vector<PaneHandle> &panes) const
    {
        forEachPane(
            [&panes](const PaneHandle &handle) { panes.push_back(handle); });
    }

    template <typename Visitor> void forEachPane(Visitor &&visitor) const
    {
        if (isLeaf()) {
            visitor(PaneHandle{paneId, pane});
            return;
        }
        if (first != nullptr) first->forEachPane(visitor);
        if (second != nullptr) second->forEachPane(visitor);
    }

    [[nodiscard]] AxisWeights equalize()
    {
        if (isLeaf()) return {};

        const AxisWeights firstWeights =
            first != nullptr ? first->equalize() : AxisWeights{};
        const AxisWeights secondWeights =
            second != nullptr ? second->equalize() : AxisWeights{};
        const std::size_t firstWeight = firstWeights.along(orientation);
        const std::size_t secondWeight = secondWeights.along(orientation);
        const std::size_t combinedWeight = firstWeight + secondWeight;
        ratio = static_cast<qreal>(firstWeight)
            / static_cast<qreal>(combinedWeight);

        return orientation == Qt::Horizontal ? AxisWeights{combinedWeight, 1}
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

namespace {

void clearPaneOverlay(QPointer<TerminalPane> pane, const char *property)
{
    if (pane == nullptr) return;
    const QPointer<QObject> overlay(
        pane->property(property).value<QObject *>());
    pane->setProperty(property, {});
    delete overlay.data();
}

[[nodiscard]] bool attachPaneOverlay(QPointer<QQmlComponent> component,
                                     QPointer<TerminalPane> pane,
                                     const char *property,
                                     const char *description)
{
    if (pane == nullptr || component == nullptr
        || pane->property(property).value<QObject *>() != nullptr) {
        return pane != nullptr;
    }

    QObject *const overlay = component->createWithInitialProperties({
        {QStringLiteral("terminalPane"),
         QVariant::fromValue(static_cast<QObject *>(pane))},
    });
    if (pane == nullptr) {
        delete overlay;
        return false;
    }
    if (overlay == nullptr) {
        if (component != nullptr) {
            qWarning().noquote() << "Could not create" << description << ':'
                                 << component->errorString();
        }
        return true;
    }

    const QPointer<QObject> overlayGuard(overlay);
    auto *overlayItem = qobject_cast<QQuickItem *>(overlay);
    if (overlayItem == nullptr) {
        qWarning().noquote()
            << description << "component did not create a QQuickItem";
        delete overlay;
        return pane != nullptr;
    }

    overlay->setParent(pane.data());
    if (pane == nullptr || overlayGuard == nullptr) {
        return pane != nullptr;
    }
    overlayItem = qobject_cast<QQuickItem *>(overlayGuard.data());
    if (overlayItem == nullptr) return true;
    overlayItem->setParentItem(pane.data());
    if (pane == nullptr || overlayGuard == nullptr) {
        return pane != nullptr;
    }
    pane->setProperty(property, QVariant::fromValue(overlayGuard.data()));
    return pane != nullptr;
}

} // namespace

struct TerminalWorkspace::Tab {
    TabId id;
    std::unique_ptr<Node> root;
    PaneId activePaneId;
    PaneId zoomedPaneId;
    QString titleOverride;
    bool attention = false;
};

template <typename Visitor>
void TerminalWorkspace::forEachPane(Visitor &&visitor) const
{
    for (const std::unique_ptr<Tab> &tab : tabs_) {
        if (tab->root != nullptr) {
            tab->root->forEachPane(visitor);
        }
    }
}

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
        setCursor(orientation == Qt::Horizontal ? Qt::SplitHCursor
                                                : Qt::SplitVCursor);
    }

    void markCurrent(quint64 generation) { generation_ = generation; }

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

    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override
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
                splitId_, mapToItem(workspace_, event->position()), drag_)) {
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
            (void)workspace_->dragSplitDivider(
                splitId_, mapToItem(workspace_, event->position()), drag_);
        }
        clearDragging();
        event->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        mousePressEvent(event);
    }

    void mouseUngrabEvent() override { clearDragging(); }

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
{
    setClip(true);
    QTimer::singleShot(0, this, [this] {
        if (!initialized_) (void)initialize(effectiveOptions_);
    });
}

TerminalWorkspace::~TerminalWorkspace() = default;

void TerminalWorkspace::setSearchOverlayComponent(QQmlComponent *component)
{
    setPaneOverlayComponent(searchOverlay_, component, kSearchOverlayProperty,
                            "terminal search overlay",
                            &TerminalWorkspace::searchOverlayComponentChanged);
}

void TerminalWorkspace::setReadOnlyOverlayComponent(QQmlComponent *component)
{
    setPaneOverlayComponent(
        readOnlyOverlay_, component, kReadOnlyOverlayProperty,
        "terminal read-only overlay",
        &TerminalWorkspace::readOnlyOverlayComponentChanged);
}

void TerminalWorkspace::setResizeOverlayComponent(QQmlComponent *component)
{
    setPaneOverlayComponent(resizeOverlay_, component, kResizeOverlayProperty,
                            "terminal resize overlay",
                            &TerminalWorkspace::resizeOverlayComponentChanged);
}

void TerminalWorkspace::setScrollbarComponent(QQmlComponent *component)
{
    setPaneOverlayComponent(scrollbar_, component, kScrollbarProperty,
                            "terminal scrollbar",
                            &TerminalWorkspace::scrollbarComponentChanged);
}

void TerminalWorkspace::setBellBorderComponent(QQmlComponent *component)
{
    setPaneOverlayComponent(bellBorder_, component, kBellBorderProperty,
                            "terminal bell border",
                            &TerminalWorkspace::bellBorderComponentChanged);
}

void TerminalWorkspace::setPaneOverlayComponent(
    PaneOverlaySlot &slot, QQmlComponent *component, const char *paneProperty,
    const char *description, void (TerminalWorkspace::*changedSignal)())
{
    if (slot.component == component) {
        return;
    }
    const RevisionCounter::Value revision = slot.revision.advance();
    const QPointer<QQmlComponent> requestedComponent(component);
    const QPointer<TerminalWorkspace> guard(this);
    const auto stillCurrent = [guard, &slot, revision] {
        return guard != nullptr && slot.revision.isCurrent(revision);
    };

    QObject::disconnect(slot.destructionConnection);
    slot.destructionConnection = {};

    // Component creation and destruction can execute QML callbacks. Traverse
    // a stable pane snapshot so a callback cannot invalidate the split tree
    // underneath this lifecycle update.
    const QVector<QPointer<TerminalPane>> panes = paneSnapshot();
    for (const QPointer<TerminalPane> &pane : panes) {
        clearPaneOverlay(pane, paneProperty);
        if (!stillCurrent()) return;
    }

    slot.component = requestedComponent;
    if (requestedComponent != nullptr) {
        PaneOverlaySlot *const guardedSlot = &slot;
        slot.destructionConnection = connect(
            requestedComponent, &QObject::destroyed, this,
            [this, guardedSlot, paneProperty, changedSignal] {
                const QPointer<TerminalWorkspace> destructionGuard(this);
                const RevisionCounter::Value destructionRevision =
                    guardedSlot->revision.advance();
                guardedSlot->component.clear();
                guardedSlot->destructionConnection = {};
                for (const QPointer<TerminalPane> &pane : paneSnapshot()) {
                    clearPaneOverlay(pane, paneProperty);
                    if (destructionGuard == nullptr
                        || !guardedSlot->revision.isCurrent(
                            destructionRevision)) {
                        return;
                    }
                }
                (this->*changedSignal)();
            });

        for (const QPointer<TerminalPane> &pane : panes) {
            if (pane == nullptr) continue;
            if (!attachPaneOverlay(requestedComponent, pane, paneProperty,
                                   description)
                || !stillCurrent()) {
                return;
            }
        }
    }

    (this->*changedSignal)();
}

bool TerminalWorkspace::attachPaneOverlays(TerminalPane *pane)
{
    const QPointer<TerminalWorkspace> guard(this);
    const QPointer<TerminalPane> paneGuard(pane);
    if (!attachPaneOverlay(searchOverlay_.component, paneGuard,
                           kSearchOverlayProperty, "terminal search overlay")
        || guard == nullptr) {
        return false;
    }
    if (!attachPaneOverlay(readOnlyOverlay_.component, paneGuard,
                           kReadOnlyOverlayProperty,
                           "terminal read-only overlay")
        || guard == nullptr) {
        return false;
    }
    if (!attachPaneOverlay(resizeOverlay_.component, paneGuard,
                           kResizeOverlayProperty, "terminal resize overlay")
        || guard == nullptr) {
        return false;
    }
    if (!attachPaneOverlay(scrollbar_.component, paneGuard, kScrollbarProperty,
                           "terminal scrollbar")
        || guard == nullptr) {
        return false;
    }
    return attachPaneOverlay(bellBorder_.component, paneGuard,
                             kBellBorderProperty, "terminal bell border")
        && guard != nullptr;
}

QVector<QPointer<TerminalPane>> TerminalWorkspace::paneSnapshot() const
{
    QVector<QPointer<TerminalPane>> panes;
    forEachPane([&panes](const PaneHandle &handle) {
        if (handle.pane != nullptr) panes.append(handle.pane);
    });
    for (const QPointer<TerminalPane> &pane : pendingPanes_) {
        if (pane != nullptr && !panes.contains(pane)) panes.append(pane);
    }
    return panes;
}

QVector<TerminalWorkspace::BroadPaneTarget>
TerminalWorkspace::broadPaneSnapshot() const
{
    QVector<BroadPaneTarget> panes;
    forEachPane([&panes](const PaneHandle &handle) {
        if (handle.pane != nullptr) {
            panes.append({
                .paneId = handle.id,
                .pane = handle.pane,
            });
        }
    });
    return panes;
}

bool TerminalWorkspace::broadPaneTargetIsLive(
    const BroadPaneTarget &target) const
{
    return target.pane != nullptr && paneForId(target.paneId) == target.pane;
}

void TerminalWorkspace::setDefaultLaunchOptions(const LaunchOptions &options)
{
    defaultOptions_ = options;
}

bool TerminalWorkspace::initialize(
    const LaunchOptions &options,
    TerminalSessionStartMode initialSessionStartMode,
    std::shared_ptr<InitialSessionCoordinator> initialSessionCoordinator)
{
    return initialize(
        options, initialSessionStartMode, std::move(initialSessionCoordinator),
        GhosttyKeybindProgram::compile(options.keybindSource).program);
}

bool TerminalWorkspace::initialize(
    const LaunchOptions &options,
    TerminalSessionStartMode initialSessionStartMode,
    std::shared_ptr<InitialSessionCoordinator> initialSessionCoordinator,
    GhosttyKeybindProgram keybindProgram)
{
    if (initialized_) return false;
    initialized_ = true;
    initialSessionCoordinator_ = std::move(initialSessionCoordinator);
    const QPointer<TerminalWorkspace> guard(this);
    applyLaunchOptions(options, std::move(keybindProgram));
    if (guard == nullptr) return true;

    const LaunchOptions initialOptions = effectiveOptions_;
    const qreal devicePixelRatio =
        window() != nullptr ? window()->devicePixelRatio() : 1.0;
    const TerminalCellMetrics metrics =
        terminalCellMetrics(initialOptions.typography, devicePixelRatio);
    const PaneHandle initialPane =
        createNewTab({},
                     terminalSessionGeometryForViewport(
                         width(), height(), metrics.cellWidth,
                         metrics.cellHeight, devicePixelRatio),
                     initialSessionStartMode);
    if (guard == nullptr) return true;
    if (!initialPane.isValid()) return false;
    if (initialSessionStartMode == TerminalSessionStartMode::Deferred) {
        deferredInitialPane_ = initialPane.pane;
        deferredInitialPaneId_ = initialPane.id;
    }
    return true;
}

bool TerminalWorkspace::armInitialSessionStart()
{
    if (deferredInitialPane_ == nullptr) {
        return false;
    }
    const QPointer<TerminalWorkspace> workspace(this);
    const QPointer<TerminalPane> pane(deferredInitialPane_);
    const PaneId paneId = deferredInitialPaneId_;
    const bool armed = deferredInitialPane_->armDeferredSessionStart(
        [workspace, pane, paneId] {
            if (workspace == nullptr || pane == nullptr) {
                return QSizeF{};
            }
            if (pane->isVisible()) {
                return pane->size();
            }
            Tab *const tab =
                workspace->tabById(workspace->tabIdForPane(paneId));
            if (tab == nullptr || tab->root == nullptr) {
                return pane->size();
            }
            workspace->updateNodeGeometry(tab->root.get(),
                                          workspace->boundingRect());
            QSizeF viewportSize;
            if (tab->zoomedPaneId == paneId) {
                viewportSize = workspace->boundingRect().size();
            } else {
                Node *const node = workspace->findNode(tab->root.get(), paneId);
                viewportSize =
                    node != nullptr ? node->geometry.size() : pane->size();
            }
            // No PTY exists yet, so keeping this hidden item's logical size in
            // sync cannot violate inactive-tab resize suppression. It also
            // prevents a later DPR/screen refresh from replaying the hidden
            // normal size after the deferred session has started.
            if (pane->size() != viewportSize) {
                pane->setSize(viewportSize);
            }
            return viewportSize;
        });
    if (!armed) {
        deferredInitialPane_ = nullptr;
        deferredInitialPaneId_ = {};
    }
    return armed;
}

void TerminalWorkspace::applyLaunchOptions(const LaunchOptions &options)
{
    applyLaunchOptions(
        options, GhosttyKeybindProgram::compile(options.keybindSource).program);
}

void TerminalWorkspace::applyLaunchOptions(const LaunchOptions &options,
                                           GhosttyKeybindProgram keybindProgram)
{
    const RevisionCounter::Value revision = launchOptionsRevision_.advance();
    const QPointer<TerminalWorkspace> guard(this);
    const bool wasTabBarVisible = tabBarVisible();
    const bool wasTabBarAtBottom = tabBarAtBottom();
    const WindowDecorationMode previousWindowDecoration = windowDecoration();
    keybindProgram_ = std::move(keybindProgram);
    const GhosttyKeybindProgram appliedProgram = keybindProgram_;
    effectiveOptions_ = options;
    const LaunchOptions appliedOptions = effectiveOptions_;
    const auto stillCurrentUpdate = [guard, revision, appliedProgram] {
        return guard != nullptr
            && guard->launchOptionsRevision_.isCurrent(revision)
            && guard->keybindProgram().isSameGeneration(appliedProgram);
    };
    if (windowDecoration() != previousWindowDecoration) {
        Q_EMIT windowDecorationChanged();
        if (!stillCurrentUpdate()) return;
    }
    if (tabBarAtBottom() != wasTabBarAtBottom) {
        Q_EMIT tabsLocationChanged();
        if (!stillCurrentUpdate()) return;
    }
    if (tabBarVisible() != wasTabBarVisible) {
        Q_EMIT tabBarVisibleChanged();
        if (!stillCurrentUpdate()) return;
    }
    for (SplitDividerItem *divider : std::as_const(splitDividers_)) {
        divider->setColor(effectiveOptions_.splitAppearance.dividerColor);
    }
    const QVector<QPointer<TerminalPane>> panes = paneSnapshot();
    for (const QPointer<TerminalPane> &pane : panes) {
        if (pane == nullptr) continue;
        pane->applyRuntimeOptions(appliedOptions, appliedProgram);
        if (!stillCurrentUpdate()) return;
    }
    reevaluatePendingClose();
}

bool TerminalWorkspace::tabBarVisible() const
{
    switch (effectiveOptions_.windowShowTabBar) {
    case WindowShowTabBar::Always: return true;
    case WindowShowTabBar::Auto: return tabs_.size() >= 2;
    case WindowShowTabBar::Never: return false;
    }
    Q_UNREACHABLE_RETURN(false);
}

QStringList TerminalWorkspace::tabTitles() const
{
    QStringList result;
    result.reserve(tabModel_.count());
    for (int index = 0; index < tabModel_.count(); ++index) {
        result.append(
            tabModel_.data(tabModel_.index(index, 0), TabListModel::TitleRole)
                .toString());
    }
    return result;
}

QString TerminalWorkspace::currentTitle() const
{
    return tabModel_
        .data(tabModel_.index(currentIndex_, 0), TabListModel::TitleRole)
        .toString();
}

bool TerminalWorkspace::dispatchAction(const WorkspaceActionRequest &request)
{
    return executeAction(request);
}

bool TerminalWorkspace::executeSurfaceActionOnAllPanes(QStringView action)
{
    const std::optional<GhosttyConfiguredAction> parsed =
        GhosttyActionCatalog::parseConfiguredAction(action);
    return parsed.has_value() && executeSurfaceActionOnAllPanes(*parsed);
}

bool TerminalWorkspace::executeSurfaceActionOnAllPanes(
    const GhosttyConfiguredAction &action)
{
    if (topologyMutation_
        || std::holds_alternative<ApplicationAction>(action)) {
        return false;
    }

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

    // Ghostty scopes window-state actions to a source surface, but every Qt
    // pane in a workspace ultimately mutates the same host window. Applying a
    // toggle synchronously once per pane would cancel itself for an even pane
    // count. Keep broad dispatch once per registered workspace/window while
    // leaving all other surface actions on the ordinary per-pane fanout path.
    const auto *workspaceAction = std::get_if<WorkspaceActionRequest>(&action);
    if (workspaceAction != nullptr
        && (workspaceAction->action == WorkspaceAction::ToggleFullscreen
            || workspaceAction->action == WorkspaceAction::ToggleMaximize
            || workspaceAction->action
                == WorkspaceAction::ToggleWindowDecorations)) {
        return !panes.empty() && dispatchAction(*workspaceAction);
    }

    const QPointer<TerminalWorkspace> guard(this);
    const bool previousBroadFanout = std::exchange(broadActionFanout_, true);
    const auto restoreBroadFanout = qScopeGuard([guard, previousBroadFanout] {
        if (guard != nullptr) {
            guard->broadActionFanout_ = previousBroadFanout;
        }
    });
    bool performed = false;
    for (const QPointer<TerminalPane> &pane : panes) {
        if (guard == nullptr) break;
        if (pane != nullptr) {
            performed = pane->executeConfiguredAction(action) || performed;
        }
    }
    return performed;
}

bool TerminalWorkspace::executeAction(const WorkspaceActionRequest &request)
{
    // Model and workspace signals are synchronous. Reject nested actions while
    // a stable-ID batch is being committed so observers cannot invalidate the
    // topology between its pane shutdown and row-removal phases.
    if (topologyMutation_ || windowCloseState_ != WindowCloseState::Open) {
        return false;
    }

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
        return createNewTab(sourcePaneId).isValid();
    }
    case WorkspaceAction::ActivateTab:
        if (tabById(request.context.tabId) == nullptr) return false;
        activateTab(request.context.tabId);
        return true;
    case WorkspaceAction::ActivatePane:
        if (paneForId(request.context.paneId) == nullptr
            || !contextMatchesPane())
            return false;
        activatePane(request.context.paneId);
        return true;
    case WorkspaceAction::CloseTab:
        if (tabById(request.context.tabId) == nullptr
            || (request.context.paneId.isValid()
                && (paneForId(request.context.paneId) == nullptr
                    || !contextMatchesPane()))) {
            return false;
        }
        closeTab(request.context.tabId, request.context.closeTabMode);
        return true;
    case WorkspaceAction::ClosePane:
        if (paneForId(request.context.paneId) == nullptr
            || !contextMatchesPane())
            return false;
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
            const int surfaceWidth =
                std::max(1, qRound(sourcePane->width() * devicePixelRatio));
            const int surfaceHeight =
                std::max(1, qRound(sourcePane->height() * devicePixelRatio));
            direction = surfaceWidth > surfaceHeight
                ? WorkspaceAction::SplitRight
                : WorkspaceAction::SplitDown;
        }
        const bool horizontal = direction == WorkspaceAction::SplitLeft
            || direction == WorkspaceAction::SplitRight;
        const bool placeNewPaneFirst = direction == WorkspaceAction::SplitLeft
            || direction == WorkspaceAction::SplitUp;
        return splitPane(request.context.paneId,
                         horizontal ? Qt::Horizontal : Qt::Vertical,
                         placeNewPaneFirst);
    }
    case WorkspaceAction::NavigatePane:
        if (paneForId(request.context.paneId) == nullptr
            || !contextMatchesPane())
            return false;
        return navigateFrom(request.context.paneId,
                            static_cast<int>(request.context.value));
    case WorkspaceAction::NavigatePaneRelative:
        if (paneForId(request.context.paneId) == nullptr
            || !contextMatchesPane())
            return false;
        return navigateRelative(request.context.paneId, request.context.value);
    case WorkspaceAction::ChangeTabRelative:
        if (request.context.tabId.isValid()
            && tabById(request.context.tabId) == nullptr)
            return false;
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
    case WorkspaceAction::SetSurfaceTitle: {
        TerminalPane *pane = paneForId(request.context.paneId);
        if (pane == nullptr || !contextMatchesPane()) return false;
        pane->setSurfaceTitle(request.payload);
        return true;
    }
    case WorkspaceAction::PromptSurfaceTitle:
        if (paneForId(request.context.paneId) == nullptr
            || !contextMatchesPane()) {
            return false;
        }
        return enqueueTitlePrompt(request.context.paneId);
    case WorkspaceAction::PromptTabTitle: {
        if (request.context.paneId.isValid()
            && (paneForId(request.context.paneId) == nullptr
                || !contextMatchesPane())) {
            return false;
        }
        const TabId tabId = request.context.paneId.isValid()
            ? tabIdForPane(request.context.paneId)
            : (request.context.tabId.isValid() ? request.context.tabId
                                               : currentTabId());
        return enqueueTitlePrompt(tabId);
    }
    case WorkspaceAction::SetTabTitle: {
        if (request.context.paneId.isValid()
            && (paneForId(request.context.paneId) == nullptr
                || !contextMatchesPane())) {
            return false;
        }
        const TabId tabId = request.context.paneId.isValid()
            ? tabIdForPane(request.context.paneId)
            : (request.context.tabId.isValid() ? request.context.tabId
                                               : currentTabId());
        Tab *tab = tabById(tabId);
        if (tab == nullptr) return false;
        if (tab->titleOverride != request.payload) {
            tab->titleOverride = request.payload;
            refreshTab(tabId);
        }
        return true;
    }
    case WorkspaceAction::ResizeSplit:
        if (paneForId(request.context.paneId) == nullptr
            || !contextMatchesPane())
            return false;
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
    case WorkspaceAction::ToggleMaximize:
    case WorkspaceAction::ToggleWindowDecorations:
        if ((request.context.paneId.isValid()
             && (paneForId(request.context.paneId) == nullptr
                 || !contextMatchesPane()))
            || (request.context.tabId.isValid()
                && tabById(request.context.tabId) == nullptr)
            || window() == nullptr) {
            return false;
        }
        if (request.action == WorkspaceAction::ToggleFullscreen) {
            Q_EMIT toggleFullscreenRequested();
        } else if (request.action == WorkspaceAction::ToggleMaximize) {
            Q_EMIT toggleMaximizeRequested();
        } else {
            toggleWindowDecorations();
        }
        return true;
    }
    return false;
}

void TerminalWorkspace::toggleWindowDecorations()
{
    const WindowDecorationMode previous = windowDecoration();
    if (windowDecorationOverride_.has_value()) {
        windowDecorationOverride_.reset();
    } else {
        windowDecorationOverride_ =
            effectiveOptions_.windowDecoration == WindowDecorationMode::None
            ? WindowDecorationMode::Auto
            : WindowDecorationMode::None;
    }
    if (windowDecoration() != previous) {
        Q_EMIT windowDecorationChanged();
    }
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

bool TerminalWorkspace::hasActivePane() const
{
    return paneForId(currentPaneId()) != nullptr;
}

bool TerminalWorkspace::focusActivePane()
{
    const QPointer<TerminalWorkspace> guard(this);
    const QPointer<TerminalPane> pane(paneForId(currentPaneId()));
    if (pane == nullptr) return false;
    pane->focusTerminal();
    return guard != nullptr && pane != nullptr;
}

std::optional<LaunchOptions> TerminalWorkspace::newWindowLaunchOptions(
    const LaunchOptions &applicationOptions, PaneId sourcePaneId) const
{
    if (!sourcePaneId.isValid()) sourcePaneId = currentPaneId();
    TerminalPane *const pane = paneForId(sourcePaneId);
    if (pane == nullptr) return std::nullopt;
    return pane->windowLaunchOptions(applicationOptions);
}

WorkspaceCloseAssessment TerminalWorkspace::closeAssessment() const
{
    return assessWorkspaceClose();
}

TerminalWorkspace::PaneHandle TerminalWorkspace::createPane(
    const LaunchOptions &options,
    std::optional<TerminalSessionGeometry> initialGeometry,
    TerminalSessionStartMode startMode)
{
    const PaneId paneId(nextPaneId_++);
    auto detachedPane = std::make_unique<TerminalPane>(
        options, nullptr, std::move(initialGeometry), startMode,
        initialSessionCoordinator_, keybindProgram_);
    TerminalPane *const pane = detachedPane.get();
    const QPointer<TerminalWorkspace> workspaceGuard(this);
    const QPointer<TerminalPane> paneGuard(pane);
    pendingPanes_.append(paneGuard);
    const auto removePending = qScopeGuard([workspaceGuard, paneGuard] {
        if (workspaceGuard == nullptr) return;
        workspaceGuard->pendingPanes_.removeIf(
            [paneGuard](const QPointer<TerminalPane> &candidate) {
                return candidate == paneGuard || candidate == nullptr;
            });
    });
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
                default: break;
                }
            }
            return dispatchAction(request);
        });
    connect(pane, &TerminalPane::activated, this,
            [this, paneId](TerminalPane *) {
                dispatchAction(
                    {WorkspaceAction::ActivatePane, {TabId{}, paneId, 0}});
                Q_EMIT workspaceActivated();
            });
    connect(pane, &TerminalPane::titleChanged, this,
            [this, paneId] { refreshTab(tabIdForPane(paneId)); });
    connect(pane, &TerminalPane::bellChanged, this, [this, paneId] {
        const TabId tabId = tabIdForPane(paneId);
        const Tab *const tab = tabById(tabId);
        if (tab != nullptr && tab->activePaneId == paneId) {
            refreshTab(tabId);
        }
    });
    connect(pane, &TerminalPane::bellRang, this,
            [this, paneId, pane](TerminalPane *) {
                if (paneForId(paneId) != pane) return;
                const TabId tabId = tabIdForPane(paneId);
                Tab *const tab = tabById(tabId);
                if (tab == nullptr) return;

                const QPointer<TerminalWorkspace> guard(this);
                if (tabIndexForId(tabId) != currentIndex_ && !tab->attention) {
                    tab->attention = true;
                    refreshTab(tabId);
                    if (guard == nullptr) return;
                }

                if (paneForId(paneId) != pane) return;
                QQuickWindow *const host = window();
                if (!effectiveOptions_.bellFeatures.attention || host == nullptr
                    || host->isActive()) {
                    return;
                }
                Q_EMIT windowAttentionRequested();
            });
    connect(pane, &TerminalPane::currentDirectoryChanged, this,
            [this, paneId] { refreshTab(tabIdForPane(paneId)); });
    connect(pane, &TerminalPane::sessionEnded, this,
            [this, paneId, pane](TerminalPane *, int, int) {
                removePendingPastesForPane({paneId, pane});
                refreshTab(tabIdForPane(paneId));
            });
    connect(pane, &TerminalPane::processStateChanged, this, [this, paneId] {
        refreshTab(tabIdForPane(paneId));
        reevaluatePendingClose();
    });
    connect(pane, &TerminalPane::readOnlyChanged, this, [this, paneId] {
        refreshTab(tabIdForPane(paneId));
        reevaluatePendingClose();
    });
    connect(pane, &TerminalPane::requestNewTab, this, [this, paneId] {
        dispatchAction(
            {WorkspaceAction::NewTab, {tabIdForPane(paneId), paneId, 0}});
    });
    connect(pane, &TerminalPane::requestSplit, this,
            [this, paneId](WorkspaceAction action) {
                dispatchAction({action, {tabIdForPane(paneId), paneId, 0}});
            });
    connect(pane, &TerminalPane::requestClose, this, [this, paneId] {
        dispatchAction(
            {WorkspaceAction::ClosePane, {tabIdForPane(paneId), paneId, 0}});
    });
    connect(pane, &TerminalPane::requestCloseTab, this,
            [this, paneId](CloseTabMode mode) {
                const TabId tabId = tabIdForPane(paneId);
                WorkspaceActionContext context;
                context.tabId = tabId;
                context.paneId = paneId;
                context.closeTabMode = mode;
                dispatchAction({WorkspaceAction::CloseTab, context});
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
    // Close approval may synchronously destroy this workspace. Queue pane
    // requests so a multi-action keybinding always unwinds first.
    connect(pane, &TerminalPane::requestCloseWindow, this,
            &TerminalWorkspace::requestWindowClose, Qt::QueuedConnection);
    connect(pane, &TerminalPane::applicationActionRequested, this,
            [this, paneId](ApplicationAction action) {
                Q_EMIT applicationActionRequested(action, paneId);
            });
    connect(pane, &TerminalPane::windowNavigationRequested, this,
            [this, paneId, pane](WindowNavigationAction action) {
                if (paneForId(paneId) != pane) return;
                Q_EMIT windowNavigationRequested(action, paneId);
            });
    connect(pane, &TerminalPane::broadActionsRequested, this,
            &TerminalWorkspace::broadActionsRequested);
    connect(pane, &TerminalPane::unsafePasteRequested, this,
            [this, paneId, pane](quint64 requestId, const QString &text,
                                 TerminalPane *) {
                if (paneForId(paneId) != pane) {
                    pane->cancelPaste(requestId);
                    return;
                }
                beginUnsafePaste(requestId, text, paneId);
            });
    connect(pane, &TerminalPane::contextMenuRequested, this,
            [this, paneId, pane](const QPointF &windowPosition,
                                 bool selectionAvailable) {
                beginContextMenu({paneId, pane}, windowPosition,
                                 selectionAvailable);
            });

    // Parent and overlay publication can execute arbitrary QML. The pending
    // registry makes this pane participate in any nested config reload before
    // its stable ID is inserted into the tab tree.
    detachedPane.release();
    const auto validateOwnership = [workspaceGuard,
                                    paneGuard](bool requireVisualParent) {
        const bool valid = workspaceGuard != nullptr && paneGuard != nullptr
            && paneGuard->parent() == workspaceGuard
            && (!requireVisualParent
                || paneGuard->parentItem() == workspaceGuard);
        if (!valid && paneGuard != nullptr) delete paneGuard.data();
        return valid;
    };
    pane->setParent(this);
    if (!validateOwnership(false)) return {};
    pane->setParentItem(this);
    if (!validateOwnership(true)) return {};
    if (!attachPaneOverlays(paneGuard)) {
        (void)validateOwnership(true);
        return {};
    }
    if (!validateOwnership(true)) return {};
    return {paneId, paneGuard};
}

void TerminalWorkspace::newTab()
{
    dispatchAction({WorkspaceAction::NewTab, {}});
}

TerminalWorkspace::PaneHandle TerminalWorkspace::createNewTab(
    PaneId sourcePaneId, std::optional<TerminalSessionGeometry> initialGeometry,
    TerminalSessionStartMode startMode)
{
    if (topologyMutation_) return {};
    const QPointer<TerminalWorkspace> guard(this);
    const bool previousMutation = std::exchange(topologyMutation_, true);
    const auto restoreMutation = qScopeGuard([guard, previousMutation] {
        if (guard != nullptr) {
            guard->topologyMutation_ = previousMutation;
        }
    });

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
            options = withoutInitialCommand(std::move(options));
        }
    }
    const int insertionIndex =
        options.windowNewTabPosition == WindowNewTabPosition::Current
            && currentIndex_ >= 0
        ? currentIndex_ + 1
        : static_cast<int>(tabs_.size());

    const PaneHandle pane =
        createPane(options, std::move(initialGeometry), startMode);
    if (guard == nullptr || !pane.isValid()) return {};
    auto tab = std::make_unique<Tab>();
    tab->id = TabId(nextTabId_++);
    tab->root = std::make_unique<Node>(pane);
    tab->activePaneId = pane.id;
    const TabId tabId = tab->id;
    const TabListEntry entry = tabListEntry(*tab);
    tabs_.insert(tabs_.begin() + insertionIndex, std::move(tab));
    initialTabCreated_ = true;

    const bool modelInserted = tabModel_.insert(insertionIndex, entry);
    if (guard == nullptr) return {};
    Q_ASSERT(modelInserted);
    if (!modelInserted) return {};
    Q_EMIT tabTitlesChanged();
    if (guard == nullptr) return {};
    if (tabBarVisible() != wasTabBarVisible) {
        Q_EMIT tabBarVisibleChanged();
        if (guard == nullptr) return {};
    }
    activateTab(tabId);
    if (guard == nullptr || pane.pane == nullptr) return {};
    return pane;
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
    int index = tabIndexForId(id);
    if (index < 0) {
        return;
    }
    Tab *const targetTab = tabs_[static_cast<size_t>(index)].get();
    if (targetTab->attention) {
        targetTab->attention = false;
        const QPointer<TerminalWorkspace> attentionGuard(this);
        tabModel_.replace(id, tabListEntry(*targetTab));
        if (attentionGuard == nullptr) return;
        index = tabIndexForId(id);
        if (index < 0) return;
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
    const QPointer<TerminalWorkspace> guard(this);
    Q_EMIT currentIndexChanged();
    if (guard == nullptr) return;
    Q_EMIT currentTitleChanged();
    if (guard == nullptr) return;
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
    const int source = tabId.isValid() ? tabIndexForId(tabId) : currentIndex_;
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
    // Ghostty uses the source surface to resolve its containing window, then
    // advances from that window's currently selected tab. Keep the stable
    // source validation without letting inactive-pane or broad dispatch use
    // the source tab as the navigation base.
    if (origin.isValid() && tabById(origin) == nullptr) {
        return false;
    }
    const int count = static_cast<int>(tabs_.size());
    if (currentIndex_ < 0) {
        return false;
    }
    const int target = (currentIndex_ + delta % count + count) % count;
    if (target == currentIndex_) {
        return false;
    }
    setCurrentIndex(target);
    return true;
}

void TerminalWorkspace::splitRight()
{
    dispatchAction(
        {WorkspaceAction::SplitRight, {TabId{}, currentPaneId(), 0}});
}

void TerminalWorkspace::splitDown()
{
    dispatchAction({WorkspaceAction::SplitDown, {TabId{}, currentPaneId(), 0}});
}

bool TerminalWorkspace::splitPane(PaneId paneId, Qt::Orientation orientation,
                                  bool placeNewPaneFirst)
{
    if (topologyMutation_) return false;
    const TabId tabId = tabIdForPane(paneId);
    Tab *tab = tabById(tabId);
    if (tab == nullptr) {
        return false;
    }
    Node *node = findNode(tab->root.get(), paneId);
    if (node == nullptr || !node->isLeaf()) {
        return false;
    }

    const QPointer<TerminalWorkspace> guard(this);
    const QPointer<TerminalPane> oldPane(node->pane);
    if (oldPane == nullptr) return false;
    const LaunchOptions newPaneOptions =
        oldPane->splitLaunchOptions(effectiveOptions_);
    const bool previousMutation = std::exchange(topologyMutation_, true);
    const auto restoreMutation = qScopeGuard([guard, previousMutation] {
        if (guard != nullptr) {
            guard->topologyMutation_ = previousMutation;
        }
    });
    const PaneHandle newPane = createPane(newPaneOptions);
    if (guard == nullptr || oldPane == nullptr || !newPane.isValid()) {
        return false;
    }

    // Pane construction can run QML and config callbacks. Resolve the target
    // again by stable IDs instead of carrying tree pointers across that
    // boundary.
    tab = tabById(tabId);
    node = tab != nullptr ? findNode(tab->root.get(), paneId) : nullptr;
    if (node == nullptr || !node->isLeaf() || node->pane != oldPane) {
        delete newPane.pane.data();
        return false;
    }
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
    node->first =
        std::make_unique<Node>(placeNewPaneFirst ? newPane : oldHandle);
    node->second =
        std::make_unique<Node>(placeNewPaneFirst ? oldHandle : newPane);
    tab->activePaneId = newPane.id;
    updateSplitMembership(*tab);
    if (guard == nullptr) return true;
    const bool targetIsCurrent = tabId == currentTabId();
    if (targetIsCurrent) {
        layoutCurrentTab();
        if (guard == nullptr || newPane.pane == nullptr) return true;
        newPane.pane->focusTerminal();
        if (guard == nullptr) return true;
    } else {
        updateTabVisibility(*tab, false);
        if (guard == nullptr) return true;
    }
    refreshTab(tabId);
    return true;
}

void TerminalWorkspace::activatePane(PaneId paneId, PaneActivationReason reason)
{
    const int tabIndex = tabIndexForPane(paneId);
    if (tabIndex < 0) {
        return;
    }
    Tab *targetTab = tabs_[static_cast<size_t>(tabIndex)].get();
    bool zoomChanged = false;
    if (targetTab->zoomedPaneId.isValid()
        && targetTab->zoomedPaneId != paneId) {
        // Only split navigation participates in split-preserve-zoom. Direct
        // activation and structural operations retain Ghostty's ordinary
        // unzoom behavior.
        targetTab->zoomedPaneId =
            reason == PaneActivationReason::SplitNavigation
                && effectiveOptions_.splitPreserveZoomNavigation
            ? paneId
            : PaneId{};
        zoomChanged = true;
    }
    const bool activePaneChanged = targetTab->activePaneId != paneId;
    targetTab->activePaneId = paneId;

    if (tabIndex != currentIndex_) {
        if (zoomChanged || activePaneChanged) {
            refreshTab(targetTab->id);
        }
        activateTab(targetTab->id);
        return;
    }

    // Publish a single coherent tab-model update after the presentation has
    // switched. Direct signal observers never see the old active pane paired
    // with a zoom that has already moved to the destination.
    if (zoomChanged) {
        layoutCurrentTab();
    }
    if (zoomChanged || activePaneChanged) {
        refreshTab(targetTab->id);
    }
    if (TerminalPane *pane = paneForId(paneId); pane != nullptr) {
        pane->focusTerminal();
    }
}

void TerminalWorkspace::closeActivePane()
{
    const PaneId paneId = currentPaneId();
    dispatchAction({WorkspaceAction::ClosePane, {currentTabId(), paneId, 0}});
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
    if (!force && shouldConfirmPaneClose(*pane)) {
        if (!std::holds_alternative<std::monostate>(pendingClose_)) return;
        beginCloseConfirmation(
            PendingPaneClose{paneId, tabs_[static_cast<size_t>(tabIndex)]->id},
            pane->isReadOnly()
                ? QStringLiteral("This pane is read-only. Close it?")
                : QStringLiteral(
                      "A process is still running in this pane. Close it?"));
        return;
    }

    const QPointer<TerminalWorkspace> guard(this);
    {
        // Pending-prompt resolution and model updates below emit synchronous
        // signals. Keep the cached tab/tree valid until this mutation commits.
        const bool previousMutation = std::exchange(topologyMutation_, true);
        const auto restoreMutation = qScopeGuard([guard, previousMutation] {
            if (guard != nullptr) {
                guard->topologyMutation_ = previousMutation;
            }
        });
        Tab *tab = tabs_[static_cast<size_t>(tabIndex)].get();
        const TabId tabId = tab->id;
        const bool removedActivePane = tab->activePaneId == paneId;
        const PaneId nextActivePaneId = removedActivePane
            ? focusTargetAfterClosing(*tab, paneId)
            : tab->activePaneId;
        if (tab->zoomedPaneId == paneId) {
            tab->zoomedPaneId = {};
        }
        resolvePendingPaneRemoval({paneId, pane});
        if (guard == nullptr) return;
        removePaneFromNode(tab->root, paneId);
        if (tab->root == nullptr) {
            removeTab(tabId);
        } else {
            if (removedActivePane) {
                tab->activePaneId = nextActivePaneId;
            }
            if (findNode(tab->root.get(), tab->activePaneId) == nullptr) {
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
    }
    if (guard == nullptr) return;
    reevaluatePendingClose();
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
    dispatchAction({WorkspaceAction::CloseTab, {currentTabId(), PaneId{}, 0}});
}

void TerminalWorkspace::closeTab(TabId tabId, CloseTabMode mode, bool force)
{
    closeTabs({tabId, closeTabTargets(tabId, mode)}, force);
}

std::vector<TabId> TerminalWorkspace::closeTabTargets(TabId tabId,
                                                      CloseTabMode mode) const
{
    const int source = tabIndexForId(tabId);
    if (source < 0) return {};
    if (mode == CloseTabMode::This) return {tabId};

    // Match libadwaita's visual close order. Removing from right to left also
    // avoids repeatedly shifting the unvisited portion of the tab vector.
    const int firstTarget = [&] {
        switch (mode) {
        case CloseTabMode::Other: return 0;
        case CloseTabMode::Right: return source + 1;
        case CloseTabMode::This: break;
        }
        std::unreachable();
    }();
    std::vector<TabId> targets;
    targets.reserve(tabs_.size() - static_cast<std::size_t>(firstTarget));
    for (int index = static_cast<int>(tabs_.size()) - 1; index >= firstTarget;
         --index) {
        if (index != source) {
            targets.push_back(tabs_[static_cast<std::size_t>(index)]->id);
        }
    }
    return targets;
}

void TerminalWorkspace::closeTabs(PendingTabClose close, bool force)
{
    QSet<TabId> liveTabIds;
    liveTabIds.reserve(static_cast<qsizetype>(tabs_.size()));
    for (const std::unique_ptr<Tab> &tab : tabs_) liveTabIds.insert(tab->id);
    std::erase_if(close.targets, [&liveTabIds](TabId tabId) {
        return !liveTabIds.contains(tabId);
    });
    if (close.targets.empty()) return;

    if (!force) {
        // Once broad fanout has installed a dialog, later snapshot sources
        // must not mutate the first source's frozen outcome. Safe lifecycle
        // removal still enters through closePane and can prune these IDs.
        if (broadActionFanout_
            && !std::holds_alternative<std::monostate>(pendingClose_)) {
            return;
        }
        const WorkspaceCloseAssessment assessment =
            assessTabsClose(close.targets);
        if (assessment.needsConfirmation) {
            // The QML frontend presents one close dialog at a time. Keep the
            // first stable request intact while broad fanout continues.
            if (!std::holds_alternative<std::monostate>(pendingClose_)) return;

            const bool plural = close.targets.size() > 1;
            beginCloseConfirmation(
                std::move(close),
                assessment.hasReadOnlyPane
                    ? (plural
                           ? QStringLiteral(
                                 "Some tabs contain read-only panes. Close the tabs?")
                           : QStringLiteral(
                                 "This tab contains a read-only pane. Close the tab?"))
                    : (plural
                           ? QStringLiteral(
                                 "Processes are still running in these tabs. Close the tabs?")
                           : QStringLiteral(
                                 "Processes are still running in this tab. Close the tab?")));
            return;
        }
    }

    removeTabs(std::move(close));
}

void TerminalWorkspace::removeTab(TabId tabId)
{
    removeTabs({TabId{}, {tabId}});
}

void TerminalWorkspace::removeTabs(PendingTabClose close)
{
    struct IndexedTarget {
        int index = -1;
        TabId id;
    };

    std::vector<IndexedTarget> targets;
    targets.reserve(close.targets.size());
    QSet<TabId> targetIds;
    targetIds.reserve(static_cast<qsizetype>(close.targets.size()));
    for (const TabId tabId : close.targets) targetIds.insert(tabId);
    for (int index = 0; index < static_cast<int>(tabs_.size()); ++index) {
        const TabId tabId = tabs_[static_cast<std::size_t>(index)]->id;
        if (targetIds.contains(tabId)) {
            targets.push_back({index, tabId});
        }
    }
    if (targets.empty()) return;

    std::ranges::sort(targets, std::greater{}, &IndexedTarget::index);
    const bool wasTabBarVisible = tabBarVisible();
    const TabId selectedTabId = currentTabId();
    const int selectedIndex = currentIndex_;
    const auto survives = [this, &targetIds](TabId tabId) {
        return tabId.isValid() && !targetIds.contains(tabId)
            && tabById(tabId) != nullptr;
    };

    TabId nextSelectedTabId;
    if (survives(selectedTabId)) {
        nextSelectedTabId = selectedTabId;
    } else if (survives(close.originTabId)) {
        nextSelectedTabId = close.originTabId;
    } else {
        // Match the ordinary close fallback: the next surviving tab when
        // possible, otherwise the nearest survivor on the left.
        for (int index = std::max(0, selectedIndex + 1);
             index < static_cast<int>(tabs_.size()); ++index) {
            const TabId candidate = tabs_[static_cast<std::size_t>(index)]->id;
            if (!targetIds.contains(candidate)) {
                nextSelectedTabId = candidate;
                break;
            }
        }
        for (int index = std::min(selectedIndex - 1,
                                  static_cast<int>(tabs_.size()) - 1);
             !nextSelectedTabId.isValid() && index >= 0; --index) {
            const TabId candidate = tabs_[static_cast<std::size_t>(index)]->id;
            if (!targetIds.contains(candidate)) {
                nextSelectedTabId = candidate;
            }
        }
    }

    std::vector<PaneHandle> panes;
    for (const IndexedTarget &target : targets) {
        const Node *const root =
            tabs_[static_cast<std::size_t>(target.index)]->root.get();
        if (root != nullptr) root->collectPanes(panes);
    }
    const QPointer<TerminalWorkspace> guard(this);
    bool becameEmpty = false;
    {
        const bool previousMutation = std::exchange(topologyMutation_, true);
        const auto restoreMutation = qScopeGuard([guard, previousMutation] {
            if (guard != nullptr) {
                guard->topologyMutation_ = previousMutation;
            }
        });
        for (const PaneHandle &handle : panes) {
            resolvePendingPaneRemoval(handle);
            if (guard == nullptr) return;
            handle.pane->beginShutdown();
        }
        for (const PaneHandle &handle : panes) {
            handle.pane->setVisible(false);
            if (guard == nullptr) return;
            handle.pane->deleteLater();
        }

        for (const IndexedTarget &target : targets) {
            // Synchronous observers cannot mutate the topology while the
            // guard is held, but resolve the stable ID again so correctness
            // never depends on an index cached across signal boundaries.
            const int index = tabIndexForId(target.id);
            if (index < 0) continue;
            Q_ASSERT(tabModel_.idAt(index) == target.id);
            tabs_.erase(tabs_.begin() + index);

            // countChanged is emitted by removeAt. Publish a selection that
            // already describes the post-removal rows before observers run.
            currentIndex_ = nextSelectedTabId.isValid()
                ? tabIndexForId(nextSelectedTabId)
                : -1;
            const bool modelRemoved = tabModel_.removeAt(index);
            if (guard == nullptr) return;
            Q_ASSERT(modelRemoved);
            Q_UNUSED(modelRemoved);
        }
        for (const IndexedTarget &target : targets) {
            resolvePendingTabRemoval(target.id);
            if (guard == nullptr) return;
        }
        Q_EMIT tabTitlesChanged();
        if (guard == nullptr) return;
        if (tabBarVisible() != wasTabBarVisible) {
            Q_EMIT tabBarVisibleChanged();
            if (guard == nullptr) return;
        }

        becameEmpty = tabs_.empty();
        if (becameEmpty) {
            currentIndex_ = -1;
            updateSplitDividers(nullptr);
            if (guard == nullptr) return;
            Q_EMIT currentIndexChanged();
            if (guard == nullptr) return;
            Q_EMIT currentTitleChanged();
            if (guard == nullptr) return;
            if (!std::holds_alternative<std::monostate>(pendingClose_)) {
                (void)takePendingClose();
                if (guard == nullptr) return;
            }
        } else if (nextSelectedTabId != selectedTabId) {
            currentIndex_ = -1;
            activateTab(nextSelectedTabId);
        } else if (currentIndex_ != selectedIndex) {
            Q_EMIT currentIndexChanged();
        }
    }

    if (guard == nullptr) return;
    if (becameEmpty) {
        approveWindowClose();
    } else {
        reevaluatePendingClose();
    }
}

void TerminalWorkspace::resolvePendingPaneRemoval(PaneHandle handle)
{
    const QPointer<TerminalWorkspace> guard(this);
    removeContextMenuForPane(handle);
    if (guard == nullptr) return;
    removePendingPastesForPane(handle);
    if (guard == nullptr) return;
    removeTitlePrompts(TitlePromptTarget{handle.id});
    if (guard == nullptr) return;
    const PendingPaneClose *const pending =
        std::get_if<PendingPaneClose>(&pendingClose_);
    if (pending == nullptr || pending->paneId != handle.id) {
        return;
    }
    (void)takePendingClose();
}

void TerminalWorkspace::beginContextMenu(PaneHandle handle,
                                         const QPointF &windowPosition,
                                         bool selectionAvailable)
{
    if (topologyMutation_ || !handle.isValid()
        || paneForId(handle.id) != handle.pane
        || !std::isfinite(windowPosition.x())
        || !std::isfinite(windowPosition.y())) {
        return;
    }

    const QPointer<TerminalWorkspace> guard(this);
    if (pendingContextMenu_.has_value()) {
        const quint64 supersededRequestId = pendingContextMenu_->requestId;
        pendingContextMenu_.reset();
        Q_EMIT contextMenuCancelled(supersededRequestId);
        if (guard == nullptr || handle.pane == nullptr
            || paneForId(handle.id) != handle.pane
            || pendingContextMenu_.has_value()) {
            return;
        }
    }

    const quint64 requestId = nextNonzeroId(nextContextMenuId_);
    pendingContextMenu_ = PendingContextMenu{
        .requestId = requestId,
        .paneId = handle.id,
        .pane = handle.pane,
    };
    Q_EMIT contextMenuRequested(requestId, windowPosition, selectionAvailable);
}

void TerminalWorkspace::removeContextMenuForPane(PaneHandle handle)
{
    if (!pendingContextMenu_.has_value()
        || pendingContextMenu_->paneId != handle.id
        || pendingContextMenu_->pane != handle.pane) {
        return;
    }

    const quint64 requestId = pendingContextMenu_->requestId;
    pendingContextMenu_.reset();
    Q_EMIT contextMenuCancelled(requestId);
}

bool TerminalWorkspace::executeContextMenuAction(quint64 requestId,
                                                 const QString &action)
{
    if ((action != QStringLiteral("copy_to_clipboard:mixed")
         && action != QStringLiteral("paste_from_clipboard"))
        || requestId == 0 || !pendingContextMenu_.has_value()
        || pendingContextMenu_->requestId != requestId
        || pendingContextMenu_->pane == nullptr
        || paneForId(pendingContextMenu_->paneId)
            != pendingContextMenu_->pane) {
        return false;
    }

    const QPointer<TerminalPane> target = pendingContextMenu_->pane;
    return target->executeConfiguredAction(QStringView(action));
}

void TerminalWorkspace::finishContextMenu(quint64 requestId)
{
    if (requestId == 0 || !pendingContextMenu_.has_value()
        || pendingContextMenu_->requestId != requestId) {
        return;
    }

    pendingContextMenu_.reset();
    focusActivePane();
}

void TerminalWorkspace::resolvePendingTabRemoval(TabId tabId)
{
    const QPointer<TerminalWorkspace> guard(this);
    removeTitlePrompts(TitlePromptTarget{tabId});
    if (guard == nullptr) return;
    PendingTabClose *const pending =
        std::get_if<PendingTabClose>(&pendingClose_);
    if (pending == nullptr) return;

    std::erase(pending->targets, tabId);
    if (pending->targets.empty()) {
        (void)takePendingClose();
    }
}

void TerminalWorkspace::beginCloseConfirmation(PendingClose close,
                                               const QString &message)
{
    Q_ASSERT(std::holds_alternative<std::monostate>(pendingClose_));
    Q_ASSERT(!std::holds_alternative<std::monostate>(close));
    const quint64 requestId = nextNonzeroId(nextCloseConfirmationId_);
    std::visit(
        [requestId](auto &pending) {
            using T = std::remove_cvref_t<decltype(pending)>;
            if constexpr (!std::same_as<T, std::monostate>) {
                pending.requestId = requestId;
            }
        },
        close);
    pendingClose_ = std::move(close);
    Q_EMIT closeConfirmationRequested(requestId, message);
}

quint64
TerminalWorkspace::pendingCloseRequestId(const PendingClose &close) const
{
    return std::visit(
        [](const auto &pending) -> quint64 {
            using T = std::remove_cvref_t<decltype(pending)>;
            if constexpr (std::same_as<T, std::monostate>) {
                return 0;
            } else {
                return pending.requestId;
            }
        },
        close);
}

TerminalWorkspace::PendingClose TerminalWorkspace::takePendingClose()
{
    PendingClose close = std::exchange(pendingClose_, std::monostate{});
    const quint64 requestId = pendingCloseRequestId(close);
    if (requestId != 0) Q_EMIT closeConfirmationResolved(requestId);
    return close;
}

void TerminalWorkspace::commitPendingClose()
{
    Q_ASSERT(!topologyMutation_);
    // Resolving a dialog emits synchronously. Treat resolution and the
    // approved topology change as one transaction so an observer cannot
    // insert, split, or close a different target between those phases.
    const QPointer<TerminalWorkspace> guard(this);
    const bool previousMutation = std::exchange(topologyMutation_, true);
    const auto restoreMutation = qScopeGuard([guard, previousMutation] {
        if (guard != nullptr) {
            guard->topologyMutation_ = previousMutation;
        }
    });
    PendingClose close = takePendingClose();
    if (guard != nullptr) {
        performPendingClose(std::move(close));
    }
}

void TerminalWorkspace::performPendingClose(PendingClose close)
{
    if (const auto *pane = std::get_if<PendingPaneClose>(&close)) {
        if (tabIdForPane(pane->paneId) == pane->tabId) {
            closePane(pane->paneId, true);
        }
        return;
    }
    if (auto *tabs = std::get_if<PendingTabClose>(&close)) {
        closeTabs(std::move(*tabs), true);
        return;
    }
    if (std::holds_alternative<PendingWindowClose>(close)) {
        approveWindowClose();
    }
}

void TerminalWorkspace::reevaluatePendingClose()
{
    if (topologyMutation_) return;

    if (const auto *pendingPane =
            std::get_if<PendingPaneClose>(&pendingClose_)) {
        TerminalPane *pane = paneForId(pendingPane->paneId);
        if (pane != nullptr && shouldConfirmPaneClose(*pane)) {
            return;
        }
    } else if (const auto *pendingTabs =
                   std::get_if<PendingTabClose>(&pendingClose_)) {
        if (assessTabsClose(pendingTabs->targets).needsConfirmation) {
            return;
        }
    } else if (std::holds_alternative<PendingWindowClose>(pendingClose_)) {
        // An application-scoped confirmation represents a snapshot across all
        // windows. A state change in this one workspace must not silently
        // resolve that process-wide decision.
        if (applicationQuitRequested_) return;
        if (assessWorkspaceClose().needsConfirmation) return;
    } else {
        return;
    }

    commitPendingClose();
}

void TerminalWorkspace::requestWindowClose()
{
    requestWindowCloseImpl(WindowCloseIntent::WindowOnly);
}

void TerminalWorkspace::requestApplicationQuitConfirmation(
    WorkspaceCloseAssessment applicationAssessment)
{
    applicationQuitAssessment_ = applicationAssessment;
    requestWindowCloseImpl(WindowCloseIntent::QuitApplication);
}

void TerminalWorkspace::forceShutdownForApplicationQuit()
{
    if (windowCloseState_ != WindowCloseState::Open) return;
    if (topologyMutation_) {
        if (forceApplicationQuitShutdownScheduled_) return;
        forceApplicationQuitShutdownScheduled_ = true;
        QTimer::singleShot(0, this, [this] {
            forceApplicationQuitShutdownScheduled_ = false;
            forceShutdownForApplicationQuit();
        });
        return;
    }

    applicationQuitRequested_ = false;
    applicationQuitAssessment_.reset();
    if (!std::holds_alternative<std::monostate>(pendingClose_)) {
        const QPointer<TerminalWorkspace> guard(this);
        const bool previousMutation = std::exchange(topologyMutation_, true);
        const auto restoreMutation = qScopeGuard([guard, previousMutation] {
            if (guard != nullptr) {
                guard->topologyMutation_ = previousMutation;
            }
        });
        (void)takePendingClose();
        if (guard == nullptr) return;
    }
    approveWindowClose();
}

void TerminalWorkspace::requestWindowCloseImpl(WindowCloseIntent intent)
{
    if (intent == WindowCloseIntent::QuitApplication) {
        applicationQuitRequested_ = true;
    }
    if (windowCloseState_ != WindowCloseState::Open) {
        if (windowCloseState_ == WindowCloseState::Published) {
            approveApplicationQuit();
        }
        return;
    }
    if (topologyMutation_) {
        if (intent == WindowCloseIntent::QuitApplication) {
            scheduleApplicationQuitReconciliation();
        }
        return;
    }

    if (!std::holds_alternative<std::monostate>(pendingClose_)) {
        if (intent == WindowCloseIntent::WindowOnly
            || std::holds_alternative<PendingWindowClose>(pendingClose_)) {
            return;
        }

        // A process-wide quit supersedes a narrower pane or tab dialog. Give
        // the frontend a correctly scoped window confirmation with a fresh
        // correlation ID; responses from the replaced dialog stay harmless.
        const QPointer<TerminalWorkspace> guard(this);
        const bool previousMutation = std::exchange(topologyMutation_, true);
        const auto restoreMutation = qScopeGuard([guard, previousMutation] {
            if (guard != nullptr) {
                guard->topologyMutation_ = previousMutation;
            }
        });
        (void)takePendingClose();
        if (guard == nullptr) return;
    }

    const WorkspaceCloseAssessment assessment =
        applicationQuitRequested_ && applicationQuitAssessment_.has_value()
        ? *applicationQuitAssessment_
        : assessWorkspaceClose();
    if (assessment.needsConfirmation) {
        beginCloseConfirmation(
            PendingWindowClose{},
            assessment.hasReadOnlyPane
                ? (applicationQuitRequested_
                       ? QStringLiteral(
                             "A terminal window contains a read-only pane. Quit the application?")
                       : QStringLiteral(
                             "This window contains a read-only pane. Quit?"))
                : QStringLiteral(
                      "Terminal processes are still running. Quit and terminate them?"));
        return;
    }
    approveWindowClose();
}

void TerminalWorkspace::approveWindowClose()
{
    if (windowCloseState_ != WindowCloseState::Open) {
        if (windowCloseState_ == WindowCloseState::Published) {
            approveApplicationQuit();
        }
        return;
    }
    // This is an irreversible lifecycle boundary. Structural typed workspace
    // actions are rejected after the latch is set, including during
    // synchronous observers and before the host's deferred window close, so
    // every worker is covered by this one shutdown sweep. An application-quit
    // escalation, non-structural application action, or pane-local action may
    // still finish the Ghostty action chain that approved the ordinary close.
    windowCloseState_ = WindowCloseState::Committed;
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
    // Publishing is the externally destructive boundary: a host observer may
    // synchronously destroy the workspace. Leave the current key event and
    // complete configured action chain before crossing it.
    QTimer::singleShot(0, this, &TerminalWorkspace::publishWindowCloseApproval);
}

void TerminalWorkspace::publishWindowCloseApproval()
{
    if (windowCloseState_ != WindowCloseState::Committed) return;

    windowCloseState_ = WindowCloseState::Publishing;
    const QPointer<TerminalWorkspace> guard(this);
    Q_EMIT windowCloseApproved();
    if (guard != nullptr) {
        windowCloseState_ = WindowCloseState::Published;
        approveApplicationQuit();
    }
}

void TerminalWorkspace::approveApplicationQuit()
{
    if (!applicationQuitRequested_ || applicationQuitApprovedEmitted_) return;
    applicationQuitApprovedEmitted_ = true;
    Q_EMIT applicationQuitApproved();
}

void TerminalWorkspace::scheduleApplicationQuitReconciliation()
{
    if (applicationQuitReconciliationScheduled_
        || applicationQuitApprovedEmitted_) {
        return;
    }
    applicationQuitReconciliationScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        applicationQuitReconciliationScheduled_ = false;
        if (applicationQuitRequested_ && !applicationQuitApprovedEmitted_) {
            requestWindowCloseImpl(WindowCloseIntent::QuitApplication);
        }
    });
}

void TerminalWorkspace::confirmClose(quint64 confirmationId)
{
    if (topologyMutation_) {
        QTimer::singleShot(
            0, this, [this, confirmationId] { confirmClose(confirmationId); });
        return;
    }
    if (confirmationId == 0
        || pendingCloseRequestId(pendingClose_) != confirmationId) {
        return;
    }
    commitPendingClose();
}

void TerminalWorkspace::cancelClose(quint64 confirmationId)
{
    if (topologyMutation_) {
        QTimer::singleShot(
            0, this, [this, confirmationId] { cancelClose(confirmationId); });
        return;
    }
    if (confirmationId == 0
        || pendingCloseRequestId(pendingClose_) != confirmationId) {
        return;
    }
    if (std::holds_alternative<PendingWindowClose>(pendingClose_)) {
        // Clear the cancelled intent before publishing resolution. A direct
        // observer may synchronously issue a new quit request, which must win.
        const bool cancelledApplicationQuit = applicationQuitRequested_;
        applicationQuitRequested_ = false;
        applicationQuitAssessment_.reset();
        const QPointer<TerminalWorkspace> guard(this);
        (void)takePendingClose();
        if (guard == nullptr) return;
        // A synchronous resolution observer may already have submitted a new
        // application quit. In that case the replacement request wins and the
        // old cancellation must not clear the controller's new state.
        if (cancelledApplicationQuit && !applicationQuitRequested_) {
            Q_EMIT applicationQuitCancelled();
        }
        return;
    }
    (void)takePendingClose();
}

void TerminalWorkspace::beginUnsafePaste(quint64 requestId, const QString &text,
                                         PaneId paneId)
{
    if (requestId == 0 || text.isEmpty() || !paneId.isValid()) {
        return;
    }

    const auto matchesTarget = [paneId,
                                requestId](const PendingPasteTarget &target) {
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
            std::as_const(active.targets), paneId, &PendingPasteTarget::paneId);
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
    activePasteConfirmationId_ = nextNonzeroId(nextPasteConfirmationId_);
    Q_EMIT unsafePasteConfirmationRequested(
        activePasteConfirmationId_, pastePreview(pendingPastes_.front().text));
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

QString TerminalWorkspace::tabTitlePromptInitialValue(const Tab &tab) const
{
    if (!tab.titleOverride.isEmpty()) {
        return tab.titleOverride;
    }

    QString title = tabListEntry(tab).title;
    if (tab.zoomedPaneId.isValid()) {
        title.prepend(QStringLiteral("🔍 "));
    }
    return title;
}

bool TerminalWorkspace::enqueueTitlePrompt(PaneId paneId)
{
    TerminalPane *pane = paneForId(paneId);
    if (pane == nullptr) {
        return false;
    }
    const std::optional<QString> effective = pane->effectiveSurfaceTitle();
    return enqueueTitlePrompt(TitlePromptTarget{paneId},
                              effective.has_value() ? *effective : QString{});
}

bool TerminalWorkspace::enqueueTitlePrompt(TabId tabId)
{
    const Tab *tab = tabById(tabId);
    if (tab == nullptr) {
        return false;
    }

    return enqueueTitlePrompt(TitlePromptTarget{tabId},
                              tabTitlePromptInitialValue(*tab));
}

bool TerminalWorkspace::enqueueTitlePrompt(TitlePromptTarget target,
                                           QString initialTitle)
{
    if (!titlePromptTargetExists(target)) {
        return false;
    }

    const quint64 promptId = nextNonzeroId(nextTitlePromptId_);
    pendingTitlePrompts_.push_back({
        promptId,
        std::move(target),
        std::move(initialTitle),
    });

    if (!activeTitlePrompt_.has_value() && !titlePromptAdvanceScheduled_) {
        showNextTitlePrompt();
    }
    return true;
}

bool TerminalWorkspace::titlePromptTargetExists(
    const TitlePromptTarget &target) const
{
    if (const auto *paneId = std::get_if<PaneId>(&target)) {
        return paneForId(*paneId) != nullptr;
    }
    return tabById(std::get<TabId>(target)) != nullptr;
}

void TerminalWorkspace::showNextTitlePrompt()
{
    if (activeTitlePrompt_.has_value()) {
        return;
    }

    while (!pendingTitlePrompts_.empty()) {
        PendingTitlePrompt prompt = std::move(pendingTitlePrompts_.front());
        pendingTitlePrompts_.pop_front();
        if (!titlePromptTargetExists(prompt.target)) {
            continue;
        }

        activeTitlePrompt_ = std::move(prompt);
        const QString heading =
            std::holds_alternative<PaneId>(activeTitlePrompt_->target)
            ? QStringLiteral("Change Terminal Title")
            : QStringLiteral("Change Tab Title");
        Q_EMIT titlePromptRequested(activeTitlePrompt_->requestId, heading,
                                    activeTitlePrompt_->initialTitle);
        return;
    }
}

void TerminalWorkspace::scheduleNextTitlePrompt()
{
    if (activeTitlePrompt_.has_value() || pendingTitlePrompts_.empty()
        || titlePromptAdvanceScheduled_) {
        return;
    }

    titlePromptAdvanceScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        titlePromptAdvanceScheduled_ = false;
        showNextTitlePrompt();
    });
}

void TerminalWorkspace::confirmTitlePrompt(quint64 promptId,
                                           const QString &title)
{
    finishTitlePrompt(promptId, title);
}

void TerminalWorkspace::cancelTitlePrompt(quint64 promptId)
{
    finishTitlePrompt(promptId, std::nullopt);
}

void TerminalWorkspace::finishTitlePrompt(quint64 promptId,
                                          const std::optional<QString> &title)
{
    if (promptId == 0 || !activeTitlePrompt_.has_value()
        || activeTitlePrompt_->requestId != promptId) {
        return;
    }

    const PendingTitlePrompt prompt = std::move(*activeTitlePrompt_);
    activeTitlePrompt_.reset();
    const QPointer<TerminalWorkspace> guard(this);
    if (title.has_value()) {
        if (const auto *paneId = std::get_if<PaneId>(&prompt.target)) {
            if (TerminalPane *pane = paneForId(*paneId); pane != nullptr) {
                std::optional<QString> titleOverride;
                if (!title->isEmpty()) {
                    titleOverride = *title;
                }
                pane->setSurfaceTitleOverride(std::move(titleOverride));
                if (guard == nullptr) return;
            }
        } else {
            const TabId tabId = std::get<TabId>(prompt.target);
            if (tabById(tabId) != nullptr) {
                dispatchAction({
                    WorkspaceAction::SetTabTitle,
                    {tabId, PaneId{}, 0},
                    *title,
                });
                if (guard == nullptr) return;
            }
        }
    }
    Q_EMIT titlePromptResolved(promptId);
    if (guard != nullptr) scheduleNextTitlePrompt();
}

void TerminalWorkspace::removeTitlePrompts(TitlePromptTarget target)
{
    std::erase_if(pendingTitlePrompts_, [&target](const auto &prompt) {
        return prompt.target == target;
    });

    if (!activeTitlePrompt_.has_value()
        || activeTitlePrompt_->target != target) {
        return;
    }

    const quint64 promptId = activeTitlePrompt_->requestId;
    activeTitlePrompt_.reset();
    const QPointer<TerminalWorkspace> guard(this);
    Q_EMIT titlePromptResolved(promptId);
    if (guard != nullptr) scheduleNextTitlePrompt();
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
    const QPointer<TerminalWorkspace> guard(this);
    for (const PendingPasteTarget &target : std::as_const(pending.targets)) {
        if (TerminalPane *pane = paneForId(target.paneId); pane != nullptr) {
            if (confirmed) {
                pane->confirmPaste(target.requestId);
            } else {
                pane->cancelPaste(target.requestId);
            }
            if (guard == nullptr) return;
        }
    }
    Q_EMIT unsafePasteConfirmationResolved(confirmationId);
    if (guard != nullptr) schedulePendingPastePreview();
}

void TerminalWorkspace::removePendingPastesForPane(PaneHandle handle)
{
    const QPointer<TerminalWorkspace> guard(this);
    bool removedActiveRequest = false;
    for (qsizetype pendingIndex = pendingPastes_.size(); pendingIndex-- > 0;) {
        PendingPaste &pending = pendingPastes_[pendingIndex];
        for (qsizetype targetIndex = pending.targets.size();
             targetIndex-- > 0;) {
            const PendingPasteTarget target = pending.targets[targetIndex];
            if (target.paneId != handle.id) {
                continue;
            }
            if (handle.pane != nullptr) {
                handle.pane->cancelPaste(target.requestId);
                if (guard == nullptr) return;
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
            if (guard == nullptr) return;
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

    bool running = false;
    if (tab.root != nullptr) {
        tab.root->forEachPane([&running](const PaneHandle &handle) {
            running = running || handle.pane->isRunning();
        });
    }

    TabListEntry entry;
    entry.id = tab.id;
    entry.activePaneId = tab.activePaneId;
    entry.zoomed = tab.zoomedPaneId.isValid();
    entry.bell = activePane != nullptr && activePane->bellTitleVisible();
    entry.attention = tab.attention;
    entry.title = activePane != nullptr ? activePane->title()
                                        : QStringLiteral("Terminal");
    entry.titleOverride = tab.titleOverride;
    entry.currentDirectory =
        activePane != nullptr ? activePane->currentDirectory() : QString{};
    entry.running = running;
    entry.readOnly = activePane != nullptr && activePane->isReadOnly();
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
    const QPointer<TerminalWorkspace> guard(this);
    tabModel_.replace(tabId, tabListEntry(*tab));
    if (guard == nullptr) return;
    Q_EMIT tabTitlesChanged();
    if (guard == nullptr) return;
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

void TerminalWorkspace::updateNodeGeometry(Node *node, const QRectF &geometry)
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
        updateNodeGeometry(node->second.get(),
                           QRectF(geometry.x() + firstWidth + splitGap,
                                  geometry.y(), available - firstWidth,
                                  geometry.height()));
    } else {
        const qreal available = splitExtent(geometry, node->orientation);
        const qreal firstHeight = std::floor(available * node->ratio);
        updateNodeGeometry(
            node->first.get(),
            QRectF(geometry.x(), geometry.y(), geometry.width(), firstHeight));
        updateNodeGeometry(node->second.get(),
                           QRectF(geometry.x(),
                                  geometry.y() + firstHeight + splitGap,
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

TerminalWorkspace::Node *TerminalWorkspace::findSplitNode(Node *node,
                                                          quint64 splitId) const
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
        ? node->geometry.height()
        : node->geometry.width();
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
            geometry =
                QRectF(node->first->geometry.right(), node->geometry.top(),
                       splitGap, node->geometry.height());
        } else {
            geometry =
                QRectF(node->geometry.left(), node->first->geometry.bottom(),
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
TerminalWorkspace::beginSplitDividerDrag(quint64 splitId,
                                         const QPointF &position) const
{
    const Tab *tab = currentTab();
    if (tab == nullptr || tab->root == nullptr || tab->zoomedPaneId.isValid()) {
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
    const qreal pointer =
        split->orientation == Qt::Horizontal ? position.x() : position.y();
    if (!std::isfinite(pointer)) {
        return std::nullopt;
    }
    return SplitDividerDrag{pointer, split->ratio};
}

bool TerminalWorkspace::dragSplitDivider(quint64 splitId,
                                         const QPointF &position,
                                         const SplitDividerDrag &drag)
{
    Tab *tab = currentTab();
    if (tab == nullptr || tab->root == nullptr || tab->zoomedPaneId.isValid()) {
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
    const qreal pointer =
        split->orientation == Qt::Horizontal ? position.x() : position.y();
    if (!std::isfinite(pointer) || !std::isfinite(drag.pointer)
        || !std::isfinite(drag.ratio)) {
        return false;
    }
    const qreal ratio =
        std::clamp(drag.ratio + (pointer - drag.pointer) / available, 0.0, 1.0);
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

PaneId TerminalWorkspace::focusTargetAfterClosing(const Tab &tab,
                                                  PaneId paneId) const
{
    std::vector<PaneHandle> panes;
    if (tab.root != nullptr) tab.root->collectPanes(panes);
    if (panes.size() <= 1) return {};

    const auto closing = std::ranges::find(panes, paneId, &PaneHandle::id);
    if (closing == panes.end()) return {};

    // Match Ghostty's GTK and macOS split-tree rule: move to the previous
    // leaf, except that closing the leftmost leaf moves to the next one.
    return closing == panes.begin() ? panes[1].id : std::prev(closing)->id;
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
        slots, [paneId](const auto &slot) { return slot.first == paneId; });
    if (sourceSlot == slots.end()) return false;

    const auto findNearest = [&slots, paneId,
                              direction](const QRectF &source) -> PaneId {
        PaneId best;
        qreal bestDistance = std::numeric_limits<qreal>::max();
        for (const auto &[candidateId, candidate] : slots) {
            if (candidateId == paneId) continue;
            const bool valid = (direction == Qt::Key_Left
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

    activatePane(target, PaneActivationReason::SplitNavigation);
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
    const size_t destination =
        static_cast<size_t>((source + offset + count) % count);
    if (destination == static_cast<size_t>(source)) return false;

    activatePane(panes[destination].id, PaneActivationReason::SplitNavigation);
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
            setSplitRatio(*tab, *split,
                          split->ratio
                              + sign * static_cast<qreal>(amount) / extent);
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
    (void)tab->root->equalize();
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
    tab->zoomedPaneId =
        tab->zoomedPaneId.isValid() ? PaneId{} : tab->activePaneId;
    refreshTab(tab->id);
    if (tab->id == currentTabId()) {
        layoutCurrentTab();
    } else {
        updateTabVisibility(*tab, false);
    }
    return true;
}

WorkspaceCloseAssessment TerminalWorkspace::assessTabClose(const Tab &tab) const
{
    bool hasReadOnlyPane = false;
    bool childRunning = false;
    bool activeProcess = false;
    if (tab.root != nullptr) {
        tab.root->forEachPane([&hasReadOnlyPane, &childRunning,
                               &activeProcess](const PaneHandle &handle) {
            hasReadOnlyPane = hasReadOnlyPane || handle.pane->isReadOnly();
            childRunning = childRunning || handle.pane->isRunning();
            activeProcess = activeProcess || handle.pane->hasActiveProcess();
        });
    }
    return {
        .needsConfirmation = hasReadOnlyPane
            || shouldConfirmClose(effectiveOptions_.confirmCloseMode,
                                  childRunning, activeProcess),
        .hasReadOnlyPane = hasReadOnlyPane,
    };
}

WorkspaceCloseAssessment
TerminalWorkspace::assessTabsClose(const std::vector<TabId> &tabIds) const
{
    QSet<TabId> targetIds;
    targetIds.reserve(static_cast<qsizetype>(tabIds.size()));
    for (const TabId tabId : tabIds) targetIds.insert(tabId);

    WorkspaceCloseAssessment result;
    for (const std::unique_ptr<Tab> &tab : tabs_) {
        if (!targetIds.contains(tab->id)) continue;
        result |= assessTabClose(*tab);
    }
    return result;
}

WorkspaceCloseAssessment TerminalWorkspace::assessWorkspaceClose() const
{
    WorkspaceCloseAssessment result;
    for (const std::unique_ptr<Tab> &tab : tabs_) {
        result |= assessTabClose(*tab);
    }
    return result;
}

bool TerminalWorkspace::shouldConfirmPaneClose(const TerminalPane &pane) const
{
    return pane.isReadOnly()
        || shouldConfirmClose(effectiveOptions_.confirmCloseMode,
                              pane.isRunning(), pane.hasActiveProcess());
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
        if (findNode(tabs_[static_cast<size_t>(index)]->root.get(), paneId)
            != nullptr) {
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
