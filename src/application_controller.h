#pragma once

#include "application_action.h"
#include "application_lifetime.h"
#include "launch_options.h"
#include "workspace_ids.h"

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QVector>

#include <expected>
#include <functional>
#include <memory>
#include <vector>

class GhosttyApplicationKeybindings;
class QQmlEngine;
class QQuickWindow;
class TerminalWorkspace;

Q_MOC_INCLUDE("terminal_workspace.h")

struct ApplicationWindow {
    QQuickWindow *window = nullptr;
    TerminalWorkspace *workspace = nullptr;
};

// Owns process-scoped action routing and every primary terminal window. This
// object intentionally outlives all QML roots so resident-mode actions and
// configuration reloads continue to work with zero windows.
class ApplicationController final : public QObject {
    Q_OBJECT

public:
    using WindowFactory = std::move_only_function<
        std::expected<ApplicationWindow, QString>()>;

    ApplicationController(QQmlEngine &engine,
                          LaunchOptions effectiveOptions,
                          bool enableGlobalShortcutsPortal = true,
                          QObject *parent = nullptr);
    ApplicationController(LaunchOptions effectiveOptions,
                          WindowFactory windowFactory,
                          bool enableGlobalShortcutsPortal = true,
                          QObject *parent = nullptr);
    ~ApplicationController() override;

    [[nodiscard]] std::expected<ApplicationWindow, QString>
    createInitialWindow();
    [[nodiscard]] bool dispatch(
        ApplicationAction action,
        TerminalWorkspace *sourceWorkspace = nullptr,
        PaneId sourcePaneId = {});
    void applyLaunchOptions(const LaunchOptions &options);

    [[nodiscard]] TerminalWorkspace *activeWorkspace() const;
    [[nodiscard]] int windowCount() const;
    [[nodiscard]] QVector<ApplicationWindow> windows() const;
    [[nodiscard]] ApplicationLifetimeController *lifetimeController()
    {
        return &lifetime_;
    }

Q_SIGNALS:
    void configReloadRequested();
    void windowCreated(QQuickWindow *window, TerminalWorkspace *workspace);
    void windowRetired();
    void applicationQuitCommitted();
    void quitRequested();
    void windowCreationFailed(const QString &message);

private:
    struct WindowRecord {
        QPointer<QQuickWindow> window;
        QPointer<TerminalWorkspace> workspace;
    };

    enum class QuitState {
        Idle,
        AwaitingConfirmation,
        ClosingWindows,
    };

    static WindowFactory qmlWindowFactory(QQmlEngine &engine);
    [[nodiscard]] std::expected<ApplicationWindow, QString> createWindow(
        const LaunchOptions &options);
    [[nodiscard]] LaunchOptions nextWindowOptions(
        TerminalWorkspace *sourceWorkspace,
        PaneId sourcePaneId) const;
    [[nodiscard]] bool containsWorkspace(
        const TerminalWorkspace *workspace) const;
    [[nodiscard]] std::vector<QPointer<TerminalWorkspace>>
    workspaceSnapshot() const;
    void registerWindow(ApplicationWindow window);
    void noteWorkspaceActivated(TerminalWorkspace *workspace);
    void retireWindow(QQuickWindow *window);
    void workspaceDestroyed(TerminalWorkspace *workspace);
    void requestApplicationQuit();
    void commitApplicationQuit(TerminalWorkspace *host);
    void applicationQuitCancelled(TerminalWorkspace *host);
    void workspaceShutdownApproved(TerminalWorkspace *workspace);
    void finishApplicationQuitIfReady();
    void rehostApplicationQuit();

    WindowFactory windowFactory_;
    LaunchOptions effectiveOptions_;
    ApplicationLifetimeController lifetime_;
    std::unique_ptr<GhosttyApplicationKeybindings> keybindings_;
    std::vector<WindowRecord> windows_;
    QPointer<TerminalWorkspace> lastActiveWorkspace_;
    QPointer<TerminalWorkspace> quitDialogHost_;
    QSet<TerminalWorkspace *> awaitingShutdown_;
    QuitState quitState_ = QuitState::Idle;
    bool initialWindowCreated_ = false;
    bool startingApplicationShutdown_ = false;
    bool quitRehostScheduled_ = false;
    bool destroying_ = false;
};
