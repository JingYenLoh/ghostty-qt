#include "ghostty_application_keybindings.h"

#include "ghostty_action_catalog.h"
#include "ghostty_global_shortcut_portal.h"
#include "terminal_workspace.h"

#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQuickWindow>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace {

std::uint32_t unshiftedCodepoint(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return static_cast<std::uint32_t>('a' + key - Qt::Key_A);
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return static_cast<std::uint32_t>('0' + key - Qt::Key_0);
    }
    switch (key) {
    case Qt::Key_Space: return ' ';
    case Qt::Key_QuoteLeft:
    case Qt::Key_AsciiTilde: return '`';
    case Qt::Key_Backslash:
    case Qt::Key_Bar: return '\\';
    case Qt::Key_BracketLeft:
    case Qt::Key_BraceLeft: return '[';
    case Qt::Key_BracketRight:
    case Qt::Key_BraceRight: return ']';
    case Qt::Key_Comma:
    case Qt::Key_Less: return ',';
    case Qt::Key_Equal:
    case Qt::Key_Plus: return '=';
    case Qt::Key_Minus:
    case Qt::Key_Underscore: return '-';
    case Qt::Key_Period:
    case Qt::Key_Greater: return '.';
    case Qt::Key_Apostrophe:
    case Qt::Key_QuoteDbl: return '\'';
    case Qt::Key_Semicolon:
    case Qt::Key_Colon: return ';';
    case Qt::Key_Slash:
    case Qt::Key_Question: return '/';
    default: return 0;
    }
}

quint64 keyEventIdentity(const QKeyEvent *event)
{
    const quint64 physical = static_cast<quint64>(event->nativeScanCode());
    const quint64 logical = static_cast<quint64>(
        static_cast<quint32>(event->key()));
    return physical != 0 ? (quint64{1} << 63U) | physical : logical;
}

bool containsIgnore(const QStringList &actions)
{
    return std::ranges::any_of(actions, [](const QString &action) {
        return action == QLatin1StringView("ignore");
    });
}

bool closesEverySurface(QStringView action)
{
    // Other/right retain their originating tab during ordinary per-surface
    // fanout. Only actions that close the source itself converge on quit.
    return action == QLatin1StringView("close_surface")
        || action == QLatin1StringView("close_tab")
        || action == QLatin1StringView("close_tab:this")
        || action == QLatin1StringView("close_window");
}

} // namespace

GhosttyApplicationKeybindings::GhosttyApplicationKeybindings(
    const LaunchOptions &options,
    bool enableGlobalShortcutsPortal,
    QObject *parent)
    : QObject(parent)
{
    if (QCoreApplication::instance() != nullptr) {
        QCoreApplication::instance()->installEventFilter(this);
    }
    if (enableGlobalShortcutsPortal) {
        portal_ = std::make_unique<GhosttyGlobalShortcutPortal>();
        connect(portal_.get(),
                &GhosttyGlobalShortcutPortal::shortcutActivated,
                this,
                [this](const QString &action) {
                    dispatchBroadActions({action});
                });
        connect(portal_.get(),
                &GhosttyGlobalShortcutPortal::warningOccurred,
                this,
                [](const QString &message) {
                    qWarning().noquote()
                        << "Ghostty global shortcut registration:" << message;
                });
    }
    applyLaunchOptions(options);
}

GhosttyApplicationKeybindings::~GhosttyApplicationKeybindings()
{
    if (QCoreApplication::instance() != nullptr) {
        QCoreApplication::instance()->removeEventFilter(this);
    }
    // Close the portal before registered QML workspaces are destroyed.
    portal_.reset();
}

void GhosttyApplicationKeybindings::registerWorkspace(
    TerminalWorkspace *workspace)
{
    if (workspace == nullptr) return;
    if (std::ranges::any_of(
            std::as_const(workspaces_), [workspace](const auto &candidate) {
                return candidate == workspace;
            })) {
        return;
    }

    workspaces_.append(workspace);
    connect(workspace, &TerminalWorkspace::broadActionsRequested,
            this, &GhosttyApplicationKeybindings::dispatchBroadActions);
    connect(workspace, &QObject::destroyed, this, [this] {
        workspaces_.removeIf([](const auto &candidate) {
            return candidate.isNull();
        });
    });
}

void GhosttyApplicationKeybindings::applyLaunchOptions(
    const LaunchOptions &options)
{
    GhosttyKeybindSet candidate;
    if (options.keybindingsConfigured) {
        if (!options.keybindings.isEmpty()) {
            (void) candidate.load(options.keybindings);
        } else {
            (void) candidate.load(options.keybindConfig);
        }
    }
    rootBindings_ = std::move(candidate);

    if (portal_ != nullptr) {
        if (options.keybindingsConfigured && options.keybindings.isEmpty()) {
            portal_->setKeybindConfig(options.keybindConfig);
        } else {
            portal_->clear();
        }
    }
}

QVector<QPointer<TerminalWorkspace>>
GhosttyApplicationKeybindings::workspaceSnapshot() const
{
    QVector<QPointer<TerminalWorkspace>> result;
    result.reserve(workspaces_.size());
    for (const QPointer<TerminalWorkspace> &workspace : workspaces_) {
        if (workspace != nullptr) result.append(workspace);
    }
    return result;
}

TerminalWorkspace *GhosttyApplicationKeybindings::activeWorkspace() const
{
    QWindow *focusWindow = QGuiApplication::focusWindow();
    for (const QPointer<TerminalWorkspace> &workspace : workspaces_) {
        if (workspace != nullptr && workspace->window() == focusWindow) {
            return workspace;
        }
    }
    for (const QPointer<TerminalWorkspace> &workspace : workspaces_) {
        if (workspace != nullptr) return workspace;
    }
    return nullptr;
}

bool GhosttyApplicationKeybindings::executeApplicationActions(
    const QStringList &actions)
{
    TerminalWorkspace *workspace = activeWorkspace();
    bool performed = false;
    for (const QString &action : actions) {
        if (workspace != nullptr) {
            performed = workspace->executeApplicationConfiguredAction(action)
                || performed;
        }
    }
    return performed;
}

void GhosttyApplicationKeybindings::dispatchBroadActions(
    const QStringList &actions)
{
    // Ghostty dispatches chains action-major: each surface receives action N
    // before any surface receives action N+1. App actions run only once.
    for (const QString &action : actions) {
        if (GhosttyActionCatalog::scope(action)
            == GhosttyActionScope::Application) {
            if (TerminalWorkspace *workspace = activeWorkspace();
                workspace != nullptr) {
                (void) workspace->executeApplicationConfiguredAction(action);
            }
            continue;
        }

        const QVector<QPointer<TerminalWorkspace>> workspaces =
            workspaceSnapshot();

        // In the current frontend every surface belongs to a tab/window. A
        // broad close of surfaces, tabs, or windows therefore converges on the
        // same confirmed workspace shutdown, without overwriting its single
        // pending-close dialog state during fanout.
        if (closesEverySurface(action)) {
            for (const QPointer<TerminalWorkspace> &workspace : workspaces) {
                if (workspace != nullptr) workspace->requestQuit();
            }
            continue;
        }

        for (const QPointer<TerminalWorkspace> &workspace : workspaces) {
            if (workspace != nullptr) {
                (void) workspace->executeSurfaceActionOnAllPanes(action);
            }
        }
    }
}

bool GhosttyApplicationKeybindings::eventFilter(QObject *watched,
                                                 QEvent *event)
{
    if (event == nullptr) {
        return QObject::eventFilter(watched, event);
    }
    if (event->type() == QEvent::KeyRelease) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (consumedKeys_.remove(keyEventIdentity(keyEvent))) {
            return true;
        }
        return QObject::eventFilter(watched, event);
    }
    if (event->type() != QEvent::KeyPress) {
        return QObject::eventFilter(watched, event);
    }
    auto *keyEvent = static_cast<QKeyEvent *>(event);
    const auto match = rootBindings_.match(GhosttyKeybindEvent{
        .qtKey = keyEvent->key(),
        .modifiers = keyEvent->modifiers(),
        .text = keyEvent->text(),
        .nativeScanCode = keyEvent->nativeScanCode(),
        .unshiftedCodepoint = unshiftedCodepoint(keyEvent->key()),
    });
    if (!match.has_value()) {
        return QObject::eventFilter(watched, event);
    }

    if (match->global) {
        dispatchBroadActions(match->actions);
        if (!containsIgnore(match->actions)) {
            consumedKeys_.insert(keyEventIdentity(keyEvent));
        }
        return true;
    }

    const bool applicationOnly = std::ranges::all_of(
        match->actions,
        [](const QString &action) {
            return GhosttyActionCatalog::scope(action)
                == GhosttyActionScope::Application;
        });
    if (!applicationOnly) {
        return QObject::eventFilter(watched, event);
    }

    // App.keyEvent consumes a root app binding before surface/table lookup,
    // independently of its unconsumed/performable flags.
    (void) executeApplicationActions(match->actions);
    if (!containsIgnore(match->actions)) {
        consumedKeys_.insert(keyEventIdentity(keyEvent));
    }
    return true;
}
