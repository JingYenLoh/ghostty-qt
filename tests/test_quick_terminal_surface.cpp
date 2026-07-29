#include "quick_terminal_surface.h"

#include <LayerShellQt/Window>

#include <QGuiApplication>
#include <QScreen>
#include <QSignalSpy>
#include <QTest>
#include <QWindow>

#include <array>
#include <expected>
#include <memory>

namespace {

using LayerWindow = LayerShellQt::Window;

std::unique_ptr<QuickTerminalSurface>
createSurface(QWindow &window, const QuickTerminalOptions &options,
              QScreen &screen)
{
    QuickTerminalSurface::CreateResult created =
        QuickTerminalSurface::create(window, options, screen);
    if (!created) {
        qFatal("Failed to create a quick-terminal surface: %s",
               qPrintable(created.error()));
    }
    return std::move(*created);
}

QScreen &primaryScreen()
{
    QScreen *const screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) qFatal("The GUI test platform has no screen");
    return *screen;
}

} // namespace

class QuickTerminalSurfaceTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void configuresInvariantLayerProperties();
    void mapsEveryPlacement();
    void mapsEveryKeyboardMode();
    void mapsEveryScreenMode();
    void calculatesEveryPositionFromFullLogicalGeometry();
    void unchangedSyncIsANoOp();
    void liveSyncUpdatesOnlyRelevantLayerProperties();
    void rejectsSynchronizationAfterWindowDestruction();
};

void QuickTerminalSurfaceTest::configuresInvariantLayerProperties()
{
    QWindow window;
    QuickTerminalOptions options;
    const std::unique_ptr<QuickTerminalSurface> surface =
        createSurface(window, options, primaryScreen());
    const LayerWindow *const layer = surface->layerShellWindow();

    QVERIFY(layer != nullptr);
    QCOMPARE(layer, LayerWindow::get(&window));
    QCOMPARE(layer->layer(), LayerWindow::LayerTop);
    QCOMPARE(layer->scope(), QStringLiteral("ghostty-quick-terminal"));
    QCOMPARE(layer->exclusionZone(), 0);
    QVERIFY(!layer->closeOnDismissed());
    QVERIFY(!window.isVisible());
}

void QuickTerminalSurfaceTest::mapsEveryPlacement()
{
    struct Case {
        QuickTerminalPosition position;
        LayerWindow::Anchors anchors;
        QMargins margins;
    };
    const std::array cases{
        Case{QuickTerminalPosition::Top, LayerWindow::AnchorTop,
             QMargins(20, 0, 20, 20)},
        Case{QuickTerminalPosition::Bottom, LayerWindow::AnchorBottom,
             QMargins(20, 20, 20, 0)},
        Case{QuickTerminalPosition::Left, LayerWindow::AnchorLeft,
             QMargins(0, 20, 20, 20)},
        Case{QuickTerminalPosition::Right, LayerWindow::AnchorRight,
             QMargins(20, 20, 0, 20)},
        Case{QuickTerminalPosition::Center, LayerWindow::AnchorNone,
             QMargins(20, 20, 20, 20)},
    };

    for (const Case &test : cases) {
        QWindow window;
        QuickTerminalOptions options;
        options.position = test.position;
        const std::unique_ptr<QuickTerminalSurface> surface =
            createSurface(window, options, primaryScreen());
        const LayerWindow *const layer = surface->layerShellWindow();

        QCOMPARE(layer->anchors(), test.anchors);
        QCOMPARE(layer->margins(), test.margins);
    }
}

void QuickTerminalSurfaceTest::mapsEveryKeyboardMode()
{
    struct Case {
        QuickTerminalKeyboardInteractivity option;
        LayerWindow::KeyboardInteractivity layer;
        bool activateOnShow;
    };
    constexpr std::array cases{
        Case{QuickTerminalKeyboardInteractivity::None,
             LayerWindow::KeyboardInteractivityNone, false},
        Case{QuickTerminalKeyboardInteractivity::OnDemand,
             LayerWindow::KeyboardInteractivityOnDemand, true},
        Case{QuickTerminalKeyboardInteractivity::Exclusive,
             LayerWindow::KeyboardInteractivityExclusive, true},
    };

    QWindow window;
    QuickTerminalOptions options;
    std::unique_ptr<QuickTerminalSurface> surface =
        createSurface(window, options, primaryScreen());
    for (const Case &test : cases) {
        options.keyboardInteractivity = test.option;
        QVERIFY(surface->syncOptions(options, primaryScreen()));
        const LayerWindow *const layer = surface->layerShellWindow();
        QCOMPARE(layer->keyboardInteractivity(), test.layer);
        QCOMPARE(layer->activateOnShow(), test.activateOnShow);
    }
}

void QuickTerminalSurfaceTest::mapsEveryScreenMode()
{
    QScreen &screen = primaryScreen();
    QWindow window;
    QuickTerminalOptions options;
    std::unique_ptr<QuickTerminalSurface> surface =
        createSurface(window, options, screen);

    const std::array explicitSelections{
        QuickTerminalScreen::Main,
        QuickTerminalScreen::MacosMenuBar,
    };
    for (const QuickTerminalScreen selection : explicitSelections) {
        options.screen = selection;
        QVERIFY(surface->syncOptions(options, screen));
        const LayerWindow *const layer = surface->layerShellWindow();
        QCOMPARE(layer->screen(), &screen);
        QVERIFY(!layer->wantsToBeOnActiveScreen());
    }

    options.screen = QuickTerminalScreen::Mouse;
    QVERIFY(surface->syncOptions(options, screen));
    const LayerWindow *const mouseLayer = surface->layerShellWindow();
    QCOMPARE(mouseLayer->screen(), nullptr);
    QVERIFY(mouseLayer->wantsToBeOnActiveScreen());

    options.screen = QuickTerminalScreen::Main;
    QVERIFY(surface->syncOptions(options, screen));
    const LayerWindow *const mainLayer = surface->layerShellWindow();
    QCOMPARE(mainLayer->screen(), &screen);
    QVERIFY(!mainLayer->wantsToBeOnActiveScreen());
}

void QuickTerminalSurfaceTest::calculatesEveryPositionFromFullLogicalGeometry()
{
    QScreen &screen = primaryScreen();
    QWindow window;
    QuickTerminalOptions options;
    options.size.primary = QuickTerminalPercentage{37.5F};
    options.size.secondary = QuickTerminalPixels{263};
    std::unique_ptr<QuickTerminalSurface> surface =
        createSurface(window, options, screen);

    constexpr std::array positions{
        QuickTerminalPosition::Top,    QuickTerminalPosition::Bottom,
        QuickTerminalPosition::Left,   QuickTerminalPosition::Right,
        QuickTerminalPosition::Center,
    };
    for (const QuickTerminalPosition position : positions) {
        options.position = position;
        QVERIFY(surface->syncOptions(options, screen));
        QCOMPARE(surface->layerShellWindow()->desiredSize(),
                 quickTerminalSize(options.size, position,
                                   screen.geometry().size()));
    }
}

void QuickTerminalSurfaceTest::unchangedSyncIsANoOp()
{
    QScreen &screen = primaryScreen();
    QWindow window;
    QuickTerminalOptions options;
    const std::unique_ptr<QuickTerminalSurface> surface =
        createSurface(window, options, screen);
    const LayerWindow *const layer = surface->layerShellWindow();
    QSignalSpy anchors(layer, &LayerWindow::anchorsChanged);
    QSignalSpy margins(layer, &LayerWindow::marginsChanged);
    QSignalSpy desiredSize(layer, &LayerWindow::desiredSizeChanged);
    QSignalSpy keyboard(layer, &LayerWindow::keyboardInteractivityChanged);
    QSignalSpy activeScreen(layer,
                            &LayerWindow::wantsToBeOnActiveScreenChanged);
    QSignalSpy explicitScreen(layer, &LayerWindow::screenChanged);

    QVERIFY(surface->syncOptions(options, screen));

    QCOMPARE(anchors.count(), 0);
    QCOMPARE(margins.count(), 0);
    QCOMPARE(desiredSize.count(), 0);
    QCOMPARE(keyboard.count(), 0);
    QCOMPARE(activeScreen.count(), 0);
    QCOMPARE(explicitScreen.count(), 0);
}

void QuickTerminalSurfaceTest::liveSyncUpdatesOnlyRelevantLayerProperties()
{
    QScreen &screen = primaryScreen();
    QWindow window;
    QuickTerminalOptions options;
    const std::unique_ptr<QuickTerminalSurface> surface =
        createSurface(window, options, screen);
    const LayerWindow *const layer = surface->layerShellWindow();
    QSignalSpy anchors(layer, &LayerWindow::anchorsChanged);
    QSignalSpy margins(layer, &LayerWindow::marginsChanged);
    QSignalSpy desiredSize(layer, &LayerWindow::desiredSizeChanged);
    QSignalSpy keyboard(layer, &LayerWindow::keyboardInteractivityChanged);
    QSignalSpy activeScreen(layer,
                            &LayerWindow::wantsToBeOnActiveScreenChanged);
    QSignalSpy explicitScreen(layer, &LayerWindow::screenChanged);

    options.autohide = true;
    QVERIFY(surface->syncOptions(options, screen));
    QCOMPARE(anchors.count(), 0);
    QCOMPARE(margins.count(), 0);
    QCOMPARE(desiredSize.count(), 0);
    QCOMPARE(keyboard.count(), 0);
    QCOMPARE(activeScreen.count(), 0);
    QCOMPARE(explicitScreen.count(), 0);

    options.position = QuickTerminalPosition::Right;
    options.size.primary = QuickTerminalPixels{537};
    options.keyboardInteractivity = QuickTerminalKeyboardInteractivity::None;
    options.screen = QuickTerminalScreen::Mouse;
    QVERIFY(surface->syncOptions(options, screen));

    QCOMPARE(layer->anchors(), LayerWindow::Anchors(LayerWindow::AnchorRight));
    QCOMPARE(layer->margins(), QMargins(20, 20, 0, 20));
    QCOMPARE(layer->desiredSize(),
             quickTerminalSize(options.size, options.position,
                               screen.geometry().size()));
    QCOMPARE(layer->keyboardInteractivity(),
             LayerWindow::KeyboardInteractivityNone);
    QVERIFY(!layer->activateOnShow());
    QCOMPARE(layer->screen(), nullptr);
    QVERIFY(layer->wantsToBeOnActiveScreen());
    QVERIFY(anchors.count() > 0);
    QVERIFY(margins.count() > 0);
    QVERIFY(desiredSize.count() > 0);
    QVERIFY(keyboard.count() > 0);
}

void QuickTerminalSurfaceTest::rejectsSynchronizationAfterWindowDestruction()
{
    QScreen &screen = primaryScreen();
    auto window = std::make_unique<QWindow>();
    std::unique_ptr<QuickTerminalSurface> surface =
        createSurface(*window, QuickTerminalOptions{}, screen);

    window.reset();
    QVERIFY(surface->layerShellWindow() == nullptr);
    const std::expected<void, QString> synchronized =
        surface->syncOptions(QuickTerminalOptions{}, screen);
    QVERIFY(!synchronized);
    QVERIFY(
        synchronized.error().contains(QStringLiteral("window was destroyed")));
}

QTEST_MAIN(QuickTerminalSurfaceTest)

#include "test_quick_terminal_surface.moc"
