#include "application_lifetime.h"

#include <QGuiApplication>
#include <QSignalSpy>
#include <QTest>
#include <QWindow>

#include <chrono>
#include <memory>
#include <optional>

namespace {

using namespace std::chrono_literals;

LaunchOptions lifetimeOptions(
    bool quitAfterLastWindowClosed,
    std::optional<std::chrono::milliseconds> delay = std::nullopt)
{
    LaunchOptions options;
    options.quitAfterLastWindowClosed = quitAfterLastWindowClosed;
    options.quitAfterLastWindowClosedDelay = delay;
    return options;
}

void showPrimaryWindow(ApplicationLifetimeController &controller,
                       QWindow &window)
{
    QVERIFY(controller.registerWindow(&window));
    window.show();
    QTRY_VERIFY(controller.hasOpenWindow());
}

} // namespace

class ApplicationLifetimeTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void ignoresLastWindowSignalUntilAWindowWasVisible_data();
    void ignoresLastWindowSignalUntilAWindowWasVisible();
    void disabledPolicyKeepsTheApplicationResident();
    void immediatePolicyQueuesOneQuit();
    void openingAWindowCancelsThePendingDelay();
    void multipleWindowsWaitForTheGlobalLastWindowSignal();
    void identicalReloadDoesNotRestartTheDelay();
    void changedReloadReconcilesAndInvalidatesStaleTimers();
    void explicitQuitBypassesPolicyAndLatches();
    void rejectsDuplicateAndTransientWindows();
};

void ApplicationLifetimeTest::initTestCase()
{
    qGuiApp->setQuitOnLastWindowClosed(false);
}

void ApplicationLifetimeTest::ignoresLastWindowSignalUntilAWindowWasVisible()
{
    QFETCH(bool, enabled);
    QFETCH(int, delayMilliseconds);

    ApplicationLifetimeController controller;
    controller.applyLaunchOptions(lifetimeOptions(
        enabled,
        delayMilliseconds < 0
            ? std::nullopt
            : std::optional(std::chrono::milliseconds(delayMilliseconds))));
    QSignalSpy quit(&controller,
                    &ApplicationLifetimeController::quitRequested);

    controller.lastWindowClosed();
    QTest::qWait(30);
    QCOMPARE(quit.count(), 0);
    QVERIFY(!controller.quitPending());
}

void ApplicationLifetimeTest::ignoresLastWindowSignalUntilAWindowWasVisible_data()
{
    QTest::addColumn<bool>("enabled");
    QTest::addColumn<int>("delayMilliseconds");

    QTest::newRow("disabled") << false << 1;
    QTest::newRow("immediate") << true << -1;
    QTest::newRow("delayed") << true << 1;
}

void ApplicationLifetimeTest::disabledPolicyKeepsTheApplicationResident()
{
    ApplicationLifetimeController controller;
    controller.applyLaunchOptions(lifetimeOptions(false, 1ms));
    QWindow window;
    showPrimaryWindow(controller, window);
    QSignalSpy quit(&controller,
                    &ApplicationLifetimeController::quitRequested);

    window.hide();
    controller.lastWindowClosed();
    QTest::qWait(30);

    QCOMPARE(quit.count(), 0);
    QVERIFY(!controller.hasOpenWindow());
    QVERIFY(!controller.quitPending());
}

void ApplicationLifetimeTest::immediatePolicyQueuesOneQuit()
{
    ApplicationLifetimeController controller;
    QWindow window;
    showPrimaryWindow(controller, window);
    QSignalSpy quit(&controller,
                    &ApplicationLifetimeController::quitRequested);

    window.hide();
    controller.lastWindowClosed();
    QVERIFY(controller.quitPending());
    QCOMPARE(quit.count(), 0);
    QTRY_COMPARE(quit.count(), 1);
    QVERIFY(controller.hasRequestedQuit());
    QVERIFY(!controller.quitPending());

    controller.lastWindowClosed();
    controller.requestQuitNow();
    QTest::qWait(20);
    QCOMPARE(quit.count(), 1);
}

void ApplicationLifetimeTest::openingAWindowCancelsThePendingDelay()
{
    ApplicationLifetimeController controller;
    controller.applyLaunchOptions(lifetimeOptions(true, 80ms));
    QWindow firstWindow;
    showPrimaryWindow(controller, firstWindow);
    QSignalSpy quit(&controller,
                    &ApplicationLifetimeController::quitRequested);

    firstWindow.hide();
    controller.lastWindowClosed();
    QVERIFY(controller.quitPending());

    QWindow replacementWindow;
    showPrimaryWindow(controller, replacementWindow);
    QVERIFY(!controller.quitPending());
    QTest::qWait(110);
    QCOMPARE(quit.count(), 0);

    replacementWindow.hide();
    controller.lastWindowClosed();
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 250);
}

void ApplicationLifetimeTest::multipleWindowsWaitForTheGlobalLastWindowSignal()
{
    ApplicationLifetimeController controller;
    controller.applyLaunchOptions(lifetimeOptions(true, 10ms));
    QWindow firstWindow;
    QWindow secondWindow;
    showPrimaryWindow(controller, firstWindow);
    showPrimaryWindow(controller, secondWindow);
    QSignalSpy quit(&controller,
                    &ApplicationLifetimeController::quitRequested);

    firstWindow.hide();
    QTest::qWait(30);
    QCOMPARE(quit.count(), 0);
    QVERIFY(controller.hasOpenWindow());

    secondWindow.hide();
    controller.lastWindowClosed();
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 200);
    QVERIFY(!controller.hasOpenWindow());
}

void ApplicationLifetimeTest::identicalReloadDoesNotRestartTheDelay()
{
    ApplicationLifetimeController controller;
    const LaunchOptions options = lifetimeOptions(true, 300ms);
    controller.applyLaunchOptions(options);
    QWindow window;
    showPrimaryWindow(controller, window);
    QSignalSpy quit(&controller,
                    &ApplicationLifetimeController::quitRequested);

    window.hide();
    controller.lastWindowClosed();
    QTest::qWait(180);
    controller.applyLaunchOptions(options);

    // The original deadline has about 120 ms left. Restarting it here would
    // push delivery another 180 ms beyond this assertion window.
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 220);
}

void ApplicationLifetimeTest::changedReloadReconcilesAndInvalidatesStaleTimers()
{
    ApplicationLifetimeController controller;
    controller.applyLaunchOptions(lifetimeOptions(true, 80ms));
    QWindow window;
    showPrimaryWindow(controller, window);
    QSignalSpy quit(&controller,
                    &ApplicationLifetimeController::quitRequested);

    window.hide();
    controller.lastWindowClosed();
    QTest::qWait(30);
    controller.applyLaunchOptions(lifetimeOptions(true, 180ms));
    QTest::qWait(90);
    QCOMPARE(quit.count(), 0);
    QVERIFY(controller.quitPending());

    controller.applyLaunchOptions(lifetimeOptions(false, 1ms));
    QVERIFY(!controller.quitPending());
    QTest::qWait(190);
    QCOMPARE(quit.count(), 0);

    controller.applyLaunchOptions(lifetimeOptions(true, 30ms));
    QVERIFY(controller.quitPending());
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 200);
}

void ApplicationLifetimeTest::explicitQuitBypassesPolicyAndLatches()
{
    ApplicationLifetimeController controller;
    controller.applyLaunchOptions(lifetimeOptions(false, 10s));
    QSignalSpy quit(&controller,
                    &ApplicationLifetimeController::quitRequested);

    controller.requestQuitNow();
    QCOMPARE(quit.count(), 1);
    QVERIFY(controller.hasRequestedQuit());
    QVERIFY(!controller.quitPending());

    controller.requestQuitNow();
    controller.applyLaunchOptions(lifetimeOptions(true, 1ms));
    controller.lastWindowClosed();
    QTest::qWait(20);
    QCOMPARE(quit.count(), 1);
}

void ApplicationLifetimeTest::rejectsDuplicateAndTransientWindows()
{
    ApplicationLifetimeController controller;
    auto primary = std::make_unique<QWindow>();
    QVERIFY(controller.registerWindow(primary.get()));
    QVERIFY(!controller.registerWindow(primary.get()));

    QWindow transient;
    transient.setTransientParent(primary.get());
    QVERIFY(!controller.registerWindow(&transient));
    QCOMPARE(controller.registeredWindowCount(), 1);

    primary.reset();
    QCOMPARE(controller.registeredWindowCount(), 0);
}

QTEST_MAIN(ApplicationLifetimeTest)

#include "test_application_lifetime.moc"
