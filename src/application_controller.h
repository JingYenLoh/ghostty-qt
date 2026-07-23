#pragma once

#include "application_action.h"
#include "application_lifetime.h"
#include "desktop_activation.h"
#include "launch_options.h"
#include "revision_counter.h"
#include "window_navigation_action.h"
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
class GhosttyKeybindProgram;
class InitialSessionCoordinator;
class QQmlEngine;
class QQuickWindow;
class QScreen;
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
    createInitialWindow(DesktopActivationContext activation = {});
    // Completes the one-shot startup decision without requesting a surface.
    // A later explicit or remote new-window action can still create the first
    // surface and consume the current initial command.
    [[nodiscard]] bool startWithoutInitialWindow();
    // Handles a source-less process activation synchronously so the caller
    // can acknowledge only after a replacement window is registered. Ghostty
    // inherits the focused surface's cwd for this path, but not its font size.
    // Startup must first choose createInitialWindow or
    // startWithoutInitialWindow.
    [[nodiscard]] bool activateNoCommand(
        DesktopActivationContext activation = {});
    [[nodiscard]] bool dispatch(
        ApplicationAction action,
        TerminalWorkspace *sourceWorkspace = nullptr,
        PaneId sourcePaneId = {});
    // goto_window remains a surface-scoped binding action, but top-level
    // traversal belongs to the process owner. Dispatch stays synchronous so
    // Wayland can associate requestActivate() with the originating key event.
    [[nodiscard]] bool dispatch(WindowNavigationAction action);
    void applyLaunchOptions(const LaunchOptions &options);

    [[nodiscard]] TerminalWorkspace *activeWorkspace() const;
    [[nodiscard]] int windowCount() const;
    [[nodiscard]] QVector<ApplicationWindow> windows() const;
    [[nodiscard]] GhosttyKeybindProgram keybindProgram() const;
    [[nodiscard]] ApplicationLifetimeController *lifetimeController()
    {
        return &lifetime_;
    }

Q_SIGNALS:
    void configOpenRequested();
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
        LaunchOptions options,
        const DesktopActivationContext &activation = {},
        QScreen *preferredScreen = nullptr);
    [[nodiscard]] LaunchOptions nextWindowOptions(
        TerminalWorkspace *sourceWorkspace,
        PaneId sourcePaneId) const;
    [[nodiscard]] LaunchOptions activationWindowOptions() const;
    [[nodiscard]] TerminalWorkspace *focusedWorkspace() const;
    [[nodiscard]] bool containsWorkspace(
        const TerminalWorkspace *workspace) const;
    [[nodiscard]] std::vector<QPointer<TerminalWorkspace>>
    workspaceSnapshot() const;
    void dispatchRequestedAction(
        ApplicationAction action,
        TerminalWorkspace *sourceWorkspace = nullptr,
        PaneId sourcePaneId = {});
    void registerWindow(ApplicationWindow window);
    void syncWindowDecoration(QQuickWindow *window,
                              TerminalWorkspace *workspace);
    void noteWorkspaceActivated(TerminalWorkspace *workspace);
    void retireWindow(QQuickWindow *window);
    void workspaceDestroyed(TerminalWorkspace *workspace,
                            QQuickWindow *window);
    void requestApplicationQuit();
    void beginApplicationShutdown();
    void commitApplicationQuit(TerminalWorkspace *host);
    void applicationQuitCancelled(TerminalWorkspace *host);
    void workspaceShutdownApproved(TerminalWorkspace *workspace);
    void finishApplicationQuitIfReady();
    void rehostApplicationQuit();

    WindowFactory windowFactory_;
    LaunchOptions effectiveOptions_;
    RevisionCounter launchOptionsRevision_;
    std::shared_ptr<InitialSessionCoordinator> initialSessionCoordinator_;
    ApplicationLifetimeController lifetime_;
    std::unique_ptr<GhosttyApplicationKeybindings> keybindings_;
    std::vector<WindowRecord> windows_;
    QPointer<TerminalWorkspace> lastActiveWorkspace_;
    QPointer<TerminalWorkspace> quitDialogHost_;
    QSet<TerminalWorkspace *> awaitingShutdown_;
    QuitState quitState_ = QuitState::Idle;
    bool startupWindowHandled_ = false;
    bool windowCreationInProgress_ = false;
    bool startingApplicationShutdown_ = false;
    bool quitRehostScheduled_ = false;
    bool destroying_ = false;
};
