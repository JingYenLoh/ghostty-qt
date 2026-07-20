#pragma once

#include "launch_options.h"
#include "tab_list_model.h"
#include "workspace_action.h"

#include <QHash>
#include <QQuickItem>
#include <QStringList>
#include <QStringView>
#include <QVector>

#include <memory>
#include <optional>
#include <vector>

class QQmlComponent;
class TerminalPane;

class TerminalWorkspace : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(TabListModel *tabModel READ tabModel CONSTANT)
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

public:
    explicit TerminalWorkspace(QQuickItem *parent = nullptr);
    ~TerminalWorkspace() override;

    static void setDefaultLaunchOptions(const LaunchOptions &options);
    void applyConfigSnapshot(const GhosttyConfigSnapshot &snapshot);

    QStringList tabTitles() const;
    QString currentTitle() const;
    TabListModel *tabModel() { return &tabModel_; }
    int currentIndex() const { return currentIndex_; }
    int tabCount() const { return static_cast<int>(tabs_.size()); }
    bool tabBarVisible() const;
    QQmlComponent *searchOverlayComponent() const
    {
        return searchOverlayComponent_;
    }
    void setSearchOverlayComponent(QQmlComponent *component);
    QQmlComponent *readOnlyOverlayComponent() const
    {
        return readOnlyOverlayComponent_;
    }
    void setReadOnlyOverlayComponent(QQmlComponent *component);

    bool dispatchAction(const WorkspaceActionRequest &request);
    bool executeApplicationConfiguredAction(QStringView action);
    bool executeSurfaceActionOnAllPanes(QStringView action);

    Q_INVOKABLE void setCurrentIndex(int index);
    Q_INVOKABLE void newTab();
    Q_INVOKABLE void closeCurrentTab();
    Q_INVOKABLE void splitRight();
    Q_INVOKABLE void splitDown();
    Q_INVOKABLE void closeActivePane();
    Q_INVOKABLE void requestQuit();
    Q_INVOKABLE void confirmClose();
    Q_INVOKABLE void cancelClose();
    Q_INVOKABLE void confirmPaste(quint64 confirmationId);
    Q_INVOKABLE void cancelPaste(quint64 confirmationId);
    Q_INVOKABLE void confirmTabTitlePrompt(quint64 promptId,
                                           const QString &title);
    Q_INVOKABLE void cancelTabTitlePrompt(quint64 promptId);

Q_SIGNALS:
    void tabTitlesChanged();
    void currentTitleChanged();
    void tabBarVisibleChanged();
    void currentIndexChanged();
    void closeConfirmationRequested(const QString &message);
    void closeConfirmationResolved();
    void unsafePasteConfirmationRequested(quint64 confirmationId,
                                          const QString &preview);
    void unsafePasteConfirmationResolved(quint64 confirmationId);
    void tabTitlePromptRequested(quint64 promptId,
                                 const QString &initialTitle);
    void tabTitlePromptResolved(quint64 promptId);
    void configReloadRequested();
    void broadActionsRequested(const QStringList &actions);
    void toggleFullscreenRequested();
    void quitApproved();
    void searchOverlayComponentChanged();
    void readOnlyOverlayComponentChanged();

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    struct Node;
    struct Tab;
    struct PaneHandle;
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
    struct PendingTabTitlePrompt {
        quint64 requestId = 0;
        TabId tabId;
        QString initialTitle;
    };
    enum class PendingClose {
        None,
        Pane,
        Tab,
        Quit,
    };
    enum class PaneActivationReason {
        Direct,
        SplitNavigation,
    };

    PaneHandle createPane(const LaunchOptions &options);
    void createSearchOverlay(TerminalPane *pane);
    void createReadOnlyOverlay(TerminalPane *pane);
    bool executeAction(const WorkspaceActionRequest &request);
    void createNewTab(PaneId sourcePaneId = {});
    void activateTab(TabId id);
    bool activateTabByIndex(qint64 oneBasedIndex);
    bool moveTab(TabId tabId, qint64 delta);
    void activatePane(
        PaneId id,
        PaneActivationReason reason = PaneActivationReason::Direct);
    void requestQuitImpl();
    void approveQuit();
    void resolvePendingPaneRemoval(PaneHandle handle);
    void resolvePendingTabRemoval(TabId tabId);
    void reevaluatePendingClose();
    Tab *currentTab();
    const Tab *currentTab() const;
    Tab *tabById(TabId id);
    const Tab *tabById(TabId id) const;
    TabId currentTabId() const;
    PaneId currentPaneId() const;
    void splitPane(PaneId paneId, Qt::Orientation orientation,
                   bool placeNewPaneFirst);
    void closePane(PaneId paneId, bool force = false);
    void closeTab(TabId tabId, bool force = false);
    void removeTab(TabId tabId);
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
    TerminalPane *paneForId(PaneId paneId) const;
    bool navigateFrom(PaneId paneId, int direction);
    bool navigateRelative(PaneId paneId, qint64 delta);
    bool resizeSplit(PaneId paneId, int direction, int amount);
    bool equalizeSplits(TabId tabId);
    bool toggleSplitZoom(TabId tabId);
    bool findNodePath(Node *node, PaneId paneId,
                      std::vector<Node *> *path) const;
    bool shouldConfirmPaneClose(const TerminalPane &pane) const;
    bool tabHasReadOnlyPane(const Tab &tab) const;
    bool shouldConfirmTabClose(const Tab &tab) const;
    bool workspaceHasReadOnlyPane() const;
    bool shouldConfirmWorkspaceClose() const;
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
    bool enqueueTabTitlePrompt(TabId tabId);
    void showNextTabTitlePrompt();
    void scheduleNextTabTitlePrompt();
    void finishTabTitlePrompt(quint64 promptId,
                              const std::optional<QString> &title);
    void removeTabTitlePromptsForTab(TabId tabId);
    QString tabTitlePromptInitialValue(const Tab &tab) const;

    static LaunchOptions defaultOptions_;
    LaunchOptions effectiveOptions_;
    TabListModel tabModel_;
    WorkspaceActionDispatcher actionDispatcher_;
    std::vector<std::unique_ptr<Tab>> tabs_;
    int currentIndex_ = -1;
    quint64 nextTabId_ = 1;
    quint64 nextPaneId_ = 1;
    quint64 nextSplitId_ = 1;
    quint64 splitDividerGeneration_ = 0;
    QHash<quint64, SplitDividerItem *> splitDividers_;
    bool initialTabCreated_ = false;
    bool quitApprovedEmitted_ = false;
    PendingClose pendingClose_ = PendingClose::None;
    PaneId pendingPaneId_;
    TabId pendingTabId_;
    QVector<PendingPaste> pendingPastes_;
    bool pendingPastePreviewScheduled_ = false;
    quint64 nextPasteConfirmationId_ = 0;
    quint64 activePasteConfirmationId_ = 0;
    QVector<PendingTabTitlePrompt> pendingTabTitlePrompts_;
    std::optional<PendingTabTitlePrompt> activeTabTitlePrompt_;
    quint64 nextTabTitlePromptId_ = 0;
    bool tabTitlePromptAdvanceScheduled_ = false;
    bool broadActionFanout_ = false;
    QQmlComponent *searchOverlayComponent_ = nullptr;
    QQmlComponent *readOnlyOverlayComponent_ = nullptr;
};
