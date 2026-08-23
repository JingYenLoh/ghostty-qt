#include "desktop/quick_terminal_surface.h"

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

LayerWindow::Layer layerShellLayer(QuickTerminalLayer layer) noexcept
{
    switch (layer) {
    case QuickTerminalLayer::Background: return LayerWindow::LayerBackground;
    case QuickTerminalLayer::Bottom: return LayerWindow::LayerBottom;
    case QuickTerminalLayer::Top: return LayerWindow::LayerTop;
    case QuickTerminalLayer::Overlay: return LayerWindow::LayerOverlay;
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
    QWindow &window, const QuickTerminalOptions &options,
    const QuickTerminalLayerShellOptions &layerShellOptions,
    QScreen &sizingScreen)
{
    if (window.isVisible()) {
        return std::unexpected(QStringLiteral(
            "The quick-terminal layer-shell surface must be attached before "
            "the window is shown"));
    }
    if (window.handle() != nullptr) {
        return std::unexpected(QStringLiteral(
            "The quick-terminal layer-shell surface must be attached before "
            "the native platform surface is created"));
    }

    LayerWindow *const layerShellWindow = LayerWindow::get(&window);
    if (layerShellWindow == nullptr) {
        return std::unexpected(QStringLiteral(
            "LayerShellQt could not attach to the quick-terminal window"));
    }

    auto surface = std::unique_ptr<QuickTerminalSurface>(
        new QuickTerminalSurface(window, *layerShellWindow));
    surface->applyStaticOptions();
    if (auto synchronized =
            surface->syncOptions(options, layerShellOptions, sizingScreen);
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

std::expected<void, QString> QuickTerminalSurface::syncOptions(
    const QuickTerminalOptions &options,
    const QuickTerminalLayerShellOptions &layerShellOptions,
    QScreen &sizingScreen)
{
    if (window_.isNull() || layerShellWindow_.isNull()) {
        return std::unexpected(destroyedSurfaceError());
    }

    if (initialized_ && options_ == options
        && layerShellOptions_ == layerShellOptions
        && sizingScreen_ == &sizingScreen) {
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
    const bool layerChanged =
        firstSync || layerShellOptions_.layer != layerShellOptions.layer;
    const bool namespaceChanged = firstSync
        || layerShellOptions_.layerNamespace
            != layerShellOptions.layerNamespace;
    const bool sizeChanged = firstSync || outputChanged || placementChanged
        || options_.size != options.size;

    options_ = options;
    layerShellOptions_ = layerShellOptions;
    initialized_ = true;

    if (layerChanged) applyLayer(layerShellOptions.layer);
    if (namespaceChanged) {
        layerShellWindow_->setScope(layerShellOptions.layerNamespace);
    }
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
    // The requested size is calculated from the complete output geometry.
    // Zone zero would make the compositor place the surface inside the area
    // left by exclusive layer surfaces. For an edge quick terminal whose
    // unanchored dimension spans the output, a panel on one side then makes
    // the compositor center an output-sized window in a smaller area and
    // shifts half of the panel extent off-screen. Zone -1 neither reserves
    // space nor moves the quick terminal around panels.
    layerShellWindow->setExclusiveZone(-1);
    layerShellWindow->setCloseOnDismissed(false);
}

void QuickTerminalSurface::applyLayer(QuickTerminalLayer layer)
{
    LayerWindow *const layerShellWindow = layerShellWindow_.data();
    Q_ASSERT(layerShellWindow != nullptr);
    layerShellWindow->setLayer(layerShellLayer(layer));
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
        layerShellWindow->setScreen(nullptr);
        layerShellWindow->setWantsToBeOnActiveScreen(true);
        break;
    case QuickTerminalScreen::Main:
    case QuickTerminalScreen::MacosMenuBar:
        layerShellWindow->setWantsToBeOnActiveScreen(false);
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
