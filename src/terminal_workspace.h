#pragma once

#include "launch_options.h"

#include <QPointer>
#include <QQuickItem>
#include <QStringList>

#include <memory>
#include <vector>

class TerminalPane;

class TerminalWorkspace : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QStringList tabTitles READ tabTitles NOTIFY tabTitlesChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(int tabCount READ tabCount NOTIFY tabTitlesChanged)

public:
    explicit TerminalWorkspace(QQuickItem *parent = nullptr);
    ~TerminalWorkspace() override;

    static void setDefaultLaunchOptions(const LaunchOptions &options);

    QStringList tabTitles() const;
    int currentIndex() const { return currentIndex_; }
    int tabCount() const { return static_cast<int>(tabs_.size()); }

    Q_INVOKABLE void setCurrentIndex(int index);
    Q_INVOKABLE void newTab();
    Q_INVOKABLE void closeCurrentTab();
    Q_INVOKABLE void splitRight();
    Q_INVOKABLE void splitDown();
    Q_INVOKABLE void closeActivePane();
    Q_INVOKABLE void requestQuit();
    Q_INVOKABLE void confirmClose();
    Q_INVOKABLE void cancelClose();
    Q_INVOKABLE void confirmPaste();
    Q_INVOKABLE void cancelPaste();

Q_SIGNALS:
    void tabTitlesChanged();
    void currentIndexChanged();
    void closeConfirmationRequested(const QString &message);
    void unsafePasteConfirmationRequested(const QString &preview);
    void quitApproved();

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    struct Node;
    struct Tab;
    enum class PendingClose {
        None,
        Pane,
        Tab,
        Quit,
    };

    TerminalPane *createPane(const LaunchOptions &options);
    Tab *currentTab();
    const Tab *currentTab() const;
    void splitActive(Qt::Orientation orientation);
    void setActivePane(TerminalPane *pane);
    void closePane(TerminalPane *pane, bool force = false);
    void removeTab(int index);
    void layoutCurrentTab();
    void layoutNode(Node *node, const QRectF &geometry);
    void setNodeVisibility(Node *node, bool visible);
    Node *findNode(Node *node, TerminalPane *pane) const;
    bool removePaneFromNode(std::unique_ptr<Node> &node, TerminalPane *pane);
    TerminalPane *firstPane(Node *node) const;
    void collectPanes(Node *node, std::vector<TerminalPane *> *panes) const;
    void navigateFrom(TerminalPane *pane, int direction);
    bool hasRunningProcesses() const;
    int tabIndexForPane(TerminalPane *pane) const;
    void changeTabRelative(int delta);
    void beginUnsafePaste(const QString &text, TerminalPane *pane);

    static LaunchOptions defaultOptions_;
    std::vector<std::unique_ptr<Tab>> tabs_;
    int currentIndex_ = -1;
    bool initialTabCreated_ = false;
    PendingClose pendingClose_ = PendingClose::None;
    QPointer<TerminalPane> pendingPane_;
    int pendingTabIndex_ = -1;
    QString pendingPaste_;
    QPointer<TerminalPane> pendingPastePane_;
};
