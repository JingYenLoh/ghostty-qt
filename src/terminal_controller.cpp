#include "terminal_controller.h"

#include "ghostty_vt_adapter.h"
#include "session_worker.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMetaObject>
#include <QThread>

#include <algorithm>

namespace {

bool keyMayStartProcess(const TerminalKeyInput &input)
{
    return input.pressed
        && (input.key == Qt::Key_Return || input.key == Qt::Key_Enter
            || input.text.contains(u'\n') || input.text.contains(u'\r'));
}

} // namespace

TerminalController::TerminalController(const LaunchOptions &options, QObject *parent)
    : QObject(parent)
    , options_(options)
    , currentDirectory_(options.workingDirectory)
    // Treat a starting child conservatively until the worker can identify an
    // idle interactive-shell prompt.
    , activeProcess_(true)
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalViewportRequest>();
    qRegisterMetaType<TerminalSelectionAdjustment>();
    qRegisterMetaType<TerminalKeyInput>();
    qRegisterMetaType<TerminalSequenceResolution>();
    qRegisterMetaType<TerminalMouseInput>();
    qRegisterMetaType<LaunchOptions>();

    thread_ = new QThread(this);
    worker_ = new SessionWorker;
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);

    connect(this, &TerminalController::resizeRequested,
            worker_, &SessionWorker::resizeTerminal, Qt::QueuedConnection);
    connect(this, &TerminalController::keyRequested,
            worker_, &SessionWorker::sendKey, Qt::QueuedConnection);
    connect(this, &TerminalController::sequenceKeyStagingRequested,
            worker_, &SessionWorker::stageSequenceKey, Qt::QueuedConnection);
    connect(this, &TerminalController::sequenceResolutionRequested,
            worker_, &SessionWorker::resolveSequence, Qt::QueuedConnection);
    connect(this, &TerminalController::textRequested,
            worker_, &SessionWorker::sendText, Qt::QueuedConnection);
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
    connect(this, &TerminalController::runtimeOptionsRequested,
            worker_, &SessionWorker::applyRuntimeOptions, Qt::QueuedConnection);
    connect(this, &TerminalController::shutdownRequested,
            worker_, &SessionWorker::shutdown, Qt::QueuedConnection);

    connect(worker_, &SessionWorker::terminalUpdated,
            this, &TerminalController::terminalUpdated, Qt::QueuedConnection);
    connect(worker_, &SessionWorker::titleChanged, this,
            [this](const QString &title) {
                if (title_ == title) return;
                title_ = title;
                Q_EMIT titleChanged(title_);
            }, Qt::QueuedConnection);
    connect(worker_, &SessionWorker::currentDirectoryChanged, this,
            [this](const QString &directory) {
                if (currentDirectory_ == directory) return;
                currentDirectory_ = directory;
                Q_EMIT currentDirectoryChanged(currentDirectory_);
            }, Qt::QueuedConnection);
    connect(worker_, &SessionWorker::mouseTrackingChanged, this,
            [this](bool enabled) {
                if (mouseTracking_ == enabled) return;
                mouseTracking_ = enabled;
                Q_EMIT mouseTrackingChanged(mouseTracking_);
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
    connect(worker_, &SessionWorker::clipboardTextReady, this,
            [](const QString &text) {
                if (QGuiApplication::clipboard() != nullptr) {
                    QGuiApplication::clipboard()->setText(text);
                }
            }, Qt::QueuedConnection);
    connect(worker_, &SessionWorker::errorOccurred,
            this, &TerminalController::errorOccurred, Qt::QueuedConnection);
    connect(worker_, &SessionWorker::bell,
            this, &TerminalController::bell, Qt::QueuedConnection);
    connect(worker_, &SessionWorker::sessionExited, this,
            [this](int exitCode, int signalNumber, bool hold) {
                if (closing_) return;
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

    connect(thread_, &QThread::started, worker_,
            [worker = worker_, options] { worker->initialize(options); });
    thread_->start();
}

TerminalController::~TerminalController()
{
    closing_ = true;
    if (thread_ != nullptr && thread_->isRunning() && worker_ != nullptr) {
        QMetaObject::invokeMethod(worker_, &SessionWorker::shutdown,
                                  Qt::BlockingQueuedConnection);
        thread_->quit();
        thread_->wait();
    }
    worker_ = nullptr;
}

void TerminalController::resizeTerminal(int columns, int rows,
                                        int cellWidthPixels, int cellHeightPixels,
                                        int surfaceWidthPixels, int surfaceHeightPixels)
{
    Q_EMIT resizeRequested(columns, rows, cellWidthPixels, cellHeightPixels,
                         surfaceWidthPixels, surfaceHeightPixels);
}

void TerminalController::applyRuntimeOptions(const LaunchOptions &options)
{
    options_ = options;
    Q_EMIT runtimeOptionsRequested(options_);
}

void TerminalController::beginShutdown()
{
    if (thread_ != nullptr && thread_->isRunning() && worker_ != nullptr) {
        Q_EMIT shutdownRequested();
    }
}

void TerminalController::sendKey(const TerminalKeyInput &input)
{
    if (input.pressed
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
    if (potentialActivity) {
        notePotentialActivity();
    }
}

void TerminalController::sendText(const QString &text)
{
    if (text.contains(u'\n') || text.contains(u'\r')) {
        notePotentialActivity();
    }
    Q_EMIT textRequested(text);
}

void TerminalController::sendCsi(const QByteArray &payload)
{
    if (SessionWorker::canonicalBytesMayStartProcess(payload)) {
        notePotentialActivity();
    }
    Q_EMIT csiRequested(payload);
}

void TerminalController::sendEscape(const QByteArray &payload)
{
    if (SessionWorker::canonicalBytesMayStartProcess(payload)) {
        notePotentialActivity();
    }
    Q_EMIT escapeRequested(payload);
}

void TerminalController::sendRawText(const QByteArray &serializedText)
{
    if (SessionWorker::canonicalTextMayStartProcess(serializedText)) {
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
    if (text.contains(u'\n') || text.contains(u'\r')) {
        notePotentialActivity();
    }
    Q_EMIT pasteRequested(text);
}

void TerminalController::notePotentialActivity()
{
    if (!running_ || !options_.program.isEmpty() || activeProcess_) {
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

bool TerminalController::isPasteSafe(const QString &text)
{
    const QByteArray utf8 = text.toUtf8();
    return GhosttyVtAdapter::isPasteSafe(utf8);
}
