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
    void ordinaryCloseUsesOnlyTheFinalWindowForLifetimePolicy();
    void successfulReplacementCancelsDelayedQuit();
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

    QVERIFY(controller.dispatch(ApplicationAction::Quit, second.workspace));
    QCOMPARE(firstConfirmation.count(), 0);
    QCOMPARE(secondConfirmation.count(), 1);
    QCOMPARE(firstClose.count(), 0);
    QCOMPARE(secondClose.count(), 0);
    QCOMPARE(secondConfirmation.constFirst().at(1).toString(),
             QStringLiteral(
                 "A terminal window contains a read-only pane. Quit the application?"));

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
