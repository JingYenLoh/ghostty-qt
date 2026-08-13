#pragma once

#include "application_action.h"
#include "ghostty_keybind_set.h"
#include "launch_options.h"
#include "revision_counter.h"
#include "tab_list_model.h"
#include "terminal_desktop_notification.h"
#include "terminal_types.h"
#include "window_navigation_action.h"
#include "workspace_action.h"

#include <QColor>
#include <QHash>
#include <QMetaObject>
#include <QPointF>
#include <QPointer>
#include <QQmlComponent>
#include <QQuickItem>
#include <QSet>
#include <QStringList>
#include <QStringView>
#include <QVector>
#include <QtQmlIntegration/qqmlintegration.h>

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

class GhosttyApplicationKeybindings;
class InitialSessionCoordinator;
class TerminalPane;

struct WorkspaceCloseAssessment {
    bool needsConfirmation = false;
    bool hasReadOnlyPane = false;

    WorkspaceCloseAssessment &
    operator|=(const WorkspaceCloseAssessment &other) noexcept
    {
        needsConfirmation |= other.needsConfirmation;
        hasReadOnlyPane |= other.hasReadOnlyPane;
        return *this;
    }
};

// Overrides delivered with a remote new-window or new-tab request belong
// exclusively to that container's first surface. Optional QString values
// preserve the distinction between omission and an explicit empty value.
struct FirstSurfaceOverrides {
    std::optional<TerminalCommand> command;
    std::optional<GhosttyShellIntegrationMode> shellIntegration;
    std::optional<QString> workingDirectory;
    std::optional<QString> titleOverride;

    bool operator==(const FirstSurfaceOverrides &) const = default;
};

struct WorkspaceSurfaceSnapshot {
    SurfaceId surfaceId;
    PaneId paneId;
    std::optional<QString> effectiveTitle;
    QString currentDirectory;

    bool operator==(const WorkspaceSurfaceSnapshot &) const = default;
};

class TerminalWorkspace : public QQuickItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(TerminalWorkspace)
    Q_PROPERTY(QAbstractItemModel *tabModel READ qmlTabModel CONSTANT)
    Q_PROPERTY(QStringList tabTitles READ tabTitles NOTIFY tabTitlesChanged)
    Q_PROPERTY(
        QString currentTitle READ currentTitle NOTIFY currentTitleChanged)
    Q_PROPERTY(QString currentSubtitle READ currentSubtitle NOTIFY
                   currentSubtitleChanged)
    Q_PROPERTY(QColor chromeBackground READ chromeBackground NOTIFY
                   windowAppearanceChanged)
    Q_PROPERTY(QColor chromeForeground READ chromeForeground NOTIFY
                   windowAppearanceChanged)
    Q_PROPERTY(bool platformChromePalette READ usesPlatformChromePalette NOTIFY
                   windowAppearanceChanged)
    Q_PROPERTY(QString titleFontFamily READ titleFontFamily NOTIFY
                   windowAppearanceChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY
                   currentIndexChanged)
    Q_PROPERTY(int tabCount READ tabCount NOTIFY tabTitlesChanged)
    Q_PROPERTY(
        bool tabBarVisible READ tabBarVisible NOTIFY tabBarVisibleChanged)
    Q_PROPERTY(
        bool tabBarAtBottom READ tabBarAtBottom NOTIFY tabsLocationChanged)
    Q_PROPERTY(bool wideTabs READ wideTabs NOTIFY wideTabsChanged)
    Q_PROPERTY(QQmlComponent *inspectorComponent READ inspectorComponent WRITE
                   setInspectorComponent NOTIFY inspectorComponentChanged)
    Q_PROPERTY(
        QQmlComponent *searchOverlayComponent READ searchOverlayComponent WRITE
            setSearchOverlayComponent NOTIFY searchOverlayComponentChanged)
    Q_PROPERTY(QQmlComponent *keyStateOverlayComponent READ
                   keyStateOverlayComponent WRITE setKeyStateOverlayComponent
                       NOTIFY keyStateOverlayComponentChanged)
    Q_PROPERTY(
        QQmlComponent *abnormalExitOverlayComponent READ
            abnormalExitOverlayComponent WRITE setAbnormalExitOverlayComponent
                NOTIFY abnormalExitOverlayComponentChanged)
    Q_PROPERTY(QQmlComponent *readOnlyOverlayComponent READ
                   readOnlyOverlayComponent WRITE setReadOnlyOverlayComponent
                       NOTIFY readOnlyOverlayComponentChanged)
    Q_PROPERTY(
        QQmlComponent *resizeOverlayComponent READ resizeOverlayComponent WRITE
            setResizeOverlayComponent NOTIFY resizeOverlayComponentChanged)
    Q_PROPERTY(QQmlComponent *progressOverlayComponent READ
                   progressOverlayComponent WRITE setProgressOverlayComponent
                       NOTIFY progressOverlayComponentChanged)
    Q_PROPERTY(QQmlComponent *scrollbarComponent READ scrollbarComponent WRITE
                   setScrollbarComponent NOTIFY scrollbarComponentChanged)
    Q_PROPERTY(QQmlComponent *bellBorderComponent READ bellBorderComponent WRITE
                   setBellBorderComponent NOTIFY bellBorderComponentChanged)
    Q_PROPERTY(
        QQmlComponent *customShaderStageComponent READ
            customShaderStageComponent WRITE setCustomShaderStageComponent
                NOTIFY customShaderStageComponentChanged)

public:
    using SurfaceIdAllocator = std::function<SurfaceId()>;
    using TabTransferHandler = std::function<bool(PaneId)>;

    explicit TerminalWorkspace(QQuickItem *parent = nullptr);
    ~TerminalWorkspace() override;

    static void setDefaultLaunchOptions(const LaunchOptions &options);
    // QML constructs the item before the process controller can supply the
    // per-window launch request. Initialization is one-shot and creates the
    // first tab only after those options and QML components are ready.
    bool initialize(const LaunchOptions &options,
                    TerminalSessionStartMode initialSessionStartMode =
                        TerminalSessionStartMode::Immediate,
                    std::shared_ptr<InitialSessionCoordinator>
                        initialSessionCoordinator = {},
                    FirstSurfaceOverrides firstSurfaceOverrides = {});
    bool initialize(
        const LaunchOptions &options,
        TerminalSessionStartMode initialSessionStartMode,
        std::shared_ptr<InitialSessionCoordinator> initialSessionCoordinator,
        GhosttyKeybindProgram keybindProgram,
        FirstSurfaceOverrides firstSurfaceOverrides = {});
    // The process owner installs this before initialization so every child
    // receives a stable, application-routable GHOSTTY_SURFACE_ID before its
    // terminal session starts.
    void setSurfaceIdAllocator(SurfaceIdAllocator allocator);
    void setTabTransferHandler(TabTransferHandler handler);
    bool initializeEmpty(
        const LaunchOptions &options,
        std::shared_ptr<InitialSessionCoordinator> initialSessionCoordinator,
        GhosttyKeybindProgram keybindProgram);
    [[nodiscard]] bool armInitialSessionStart();
    void applyLaunchOptions(const LaunchOptions &options);
    void applyLaunchOptions(const LaunchOptions &options,
                            GhosttyKeybindProgram keybindProgram);

    QStringList tabTitles() const;
    QString currentTitle() const;
    QString currentSubtitle() const;
    QColor chromeBackground() const;
    QColor chromeForeground() const;
    [[nodiscard]] bool usesPlatformChromePalette() const;
    QString titleFontFamily() const;
    const TabListModel *tabModel() const { return &tabModel_; }
    int currentIndex() const { return currentIndex_; }
    int tabCount() const { return static_cast<int>(tabs_.size()); }
    bool tabBarVisible() const;
    bool tabBarAtBottom() const
    {
        return effectiveOptions_.tabsLocation == TabsLocation::Bottom;
    }
    bool wideTabs() const { return effectiveOptions_.wideTabs; }
    [[nodiscard]] const LaunchOptions &effectiveLaunchOptions() const
    {
        return effectiveOptions_;
    }
    [[nodiscard]] WindowDecorationMode windowDecoration() const noexcept
    {
        return windowDecorationOverride_.value_or(
            effectiveOptions_.windowDecoration);
    }
    [[nodiscard]] const GhosttyKeybindProgram &keybindProgram() const noexcept
    {
        return keybindProgram_;
    }
    [[nodiscard]] std::optional<LaunchOptions>
    newWindowLaunchOptions(const LaunchOptions &applicationOptions,
                           PaneId sourcePaneId = {}) const;
    [[nodiscard]] WorkspaceCloseAssessment closeAssessment() const;
    [[nodiscard]] bool hasActivePane() const;
    bool focusActivePane();
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
    QQmlComponent *inspectorComponent() const
    {
        return inspector_.component.data();
    }
    void setInspectorComponent(QQmlComponent *component);
    QQmlComponent *searchOverlayComponent() const
    {
        return searchOverlay_.component.data();
    }
    void setSearchOverlayComponent(QQmlComponent *component);
    QQmlComponent *keyStateOverlayComponent() const
    {
        return keyStateOverlay_.component.data();
    }
    void setKeyStateOverlayComponent(QQmlComponent *component);
    QQmlComponent *abnormalExitOverlayComponent() const
    {
        return abnormalExitOverlay_.component.data();
    }
    void setAbnormalExitOverlayComponent(QQmlComponent *component);
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
    QQmlComponent *progressOverlayComponent() const
    {
        return progressOverlay_.component.data();
    }
    void setProgressOverlayComponent(QQmlComponent *component);
    QQmlComponent *scrollbarComponent() const
    {
        return scrollbar_.component.data();
    }
    void setScrollbarComponent(QQmlComponent *component);
    QQmlComponent *bellBorderComponent() const
    {
        return bellBorder_.component.data();
    }
    void setBellBorderComponent(QQmlComponent *component);
    QQmlComponent *customShaderStageComponent() const
    {
        return customShaderStageComponent_.data();
    }
    void setCustomShaderStageComponent(QQmlComponent *component);

    bool dispatchAction(const WorkspaceActionRequest &request);
    bool executeSurfaceActionOnAllPanes(QStringView action);
    bool executeSurfaceActionOnAllPanes(const GhosttyConfiguredAction &action);
    [[nodiscard]] bool containsPane(PaneId paneId) const;
    [[nodiscard]] bool focusPaneForFrontend(PaneId paneId);
    [[nodiscard]] bool
    controlInspector(PaneId paneId,
                     WorkspaceFrontendActions::InspectorMode mode);
    [[nodiscard]] bool requestIoCrash(PaneId paneId);
    // Samples committed GUI-thread state and exposes stable identities rather
    // than pane pointers to process-owned consumers such as the palette.
    [[nodiscard]] QVector<WorkspaceSurfaceSnapshot> surfaceSnapshot() const;
    [[nodiscard]] SurfaceId surfaceIdForPane(PaneId paneId) const;
    [[nodiscard]] bool
    createApplicationTab(PaneId sourcePaneId,
                         FirstSurfaceOverrides firstSurfaceOverrides);
    [[nodiscard]] bool transferTabTo(TerminalWorkspace *destination,
                                     PaneId sourcePaneId);
    [[nodiscard]] QString customShaderDiagnostics() const;

    Q_INVOKABLE void setCurrentIndex(int index);
    Q_INVOKABLE bool activateTabByStableId(quint64 tabId);
    Q_INVOKABLE bool executeActiveConfiguredAction(const QString &action);
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
    Q_INVOKABLE void confirmClipboardWrite(quint64 confirmationId,
                                           bool remember);
    Q_INVOKABLE void cancelClipboardWrite(quint64 confirmationId,
                                          bool remember);
    Q_INVOKABLE void confirmTitlePrompt(quint64 promptId, const QString &title);
    Q_INVOKABLE void cancelTitlePrompt(quint64 promptId);
    Q_INVOKABLE bool executeContextMenuAction(quint64 requestId,
                                              const QString &action);
    Q_INVOKABLE void finishContextMenu(quint64 requestId);

Q_SIGNALS:
    void tabTitlesChanged();
    void currentTitleChanged();
    void currentSubtitleChanged();
    void windowAppearanceChanged();
    void tabBarVisibleChanged();
    void tabsLocationChanged();
    void wideTabsChanged();
    void currentIndexChanged();
    void closeConfirmationRequested(quint64 confirmationId,
                                    const QString &message);
    void closeConfirmationResolved(quint64 confirmationId);
    void unsafePasteConfirmationRequested(quint64 confirmationId,
                                          const QString &preview);
    void unsafePasteConfirmationResolved(quint64 confirmationId);
    void terminalClipboardWriteConfirmationRequested(quint64 confirmationId,
                                                     const QString &preview);
    void terminalClipboardWriteConfirmationResolved(quint64 confirmationId);
    void titlePromptRequested(quint64 promptId, const QString &heading,
                              const QString &initialTitle);
    void titlePromptResolved(quint64 promptId);
    void contextMenuRequested(quint64 requestId, const QPointF &windowPosition,
                              bool selectionAvailable);
    void contextMenuCancelled(quint64 requestId);
    void applicationActionRequested(ApplicationAction action,
                                    PaneId sourcePaneId);
    void windowNavigationRequested(WindowNavigationAction action,
                                   PaneId sourcePaneId);
    void frontendActionRequested(const WorkspaceFrontendActionRequest &request);
    void standardClipboardCommitted(bool empty);
    // Stable pane identity lets the process-owned native notification service
    // route a later click without retaining a surface pointer.
    void desktopNotificationRequested(
        PaneId sourcePaneId, const TerminalDesktopNotification &notification);
    // Process-wide broad dispatch mirrors Ghostty's append-on-create,
    // swap-remove surface vector. These fire only after a pane gains a stable
    // tree identity and immediately after that identity is retired.
    void paneCommitted(PaneId paneId, TerminalPane *pane);
    void paneRemoved(PaneId paneId, TerminalPane *pane);
    // A live surface retained its identity while its containing tab moved to
    // another registered workspace. Process-owned registries update this
    // target in place rather than applying creation/removal ordering.
    void paneTransferred(PaneId paneId, TerminalPane *pane, SurfaceId surfaceId,
                         TerminalWorkspace *destination);
    void broadActionsRequested(const GhosttyCompiledActionChain &actions);
    void workspaceActivated();
    void toggleFullscreenRequested();
    void toggleMaximizeRequested();
    void windowAttentionRequested();
    void windowDecorationChanged();
    void windowCloseApproved();
    void applicationQuitApproved();
    void applicationQuitCancelled();
    void inspectorComponentChanged();
    void searchOverlayComponentChanged();
    void keyStateOverlayComponentChanged();
    void abnormalExitOverlayComponentChanged();
    void readOnlyOverlayComponentChanged();
    void resizeOverlayComponentChanged();
    void progressOverlayComponentChanged();
    void scrollbarComponentChanged();
    void bellBorderComponentChanged();
    void customShaderStageComponentChanged();
    void customShaderDiagnosticsChanged(const QString &diagnostics);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void geometryChange(const QRectF &newGeometry,
                        const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    friend class GhosttyApplicationKeybindings;

    QAbstractItemModel *qmlTabModel() { return &tabModel_; }

    struct Node;
    struct Tab;
    struct PaneHandle;
    struct BroadPaneTarget {
        PaneId paneId;
        QPointer<TerminalPane> pane;
    };
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
    struct PendingClipboardWrite {
        TerminalClipboardWriteRequest request;
        PaneId paneId;
        QPointer<TerminalPane> pane;
        quint64 byteSize = 0;
    };
    struct PendingContextMenu {
        quint64 requestId = 0;
        PaneId paneId;
        QPointer<TerminalPane> pane;
    };
    using TitlePromptTarget = std::variant<std::monostate, PaneId, TabId>;
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
    using PendingClose = std::variant<std::monostate, PendingPaneClose,
                                      PendingTabClose, PendingWindowClose>;
    struct DeferredPaneRemoval {
        PaneId paneId;
        QPointer<TerminalPane> pane;
    };
    struct DeferredTabRemoval {
        PendingTabClose close;
        QSet<PaneId> waitingPaneIds;
    };
    using DeferredRemoval =
        std::variant<std::monostate, DeferredPaneRemoval, DeferredTabRemoval>;
    enum class PaneActivationReason {
        Direct,
        SplitNavigation,
    };

    PaneHandle createPane(
        const LaunchOptions &options,
        std::optional<TerminalSessionGeometry> initialGeometry = std::nullopt,
        TerminalSessionStartMode startMode =
            TerminalSessionStartMode::Immediate,
        std::optional<TerminalCommand> firstSessionCommandOverride =
            std::nullopt,
        std::optional<QString> surfaceTitleOverride = std::nullopt);
    void bindPane(PaneHandle handle);
    void setPaneOverlayComponent(PaneOverlaySlot &slot,
                                 QQmlComponent *component,
                                 const char *paneProperty,
                                 const char *description,
                                 void (TerminalWorkspace::*changedSignal)());
    [[nodiscard]] bool attachPaneOverlays(TerminalPane *pane);
    template <typename Visitor> void forEachPane(Visitor &&visitor) const;
    [[nodiscard]] QVector<QPointer<TerminalPane>> paneSnapshot() const;
    [[nodiscard]] QVector<BroadPaneTarget> broadPaneSnapshot() const;
    [[nodiscard]] bool
    broadPaneTargetIsLive(const BroadPaneTarget &target) const;
    [[nodiscard]] bool
    executeBroadSurfaceAction(const BroadPaneTarget &target,
                              const GhosttyConfiguredAction &action);
    bool executeAction(const WorkspaceActionRequest &request);
    PaneHandle createNewTab(
        PaneId sourcePaneId = {},
        std::optional<TerminalSessionGeometry> initialGeometry = std::nullopt,
        TerminalSessionStartMode startMode =
            TerminalSessionStartMode::Immediate,
        FirstSurfaceOverrides firstSurfaceOverrides = {});
    void activateTab(TabId id,
                     std::optional<QString> previousSubtitle = std::nullopt);
    bool activateTabByIndex(qint64 oneBasedIndex);
    bool moveTab(TabId tabId, qint64 delta);
    void
    activatePane(PaneId id,
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
    [[nodiscard]] bool splitPane(PaneId paneId, Qt::Orientation orientation,
                                 bool placeNewPaneFirst);
    void closePane(PaneId paneId, bool force = false);
    void closeTab(TabId tabId, CloseTabMode mode = CloseTabMode::This,
                  bool force = false);
    void closeTabs(PendingTabClose close, bool force = false);
    std::vector<TabId> closeTabTargets(TabId tabId, CloseTabMode mode) const;
    void removeTab(TabId tabId);
    void removeTabs(PendingTabClose close);
    void commitPaneRemoval(PaneId paneId, QPointer<TerminalPane> pane);
    void commitTabRemoval(PendingTabClose close);
    void finishDeferredPaneTransition(PaneId paneId);
    void finishDeferredRemovalNow();
    void finishWindowPaneTransition(PaneId paneId);
    void refreshTab(TabId tabId);
    void updateSplitMembership(Tab &tab);
    TabListEntry tabListEntry(const Tab &tab) const;
    void layoutCurrentTab();
    void updateNodeGeometry(Node *node, const QRectF &geometry);
    void applyNodeGeometry(Node *node);
    void updateSplitDividers(const Tab *tab);
    void updateSplitDividers(Node *node, quint64 generation);
    Node *findSplitNode(Node *node, quint64 splitId) const;
    [[nodiscard]] std::optional<SplitDividerDrag>
    beginSplitDividerDrag(quint64 splitId, const QPointF &position) const;
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
    void toggleWindowDecorations();
    bool findNodePath(Node *node, PaneId paneId,
                      std::vector<Node *> *path) const;
    WorkspaceCloseAssessment assessTabClose(const Tab &tab) const;
    WorkspaceCloseAssessment
    assessTabsClose(const std::vector<TabId> &tabIds) const;
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
    void
    beginTerminalClipboardWrite(const TerminalClipboardWriteRequest &request,
                                PaneHandle handle);
    void finishTerminalClipboardWrite(quint64 confirmationId, bool confirmed,
                                      bool remember);
    void removePendingClipboardWritesForPane(PaneHandle handle);
    bool commitTerminalClipboardWrite(const TerminalClipboardWrite &write);
    void schedulePendingClipboardWriteDrain();
    void drainPendingClipboardWrites();
    static QString
    terminalClipboardWritePreview(const TerminalClipboardWrite &write);
    void beginContextMenu(PaneHandle handle, const QPointF &windowPosition,
                          bool selectionAvailable);
    void removeContextMenuForPane(PaneHandle handle);
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
    // Ghostty's runtime toggle belongs to the containing top-level, not an
    // individual pane. A present override masks live config reloads until the
    // next toggle clears it and reveals the newest configured value.
    std::optional<WindowDecorationMode> windowDecorationOverride_;
    std::optional<QString> windowTitleOverride_;
    GhosttyKeybindProgram keybindProgram_;
    RevisionCounter launchOptionsRevision_;
    std::shared_ptr<InitialSessionCoordinator> initialSessionCoordinator_;
    TabListModel tabModel_;
    std::vector<std::unique_ptr<Tab>> tabs_;
    QVector<QPointer<TerminalPane>> pendingPanes_;
    // A pane stops being an action/clipboard target as soon as its close
    // commits, including while synchronous model observers can still see its
    // tree node during a batched removal.
    QSet<PaneId> retiringPaneIds_;
    QHash<PaneId, SurfaceId> surfaceIds_;
    int currentIndex_ = -1;
    quint64 nextTabId_ = 1;
    quint64 nextPaneId_ = 1;
    SurfaceIdAllocator surfaceIdAllocator_;
    TabTransferHandler tabTransferHandler_;
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
    DeferredRemoval deferredRemoval_;
    QSet<PaneId> pendingWindowExitPaneIds_;
    quint64 nextCloseConfirmationId_ = 0;
    QVector<PendingPaste> pendingPastes_;
    bool pendingPastePreviewScheduled_ = false;
    quint64 nextPasteConfirmationId_ = 0;
    quint64 activePasteConfirmationId_ = 0;
    std::deque<PendingClipboardWrite> pendingClipboardWrites_;
    quint64 pendingClipboardWriteBytes_ = 0;
    bool pendingClipboardWriteDrainScheduled_ = false;
    quint64 nextClipboardWriteConfirmationId_ = 0;
    quint64 activeClipboardWriteConfirmationId_ = 0;
    std::optional<PendingContextMenu> pendingContextMenu_;
    quint64 nextContextMenuId_ = 0;
    std::deque<PendingTitlePrompt> pendingTitlePrompts_;
    std::optional<PendingTitlePrompt> activeTitlePrompt_;
    quint64 nextTitlePromptId_ = 0;
    bool titlePromptAdvanceScheduled_ = false;
    bool broadActionFanout_ = false;
    bool topologyMutation_ = false;
    PaneOverlaySlot inspector_;
    PaneOverlaySlot searchOverlay_;
    PaneOverlaySlot keyStateOverlay_;
    PaneOverlaySlot abnormalExitOverlay_;
    PaneOverlaySlot readOnlyOverlay_;
    PaneOverlaySlot resizeOverlay_;
    PaneOverlaySlot progressOverlay_;
    PaneOverlaySlot scrollbar_;
    PaneOverlaySlot bellBorder_;
    QPointer<QQmlComponent> customShaderStageComponent_;
    QMetaObject::Connection customShaderStageDestructionConnection_;
    QHash<PaneId, QString> customShaderDiagnostics_;
};
