#include "window_blur_controller.h"

#include <QCoreApplication>
#include <QPlatformSurfaceEvent>
#include <QTest>
#include <QWindow>

class WindowBlurControllerTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void appliesEnabledStateOnlyAfterSurfaceCreation();
    void retriesUnavailableEffectsWithoutCachingFailure();
};

void WindowBlurControllerTest::appliesEnabledStateOnlyAfterSurfaceCreation()
{
    QWindow window;
    QList<bool> requests;
    QList<QWindow *> targets;
    WindowBlurController controller(
        &window, [&requests, &targets](QWindow *target, bool enabled) {
            targets.append(target);
            requests.append(enabled);
            return true;
        });

    controller.setBlur(20);
    QCOMPARE(controller.blur(), qint16{20});
    QVERIFY(controller.enabled());
    QVERIFY(requests.isEmpty());

    QPlatformSurfaceEvent created(QPlatformSurfaceEvent::SurfaceCreated);
    QCoreApplication::sendEvent(&window, &created);
    QCOMPARE(requests, QList<bool>{true});
    QCOMPARE(targets, QList<QWindow *>{&window});

    // KWin exposes a boolean request, so changing only Ghostty's retained
    // radius does not repeat an identical compositor operation.
    controller.setBlur(42);
    QCOMPARE(controller.blur(), qint16{42});
    QCOMPARE(requests, QList<bool>{true});

    controller.setBlur(0);
    QCOMPARE(requests, QList<bool>({true, false}));
    controller.setBlur(-1);
    QCOMPARE(requests, QList<bool>({true, false, true}));
    controller.setBlur(-2);
    QCOMPARE(requests, QList<bool>({true, false, true}));

    QPlatformSurfaceEvent destroyed(
        QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);
    QCoreApplication::sendEvent(&window, &destroyed);
    controller.setBlur(0);
    QCOMPARE(requests, QList<bool>({true, false, true}));

    QCoreApplication::sendEvent(&window, &created);
    QCOMPARE(requests, QList<bool>({true, false, true, false}));
    QCOMPARE(targets, QList<QWindow *>({&window, &window, &window, &window}));
}

void WindowBlurControllerTest::retriesUnavailableEffectsWithoutCachingFailure()
{
    QWindow window;
    int attempts = 0;
    bool available = false;
    WindowBlurController controller(
        &window, [&attempts, &available](QWindow *, bool enabled) {
            ++attempts;
            return !enabled || available;
        });
    controller.setBlur(20);

    QPlatformSurfaceEvent created(QPlatformSurfaceEvent::SurfaceCreated);
    QCoreApplication::sendEvent(&window, &created);
    QCOMPARE(attempts, 1);

    controller.setBlur(20);
    QCOMPARE(attempts, 2);
    available = true;
    controller.setBlur(20);
    QCOMPARE(attempts, 3);

    // A successfully applied state is retained for this surface.
    controller.setBlur(20);
    QCOMPARE(attempts, 3);
}

QTEST_MAIN(WindowBlurControllerTest)

#include "test_window_blur_controller.moc"
