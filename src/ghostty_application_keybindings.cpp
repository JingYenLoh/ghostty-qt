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
    const quint64 logical =
        static_cast<quint64>(static_cast<quint32>(event->key()));
    return physical != 0 ? (quint64{1} << 63U) | physical : logical;
}

quint64 keyEventIdentity(const KeyEventSnapshot &event)
{
    const quint64 physical = static_cast<quint64>(event.nativeScanCode);
    const quint64 logical =
        static_cast<quint64>(static_cast<quint32>(event.key));
    return physical != 0 ? (quint64{1} << 63U) | physical : logical;
}

bool actionRequiresTerminalBarrier(const GhosttyConfiguredAction &action)
{
    const auto *paneAction = std::get_if<GhosttyPaneAction>(&action);
    return paneAction != nullptr
        && (std::holds_alternative<GhosttyPaneActions::CopyToClipboard>(
                *paneAction)
            || std::holds_alternative<GhosttyPaneActions::SelectAll>(
                *paneAction)
            || std::holds_alternative<GhosttyPaneActions::AdjustSelection>(
                *paneAction)
            || std::holds_alternative<GhosttyPaneActions::ScrollToSelection>(
                *paneAction)
            || std::holds_alternative<GhosttyPaneActions::SearchSelection>(
                *paneAction)
            || std::holds_alternative<TerminalWriteFileAction>(*paneAction));
}

bool isPerWindowToggle(const GhosttyConfiguredAction &action)
{
    const auto *workspaceAction = std::get_if<WorkspaceActionRequest>(&action);
    return workspaceAction != nullptr
        && (workspaceAction->action == WorkspaceAction::ToggleFullscreen
            || workspaceAction->action == WorkspaceAction::ToggleMaximize
            || workspaceAction->action
                == WorkspaceAction::ToggleWindowDecorations);
}

} // namespace

struct GhosttyApplicationKeybindings::BroadExecution {
    struct Target {
        TerminalWorkspace *workspaceIdentity = nullptr;
        QPointer<TerminalWorkspace> workspace;
        PaneId paneId;
        QPointer<TerminalPane> pane;
        bool resolved = false;
        TerminalActionExecutionResult result;
        QMetaObject::Connection paneDestruction;
        QMetaObject::Connection workspaceDestruction;
    };

    quint64 generation = 0;
    GhosttyCompiledActionChain actions;
    qsizetype entryIndex = 0;
    bool barrierInitialized = false;
    bool startingTargets = false;
    bool continuing = false;
    bool continuationMustDefer = false;
    bool continuationQueued = false;
    qsizetype unresolvedTargets = 0;
    qsizetype publishIndex = 0;
    QVector<Target> targets;
};

GhosttyApplicationKeybindings::GhosttyApplicationKeybindings(
    const LaunchOptions &options, bool enableGlobalShortcutsPortal,
    QObject *parent)
    : QObject(parent)
{
    if (QCoreApplication::instance() != nullptr) {
        QCoreApplication::instance()->installEventFilter(this);
    }
    if (enableGlobalShortcutsPortal) {
        portal_ = std::make_unique<GhosttyGlobalShortcutPortal>();
        connect(portal_.get(), &GhosttyGlobalShortcutPortal::shortcutActivated,
                this, [this](const QString &action) {
                    dispatchBroadActions({action});
                });
        connect(portal_.get(), &GhosttyGlobalShortcutPortal::warningOccurred,
                this, [](const QString &message) {
                    qWarning().noquote()
                        << "Ghostty global shortcut registration:" << message;
                });
    }
    (void)applyLaunchOptions(options);
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
    if (configurationUpdateDepth_ == 0) {
        resumeReadyBroadExecution();
        drainDeferredInputs();
    }
}

void GhosttyApplicationKeybindings::dispatchOrDeferBroadActions(
    GhosttyCompiledActionChain actions)
{
    if (configurationUpdateDepth_ != 0 || drainingDeferredInputs_
        || broadExecution_ != nullptr) {
        deferredInputs_.emplace_back(std::move(actions));
        return;
    }
    dispatchCompiledBroadActions(actions);
}

void GhosttyApplicationKeybindings::drainDeferredInputs()
{
    if (configurationUpdateDepth_ != 0 || keyEventDispatchDepth_ != 0
        || broadExecution_ != nullptr || drainingDeferredInputs_) {
        return;
    }

    const QPointer<GhosttyApplicationKeybindings> guard(this);
    drainingDeferredInputs_ = true;
    while (guard != nullptr && guard->configurationUpdateDepth_ == 0
           && guard->keyEventDispatchDepth_ == 0
           && guard->broadExecution_ == nullptr
           && !guard->deferredInputs_.empty()) {
        DeferredInput deferred = std::move(guard->deferredInputs_.front());
        guard->deferredInputs_.pop_front();
        if (auto *key = std::get_if<DeferredKeyEvent>(&deferred)) {
            if (key->target == nullptr) {
                if (!key->event.pressed) {
                    guard->consumedKeys_.remove(keyEventIdentity(key->event));
                }
                continue;
            }
            QKeyEvent replay = key->event.replay();
            guard->replayingDeferredKeyEvent_ = &replay;
            QCoreApplication::sendEvent(key->target, &replay);
            if (guard != nullptr) {
                guard->replayingDeferredKeyEvent_ = nullptr;
            }
        } else if (auto *inputMethod =
                       std::get_if<DeferredInputMethodEvent>(&deferred)) {
            if (inputMethod->target == nullptr) continue;
            QInputMethodEvent replay(inputMethod->preedit,
                                     inputMethod->attributes);
            replay.setCommitString(inputMethod->commit,
                                   inputMethod->replacementStart,
                                   inputMethod->replacementLength);
            guard->replayingDeferredInputMethodEvent_ = &replay;
            QCoreApplication::sendEvent(inputMethod->target, &replay);
            if (guard != nullptr) {
                guard->replayingDeferredInputMethodEvent_ = nullptr;
            }
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
    if (workspaces_.contains(workspace)) return;

    workspaces_.insert(workspace);
    connect(workspace, &TerminalWorkspace::broadActionsRequested, this,
            [this](const GhosttyCompiledActionChain &actions) {
                dispatchOrDeferBroadActions(actions);
            });
    connect(workspace, &TerminalWorkspace::paneCommitted, this,
            [this, workspace](PaneId paneId, TerminalPane *pane) {
                appendSurface(workspace, paneId, pane);
            });
    connect(workspace, &TerminalWorkspace::paneRemoved, this,
            [this, workspace](PaneId paneId, TerminalPane *) {
                removeSurface(workspace, paneId);
            });
    connect(workspace, &QObject::destroyed, this, [this, workspace] {
        removeWorkspaceSurfaces(workspace);
        workspaces_.remove(workspace);
    });

    for (const TerminalWorkspace::BroadPaneTarget &target :
         workspace->broadPaneSnapshot()) {
        appendSurface(workspace, target.paneId, target.pane);
    }
}

void GhosttyApplicationKeybindings::appendSurface(TerminalWorkspace *workspace,
                                                  PaneId paneId,
                                                  TerminalPane *pane)
{
    if (workspace == nullptr || !paneId.isValid() || pane == nullptr
        || std::ranges::any_of(
            std::as_const(surfaces_),
            [workspace, paneId, pane](const SurfaceTarget &target) {
                return target.workspaceIdentity == workspace
                    && target.paneId == paneId && target.pane == pane;
            })) {
        return;
    }

    surfaces_.append({
        .workspaceIdentity = workspace,
        .workspace = workspace,
        .paneId = paneId,
        .pane = pane,
    });
    connect(pane, &QObject::destroyed, this,
            [this, workspace, paneId] { removeSurface(workspace, paneId); });
}

void GhosttyApplicationKeybindings::removeSurface(TerminalWorkspace *workspace,
                                                  PaneId paneId)
{
    for (qsizetype index = 0; index < surfaces_.size(); ++index) {
        const SurfaceTarget &target = surfaces_[index];
        if (target.workspaceIdentity == workspace && target.paneId == paneId) {
            swapRemoveSurface(index);
            return;
        }
    }
}

void GhosttyApplicationKeybindings::removeWorkspaceSurfaces(
    TerminalWorkspace *workspace)
{
    for (qsizetype index = surfaces_.size(); index-- > 0;) {
        if (surfaces_[index].workspaceIdentity != workspace) continue;
        swapRemoveSurface(index);
    }
}

void GhosttyApplicationKeybindings::swapRemoveSurface(qsizetype index)
{
    Q_ASSERT(index >= 0 && index < surfaces_.size());
    if (index != surfaces_.size() - 1) {
        surfaces_[index] = std::move(surfaces_.back());
    }
    surfaces_.removeLast();
}

QVector<GhosttyApplicationKeybindings::SurfaceTarget>
GhosttyApplicationKeybindings::surfaceSnapshot() const
{
    QVector<SurfaceTarget> result;
    result.reserve(surfaces_.size());
    for (const SurfaceTarget &target : surfaces_) {
        if (target.workspace != nullptr && target.pane != nullptr) {
            result.append(target);
        }
    }
    return result;
}

bool GhosttyApplicationKeybindings::surfaceTargetIsLive(
    const SurfaceTarget &target) const
{
    return target.workspace != nullptr && target.pane != nullptr
        && target.workspace->broadPaneTargetIsLive({
            .paneId = target.paneId,
            .pane = target.pane,
        });
}

GhosttyKeybindProgram
GhosttyApplicationKeybindings::applyLaunchOptions(const LaunchOptions &options)
{
    beginConfigurationUpdate();
    const QPointer<GhosttyApplicationKeybindings> guard(this);
    const auto updateGuard = qScopeGuard([guard] {
        if (guard != nullptr) guard->endConfigurationUpdate();
    });

    GhosttyKeybindProgram program =
        GhosttyKeybindProgram::compile(options.keybindSource).program;
    // Both values become visible inside one input-deferral transaction. A new
    // keybinding generation also resets any traversal/table state held by the
    // root matcher.
    (void)rootState_.replaceProgram(program);
    modifierRemaps_.replaceMappings(options.modifierRemaps);

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
    if (broadExecution_ != nullptr) {
        deferredInputs_.emplace_back(actions);
        return;
    }

    do {
        ++nextBroadExecutionGeneration_;
    } while (nextBroadExecutionGeneration_ == 0);
    broadExecution_ = std::make_shared<BroadExecution>(BroadExecution{
        .generation = nextBroadExecutionGeneration_,
        .actions = actions,
        .entryIndex = 0,
        .barrierInitialized = false,
        .startingTargets = false,
        .continuing = false,
        .continuationMustDefer = false,
        .continuationQueued = false,
        .unresolvedTargets = 0,
        .publishIndex = 0,
        .targets = {},
    });
    continueBroadExecution();
}

void GhosttyApplicationKeybindings::continueBroadExecution()
{
    const std::shared_ptr<BroadExecution> execution = broadExecution_;
    if (execution == nullptr || execution->continuing
        || configurationUpdateDepth_ != 0) {
        return;
    }

    const quint64 generation = execution->generation;
    const QPointer<GhosttyApplicationKeybindings> guard(this);
    execution->continuing = true;
    const auto continuingGuard = qScopeGuard([guard, execution] {
        if (guard != nullptr && guard->broadExecution_ == execution) {
            execution->continuing = false;
        }
    });
    const auto stillCurrent = [guard, execution, generation] {
        return guard != nullptr && guard->broadExecution_ == execution
            && execution->generation == generation;
    };

    // Ghostty dispatches chains action-major: each surface receives action N
    // before any surface receives action N+1. Worker-owned actions add a
    // per-entry barrier: prepare on every target concurrently, publish their
    // GUI effects in snapshot order, and only then advance the chain.
    while (execution->entryIndex < execution->actions.entries.size()) {
        const GhosttyCompiledAction &entry =
            execution->actions.entries.at(execution->entryIndex);
        if (entry.scope == GhosttyActionScope::Application) {
            const auto *application = entry.getIf<ApplicationAction>();
            if (application != nullptr) {
                Q_EMIT applicationActionRequested(*application);
                if (!stillCurrent()) return;
            }
            ++execution->entryIndex;
            continue;
        }

        if (!entry.action.has_value()) {
            ++execution->entryIndex;
            continue;
        }

        // In the current frontend every surface belongs to a tab/window. A
        // broad close of surfaces, tabs, or windows therefore converges on the
        // same confirmed workspace shutdown, without overwriting its single
        // pending-close dialog state during fanout.
        if (GhosttyActionCatalog::shouldCoalesceBroadClose(*entry.action)) {
            QSet<TerminalWorkspace *> visited;
            for (const SurfaceTarget &target : surfaceSnapshot()) {
                if (!surfaceTargetIsLive(target)
                    || visited.contains(target.workspaceIdentity)) {
                    continue;
                }
                visited.insert(target.workspaceIdentity);
                target.workspace->requestWindowClose();
                if (!stillCurrent()) return;
            }
            ++execution->entryIndex;
            continue;
        }

        if (!actionRequiresTerminalBarrier(*entry.action)) {
            const QVector<SurfaceTarget> surfaces = surfaceSnapshot();
            if (isPerWindowToggle(*entry.action)) {
                QSet<TerminalWorkspace *> visited;
                for (const SurfaceTarget &target : surfaces) {
                    if (!surfaceTargetIsLive(target)
                        || visited.contains(target.workspaceIdentity)) {
                        continue;
                    }
                    visited.insert(target.workspaceIdentity);
                    (void)target.workspace->executeSurfaceActionOnAllPanes(
                        *entry.action);
                    if (!stillCurrent()) return;
                }
            } else {
                for (const SurfaceTarget &target : surfaces) {
                    if (!surfaceTargetIsLive(target)) continue;
                    (void)target.workspace->executeBroadSurfaceAction(
                        {
                            .paneId = target.paneId,
                            .pane = target.pane,
                        },
                        *entry.action);
                    if (!stillCurrent()) return;
                }
            }
            ++execution->entryIndex;
            continue;
        }

        if (!execution->barrierInitialized) {
            execution->barrierInitialized = true;
            execution->startingTargets = true;
            execution->continuationMustDefer = false;
            execution->continuationQueued = false;
            execution->publishIndex = 0;
            execution->targets.clear();

            // A fresh snapshot for each action matches Ghostty's action-major
            // fanout: topology changes from the previous entry affect the
            // target set of the next entry, never the entry already running.
            for (const SurfaceTarget &target : surfaceSnapshot()) {
                execution->targets.append({
                    .workspaceIdentity = target.workspaceIdentity,
                    .workspace = target.workspace,
                    .paneId = target.paneId,
                    .pane = target.pane,
                    .resolved = false,
                    .result = {},
                    .paneDestruction = {},
                    .workspaceDestruction = {},
                });
            }

            execution->unresolvedTargets = execution->targets.size();
            for (qsizetype index = 0; index < execution->targets.size();
                 ++index) {
                BroadExecution::Target &target = execution->targets[index];
                if (target.workspace == nullptr || target.pane == nullptr) {
                    resolveBroadTarget(generation, index,
                                       TerminalActionExecutionResult{}, true);
                    if (!stillCurrent()) return;
                    continue;
                }

                target.paneDestruction =
                    connect(target.pane, &QObject::destroyed, this,
                            [this, generation, index] {
                                resolveBroadTarget(
                                    generation, index,
                                    TerminalActionExecutionResult{}, true);
                            });
                target.workspaceDestruction =
                    connect(target.workspace, &QObject::destroyed, this,
                            [this, generation, index] {
                                resolveBroadTarget(
                                    generation, index,
                                    TerminalActionExecutionResult{}, true);
                            });

                const QPointer<GhosttyApplicationKeybindings> completionGuard(
                    this);
                TerminalActionExecutionStart start =
                    target.pane->startConfiguredAction(
                        *entry.action,
                        [completionGuard, generation,
                         index](TerminalActionExecutionResult result) {
                            if (completionGuard != nullptr) {
                                completionGuard->resolveBroadTarget(
                                    generation, index, std::move(result));
                            }
                        });
                if (!stillCurrent()) return;
                if (!start.pending) {
                    resolveBroadTarget(generation, index,
                                       std::move(start.result));
                    if (!stillCurrent()) return;
                }
            }
            execution->startingTargets = false;

            if (execution->unresolvedTargets != 0) {
                return;
            }
            execution->continuationMustDefer = execution->continuationMustDefer
                || broadExecutionHasLostTarget(*execution);
            if (execution->continuationMustDefer) {
                deferBroadExecutionContinuation(generation);
                return;
            }
        }

        // All worker results are now known. Publish side effects only for
        // panes which still occupy their snapshotted identity, and do so in
        // the deterministic append-on-create/swap-remove order of the
        // process surface snapshot.
        // Publication is resumable because a GUI effect can synchronously
        // destroy a target. Advancing the cursor before deferring prevents
        // both duplicate effects and the next chain entry from running
        // inside QObject teardown.
        while (execution->publishIndex < execution->targets.size()) {
            BroadExecution::Target &target =
                execution->targets[execution->publishIndex];
            if (surfaceTargetIsLive({
                    .workspaceIdentity = target.workspaceIdentity,
                    .workspace = target.workspace,
                    .paneId = target.paneId,
                    .pane = target.pane,
                })) {
                (void)target.pane->commitConfiguredActionResult(target.result);
                if (!stillCurrent()) return;
            }
            QObject::disconnect(target.paneDestruction);
            QObject::disconnect(target.workspaceDestruction);
            ++execution->publishIndex;
            if (configurationUpdateDepth_ != 0) {
                return;
            }
            if (execution->continuationMustDefer) {
                deferBroadExecutionContinuation(generation);
                return;
            }
        }
        execution->targets.clear();
        execution->barrierInitialized = false;
        execution->continuationMustDefer = false;
        execution->continuationQueued = false;
        execution->publishIndex = 0;
        ++execution->entryIndex;
    }

    if (!stillCurrent()) return;
    broadExecution_.reset();
    drainDeferredInputs();
}

void GhosttyApplicationKeybindings::resolveBroadTarget(
    quint64 generation, qsizetype targetIndex,
    TerminalActionExecutionResult result, bool deferContinuation)
{
    const std::shared_ptr<BroadExecution> execution = broadExecution_;
    if (execution == nullptr || execution->generation != generation
        || targetIndex < 0 || targetIndex >= execution->targets.size()) {
        return;
    }

    BroadExecution::Target &target = execution->targets[targetIndex];
    execution->continuationMustDefer =
        execution->continuationMustDefer || deferContinuation;
    if (deferContinuation) {
        QObject::disconnect(target.paneDestruction);
        QObject::disconnect(target.workspaceDestruction);
    }
    if (target.resolved) return;

    target.resolved = true;
    target.result = std::move(result);
    Q_ASSERT(execution->unresolvedTargets > 0);
    --execution->unresolvedTargets;
    if (execution->unresolvedTargets == 0 && !execution->startingTargets) {
        execution->continuationMustDefer = execution->continuationMustDefer
            || broadExecutionHasLostTarget(*execution);
        if (configurationUpdateDepth_ != 0 || execution->continuing) {
            return;
        }
        if (!execution->continuationMustDefer) {
            continueBroadExecution();
            return;
        }
        deferBroadExecutionContinuation(generation);
    }
}

void GhosttyApplicationKeybindings::deferBroadExecutionContinuation(
    quint64 generation)
{
    const std::shared_ptr<BroadExecution> execution = broadExecution_;
    if (execution == nullptr || execution->generation != generation
        || execution->continuationQueued) {
        return;
    }

    execution->continuationQueued = true;
    const QPointer<GhosttyApplicationKeybindings> guard(this);
    QMetaObject::invokeMethod(
        this,
        [guard, generation] {
            if (guard == nullptr || guard->broadExecution_ == nullptr
                || guard->broadExecution_->generation != generation) {
                return;
            }
            const std::shared_ptr<BroadExecution> currentExecution =
                guard->broadExecution_;
            currentExecution->continuationQueued = false;
            if (currentExecution->unresolvedTargets != 0
                || currentExecution->startingTargets
                || currentExecution->continuing
                || guard->configurationUpdateDepth_ != 0) {
                return;
            }
            // The queued turn satisfies the teardown deferral. Any new
            // target loss during resumed publication sets this again.
            currentExecution->continuationMustDefer = false;
            guard->continueBroadExecution();
        },
        Qt::QueuedConnection);
}

void GhosttyApplicationKeybindings::resumeReadyBroadExecution()
{
    const std::shared_ptr<BroadExecution> execution = broadExecution_;
    if (execution == nullptr || configurationUpdateDepth_ != 0
        || execution->continuing || execution->startingTargets
        || execution->unresolvedTargets != 0) {
        return;
    }
    if (execution->continuationMustDefer) {
        deferBroadExecutionContinuation(execution->generation);
    } else {
        continueBroadExecution();
    }
}

bool GhosttyApplicationKeybindings::broadExecutionHasLostTarget(
    const BroadExecution &execution)
{
    return std::ranges::any_of(
        execution.targets, [](const BroadExecution::Target &target) {
            return target.workspace == nullptr || target.pane == nullptr;
        });
}

bool GhosttyApplicationKeybindings::eventFilter(QObject *watched, QEvent *event)
{
    if (event == nullptr) {
        return QObject::eventFilter(watched, event);
    }
    const bool keyPress = event->type() == QEvent::KeyPress;
    const bool keyRelease = event->type() == QEvent::KeyRelease;
    auto *pane = qobject_cast<TerminalPane *>(watched);
    const bool inputMethod =
        event->type() == QEvent::InputMethod && pane != nullptr;
    const bool currentReplay = (keyPress || keyRelease)
        && replayingDeferredKeyEvent_ == static_cast<QKeyEvent *>(event);
    const bool currentInputMethodReplay = inputMethod
        && replayingDeferredInputMethodEvent_
            == static_cast<QInputMethodEvent *>(event);
    if ((keyPress || keyRelease || inputMethod)
        && (configurationUpdateDepth_ > 0 || broadExecution_ != nullptr
            || (drainingDeferredInputs_ && !currentReplay
                && !currentInputMethodReplay))) {
        if (inputMethod) {
            const auto *input = static_cast<QInputMethodEvent *>(event);
            deferredInputs_.emplace_back(DeferredInputMethodEvent{
                .target = watched,
                .preedit = input->preeditString(),
                .attributes = input->attributes(),
                .commit = input->commitString(),
                .replacementStart = input->replacementStart(),
                .replacementLength = input->replacementLength(),
            });
        } else {
            deferredInputs_.emplace_back(DeferredKeyEvent{
                .target = watched,
                .event =
                    KeyEventSnapshot::capture(*static_cast<QKeyEvent *>(event)),
            });
        }
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
    auto *keyEvent = static_cast<QKeyEvent *>(event);
    if (pane != nullptr && pane->deferKeyEventIfNeeded(*keyEvent)) {
        event->accept();
        return true;
    }
    const KeyEventSnapshot remapped =
        modifierRemaps_.remapEvent(KeyEventSnapshot::capture(*keyEvent));
    const auto publishRootTrace = [pane, &remapped](
                                      TerminalKeyboardTraceDecisionKind kind,
                                      const GhosttyKeybindMatch *match =
                                          nullptr) {
        if (pane == nullptr || !pane->inspectorKeyboardTraceCaptureActive()) {
            return true;
        }
        const QPointer<TerminalPane> paneGuard(pane);
        QKeyEvent tracedEvent = remapped.replay();
        TerminalKeyInput input =
            pane->beginInspectorKeyboardTrace(tracedEvent, remapped.pressed);
        TerminalKeyboardTraceDecision decision;
        decision.input = std::move(input);
        decision.kind = kind;
        if (match != nullptr) {
            decision.actions = match->actionChain.serializedActions();
            decision.activeTables = pane->activeKeyTables();
            decision.pendingSequence = pane->pendingKeySequence();
            // Root application and global matches are consumed before
            // surface lookup regardless of their raw unconsumed flag.
            decision.consumed = true;
            decision.performable = match->performable;
            decision.all = match->all;
            decision.global = match->global;
            decision.physical = match->physical;
        } else {
            decision.consumed = true;
        }
        pane->publishInspectorKeyboardTrace(std::move(decision));
        return paneGuard != nullptr;
    };
    if (keyRelease) {
        if (consumedKeys_.remove(keyEventIdentity(keyEvent))) {
            if (!publishRootTrace(
                    TerminalKeyboardTraceDecisionKind::RootConsumedRelease)) {
                return true;
            }
            return true;
        }
        return QObject::eventFilter(watched, event);
    }
    const auto match = rootState_.match(GhosttyKeybindEvent{
        .qtKey = remapped.key,
        .modifiers = remapped.modifiers,
        .text = remapped.text,
        .nativeScanCode = remapped.nativeScanCode,
        .unshiftedCodepoint = unshiftedCodepoint(remapped.key),
    });
    if (!match.has_value()) {
        return QObject::eventFilter(watched, event);
    }

    if (match->global) {
        if (!publishRootTrace(
                TerminalKeyboardTraceDecisionKind::RootGlobalBinding, &*match)
            || dispatchGuard == nullptr) {
            return true;
        }
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
    if (!publishRootTrace(
            TerminalKeyboardTraceDecisionKind::RootApplicationBinding, &*match)
        || dispatchGuard == nullptr) {
        return true;
    }
    const QPointer<GhosttyApplicationKeybindings> guard(this);
    const QPointer<QObject> watchedGuard(watched);
    (void)executeApplicationActions(match->actionChain);
    if (guard == nullptr || watchedGuard == nullptr) return true;
    if (match->actionChain.inputEffect != GhosttyActionInputEffect::Ignore) {
        consumedKeys_.insert(keyEventIdentity(keyEvent));
    }
    return true;
}
