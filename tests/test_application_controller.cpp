#include "application_controller.h"
#include "terminal_pane.h"
#include "terminal_workspace.h"

#include <QDir>
#include <QGuiApplication>
#include <QPointer>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <expected>

namespace {

struct WindowFactoryHarness {
    int calls = 0;
    int failOnCall = 0;

    ApplicationController::WindowFactory factory()
    {
        return [this]() -> std::expected<ApplicationWindow, QString> {
            ++calls;
            if (failOnCall == calls) {
                return std::unexpected(
                    QStringLiteral("injected window creation failure"));
            }

            auto *window = new QQuickWindow;
            window->setWidth(800);
            window->setHeight(500);
            auto *workspace =
                new TerminalWorkspace(window->contentItem());
            workspace->setParentItem(window->contentItem());
            workspace->setSize(QSizeF(window->size()));
            return ApplicationWindow{window, workspace};
        };
    }
};

LaunchOptions baseOptions(const QString &directory)
{
    LaunchOptions options;
    options.workingDirectory = directory;
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    options.quitAfterLastWindowClosed = false;
    options.keybindingsConfigured = true;
    return options;
}

TerminalPane *onlyPane(TerminalWorkspace *workspace)
{
    if (workspace == nullptr) return nullptr;
    const QList<TerminalPane *> panes =
        workspace->findChildren<TerminalPane *>();
    return panes.size() == 1 ? panes.constFirst() : nullptr;
}

PaneId activePaneId(TerminalWorkspace *workspace)
{
    if (workspace == nullptr || workspace->tabCount() != 1) return {};
    const TabListEntry *const entry = workspace->tabModel()->entryAt(0);
    return entry != nullptr ? entry->activePaneId : PaneId{};
}

void closeWorkspace(TerminalWorkspace *workspace)
{
    QVERIFY(workspace != nullptr);
    workspace->requestWindowClose();
}

} // namespace

class ApplicationControllerTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void preservesCompositeSourceAndWindowInheritancePolicies();
    void residentProcessReloadsRecreatesAndQuitsWithZeroWindows();
    void suppressedStartupPreservesFirstSurfaceOptions();
    void failedLazyCreationDoesNotConsumeFirstSurfaceOptions();
    void ordinaryCloseUsesOnlyTheFinalWindowForLifetimePolicy();
    void successfulReplacementCancelsDelayedQuit();
    void sourceLessActivationMatchesUpstreamInheritance();
    void sourceLessActivationCancelsQuitAndReportsFailure();
    void explicitQuitAggregatesEveryWindowIntoOneConfirmation();
    void pendingQuitRehostsAfterItsWindowDisappears();
    void failedReplacementLeavesDelayedQuitArmed();
};

void ApplicationControllerTest::initTestCase()
{
    QVERIFY(QDir().mkpath(
        QDir::current().filePath(QStringLiteral("tmp"))));
}

void ApplicationControllerTest::preservesCompositeSourceAndWindowInheritancePolicies()
{
    QTemporaryDir directories(
        QDir::current().filePath(
            QStringLiteral("tmp/application-controller-inheritance-XXXXXX")));
    QVERIFY(directories.isValid());
    const QString firstDirectory =
        QDir(directories.path()).filePath(QStringLiteral("first"));
    const QString secondDirectory =
        QDir(directories.path()).filePath(QStringLiteral("second"));
    QVERIFY(QDir().mkpath(firstDirectory));
    QVERIFY(QDir().mkpath(secondDirectory));

    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(firstDirectory);
    options.fontSize = 13.0;
    ApplicationController controller(options, harness.factory(), false);

    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());
    QCOMPARE(controller.windowCount(), 1);
    TerminalPane *const firstPane = onlyPane(initial->workspace);
    QVERIFY(firstPane != nullptr);
    QCOMPARE(activePaneId(initial->workspace), PaneId(1));
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("set_font_size:18")));
    QCOMPARE(firstPane->fontPointSize(), 18.0);

    // Create a second window from application defaults. It also owns PaneId 1,
    // proving that a naked PaneId is not a process-wide identity.
    LaunchOptions fallback = options;
    fallback.workingDirectory = secondDirectory;
    fallback.fontSize = 11.0;
    fallback.windowInheritWorkingDirectory = false;
    fallback.windowInheritFontSize = false;
    controller.applyLaunchOptions(fallback);
    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 1000);
    const QVector<ApplicationWindow> firstTwo = controller.windows();
    TerminalWorkspace *const secondWorkspace = firstTwo.constLast().workspace;
    TerminalPane *const secondPane = onlyPane(secondWorkspace);
    QVERIFY(secondPane != nullptr);
    QCOMPARE(activePaneId(secondWorkspace), PaneId(1));
    QCOMPARE(secondWorkspace->effectiveLaunchOptions().workingDirectory,
             secondDirectory);
    QCOMPARE(secondWorkspace->effectiveLaunchOptions().fontSize, 11.0);
    QVERIFY(secondWorkspace->effectiveLaunchOptions().program.isEmpty());
    QVERIFY(!secondWorkspace->effectiveLaunchOptions().hold);

    LaunchOptions inheriting = fallback;
    inheriting.windowInheritWorkingDirectory = true;
    inheriting.windowInheritFontSize = true;
    controller.applyLaunchOptions(inheriting);
    QVERIFY(initial->workspace->effectiveLaunchOptions()
                .windowInheritWorkingDirectory);
    QVERIFY(secondWorkspace->effectiveLaunchOptions()
                .windowInheritWorkingDirectory);

    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("new_window")));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 3, 1000);
    TerminalWorkspace *const inheritedFirst =
        controller.windows().constLast().workspace;
    QCOMPARE(inheritedFirst->effectiveLaunchOptions().workingDirectory,
             firstDirectory);
    QCOMPARE(inheritedFirst->effectiveLaunchOptions().fontSize, 18.0);
    QVERIFY(inheritedFirst->effectiveLaunchOptions().program.isEmpty());
    QVERIFY(!inheritedFirst->effectiveLaunchOptions().hold);

    QVERIFY(secondPane->executeConfiguredAction(
        QStringLiteral("new_window")));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 4, 1000);
    TerminalWorkspace *const inheritedSecond =
        controller.windows().constLast().workspace;
    QCOMPARE(inheritedSecond->effectiveLaunchOptions().workingDirectory,
             secondDirectory);
    QCOMPARE(inheritedSecond->effectiveLaunchOptions().fontSize, 11.0);

    // Delivery validates the composite source again. If its whole window has
    // disappeared, the most recently active live pane supplies inheritance.
    QVERIFY(controller.dispatch(ApplicationAction::NewWindow,
                                initial->workspace, PaneId(1)));
    QPointer<QQuickWindow> staleSourceWindow(initial->window);
    delete initial->window;
    QVERIFY(staleSourceWindow.isNull());
    QCOMPARE(controller.windowCount(), 3);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 4, 1000);
    const TerminalWorkspace *const staleSourceFallback =
        controller.windows().constLast().workspace;
    QCOMPARE(staleSourceFallback->effectiveLaunchOptions().workingDirectory,
             secondDirectory);
    QCOMPARE(staleSourceFallback->effectiveLaunchOptions().fontSize, 11.0);
}

void ApplicationControllerTest::residentProcessReloadsRecreatesAndQuitsWithZeroWindows()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    ApplicationController controller(options, harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());

    QPointer<QQuickWindow> retiredWindow(initial->window);
    QPointer<TerminalWorkspace> retiredWorkspace(initial->workspace);
    closeWorkspace(initial->workspace);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(retiredWindow.isNull(), 1000);
    QVERIFY(retiredWorkspace.isNull());
    QVERIFY(!controller.lifetimeController()->quitPending());
    QVERIFY(!controller.lifetimeController()->hasRequestedQuit());

    QSignalSpy reload(&controller,
                      &ApplicationController::configReloadRequested);
    QVERIFY(controller.dispatch(ApplicationAction::ReloadConfig));
    QCOMPARE(reload.count(), 1);

    LaunchOptions reloaded = options;
    reloaded.fontSize = 17.0;
    controller.applyLaunchOptions(reloaded);
    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 1, 1000);
    const ApplicationWindow replacement = controller.windows().constFirst();
    QCOMPARE(replacement.workspace->effectiveLaunchOptions().fontSize, 17.0);
    QVERIFY(replacement.workspace->effectiveLaunchOptions().program.isEmpty());
    QVERIFY(!replacement.workspace->effectiveLaunchOptions().hold);

    closeWorkspace(replacement.workspace);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
    QSignalSpy committed(&controller,
                         &ApplicationController::applicationQuitCommitted);
    QSignalSpy quit(&controller, &ApplicationController::quitRequested);
    QVERIFY(controller.dispatch(ApplicationAction::Quit));
    QCOMPARE(committed.count(), 1);
    QCOMPARE(quit.count(), 1);
}

void ApplicationControllerTest::suppressedStartupPreservesFirstSurfaceOptions()
{
    WindowFactoryHarness localHarness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.initialWindow = false;
    options.quitAfterLastWindowClosed = true;
    options.quitAfterLastWindowClosedDelay = std::chrono::milliseconds(25);
    ApplicationController local(options, localHarness.factory(), false);

    QVERIFY(local.startWithoutInitialWindow());
    QCOMPARE(localHarness.calls, 0);
    QCOMPARE(local.windowCount(), 0);
    QVERIFY(!local.startWithoutInitialWindow());
    QVERIFY(!local.createInitialWindow().has_value());
    QTest::qWait(50);
    QVERIFY(!local.lifetimeController()->quitPending());
    QVERIFY(!local.lifetimeController()->hasRequestedQuit());

    LaunchOptions reloaded = options;
    reloaded.initialWindow = true;
    reloaded.fontSize = 19.0;
    reloaded.program = {
        QStringLiteral("/bin/sh"), QStringLiteral("-c"),
        QStringLiteral("exit 0"),
    };
    reloaded.hold = true;
    local.applyLaunchOptions(reloaded);
    QCOMPARE(local.windowCount(), 0);
    QCOMPARE(localHarness.calls, 0);

    QVERIFY(local.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(local.windowCount(), 1, 1000);
    const TerminalWorkspace *const first =
        local.windows().constFirst().workspace;
    QCOMPARE(first->effectiveLaunchOptions().program, reloaded.program);
    QVERIFY(first->effectiveLaunchOptions().hold);
    QCOMPARE(first->effectiveLaunchOptions().fontSize, 19.0);

    reloaded.initialWindow = false;
    local.applyLaunchOptions(reloaded);
    QCOMPARE(local.windowCount(), 1);
    QVERIFY(local.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(local.windowCount(), 2, 1000);
    const TerminalWorkspace *const second =
        local.windows().constLast().workspace;
    QVERIFY(second->effectiveLaunchOptions().program.isEmpty());
    QVERIFY(!second->effectiveLaunchOptions().hold);

    WindowFactoryHarness activationHarness;
    ApplicationController activated(
        options, activationHarness.factory(), false);
    QVERIFY(activated.startWithoutInitialWindow());
    QVERIFY(activated.activateNoCommand());
    QCOMPARE(activated.windowCount(), 1);
    const TerminalWorkspace *const activatedFirst =
        activated.windows().constFirst().workspace;
    QCOMPARE(activatedFirst->effectiveLaunchOptions().program,
             options.program);
    QVERIFY(activatedFirst->effectiveLaunchOptions().hold);
    QVERIFY(activated.activateNoCommand());
    QCOMPARE(activated.windowCount(), 2);
    QVERIFY(activated.windows().constLast().workspace
                ->effectiveLaunchOptions().program.isEmpty());
    QVERIFY(!activated.windows().constLast().workspace
                 ->effectiveLaunchOptions().hold);
}

void ApplicationControllerTest::failedLazyCreationDoesNotConsumeFirstSurfaceOptions()
{
    WindowFactoryHarness harness;
    harness.failOnCall = 1;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.initialWindow = false;
    ApplicationController controller(options, harness.factory(), false);
    QVERIFY(controller.startWithoutInitialWindow());

    QSignalSpy failure(&controller,
                       &ApplicationController::windowCreationFailed);
    QVERIFY(!controller.activateNoCommand());
    QCOMPARE(failure.count(), 1);
    QCOMPARE(harness.calls, 1);
    QCOMPARE(controller.windowCount(), 0);

    QVERIFY(controller.activateNoCommand());
    QCOMPARE(harness.calls, 2);
    QCOMPARE(controller.windowCount(), 1);
    const LaunchOptions &actual = controller.windows().constFirst().workspace
                                      ->effectiveLaunchOptions();
    QCOMPARE(actual.program, options.program);
    QVERIFY(actual.hold);
}

void ApplicationControllerTest::ordinaryCloseUsesOnlyTheFinalWindowForLifetimePolicy()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.quitAfterLastWindowClosed = true;
    options.quitAfterLastWindowClosedDelay = std::chrono::milliseconds(150);
    ApplicationController controller(options, harness.factory(), false);
    QVERIFY(controller.createInitialWindow().has_value());
    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 1000);

    QSignalSpy quit(&controller, &ApplicationController::quitRequested);
    const QVector<ApplicationWindow> windows = controller.windows();
    closeWorkspace(windows.constFirst().workspace);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 1, 1000);
    QTest::qWait(200);
    QCOMPARE(quit.count(), 0);
    QVERIFY(controller.lifetimeController()->hasOpenWindow());

    closeWorkspace(windows.constLast().workspace);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 1000);
}

void ApplicationControllerTest::successfulReplacementCancelsDelayedQuit()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.quitAfterLastWindowClosed = true;
    options.quitAfterLastWindowClosedDelay = std::chrono::milliseconds(150);
    ApplicationController controller(options, harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());

    QSignalSpy quit(&controller, &ApplicationController::quitRequested);
    closeWorkspace(initial->workspace);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
    QVERIFY(controller.lifetimeController()->quitPending());

    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 1, 1000);
    QVERIFY(!controller.lifetimeController()->quitPending());
    QTest::qWait(200);
    QCOMPARE(quit.count(), 0);

    QVERIFY(controller.dispatch(ApplicationAction::Quit));
    QCOMPARE(quit.count(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
}

void ApplicationControllerTest::sourceLessActivationMatchesUpstreamInheritance()
{
    QTemporaryDir directories(
        QDir::current().filePath(
            QStringLiteral("tmp/application-activation-XXXXXX")));
    QVERIFY(directories.isValid());
    const QString focusedDirectory =
        QDir(directories.path()).filePath(QStringLiteral("focused"));
    const QString configuredDirectory =
        QDir(directories.path()).filePath(QStringLiteral("configured"));
    QVERIFY(QDir().mkpath(focusedDirectory));
    QVERIFY(QDir().mkpath(configuredDirectory));

    WindowFactoryHarness harness;
    LaunchOptions initialOptions = baseOptions(focusedDirectory);
    initialOptions.fontSize = 13.0;
    ApplicationController controller(
        initialOptions, harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());
    TerminalPane *const focusedPane = onlyPane(initial->workspace);
    QVERIFY(focusedPane != nullptr);
    QVERIFY(focusedPane->executeConfiguredAction(
        QStringLiteral("set_font_size:18")));

    LaunchOptions currentOptions = initialOptions;
    currentOptions.workingDirectory = configuredDirectory;
    currentOptions.fontSize = 11.0;
    controller.applyLaunchOptions(currentOptions);

    // Registration alone is not focus. With no pane ever focused, source-less
    // activation keeps the configured cwd instead of choosing an arbitrary
    // live workspace.
    QVERIFY(controller.activateNoCommand());
    QCOMPARE(controller.windowCount(), 2);
    TerminalWorkspace *const noFocus =
        controller.windows().constLast().workspace;
    QCOMPARE(noFocus->effectiveLaunchOptions().workingDirectory,
             configuredDirectory);

    closeWorkspace(noFocus);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 1, 1000);
    initial->window->requestActivate();
    QTRY_VERIFY_WITH_TIMEOUT(
        QGuiApplication::focusWindow() == initial->window, 1000);
    focusedPane->setFocus(false);
    focusedPane->forceActiveFocus(Qt::OtherFocusReason);
    QTRY_VERIFY_WITH_TIMEOUT(focusedPane->hasActiveFocus(), 1000);
    QCOMPARE(controller.activeWorkspace(), initial->workspace);
    QVERIFY(controller.activateNoCommand());
    QCOMPARE(controller.windowCount(), 2);
    const TerminalWorkspace *const inherited =
        controller.windows().constLast().workspace;
    QCOMPARE(inherited->effectiveLaunchOptions().workingDirectory,
             focusedDirectory);
    QCOMPARE(inherited->effectiveLaunchOptions().fontSize, 11.0);
    QVERIFY(inherited->effectiveLaunchOptions().program.isEmpty());
    QVERIFY(!inherited->effectiveLaunchOptions().hold);

    // If the last-focused workspace disappears while another live window is
    // hidden, the remaining workspace is not an implicit focus substitute.
    QQuickWindow *const remainingWindow =
        controller.windows().constLast().window;
    remainingWindow->setFlag(Qt::WindowDoesNotAcceptFocus, true);
    remainingWindow->hide();
    initial->window->requestActivate();
    QTRY_VERIFY_WITH_TIMEOUT(
        QGuiApplication::focusWindow() == initial->window, 1000);
    QSignalSpy workspaceActivation(
        initial->workspace, &TerminalWorkspace::workspaceActivated);
    focusedPane->setFocus(false);
    focusedPane->forceActiveFocus(Qt::OtherFocusReason);
    QTRY_VERIFY_WITH_TIMEOUT(focusedPane->hasActiveFocus(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(workspaceActivation.count(), 1, 1000);
    QPointer<QQuickWindow> destroyedWindow(initial->window);
    delete initial->window;
    QVERIFY(destroyedWindow.isNull());
    QCOMPARE(controller.windowCount(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(QGuiApplication::focusWindow() == nullptr, 1000);
    QVERIFY(controller.activateNoCommand());
    QCOMPARE(controller.windowCount(), 2);
    const TerminalWorkspace *const staleFocus =
        controller.windows().constLast().workspace;
    QCOMPARE(staleFocus->effectiveLaunchOptions().workingDirectory,
             configuredDirectory);

    currentOptions.windowInheritWorkingDirectory = false;
    controller.applyLaunchOptions(currentOptions);
    QVERIFY(controller.activateNoCommand());
    QCOMPARE(controller.windowCount(), 3);
    const TerminalWorkspace *const configured =
        controller.windows().constLast().workspace;
    QCOMPARE(configured->effectiveLaunchOptions().workingDirectory,
             configuredDirectory);
    QCOMPARE(configured->effectiveLaunchOptions().fontSize, 11.0);
}

void ApplicationControllerTest::sourceLessActivationCancelsQuitAndReportsFailure()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.quitAfterLastWindowClosed = true;
    options.quitAfterLastWindowClosedDelay = std::chrono::milliseconds(150);
    ApplicationController controller(options, harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());

    QSignalSpy quit(&controller, &ApplicationController::quitRequested);
    closeWorkspace(initial->workspace);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
    QVERIFY(controller.lifetimeController()->quitPending());
    QVERIFY(controller.activateNoCommand());
    QCOMPARE(controller.windowCount(), 1);
    QVERIFY(!controller.lifetimeController()->quitPending());
    QTest::qWait(200);
    QCOMPARE(quit.count(), 0);

    WindowFactoryHarness failingHarness;
    failingHarness.failOnCall = 2;
    LaunchOptions residentOptions = baseOptions(QDir::currentPath());
    ApplicationController failing(
        residentOptions, failingHarness.factory(), false);
    QVERIFY(failing.createInitialWindow().has_value());
    QSignalSpy failure(&failing,
                       &ApplicationController::windowCreationFailed);
    QVERIFY(!failing.activateNoCommand());
    QCOMPARE(failure.count(), 1);
    QCOMPARE(failing.windowCount(), 1);
}

void ApplicationControllerTest::explicitQuitAggregatesEveryWindowIntoOneConfirmation()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    ApplicationController controller(options, harness.factory(), false);
    const auto first = controller.createInitialWindow();
    QVERIFY(first.has_value());
    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 1000);
    const ApplicationWindow second = controller.windows().constLast();
    QCOMPARE(controller.activeWorkspace(), second.workspace);

    TerminalPane *const guardedBackground = onlyPane(first->workspace);
    QVERIFY(guardedBackground != nullptr);
    QVERIFY(guardedBackground->executeConfiguredAction(
        QStringLiteral("toggle_readonly")));

    QSignalSpy firstConfirmation(
        first->workspace, &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy secondConfirmation(
        second.workspace, &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy firstClose(
        first->workspace, &TerminalWorkspace::windowCloseApproved);
    QSignalSpy secondClose(
        second.workspace, &TerminalWorkspace::windowCloseApproved);
    QSignalSpy committed(&controller,
                         &ApplicationController::applicationQuitCommitted);
    QSignalSpy quit(&controller, &ApplicationController::quitRequested);
    QSignalSpy creationFailure(
        &controller, &ApplicationController::windowCreationFailed);

    QVERIFY(controller.dispatch(ApplicationAction::Quit, second.workspace));
    QCOMPARE(firstConfirmation.count(), 0);
    QCOMPARE(secondConfirmation.count(), 1);
    QCOMPARE(firstClose.count(), 0);
    QCOMPARE(secondClose.count(), 0);
    QCOMPARE(secondConfirmation.constFirst().at(1).toString(),
             QStringLiteral(
                 "A terminal window contains a read-only pane. Quit the application?"));
    QVERIFY(!controller.activateNoCommand());
    QCOMPARE(creationFailure.count(), 1);
    QCOMPARE(controller.windowCount(), 2);

    const quint64 cancelledId =
        secondConfirmation.constFirst().constFirst().toULongLong();
    second.workspace->cancelClose(cancelledId);
    QCOMPARE(controller.windowCount(), 2);
    QCOMPARE(firstClose.count(), 0);
    QCOMPARE(secondClose.count(), 0);
    QCOMPARE(committed.count(), 0);

    QVERIFY(controller.dispatch(ApplicationAction::Quit, second.workspace));
    QCOMPARE(secondConfirmation.count(), 2);
    const quint64 acceptedId =
        secondConfirmation.constLast().constFirst().toULongLong();
    QVERIFY(acceptedId != cancelledId);
    second.workspace->confirmClose(acceptedId);
    QCOMPARE(firstClose.count(), 1);
    QCOMPARE(secondClose.count(), 1);
    QCOMPARE(committed.count(), 1);
    QCOMPARE(quit.count(), 1);
    QVERIFY(!controller.activateNoCommand());
    QCOMPARE(creationFailure.count(), 2);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
}

void ApplicationControllerTest::pendingQuitRehostsAfterItsWindowDisappears()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    ApplicationController controller(options, harness.factory(), false);
    const auto first = controller.createInitialWindow();
    QVERIFY(first.has_value());
    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 1000);
    const ApplicationWindow second = controller.windows().constLast();

    TerminalPane *const guardedBackground = onlyPane(first->workspace);
    QVERIFY(guardedBackground != nullptr);
    QVERIFY(guardedBackground->executeConfiguredAction(
        QStringLiteral("toggle_readonly")));

    QSignalSpy firstConfirmation(
        first->workspace, &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy secondConfirmation(
        second.workspace, &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy committed(&controller,
                         &ApplicationController::applicationQuitCommitted);
    QSignalSpy quit(&controller, &ApplicationController::quitRequested);

    QVERIFY(controller.dispatch(ApplicationAction::Quit, second.workspace));
    QCOMPARE(secondConfirmation.count(), 1);
    QCOMPARE(firstConfirmation.count(), 0);

    QPointer<QQuickWindow> removedHost(second.window);
    delete second.window;
    QVERIFY(removedHost.isNull());
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(firstConfirmation.count(), 1, 1000);
    QCOMPARE(committed.count(), 0);
    QCOMPARE(firstConfirmation.constFirst().at(1).toString(),
             QStringLiteral(
                 "A terminal window contains a read-only pane. Quit the application?"));

    const quint64 confirmationId =
        firstConfirmation.constFirst().constFirst().toULongLong();
    first->workspace->confirmClose(confirmationId);
    QCOMPARE(committed.count(), 1);
    QCOMPARE(quit.count(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
}

void ApplicationControllerTest::failedReplacementLeavesDelayedQuitArmed()
{
    WindowFactoryHarness harness;
    harness.failOnCall = 2;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.quitAfterLastWindowClosed = true;
    options.quitAfterLastWindowClosedDelay = std::chrono::milliseconds(200);
    ApplicationController controller(options, harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());

    QSignalSpy failure(&controller,
                       &ApplicationController::windowCreationFailed);
    QSignalSpy quit(&controller, &ApplicationController::quitRequested);
    closeWorkspace(initial->workspace);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
    QVERIFY(controller.lifetimeController()->quitPending());

    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(failure.count(), 1, 1000);
    QVERIFY(controller.lifetimeController()->quitPending());
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 1000);
}

QTEST_MAIN(ApplicationControllerTest)

#include "test_application_controller.moc"
