#pragma once

#include "application_action.h"
#include "ghostty_keybind_set.h"
#include "launch_options.h"
#include "revision_counter.h"
#include "tab_list_model.h"
#include "workspace_action.h"

#include <QHash>
#include <QMetaObject>
#include <QPointer>
#include <QQuickItem>
#include <QQmlComponent>
#include <QStringList>
#include <QStringView>
#include <QVector>
#include <QtQmlIntegration/qqmlintegration.h>

#include <deque>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

class InitialSessionCoordinator;
class TerminalPane;

struct WorkspaceCloseAssessment {
    bool needsConfirmation = false;
    bool hasReadOnlyPane = false;

    WorkspaceCloseAssessment &operator|=(
        const WorkspaceCloseAssessment &other) noexcept
    {
        needsConfirmation |= other.needsConfirmation;
        hasReadOnlyPane |= other.hasReadOnlyPane;
        return *this;
    }
};

class TerminalWorkspace : public QQuickItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(TerminalWorkspace)
    Q_PROPERTY(QAbstractItemModel *tabModel READ qmlTabModel CONSTANT)
    Q_PROPERTY(QStringList tabTitles READ tabTitles NOTIFY tabTitlesChanged)
    Q_PROPERTY(QString currentTitle READ currentTitle NOTIFY currentTitleChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(int tabCount READ tabCount NOTIFY tabTitlesChanged)
    Q_PROPERTY(bool tabBarVisible READ tabBarVisible NOTIFY tabBarVisibleChanged)
    Q_PROPERTY(QQmlComponent *searchOverlayComponent
               READ searchOverlayComponent
               WRITE setSearchOverlayComponent
               NOTIFY searchOverlayComponentChanged)
    Q_PROPERTY(QQmlComponent *readOnlyOverlayComponent
               READ readOnlyOverlayComponent
               WRITE setReadOnlyOverlayComponent
               NOTIFY readOnlyOverlayComponentChanged)
    Q_PROPERTY(QQmlComponent *resizeOverlayComponent
               READ resizeOverlayComponent
               WRITE setResizeOverlayComponent
               NOTIFY resizeOverlayComponentChanged)

public:
    explicit TerminalWorkspace(QQuickItem *parent = nullptr);
    ~TerminalWorkspace() override;

    static void setDefaultLaunchOptions(const LaunchOptions &options);
    // QML constructs the item before the process controller can supply the
    // per-window launch request. Initialization is one-shot and creates the
    // first tab only after those options and QML components are ready.
    bool initialize(
        const LaunchOptions &options,
        TerminalSessionStartMode initialSessionStartMode =
            TerminalSessionStartMode::Immediate,
        std::shared_ptr<InitialSessionCoordinator>
            initialSessionCoordinator = {});
    bool initialize(
        const LaunchOptions &options,
        TerminalSessionStartMode initialSessionStartMode,
        std::shared_ptr<InitialSessionCoordinator>
            initialSessionCoordinator,
        GhosttyKeybindProgram keybindProgram);
    [[nodiscard]] bool armInitialSessionStart();
    void applyLaunchOptions(const LaunchOptions &options);
    void applyLaunchOptions(const LaunchOptions &options,
                            GhosttyKeybindProgram keybindProgram);

    QStringList tabTitles() const;
    QString currentTitle() const;
    const TabListModel *tabModel() const { return &tabModel_; }
    int currentIndex() const { return currentIndex_; }
    int tabCount() const { return static_cast<int>(tabs_.size()); }
    bool tabBarVisible() const;
    [[nodiscard]] const LaunchOptions &effectiveLaunchOptions() const
    {
        return effectiveOptions_;
    }
    [[nodiscard]] const GhosttyKeybindProgram &keybindProgram() const noexcept
    {
        return keybindProgram_;
    }
    [[nodiscard]] std::optional<LaunchOptions> newWindowLaunchOptions(
        const LaunchOptions &applicationOptions,
        PaneId sourcePaneId = {}) const;
    [[nodiscard]] WorkspaceCloseAssessment closeAssessment() const;
    void requestApplicationQuitConfirmation(
        WorkspaceCloseAssessment applicationAssessment);
    void forceShutdownForApplicationQuit();
    [[nodiscard]] bool canHostApplicationQuitConfirmation() const
    {
        return windowCloseState_ == WindowCloseState::Open;
    }
    // Publication is irrevocably in flight once direct signal delivery has
    // begun, even before every observer has returned.
    [[nodiscard]] bool isWindowCloseApprovalPublished() const
    {
        return windowCloseState_ == WindowCloseState::Publishing
            || windowCloseState_ == WindowCloseState::Published;
    }
    QQmlComponent *searchOverlayComponent() const
    {
        return searchOverlay_.component.data();
    }
    void setSearchOverlayComponent(QQmlComponent *component);
    QQmlComponent *readOnlyOverlayComponent() const
    {
        return readOnlyOverlay_.component.data();
    }
    void setReadOnlyOverlayComponent(QQmlComponent *component);
    QQmlComponent *resizeOverlayComponent() const
    {
        return resizeOverlay_.component.data();
    }
    void setResizeOverlayComponent(QQmlComponent *component);

    bool dispatchAction(const WorkspaceActionRequest &request);
    bool executeSurfaceActionOnAllPanes(QStringView action);
    bool executeSurfaceActionOnAllPanes(
        const GhosttyConfiguredAction &action);

    Q_INVOKABLE void setCurrentIndex(int index);
    Q_INVOKABLE void newTab();
    Q_INVOKABLE void closeCurrentTab();
    Q_INVOKABLE void splitRight();
    Q_INVOKABLE void splitDown();
    Q_INVOKABLE void closeActivePane();
    Q_INVOKABLE void requestWindowClose();
    Q_INVOKABLE void confirmClose(quint64 confirmationId);
    Q_INVOKABLE void cancelClose(quint64 confirmationId);
    Q_INVOKABLE void confirmPaste(quint64 confirmationId);
    Q_INVOKABLE void cancelPaste(quint64 confirmationId);
    Q_INVOKABLE void confirmTitlePrompt(quint64 promptId,
                                        const QString &title);
    Q_INVOKABLE void cancelTitlePrompt(quint64 promptId);

Q_SIGNALS:
    void tabTitlesChanged();
    void currentTitleChanged();
    void tabBarVisibleChanged();
    void currentIndexChanged();
    void closeConfirmationRequested(quint64 confirmationId,
                                    const QString &message);
    void closeConfirmationResolved(quint64 confirmationId);
    void unsafePasteConfirmationRequested(quint64 confirmationId,
                                          const QString &preview);
    void unsafePasteConfirmationResolved(quint64 confirmationId);
    void titlePromptRequested(quint64 promptId, const QString &heading,
                              const QString &initialTitle);
    void titlePromptResolved(quint64 promptId);
    void applicationActionRequested(ApplicationAction action,
                                    PaneId sourcePaneId);
    void broadActionsRequested(const GhosttyCompiledActionChain &actions);
    void workspaceActivated();
    void toggleFullscreenRequested();
    void toggleMaximizeRequested();
    void windowCloseApproved();
    void applicationQuitApproved();
    void applicationQuitCancelled();
    void searchOverlayComponentChanged();
    void readOnlyOverlayComponentChanged();
    void resizeOverlayComponentChanged();

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    QAbstractItemModel *qmlTabModel() { return &tabModel_; }

    struct Node;
    struct Tab;
    struct PaneHandle;
    struct PaneOverlaySlot {
        QPointer<QQmlComponent> component;
        QMetaObject::Connection destructionConnection;
        RevisionCounter revision;
    };
    struct SplitDividerDrag {
        qreal pointer = 0.0;
        qreal ratio = 0.0;
    };
    class SplitDividerItem;
    struct PendingPasteTarget {
        PaneId paneId;
        quint64 requestId = 0;
    };
    struct PendingPaste {
        QString text;
        QVector<PendingPasteTarget> targets;
    };
    using TitlePromptTarget = std::variant<PaneId, TabId>;
    struct PendingTitlePrompt {
        quint64 requestId = 0;
        TitlePromptTarget target;
        QString initialTitle;
    };
    struct PendingPaneClose {
        PaneId paneId;
        TabId tabId;
        quint64 requestId = 0;
    };
    struct PendingTabClose {
        TabId originTabId;
        std::vector<TabId> targets;
        quint64 requestId = 0;
    };
    enum class WindowCloseIntent {
        WindowOnly,
        QuitApplication,
    };
    enum class WindowCloseState {
        Open,
        Committed,
        Publishing,
        Published,
    };
    struct PendingWindowClose {
        quint64 requestId = 0;
    };
    using PendingClose = std::variant<
        std::monostate, PendingPaneClose, PendingTabClose,
        PendingWindowClose>;
    enum class PaneActivationReason {
        Direct,
        SplitNavigation,
    };

    PaneHandle createPane(
        const LaunchOptions &options,
        std::optional<TerminalSessionGeometry> initialGeometry = std::nullopt,
        TerminalSessionStartMode startMode =
            TerminalSessionStartMode::Immediate);
    void setPaneOverlayComponent(
        PaneOverlaySlot &slot,
        QQmlComponent *component,
        const char *paneProperty,
        const char *description,
        void (TerminalWorkspace::*changedSignal)());
    [[nodiscard]] bool attachPaneOverlays(TerminalPane *pane);
    template<typename Visitor>
    void forEachPane(Visitor &&visitor) const;
    [[nodiscard]] QVector<QPointer<TerminalPane>> paneSnapshot() const;
    bool executeAction(const WorkspaceActionRequest &request);
    PaneHandle createNewTab(
        PaneId sourcePaneId = {},
        std::optional<TerminalSessionGeometry> initialGeometry = std::nullopt,
        TerminalSessionStartMode startMode =
            TerminalSessionStartMode::Immediate);
    void activateTab(TabId id);
    bool activateTabByIndex(qint64 oneBasedIndex);
    bool moveTab(TabId tabId, qint64 delta);
    void activatePane(
        PaneId id,
        PaneActivationReason reason = PaneActivationReason::Direct);
    void requestWindowCloseImpl(WindowCloseIntent intent);
    void approveWindowClose();
    void publishWindowCloseApproval();
    void approveApplicationQuit();
    void scheduleApplicationQuitReconciliation();
    void beginCloseConfirmation(PendingClose close, const QString &message);
    quint64 pendingCloseRequestId(const PendingClose &close) const;
    PendingClose takePendingClose();
    void commitPendingClose();
    void performPendingClose(PendingClose close);
    void resolvePendingPaneRemoval(PaneHandle handle);
    void resolvePendingTabRemoval(TabId tabId);
    void reevaluatePendingClose();
    Tab *currentTab();
    const Tab *currentTab() const;
    Tab *tabById(TabId id);
    const Tab *tabById(TabId id) const;
    TabId currentTabId() const;
    PaneId currentPaneId() const;
    [[nodiscard]] bool splitPane(PaneId paneId,
                                 Qt::Orientation orientation,
                                 bool placeNewPaneFirst);
    void closePane(PaneId paneId, bool force = false);
    void closeTab(TabId tabId, CloseTabMode mode = CloseTabMode::This,
                  bool force = false);
    void closeTabs(PendingTabClose close, bool force = false);
    std::vector<TabId> closeTabTargets(TabId tabId,
                                       CloseTabMode mode) const;
    void removeTab(TabId tabId);
    void removeTabs(PendingTabClose close);
    void refreshTab(TabId tabId);
    void updateSplitMembership(Tab &tab);
    TabListEntry tabListEntry(const Tab &tab) const;
    void layoutCurrentTab();
    void updateNodeGeometry(Node *node, const QRectF &geometry);
    void applyNodeGeometry(Node *node);
    void updateSplitDividers(const Tab *tab);
    void updateSplitDividers(Node *node, quint64 generation);
    Node *findSplitNode(Node *node, quint64 splitId) const;
    [[nodiscard]] std::optional<SplitDividerDrag> beginSplitDividerDrag(
        quint64 splitId, const QPointF &position) const;
    bool dragSplitDivider(quint64 splitId, const QPointF &position,
                          const SplitDividerDrag &drag);
    void setSplitRatio(Tab &tab, Node &split, qreal ratio);
    void updateTabVisibility(Tab &tab, bool visible);
    void setNodeVisibility(Node *node, bool visible);
    Node *findNode(Node *node, PaneId paneId) const;
    bool removePaneFromNode(std::unique_ptr<Node> &node, PaneId paneId);
    TerminalPane *firstPane(Node *node) const;
    PaneId firstPaneId(Node *node) const;
    PaneId focusTargetAfterClosing(const Tab &tab, PaneId paneId) const;
    TerminalPane *paneForId(PaneId paneId) const;
    bool navigateFrom(PaneId paneId, int direction);
    bool navigateRelative(PaneId paneId, qint64 delta);
    bool resizeSplit(PaneId paneId, int direction, int amount);
    bool equalizeSplits(TabId tabId);
    bool toggleSplitZoom(TabId tabId);
    bool findNodePath(Node *node, PaneId paneId,
                      std::vector<Node *> *path) const;
    WorkspaceCloseAssessment assessTabClose(const Tab &tab) const;
    WorkspaceCloseAssessment assessTabsClose(
        const std::vector<TabId> &tabIds) const;
    WorkspaceCloseAssessment assessWorkspaceClose() const;
    bool shouldConfirmPaneClose(const TerminalPane &pane) const;
    int tabIndexForId(TabId tabId) const;
    int tabIndexForPane(PaneId paneId) const;
    TabId tabIdForPane(PaneId paneId) const;
    bool changeTabRelativeImpl(int delta, TabId origin = {});
    void beginUnsafePaste(quint64 requestId, const QString &text,
                          PaneId paneId);
    void finishPendingPaste(quint64 confirmationId, bool confirmed);
    void removePendingPastesForPane(PaneHandle handle);
    void schedulePendingPastePreview();
    void showPendingPastePreview();
    static QString pastePreview(const QString &text);
    bool enqueueTitlePrompt(PaneId paneId);
    bool enqueueTitlePrompt(TabId tabId);
    bool enqueueTitlePrompt(TitlePromptTarget target, QString initialTitle);
    void showNextTitlePrompt();
    void scheduleNextTitlePrompt();
    void finishTitlePrompt(quint64 promptId,
                           const std::optional<QString> &title);
    void removeTitlePrompts(TitlePromptTarget target);
    bool titlePromptTargetExists(const TitlePromptTarget &target) const;
    QString tabTitlePromptInitialValue(const Tab &tab) const;

    static LaunchOptions defaultOptions_;
    LaunchOptions effectiveOptions_;
    GhosttyKeybindProgram keybindProgram_;
    RevisionCounter launchOptionsRevision_;
    std::shared_ptr<InitialSessionCoordinator> initialSessionCoordinator_;
    TabListModel tabModel_;
    std::vector<std::unique_ptr<Tab>> tabs_;
    QVector<QPointer<TerminalPane>> pendingPanes_;
    int currentIndex_ = -1;
    quint64 nextTabId_ = 1;
    quint64 nextPaneId_ = 1;
    quint64 nextSplitId_ = 1;
    quint64 splitDividerGeneration_ = 0;
    QHash<quint64, SplitDividerItem *> splitDividers_;
    bool initialTabCreated_ = false;
    QPointer<TerminalPane> deferredInitialPane_;
    PaneId deferredInitialPaneId_;
    bool initialized_ = false;
    WindowCloseState windowCloseState_ = WindowCloseState::Open;
    bool applicationQuitRequested_ = false;
    bool applicationQuitApprovedEmitted_ = false;
    bool applicationQuitReconciliationScheduled_ = false;
    bool forceApplicationQuitShutdownScheduled_ = false;
    std::optional<WorkspaceCloseAssessment> applicationQuitAssessment_;
    PendingClose pendingClose_;
    quint64 nextCloseConfirmationId_ = 0;
    QVector<PendingPaste> pendingPastes_;
    bool pendingPastePreviewScheduled_ = false;
    quint64 nextPasteConfirmationId_ = 0;
    quint64 activePasteConfirmationId_ = 0;
    std::deque<PendingTitlePrompt> pendingTitlePrompts_;
    std::optional<PendingTitlePrompt> activeTitlePrompt_;
    quint64 nextTitlePromptId_ = 0;
    bool titlePromptAdvanceScheduled_ = false;
    bool broadActionFanout_ = false;
    bool topologyMutation_ = false;
    PaneOverlaySlot searchOverlay_;
    PaneOverlaySlot readOnlyOverlay_;
    PaneOverlaySlot resizeOverlay_;
};
