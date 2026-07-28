#include "terminal_controller.h"

#include "session_worker.h"
#include "terminal_clipboard.h"

#include <QFileInfo>
#include <QGuiApplication>
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
    return input.pressed
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
    relayWorkerRequest(&TerminalController::rightClickRequested,
                       &SessionWorker::resolveRightClick);
    relayWorkerRequest(&TerminalController::focusRequested,
                       &SessionWorker::setFocused);
    relayWorkerRequest(&TerminalController::pasteRequested,
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
    , currentDirectory_(options.inheritWorkingDirectory
                            ? QString{}
                            : options.workingDirectory)
    , explicitProgram_(hasExplicitCommand(options))
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalHyperlinkState>();
    qRegisterMetaType<TerminalLinkKind>();
    qRegisterMetaType<TerminalSearchDirection>();
    qRegisterMetaType<TerminalSearchUpdate>();
    qRegisterMetaType<TerminalViewportRequest>();
    qRegisterMetaType<TerminalSelectionAdjustment>();
    qRegisterMetaType<TerminalKeyInput>();
    qRegisterMetaType<TerminalInputMethodInput>();
    qRegisterMetaType<TerminalSequenceResolution>();
    qRegisterMetaType<TerminalMouseInput>();
    qRegisterMetaType<TerminalRightClickInput>();
    qRegisterMetaType<TerminalRightClickResult>();
    qRegisterMetaType<TerminalSelectionPressInput>();
    qRegisterMetaType<TerminalSelectionDragInput>();
    qRegisterMetaType<QVector<QPoint>>();
    qRegisterMetaType<TerminalSessionRuntimeOptions>();
    qRegisterMetaType<TerminalClipboardDestination>();
    qRegisterMetaType<TerminalClipboardWriteRequest>();
    qRegisterMetaType<TerminalActionResult>();
    qRegisterMetaType<TerminalWriteFileAction>();

    if (initialSessionCoordinator_ != nullptr) {
        launchOptions_.program.clear();
        launchOptions_.hold = false;
        explicitProgram_ = launchOptions_.command.has_value()
            && !launchOptions_.command->defaultShell;
        connect(initialSessionCoordinator_.get(),
                &InitialSessionCoordinator::requestsChanged, this,
                &TerminalController::tryStartSession, Qt::QueuedConnection);
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
            // Empty terminal metadata means no title and restores the
            // pane's launch fallback. set_surface_title: instead installs
            // an explicit empty optional until this worker event arrives.
            std::optional<QString> next;
            if (!title.isEmpty()) next = title;
            if (baseTitle_ == next) return;
            baseTitle_ = std::move(next);
            Q_EMIT titleChanged(this->title());
        },
        Qt::QueuedConnection);
    connect(
        worker, &SessionWorker::currentDirectoryChanged, this,
        [this](const QString &directory) {
            if (currentDirectory_ == directory) return;
            currentDirectory_ = directory;
            Q_EMIT currentDirectoryChanged(currentDirectory_);
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
            searchExpected_ = update.active;
            Q_EMIT searchUpdated(update);
        },
        Qt::QueuedConnection);
    connect(
        worker, &SessionWorker::clipboardTextReady, this,
        [](const QString &text, TerminalClipboardDestination destination) {
            writeTerminalClipboard(QGuiApplication::clipboard(), text,
                                   destination);
        },
        Qt::QueuedConnection);
    connect(worker, &SessionWorker::terminalClipboardWriteRequested, this,
            &TerminalController::terminalClipboardWriteRequested,
            Qt::QueuedConnection);
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
            cancelSearch();
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

void TerminalController::createWorkerRuntime()
{
    Q_ASSERT(thread_ == nullptr);
    Q_ASSERT(worker_.isNull());

    thread_ = new QThread(this);
    auto *worker = new SessionWorker;
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

void TerminalController::applyRuntimeOptions(
    const TerminalSessionRuntimeOptions &options)
{
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
    pendingRightClickRequestIds_.clear();
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

void TerminalController::sendKey(const TerminalKeyInput &input)
{
    if (!readOnly_ && input.pressed
        && (input.key == Qt::Key_Return || input.key == Qt::Key_Enter
            || input.text.contains(u'\n') || input.text.contains(u'\r'))) {
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
    const std::optional<TerminalKeyInput> &current)
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
    Q_EMIT sequenceResolutionRequested(token, resolution, current.has_value(),
                                       current.value_or(TerminalKeyInput{}));
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

void TerminalController::paste(const QString &text)
{
    Q_EMIT pasteRequested(text);
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

void TerminalController::searchSerialized(const QByteArray &serializedText)
{
    activeSearchGeneration_ = nextSearchGeneration();
    // Escape decoding intentionally stays on the worker. Treat the request as
    // active until its authoritative update arrives so an action chain such
    // as `search:term>navigate_search:next` remains ordered and performable.
    searchExpected_ = !serializedText.isEmpty();
    Q_EMIT serializedSearchRequested(activeSearchGeneration_, serializedText);
}

void TerminalController::cancelSearch()
{
    activeSearchGeneration_ = nextSearchGeneration();
    searchExpected_ = false;
    Q_EMIT searchCancellationRequested(activeSearchGeneration_);
}

void TerminalController::navigateSearch(TerminalSearchDirection direction)
{
    if (!searchExpected_ || activeSearchGeneration_ == 0) {
        return;
    }
    Q_EMIT searchNavigationRequested(activeSearchGeneration_, direction);
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
