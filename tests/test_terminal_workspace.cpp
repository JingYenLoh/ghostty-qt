#include "ghostty_application_keybindings.h"
#include "launch_options.h"
#include "terminal_controller.h"
#include "terminal_pane.h"
#include "terminal_workspace.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <limits>
#include <memory>

namespace {

class ShellEnvironment final {
public:
    explicit ShellEnvironment(
        const QByteArray &shell = QByteArrayLiteral("/bin/sh"))
        : wasSet_(qEnvironmentVariableIsSet("SHELL"))
        , previous_(qgetenv("SHELL"))
    {
        qputenv("SHELL", shell);
    }

    ~ShellEnvironment()
    {
        if (wasSet_) {
            qputenv("SHELL", previous_);
        } else {
            qunsetenv("SHELL");
        }
    }

private:
    bool wasSet_ = false;
    QByteArray previous_;
};

LaunchOptions baseOptions()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.confirmCloseMode = ConfirmCloseMode::RunningProcesses;
    return options;
}

} // namespace

class TerminalWorkspaceTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void runningProgramPromptsThenResolvesOnceOnExit();
    void idleShellDoesNotPromptInRunningProcessesMode();
    void submittedCommandPromptsBeforeForegroundPoll();
    void performableTabChangeRequiresDifferentTarget();
    void alwaysModePromptsForIdleShell();
    void multiPaneShutdownGracePeriodsOverlap();
    void rootApplicationBindingPrecedesActiveTable();
    void broadBindingsReachInactivePanesAndIgnoreLocalFlags();
    void indexedLastAndMovedTabsPreserveStableIds();
    void splitNavigationWrapsInTreeAndSpatialOrder();
    void splitResizeAndEqualizeRespectTreeAxes();
    void splitZoomPreservesLayoutAndResetsOnNavigationAndSplit();
    void broadContainerActionsResolveFromActivePane();
    void inactiveTabResizeAppliesWhenActivated();
};

void TerminalWorkspaceTest::runningProgramPromptsThenResolvesOnceOnExit()
{
    LaunchOptions options = baseOptions();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("sleep 0.6"),
    };
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QSignalSpy confirmation(&workspace,
                            &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy resolved(&workspace,
                        &TerminalWorkspace::closeConfirmationResolved);
    QSignalSpy quit(&workspace, &TerminalWorkspace::quitApproved);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);

    workspace.requestQuit();
    QCOMPARE(confirmation.count(), 1);
    QCOMPARE(quit.count(), 0);

    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 3000);
    QTest::qWait(100);
    QCOMPARE(quit.count(), 1);
}

void TerminalWorkspaceTest::idleShellDoesNotPromptInRunningProcessesMode()
{
    ShellEnvironment shell;
    LaunchOptions options = baseOptions();
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QSignalSpy confirmation(&workspace,
                            &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy quit(&workspace, &TerminalWorkspace::quitApproved);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    QTest::qWait(350);

    workspace.requestQuit();
    QCOMPARE(confirmation.count(), 0);
    QCOMPARE(quit.count(), 1);
}

void TerminalWorkspaceTest::submittedCommandPromptsBeforeForegroundPoll()
{
    ShellEnvironment shell;
    LaunchOptions options = baseOptions();
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QSignalSpy confirmation(&workspace,
                            &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy quit(&workspace, &TerminalWorkspace::quitApproved);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    QTest::qWait(350);

    TerminalPane *pane = workspace.findChild<TerminalPane *>();
    QVERIFY(pane != nullptr);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return,
                    Qt::NoModifier, QStringLiteral("\r"));
    QCoreApplication::sendEvent(pane, &enter);

    // The UI-side latch closes the polling race: a close requested in the
    // same event turn as command submission must still protect active work.
    workspace.requestQuit();
    QCOMPARE(confirmation.count(), 1);
    QCOMPARE(quit.count(), 0);
    workspace.cancelClose();

    // An empty command settles back to an idle prompt after the conservative
    // grace interval and no longer needs confirmation.
    QTest::qWait(500);
    workspace.requestQuit();
    QCOMPARE(confirmation.count(), 1);
    QCOMPARE(quit.count(), 1);
}

void TerminalWorkspaceTest::performableTabChangeRequiresDifferentTarget()
{
    ShellEnvironment shell;
    LaunchOptions options = baseOptions();
    options.keybindingsConfigured = true;
    options.keybindings = {
        QStringLiteral("performable:ctrl+tab=next_tab"),
    };
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    TerminalPane *pane = workspace.findChild<TerminalPane *>();
    QVERIFY(pane != nullptr);
    TerminalController *controller =
        pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);

    QKeyEvent oneTab(QEvent::KeyPress, Qt::Key_Tab,
                     Qt::ControlModifier, QStringLiteral("\t"));
    QCoreApplication::sendEvent(pane, &oneTab);
    QCOMPARE(forwarded.count(), 1);
    QCOMPARE(workspace.currentIndex(), 0);

    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 2);
    QCOMPARE(workspace.currentIndex(), 1);
    TerminalPane *activePane = nullptr;
    for (TerminalPane *candidate : workspace.findChildren<TerminalPane *>()) {
        if (candidate->isVisible()) {
            activePane = candidate;
        }
    }
    QVERIFY(activePane != nullptr);
    TerminalController *activeController =
        activePane->findChild<TerminalController *>();
    QVERIFY(activeController != nullptr);
    QSignalSpy activeForwarded(activeController,
                               &TerminalController::keyRequested);

    QKeyEvent twoTabs(QEvent::KeyPress, Qt::Key_Tab,
                      Qt::ControlModifier, QStringLiteral("\t"));
    QCoreApplication::sendEvent(activePane, &twoTabs);
    QCOMPARE(workspace.currentIndex(), 0);
    QCOMPARE(activeForwarded.count(), 0);
}

void TerminalWorkspaceTest::alwaysModePromptsForIdleShell()
{
    ShellEnvironment shell;
    LaunchOptions options = baseOptions();
    options.confirmCloseMode = ConfirmCloseMode::Always;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QSignalSpy confirmation(&workspace,
                            &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy quit(&workspace, &TerminalWorkspace::quitApproved);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    QTest::qWait(350);

    workspace.requestQuit();
    QCOMPARE(confirmation.count(), 1);
    QCOMPARE(quit.count(), 0);
    workspace.cancelClose();
}

void TerminalWorkspaceTest::multiPaneShutdownGracePeriodsOverlap()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString shellPath = directory.filePath(QStringLiteral("resistant-shell"));
    QFile shellFile(shellPath);
    QVERIFY(shellFile.open(QIODevice::WriteOnly));
    const QByteArray script = QByteArrayLiteral(
        "#!/bin/sh\n"
        "trap '' HUP\n"
        "exec /bin/sleep 30\n");
    QCOMPARE(shellFile.write(script), script.size());
    shellFile.close();
    QVERIFY(shellFile.setPermissions(QFileDevice::ReadOwner
                                     | QFileDevice::WriteOwner
                                     | QFileDevice::ExeOwner));

    ShellEnvironment shell(QFile::encodeName(shellPath));
    LaunchOptions options = baseOptions();
    options.confirmCloseMode = ConfirmCloseMode::Always;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    auto workspace = std::make_unique<TerminalWorkspace>();
    QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);
    workspace->newTab();
    workspace->newTab();
    QCOMPARE(workspace->tabCount(), 3);

    // Give every worker time to exec the helper and install the inherited
    // ignored-SIGHUP disposition. Each must therefore reach the worker's
    // two-second SIGKILL fallback during shutdown.
    QTest::qWait(500);
    QSignalSpy confirmation(workspace.get(),
                            &TerminalWorkspace::closeConfirmationRequested);
    workspace->requestQuit();
    QCOMPARE(confirmation.count(), 1);

    QElapsedTimer elapsed;
    elapsed.start();
    workspace->confirmClose();
    workspace.reset();

    const qint64 shutdownMilliseconds = elapsed.elapsed();
    QVERIFY2(shutdownMilliseconds >= 1'500,
             "signal-resistant children did not exercise the shutdown grace period");
    QVERIFY2(shutdownMilliseconds < 4'500,
             "pane shutdown grace periods ran serially instead of concurrently");
}

void TerminalWorkspaceTest::rootApplicationBindingPrecedesActiveTable()
{
    ShellEnvironment shell;
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindingsConfigured = true;
    const auto unicode = [](quint32 codepoint, quint8 modifiers = 0) {
        return GhosttyKeybindTrigger{
            .kind = GhosttyKeybindKeyKind::Unicode,
            .unicodeCodepoint = codepoint,
            .modifiers = modifiers,
        };
    };
    options.keybindConfig.root = {
        GhosttyKeybindDefinition{
            .sequence = {unicode('m', GhosttyKeybindCtrl)},
            .actions = {QStringLiteral("activate_key_table:modal")},
        },
        GhosttyKeybindDefinition{
            .sequence = {unicode('r', GhosttyKeybindCtrl)},
            .actions = {QStringLiteral("reload_config")},
        },
    };
    options.keybindConfig.tables = {GhosttyKeybindTable{
        .name = QStringLiteral("modal"),
        .bindings = {GhosttyKeybindDefinition{
            .sequence = {unicode('r', GhosttyKeybindCtrl)},
            .actions = {QStringLiteral("new_tab")},
        }},
    }};
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    GhosttyApplicationKeybindings applicationBindings(options, false);
    applicationBindings.registerWorkspace(&workspace);
    QSignalSpy reload(&workspace,
                      &TerminalWorkspace::configReloadRequested);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    TerminalPane *pane = workspace.findChild<TerminalPane *>();
    QVERIFY(pane != nullptr);
    TerminalController *controller =
        pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);

    QKeyEvent activate(QEvent::KeyPress, Qt::Key_M,
                       Qt::ControlModifier, QString(QChar(0x0d)));
    QCoreApplication::sendEvent(pane, &activate);
    QCOMPARE(pane->activeKeyTables(),
             QStringList({QStringLiteral("modal")}));

    QKeyEvent rootApp(QEvent::KeyPress, Qt::Key_R,
                      Qt::ControlModifier, QString(QChar(0x12)));
    QCoreApplication::sendEvent(pane, &rootApp);
    QKeyEvent rootAppRelease(QEvent::KeyRelease, Qt::Key_R,
                             Qt::ControlModifier);
    QCoreApplication::sendEvent(pane, &rootAppRelease);
    QCOMPARE(reload.count(), 1);
    QCOMPARE(forwarded.count(), 0);
    QCOMPARE(workspace.tabCount(), 1);
    QCOMPARE(pane->activeKeyTables(),
             QStringList({QStringLiteral("modal")}));
}

void TerminalWorkspaceTest::broadBindingsReachInactivePanesAndIgnoreLocalFlags()
{
    ShellEnvironment shell;
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindingsConfigured = true;
    const auto unicode = [](quint32 codepoint, quint8 modifiers = 0) {
        return GhosttyKeybindTrigger{
            .kind = GhosttyKeybindKeyKind::Unicode,
            .unicodeCodepoint = codepoint,
            .modifiers = modifiers,
        };
    };
    options.keybindConfig.root = {
        GhosttyKeybindDefinition{
            .sequence = {unicode('g', GhosttyKeybindCtrl)},
            .actions = {QStringLiteral("increase_font_size:2")},
            .flags = GhosttyKeybindFlags{
                .consumed = false,
                .global = true,
                .performable = true,
            },
        },
        GhosttyKeybindDefinition{
            .sequence = {unicode('r', GhosttyKeybindCtrl)},
            .actions = {QStringLiteral("reload_config")},
            .flags = GhosttyKeybindFlags{.all = true},
        },
        GhosttyKeybindDefinition{
            .sequence = {unicode('x', GhosttyKeybindCtrl), unicode('n')},
            .actions = {QStringLiteral("new_tab")},
        },
    };
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    GhosttyApplicationKeybindings applicationBindings(options, false);
    applicationBindings.registerWorkspace(&workspace);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 2);

    const QList<TerminalPane *> panes =
        workspace.findChildren<TerminalPane *>();
    QCOMPARE(panes.size(), 2);
    TerminalPane *activePane = nullptr;
    TerminalPane *inactivePane = nullptr;
    for (TerminalPane *pane : panes) {
        if (pane->isVisible()) activePane = pane;
        else inactivePane = pane;
    }
    QVERIFY(activePane != nullptr);
    QVERIFY(inactivePane != nullptr);
    QCOMPARE(activePane->fontPointSize(), 12.0);
    QCOMPARE(inactivePane->fontPointSize(), 12.0);
    TerminalController *controller =
        activePane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    TerminalController *inactiveController =
        inactivePane->findChild<TerminalController *>();
    QVERIFY(inactiveController != nullptr);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QSignalSpy activeSequenceResolution(
        controller, &TerminalController::sequenceResolutionRequested);
    QSignalSpy inactiveSequenceResolution(
        inactiveController, &TerminalController::sequenceResolutionRequested);

    // An external all/global action can end sequences staged in panes that no
    // longer have focus. It must reset both the matcher and worker token.
    QKeyEvent activeLeader(QEvent::KeyPress, Qt::Key_X,
                           Qt::ControlModifier, QString(QChar(0x18)));
    QKeyEvent inactiveLeader(QEvent::KeyPress, Qt::Key_X,
                             Qt::ControlModifier, QString(QChar(0x18)));
    QCoreApplication::sendEvent(activePane, &activeLeader);
    QCoreApplication::sendEvent(inactivePane, &inactiveLeader);
    QCOMPARE(activeSequenceResolution.count(), 0);
    QCOMPARE(inactiveSequenceResolution.count(), 0);
    applicationBindings.dispatchBroadActions(
        {QStringLiteral("end_key_sequence")});
    QCOMPARE(activeSequenceResolution.count(), 1);
    QCOMPARE(inactiveSequenceResolution.count(), 1);
    QCOMPARE(qvariant_cast<TerminalSequenceResolution>(
                 activeSequenceResolution.front().at(1)),
             TerminalSequenceResolution::Flush);
    QCOMPARE(qvariant_cast<TerminalSequenceResolution>(
                 inactiveSequenceResolution.front().at(1)),
             TerminalSequenceResolution::Flush);

    // global implies all. The local unconsumed/performable flags have no
    // effect, and the inactive tab's surface receives the action too.
    QKeyEvent global(QEvent::KeyPress, Qt::Key_G,
                     Qt::ControlModifier, QString(QChar(0x07)));
    QCoreApplication::sendEvent(activePane, &global);
    QKeyEvent globalRelease(QEvent::KeyRelease, Qt::Key_G,
                            Qt::ControlModifier);
    QCoreApplication::sendEvent(activePane, &globalRelease);
    QCOMPARE(activePane->fontPointSize(), 14.0);
    QCOMPARE(inactivePane->fontPointSize(), 14.0);
    QCOMPARE(forwarded.count(), 0);

    // An app-scoped all: action runs once in the root pre-pass rather than
    // once per surface.
    QSignalSpy reload(&workspace,
                      &TerminalWorkspace::configReloadRequested);
    QKeyEvent allReload(QEvent::KeyPress, Qt::Key_R,
                        Qt::ControlModifier, QString(QChar(0x12)));
    QCoreApplication::sendEvent(activePane, &allReload);
    QKeyEvent allReloadRelease(QEvent::KeyRelease, Qt::Key_R,
                               Qt::ControlModifier);
    QCoreApplication::sendEvent(activePane, &allReloadRelease);
    QCOMPARE(reload.count(), 1);
    QCOMPARE(forwarded.count(), 0);

    // Valid-but-unimplemented close modes must remain no-ops, not be widened
    // into the current single-window quit path.
    QSignalSpy quit(&workspace, &TerminalWorkspace::quitApproved);
    applicationBindings.dispatchBroadActions(
        {QStringLiteral("close_tab:other")});
    QCOMPARE(quit.count(), 0);
    QCOMPARE(workspace.tabCount(), 2);
}

void TerminalWorkspaceTest::indexedLastAndMovedTabsPreserveStableIds()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId first = workspace.tabModel()->idAt(0);
    const PaneId firstPane = workspace.tabModel()->entryAt(0)->activePaneId;

    workspace.newTab();
    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 3);
    const TabId second = workspace.tabModel()->idAt(1);
    const TabId third = workspace.tabModel()->idAt(2);
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    QVERIFY(third.isValid());
    QVERIFY(first != second && second != third && first != third);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivateTabByIndex,
        {TabId{}, PaneId{}, 1},
    }));
    QCOMPARE(workspace.currentIndex(), 0);
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()), first);

    // A high one-based index clamps to the final tab.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivateTabByIndex,
        {TabId{}, PaneId{}, 999},
    }));
    QCOMPARE(workspace.currentIndex(), 2);
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()), third);
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::ActivateLastTab,
        {},
    }));
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::ActivateTabByIndex,
        {TabId{}, PaneId{}, 0},
    }));

    // The parser accepts usize, but Ghostty's execution boundary is c_int.
    // Start away from the final tab so an old clamp-to-final implementation
    // cannot make this rejection assertion pass accidentally.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivateTabByIndex,
        {TabId{}, PaneId{}, 2},
    }));
    const TabId selectedBeforeOversizedIndex =
        workspace.tabModel()->idAt(workspace.currentIndex());
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::ActivateTabByIndex,
        {TabId{}, PaneId{},
         static_cast<qint64>(std::numeric_limits<int>::max()) + 1},
    }));
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()),
             selectedBeforeOversizedIndex);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivateLastTab,
        {},
    }));
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()), third);

    // Move the first tab forward without changing the selected tab identity.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::MoveTab,
        {first, firstPane, 1},
    }));
    QCOMPARE(workspace.tabModel()->idAt(0), second);
    QCOMPARE(workspace.tabModel()->idAt(1), first);
    QCOMPARE(workspace.tabModel()->idAt(2), third);
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()), third);

    // Moving the selected final tab forward wraps it to the front and updates
    // only its row index; the stable TabId remains selected.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::MoveTab,
        {third, PaneId{}, 1},
    }));
    QCOMPARE(workspace.tabModel()->idAt(0), third);
    QCOMPARE(workspace.tabModel()->idAt(1), second);
    QCOMPARE(workspace.tabModel()->idAt(2), first);
    QCOMPARE(workspace.currentIndex(), 0);
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()), third);

    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::MoveTab,
        {third, PaneId{}, 3},
    }));
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::MoveTab,
        {TabId{}, PaneId(999'999), 1},
    }));
    QCOMPARE(workspace.tabModel()->idAt(0), third);
    QCOMPARE(workspace.tabModel()->idAt(1), second);
    QCOMPARE(workspace.tabModel()->idAt(2), first);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivateLastTab,
        {},
    }));
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()), first);
}

void TerminalWorkspaceTest::splitNavigationWrapsInTreeAndSpatialOrder()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(902.0, 602.0));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId tabId = workspace.tabModel()->idAt(0);
    const PaneId first = workspace.tabModel()->entryAt(0)->activePaneId;

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, first, 0},
    }));
    const PaneId second = workspace.tabModel()->entryAt(0)->activePaneId;
    QVERIFY(second.isValid());
    QVERIFY(second != first);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitDown,
        {tabId, second, 0},
    }));
    const PaneId third = workspace.tabModel()->entryAt(0)->activePaneId;
    QVERIFY(third.isValid());
    QVERIFY(third != first && third != second);

    // DFS leaf order is first, second, third; relative navigation wraps.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::NavigatePaneRelative,
        {tabId, third, 1},
    }));
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, first);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::NavigatePaneRelative,
        {tabId, first, -1},
    }));
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, third);
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::NavigatePaneRelative,
        {tabId, third, 3},
    }));

    // The first pane has no pane to its left. Spatial navigation wraps to the
    // nearest pane on the opposite edge, which is the upper-right pane.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::NavigatePane,
        {tabId, first, Qt::Key_Left},
    }));
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, second);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::NavigatePane,
        {tabId, second, Qt::Key_Right},
    }));
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, first);

    // Likewise, wrapping upward from the full-height left pane selects the
    // lower-right pane because it is nearest after translating by one grid.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::NavigatePane,
        {tabId, first, Qt::Key_Up},
    }));
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, third);
}

void TerminalWorkspaceTest::splitResizeAndEqualizeRespectTreeAxes()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(902.0, 602.0));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId tabId = workspace.tabModel()->idAt(0);
    const PaneId firstId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *firstPane = workspace.findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, firstId, 0},
    }));
    const PaneId secondId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *secondPane = nullptr;
    for (TerminalPane *candidate : workspace.findChildren<TerminalPane *>()) {
        if (candidate != firstPane) secondPane = candidate;
    }
    QVERIFY(secondPane != nullptr);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, secondId, 0},
    }));
    const PaneId thirdId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *thirdPane = nullptr;
    for (TerminalPane *candidate : workspace.findChildren<TerminalPane *>()) {
        if (candidate != firstPane && candidate != secondPane) {
            thirdPane = candidate;
        }
    }
    QVERIFY(thirdPane != nullptr);
    QCOMPARE(workspace.findChildren<TerminalPane *>().size(), 3);

    const qreal initialSecondWidth = secondPane->width();
    const qreal initialThirdWidth = thirdPane->width();
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ResizeSplit,
        {tabId, thirdId, Qt::Key_Right, 60},
    }));
    QVERIFY(qAbs(secondPane->width() - (initialSecondWidth + 60.0)) <= 1.0);
    QVERIFY(qAbs(thirdPane->width() - (initialThirdWidth - 60.0)) <= 1.0);

    // A split tree is performable even when this leaf has no ancestor on the
    // requested axis, but the tree remains unchanged. Zero amount is not
    // performable.
    const QSizeF unchangedFirst = firstPane->size();
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ResizeSplit,
        {tabId, firstId, Qt::Key_Up, 10},
    }));
    QCOMPARE(firstPane->size(), unchangedFirst);
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::ResizeSplit,
        {tabId, firstId, Qt::Key_Right, 0},
    }));

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitDown,
        {tabId, thirdId, 0},
    }));
    const PaneId fourthId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *fourthPane = nullptr;
    for (TerminalPane *candidate : workspace.findChildren<TerminalPane *>()) {
        if (candidate != firstPane && candidate != secondPane
            && candidate != thirdPane) {
            fourthPane = candidate;
        }
    }
    QVERIFY(fourthPane != nullptr);

    const qreal initialThirdHeight = thirdPane->height();
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ResizeSplit,
        {tabId, fourthId, Qt::Key_Down, 60},
    }));
    QVERIFY(qAbs(thirdPane->height() - (initialThirdHeight + 60.0)) <= 1.0);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::EqualizeSplits,
        {tabId, fourthId, 0},
    }));
    // The two contiguous horizontal splits weight the left leaf against the
    // two right-hand units at 1:2, while the perpendicular vertical subtree
    // counts as one unit. All three columns therefore have equal width.
    QVERIFY(qAbs(firstPane->width() - secondPane->width()) <= 1.0);
    QVERIFY(qAbs(secondPane->width() - thirdPane->width()) <= 1.0);
    QVERIFY(qAbs(thirdPane->height() - fourthPane->height()) <= 1.0);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, fourthId);
}

void TerminalWorkspaceTest::splitZoomPreservesLayoutAndResetsOnNavigationAndSplit()
{
    // Keep later split/tab shells alive while the explicitly held /bin/true
    // source becomes a stopped pane that can be closed without confirmation.
    ShellEnvironment shell(QByteArrayLiteral("/bin/sh"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(902.0, 602.0));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId tabId = workspace.tabModel()->idAt(0);
    const PaneId firstId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *firstPane = workspace.findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);

    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, firstId, 0},
    }));
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, firstId, 0},
    }));
    const PaneId secondId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *secondPane = nullptr;
    for (TerminalPane *candidate : workspace.findChildren<TerminalPane *>()) {
        if (candidate != firstPane) secondPane = candidate;
    }
    QVERIFY(secondPane != nullptr);
    const QSizeF normalFirstSize = firstPane->size();
    const QSizeF normalSecondSize = secondPane->size();

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, secondId, 0},
    }));
    QVERIFY(workspace.tabModel()->entryAt(0)->zoomed);
    QVERIFY(!firstPane->isVisible());
    QVERIFY(secondPane->isVisible());
    QCOMPARE(secondPane->position(), workspace.boundingRect().topLeft());
    QCOMPARE(secondPane->size(), workspace.boundingRect().size());

    workspace.newTab();
    QCOMPARE(workspace.currentIndex(), 1);
    QVERIFY(!firstPane->isVisible());
    QVERIFY(!secondPane->isVisible());
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivateTab,
        {tabId, PaneId{}, 0},
    }));
    QCOMPARE(workspace.currentIndex(), 0);
    QVERIFY(workspace.tabModel()->entryAt(0)->zoomed);
    QVERIFY(!firstPane->isVisible());
    QVERIFY(secondPane->isVisible());
    QCOMPARE(secondPane->size(), workspace.boundingRect().size());

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivatePane,
        {tabId, firstId, 0},
    }));
    QVERIFY(!workspace.tabModel()->entryAt(0)->zoomed);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, firstId);
    QVERIFY(firstPane->isVisible());
    QVERIFY(secondPane->isVisible());

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivatePane,
        {tabId, secondId, 0},
    }));
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, secondId, 0},
    }));
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, secondId, 0},
    }));
    QVERIFY(!workspace.tabModel()->entryAt(0)->zoomed);
    QVERIFY(firstPane->isVisible());
    QVERIFY(secondPane->isVisible());
    QCOMPARE(firstPane->size(), normalFirstSize);
    QCOMPARE(secondPane->size(), normalSecondSize);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, secondId, 0},
    }));
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::NavigatePaneRelative,
        {tabId, secondId, 1},
    }));
    QVERIFY(!workspace.tabModel()->entryAt(0)->zoomed);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, firstId);
    QVERIFY(firstPane->isVisible());
    QVERIFY(secondPane->isVisible());

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, firstId, 0},
    }));
    QVERIFY(workspace.tabModel()->entryAt(0)->zoomed);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, firstId, 0},
    }));
    QVERIFY(!workspace.tabModel()->entryAt(0)->zoomed);
    QCOMPARE(workspace.findChildren<TerminalPane *>().size(), 4);
    int visiblePaneCount = 0;
    for (TerminalPane *pane : workspace.findChildren<TerminalPane *>()) {
        if (pane->isVisible()) ++visiblePaneCount;
    }
    // The second tab remains hidden; every pane in the current tab is visible
    // after splitting resets zoom.
    QCOMPARE(visiblePaneCount, 3);

    const PaneId zoomTarget = workspace.tabModel()->entryAt(0)->activePaneId;
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, zoomTarget, 0},
    }));
    TerminalPane *zoomedPane = nullptr;
    for (TerminalPane *pane : workspace.findChildren<TerminalPane *>()) {
        if (pane->isVisible()) zoomedPane = pane;
    }
    QVERIFY(zoomedPane != nullptr);
    QPointer<TerminalPane> zoomedPaneGuard(zoomedPane);
    QTRY_VERIFY_WITH_TIMEOUT(!firstPane->isRunning(), 1000);
    const int paneCountBeforeClose =
        workspace.findChildren<TerminalPane *>().size();
    QPointer<TerminalPane> closedPane(firstPane);
    QSignalSpy confirmation(&workspace,
                            &TerminalWorkspace::closeConfirmationRequested);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ClosePane,
        {tabId, firstId, 0},
    }));
    QCOMPARE(confirmation.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.findChildren<TerminalPane *>().size(),
                              paneCountBeforeClose - 1,
                              1000);
    QTRY_VERIFY_WITH_TIMEOUT(closedPane.isNull(), 1000);
    QCOMPARE(workspace.tabCount(), 2);
    QCOMPARE(workspace.tabModel()->count(), 2);
    const TabListEntry *survivingTab = workspace.tabModel()->entryAt(0);
    QVERIFY(survivingTab != nullptr);
    QVERIFY(survivingTab->zoomed);
    QCOMPARE(survivingTab->activePaneId, zoomTarget);
    QVERIFY(!zoomedPaneGuard.isNull());
    QVERIFY(zoomedPaneGuard->isVisible());
}

void TerminalWorkspaceTest::broadContainerActionsResolveFromActivePane()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(902.0, 602.0));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId tabId = workspace.tabModel()->idAt(0);
    const PaneId firstId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *firstPane = workspace.findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, firstId, 0},
    }));
    const PaneId secondId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *secondPane = nullptr;
    for (TerminalPane *candidate : workspace.findChildren<TerminalPane *>()) {
        if (candidate != firstPane) secondPane = candidate;
    }
    QVERIFY(secondPane != nullptr);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, secondId, 0},
    }));
    const PaneId thirdId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *thirdPane = nullptr;
    for (TerminalPane *candidate : workspace.findChildren<TerminalPane *>()) {
        if (candidate != firstPane && candidate != secondPane) {
            thirdPane = candidate;
        }
    }
    QVERIFY(thirdPane != nullptr);

    const qreal firstWidth = firstPane->width();
    const qreal secondWidth = secondPane->width();
    const qreal thirdWidth = thirdPane->width();
    QVERIFY(workspace.executeSurfaceActionOnAllPanes(
        QStringLiteral("resize_split:right,10")));

    // All three snapshot surfaces resolve resize from the tree's active third
    // pane, so only its nearest divider moves, three times.
    QVERIFY(qAbs(firstPane->width() - firstWidth) <= 1.0);
    QVERIFY(qAbs(secondPane->width() - (secondWidth + 30.0)) <= 1.0);
    QVERIFY(qAbs(thirdPane->width() - (thirdWidth - 30.0)) <= 1.0);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, thirdId);

    QVERIFY(workspace.executeSurfaceActionOnAllPanes(
        QStringLiteral("goto_split:previous")));
    // Three wrapped moves across three panes return to the original active
    // pane; resolving from each snapshot source would end on a different ID.
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, thirdId);
}

void TerminalWorkspaceTest::inactiveTabResizeAppliesWhenActivated()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(902.0, 602.0));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId firstTab = workspace.tabModel()->idAt(0);
    const PaneId firstId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *firstPane = workspace.findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {firstTab, firstId, 0},
    }));
    const PaneId secondId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *secondPane = nullptr;
    for (TerminalPane *candidate : workspace.findChildren<TerminalPane *>()) {
        if (candidate != firstPane) secondPane = candidate;
    }
    QVERIFY(secondPane != nullptr);
    const qreal firstWidth = firstPane->width();
    const qreal secondWidth = secondPane->width();

    workspace.newTab();
    QCOMPARE(workspace.currentIndex(), 1);
    QVERIFY(!firstPane->isVisible());
    QVERIFY(!secondPane->isVisible());
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ResizeSplit,
        {firstTab, secondId, Qt::Key_Right, 90},
    }));
    // Hidden panes retain their last applied PTY size until their tab is shown.
    QCOMPARE(firstPane->width(), firstWidth);
    QCOMPARE(secondPane->width(), secondWidth);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivateTab,
        {firstTab, PaneId{}, 0},
    }));
    QVERIFY(qAbs(firstPane->width() - (firstWidth + 90.0)) <= 1.0);
    QVERIFY(qAbs(secondPane->width() - (secondWidth - 90.0)) <= 1.0);
}

QTEST_MAIN(TerminalWorkspaceTest)

#include "test_terminal_workspace.moc"
