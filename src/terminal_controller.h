#pragma once

#include "terminal_session_options.h"
#include "terminal_types.h"

#include <QByteArray>
#include <QObject>
#include <QPoint>
#include <QString>
#include <QVector>

#include <optional>

class QThread;
class SessionWorker;

class TerminalController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(QString currentDirectory READ currentDirectory NOTIFY currentDirectoryChanged)
    Q_PROPERTY(bool mouseTracking READ mouseTracking NOTIFY mouseTrackingChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(bool activeProcess READ activeProcess NOTIFY activeProcessChanged)
    Q_PROPERTY(bool selectionAvailable READ selectionAvailable NOTIFY selectionAvailableChanged)

public:
    explicit TerminalController(const TerminalSessionLaunchOptions &options,
                                QObject *parent = nullptr);
    ~TerminalController() override;

    QString title() const { return title_; }
    QString currentDirectory() const { return currentDirectory_; }
    bool mouseTracking() const { return mouseTracking_; }
    bool running() const { return running_; }
    bool activeProcess() const { return activeProcess_; }
    bool selectionAvailable() const { return selectionAvailable_; }
    // True while an earlier queued select-all may establish a selection.
    // This lets a Ghostty action chain enqueue its dependent actions in order
    // without exposing speculative state through the Q_PROPERTY.
    bool selectionExpected() const
    {
        return selectionAvailable_ || pendingSelectAllRequests_ > 0;
    }
    void resizeTerminal(int columns, int rows, int cellWidthPixels,
                        int cellHeightPixels, int surfaceWidthPixels,
                        int surfaceHeightPixels);
    void applyRuntimeOptions(const TerminalSessionRuntimeOptions &options);
    // Queue graceful teardown without blocking the UI. Workspace-wide closes
    // call this for every pane first so their grace periods run concurrently.
    void beginShutdown();
    void sendKey(const TerminalKeyInput &input);
    // Sequence leaders cross to SessionWorker immediately for mode-sensitive
    // VT encoding, but their bytes remain staged until the UI resolves the
    // sequence. The first overload allocates a non-zero monotonic token; the
    // second appends another leader to that same sequence.
    [[nodiscard]] quint64 stageSequenceKey(const TerminalKeyInput &input);
    void stageSequenceKey(quint64 token, const TerminalKeyInput &input);
    void resolveSequence(
        quint64 token, TerminalSequenceResolution resolution,
        const std::optional<TerminalKeyInput> &current = std::nullopt);
    void sendInputMethod(const TerminalInputMethodInput &input);
    // Ghostty terminal-control actions are byte-oriented. Keeping their
    // payloads as QByteArray preserves embedded NUL and queues each complete
    // control sequence as one worker operation.
    void sendCsi(const QByteArray &payload);
    void sendEscape(const QByteArray &payload);
    void sendRawText(const QByteArray &serializedText);
    void resetTerminal();
    void sendMouse(const TerminalMouseInput &input);
    void setFocused(bool focused);
    void paste(const QString &text);
    void confirmPaste(quint64 requestId);
    void cancelPaste(quint64 requestId);
    void copySelection();
    void clearSelection();
    void beginSelection(int column, int row, int clickCount, bool rectangular);
    void updateSelection(int column, int row, bool rectangular);
    void endSelection(int column, int row);
    void selectAll();
    void adjustSelection(TerminalSelectionAdjustment adjustment);
    void scrollViewport(const TerminalViewportRequest &request);
    // Search requests are generation-scoped so an incremental scan can yield
    // to PTY and UI work without publishing results from a superseded query.
    void search(const QString &text);
    void searchSerialized(const QByteArray &serializedText);
    void cancelSearch();
    void navigateSearch(TerminalSearchDirection direction);
    void requestSearchSelection();
    [[nodiscard]] bool searchExpected() const { return searchExpected_; }
    void requestHyperlink(int column, int row, quint64 contentRevision);
    void cancelHyperlinkRequest();
    [[nodiscard]] quint64 prepareHyperlinkActivation(
        int column, int row, quint64 contentRevision);
    void commitHyperlinkActivation(quint64 requestId,
                                   int column, int row);
    void cancelHyperlinkActivation(quint64 requestId);

Q_SIGNALS:
    void terminalUpdated(const TerminalUpdate &update);
    void titleChanged(const QString &title);
    void currentDirectoryChanged(const QString &directory);
    void mouseTrackingChanged(bool enabled);
    void runningChanged(bool running);
    void activeProcessChanged(bool active);
    void selectionAvailableChanged(bool available);
    void sessionExited(int exitCode, int signalNumber, bool hold);
    void errorOccurred(const QString &message);
    void bell();
    void hyperlinkResolved(quint64 contentRevision,
                           TerminalHyperlinkState state,
                           TerminalLinkKind kind,
                           const QByteArray &uri, const QPoint &targetCell,
                           const QVector<QPoint> &matchingCells);
    void hyperlinkActivationResolved(quint64 contentRevision,
                                     TerminalLinkKind kind,
                                     const QByteArray &uri);
    void searchUpdated(const TerminalSearchUpdate &update);
    void searchSelectionReady(bool available, const QString &text);
    void unsafePasteConfirmationRequested(quint64 requestId,
                                          const QString &text);

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
    void focusRequested(bool focused);
    void pasteRequested(const QString &text);
    void confirmPasteRequested(quint64 requestId);
    void cancelPasteRequested(quint64 requestId);
    void copyRequested();
    void clearSelectionRequested();
    void beginSelectionRequested(int column, int row, int clickCount,
                                 bool rectangular);
    void updateSelectionRequested(int column, int row, bool rectangular);
    void endSelectionRequested(int column, int row);
    void selectAllRequested();
    void selectionAdjustmentRequested(TerminalSelectionAdjustment adjustment);
    void scrollRequested(const TerminalViewportRequest &request);
    void searchRequested(quint64 generation, const QByteArray &needle);
    void serializedSearchRequested(quint64 generation,
                                   const QByteArray &serializedNeedle);
    void searchCancellationRequested(quint64 generation);
    void searchNavigationRequested(quint64 generation,
                                   TerminalSearchDirection direction);
    void searchSelectionRequested(quint64 requestId);
    void hyperlinkQueryRequested(quint64 requestId, quint64 contentRevision,
                                 int column, int row);
    void hyperlinkQueryCancellationRequested(quint64 requestId);
    void hyperlinkActivationPreparationRequested(
        quint64 requestId, quint64 contentRevision, int column, int row);
    void hyperlinkActivationCommitRequested(
        quint64 requestId, int column, int row);
    void hyperlinkActivationCancellationRequested(quint64 requestId);
    void runtimeOptionsRequested(const TerminalSessionRuntimeOptions &options);
    void shutdownRequested();

private:
    void notePotentialActivity();
    quint64 nextHyperlinkRequestId();
    quint64 nextSearchGeneration();
    quint64 nextSearchSelectionRequestId();

    QThread *thread_ = nullptr;
    SessionWorker *worker_ = nullptr;
    QString title_;
    QString currentDirectory_;
    bool mouseTracking_ = false;
    bool running_ = true;
    bool activeProcess_ = false;
    bool explicitProgram_ = false;
    bool selectionAvailable_ = false;
    int pendingSelectAllRequests_ = 0;
    bool closing_ = false;
    quint64 nextSequenceToken_ = 0;
    quint64 activeSequenceToken_ = 0;
    bool stagedSequencePotentialActivity_ = false;
    quint64 nextHyperlinkRequestId_ = 0;
    quint64 activeHyperlinkRequestId_ = 0;
    quint64 activeHyperlinkActivationId_ = 0;
    quint64 nextSearchGeneration_ = 0;
    quint64 activeSearchGeneration_ = 0;
    quint64 nextSearchSelectionRequestId_ = 0;
    quint64 activeSearchSelectionRequestId_ = 0;
    bool searchExpected_ = false;
};
