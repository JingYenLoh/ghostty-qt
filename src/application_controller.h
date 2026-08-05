#pragma once

#include "application_action.h"
#include "application_lifetime.h"
#include "desktop_activation.h"
#include "launch_options.h"
#include "revision_counter.h"
#include "window_navigation_action.h"
#include "workspace_action.h"
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
class DesktopNotificationService;
class InitialSessionCoordinator;
class QQmlEngine;
class QQuickWindow;
class QScreen;
class QTimer;
class QuickTerminalSurface;
class TerminalWorkspace;
class WindowBlurController;
class WindowUiController;
struct FirstSurfaceOverrides;
struct GhosttyNewWindowTransportOverrides;
struct WorkspaceFrontendActionRequest;

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
    using WindowFactory =
        std::move_only_function<std::expected<ApplicationWindow, QString>()>;
    using CrashActionDispatcher = std::move_only_function<bool(
        QQuickWindow *, TerminalWorkspace *, PaneId,
        WorkspaceFrontendActions::CrashTarget)>;

    enum class ConfigurationSource {
        Ghostty,
        Frontend,
    };
    Q_ENUM(ConfigurationSource)

    ApplicationController(QQmlEngine &engine, LaunchOptions effectiveOptions,
                          bool enableGlobalShortcutsPortal = true,
                          QObject *parent = nullptr);
    ApplicationController(LaunchOptions effectiveOptions,
                          WindowFactory windowFactory,
                          bool enableGlobalShortcutsPortal = true,
                          QObject *parent = nullptr);
    ApplicationController(LaunchOptions effectiveOptions,
                          WindowFactory windowFactory,
                          CrashActionDispatcher crashActionDispatcher,
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
    [[nodiscard]] bool
    activateNoCommand(DesktopActivationContext activation = {});
    // Ghostty application actions are source-less. Their decoded launch
    // overrides apply only to the newly created window's first surface.
    [[nodiscard]] bool
    activateNewWindow(GhosttyNewWindowTransportOverrides overrides,
                      DesktopActivationContext activation = {});
    [[nodiscard]] bool
    activateQuickTerminal(DesktopActivationContext activation = {});
    [[nodiscard]] bool dispatch(ApplicationAction action,
                                TerminalWorkspace *sourceWorkspace = nullptr,
                                PaneId sourcePaneId = {});
    // goto_window remains a surface-scoped binding action, but top-level
    // traversal belongs to the process owner. Dispatch stays synchronous so
    // Wayland can associate requestActivate() with the originating key event.
    [[nodiscard]] bool dispatch(WindowNavigationAction action);
    void applyLaunchOptions(const LaunchOptions &options);
    // Call only after a configuration source has completed a successful
    // post-startup reload. Initial bootstrap and failed reloads deliberately
    // never produce an in-application notification.
    void notifyConfigurationReloaded();
    void reportConfigurationFailure(ConfigurationSource source,
                                    const QString &message);
    void clearConfigurationFailure(ConfigurationSource source);
    [[nodiscard]] QString configurationDiagnosticsText() const;

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
    enum class WindowRole {
        Normal,
        QuickTerminal,
    };

    struct WindowRecord {
        WindowId id;
        WindowRole role = WindowRole::Normal;
        QPointer<QQuickWindow> window;
        QPointer<TerminalWorkspace> workspace;
        std::unique_ptr<WindowUiController> ui;
        std::unique_ptr<WindowBlurController> blur;
        std::unique_ptr<QuickTerminalSurface> quickTerminalSurface;
        QPointer<QTimer> quickTerminalAutohideTimer;
        bool quickTerminalActivationAcknowledged = false;
        bool retiring = false;
    };

    enum class QuitState {
        Idle,
        AwaitingConfirmation,
        ClosingWindows,
    };

    static WindowFactory qmlWindowFactory(QQmlEngine &engine);
    [[nodiscard]] std::expected<ApplicationWindow, QString>
    createWindow(LaunchOptions options,
                 const DesktopActivationContext &activation = {},
                 QScreen *preferredScreen = nullptr,
                 WindowRole role = WindowRole::Normal,
                 const FirstSurfaceOverrides *firstSurfaceOverrides = nullptr);
    [[nodiscard]] LaunchOptions
    nextWindowOptions(TerminalWorkspace *sourceWorkspace,
                      PaneId sourcePaneId) const;
    [[nodiscard]] LaunchOptions activationWindowOptions() const;
    [[nodiscard]] TerminalWorkspace *focusedWorkspace() const;
    [[nodiscard]] bool
    containsWorkspace(const TerminalWorkspace *workspace) const;
    [[nodiscard]] std::vector<QPointer<TerminalWorkspace>>
    workspaceSnapshot() const;
    void dispatchRequestedAction(ApplicationAction action,
                                 TerminalWorkspace *sourceWorkspace = nullptr,
                                 PaneId sourcePaneId = {});
    void
    registerWindow(ApplicationWindow window, WindowRole role,
                   std::unique_ptr<QuickTerminalSurface> quickTerminalSurface);
    [[nodiscard]] WindowRecord *recordForWindow(QQuickWindow *window);
    [[nodiscard]] const WindowRecord *
    recordForWindow(QQuickWindow *window) const;
    [[nodiscard]] WindowRecord *
    recordForWorkspace(TerminalWorkspace *workspace);
    [[nodiscard]] WindowRecord *recordForWindowId(WindowId id);
    [[nodiscard]] WindowRecord *quickTerminalRecord();
    [[nodiscard]] QScreen *quickTerminalSizingScreen() const;
    [[nodiscard]] bool
    toggleQuickTerminal(const DesktopActivationContext &activation = {});
    [[nodiscard]] std::expected<void, QString>
    syncQuickTerminal(WindowRecord &record);
    void refreshCommandPalette(WindowId sourceWindowId);
    [[nodiscard]] bool executePaletteAction(WindowId sourceWindowId,
                                            const QString &action);
    [[nodiscard]] bool presentSurface(SurfaceTarget target);
    void updateQuickTerminalAutohide(WindowRecord &record);
    void syncConfigurationDiagnostics();
    void syncWindowBlur();
    void syncApplicationShell();
    [[nodiscard]] bool
    dispatchFrontendAction(TerminalWorkspace *workspace,
                           const WorkspaceFrontendActionRequest &request);
    void syncWindowDecoration(QQuickWindow *window,
                              TerminalWorkspace *workspace);
    void noteWorkspaceActivated(TerminalWorkspace *workspace);
    void unmapAndRetireWindow(QQuickWindow *window);
    void retireWindow(QQuickWindow *window);
    void workspaceDestroyed(TerminalWorkspace *workspace, QQuickWindow *window);
    void requestApplicationQuit();
    void beginApplicationShutdown();
    void commitApplicationQuit(TerminalWorkspace *host);
    void applicationQuitCancelled(TerminalWorkspace *host);
    void workspaceShutdownApproved(TerminalWorkspace *workspace);
    void finishApplicationQuitIfReady();
    void rehostApplicationQuit();

    WindowFactory windowFactory_;
    CrashActionDispatcher crashActionDispatcher_;
    LaunchOptions effectiveOptions_;
    RevisionCounter launchOptionsRevision_;
    std::shared_ptr<InitialSessionCoordinator> initialSessionCoordinator_;
    ApplicationLifetimeController lifetime_;
    std::unique_ptr<DesktopNotificationService> desktopNotifications_;
    std::unique_ptr<GhosttyApplicationKeybindings> keybindings_;
    std::vector<WindowRecord> windows_;
    QPointer<TerminalWorkspace> lastActiveWorkspace_;
    QPointer<TerminalWorkspace> quitDialogHost_;
    QSet<TerminalWorkspace *> awaitingShutdown_;
    QString ghosttyConfigurationFailure_;
    QString frontendConfigurationFailure_;
    QuitState quitState_ = QuitState::Idle;
    bool startupWindowHandled_ = false;
    bool windowCreationInProgress_ = false;
    bool startingApplicationShutdown_ = false;
    bool quitRehostScheduled_ = false;
    bool destroying_ = false;
    quint64 nextWindowId_ = 1;
};
