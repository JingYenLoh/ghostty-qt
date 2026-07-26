#pragma once

#include "terminal_action_result.h"
#include "terminal_session_options.h"
#include "terminal_types.h"
#include "terminal_write_file_action.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPoint>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>

class QSocketNotifier;
class QTimer;
class GhosttyLinkMatcher;
class GhosttyVtAdapter;

class SessionWorker final : public QObject {
    Q_OBJECT

public:
    explicit SessionWorker(QObject *parent = nullptr);
    ~SessionWorker() override;

    // Pure previews used by TerminalController to close the UI/worker race
    // around an immediately submitted command and close request. Execution
    // still decodes and writes only on the worker thread.
    static bool canonicalBytesMayStartProcess(const QByteArray &payload);
    static bool canonicalTextMayStartProcess(const QByteArray &payload);
    using InitializationObserver = std::move_only_function<void(bool)>;

    // True means libghostty-vt was created for this worker. A later child
    // launch failure is reported through the existing signals without
    // changing that result. The optional one-shot observer runs immediately
    // after terminal creation succeeds or fails, before child launch begins.
    bool initialize(
        const TerminalSessionLaunchOptions &options,
        InitializationObserver observer = {});

public Q_SLOTS:
    // Apply the worker-owned live state: terminal appearance and regex URL
    // matching. Font, keybindings, previews, and future-pane scrollback remain
    // owned by TerminalPane.
    void applyRuntimeOptions(const TerminalSessionRuntimeOptions &options);
    // Ordered with input on the worker event queue so bytes submitted before
    // and after a toggle cannot cross the policy boundary.
    void setReadOnly(bool readOnly);
    void resizeTerminal(int columns, int rows, int cellWidthPixels,
                        int cellHeightPixels, int surfaceWidthPixels,
                        int surfaceHeightPixels);
    void sendKey(const TerminalKeyInput &input);
    void stageSequenceKey(quint64 token, const TerminalKeyInput &input);
    void resolveSequence(quint64 token,
                         TerminalSequenceResolution resolution,
                         bool hasCurrent,
                         const TerminalKeyInput &current);
    void sendInputMethod(const TerminalInputMethodInput &input);
    void sendCsi(const QByteArray &payload);
    void sendEscape(const QByteArray &payload);
    // Decode Ghostty's Zig string-literal payload on the worker thread before
    // writing the resulting bytes directly to the PTY.
    void sendRawText(const QByteArray &serializedText);
    void resetTerminal();
    void sendMouse(const TerminalMouseInput &input);
    void setFocused(bool focused);
    void paste(const QString &text);
    void confirmPaste(quint64 requestId);
    void cancelPaste(quint64 requestId);
    void copySelection();
    void copySelectionAction(quint64 requestId);
    void writeTerminalFile(
        quint64 requestId, const TerminalWriteFileAction &action);
    void clearSelection();
    void clearSelectionIfMouseTracking();
    void beginSelection(const TerminalSelectionPressInput &input);
    void updateSelection(const TerminalSelectionDragInput &input);
    void endSelection(int column, int row);
    void selectAll();
    void selectAllAction(quint64 requestId);
    void adjustSelection(TerminalSelectionAdjustment adjustment);
    void adjustSelectionAction(
        quint64 requestId, TerminalSelectionAdjustment adjustment);
    void scrollViewport(const TerminalViewportRequest &request);
    void scrollToSelectionAction(quint64 requestId);
    void search(quint64 generation, const QByteArray &needle);
    void searchSerialized(quint64 generation,
                          const QByteArray &serializedNeedle);
    void cancelSearch(quint64 generation);
    void navigateSearch(quint64 generation,
                        TerminalSearchDirection direction);
    void searchSelectionAction(quint64 requestId);
    void queryHyperlink(quint64 requestId, quint64 contentRevision,
                        int column, int row);
    void cancelHyperlinkQuery(quint64 requestId);
    void prepareHyperlinkActivation(quint64 requestId,
                                    quint64 contentRevision,
                                    int column, int row);
    void commitHyperlinkActivation(quint64 requestId,
                                   int column, int row);
    void cancelHyperlinkActivation(quint64 requestId);
    void shutdown();

Q_SIGNALS:
    void terminalUpdated(const TerminalUpdate &update);
    void titleChanged(const QString &title);
    void currentDirectoryChanged(const QString &directory);
    void mouseTrackingChanged(bool enabled);
    void clipboardTextReady(const QString &text,
                            TerminalClipboardDestination destination);
    void terminalActionFinished(const TerminalActionResult &result);
    void unsafePasteConfirmationRequested(quint64 requestId,
                                          const QString &text);
    void bell();
    void started(qint64 processId);
    // True means closing this surface would interrupt active work. An idle
    // interactive shell remains running but reports false here.
    void activeProcessChanged(bool active);
    void selectionAvailableChanged(bool available);
    // Emitted for every select-all request, including a blank terminal where
    // selection availability remains false. This reconciles the UI's queued
    // selection intent without relying on a state-change-only signal.
    void selectAllCompleted(bool selectionAvailable);
    void hyperlinkResolved(quint64 requestId, quint64 contentRevision,
                           TerminalHyperlinkState state,
                           TerminalLinkKind kind,
                           const QByteArray &uri, const QPoint &targetCell,
                           const QVector<QPoint> &matchingCells);
    void hyperlinkActivationResolved(quint64 requestId,
                                     quint64 contentRevision,
                                     TerminalLinkKind kind,
                                     const QByteArray &uri);
    void searchUpdated(const TerminalSearchUpdate &update);
    void sessionExited(int exitCode, int signalNumber, bool hold);
    void errorOccurred(const QString &message);

private Q_SLOTS:
    void readFromPty();
    void flushPtyWrites();
    void checkChild();
    void publishFrame();
    void compressScrollback();
    void processPendingHyperlinkQuery();
    void processSearchChunk();

private:
    bool createTerminal();
    bool spawnChild();
    void destroyTerminal();
    void closePty();
    void queuePtyWrite(const QByteArray &data);
    void queueInputWrite(const QByteArray &data);
    void sendRawAction(const QByteArray &data);
    void scheduleFrame();
    void noteCompressionActivity();
    void syncMouseEncoder();
    void syncSelectionAvailability();
    void markTerminalContentChanged();
    void markSearchContentChanged();
    void beginSearch(quint64 generation, const QByteArray &needle);
    void restartSearch();
    void scheduleSearchChunk();
    void publishSearchUpdate();
    void rebuildSearchVisibleCells();
    void refreshTrackedHyperlink(bool force = false);
    void processDeferredEffects();
    void drainPty(bool finalDrain);
    void notePotentialActivity();
    void updateProcessActivity();
    void setActiveProcess(bool active);
    void handleChildStatus(int status);
    QByteArray encodeMouse(const TerminalMouseInput &input);
    void commitPaste(const QString &text, const QByteArray &encoded);
    void scrollToBottomForInput();
    void clearSelectionState();
    void clearSelectionAndResetGestureState();
    void clearSelectionAfterKey(bool modifier, bool escape);
    void copySelectionTo(TerminalClipboardDestination destination,
                         bool clearAfterCopy);
    void copySelectionOnSelect();

    TerminalSessionLaunchOptions options_;
    std::unique_ptr<GhosttyVtAdapter> vt_;
    std::unique_ptr<GhosttyLinkMatcher> linkMatcher_;
    struct HyperlinkState;
    std::unique_ptr<HyperlinkState> hyperlinkState_;
    struct SearchState;
    std::unique_ptr<SearchState> searchState_;

    int masterFd_ = -1;
    qint64 childPid_ = -1;
    QSocketNotifier *readNotifier_ = nullptr;
    QSocketNotifier *writeNotifier_ = nullptr;
    QTimer *childTimer_ = nullptr;
    QTimer *frameTimer_ = nullptr;
    QTimer *compressionTimer_ = nullptr;
    QByteArray pendingWrites_;
    QHash<quint64, QString> pendingPastes_;
    quint64 nextPasteRequestId_ = 0;
    QByteArray stagedSequenceBytes_;
    quint64 newestSequenceToken_ = 0;
    quint64 activeSequenceToken_ = 0;
    bool stagedSequencePotentialActivity_ = false;

    int columns_ = 80;
    int rows_ = 24;
    int cellWidthPixels_ = 8;
    int cellHeightPixels_ = 16;
    int surfaceWidthPixels_ = 640;
    int surfaceHeightPixels_ = 384;
    bool running_ = false;
    bool interactiveShell_ = false;
    bool activeProcess_ = false;
    bool selectionAvailable_ = false;
    bool readOnly_ = false;
    QElapsedTimer potentialActivityTimer_;
    QElapsedTimer cursorBlinkResetTimer_;
    bool cursorBlinkResetPending_ = false;
    bool shuttingDown_ = false;
    bool mouseTracking_ = false;
    quint64 terminalContentRevision_ = 1;
    quint64 searchContentRevision_ = 1;
    quint64 publishedContentRevision_ = 0;
    uint64_t compressionActivity_ = 0;
};
