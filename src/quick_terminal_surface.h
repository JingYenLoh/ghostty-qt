#pragma once

#include "quick_terminal.h"

#include <QObject>
#include <QPointer>
#include <QString>

#include <expected>
#include <memory>

class QScreen;
class QWindow;

namespace LayerShellQt {
class Window;
}

// Owns the LayerShellQt attachment policy for one quick-terminal window.
//
// Create this before the QWindow is shown. LayerShellQt::Window itself remains
// owned by QWindow; this object owns the synchronization lifetime and can
// therefore be held by ApplicationController in a std::unique_ptr.
class QuickTerminalSurface final : public QObject {
public:
    using CreateResult =
        std::expected<std::unique_ptr<QuickTerminalSurface>, QString>;

    [[nodiscard]] static CreateResult
    create(QWindow &window, const QuickTerminalOptions &options,
           QScreen &sizingScreen);

    ~QuickTerminalSurface() override;

    QuickTerminalSurface(const QuickTerminalSurface &) = delete;
    QuickTerminalSurface &operator=(const QuickTerminalSurface &) = delete;
    QuickTerminalSurface(QuickTerminalSurface &&) = delete;
    QuickTerminalSurface &operator=(QuickTerminalSurface &&) = delete;

    // Applies live option and output changes. The sizing screen supplies the
    // full logical output geometry. Mouse mode still asks the compositor to
    // choose the active output instead of binding the layer surface to it.
    [[nodiscard]] std::expected<void, QString>
    syncOptions(const QuickTerminalOptions &options, QScreen &sizingScreen);

    // Read-only access is intentionally exposed for diagnostics and tests.
    // LayerShellQt::Window remains owned by the attached QWindow.
    [[nodiscard]] const LayerShellQt::Window *layerShellWindow() const noexcept;

private:
    QuickTerminalSurface(QWindow &window,
                         LayerShellQt::Window &layerShellWindow);

    void applyStaticOptions();
    void applyPlacement(QuickTerminalPosition position);
    void applyScreen(QuickTerminalScreen selection, QScreen &sizingScreen);
    void applyKeyboard(QuickTerminalKeyboardInteractivity interactivity);
    void applyDesiredSize();
    void bindSizingScreen(QScreen &screen);

    QPointer<QWindow> window_;
    QPointer<LayerShellQt::Window> layerShellWindow_;
    QPointer<QScreen> sizingScreen_;
    QMetaObject::Connection sizingScreenGeometryConnection_;
    QuickTerminalOptions options_;
    bool initialized_ = false;
};
