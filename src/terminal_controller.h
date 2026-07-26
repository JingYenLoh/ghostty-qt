#pragma once

#include "initial_session_coordinator.h"
#include "terminal_action_result.h"
#include "terminal_session_options.h"
#include "terminal_types.h"
#include "terminal_write_file_action.h"

#include <QByteArray>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QThread;
class SessionWorker;
class TerminalPane;

class TerminalController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(QString currentDirectory READ currentDirectory NOTIFY
                   currentDirectoryChanged)
    Q_PROPERTY(
        bool mouseTracking READ mouseTracking NOTIFY mouseTrackingChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(
        bool activeProcess READ activeProcess NOTIFY activeProcessChanged)
    Q_PROPERTY(bool selectionAvailable READ selectionAvailable NOTIFY
                   selectionAvailableChanged)
    Q_PROPERTY(bool readOnly READ readOnly NOTIFY readOnlyChanged)

public:
    explicit TerminalController(const TerminalSessionLaunchOptions &options,
                                QObject *parent = nullptr,
                                std::shared_ptr<InitialSessionCoordinator>
                                    initialSessionCoordinator = {});
    ~TerminalController() override;

    QString title() const
    {
        return baseTitle_.has_value() ? *baseTitle_ : QString{};
    }
    // An explicit empty set_surface_title value is distinct from the absence
    // of terminal metadata, which lets TerminalPane select its launch fallback.
    bool hasTitle() const { return baseTitle_.has_value(); }
    QString currentDirectory() const { return currentDirectory_; }
    // Effective capture requires both the terminal's DEC mouse mode and the
    // surface-local Ghostty policy. Keeping the conjunction here gives every
    // pointer path one authoritative predicate.
    bool mouseTracking() const
    {
        return terminalMouseTracking_ && mouseReportingEnabled_;
    }
    bool terminalMouseTracking() const { return terminalMouseTracking_; }
    bool mouseReportingEnabled() const { return mouseReportingEnabled_; }
    bool running() const { return running_; }
    bool activeProcess() const { return activeProcess_; }
    bool sessionStarted() const
    {
        return sessionStartState_ == SessionStartState::Started;
    }
    [[nodiscard]] const QStringList &launchProgram() const
    {
        return launchOptions_.program;
    }
    [[nodiscard]] const std::optional<TerminalCommand> &launchCommand() const
    {
        return launchOptions_.command;
    }
    // Human-readable fallback derived without changing the raw bytes used for
    // execution. Terminal-reported titles still take precedence in the pane.
    [[nodiscard]] QString launchTitle() const;
    [[nodiscard]] bool launchHold() const { return launchOptions_.hold; }
    [[nodiscard]] const QByteArray &launchTerm() const
    {
        return launchOptions_.term;
    }
    [[nodiscard]] const TerminalEnvironment &launchEnvironment() const
    {
        return launchOptions_.environment;
    }
    [[nodiscard]] const LinuxCgroupConfig &launchLinuxCgroup() const
    {
        return launchOptions_.linuxCgroup;
    }
    [[nodiscard]] bool launchProcessUsesSingleInstance() const
    {
        return launchOptions_.processUsesSingleInstance;
    }
    [[nodiscard]] const std::optional<TerminalSessionGeometry> &
    launchGeometry() const
    {
        return launchOptions_.initialGeometry;
    }
    // Accepts one start request. The actual worker may wait for the
    // application-wide initial-session lease. A supplied geometry replaces
    // every hidden/pre-presentation resize coalesced into the launch snapshot.
    [[nodiscard]] bool startSession(
        std::optional<TerminalSessionGeometry> initialGeometry = std::nullopt);
    bool selectionAvailable() const { return selectionAvailable_; }
    bool readOnly() const { return readOnly_; }
    void resizeTerminal(int columns, int rows, int cellWidthPixels,
                        int cellHeightPixels, int surfaceWidthPixels,
                        int surfaceHeightPixels);
    void applyRuntimeOptions(const TerminalSessionRuntimeOptions &options);
    // Update the GUI-thread base-title cache. The next worker title event,
    // including an empty value, replaces this action-originated value.
    void setSurfaceTitle(QString title);
    // Queue graceful teardown without blocking the UI. Workspace-wide closes
    // call this for every pane first so their grace periods run concurrently.
    void beginShutdown();
    void setMouseReportingEnabled(bool enabled);
    void setReadOnly(bool readOnly);
    void sendKey(const TerminalKeyInput &input);
    void sendInputMethod(const TerminalInputMethodInput &input);
    // Ghostty terminal-control actions are byte-oriented. Keeping their
    // payloads as QByteArray preserves embedded NUL and queues each complete
    // control sequence as one worker operation.
    void sendCsi(const QByteArray &payload);
    void sendEscape(const QByteArray &payload);
    void sendRawText(const QByteArray &serializedText);
    void resetTerminal();
    void sendMouse(const TerminalMouseInput &input);
    // Returns a non-zero correlation ID retained until the worker resolves
    // this request. Multiple presses remain independently in flight because
    // paste is non-idempotent; popup supersession belongs to TerminalPane.
    [[nodiscard]] quint64 requestRightClick(quint64 contentRevision, int column,
                                            int row, int modifiers,
                                            bool shiftBypassedMouseCapture);
    void setFocused(bool focused);
    void paste(const QString &text);
    void confirmPaste(quint64 requestId);
    void cancelPaste(quint64 requestId);
    void copySelection();
    // Correlated request IDs must be non-zero and unique among in-flight
    // actions. A false return means the request was not accepted and no
    // terminalActionReady signal will be published for that invocation.
    [[nodiscard]] bool copySelectionAction(quint64 requestId);
    [[nodiscard]] bool writeTerminalFile(quint64 requestId,
                                         const TerminalWriteFileAction &action);
    void clearSelection();
    // Fractional captured wheel input has no protocol event yet, but Ghostty
    // still clears selection. The worker rechecks current DEC tracking before
    // applying this queued side effect.
    void clearSelectionIfMouseTracking();
    void beginSelection(const TerminalSelectionPressInput &input);
    void updateSelection(const TerminalSelectionDragInput &input);
    void endSelection(int column, int row);
    void selectAll();
    [[nodiscard]] bool selectAllAction(quint64 requestId);
    void adjustSelection(TerminalSelectionAdjustment adjustment);
    [[nodiscard]] bool
    adjustSelectionAction(quint64 requestId,
                          TerminalSelectionAdjustment adjustment);
    void scrollViewport(const TerminalViewportRequest &request);
    [[nodiscard]] bool scrollToSelectionAction(quint64 requestId);
    // Search requests are generation-scoped so an incremental scan can yield
    // to PTY and UI work without publishing results from a superseded query.
    void search(const QString &text);
    void searchSerialized(const QByteArray &serializedText);
    void cancelSearch();
    void navigateSearch(TerminalSearchDirection direction);
    [[nodiscard]] bool searchSelectionAction(quint64 requestId);
    [[nodiscard]] bool searchExpected() const { return searchExpected_; }
    void requestHyperlink(int column, int row, quint64 contentRevision);
    void cancelHyperlinkRequest();
    [[nodiscard]] quint64 prepareHyperlinkActivation(int column, int row,
                                                     quint64 contentRevision);
    void commitHyperlinkActivation(quint64 requestId, int column, int row);
    void cancelHyperlinkActivation(quint64 requestId);

Q_SIGNALS:
    void terminalUpdated(const TerminalUpdate &update);
    void titleChanged(const QString &title);
    void currentDirectoryChanged(const QString &directory);
    void terminalMouseTrackingChanged(bool enabled);
    void mouseTrackingChanged(bool enabled);
    void runningChanged(bool running);
    void activeProcessChanged(bool active);
    void selectionAvailableChanged(bool available);
    void readOnlyChanged(bool readOnly);
    void launchProgramChanged();
    void sessionExited(int exitCode, int signalNumber, bool hold,
                       bool waitForKey);
    void waitAfterCommandDismissed();
    void errorOccurred(const QString &message);
    void bell();
    void hyperlinkResolved(quint64 contentRevision,
                           TerminalHyperlinkState state, TerminalLinkKind kind,
                           const QByteArray &uri, const QPoint &targetCell,
                           const QVector<QPoint> &matchingCells);
    void hyperlinkActivationResolved(quint64 contentRevision,
                                     TerminalLinkKind kind,
                                     const QByteArray &uri);
    void searchUpdated(const TerminalSearchUpdate &update);
    void unsafePasteConfirmationRequested(quint64 requestId,
                                          const QString &text);
    void terminalActionReady(const TerminalActionResult &result);
    void rightClickResolved(const TerminalRightClickResult &result);

    void resizeRequested(int columns, int rows, int cellWidthPixels,
                         int cellHeightPixels, int surfaceWidthPixels,
                         int surfaceHeightPixels);
    void keyRequested(const TerminalKeyInput &input);
    void sequenceKeyStagingRequested(quint64 token,
                                     const TerminalKeyInput &input);
    // std::optional is deliberately lowered at the queued Qt boundary. This
    // keeps the metatype contract value-only and usable from moc connections.
    void sequenceResolutionRequested(quint64 token,
                                     TerminalSequenceResolution resolution,
                                     bool hasCurrent,
                                     const TerminalKeyInput &current);
    void inputMethodRequested(const TerminalInputMethodInput &input);
    void csiRequested(const QByteArray &payload);
    void escapeRequested(const QByteArray &payload);
    void rawTextRequested(const QByteArray &serializedText);
    void resetTerminalRequested();
    void mouseRequested(const TerminalMouseInput &input);
    void rightClickRequested(const TerminalRightClickInput &input);
    void focusRequested(bool focused);
    void pasteRequested(const QString &text);
    void confirmPasteRequested(quint64 requestId);
    void cancelPasteRequested(quint64 requestId);
    void copyRequested();
    void copyActionRequested(quint64 requestId);
    void writeTerminalFileRequested(quint64 requestId,
                                    const TerminalWriteFileAction &action);
    void clearSelectionRequested();
    void clearSelectionIfMouseTrackingRequested();
    void beginSelectionRequested(const TerminalSelectionPressInput &input);
    void updateSelectionRequested(const TerminalSelectionDragInput &input);
    void endSelectionRequested(int column, int row);
    void selectAllRequested();
    void selectAllActionRequested(quint64 requestId);
    void selectionAdjustmentRequested(TerminalSelectionAdjustment adjustment);
    void
    selectionAdjustmentActionRequested(quint64 requestId,
                                       TerminalSelectionAdjustment adjustment);
    void scrollRequested(const TerminalViewportRequest &request);
    void scrollToSelectionActionRequested(quint64 requestId);
    void searchRequested(quint64 generation, const QByteArray &needle);
    void serializedSearchRequested(quint64 generation,
                                   const QByteArray &serializedNeedle);
    void searchCancellationRequested(quint64 generation);
    void searchNavigationRequested(quint64 generation,
                                   TerminalSearchDirection direction);
    void searchSelectionActionRequested(quint64 requestId);
    void hyperlinkQueryRequested(quint64 requestId, quint64 contentRevision,
                                 int column, int row);
    void hyperlinkQueryCancellationRequested(quint64 requestId);
    void hyperlinkActivationPreparationRequested(quint64 requestId,
                                                 quint64 contentRevision,
                                                 int column, int row);
    void hyperlinkActivationCommitRequested(quint64 requestId, int column,
                                            int row);
    void hyperlinkActivationCancellationRequested(quint64 requestId);
    void runtimeOptionsRequested(const TerminalSessionRuntimeOptions &options);
    void readOnlyRequested(bool readOnly);
    void shutdownRequested();

private:
    friend class TerminalPane;

    using WorkerRequest = std::function<void(SessionWorker &)>;
    enum class SessionStartState : quint8 {
        Idle,
        WaitingForLease,
        Started,
        Cancelled,
    };

    template <typename... SignalArgs, typename... WorkerArgs>
    void relayWorkerRequest(void (TerminalController::*signal)(SignalArgs...),
                            void (SessionWorker::*slot)(WorkerArgs...));
    void connectWorkerRequestRelays();
    void enqueueWorkerRequest(WorkerRequest request);
    void createWorkerRuntime();
    void connectWorkerResults(SessionWorker *worker);
    void tryStartSession();
    [[nodiscard]] bool applyInitialSessionPayload(
        const InitialSessionCoordinator::Payload &payload);
    void cancelInitialSessionRequest();
    [[nodiscard]] bool beginTerminalActionRequest(quint64 requestId);
    void failPendingTerminalActions();
    void notePotentialActivity();
    // Sequence leaders cross to SessionWorker immediately for mode-sensitive
    // VT encoding, but their bytes remain staged until their owning pane
    // resolves them. Keeping this protocol private prevents another caller
    // from superseding a pane's token without also updating its traversal.
    [[nodiscard]] quint64 beginSequence();
    // False means a synchronous staging observer resolved or superseded the
    // token (or destroyed this controller); the pane must not retain it.
    [[nodiscard]] bool stageSequenceKey(quint64 token,
                                        const TerminalKeyInput &input);
    void resolveSequence(
        quint64 token, TerminalSequenceResolution resolution,
        const std::optional<TerminalKeyInput> &current = std::nullopt);
    quint64 nextRightClickRequestId();
    quint64 nextHyperlinkRequestId();
    quint64 nextSearchGeneration();

    TerminalSessionLaunchOptions launchOptions_;
    std::shared_ptr<InitialSessionCoordinator> initialSessionCoordinator_;
    std::optional<InitialSessionCoordinator::Ticket> initialSessionTicket_;
    QThread *thread_ = nullptr;
    QPointer<SessionWorker> worker_;
    std::vector<WorkerRequest> pendingWorkerRequests_;
    QSet<quint64> pendingTerminalActionRequests_;
    std::optional<QString> baseTitle_;
    QString currentDirectory_;
    bool terminalMouseTracking_ = false;
    bool mouseReportingEnabled_ = true;
    bool running_ = false;
    bool activeProcess_ = false;
    bool explicitProgram_ = false;
    bool selectionAvailable_ = false;
    bool readOnly_ = false;
    bool closing_ = false;
    SessionStartState sessionStartState_ = SessionStartState::Idle;
    quint64 nextSequenceToken_ = 0;
    quint64 activeSequenceToken_ = 0;
    bool stagedSequencePotentialActivity_ = false;
    quint64 nextHyperlinkRequestId_ = 0;
    quint64 activeHyperlinkRequestId_ = 0;
    quint64 activeHyperlinkActivationId_ = 0;
    quint64 nextRightClickRequestId_ = 0;
    QSet<quint64> pendingRightClickRequestIds_;
    quint64 nextSearchGeneration_ = 0;
    quint64 activeSearchGeneration_ = 0;
    bool searchExpected_ = false;
};
