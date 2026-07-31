#pragma once

#include "terminal_backdrop.h"

// Internal post-decode preparation seam used by the loader and renderer
// microbenchmark. Passing an unshared image permits the RGBA8888 fast path to
// transfer its storage without copying.
[[nodiscard]] std::expected<TerminalBackgroundImageAsset, QString>
prepareTerminalBackgroundImage(QImage source, quint64 serial);
