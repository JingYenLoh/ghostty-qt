#include "terminal_controller.h"

#include "session_worker.h"
#include "terminal_clipboard.h"

#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <algorithm>
#include <utility>

namespace {

bool keyMayStartProcess(const TerminalKeyInput &input)
{
    return input.pressed
        && (input.key == Qt::Key_Return || input.key == Qt::Key_Enter
            || input.text.contains(u'\n') || input.text.contains(u'\r'));
}

} // namespace

TerminalController::TerminalController(
    const TerminalSessionLaunchOptions &options, QObject *parent,
    TerminalSessionStartMode startMode)
    : QObject(parent)
    , launchOptions_(options)
    , currentDirectory_(options.inheritWorkingDirectory
                            ? QString{}
                            : options.workingDirectory)
    , explicitProgram_(!options.program.isEmpty())
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
    qRegisterMetaType<QVector<QPoint>>();
    qRegisterMetaType<TerminalSessionRuntimeOptions>();
    qRegisterMetaType<TerminalClipboardDestination>();

    thread_ = new QThread(this);
    worker_ = new SessionWorker;
    worker_->moveToThread(thread_);

    connect(this, &TerminalController::resizeRequested,
            worker_, &SessionWorker::resizeTerminal, Qt::QueuedConnection);
    connect(this, &TerminalController::keyRequested,
            worker_, &SessionWorker::sendKey, Qt::QueuedConnection);
    connect(this, &TerminalController::sequenceKeyStagingRequested,
            worker_, &SessionWorker::stageSequenceKey, Qt::QueuedConnection);
    connect(this, &TerminalController::sequenceResolutionRequested,
            worker_, &SessionWorker::resolveSequence, Qt::QueuedConnection);
    connect(this, &TerminalController::inputMethodRequested,
            worker_, &SessionWorker::sendInputMethod, Qt::QueuedConnection);
    connect(this, &TerminalController::csiRequested,
            worker_, &SessionWorker::sendCsi, Qt::QueuedConnection);
    connect(this, &TerminalController::escapeRequested,
            worker_, &SessionWorker::sendEscape, Qt::QueuedConnection);
    connect(this, &TerminalController::rawTextRequested,
            worker_, &SessionWorker::sendRawText, Qt::QueuedConnection);
    connect(this, &TerminalController::resetTerminalRequested,
            worker_, &SessionWorker::resetTerminal, Qt::QueuedConnection);
    connect(this, &TerminalController::mouseRequested,
            worker_, &SessionWorker::sendMouse, Qt::QueuedConnection);
    connect(this, &TerminalController::focusRequested,
            worker_, &SessionWorker::setFocused, Qt::QueuedConnection);
    connect(this, &TerminalController::pasteRequested,
            worker_, &SessionWorker::paste, Qt::QueuedConnection);
    connect(this, &TerminalController::confirmPasteRequested,
            worker_, &SessionWorker::confirmPaste, Qt::QueuedConnection);
    connect(this, &TerminalController::cancelPasteRequested,
            worker_, &SessionWorker::cancelPaste, Qt::QueuedConnection);
    connect(this, &TerminalController::copyRequested,
            worker_, &SessionWorker::copySelection, Qt::QueuedConnection);
    connect(this, &TerminalController::clearSelectionRequested,
            worker_, &SessionWorker::clearSelection, Qt::QueuedConnection);
    connect(this, &TerminalController::beginSelectionRequested,
            worker_, &SessionWorker::beginSelection, Qt::QueuedConnection);
    connect(this, &TerminalController::updateSelectionRequested,
            worker_, &SessionWorker::updateSelection, Qt::QueuedConnection);
    connect(this, &TerminalController::endSelectionRequested,
            worker_, &SessionWorker::endSelection, Qt::QueuedConnection);
    connect(this, &TerminalController::selectAllRequested,
            worker_, &SessionWorker::selectAll, Qt::QueuedConnection);
    connect(this, &TerminalController::selectionAdjustmentRequested,
            worker_, &SessionWorker::adjustSelection, Qt::QueuedConnection);
    connect(this, &TerminalController::scrollRequested,
            worker_, &SessionWorker::scrollViewport, Qt::QueuedConnection);
    connect(this, &TerminalController::searchRequested,
            worker_, &SessionWorker::search, Qt::QueuedConnection);
    connect(this, &TerminalController::serializedSearchRequested,
            worker_, &SessionWorker::searchSerialized, Qt::QueuedConnection);
    connect(this, &TerminalController::searchCancellationRequested,
            worker_, &SessionWorker::cancelSearch, Qt::QueuedConnection);
    connect(this, &TerminalController::searchNavigationRequested,
            worker_, &SessionWorker::navigateSearch, Qt::QueuedConnection);
    connect(this, &TerminalController::searchSelectionRequested,
            worker_, &SessionWorker::requestSearchSelection,
            Qt::QueuedConnection);
    connect(this, &TerminalController::hyperlinkQueryRequested,
            worker_, &SessionWorker::queryHyperlink, Qt::QueuedConnection);
    connect(this, &TerminalController::hyperlinkQueryCancellationRequested,
            worker_, &SessionWorker::cancelHyperlinkQuery,
            Qt::QueuedConnection);
    connect(this,
            &TerminalController::hyperlinkActivationPreparationRequested,
            worker_, &SessionWorker::prepareHyperlinkActivation,
            Qt::QueuedConnection);
    connect(this, &TerminalController::hyperlinkActivationCommitRequested,
            worker_, &SessionWorker::commitHyperlinkActivation,
            Qt::QueuedConnection);
    connect(this,
            &TerminalController::hyperlinkActivationCancellationRequested,
            worker_, &SessionWorker::cancelHyperlinkActivation,
            Qt::QueuedConnection);
    connect(this, &TerminalController::runtimeOptionsRequested,
            worker_, &SessionWorker::applyRuntimeOptions, Qt::QueuedConnection);
    connect(this, &TerminalController::readOnlyRequested,
            worker_, &SessionWorker::setReadOnly, Qt::QueuedConnection);
    connect(this, &TerminalController::shutdownRequested,
            worker_, &SessionWorker::shutdown, Qt::QueuedConnection);

    connect(worker_, &SessionWorker::terminalUpdated,
            this, &TerminalController::terminalUpdated, Qt::QueuedConnection);
    connect(worker_, &SessionWorker::titleChanged, this,
            [this](const QString &title) {
                // Empty terminal metadata means no title and restores the
                // pane's launch fallback. set_surface_title: instead installs
                // an explicit empty optional until this worker event arrives.
                std::optional<QString> next;
                if (!title.isEmpty()) next = title;
                if (baseTitle_ == next) return;
                baseTitle_ = std::move(next);
                Q_EMIT titleChanged(this->title());
            }, Qt::QueuedConnection);
    connect(worker_, &SessionWorker::currentDirectoryChanged, this,
            [this](const QString &directory) {
                if (currentDirectory_ == directory) return;
                currentDirectory_ = directory;
                Q_EMIT currentDirectoryChanged(currentDirectory_);
            }, Qt::QueuedConnection);
    connect(worker_, &SessionWorker::mouseTrackingChanged, this,
            [this](bool enabled) {
                if (terminalMouseTracking_ == enabled) return;
                const bool previous = mouseTracking();
                terminalMouseTracking_ = enabled;
                Q_EMIT terminalMouseTrackingChanged(enabled);
                if (previous != mouseTracking()) {
                    Q_EMIT mouseTrackingChanged(mouseTracking());
                }
            }, Qt::QueuedConnection);
    connect(worker_, &SessionWorker::activeProcessChanged, this,
            [this](bool active) {
                if (activeProcess_ == active) return;
                activeProcess_ = active;
                Q_EMIT activeProcessChanged(activeProcess_);
            }, Qt::QueuedConnection);
    connect(worker_, &SessionWorker::selectionAvailableChanged, this,
            [this](bool available) {
                if (selectionAvailable_ == available) return;
                selectionAvailable_ = available;
                Q_EMIT selectionAvailableChanged(selectionAvailable_);
            }, Qt::QueuedConnection);
    connect(worker_, &SessionWorker::selectAllCompleted, this,
            [this](bool available) {
                pendingSelectAllRequests_ = std::max(
                    0, pendingSelectAllRequests_ - 1);
                if (selectionAvailable_ == available) return;
                selectionAvailable_ = available;
                Q_EMIT selectionAvailableChanged(selectionAvailable_);
            }, Qt::QueuedConnection);
    connect(worker_, &SessionWorker::hyperlinkResolved, this,
            [this](quint64 requestId, quint64 contentRevision,
                   TerminalHyperlinkState state, TerminalLinkKind kind,
                   const QByteArray &uri,
                   const QPoint &targetCell,
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
            }, Qt::QueuedConnection);
    connect(worker_, &SessionWorker::hyperlinkActivationResolved, this,
            [this](quint64 requestId, quint64 contentRevision,
                   TerminalLinkKind kind, const QByteArray &uri) {
                if (requestId != activeHyperlinkActivationId_) {
                    return;
                }
                activeHyperlinkActivationId_ = 0;
                Q_EMIT hyperlinkActivationResolved(
                    contentRevision, kind, uri);
            }, Qt::QueuedConnection);
    connect(worker_, &SessionWorker::searchUpdated, this,
            [this](const TerminalSearchUpdate &update) {
                if (update.generation != activeSearchGeneration_) {
                    return;
                }
                searchExpected_ = update.active;
                Q_EMIT searchUpdated(update);
            }, Qt::QueuedConnection);
    connect(worker_, &SessionWorker::searchSelectionReady, this,
            [this](quint64 requestId, bool available, const QString &text) {
                if (requestId != activeSearchSelectionRequestId_) {
                    return;
                }
                activeSearchSelectionRequestId_ = 0;
                Q_EMIT searchSelectionReady(available, text);
            }, Qt::QueuedConnection);
    connect(worker_, &SessionWorker::clipboardTextReady, this,
            [](const QString &text,
               TerminalClipboardDestination destination) {
                writeTerminalClipboard(QGuiApplication::clipboard(), text,
                                       destination);
            }, Qt::QueuedConnection);
    connect(worker_, &SessionWorker::unsafePasteConfirmationRequested,
            this, &TerminalController::unsafePasteConfirmationRequested,
            Qt::QueuedConnection);
    connect(worker_, &SessionWorker::errorOccurred,
            this, &TerminalController::errorOccurred, Qt::QueuedConnection);
    connect(worker_, &SessionWorker::bell,
            this, &TerminalController::bell, Qt::QueuedConnection);
    connect(worker_, &SessionWorker::sessionExited, this,
            [this](int exitCode, int signalNumber, bool hold) {
                if (closing_) return;
                // Invalidate queued search progress and selection-derived
                // queries before observers clear their UI. The held terminal
                // remains readable, so also ask the worker to release search
                // state and finish recompressing any pages restored by it.
                cancelSearch();
                if (activeProcess_) {
                    activeProcess_ = false;
                    Q_EMIT activeProcessChanged(false);
                }
                if (running_) {
                    running_ = false;
                    Q_EMIT runningChanged(false);
                }
                Q_EMIT sessionExited(exitCode, signalNumber, hold);
            }, Qt::QueuedConnection);

    if (startMode == TerminalSessionStartMode::Immediate) {
        (void) startSession();
    }
}

bool TerminalController::startSession(
    std::optional<TerminalSessionGeometry> initialGeometry)
{
    if (sessionStarted_ || sessionStartCancelled_ || closing_) {
        return false;
    }
    if (initialGeometry.has_value()) {
        launchOptions_.initialGeometry =
            normalizedTerminalSessionGeometry(*initialGeometry);
    }

    sessionStarted_ = true;
    const bool notifyRunning = !running_;
    const bool notifyActiveProcess = !activeProcess_;
    // Treat a starting child conservatively until the worker can identify an
    // idle interactive-shell prompt.
    running_ = true;
    activeProcess_ = true;

    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(thread_, &QThread::started, worker_,
            [worker = worker_, launchOptions = launchOptions_] {
                worker->initialize(launchOptions);
            });
    thread_->start();

    QPointer<TerminalController> guard(this);
    if (notifyRunning) {
        Q_EMIT runningChanged(true);
        if (guard.isNull()) return true;
    }
    if (notifyActiveProcess) {
        Q_EMIT activeProcessChanged(true);
    }
    return true;
}

TerminalController::~TerminalController()
{
    closing_ = true;
    if (!sessionStarted_ && thread_ != nullptr && worker_ != nullptr) {
        // The worker already owns every pre-start queued request, but its
        // affinity thread has never run. Move it home from that thread's
        // started callback, quit before entering the event loop, then delete
        // it synchronously on this controller's thread. Pending MetaCall
        // events migrate with the worker and QObject destruction removes them
        // without invoking an uninitialized session.
        QThread *const controllerThread = QObject::thread();
        QThread *const workerThread = thread_;
        SessionWorker *const worker = worker_;
        connect(workerThread, &QThread::started, worker,
                [controllerThread, workerThread, worker] {
                    (void) worker->moveToThread(controllerThread);
                    workerThread->quit();
                }, Qt::DirectConnection);
        workerThread->start();
        workerThread->wait();
        delete worker_;
        worker_ = nullptr;
        return;
    }
    if (thread_ != nullptr && thread_->isRunning() && worker_ != nullptr) {
        QMetaObject::invokeMethod(worker_, &SessionWorker::shutdown,
                                  Qt::BlockingQueuedConnection);
        thread_->quit();
        thread_->wait();
    }
    worker_ = nullptr;
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

void TerminalController::resizeTerminal(int columns, int rows,
                                        int cellWidthPixels, int cellHeightPixels,
                                        int surfaceWidthPixels, int surfaceHeightPixels)
{
    if (!sessionStarted_) {
        if (!sessionStartCancelled_) {
            launchOptions_.initialGeometry =
                normalizedTerminalSessionGeometry({
                    .columns = columns,
                    .rows = rows,
                    .cellWidthPixels = cellWidthPixels,
                    .cellHeightPixels = cellHeightPixels,
                    .surfaceWidthPixels = surfaceWidthPixels,
                    .surfaceHeightPixels = surfaceHeightPixels,
                });
        }
        return;
    }
    Q_EMIT resizeRequested(columns, rows, cellWidthPixels, cellHeightPixels,
                         surfaceWidthPixels, surfaceHeightPixels);
}

void TerminalController::applyRuntimeOptions(
    const TerminalSessionRuntimeOptions &options)
{
    if (!sessionStarted_) {
        launchOptions_.runtime = options;
        return;
    }
    Q_EMIT runtimeOptionsRequested(options);
}

void TerminalController::beginShutdown()
{
    if (!sessionStarted_) {
        sessionStartCancelled_ = true;
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

quint64 TerminalController::stageSequenceKey(const TerminalKeyInput &input)
{
    // Zero is reserved for "no active sequence". Unsigned wraparound retains
    // monotonic modular ordering for the worker's stale-token guard.
    do {
        ++nextSequenceToken_;
    } while (nextSequenceToken_ == 0);

    activeSequenceToken_ = nextSequenceToken_;
    stagedSequencePotentialActivity_ = keyMayStartProcess(input);
    Q_EMIT sequenceKeyStagingRequested(activeSequenceToken_, input);
    return activeSequenceToken_;
}

void TerminalController::stageSequenceKey(quint64 token,
                                          const TerminalKeyInput &input)
{
    if (token == 0 || token != activeSequenceToken_) {
        return;
    }
    stagedSequencePotentialActivity_ =
        stagedSequencePotentialActivity_ || keyMayStartProcess(input);
    Q_EMIT sequenceKeyStagingRequested(token, input);
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
    Q_EMIT sequenceResolutionRequested(
        token, resolution, current.has_value(), current.value_or(TerminalKeyInput{}));
    if (!readOnly_ && potentialActivity) {
        notePotentialActivity();
    }
}

void TerminalController::sendInputMethod(
    const TerminalInputMethodInput &input)
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
    if (!readOnly_
        && SessionWorker::canonicalBytesMayStartProcess(payload)) {
        notePotentialActivity();
    }
    Q_EMIT csiRequested(payload);
}

void TerminalController::sendEscape(const QByteArray &payload)
{
    if (!readOnly_
        && SessionWorker::canonicalBytesMayStartProcess(payload)) {
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

void TerminalController::clearSelection()
{
    Q_EMIT clearSelectionRequested();
}

void TerminalController::beginSelection(int column, int row, int clickCount,
                                        bool rectangular)
{
    Q_EMIT beginSelectionRequested(column, row, clickCount, rectangular);
}

void TerminalController::updateSelection(int column, int row, bool rectangular)
{
    Q_EMIT updateSelectionRequested(column, row, rectangular);
}

void TerminalController::endSelection(int column, int row)
{
    Q_EMIT endSelectionRequested(column, row);
}

void TerminalController::selectAll()
{
    ++pendingSelectAllRequests_;
    Q_EMIT selectAllRequested();
}

void TerminalController::adjustSelection(TerminalSelectionAdjustment adjustment)
{
    Q_EMIT selectionAdjustmentRequested(adjustment);
}

void TerminalController::scrollViewport(const TerminalViewportRequest &request)
{
    Q_EMIT scrollRequested(request);
}

quint64 TerminalController::nextSearchGeneration()
{
    do {
        ++nextSearchGeneration_;
    } while (nextSearchGeneration_ == 0);
    return nextSearchGeneration_;
}

quint64 TerminalController::nextSearchSelectionRequestId()
{
    do {
        ++nextSearchSelectionRequestId_;
    } while (nextSearchSelectionRequestId_ == 0);
    return nextSearchSelectionRequestId_;
}

void TerminalController::search(const QString &text)
{
    activeSearchSelectionRequestId_ = 0;
    activeSearchGeneration_ = nextSearchGeneration();
    searchExpected_ = !text.isEmpty();
    Q_EMIT searchRequested(activeSearchGeneration_, text.toUtf8());
}

void TerminalController::searchSerialized(const QByteArray &serializedText)
{
    activeSearchSelectionRequestId_ = 0;
    activeSearchGeneration_ = nextSearchGeneration();
    // Escape decoding intentionally stays on the worker. Treat the request as
    // active until its authoritative update arrives so an action chain such
    // as `search:term>navigate_search:next` remains ordered and performable.
    searchExpected_ = !serializedText.isEmpty();
    Q_EMIT serializedSearchRequested(activeSearchGeneration_, serializedText);
}

void TerminalController::cancelSearch()
{
    activeSearchSelectionRequestId_ = 0;
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

void TerminalController::requestSearchSelection()
{
    activeSearchSelectionRequestId_ = nextSearchSelectionRequestId();
    Q_EMIT searchSelectionRequested(activeSearchSelectionRequestId_);
}

quint64 TerminalController::nextHyperlinkRequestId()
{
    do {
        ++nextHyperlinkRequestId_;
    } while (nextHyperlinkRequestId_ == 0);
    return nextHyperlinkRequestId_;
}

void TerminalController::requestHyperlink(
    int column, int row, quint64 contentRevision)
{
    if (activeHyperlinkRequestId_ != 0) {
        Q_EMIT hyperlinkQueryCancellationRequested(
            activeHyperlinkRequestId_);
    }
    activeHyperlinkRequestId_ = nextHyperlinkRequestId();
    Q_EMIT hyperlinkQueryRequested(activeHyperlinkRequestId_, contentRevision,
                                   column, row);
}

void TerminalController::cancelHyperlinkRequest()
{
    if (activeHyperlinkRequestId_ != 0) {
        Q_EMIT hyperlinkQueryCancellationRequested(
            activeHyperlinkRequestId_);
    }
    activeHyperlinkRequestId_ = 0;
}

quint64 TerminalController::prepareHyperlinkActivation(
    int column, int row, quint64 contentRevision)
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

void TerminalController::commitHyperlinkActivation(
    quint64 requestId, int column, int row)
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
