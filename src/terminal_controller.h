#pragma once

#include "launch_options.h"
#include "terminal_types.h"

#include <QObject>
#include <QString>

class QThread;
class SessionWorker;

class TerminalController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(QString currentDirectory READ currentDirectory NOTIFY currentDirectoryChanged)
    Q_PROPERTY(bool mouseTracking READ mouseTracking NOTIFY mouseTrackingChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)

public:
    explicit TerminalController(const LaunchOptions &options, QObject *parent = nullptr);
    ~TerminalController() override;

    QString title() const { return title_; }
    QString currentDirectory() const { return currentDirectory_; }
    bool mouseTracking() const { return mouseTracking_; }
    bool running() const { return running_; }
    bool hold() const { return options_.hold; }

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

    static bool isPasteSafe(const QString &text);

Q_SIGNALS:
    void frameReady(const TerminalFrame &frame);
    void titleChanged(const QString &title);
    void currentDirectoryChanged(const QString &directory);
    void mouseTrackingChanged(bool enabled);
    void runningChanged(bool running);
    void sessionExited(int exitCode, int signalNumber, bool hold);
    void errorOccurred(const QString &message);
    void bell();

    void resizeRequested(int columns, int rows, int cellWidthPixels,
                         int cellHeightPixels, int surfaceWidthPixels,
                         int surfaceHeightPixels);
    void keyRequested(const TerminalKeyInput &input);
    void textRequested(const QString &text);
    void mouseRequested(const TerminalMouseInput &input);
    void focusRequested(bool focused);
    void pasteRequested(const QString &text);
    void copyRequested();
    void clearSelectionRequested();
    void beginSelectionRequested(int column, int row, int clickCount,
                                 bool rectangular);
    void updateSelectionRequested(int column, int row, bool rectangular);
    void endSelectionRequested(int column, int row);
    void scrollRequested(int rows);

private:
    LaunchOptions options_;
    QThread *thread_ = nullptr;
    SessionWorker *worker_ = nullptr;
    QString title_;
    QString currentDirectory_;
    bool mouseTracking_ = false;
    bool running_ = true;
    bool closing_ = false;
};
