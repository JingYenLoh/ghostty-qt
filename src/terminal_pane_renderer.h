#pragma once

#include <QtGlobal>

#ifdef GHOSTTY_QT_RENDER_TEST_PROBE

#include "terminal_session_options.h"

#include <optional>

class TerminalPane;

#endif

namespace TerminalPaneRenderer {

#ifdef GHOSTTY_QT_RENDER_TEST_PROBE

void clearProbe(const TerminalPane *pane);
void publishInitialGeometryProbe(
    const TerminalPane *pane,
    const std::optional<TerminalSessionGeometry> &geometry);

#endif

inline constexpr qreal linkPreviewHorizontalPadding = 8.0;
inline constexpr qreal linkPreviewVerticalPadding = 4.0;

[[nodiscard]] qreal normalizedDevicePixelRatio(qreal value) noexcept;
[[nodiscard]] qint64 physicalPixels(qreal logicalPixels,
                                    qreal devicePixelRatio) noexcept;

} // namespace TerminalPaneRenderer
