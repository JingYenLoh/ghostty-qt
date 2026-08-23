#pragma once

#include "terminal/rendering/terminal_backdrop.h"

// Internal post-decode preparation seam used by the loader and renderer
// microbenchmark. Passing an unshared image permits the RGBA8888 fast path to
// transfer its storage without copying.
[[nodiscard]] std::expected<TerminalBackgroundImageAsset, QString>
prepareTerminalBackgroundImage(QImage source, quint64 serial);

// Test-only request seam. The hook runs on the background worker after the
// source has been opened, identified, and registered as the load leader but
// immediately before decode, allowing replacement races to be synchronized
// without changing production request behavior.
using TerminalBackgroundImageOpenedHook = std::move_only_function<void()>;

[[nodiscard]] TerminalBackgroundImageRequestHandle
requestTerminalBackgroundImageForTest(
    const TerminalBackgroundImageRequest &request, QObject *receiver,
    TerminalBackgroundImageCallback callback,
    TerminalBackgroundImageOpenedHook openedHook);
