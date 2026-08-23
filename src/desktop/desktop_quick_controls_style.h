#pragma once

#include <QProcessEnvironment>
#include <QStringList>

// Returns the import roots Qt would ordinarily search for QML modules,
// including application-local and resource roots. The environment is injected
// so startup policy can be tested without changing process-global state.
[[nodiscard]] QStringList
desktopQuickControlsImportRoots(const QProcessEnvironment &environment =
                                    QProcessEnvironment::systemEnvironment());

// Returns true when ghostty-qt may safely choose org.kde.desktop itself. An
// existing QT_QUICK_CONTROLS_STYLE always wins, including an explicitly empty
// value. importRoots are injected to keep the selection decision deterministic
// and independent of the host running a test.
[[nodiscard]] bool
shouldSelectKdeDesktopQuickControlsStyle(const QProcessEnvironment &environment,
                                         const QStringList &importRoots);

// Convenience overload for application startup. It derives the normal import
// roots from the same environment used for desktop detection.
[[nodiscard]] bool shouldSelectKdeDesktopQuickControlsStyle(
    const QProcessEnvironment &environment =
        QProcessEnvironment::systemEnvironment());
