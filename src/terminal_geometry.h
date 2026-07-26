#pragma once

#include "terminal_session_options.h"

#include <QtGlobal>

#include <optional>

// Converts the authoritative logical pane viewport into the complete
// device-pixel geometry used by libghostty and projected into the Linux PTY
// winsize. Invalid or not-yet-laid-out viewports deliberately retain the
// worker's legacy fallback.
[[nodiscard]] std::optional<TerminalSessionGeometry>
terminalSessionGeometryForViewport(qreal width, qreal height, qreal cellWidth,
                                   qreal cellHeight,
                                   qreal devicePixelRatio) noexcept;
