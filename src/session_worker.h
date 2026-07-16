#pragma once

#include "launch_options.h"
#include "terminal_types.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include <memory>

class QSocketNotifier;
class QTimer;
class GhosttyVtAdapter;

class SessionWorker final : public QObject {
    Q_OBJECT

public:
    explicit SessionWorker(QObject *parent = nullptr);
    ~SessionWorker() override;

public Q_SLOTS:
    void initialize(const LaunchOptions &options);
    // Font rendering is owned by TerminalPane. This applies live terminal
    // colors; scrollback remains fixed for this libghostty terminal and the
    // reloaded limit is used only when a new pane is constructed.
    void applyRuntimeOptions(const LaunchOptions &options);
    void resizeTerminal(int columns, int rows, int cellWidthPixels,
                        int cellHeightPixels, int surfaceWidthPixels,
                        int surfaceHeightPixels);
    void sendKey(const TerminalKeyInput &input);
    void sendText(const QString &text);
    void sendMouse(const TerminalMouseInput &input);
    void setFocused(bool focused);
    void paste(const QString &text);
    void copySelection();
    void clearSelection();
    void beginSelection(int column, int row, int clickCount, bool rectangular);
    void updateSelection(int column, int row, bool rectangular);
    void endSelection(int column, int row);
    void scrollViewport(int rows);
    void shutdown();

Q_SIGNALS:
    void terminalUpdated(const TerminalUpdate &update);
    void titleChanged(const QString &title);
    void currentDirectoryChanged(const QString &directory);
    void mouseTrackingChanged(bool enabled);
    void clipboardTextReady(const QString &text);
    void bell();
    void started(qint64 processId);
    // True means closing this surface would interrupt active work. An idle
    // interactive shell remains running but reports false here.
    void activeProcessChanged(bool active);
    void selectionAvailableChanged(bool available);
    void sessionExited(int exitCode, int signalNumber, bool hold);
    void errorOccurred(const QString &message);

private Q_SLOTS:
    void readFromPty();
    void flushPtyWrites();
    void checkChild();
    void publishFrame();
    void compressScrollback();

private:
    bool createTerminal();
    bool spawnChild();
    void destroyTerminal();
    void closePty();
    void queuePtyWrite(const QByteArray &data);
    void scheduleFrame();
    void noteCompressionActivity();
    void syncMouseEncoder();
    void syncSelectionAvailability();
    void processDeferredEffects();
    void drainPty(bool finalDrain);
    void notePotentialActivity();
    void updateProcessActivity();
    void setActiveProcess(bool active);
    void handleChildStatus(int status);
    QByteArray encodeKey(const TerminalKeyInput &input);
    QByteArray encodeMouse(const TerminalMouseInput &input);
    QString selectedText() const;

    LaunchOptions options_;
    std::unique_ptr<GhosttyVtAdapter> vt_;

    int masterFd_ = -1;
    qint64 childPid_ = -1;
    QSocketNotifier *readNotifier_ = nullptr;
    QSocketNotifier *writeNotifier_ = nullptr;
    QTimer *childTimer_ = nullptr;
    QTimer *frameTimer_ = nullptr;
    QTimer *compressionTimer_ = nullptr;
    QByteArray pendingWrites_;

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
    QElapsedTimer potentialActivityTimer_;
    QElapsedTimer cursorBlinkResetTimer_;
    bool cursorBlinkResetPending_ = false;
    bool shuttingDown_ = false;
    bool hold_ = false;
    bool mouseTracking_ = false;
    uint64_t compressionActivity_ = 0;
};
