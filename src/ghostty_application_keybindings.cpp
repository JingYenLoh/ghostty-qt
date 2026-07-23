#include "ghostty_application_keybindings.h"

#include "ghostty_action_catalog.h"
#include "ghostty_global_shortcut_portal.h"
#include "terminal_pane.h"
#include "terminal_workspace.h"

#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QKeyEvent>
#include <QScopeGuard>

#include <algorithm>
#include <cstdint>
#include <optional>
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
    (void) applyLaunchOptions(options);
}

GhosttyApplicationKeybindings::~GhosttyApplicationKeybindings()
{
    if (QCoreApplication::instance() != nullptr) {
        QCoreApplication::instance()->removeEventFilter(this);
    }
    // Close the portal before registered QML workspaces are destroyed.
    portal_.reset();
}

void GhosttyApplicationKeybindings::beginConfigurationUpdate() noexcept
{
    ++configurationUpdateDepth_;
}

void GhosttyApplicationKeybindings::endConfigurationUpdate()
{
    Q_ASSERT(configurationUpdateDepth_ > 0);
    --configurationUpdateDepth_;
    if (configurationUpdateDepth_ == 0) drainDeferredInputs();
}

void GhosttyApplicationKeybindings::dispatchOrDeferBroadActions(
    GhosttyCompiledActionChain actions)
{
    if (configurationUpdateDepth_ != 0) {
        deferredInputs_.emplace_back(std::move(actions));
        return;
    }
    dispatchCompiledBroadActions(actions);
}

void GhosttyApplicationKeybindings::drainDeferredInputs()
{
    if (configurationUpdateDepth_ != 0 || keyEventDispatchDepth_ != 0
        || drainingDeferredInputs_) {
        return;
    }

    const QPointer<GhosttyApplicationKeybindings> guard(this);
    drainingDeferredInputs_ = true;
    while (guard != nullptr && guard->configurationUpdateDepth_ == 0
           && guard->keyEventDispatchDepth_ == 0
           && !guard->deferredInputs_.empty()) {
        DeferredInput deferred =
            std::move(guard->deferredInputs_.front());
        guard->deferredInputs_.pop_front();
        if (auto *key = std::get_if<DeferredKeyEvent>(&deferred)) {
            if (key->target == nullptr) continue;
            QKeyEvent replay = key->event.replay();
            QCoreApplication::sendEvent(key->target, &replay);
        } else {
            guard->dispatchCompiledBroadActions(
                std::get<GhosttyCompiledActionChain>(deferred));
        }
    }
    if (guard != nullptr) guard->drainingDeferredInputs_ = false;
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
            this,
            [this](const GhosttyCompiledActionChain &actions) {
                dispatchOrDeferBroadActions(actions);
            });
    connect(workspace, &QObject::destroyed, this, [this] {
        workspaces_.removeIf([](const auto &candidate) {
            return candidate.isNull();
        });
    });
}

GhosttyKeybindProgram GhosttyApplicationKeybindings::applyLaunchOptions(
    const LaunchOptions &options)
{
    GhosttyKeybindProgram program =
        GhosttyKeybindProgram::compile(options.keybindSource).program;
    (void) rootState_.replaceProgram(program);

    if (portal_ != nullptr) {
        if (const GhosttyKeybindConfig *config =
                options.keybindSource.structured()) {
            portal_->setKeybindConfig(*config);
        } else {
            portal_->clear();
        }
    }
    return program;
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

bool GhosttyApplicationKeybindings::executeApplicationActions(
    const GhosttyCompiledActionChain &actions)
{
    const QPointer<GhosttyApplicationKeybindings> guard(this);
    bool performed = false;
    for (const GhosttyCompiledAction &entry : actions.entries) {
        const auto *application = entry.getIf<ApplicationAction>();
        if (application != nullptr) {
            Q_EMIT applicationActionRequested(*application);
            performed = true;
            if (guard == nullptr) return performed;
        }
    }
    return performed;
}

void GhosttyApplicationKeybindings::dispatchBroadActions(
    const QStringList &actions)
{
    dispatchOrDeferBroadActions(
        GhosttyActionCatalog::compileActionChain(actions));
}

void GhosttyApplicationKeybindings::dispatchCompiledBroadActions(
    const GhosttyCompiledActionChain &actions)
{
    const QPointer<GhosttyApplicationKeybindings> guard(this);
    // Ghostty dispatches chains action-major: each surface receives action N
    // before any surface receives action N+1. App actions run only once.
    for (const GhosttyCompiledAction &entry : actions.entries) {
        if (entry.scope == GhosttyActionScope::Application) {
            const auto *application = entry.getIf<ApplicationAction>();
            if (application != nullptr) {
                Q_EMIT applicationActionRequested(*application);
                if (guard == nullptr) return;
            }
            continue;
        }

        if (!entry.action.has_value()) continue;

        const QVector<QPointer<TerminalWorkspace>> workspaces =
            workspaceSnapshot();

        // In the current frontend every surface belongs to a tab/window. A
        // broad close of surfaces, tabs, or windows therefore converges on the
        // same confirmed workspace shutdown, without overwriting its single
        // pending-close dialog state during fanout.
        if (GhosttyActionCatalog::shouldCoalesceBroadClose(*entry.action)) {
            for (const QPointer<TerminalWorkspace> &workspace : workspaces) {
                if (workspace != nullptr) workspace->requestWindowClose();
                if (guard == nullptr) return;
            }
            continue;
        }

        for (const QPointer<TerminalWorkspace> &workspace : workspaces) {
            if (workspace != nullptr) {
                (void) workspace->executeSurfaceActionOnAllPanes(
                    *entry.action);
                if (guard == nullptr) return;
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
    const bool keyPress = event->type() == QEvent::KeyPress;
    const bool keyRelease = event->type() == QEvent::KeyRelease;
    if ((keyPress || keyRelease) && configurationUpdateDepth_ > 0) {
        deferredInputs_.emplace_back(DeferredKeyEvent{
            .target = watched,
            .event = KeyEventSnapshot::capture(
                *static_cast<QKeyEvent *>(event)),
        });
        event->accept();
        return true;
    }
    if (!keyPress && !keyRelease) {
        return QObject::eventFilter(watched, event);
    }

    const QPointer<GhosttyApplicationKeybindings> dispatchGuard(this);
    ++keyEventDispatchDepth_;
    const auto finishDispatch = qScopeGuard([dispatchGuard] {
        if (dispatchGuard == nullptr) return;
        Q_ASSERT(dispatchGuard->keyEventDispatchDepth_ > 0);
        --dispatchGuard->keyEventDispatchDepth_;
        dispatchGuard->drainDeferredInputs();
    });
    auto *pane = qobject_cast<TerminalPane *>(watched);
    auto *keyEvent = static_cast<QKeyEvent *>(event);
    if (pane != nullptr && pane->deferKeyEventIfNeeded(*keyEvent)) {
        event->accept();
        return true;
    }
    if (keyRelease) {
        if (consumedKeys_.remove(keyEventIdentity(keyEvent))) {
            return true;
        }
        return QObject::eventFilter(watched, event);
    }
    const auto match = rootState_.match(GhosttyKeybindEvent{
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
        const QPointer<GhosttyApplicationKeybindings> guard(this);
        const QPointer<QObject> watchedGuard(watched);
        dispatchCompiledBroadActions(match->actionChain);
        if (guard == nullptr || watchedGuard == nullptr) return true;
        if (match->actionChain.inputEffect
            != GhosttyActionInputEffect::Ignore) {
            consumedKeys_.insert(keyEventIdentity(keyEvent));
        }
        return true;
    }

    if (!match->actionChain.applicationOnly) {
        return QObject::eventFilter(watched, event);
    }

    // App.keyEvent consumes a root app binding before surface/table lookup,
    // independently of its unconsumed/performable flags.
    const QPointer<GhosttyApplicationKeybindings> guard(this);
    const QPointer<QObject> watchedGuard(watched);
    (void) executeApplicationActions(match->actionChain);
    if (guard == nullptr || watchedGuard == nullptr) return true;
    if (match->actionChain.inputEffect
        != GhosttyActionInputEffect::Ignore) {
        consumedKeys_.insert(keyEventIdentity(keyEvent));
    }
    return true;
}
