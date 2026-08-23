#pragma once

class ApplicationController;
struct LaunchOptions;
class QObject;
class QQuickWindow;
class QWindow;
class TerminalWorkspace;

namespace ApplicationTestHooks {

enum class TitlePromptTestTarget {
    Surface,
    Tab,
};

enum class ApplicationLifetimeTestMode {
    None,
    ResidentAfterWindowClose,
    ExternalActivation,
    ExplicitQuit,
};

ApplicationLifetimeTestMode applicationLifetimeTestMode();

bool installApplicationLifetimeTestHook(QWindow *applicationWindow,
                                        TerminalWorkspace *workspace,
                                        ApplicationController *controller,
                                        const LaunchOptions &options,
                                        ApplicationLifetimeTestMode mode,
                                        bool *completed);
bool installCloseDialogTestHook(QObject *rootObject,
                                TerminalWorkspace *workspace);
bool installContextMenuActionTestHook(QQuickWindow *window,
                                      TerminalWorkspace *workspace,
                                      ApplicationController *controller);
bool installContextMenuPositionTestHook(QQuickWindow *window,
                                        TerminalWorkspace *workspace);
bool installDesktopActivationTestHook(ApplicationController *controller,
                                      bool *completed);
bool installFullscreenActionTestHook(QQuickWindow *window,
                                     TerminalWorkspace *workspace);
bool installInitialWindowSizeTestHook(QQuickWindow *window,
                                      TerminalWorkspace *workspace,
                                      const LaunchOptions &options);
bool installInitialWindowStateTestHook(QQuickWindow *window,
                                       const LaunchOptions &options);
bool installMaximizeActionTestHook(QQuickWindow *window,
                                   TerminalWorkspace *workspace);
bool installRendererQualificationTestHook(QQuickWindow *window,
                                          TerminalWorkspace *workspace);
bool installSuppressedStartupTestHook(ApplicationController *controller,
                                      const LaunchOptions &options,
                                      bool *completed);
bool installTabBarVisibilityTestHook(QObject *rootObject,
                                     TerminalWorkspace *workspace);
bool installTabsLocationTestHook(QQuickWindow *window,
                                 TerminalWorkspace *workspace);
bool installTitlePromptTestHook(QObject *rootObject,
                                TerminalWorkspace *workspace,
                                TitlePromptTestTarget target);
bool installWindowDecorationActionTestHook(QQuickWindow *window,
                                           TerminalWorkspace *workspace);

} // namespace ApplicationTestHooks
