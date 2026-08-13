#pragma once

#include <QMetaType>

// Ghostty has a process-level action scope distinct from surface/workspace
// actions. Keeping that vocabulary typed prevents a caller from accidentally
// interpreting quit, reload, or new-window against only one terminal window.
enum class ApplicationAction {
    Ignore,
    DeprecatedCloseAllWindows,
    NewWindow,
    OpenConfig,
    OpenConfigNewWindow,
    ReloadConfig,
    ToggleQuickTerminal,
    Quit,
};

Q_DECLARE_METATYPE(ApplicationAction)
