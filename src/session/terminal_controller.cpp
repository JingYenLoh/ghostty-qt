#include "session/terminal_controller.h"

#include "input/zig_string_escape.h"
#include "session/session_worker.h"
#include "session/terminal_clipboard_bridge.h"
#include "support/intentional_crash.h"

#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <algorithm>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace {

bool keyMayStartProcess(const TerminalKeyInput &input)
{
    return !input.composing && input.pressed
        && (input.key == Qt::Key_Return || input.key == Qt::Key_Enter
            || input.text.contains(u'\n') || input.text.contains(u'\r'));
}

bool hasExplicitCommand(const TerminalSessionLaunchOptions &options)
{
    return !options.program.isEmpty()
        || (options.command.has_value() && !options.command->defaultShell);
}

TerminalActionResult failedTerminalActionResult(quint64 requestId)
{
    return {
        .requestId = requestId,
        .outcome = TerminalActionOutcome::Failed,
        .effect = TerminalActionEffect::None,
        .performed = false,
        .payload = {},
        .clipboardDestination = TerminalClipboardDestination::Standard,
    };
}

template <typename... Types> void registerMetaTypesOnce()
{
    // Qt's registry is process-wide, so repeating this work for every pane is
    // unnecessary. One specialization owns one thread-safe initialization.
    [[maybe_unused]] static const bool registered = [] {
        (qRegisterMetaType<Types>(), ...);
        return true;
    }();
}

} // namespace

template <typename... SignalArgs, typename... WorkerArgs>
void TerminalController::relayWorkerRequest(
    void (TerminalController::*signal)(SignalArgs...),
    void (SessionWorker::*slot)(WorkerArgs...))
{
    connect(this, signal, this, [this, slot](SignalArgs... args) {
        auto values = std::make_tuple(std::decay_t<SignalArgs>(args)...);
        enqueueWorkerRequest(
            [slot, values = std::move(values)](SessionWorker &worker) mutable {
                std::apply(
                    [&worker, slot](auto &...unpacked) {
                        std::invoke(slot, worker, unpacked...);
                    },
                    values);
            });
    });
}

void TerminalController::connectWorkerRequestRelays()
{
    relayWorkerRequest(
        &TerminalController::resizeRequested,
        static_cast<void (SessionWorker::*)(const TerminalSessionGeometry &)>(
            &SessionWorker::resizeTerminal));
    relayWorkerRequest(&TerminalController::keyRequested,
                       &SessionWorker::sendKey);
    relayWorkerRequest(&TerminalController::sequenceKeyStagingRequested,
                       &SessionWorker::stageSequenceKey);
    relayWorkerRequest(&TerminalController::sequenceResolutionRequested,
                       &SessionWorker::resolveSequence);
    relayWorkerRequest(&TerminalController::inputMethodRequested,
                       &SessionWorker::sendInputMethod);
    relayWorkerRequest(&TerminalController::csiRequested,
                       &SessionWorker::sendCsi);
    relayWorkerRequest(&TerminalController::escapeRequested,
                       &SessionWorker::sendEscape);
    relayWorkerRequest(&TerminalController::rawTextRequested,
                       &SessionWorker::sendRawText);
    relayWorkerRequest(&TerminalController::resetTerminalRequested,
                       &SessionWorker::resetTerminal);
    relayWorkerRequest(&TerminalController::mouseRequested,
                       &SessionWorker::sendMouse);
    relayWorkerRequest(&TerminalController::wheelRequested,
                       &SessionWorker::sendWheel);
    relayWorkerRequest(&TerminalController::rightClickRequested,
                       &SessionWorker::resolveRightClick);
    relayWorkerRequest(&TerminalController::focusRequested,
                       &SessionWorker::setFocused);
    relayWorkerRequest(&TerminalController::pasteWithMetadataRequested,
                       &SessionWorker::paste);
    relayWorkerRequest(&TerminalController::confirmPasteRequested,
                       &SessionWorker::confirmPaste);
    relayWorkerRequest(&TerminalController::cancelPasteRequested,
                       &SessionWorker::cancelPaste);
    relayWorkerRequest(&TerminalController::copyRequested,
                       &SessionWorker::copySelection);
    relayWorkerRequest(&TerminalController::copyActionRequested,
                       &SessionWorker::copySelectionAction);
    relayWorkerRequest(&TerminalController::writeTerminalFileRequested,
                       &SessionWorker::writeTerminalFile);
    relayWorkerRequest(&TerminalController::clearSelectionRequested,
                       &SessionWorker::clearSelection);
    relayWorkerRequest(
        &TerminalController::clearSelectionIfMouseTrackingRequested,
        &SessionWorker::clearSelectionIfMouseTracking);
    relayWorkerRequest(&TerminalController::beginSelectionRequested,
                       &SessionWorker::beginSelection);
    relayWorkerRequest(&TerminalController::updateSelectionRequested,
                       &SessionWorker::updateSelection);
    relayWorkerRequest(&TerminalController::endSelectionRequested,
                       &SessionWorker::endSelection);
    relayWorkerRequest(&TerminalController::cancelSelectionGestureRequested,
                       &SessionWorker::cancelSelectionGesture);
    relayWorkerRequest(&TerminalController::selectAllRequested,
                       &SessionWorker::selectAll);
    relayWorkerRequest(&TerminalController::selectAllActionRequested,
                       &SessionWorker::selectAllAction);
    relayWorkerRequest(&TerminalController::selectionAdjustmentRequested,
                       &SessionWorker::adjustSelection);
    relayWorkerRequest(&TerminalController::selectionAdjustmentActionRequested,
                       &SessionWorker::adjustSelectionAction);
    relayWorkerRequest(&TerminalController::scrollRequested,
                       &SessionWorker::scrollViewport);
    relayWorkerRequest(&TerminalController::scrollToSelectionActionRequested,
                       &SessionWorker::scrollToSelectionAction);
    relayWorkerRequest(&TerminalController::searchRequested,
                       &SessionWorker::search);
    relayWorkerRequest(&TerminalController::serializedSearchRequested,
                       &SessionWorker::searchSerialized);
    relayWorkerRequest(&TerminalController::searchCancellationRequested,
                       &SessionWorker::cancelSearch);
    relayWorkerRequest(&TerminalController::searchNavigationRequested,
                       &SessionWorker::navigateSearch);
    relayWorkerRequest(&TerminalController::searchSelectionActionRequested,
                       &SessionWorker::searchSelectionAction);
    relayWorkerRequest(&TerminalController::hyperlinkQueryRequested,
                       &SessionWorker::queryHyperlink);
    relayWorkerRequest(&TerminalController::hyperlinkQueryCancellationRequested,
                       &SessionWorker::cancelHyperlinkQuery);
    relayWorkerRequest(
        &TerminalController::hyperlinkActivationPreparationRequested,
        &SessionWorker::prepareHyperlinkActivation);
    relayWorkerRequest(&TerminalController::hyperlinkActivationCommitRequested,
                       &SessionWorker::commitHyperlinkActivation);
    relayWorkerRequest(
        &TerminalController::hyperlinkActivationCancellationRequested,
        &SessionWorker::cancelHyperlinkActivation);
    relayWorkerRequest(&TerminalController::terminalInspectorSnapshotRequested,
                       &SessionWorker::inspectTerminal);
    relayWorkerRequest(&TerminalController::terminalInspectorCellRequested,
                       &SessionWorker::inspectTerminalCell);
    relayWorkerRequest(&TerminalController::keyboardTraceGenerationRequested,
                       &SessionWorker::setKeyboardTraceGeneration);
    relayWorkerRequest(&TerminalController::runtimeOptionsRequested,
                       &SessionWorker::applyRuntimeOptions);
    relayWorkerRequest(&TerminalController::readOnlyRequested,
                       &SessionWorker::setReadOnly);
    relayWorkerRequest(&TerminalController::shutdownRequested,
                       &SessionWorker::shutdown);
}

TerminalController::TerminalController(
    const TerminalSessionLaunchOptions &options, QObject *parent,
    std::shared_ptr<InitialSessionCoordinator> initialSessionCoordinator)
    : QObject(parent)
    , launchOptions_(options)
    , initialSessionCoordinator_(std::move(initialSessionCoordinator))
    , clipboardBridge_(std::make_shared<TerminalClipboardBridge>())
    , baseTitle_(options.configuredTitle)
    , currentDirectory_(options.inheritWorkingDirectory
                            ? TerminalPath{}
                            : options.workingDirectory)
    , explicitProgram_(options.firstSessionCommandOverride.has_value()
                       || hasExplicitCommand(options))
{
    registerMetaTypesOnce<
        TerminalUpdate, TerminalHyperlinkState, TerminalLinkKind,
        TerminalSearchDirection, TerminalSearchUpdate, TerminalViewportRequest,
        TerminalSelectionAdjustment, TerminalKeyInput, TerminalInputMethodInput,
        TerminalSequenceResolution, TerminalMouseInput, TerminalWheelInput,
        TerminalRightClickInput, TerminalRightClickResult,
        TerminalSelectionPressInput, TerminalSelectionDragInput,
        QVector<QPoint>, TerminalSessionRuntimeOptions,
        TerminalClipboardDestination, TerminalClipboardWriteRequest,
        TerminalClipboardReadRequest, TerminalClipboardReadReply,
        TerminalClipboardWriteReply, TerminalDesktopNotification,
        TerminalProgressReport, TerminalActionResult, TerminalWriteFileAction,
        TerminalInspectorSnapshot, TerminalInspectorCellSnapshot,
        TerminalKeyboardTraceDecision, TerminalKeyboardTraceResult>();

    if (initialSessionCoordinator_ != nullptr) {
        launchOptions_.program.clear();
        launchOptions_.hold = false;
        connect(initialSessionCoordinator_.get(),
                &InitialSessionCoordinator::requestsChanged, this,
                &TerminalController::tryStartSession, Qt::QueuedConnection);
    }
    (void)applyFirstSessionCommandOverride();
    if (initialSessionCoordinator_ == nullptr) {
        (void)installDirectLaunchBaseTitle();
    }
    connectWorkerRequestRelays();
}

void TerminalController::connectWorkerResults(SessionWorker *worker)
{
    connect(worker, &SessionWorker::terminalUpdated, this,
            &TerminalController::terminalUpdated, Qt::QueuedConnection);
    connect(
        worker, &SessionWorker::titleChanged, this,
        [this](const QString &title) {
            // A configured title is a live policy, not merely an initial
            // value. Keep consuming terminal metadata in the worker while
            // preventing it from replacing the GUI's configured base layer.
            if (launchOptions_.configuredTitle.has_value()) return;
            // OSC and set_surface_title share Ghostty's application-runtime
            // base layer. A present empty title is not the absence that
            // selects the launch fallback.
            std::optional<QString> next(std::in_place, title);
            if (baseTitle_ == next) return;
            baseTitle_ = std::move(next);
            Q_EMIT titleChanged(this->title());
        },
        Qt::QueuedConnection);
    connect(
        worker, &SessionWorker::currentDirectoryChanged, this,
        [this](const QByteArray &directory) {
            if (currentDirectory_.bytes() == directory) return;
            currentDirectory_ = directory;
            Q_EMIT currentDirectoryChanged(currentDirectory());
        },
        Qt::QueuedConnection);
    connect(
        worker, &SessionWorker::mouseTrackingChanged, this,
        [this](bool enabled) {
            if (terminalMouseTracking_ == enabled) return;
            const bool previous = mouseTracking();
            terminalMouseTracking_ = enabled;
            Q_EMIT terminalMouseTrackingChanged(enabled);
            if (previous != mouseTracking()) {
                Q_EMIT mouseTrackingChanged(mouseTracking());
            }
        },
        Qt::QueuedConnection);
    connect(
        worker, &SessionWorker::keyboardActionModeChanged, this,
        [this](bool enabled) {
            if (keyboardActionMode_ == enabled) return;
            keyboardActionMode_ = enabled;
            Q_EMIT keyboardActionModeChanged(enabled);
        },
        Qt::QueuedConnection);
    connect(
        worker, &SessionWorker::activeProcessChanged, this,
        [this](bool active) {
            if (activeProcess_ == active) return;
            activeProcess_ = active;
            Q_EMIT activeProcessChanged(activeProcess_);
        },
        Qt::QueuedConnection);
    connect(
        worker, &SessionWorker::inputActivityReconciled, this,
        [this](bool active) {
            if (activeProcess_ == active) return;
            activeProcess_ = active;
            Q_EMIT activeProcessChanged(activeProcess_);
        },
        Qt::QueuedConnection);
    connect(
        worker, &SessionWorker::selectionAvailableChanged, this,
        [this](bool available) {
            if (selectionAvailable_ == available) return;
            selectionAvailable_ = available;
            Q_EMIT selectionAvailableChanged(selectionAvailable_);
        },
        Qt::QueuedConnection);
    connect(
        worker, &SessionWorker::rightClickFinished, this,
        [this](const TerminalRightClickResult &result) {
            if (result.requestId == 0
                || !pendingRightClickRequestIds_.remove(result.requestId)) {
                return;
            }
            if (selectionAvailable_ != result.selectionAvailable) {
                selectionAvailable_ = result.selectionAvailable;
                const QPointer<TerminalController> guard(this);
                Q_EMIT selectionAvailableChanged(selectionAvailable_);
                if (guard == nullptr) return;
            }
            Q_EMIT rightClickResolved(result);
        },
        Qt::QueuedConnection);
    connect(
        worker, &SessionWorker::selectAllCompleted, this,
        [this](bool available) {
            if (selectionAvailable_ == available) return;
            selectionAvailable_ = available;
            Q_EMIT selectionAvailableChanged(selectionAvailable_);
        },
        Qt::QueuedConnection);
    connect(
        worker, &SessionWorker::hyperlinkResolved, this,
        [this](quint64 requestId, quint64 contentRevision,
               TerminalHyperlinkState state, TerminalLinkKind kind,
               const QByteArray &uri, const QPoint &targetCell,
               const QVector<QPoint> &matchingCells) {
            if (requestId == activeHyperlinkRequestId_) {
                if (state == TerminalHyperlinkState::Invalid
                    || state == TerminalHyperlinkState::Stale) {
                    // Clear before forwarding so a pane-side replacement
                    // request made synchronously from this signal cannot
                    // be clobbered when this handler returns.
                    activeHyperlinkRequestId_ = 0;
                }
                Q_EMIT hyperlinkResolved(contentRevision, state, kind, uri,
                                         targetCell, matchingCells);
            }
        },
        Qt::QueuedConnection);
    connect(
        worker, &SessionWorker::hyperlinkActivationResolved, this,
        [this](quint64 requestId, quint64 contentRevision,
               TerminalLinkKind kind, const QByteArray &uri) {
            if (requestId != activeHyperlinkActivationId_) {
                return;
            }
            activeHyperlinkActivationId_ = 0;
            Q_EMIT hyperlinkActivationResolved(contentRevision, kind, uri);
        },
        Qt::QueuedConnection);
    connect(
        worker, &SessionWorker::searchUpdated, this,
        [this](const TerminalSearchUpdate &update) {
            if (update.generation != activeSearchGeneration_) {
                return;
            }
            Q_EMIT searchUpdated(update);
        },
        Qt::QueuedConnection);
    connect(
        worker, &SessionWorker::terminalInspectorSnapshotReady, this,
        [this](quint64 requestId, const TerminalInspectorSnapshot &snapshot) {
            if (requestId == 0
                || requestId != activeTerminalInspectorRequestId_) {
                return;
            }
            activeTerminalInspectorRequestId_ = 0;
            Q_EMIT terminalInspectorSnapshotReady(requestId, snapshot);
        },
        Qt::QueuedConnection);
    connect(
        worker, &SessionWorker::terminalInspectorCellReady, this,
        [this](quint64 requestId,
               const TerminalInspectorCellSnapshot &snapshot) {
            if (requestId == 0
                || requestId != activeTerminalInspectorCellRequestId_) {
                return;
            }
            activeTerminalInspectorCellRequestId_ = 0;
            Q_EMIT terminalInspectorCellReady(requestId, snapshot);
        },
        Qt::QueuedConnection);
    connect(
        worker, &SessionWorker::keyboardTraceResult, this,
        [this](const TerminalKeyboardTraceResult &result) {
            if (result.generation == 0
                || result.generation != keyboardTraceGeneration_) {
                return;
            }
            Q_EMIT keyboardTraceResult(result);
        },
        Qt::QueuedConnection);
    connect(
        worker, &SessionWorker::clipboardTextReady, this,
        [this](const QString &text, TerminalClipboardDestination destination) {
            Q_EMIT selectionClipboardWriteRequested(text, destination);
        },
        Qt::QueuedConnection);
    connect(worker, &SessionWorker::terminalClipboardWriteRequested, this,
            &TerminalController::terminalClipboardWriteRequested,
            Qt::QueuedConnection);
    connect(worker, &SessionWorker::terminalClipboardReadRequested, this,
            &TerminalController::terminalClipboardReadRequested,
            Qt::QueuedConnection);
    connect(worker, &SessionWorker::desktopNotificationRequested, this,
            &TerminalController::desktopNotificationRequested,
            Qt::QueuedConnection);
    connect(worker, &SessionWorker::progressReportRequested, this,
            &TerminalController::progressReportRequested, Qt::QueuedConnection);
    connect(
        worker, &SessionWorker::terminalActionFinished, this,
        [this](const TerminalActionResult &result) {
            if (!pendingTerminalActionRequests_.remove(result.requestId)) {
                return;
            }
            Q_EMIT terminalActionReady(result);
        },
        Qt::QueuedConnection);
    connect(worker, &SessionWorker::unsafePasteConfirmationRequested, this,
            &TerminalController::unsafePasteConfirmationRequested,
            Qt::QueuedConnection);
    connect(worker, &SessionWorker::errorOccurred, this,
            &TerminalController::errorOccurred, Qt::QueuedConnection);
    connect(worker, &SessionWorker::bell, this, &TerminalController::bell,
            Qt::QueuedConnection);
    connect(worker, &SessionWorker::exitKeyDismissed, this,
            &TerminalController::exitKeyDismissed, Qt::QueuedConnection);
    connect(
        worker, &SessionWorker::sessionExited, this,
        [this](int exitCode, int signalNumber, bool hold, bool waitForKey,
               quint64 runtimeMilliseconds, bool abnormal) {
            if (closing_) return;
            // Invalidate queued search progress and selection-derived
            // queries before observers clear their UI. The held terminal
            // remains readable, so also ask the worker to release search
            // state and finish recompressing any pages restored by it.
            const QPointer<TerminalController> guard(this);
            (void)cancelSearch();
            if (guard == nullptr) return;
            failPendingTerminalActions();
            if (guard == nullptr) return;
            if (activeProcess_) {
                activeProcess_ = false;
                Q_EMIT activeProcessChanged(false);
                if (guard == nullptr) return;
            }
            if (running_) {
                running_ = false;
                Q_EMIT runningChanged(false);
                if (guard == nullptr) return;
            }
            Q_EMIT sessionExited(exitCode, signalNumber, hold, waitForKey,
                                 runtimeMilliseconds, abnormal);
        },
        Qt::QueuedConnection);
}

void TerminalController::enqueueWorkerRequest(WorkerRequest request)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (worker_.isNull()) {
        if (sessionStartState_ == SessionStartState::Cancelled || closing_) {
            return;
        }
        pendingWorkerRequests_.push_back(std::move(request));
        return;
    }

    const QPointer<SessionWorker> worker = worker_;
    QMetaObject::invokeMethod(
        worker.data(),
        [worker, request = std::move(request)]() mutable {
            if (worker != nullptr) {
                std::invoke(request, *worker);
            }
        },
        Qt::QueuedConnection);
}

bool TerminalController::requestIoCrash()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (closing_ || sessionStartState_ == SessionStartState::Cancelled) {
        return false;
    }
    enqueueWorkerRequest([](SessionWorker &) { intentionalCrash("I/O"); });
    return true;
}

void TerminalController::createWorkerRuntime()
{
    Q_ASSERT(thread_ == nullptr);
    Q_ASSERT(worker_.isNull());

    thread_ = new QThread(this);
    auto *worker = new SessionWorker;
    worker->setClipboardBridge(clipboardBridge_);
    worker_ = worker;
    worker->moveToThread(thread_);
    connectWorkerResults(worker);
    connect(thread_, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread_, &QThread::started, worker,
            [worker, launchOptions = launchOptions_,
             coordinator = initialSessionCoordinator_,
             ticket = initialSessionTicket_] {
                worker->initialize(
                    launchOptions, [coordinator, ticket](bool initialized) {
                        if (coordinator == nullptr || !ticket.has_value()) {
                            return;
                        }
                        const bool resolved = initialized
                            ? coordinator->commit(*ticket)
                            : coordinator->release(*ticket);
                        Q_ASSERT(resolved);
                        Q_UNUSED(resolved);
                    });
            });

    std::vector<WorkerRequest> pending =
        std::exchange(pendingWorkerRequests_, {});
    for (WorkerRequest &request : pending) {
        enqueueWorkerRequest(std::move(request));
    }
    thread_->start();
}

bool TerminalController::startSession(
    std::optional<TerminalSessionGeometry> initialGeometry)
{
    if (sessionStartState_ != SessionStartState::Idle || closing_) {
        return false;
    }
    if (initialGeometry.has_value()) {
        launchOptions_.initialGeometry =
            normalizedTerminalSessionGeometry(*initialGeometry);
    }

    sessionStartState_ = SessionStartState::WaitingForLease;
    tryStartSession();
    return true;
}

void TerminalController::tryStartSession()
{
    if (sessionStartState_ != SessionStartState::WaitingForLease || closing_) {
        return;
    }

    bool launchTitleChanged = false;
    if (initialSessionCoordinator_ != nullptr) {
        const InitialSessionCoordinator::RequestResult result =
            initialSessionCoordinator_->request(initialSessionTicket_);
        if (result.ticket.isValid()) {
            initialSessionTicket_ = result.ticket;
        }
        switch (result.status) {
        case InitialSessionCoordinator::RequestStatus::Waiting: return;
        case InitialSessionCoordinator::RequestStatus::Granted:
            if (!result.payload.has_value()) {
                sessionStartState_ = SessionStartState::Cancelled;
                pendingWorkerRequests_.clear();
                cancelInitialSessionRequest();
                const QPointer<TerminalController> guard(this);
                Q_EMIT errorOccurred(QStringLiteral(
                    "The initial-session lease had no launch payload"));
                if (guard == nullptr) return;
                failPendingTerminalActions();
                return;
            }
            launchTitleChanged = applyInitialSessionPayload(*result.payload);
            break;
        case InitialSessionCoordinator::RequestStatus::Consumed:
            initialSessionTicket_.reset();
            break;
        case InitialSessionCoordinator::RequestStatus::Invalid:
            sessionStartState_ = SessionStartState::Cancelled;
            pendingWorkerRequests_.clear();
            cancelInitialSessionRequest();
            const QPointer<TerminalController> guard(this);
            Q_EMIT errorOccurred(
                QStringLiteral("The initial-session lease became invalid"));
            if (guard == nullptr) return;
            failPendingTerminalActions();
            return;
        }
    }
    launchTitleChanged =
        applyFirstSessionCommandOverride() || launchTitleChanged;
    const bool baseTitleChanged = installDirectLaunchBaseTitle();

    sessionStartState_ = SessionStartState::Started;
    const bool notifyRunning = !running_;
    const bool notifyActiveProcess = !activeProcess_;
    // Treat a starting child conservatively until the worker can identify an
    // idle interactive-shell prompt.
    running_ = true;
    activeProcess_ = true;

    createWorkerRuntime();

    QPointer<TerminalController> guard(this);
    if (launchTitleChanged) {
        Q_EMIT launchProgramChanged();
        if (guard.isNull()) return;
    }
    if (baseTitleChanged) {
        Q_EMIT titleChanged(title());
        if (guard.isNull()) return;
    }
    if (notifyRunning) {
        Q_EMIT runningChanged(true);
        if (guard.isNull()) return;
    }
    if (notifyActiveProcess) {
        Q_EMIT activeProcessChanged(true);
    }
}

bool TerminalController::applyInitialSessionPayload(
    const InitialSessionCoordinator::Payload &payload)
{
    const QStringList previousProgram = launchOptions_.program;
    const std::optional<TerminalCommand> previousCommand =
        launchOptions_.command;
    launchOptions_.program = payload.program;
    if (payload.program.isEmpty()) {
        // An empty selected command is meaningful: a reload before the first
        // lease was granted may have reset both command settings. A positional
        // frontend argv, however, merely overrides the first execution and
        // leaves this pane's ordinary command snapshot intact.
        launchOptions_.command = payload.command;
    }
    launchOptions_.hold = payload.hold;
    explicitProgram_ = hasExplicitCommand(launchOptions_);
    return launchOptions_.program != previousProgram
        || launchOptions_.command != previousCommand;
}

bool TerminalController::applyFirstSessionCommandOverride()
{
    if (!launchOptions_.firstSessionCommandOverride.has_value()) return false;

    const bool changed = !launchOptions_.program.isEmpty()
        || launchOptions_.command != launchOptions_.firstSessionCommandOverride
        || launchOptions_.hold;
    launchOptions_.program.clear();
    launchOptions_.command = launchOptions_.firstSessionCommandOverride;
    launchOptions_.hold = false;
    explicitProgram_ = true;
    return changed;
}

std::optional<QString> TerminalController::directLaunchBaseTitle() const
{
    if (!launchOptions_.program.isEmpty()) {
        return launchOptions_.program.constFirst();
    }
    if (!launchOptions_.command.has_value()
        || launchOptions_.command->kind != TerminalCommandKind::Direct
        || launchOptions_.command->directArguments.isEmpty()) {
        return std::nullopt;
    }
    return QString::fromLocal8Bit(
        launchOptions_.command->directArguments.constFirst());
}

bool TerminalController::installDirectLaunchBaseTitle()
{
    if (baseTitle_.has_value()) return false;
    const std::optional<QString> title = directLaunchBaseTitle();
    if (!title.has_value()) return false;
    baseTitle_ = title;
    return true;
}

QString TerminalController::launchTitle() const
{
    if (!launchOptions_.program.isEmpty()) {
        return QFileInfo(launchOptions_.program.constFirst()).fileName();
    }
    if (!launchOptions_.command.has_value()) {
        return QStringLiteral("Terminal");
    }

    const TerminalCommand &command = *launchOptions_.command;
    switch (command.kind) {
    case TerminalCommandKind::Shell: return QStringLiteral("Terminal");
    case TerminalCommandKind::Direct:
        if (command.directArguments.isEmpty()
            || command.directArguments.constFirst().isEmpty()) {
            return QStringLiteral("Terminal");
        }
        return QString::fromLocal8Bit(command.directArguments.constFirst());
    }
    return QStringLiteral("Terminal");
}

void TerminalController::cancelInitialSessionRequest()
{
    if (initialSessionCoordinator_ != nullptr
        && initialSessionTicket_.has_value()) {
        (void)initialSessionCoordinator_->cancel(*initialSessionTicket_);
        initialSessionTicket_.reset();
    }
}

TerminalController::~TerminalController()
{
    closing_ = true;
    clipboardBridge_->cancelAll();
    pendingRightClickRequestIds_.clear();
    pendingWorkerRequests_.clear();
    pendingTerminalActionRequests_.clear();
    if (thread_ != nullptr && thread_->isRunning()) {
        if (worker_ != nullptr) {
            QMetaObject::invokeMethod(worker_.data(), &SessionWorker::shutdown,
                                      Qt::BlockingQueuedConnection);
        }
        thread_->quit();
        thread_->wait();
    }
    cancelInitialSessionRequest();
    worker_.clear();
}

void TerminalController::setSurfaceTitle(QString title)
{
    std::optional<QString> next(std::in_place, std::move(title));
    if (baseTitle_ == next) {
        return;
    }
    baseTitle_ = std::move(next);
    Q_EMIT titleChanged(this->title());
}

void TerminalController::resizeTerminal(const TerminalSessionGeometry &geometry)
{
    const TerminalSessionGeometry normalized =
        normalizedTerminalSessionGeometry(geometry);
    if (sessionStartState_ != SessionStartState::Started) {
        if (sessionStartState_ != SessionStartState::Cancelled) {
            launchOptions_.initialGeometry = normalized;
        }
        return;
    }
    Q_EMIT resizeRequested(normalized);
}

void TerminalController::applyConfiguredTitle(
    const std::optional<QString> &title)
{
    // Reapplying a configured title is meaningful even when the value is
    // unchanged: set_surface_title may have temporarily replaced the base.
    // Clearing the policy deliberately leaves the current base intact until
    // a later terminal title event replaces it.
    launchOptions_.configuredTitle = title;
    if (!title.has_value() || baseTitle_ == title) return;
    baseTitle_ = title;
    Q_EMIT titleChanged(this->title());
}

void TerminalController::applyRuntimeOptions(
    const TerminalSessionRuntimeOptions &options)
{
    if (launchOptions_.runtime == options) return;
    // Keep the GUI-side policy mirror current even after the worker starts.
    // KAM presentation routing combines this value with terminal-owned mode 2
    // while the worker independently applies the same authoritative snapshot.
    launchOptions_.runtime = options;
    if (sessionStartState_ != SessionStartState::Started) {
        return;
    }
    Q_EMIT runtimeOptionsRequested(options);
}

void TerminalController::beginShutdown()
{
    clipboardBridge_->cancelAll();
    setKeyboardTraceGeneration(0);
    pendingRightClickRequestIds_.clear();
    activeTerminalInspectorRequestId_ = 0;
    activeTerminalInspectorCellRequestId_ = 0;
    if (sessionStartState_ != SessionStartState::Started) {
        sessionStartState_ = SessionStartState::Cancelled;
        pendingWorkerRequests_.clear();
        cancelInitialSessionRequest();
        failPendingTerminalActions();
        return;
    }
    if (thread_ != nullptr && thread_->isRunning() && worker_ != nullptr) {
        Q_EMIT shutdownRequested();
    }
}

void TerminalController::setMouseReportingEnabled(bool enabled)
{
    if (mouseReportingEnabled_ == enabled) {
        return;
    }
    const bool previous = mouseTracking();
    mouseReportingEnabled_ = enabled;
    if (previous != mouseTracking()) {
        Q_EMIT mouseTrackingChanged(mouseTracking());
    }
}

void TerminalController::setReadOnly(bool readOnly)
{
    if (readOnly_ == readOnly) {
        return;
    }
    readOnly_ = readOnly;
    Q_EMIT readOnlyChanged(readOnly_);
    Q_EMIT readOnlyRequested(readOnly_);
}

void TerminalController::setKeyboardTraceGeneration(quint64 generation)
{
    if (keyboardTraceGeneration_ == generation) return;
    keyboardTraceGeneration_ = generation;
    Q_EMIT keyboardTraceGenerationRequested(generation);
}

void TerminalController::sendKey(const TerminalKeyInput &input)
{
    if (!readOnly_ && keyMayStartProcess(input)) {
        notePotentialActivity();
    }
    Q_EMIT keyRequested(input);
}

quint64 TerminalController::beginSequence()
{
    // Zero is reserved for "no active sequence". Unsigned wraparound retains
    // monotonic modular ordering for the worker's stale-token guard.
    do {
        ++nextSequenceToken_;
    } while (nextSequenceToken_ == 0);

    const quint64 token = nextSequenceToken_;
    activeSequenceToken_ = token;
    stagedSequencePotentialActivity_ = false;
    return token;
}

bool TerminalController::stageSequenceKey(quint64 token,
                                          const TerminalKeyInput &input)
{
    if (token == 0 || token != activeSequenceToken_) {
        return false;
    }
    stagedSequencePotentialActivity_ =
        stagedSequencePotentialActivity_ || keyMayStartProcess(input);
    const QPointer<TerminalController> guard(this);
    Q_EMIT sequenceKeyStagingRequested(token, input);
    return guard != nullptr && guard->activeSequenceToken_ == token;
}

void TerminalController::resolveSequence(
    quint64 token, TerminalSequenceResolution resolution,
    const std::optional<TerminalKeyInput> &current, TerminalKeyInput traceInput)
{
    if (token == 0 || token != activeSequenceToken_) {
        return;
    }

    const bool flushStaged = resolution == TerminalSequenceResolution::Flush
        || resolution == TerminalSequenceResolution::FlushAndSendCurrent;
    const bool sendCurrent =
        resolution == TerminalSequenceResolution::FlushAndSendCurrent
        && current.has_value();
    const bool potentialActivity =
        (flushStaged && stagedSequencePotentialActivity_)
        || (sendCurrent && keyMayStartProcess(*current));

    activeSequenceToken_ = 0;
    stagedSequencePotentialActivity_ = false;
    const QPointer<TerminalController> guard(this);
    if (current.has_value()) traceInput = *current;
    Q_EMIT sequenceResolutionRequested(token, resolution, current.has_value(),
                                       traceInput);
    if (guard == nullptr) return;

    if (!readOnly_ && potentialActivity) {
        notePotentialActivity();
    }
}

void TerminalController::sendInputMethod(const TerminalInputMethodInput &input)
{
    if (!readOnly_
        && (input.commitText.contains(u'\n')
            || input.commitText.contains(u'\r'))) {
        notePotentialActivity();
    }
    Q_EMIT inputMethodRequested(input);
}

void TerminalController::sendCsi(const QByteArray &payload)
{
    if (!readOnly_ && SessionWorker::canonicalBytesMayStartProcess(payload)) {
        notePotentialActivity();
    }
    Q_EMIT csiRequested(payload);
}

void TerminalController::sendEscape(const QByteArray &payload)
{
    if (!readOnly_ && SessionWorker::canonicalBytesMayStartProcess(payload)) {
        notePotentialActivity();
    }
    Q_EMIT escapeRequested(payload);
}

void TerminalController::sendRawText(const QByteArray &serializedText)
{
    if (!readOnly_
        && SessionWorker::canonicalTextMayStartProcess(serializedText)) {
        notePotentialActivity();
    }
    Q_EMIT rawTextRequested(serializedText);
}

void TerminalController::resetTerminal()
{
    Q_EMIT resetTerminalRequested();
}

void TerminalController::sendMouse(const TerminalMouseInput &input)
{
    Q_EMIT mouseRequested(input);
}

void TerminalController::sendWheel(const TerminalWheelInput &input)
{
    Q_EMIT wheelRequested(input);
}

quint64 TerminalController::nextRightClickRequestId()
{
    do {
        ++nextRightClickRequestId_;
    } while (
        nextRightClickRequestId_ == 0
        || pendingRightClickRequestIds_.contains(nextRightClickRequestId_));
    return nextRightClickRequestId_;
}

quint64 TerminalController::requestRightClick(quint64 contentRevision,
                                              int column, int row,
                                              int modifiers,
                                              bool shiftBypassedMouseCapture)
{
    const quint64 requestId = nextRightClickRequestId();
    pendingRightClickRequestIds_.insert(requestId);
    Q_EMIT rightClickRequested({
        .requestId = requestId,
        .contentRevision = contentRevision,
        .column = column,
        .row = row,
        .modifiers = modifiers,
        .shiftBypassedMouseCapture = shiftBypassedMouseCapture,
    });
    return requestId;
}

void TerminalController::setFocused(bool focused)
{
    Q_EMIT focusRequested(focused);
}

void TerminalController::paste(const QString &text,
                               TerminalClipboardLocation location,
                               bool clipboardSource)
{
    Q_EMIT pasteRequested(text);
    Q_EMIT pasteWithMetadataRequested(text, location, clipboardSource);
}

void TerminalController::resolveTerminalClipboardRead(
    quint64 requestId, TerminalClipboardReadReply reply)
{
    (void)clipboardBridge_->resolveRead(requestId, std::move(reply));
}

void TerminalController::resolveTerminalClipboardWrite(
    quint64 requestId, TerminalClipboardWriteReply reply)
{
    (void)clipboardBridge_->resolveWrite(requestId, std::move(reply));
}

void TerminalController::confirmPaste(quint64 requestId)
{
    if (requestId != 0) {
        Q_EMIT confirmPasteRequested(requestId);
    }
}

void TerminalController::cancelPaste(quint64 requestId)
{
    if (requestId != 0) {
        Q_EMIT cancelPasteRequested(requestId);
    }
}

void TerminalController::notePotentialActivity()
{
    if (readOnly_ || !running_ || explicitProgram_ || activeProcess_) {
        return;
    }
    activeProcess_ = true;
    Q_EMIT activeProcessChanged(true);
}

void TerminalController::copySelection()
{
    Q_EMIT copyRequested();
}

bool TerminalController::beginTerminalActionRequest(quint64 requestId)
{
    if (requestId == 0 || pendingTerminalActionRequests_.contains(requestId)
        || sessionStartState_ == SessionStartState::Cancelled || closing_) {
        return false;
    }

    pendingTerminalActionRequests_.insert(requestId);
    return true;
}

bool TerminalController::copySelectionAction(quint64 requestId)
{
    if (!beginTerminalActionRequest(requestId)) {
        return false;
    }
    Q_EMIT copyActionRequested(requestId);
    return true;
}

bool TerminalController::writeTerminalFile(
    quint64 requestId, const TerminalWriteFileAction &action)
{
    if (!beginTerminalActionRequest(requestId)) {
        return false;
    }
    Q_EMIT writeTerminalFileRequested(requestId, action);
    return true;
}

void TerminalController::failPendingTerminalActions()
{
    QList<quint64> requestIds = pendingTerminalActionRequests_.values();
    pendingTerminalActionRequests_.clear();
    std::ranges::sort(requestIds);

    const QPointer<TerminalController> guard(this);
    for (quint64 requestId : requestIds) {
        Q_EMIT terminalActionReady(failedTerminalActionResult(requestId));
        if (guard == nullptr) return;
    }
}

void TerminalController::clearSelection()
{
    Q_EMIT clearSelectionRequested();
}

void TerminalController::clearSelectionIfMouseTracking()
{
    Q_EMIT clearSelectionIfMouseTrackingRequested();
}

void TerminalController::beginSelection(
    const TerminalSelectionPressInput &input)
{
    Q_EMIT beginSelectionRequested(input);
}

void TerminalController::updateSelection(
    const TerminalSelectionDragInput &input)
{
    Q_EMIT updateSelectionRequested(input);
}

void TerminalController::endSelection(int column, int row)
{
    Q_EMIT endSelectionRequested(column, row);
}

void TerminalController::cancelSelectionGesture()
{
    Q_EMIT cancelSelectionGestureRequested();
}

void TerminalController::selectAll()
{
    Q_EMIT selectAllRequested();
}

bool TerminalController::selectAllAction(quint64 requestId)
{
    if (!beginTerminalActionRequest(requestId)) {
        return false;
    }

    Q_EMIT selectAllActionRequested(requestId);
    return true;
}

void TerminalController::adjustSelection(TerminalSelectionAdjustment adjustment)
{
    Q_EMIT selectionAdjustmentRequested(adjustment);
}

bool TerminalController::adjustSelectionAction(
    quint64 requestId, TerminalSelectionAdjustment adjustment)
{
    if (!beginTerminalActionRequest(requestId)) {
        return false;
    }
    Q_EMIT selectionAdjustmentActionRequested(requestId, adjustment);
    return true;
}

void TerminalController::scrollViewport(const TerminalViewportRequest &request)
{
    Q_EMIT scrollRequested(request);
}

bool TerminalController::scrollToSelectionAction(quint64 requestId)
{
    if (!beginTerminalActionRequest(requestId)) {
        return false;
    }
    Q_EMIT scrollToSelectionActionRequested(requestId);
    return true;
}

quint64 TerminalController::nextSearchGeneration()
{
    do {
        ++nextSearchGeneration_;
    } while (nextSearchGeneration_ == 0);
    return nextSearchGeneration_;
}

void TerminalController::search(const QString &text)
{
    activeSearchGeneration_ = nextSearchGeneration();
    searchExpected_ = !text.isEmpty();
    Q_EMIT searchRequested(activeSearchGeneration_, text.toUtf8());
}

bool TerminalController::searchSerialized(const QByteArray &serializedText)
{
    const bool wasActive = searchExpected_;
    activeSearchGeneration_ = nextSearchGeneration();
    const std::optional<QByteArray> decoded =
        decodeGhosttyActionString(serializedText);
    searchExpected_ = decoded.has_value() && !decoded->isEmpty();
    Q_EMIT serializedSearchRequested(activeSearchGeneration_, serializedText);
    return searchExpected_ || wasActive;
}

bool TerminalController::cancelSearch()
{
    const bool wasActive = searchExpected_;
    activeSearchGeneration_ = nextSearchGeneration();
    searchExpected_ = false;
    Q_EMIT searchCancellationRequested(activeSearchGeneration_);
    return wasActive;
}

bool TerminalController::navigateSearch(TerminalSearchDirection direction)
{
    if (!searchExpected_ || activeSearchGeneration_ == 0) {
        return false;
    }
    Q_EMIT searchNavigationRequested(activeSearchGeneration_, direction);
    return true;
}

bool TerminalController::searchSelectionAction(quint64 requestId)
{
    if (!beginTerminalActionRequest(requestId)) {
        return false;
    }
    Q_EMIT searchSelectionActionRequested(requestId);
    return true;
}

quint64 TerminalController::nextHyperlinkRequestId()
{
    do {
        ++nextHyperlinkRequestId_;
    } while (nextHyperlinkRequestId_ == 0);
    return nextHyperlinkRequestId_;
}

void TerminalController::requestHyperlink(int column, int row,
                                          quint64 contentRevision)
{
    if (activeHyperlinkRequestId_ != 0) {
        Q_EMIT hyperlinkQueryCancellationRequested(activeHyperlinkRequestId_);
    }
    activeHyperlinkRequestId_ = nextHyperlinkRequestId();
    Q_EMIT hyperlinkQueryRequested(activeHyperlinkRequestId_, contentRevision,
                                   column, row);
}

void TerminalController::cancelHyperlinkRequest()
{
    if (activeHyperlinkRequestId_ != 0) {
        Q_EMIT hyperlinkQueryCancellationRequested(activeHyperlinkRequestId_);
    }
    activeHyperlinkRequestId_ = 0;
}

quint64 TerminalController::prepareHyperlinkActivation(int column, int row,
                                                       quint64 contentRevision)
{
    if (activeHyperlinkActivationId_ != 0) {
        Q_EMIT hyperlinkActivationCancellationRequested(
            activeHyperlinkActivationId_);
    }
    activeHyperlinkActivationId_ = nextHyperlinkRequestId();
    Q_EMIT hyperlinkActivationPreparationRequested(
        activeHyperlinkActivationId_, contentRevision, column, row);
    return activeHyperlinkActivationId_;
}

void TerminalController::commitHyperlinkActivation(quint64 requestId,
                                                   int column, int row)
{
    if (requestId == 0 || requestId != activeHyperlinkActivationId_) {
        return;
    }
    Q_EMIT hyperlinkActivationCommitRequested(requestId, column, row);
}

void TerminalController::cancelHyperlinkActivation(quint64 requestId)
{
    if (requestId == 0 || requestId != activeHyperlinkActivationId_) {
        return;
    }
    Q_EMIT hyperlinkActivationCancellationRequested(requestId);
    activeHyperlinkActivationId_ = 0;
}

quint64 TerminalController::requestTerminalInspectorSnapshot()
{
    if (sessionStartState_ != SessionStartState::Started || closing_
        || activeTerminalInspectorRequestId_ != 0) {
        return 0;
    }

    do {
        ++nextTerminalInspectorRequestId_;
    } while (nextTerminalInspectorRequestId_ == 0);
    const quint64 requestId = nextTerminalInspectorRequestId_;
    activeTerminalInspectorRequestId_ = requestId;
    const QPointer<TerminalController> guard(this);
    Q_EMIT terminalInspectorSnapshotRequested(requestId);
    return guard != nullptr && activeTerminalInspectorRequestId_ == requestId
        ? requestId
        : 0;
}

quint64 TerminalController::requestTerminalInspectorCell(
    quint64 contentRevision, int viewportColumn, int viewportRow)
{
    if (sessionStartState_ != SessionStartState::Started || closing_) {
        return 0;
    }

    do {
        ++nextTerminalInspectorCellRequestId_;
    } while (nextTerminalInspectorCellRequestId_ == 0);
    const quint64 requestId = nextTerminalInspectorCellRequestId_;
    activeTerminalInspectorCellRequestId_ = requestId;
    const QPointer<TerminalController> guard(this);
    Q_EMIT terminalInspectorCellRequested(requestId, contentRevision,
                                          viewportColumn, viewportRow);
    return guard != nullptr
            && activeTerminalInspectorCellRequestId_ == requestId
        ? requestId
        : 0;
}
