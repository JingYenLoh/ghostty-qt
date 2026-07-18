#pragma once

#include "launch_options.h"
#include "tab_list_model.h"
#include "workspace_action.h"

#include <QQuickItem>
#include <QStringList>
#include <QStringView>
#include <QVector>

#include <memory>
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
    Q_PROPERTY(QQmlComponent *searchOverlayComponent
               READ searchOverlayComponent
               WRITE setSearchOverlayComponent
               NOTIFY searchOverlayComponentChanged)

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
    QQmlComponent *searchOverlayComponent() const
    {
        return searchOverlayComponent_;
    }
    void setSearchOverlayComponent(QQmlComponent *component);

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

Q_SIGNALS:
    void tabTitlesChanged();
    void currentTitleChanged();
    void currentIndexChanged();
    void closeConfirmationRequested(const QString &message);
    void closeConfirmationResolved();
    void unsafePasteConfirmationRequested(quint64 confirmationId,
                                          const QString &preview);
    void unsafePasteConfirmationResolved(quint64 confirmationId);
    void configReloadRequested();
    void broadActionsRequested(const QStringList &actions);
    void toggleFullscreenRequested();
    void quitApproved();
    void searchOverlayComponentChanged();

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    struct Node;
    struct Tab;
    struct PaneHandle;
    struct PendingPasteTarget {
        PaneId paneId;
        quint64 requestId = 0;
    };
    struct PendingPaste {
        QString text;
        QVector<PendingPasteTarget> targets;
    };
    enum class PendingClose {
        None,
        Pane,
        Tab,
        Quit,
    };

    PaneHandle createPane(const LaunchOptions &options);
    void createSearchOverlay(TerminalPane *pane);
    bool executeAction(const WorkspaceActionRequest &request);
    void createNewTab();
    void activateTab(TabId id);
    bool activateTabByIndex(qint64 oneBasedIndex);
    bool moveTab(TabId tabId, qint64 delta);
    void activatePane(PaneId id);
    void requestQuitImpl();
    void approveQuit();
    void resolvePendingPaneRemoval(PaneId paneId);
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
    TabListEntry tabListEntry(const Tab &tab) const;
    void layoutCurrentTab();
    void updateNodeGeometry(Node *node, const QRectF &geometry);
    void applyNodeGeometry(Node *node);
    void updateTabVisibility(Tab &tab, bool visible);
    void setNodeVisibility(Node *node, bool visible);
    Node *findNode(Node *node, PaneId paneId) const;
    bool removePaneFromNode(std::unique_ptr<Node> &node, PaneId paneId);
    TerminalPane *firstPane(Node *node) const;
    PaneId firstPaneId(Node *node) const;
    TerminalPane *paneForId(PaneId paneId) const;
    void collectPanes(Node *node, std::vector<TerminalPane *> *panes) const;
    bool navigateFrom(PaneId paneId, int direction);
    bool navigateRelative(PaneId paneId, qint64 delta);
    bool resizeSplit(PaneId paneId, int direction, int amount);
    bool equalizeSplits(TabId tabId);
    bool toggleSplitZoom(TabId tabId);
    bool findNodePath(Node *node, PaneId paneId,
                      std::vector<Node *> *path) const;
    static int equalizeWeight(const Node *node, Qt::Orientation orientation);
    static void equalizeNode(Node *node);
    bool shouldConfirmTabClose(const Tab &tab) const;
    bool shouldConfirmWorkspaceClose() const;
    int tabIndexForId(TabId tabId) const;
    int tabIndexForPane(PaneId paneId) const;
    TabId tabIdForPane(PaneId paneId) const;
    PaneId paneIdForPane(TerminalPane *pane) const;
    bool changeTabRelativeImpl(int delta, TabId origin = {});
    void beginUnsafePaste(quint64 requestId, const QString &text,
                          PaneId paneId);
    void finishPendingPaste(quint64 confirmationId, bool confirmed);
    void removePendingPastesForPane(PaneId paneId);
    void schedulePendingPastePreview();
    void showPendingPastePreview();
    static QString pastePreview(const QString &text);

    static LaunchOptions defaultOptions_;
    LaunchOptions effectiveOptions_;
    TabListModel tabModel_;
    WorkspaceActionDispatcher actionDispatcher_;
    std::vector<std::unique_ptr<Tab>> tabs_;
    int currentIndex_ = -1;
    quint64 nextTabId_ = 1;
    quint64 nextPaneId_ = 1;
    bool initialTabCreated_ = false;
    bool quitApprovedEmitted_ = false;
    PendingClose pendingClose_ = PendingClose::None;
    PaneId pendingPaneId_;
    TabId pendingTabId_;
    QVector<PendingPaste> pendingPastes_;
    bool pendingPastePreviewScheduled_ = false;
    quint64 nextPasteConfirmationId_ = 0;
    quint64 activePasteConfirmationId_ = 0;
    bool broadActionFanout_ = false;
    QQmlComponent *searchOverlayComponent_ = nullptr;
};
