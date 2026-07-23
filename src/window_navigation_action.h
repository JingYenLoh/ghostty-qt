#pragma once

#include <QMetaType>

// Ghostty classifies goto_window as a surface action because each invocation
// originates from one terminal surface, then delegates the actual top-level
// traversal to the application frontend.
enum class WindowNavigationAction {
    Previous,
    Next,
};

Q_DECLARE_METATYPE(WindowNavigationAction)
