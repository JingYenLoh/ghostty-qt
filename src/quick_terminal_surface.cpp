#include "quick_terminal_surface.h"

#include <LayerShellQt/Window>

#include <QMargins>
#include <QScreen>
#include <QWindow>

#include <array>
#include <utility>

namespace {

using LayerWindow = LayerShellQt::Window;

LayerWindow::Anchors layerShellAnchors(Qt::Edges anchors) noexcept
{
    constexpr auto mappings =
        std::to_array<std::pair<Qt::Edge, LayerWindow::Anchor>>({
            {Qt::TopEdge, LayerWindow::AnchorTop},
            {Qt::BottomEdge, LayerWindow::AnchorBottom},
            {Qt::LeftEdge, LayerWindow::AnchorLeft},
            {Qt::RightEdge, LayerWindow::AnchorRight},
        });

    LayerWindow::Anchors result = LayerWindow::AnchorNone;
    for (const auto &[qt, layerShell] : mappings) {
        if (anchors.testFlag(qt)) result.setFlag(layerShell);
    }
    return result;
}

LayerWindow::KeyboardInteractivity
layerShellKeyboard(QuickTerminalKeyboardInteractivity interactivity) noexcept
{
    switch (interactivity) {
    case QuickTerminalKeyboardInteractivity::None:
        return LayerWindow::KeyboardInteractivityNone;
    case QuickTerminalKeyboardInteractivity::OnDemand:
        return LayerWindow::KeyboardInteractivityOnDemand;
    case QuickTerminalKeyboardInteractivity::Exclusive:
        return LayerWindow::KeyboardInteractivityExclusive;
    }
    std::unreachable();
}

QString destroyedSurfaceError()
{
    return QStringLiteral(
        "The quick-terminal window was destroyed before its layer-shell "
        "options could be synchronized");
}

} // namespace

QuickTerminalSurface::CreateResult QuickTerminalSurface::create(
    QWindow &window, const QuickTerminalOptions &options, QScreen &sizingScreen)
{
    if (window.isVisible()) {
        return std::unexpected(QStringLiteral(
            "The quick-terminal layer-shell surface must be attached before "
            "the window is shown"));
    }

    LayerWindow *const layerShellWindow = LayerWindow::get(&window);
    if (layerShellWindow == nullptr) {
        return std::unexpected(QStringLiteral(
            "LayerShellQt could not attach to the quick-terminal window"));
    }

    auto surface = std::unique_ptr<QuickTerminalSurface>(
        new QuickTerminalSurface(window, *layerShellWindow));
    surface->applyStaticOptions();
    if (auto synchronized = surface->syncOptions(options, sizingScreen);
        !synchronized) {
        return std::unexpected(std::move(synchronized.error()));
    }
    return surface;
}

QuickTerminalSurface::QuickTerminalSurface(
    QWindow &window, LayerShellQt::Window &layerShellWindow)
    : window_(&window)
    , layerShellWindow_(&layerShellWindow)
{}

QuickTerminalSurface::~QuickTerminalSurface() = default;

std::expected<void, QString>
QuickTerminalSurface::syncOptions(const QuickTerminalOptions &options,
                                  QScreen &sizingScreen)
{
    if (window_.isNull() || layerShellWindow_.isNull()) {
        return std::unexpected(destroyedSurfaceError());
    }

    if (initialized_ && options_ == options && sizingScreen_ == &sizingScreen) {
        return {};
    }

    const bool firstSync = !initialized_;
    const bool outputChanged = sizingScreen_ != &sizingScreen;
    const bool placementChanged =
        firstSync || options_.position != options.position;
    const bool selectionChanged =
        firstSync || options_.screen != options.screen;
    const bool keyboardChanged = firstSync
        || options_.keyboardInteractivity != options.keyboardInteractivity;
    const bool sizeChanged = firstSync || outputChanged || placementChanged
        || options_.size != options.size;

    options_ = options;
    initialized_ = true;

    if (outputChanged) bindSizingScreen(sizingScreen);
    if (placementChanged) applyPlacement(options.position);
    if (selectionChanged || outputChanged) {
        applyScreen(options.screen, sizingScreen);
    }
    if (keyboardChanged) applyKeyboard(options.keyboardInteractivity);
    if (sizeChanged) applyDesiredSize();
    return {};
}

const LayerShellQt::Window *
QuickTerminalSurface::layerShellWindow() const noexcept
{
    return layerShellWindow_.data();
}

void QuickTerminalSurface::applyStaticOptions()
{
    LayerWindow *const layerShellWindow = layerShellWindow_.data();
    Q_ASSERT(layerShellWindow != nullptr);
    layerShellWindow->setLayer(LayerWindow::LayerTop);
    layerShellWindow->setScope(QStringLiteral("ghostty-quick-terminal"));
    layerShellWindow->setExclusiveZone(0);
    layerShellWindow->setCloseOnDismissed(false);
}

void QuickTerminalSurface::applyPlacement(QuickTerminalPosition position)
{
    LayerWindow *const layerShellWindow = layerShellWindow_.data();
    Q_ASSERT(layerShellWindow != nullptr);
    const QuickTerminalPlacementIntent intent =
        quickTerminalPlacementIntent(position);
    layerShellWindow->setAnchors(layerShellAnchors(intent.anchors));
    layerShellWindow->setMargins(intent.margins);
}

void QuickTerminalSurface::applyScreen(QuickTerminalScreen selection,
                                       QScreen &sizingScreen)
{
    LayerWindow *const layerShellWindow = layerShellWindow_.data();
    Q_ASSERT(layerShellWindow != nullptr);
    switch (selection) {
    case QuickTerminalScreen::Mouse:
        layerShellWindow->setWantsToBeOnActiveScreen(true);
        break;
    case QuickTerminalScreen::Main:
    case QuickTerminalScreen::MacosMenuBar:
        layerShellWindow->setScreen(&sizingScreen);
        break;
    }
}

void QuickTerminalSurface::applyKeyboard(
    QuickTerminalKeyboardInteractivity interactivity)
{
    LayerWindow *const layerShellWindow = layerShellWindow_.data();
    Q_ASSERT(layerShellWindow != nullptr);
    layerShellWindow->setKeyboardInteractivity(
        layerShellKeyboard(interactivity));
    layerShellWindow->setActivateOnShow(
        interactivity != QuickTerminalKeyboardInteractivity::None);
}

void QuickTerminalSurface::applyDesiredSize()
{
    LayerWindow *const layerShellWindow = layerShellWindow_.data();
    QScreen *const screen = sizingScreen_.data();
    if (layerShellWindow == nullptr || screen == nullptr) return;

    layerShellWindow->setDesiredSize(quickTerminalSize(
        options_.size, options_.position, screen->geometry().size()));
}

void QuickTerminalSurface::bindSizingScreen(QScreen &screen)
{
    disconnect(sizingScreenGeometryConnection_);
    sizingScreen_ = &screen;
    sizingScreenGeometryConnection_ =
        connect(&screen, &QScreen::geometryChanged, this,
                [this](const QRect &) { applyDesiredSize(); });
}
