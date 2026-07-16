#include "session_worker.h"
#include "ghostty_vt_adapter.h"
#include "terminfo_paths.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <limits>

#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr qsizetype kReadBufferSize = 64 * 1024;
constexpr qsizetype kMaximumReadPerActivation = 1024 * 1024;
constexpr qsizetype kMaximumFinalRead = 8 * 1024 * 1024;
constexpr int kFrameCoalesceMilliseconds = 8;
constexpr int kCompressionIdleMilliseconds = 500;
constexpr int kShutdownGraceMilliseconds = 2000;
constexpr int kPotentialActivityGraceMilliseconds = 250;

uint16_t boundedU16(int value)
{
    return static_cast<uint16_t>(std::clamp(value, 1, static_cast<int>(UINT16_MAX)));
}

} // namespace

SessionWorker::SessionWorker(QObject *parent)
    : QObject(parent)
{
}

SessionWorker::~SessionWorker()
{
    shutdown();
}

void SessionWorker::initialize(const LaunchOptions &options)
{
    if (vt_ != nullptr || running_) {
        return;
    }

    options_ = options;
    hold_ = options.hold;
    shuttingDown_ = false;
    potentialActivityTimer_.invalidate();

    frameTimer_ = new QTimer(this);
    frameTimer_->setSingleShot(true);
    frameTimer_->setInterval(kFrameCoalesceMilliseconds);
    connect(frameTimer_, &QTimer::timeout, this, &SessionWorker::publishFrame);

    childTimer_ = new QTimer(this);
    childTimer_->setInterval(100);
    connect(childTimer_, &QTimer::timeout, this, &SessionWorker::checkChild);

    compressionTimer_ = new QTimer(this);
    compressionTimer_->setSingleShot(true);
    compressionTimer_->setInterval(kCompressionIdleMilliseconds);
    connect(compressionTimer_, &QTimer::timeout,
            this, &SessionWorker::compressScrollback);

    if (!createTerminal()) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to initialize libghostty-vt."));
        Q_EMIT sessionExited(127, 0, hold_);
        return;
    }

    publishFrame();
    if (!spawnChild()) {
        Q_EMIT sessionExited(127, 0, hold_);
    }
}

bool SessionWorker::createTerminal()
{
    const GhosttyVtAdapter::Options options{
        .geometry = {
            .columns = columns_,
            .rows = rows_,
            .cellWidthPixels = cellWidthPixels_,
            .cellHeightPixels = cellHeightPixels_,
            .surfaceWidthPixels = surfaceWidthPixels_,
            .surfaceHeightPixels = surfaceHeightPixels_,
        },
        .scrollbackBytes = scrollbackLimitInBytes(
            options_.scrollbackLimit, columns_),
        .foregroundColor = options_.foregroundColor,
        .backgroundColor = options_.backgroundColor,
        .cursorColor = options_.cursorColor,
    };
    GhosttyVtAdapter::Callbacks callbacks;
    callbacks.writePty = [this](const QByteArray &data) { queuePtyWrite(data); };
    vt_ = GhosttyVtAdapter::create(options, std::move(callbacks));
    if (vt_ != nullptr) {
        compressionActivity_ = vt_->compressionActivity();
    }
    return vt_ != nullptr;
}

void SessionWorker::applyRuntimeOptions(const LaunchOptions &options)
{
    const bool colorsChanged = options_.foregroundColor != options.foregroundColor
        || options_.backgroundColor != options.backgroundColor
        || options_.cursorColor != options.cursorColor;
    options_ = options;
    hold_ = options.hold;

    // libghostty-vt has no API for resizing an existing scrollback allocation.
    // options_.scrollbackLimit therefore records the desired value for
    // consistency only; TerminalWorkspace applies it when creating new panes.
    if (vt_ != nullptr && colorsChanged) {
        if (!vt_->setColors(options.foregroundColor,
                            options.backgroundColor,
                            options.cursorColor)) {
            Q_EMIT errorOccurred(
                QStringLiteral("Failed to apply terminal colors to libghostty-vt."));
            return;
        }
        scheduleFrame();
    }
}

bool SessionWorker::spawnChild()
{
    QString executable;
    QStringList arguments = options_.program;
    interactiveShell_ = arguments.isEmpty();

    if (interactiveShell_) {
        executable = qEnvironmentVariable("SHELL");
        if (executable.isEmpty() || !QFileInfo(executable).isExecutable()) {
            executable = QStringLiteral("/bin/sh");
        }
        // Force interactive mode. Relying only on isatty can leave shells
        // without job control during startup races, which in turn prevents
        // the PTY foreground group from identifying active jobs.
        arguments = {executable, QStringLiteral("-i")};
    } else {
        executable = arguments.constFirst();
        if (executable.contains(QLatin1Char('/'))) {
            QFileInfo executableInfo(executable);
            if (executableInfo.isRelative()) {
                executable = QDir(options_.workingDirectory).absoluteFilePath(executable);
            }
        } else {
            executable = QStandardPaths::findExecutable(executable);
        }
    }

    if (executable.isEmpty() || !QFileInfo(executable).isExecutable()) {
        Q_EMIT errorOccurred(QStringLiteral("Program is not executable: %1")
                               .arg(options_.program.value(0, executable)));
        return false;
    }

    arguments[0] = executable;
    QVector<QByteArray> argumentStorage;
    argumentStorage.reserve(arguments.size());
    for (const QString &argument : std::as_const(arguments)) {
        argumentStorage.push_back(argument.toLocal8Bit());
    }
    QVector<char *> argv;
    argv.reserve(argumentStorage.size() + 1);
    for (QByteArray &argument : argumentStorage) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const TerminfoResolution terminfo = resolveRuntimeTerminfoDirectory();
    if (!terminfo) {
        Q_EMIT errorOccurred(terminfo.error);
        return false;
    }
    environment.insert(QStringLiteral("TERM"), QStringLiteral("xterm-ghostty"));
    environment.insert(QStringLiteral("TERMINFO"), terminfo.directory);
    environment.insert(QStringLiteral("COLORTERM"), QStringLiteral("truecolor"));
    environment.insert(QStringLiteral("TERM_PROGRAM"), QStringLiteral("ghostty-qt"));
    environment.insert(QStringLiteral("TERM_PROGRAM_VERSION"),
                       QStringLiteral(GHOSTTY_QT_VERSION));

    const QStringList environmentList = environment.toStringList();
    QVector<QByteArray> environmentStorage;
    environmentStorage.reserve(environmentList.size());
    for (const QString &entry : environmentList) {
        environmentStorage.push_back(entry.toLocal8Bit());
    }
    QVector<char *> envp;
    envp.reserve(environmentStorage.size() + 1);
    for (QByteArray &entry : environmentStorage) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);

    const QByteArray executableBytes = QFile::encodeName(executable);
    const QByteArray workingDirectoryBytes = QFile::encodeName(options_.workingDirectory);
    struct winsize size {};
    size.ws_col = boundedU16(columns_);
    size.ws_row = boundedU16(rows_);
    size.ws_xpixel = boundedU16(surfaceWidthPixels_);
    size.ws_ypixel = boundedU16(surfaceHeightPixels_);

    int ptyFd = -1;
    const pid_t pid = ::forkpty(&ptyFd, nullptr, nullptr, &size);
    if (pid < 0) {
        Q_EMIT errorOccurred(QStringLiteral("Unable to create PTY: %1")
                               .arg(QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }

    if (pid == 0) {
        if (::chdir(workingDirectoryBytes.constData()) != 0) {
            _exit(126);
        }
        ::execve(executableBytes.constData(), argv.data(), envp.data());
        _exit(errno == ENOENT ? 127 : 126);
    }

    masterFd_ = ptyFd;
    childPid_ = static_cast<qint64>(pid);
    running_ = true;
    setActiveProcess(!interactiveShell_);
    if (interactiveShell_) {
        // Until the shell reaches its first prompt, err on the side of
        // protecting startup scripts from an immediate close.
        notePotentialActivity();
    }

    const int currentFlags = ::fcntl(masterFd_, F_GETFL, 0);
    if (currentFlags >= 0) {
        ::fcntl(masterFd_, F_SETFL, currentFlags | O_NONBLOCK);
    }
    const int descriptorFlags = ::fcntl(masterFd_, F_GETFD, 0);
    if (descriptorFlags >= 0) {
        ::fcntl(masterFd_, F_SETFD, descriptorFlags | FD_CLOEXEC);
    }

    readNotifier_ = new QSocketNotifier(masterFd_, QSocketNotifier::Read, this);
    connect(readNotifier_, &QSocketNotifier::activated, this, &SessionWorker::readFromPty);
    writeNotifier_ = new QSocketNotifier(masterFd_, QSocketNotifier::Write, this);
    writeNotifier_->setEnabled(false);
    connect(writeNotifier_, &QSocketNotifier::activated, this, &SessionWorker::flushPtyWrites);

    childTimer_->start();
    Q_EMIT currentDirectoryChanged(options_.workingDirectory);
    Q_EMIT started(childPid_);
    return true;
}

void SessionWorker::resizeTerminal(int columns, int rows, int cellWidthPixels,
                                   int cellHeightPixels, int surfaceWidthPixels,
                                   int surfaceHeightPixels)
{
    const int nextColumns = std::clamp(columns, 1, static_cast<int>(UINT16_MAX));
    const int nextRows = std::clamp(rows, 1, static_cast<int>(UINT16_MAX));
    const int nextCellWidth = std::max(cellWidthPixels, 1);
    const int nextCellHeight = std::max(cellHeightPixels, 1);
    const int nextSurfaceWidth = std::max(surfaceWidthPixels, 1);
    const int nextSurfaceHeight = std::max(surfaceHeightPixels, 1);

    if (vt_ != nullptr) {
        const GhosttyVtAdapter::Geometry geometry{
            .columns = nextColumns,
            .rows = nextRows,
            .cellWidthPixels = nextCellWidth,
            .cellHeightPixels = nextCellHeight,
            .surfaceWidthPixels = nextSurfaceWidth,
            .surfaceHeightPixels = nextSurfaceHeight,
        };
        if (!vt_->resize(geometry)) {
            Q_EMIT errorOccurred(QStringLiteral("libghostty rejected the terminal resize."));
            return;
        }
        processDeferredEffects();
        noteCompressionActivity();
        scheduleFrame();
    }

    columns_ = nextColumns;
    rows_ = nextRows;
    cellWidthPixels_ = nextCellWidth;
    cellHeightPixels_ = nextCellHeight;
    surfaceWidthPixels_ = nextSurfaceWidth;
    surfaceHeightPixels_ = nextSurfaceHeight;

    if (masterFd_ >= 0) {
        struct winsize size {};
        size.ws_col = boundedU16(columns_);
        size.ws_row = boundedU16(rows_);
        size.ws_xpixel = boundedU16(surfaceWidthPixels_);
        size.ws_ypixel = boundedU16(surfaceHeightPixels_);
        ::ioctl(masterFd_, TIOCSWINSZ, &size);
    }
}

void SessionWorker::readFromPty()
{
    drainPty(false);
}

void SessionWorker::drainPty(bool finalDrain)
{
    if (masterFd_ < 0 || vt_ == nullptr) {
        return;
    }

    std::array<uint8_t, static_cast<size_t>(kReadBufferSize)> buffer{};
    qsizetype totalRead = 0;
    bool receivedData = false;

    const qsizetype readLimit = finalDrain
        ? kMaximumFinalRead
        : kMaximumReadPerActivation;
    while (totalRead < readLimit) {
        const ssize_t count = ::read(masterFd_, buffer.data(), buffer.size());
        if (count > 0) {
            const auto size = static_cast<size_t>(count);
            vt_->writeVt(QByteArrayView(
                reinterpret_cast<const char *>(buffer.data()),
                static_cast<qsizetype>(size)));
            totalRead += static_cast<qsizetype>(count);
            receivedData = true;
            continue;
        }
        if (count == 0) {
            if (readNotifier_ != nullptr) {
                readNotifier_->setEnabled(false);
            }
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EIO) {
            if (readNotifier_ != nullptr) {
                readNotifier_->setEnabled(false);
            }
            break;
        }
        Q_EMIT errorOccurred(QStringLiteral("PTY read failed: %1")
                               .arg(QString::fromLocal8Bit(std::strerror(errno))));
        break;
    }

    if (receivedData) {
        // VT input may change mouse tracking or encoding modes. Synchronize
        // once per output batch without resetting motion deduplication for
        // every individual pointer event.
        syncMouseEncoder();
        syncSelectionAvailability();
        processDeferredEffects();
        noteCompressionActivity();
        scheduleFrame();
    }
    if (!finalDrain && totalRead >= kMaximumReadPerActivation) {
        QTimer::singleShot(0, this, &SessionWorker::readFromPty);
    }
}

void SessionWorker::queuePtyWrite(const QByteArray &data)
{
    if (data.isEmpty() || masterFd_ < 0) {
        return;
    }
    pendingWrites_.append(data);
    flushPtyWrites();
}

void SessionWorker::flushPtyWrites()
{
    while (masterFd_ >= 0 && !pendingWrites_.isEmpty()) {
        const ssize_t count = ::write(masterFd_, pendingWrites_.constData(),
                                      static_cast<size_t>(pendingWrites_.size()));
        if (count > 0) {
            pendingWrites_.remove(0, static_cast<qsizetype>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (count < 0) {
            Q_EMIT errorOccurred(QStringLiteral("PTY write failed: %1")
                                   .arg(QString::fromLocal8Bit(std::strerror(errno))));
        }
        pendingWrites_.clear();
        break;
    }

    if (writeNotifier_ != nullptr) {
        writeNotifier_->setEnabled(!pendingWrites_.isEmpty());
    }
}

void SessionWorker::sendKey(const TerminalKeyInput &input)
{
    if (input.pressed
        && (input.key == Qt::Key_Return || input.key == Qt::Key_Enter
            || input.text.contains(u'\n') || input.text.contains(u'\r'))) {
        notePotentialActivity();
    }
    queuePtyWrite(encodeKey(input));
}

void SessionWorker::sendText(const QString &text)
{
    if (text.isEmpty()) {
        return;
    }
    if (text.contains(u'\n') || text.contains(u'\r')) {
        notePotentialActivity();
    }
    TerminalKeyInput input;
    input.key = Qt::Key_unknown;
    input.text = text;
    input.pressed = true;
    queuePtyWrite(encodeKey(input));
}

QByteArray SessionWorker::encodeKey(const TerminalKeyInput &input)
{
    return vt_ != nullptr ? vt_->encodeKey(input) : QByteArray{};
}

void SessionWorker::sendMouse(const TerminalMouseInput &input)
{
    queuePtyWrite(encodeMouse(input));
}

QByteArray SessionWorker::encodeMouse(const TerminalMouseInput &input)
{
    return vt_ != nullptr ? vt_->encodeMouse(input) : QByteArray{};
}

void SessionWorker::setFocused(bool focused)
{
    if (vt_ != nullptr) {
        queuePtyWrite(vt_->encodeFocus(focused));
    }
}

void SessionWorker::paste(const QString &text)
{
    if (text.contains(u'\n') || text.contains(u'\r')) {
        notePotentialActivity();
    }
    if (vt_ != nullptr) {
        queuePtyWrite(vt_->encodePaste(text));
    }
}

void SessionWorker::copySelection()
{
    const QString text = selectedText();
    if (!text.isNull()) {
        Q_EMIT clipboardTextReady(text);
    }
}

QString SessionWorker::selectedText() const
{
    return vt_ != nullptr ? vt_->selectedText() : QString{};
}

void SessionWorker::clearSelection()
{
    if (vt_ != nullptr) {
        vt_->clearSelection();
        syncSelectionAvailability();
        scheduleFrame();
    }
}

void SessionWorker::beginSelection(int column, int row, int clickCount, bool rectangular)
{
    if (vt_ != nullptr
        && vt_->beginSelection(column, row, clickCount, rectangular)) {
        syncSelectionAvailability();
        scheduleFrame();
    }
}

void SessionWorker::updateSelection(int column, int row, bool rectangular)
{
    if (vt_ != nullptr && vt_->updateSelection(column, row, rectangular)) {
        syncSelectionAvailability();
        scheduleFrame();
    }
}

void SessionWorker::endSelection(int column, int row)
{
    if (vt_ != nullptr) {
        vt_->endSelection(column, row);
        syncSelectionAvailability();
    }
}

void SessionWorker::syncSelectionAvailability()
{
    const bool available = vt_ != nullptr && vt_->hasSelection();
    if (selectionAvailable_ == available) {
        return;
    }
    selectionAvailable_ = available;
    Q_EMIT selectionAvailableChanged(selectionAvailable_);
}

void SessionWorker::scrollViewport(int rows)
{
    if (vt_ == nullptr || rows == 0) {
        return;
    }
    vt_->scrollViewport(rows);
    scheduleFrame();
}

void SessionWorker::scheduleFrame()
{
    if (frameTimer_ != nullptr && !frameTimer_->isActive()) {
        frameTimer_->start();
    }
}

void SessionWorker::noteCompressionActivity()
{
    if (vt_ == nullptr || compressionTimer_ == nullptr) {
        return;
    }
    const uint64_t activity = vt_->compressionActivity();
    if (activity != compressionActivity_) {
        compressionActivity_ = activity;
        compressionTimer_->start();
    }
}

void SessionWorker::syncMouseEncoder()
{
    if (vt_ != nullptr) {
        vt_->synchronizeInputModes();
    }
}

void SessionWorker::compressScrollback()
{
    if (vt_ == nullptr || compressionTimer_ == nullptr) {
        return;
    }
    if (vt_->compressScrollback()) {
        compressionTimer_->start(0);
    }
}

void SessionWorker::publishFrame()
{
    if (vt_ == nullptr) {
        return;
    }
    GhosttyVtAdapter::RenderSnapshot snapshot;
    const GhosttyVtAdapter::RenderResult result = vt_->renderFrame(&snapshot);
    if (result == GhosttyVtAdapter::RenderResult::Retry) {
        scheduleFrame();
    }
    if (result != GhosttyVtAdapter::RenderResult::Ready) {
        return;
    }
    if (snapshot.mouseTracking != mouseTracking_) {
        mouseTracking_ = snapshot.mouseTracking;
        Q_EMIT mouseTrackingChanged(mouseTracking_);
    }
    if (snapshot.update.hasChanges()) {
        Q_EMIT terminalUpdated(snapshot.update);
    }
}

void SessionWorker::processDeferredEffects()
{
    if (vt_ == nullptr) {
        return;
    }
    const GhosttyVtAdapter::DeferredEffects effects = vt_->takeDeferredEffects();
    if (!effects.title.isNull()) {
        Q_EMIT titleChanged(effects.title);
    }
    if (!effects.currentDirectory.isNull()) {
        Q_EMIT currentDirectoryChanged(effects.currentDirectory);
    }
    if (effects.bell) {
        Q_EMIT bell();
    }
}

void SessionWorker::checkChild()
{
    if (childPid_ <= 0) {
        return;
    }
    int status = 0;
    const pid_t result = ::waitpid(static_cast<pid_t>(childPid_), &status, WNOHANG);
    if (result == static_cast<pid_t>(childPid_)) {
        handleChildStatus(status);
    } else if (result == 0) {
        updateProcessActivity();
    } else if (result < 0 && errno == ECHILD) {
        drainPty(true);
        running_ = false;
        childPid_ = -1;
        potentialActivityTimer_.invalidate();
        setActiveProcess(false);
        if (childTimer_ != nullptr) {
            childTimer_->stop();
        }
        closePty();
        if (!shuttingDown_) {
            Q_EMIT errorOccurred(QStringLiteral("The child process was reaped unexpectedly."));
            Q_EMIT sessionExited(127, 0, hold_);
        }
    } else if (result < 0 && errno != EINTR) {
        Q_EMIT errorOccurred(QStringLiteral("waitpid failed: %1")
                               .arg(QString::fromLocal8Bit(std::strerror(errno))));
    }
}

void SessionWorker::updateProcessActivity()
{
    if (!running_ || childPid_ <= 0) {
        setActiveProcess(false);
        return;
    }
    if (!interactiveShell_) {
        setActiveProcess(true);
        return;
    }

    if (potentialActivityTimer_.isValid()
        && potentialActivityTimer_.elapsed()
               < kPotentialActivityGraceMilliseconds) {
        setActiveProcess(true);
        return;
    }
    potentialActivityTimer_.invalidate();

    // forkpty makes the shell the session and process-group leader. While the
    // shell is reading at its prompt it owns the PTY foreground group; a job
    // launched by the shell temporarily replaces it with another group.
    // Treat query failures conservatively so an unusual PTY state cannot make
    // us silently discard active work.
    const pid_t foregroundGroup = masterFd_ >= 0 ? ::tcgetpgrp(masterFd_) : -1;
    setActiveProcess(foregroundGroup < 0
                     || foregroundGroup != static_cast<pid_t>(childPid_));
}

void SessionWorker::notePotentialActivity()
{
    if (!running_ || !interactiveShell_) {
        return;
    }
    potentialActivityTimer_.restart();
    setActiveProcess(true);
}

void SessionWorker::setActiveProcess(bool active)
{
    if (activeProcess_ == active) {
        return;
    }
    activeProcess_ = active;
    Q_EMIT activeProcessChanged(activeProcess_);
}

void SessionWorker::handleChildStatus(int status)
{
    // A process may exit before Qt delivers the final readability event. Drain
    // the PTY and publish synchronously so short-lived commands cannot lose
    // their last screen update when the master descriptor is closed below.
    drainPty(true);
    if (frameTimer_ != nullptr && frameTimer_->isActive()) {
        frameTimer_->stop();
    }
    publishFrame();

    running_ = false;
    childPid_ = -1;
    potentialActivityTimer_.invalidate();
    setActiveProcess(false);
    if (childTimer_ != nullptr) {
        childTimer_->stop();
    }
    if (compressionTimer_ != nullptr) {
        compressionTimer_->stop();
    }
    closePty();

    int exitCode = 0;
    int signalNumber = 0;
    if (WIFEXITED(status)) {
        exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        signalNumber = WTERMSIG(status);
        exitCode = 128 + signalNumber;
    }
    if (!shuttingDown_) {
        Q_EMIT sessionExited(exitCode, signalNumber, hold_);
    }
}

void SessionWorker::closePty()
{
    if (readNotifier_ != nullptr) {
        delete readNotifier_;
        readNotifier_ = nullptr;
    }
    if (writeNotifier_ != nullptr) {
        delete writeNotifier_;
        writeNotifier_ = nullptr;
    }
    if (masterFd_ >= 0) {
        ::close(masterFd_);
        masterFd_ = -1;
    }
    pendingWrites_.clear();
}

void SessionWorker::shutdown()
{
    if (shuttingDown_ && vt_ == nullptr) {
        return;
    }
    shuttingDown_ = true;
    if (frameTimer_ != nullptr) {
        frameTimer_->stop();
    }
    if (childTimer_ != nullptr) {
        childTimer_->stop();
    }
    if (compressionTimer_ != nullptr) {
        compressionTimer_->stop();
    }

    if (childPid_ > 0) {
        const pid_t pid = static_cast<pid_t>(childPid_);
        ::kill(-pid, SIGHUP);

        QElapsedTimer timer;
        timer.start();
        int status = 0;
        bool reaped = false;
        while (timer.elapsed() < kShutdownGraceMilliseconds) {
            const pid_t result = ::waitpid(pid, &status, WNOHANG);
            if (result == pid || (result < 0 && errno == ECHILD)) {
                reaped = true;
                break;
            }
            QThread::msleep(10);
        }
        if (!reaped) {
            ::kill(-pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
        drainPty(true);
        childPid_ = -1;
    }
    running_ = false;
    potentialActivityTimer_.invalidate();
    setActiveProcess(false);
    closePty();
    destroyTerminal();
}

void SessionWorker::destroyTerminal()
{
    vt_.reset();
}
