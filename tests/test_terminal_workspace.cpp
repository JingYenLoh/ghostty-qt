#include "ghostty_application_keybindings.h"
#include "launch_options.h"
#include "terminal_controller.h"
#include "terminal_pane.h"
#include "terminal_workspace.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

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

QTEST_MAIN(TerminalWorkspaceTest)

#include "test_terminal_workspace.moc"
