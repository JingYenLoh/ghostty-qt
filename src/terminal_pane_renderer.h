#pragma once

#include <QtGlobal>

class TerminalPane;
class QQuickItem;

#ifdef GHOSTTY_QT_RENDER_TEST_PROBE

#include "terminal_session_options.h"

#include <optional>

#endif

namespace TerminalPaneRenderer {

#ifdef GHOSTTY_QT_RENDER_TEST_PROBE

void clearProbe(const TerminalPane *pane);
void publishInitialGeometryProbe(
    const TerminalPane *pane,
    const std::optional<TerminalSessionGeometry> &geometry);

#endif

[[nodiscard]] QQuickItem *createRenderItem(TerminalPane *pane);

inline constexpr qreal linkPreviewHorizontalPadding = 8.0;
inline constexpr qreal linkPreviewVerticalPadding = 4.0;

[[nodiscard]] qreal normalizedDevicePixelRatio(qreal value) noexcept;
[[nodiscard]] qint64 physicalPixels(qreal logicalPixels,
                                    qreal devicePixelRatio) noexcept;

} // namespace TerminalPaneRenderer
