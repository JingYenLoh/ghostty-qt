#pragma once

#include "launch_options.h"
#include "terminal_types.h"

#include <ghostty/vt.h>

#include <QByteArray>
#include <QObject>
#include <QString>

class QSocketNotifier;
class QTimer;

class SessionWorker final : public QObject {
    Q_OBJECT

public:
    explicit SessionWorker(QObject *parent = nullptr);
    ~SessionWorker() override;

public Q_SLOTS:
    void initialize(const LaunchOptions &options);
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
    void frameReady(const TerminalFrame &frame);
    void titleChanged(const QString &title);
    void currentDirectoryChanged(const QString &directory);
    void mouseTrackingChanged(bool enabled);
    void clipboardTextReady(const QString &text);
    void bell();
    void started(qint64 processId);
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
    void processDeferredEffects();
    void handleChildStatus(int status);
    QByteArray encodeKey(const TerminalKeyInput &input);
    QByteArray encodeMouse(const TerminalMouseInput &input);
    QString selectedText() const;
    bool pointToGridRef(int column, int row, GhosttyGridRef *out) const;
    void installSelection(const GhosttySelection &selection);

    static GhosttyKey mapQtKey(int key);
    static GhosttyMods mapQtModifiers(int modifiers);
    static GhosttyMouseButton mapQtMouseButton(int button);

    static void writePtyCallback(GhosttyTerminal terminal, void *userdata,
                                 const uint8_t *data, size_t length);
    static void bellCallback(GhosttyTerminal terminal, void *userdata);
    static void titleCallback(GhosttyTerminal terminal, void *userdata);
    static void pwdCallback(GhosttyTerminal terminal, void *userdata);
    static bool sizeCallback(GhosttyTerminal terminal, void *userdata,
                             GhosttySizeReportSize *size);
    static bool colorSchemeCallback(GhosttyTerminal terminal, void *userdata,
                                    GhosttyColorScheme *scheme);
    static bool deviceAttributesCallback(GhosttyTerminal terminal, void *userdata,
                                         GhosttyDeviceAttributes *attributes);
    static GhosttyClipboardWriteResult clipboardWriteCallback(
        GhosttyTerminal terminal, void *userdata,
        const GhosttyClipboardWrite *write);

    LaunchOptions options_;
    GhosttyTerminal terminal_ = nullptr;
    GhosttyRenderState renderState_ = nullptr;
    GhosttyRenderStateRowIterator rowIterator_ = nullptr;
    GhosttyRenderStateRowCells rowCells_ = nullptr;
    GhosttyKeyEncoder keyEncoder_ = nullptr;
    GhosttyKeyEvent keyEvent_ = nullptr;
    GhosttyMouseEncoder mouseEncoder_ = nullptr;
    GhosttyMouseEvent mouseEvent_ = nullptr;
    GhosttySelectionGesture selectionGesture_ = nullptr;
    GhosttySelectionGestureEvent selectionPressEvent_ = nullptr;
    GhosttySelectionGestureEvent selectionDragEvent_ = nullptr;
    GhosttySelectionGestureEvent selectionReleaseEvent_ = nullptr;

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
    bool shuttingDown_ = false;
    bool hold_ = false;
    bool mouseTracking_ = false;
    bool titleDirty_ = false;
    bool pwdDirty_ = false;
    bool bellPending_ = false;
    uint64_t compressionActivity_ = 0;
    uint32_t mouseModeFingerprint_ = 0;
    bool mouseEncoderConfigured_ = false;
};
