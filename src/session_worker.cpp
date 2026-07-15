#include "session_worker.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUrl>

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
constexpr int kFrameCoalesceMilliseconds = 8;
constexpr int kCompressionIdleMilliseconds = 500;
constexpr int kShutdownGraceMilliseconds = 2000;

QColor toQColor(GhosttyColorRgb color)
{
    return QColor::fromRgb(color.r, color.g, color.b);
}

GhosttyColorRgb themeForeground()
{
    return GhosttyColorRgb{216, 222, 233};
}

GhosttyColorRgb themeBackground()
{
    return GhosttyColorRgb{30, 34, 42};
}

GhosttyColorRgb resolveStyleColor(const GhosttyStyleColor &color,
                                  const GhosttyRenderStateColors &colors,
                                  GhosttyColorRgb fallback)
{
    switch (color.tag) {
    case GHOSTTY_STYLE_COLOR_RGB:
        return color.value.rgb;
    case GHOSTTY_STYLE_COLOR_PALETTE:
        return colors.palette[color.value.palette];
    default:
        return fallback;
    }
}

uint16_t boundedU16(int value)
{
    return static_cast<uint16_t>(std::clamp(value, 1, static_cast<int>(UINT16_MAX)));
}

uint32_t boundedU32(int value)
{
    return static_cast<uint32_t>(std::max(value, 1));
}

bool containsControlText(const QByteArray &text)
{
    return std::any_of(text.cbegin(), text.cend(), [](char value) {
        const auto byte = static_cast<unsigned char>(value);
        return byte < 0x20U || byte == 0x7fU;
    });
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
    if (terminal_ != nullptr || running_) {
        return;
    }

    options_ = options;
    hold_ = options.hold;
    shuttingDown_ = false;

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
    const GhosttyTerminalOptions terminalOptions{
        .cols = boundedU16(columns_),
        .rows = boundedU16(rows_),
        .max_scrollback = static_cast<size_t>(std::max(options_.scrollbackLines, 0)),
    };

    if (ghostty_terminal_new(nullptr, &terminal_, terminalOptions) != GHOSTTY_SUCCESS) {
        return false;
    }

    const GhosttyColorRgb foreground = themeForeground();
    const GhosttyColorRgb background = themeBackground();
    const GhosttyColorRgb cursor = foreground;
    ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &foreground);
    ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &background);
    ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_COLOR_CURSOR, &cursor);

    ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_USERDATA, this);
    ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
                         reinterpret_cast<const void *>(&SessionWorker::writePtyCallback));
    ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_BELL,
                         reinterpret_cast<const void *>(&SessionWorker::bellCallback));
    ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED,
                         reinterpret_cast<const void *>(&SessionWorker::titleCallback));
    ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_PWD_CHANGED,
                         reinterpret_cast<const void *>(&SessionWorker::pwdCallback));
    ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_SIZE,
                         reinterpret_cast<const void *>(&SessionWorker::sizeCallback));
    ghostty_terminal_set(
        terminal_, GHOSTTY_TERMINAL_OPT_COLOR_SCHEME,
        reinterpret_cast<const void *>(&SessionWorker::colorSchemeCallback));
    ghostty_terminal_set(
        terminal_, GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES,
        reinterpret_cast<const void *>(&SessionWorker::deviceAttributesCallback));
    ghostty_terminal_set(
        terminal_, GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE,
        reinterpret_cast<const void *>(&SessionWorker::clipboardWriteCallback));

    if (ghostty_render_state_new(nullptr, &renderState_) != GHOSTTY_SUCCESS
        || ghostty_render_state_row_iterator_new(nullptr, &rowIterator_) != GHOSTTY_SUCCESS
        || ghostty_render_state_row_cells_new(nullptr, &rowCells_) != GHOSTTY_SUCCESS
        || ghostty_key_encoder_new(nullptr, &keyEncoder_) != GHOSTTY_SUCCESS
        || ghostty_key_event_new(nullptr, &keyEvent_) != GHOSTTY_SUCCESS
        || ghostty_mouse_encoder_new(nullptr, &mouseEncoder_) != GHOSTTY_SUCCESS
        || ghostty_mouse_event_new(nullptr, &mouseEvent_) != GHOSTTY_SUCCESS
        || ghostty_selection_gesture_new(nullptr, &selectionGesture_) != GHOSTTY_SUCCESS
        || ghostty_selection_gesture_event_new(
               nullptr, &selectionPressEvent_, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_PRESS)
               != GHOSTTY_SUCCESS
        || ghostty_selection_gesture_event_new(
               nullptr, &selectionDragEvent_, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_DRAG)
               != GHOSTTY_SUCCESS
        || ghostty_selection_gesture_event_new(
               nullptr, &selectionReleaseEvent_, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_RELEASE)
               != GHOSTTY_SUCCESS) {
        destroyTerminal();
        return false;
    }

    syncMouseEncoder();
    const bool deduplicateMotion = true;
    ghostty_mouse_encoder_setopt(mouseEncoder_, GHOSTTY_MOUSE_ENCODER_OPT_TRACK_LAST_CELL,
                                 &deduplicateMotion);
    ghostty_terminal_compression_activity(terminal_, &compressionActivity_);

    return true;
}

bool SessionWorker::spawnChild()
{
    QString executable;
    QStringList arguments = options_.program;

    if (arguments.isEmpty()) {
        executable = qEnvironmentVariable("SHELL");
        if (executable.isEmpty() || !QFileInfo(executable).isExecutable()) {
            executable = QStringLiteral("/bin/sh");
        }
        arguments = {executable};
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
    environment.insert(QStringLiteral("TERM"), QStringLiteral("xterm-ghostty"));
    environment.insert(QStringLiteral("TERMINFO"), QStringLiteral(GHOSTTY_QT_TERMINFO_DIR));
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

    if (terminal_ != nullptr) {
        if (ghostty_terminal_resize(terminal_, boundedU16(nextColumns), boundedU16(nextRows),
                                    boundedU32(nextCellWidth), boundedU32(nextCellHeight))
            != GHOSTTY_SUCCESS) {
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
    if (masterFd_ < 0 || terminal_ == nullptr) {
        return;
    }

    std::array<uint8_t, static_cast<size_t>(kReadBufferSize)> buffer{};
    qsizetype totalRead = 0;
    bool receivedData = false;

    while (totalRead < kMaximumReadPerActivation) {
        const ssize_t count = ::read(masterFd_, buffer.data(), buffer.size());
        if (count > 0) {
            const auto size = static_cast<size_t>(count);
            ghostty_terminal_vt_write(terminal_, buffer.data(), size);
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
        processDeferredEffects();
        noteCompressionActivity();
        scheduleFrame();
    }
    if (totalRead >= kMaximumReadPerActivation) {
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
    queuePtyWrite(encodeKey(input));
}

void SessionWorker::sendText(const QString &text)
{
    if (text.isEmpty()) {
        return;
    }
    TerminalKeyInput input;
    input.key = Qt::Key_unknown;
    input.text = text;
    input.pressed = true;
    queuePtyWrite(encodeKey(input));
}

QByteArray SessionWorker::encodeKey(const TerminalKeyInput &input)
{
    if (terminal_ == nullptr || keyEncoder_ == nullptr || keyEvent_ == nullptr) {
        return {};
    }

    ghostty_key_encoder_setopt_from_terminal(keyEncoder_, terminal_);
    ghostty_key_event_set_action(
        keyEvent_, input.pressed
            ? (input.autoRepeat ? GHOSTTY_KEY_ACTION_REPEAT : GHOSTTY_KEY_ACTION_PRESS)
            : GHOSTTY_KEY_ACTION_RELEASE);

    GhosttyKey key = mapQtKey(input.key);
    const auto qtModifiers = static_cast<Qt::KeyboardModifiers>(input.modifiers);
    if (qtModifiers.testFlag(Qt::KeypadModifier)) {
        if (input.key >= Qt::Key_0 && input.key <= Qt::Key_9) {
            key = static_cast<GhosttyKey>(GHOSTTY_KEY_NUMPAD_0 + input.key - Qt::Key_0);
        } else if (input.key == Qt::Key_Enter || input.key == Qt::Key_Return) {
            key = GHOSTTY_KEY_NUMPAD_ENTER;
        } else if (input.key == Qt::Key_Plus) {
            key = GHOSTTY_KEY_NUMPAD_ADD;
        } else if (input.key == Qt::Key_Minus) {
            key = GHOSTTY_KEY_NUMPAD_SUBTRACT;
        } else if (input.key == Qt::Key_Asterisk) {
            key = GHOSTTY_KEY_NUMPAD_MULTIPLY;
        } else if (input.key == Qt::Key_Slash) {
            key = GHOSTTY_KEY_NUMPAD_DIVIDE;
        } else if (input.key == Qt::Key_Period) {
            key = GHOSTTY_KEY_NUMPAD_DECIMAL;
        } else if (input.key == Qt::Key_Comma) {
            key = GHOSTTY_KEY_NUMPAD_COMMA;
        } else if (input.key == Qt::Key_Equal) {
            key = GHOSTTY_KEY_NUMPAD_EQUAL;
        }
    }

    ghostty_key_event_set_key(keyEvent_, key);
    ghostty_key_event_set_mods(keyEvent_, mapQtModifiers(input.modifiers));
    ghostty_key_event_set_consumed_mods(keyEvent_, 0);
    ghostty_key_event_set_composing(keyEvent_, input.composing);
    ghostty_key_event_set_unshifted_codepoint(keyEvent_, input.unshiftedCodepoint);

    QByteArray utf8 = input.text.toUtf8();
    if (containsControlText(utf8)) {
        utf8.clear();
    }
    ghostty_key_event_set_utf8(keyEvent_, utf8.isEmpty() ? nullptr : utf8.constData(),
                               static_cast<size_t>(utf8.size()));

    QByteArray encoded(128, Qt::Uninitialized);
    size_t written = 0;
    GhosttyResult result = ghostty_key_encoder_encode(
        keyEncoder_, keyEvent_, encoded.data(), static_cast<size_t>(encoded.size()), &written);
    if (result == GHOSTTY_OUT_OF_SPACE) {
        encoded.resize(static_cast<qsizetype>(written));
        result = ghostty_key_encoder_encode(keyEncoder_, keyEvent_, encoded.data(),
                                            static_cast<size_t>(encoded.size()), &written);
    }
    if (result != GHOSTTY_SUCCESS) {
        return {};
    }
    encoded.resize(static_cast<qsizetype>(written));
    return encoded;
}

void SessionWorker::sendMouse(const TerminalMouseInput &input)
{
    queuePtyWrite(encodeMouse(input));
}

QByteArray SessionWorker::encodeMouse(const TerminalMouseInput &input)
{
    if (terminal_ == nullptr || mouseEncoder_ == nullptr || mouseEvent_ == nullptr) {
        return {};
    }

    GhosttyMouseEncoderSize size{};
    size.size = sizeof(size);
    size.screen_width = boundedU32(surfaceWidthPixels_);
    size.screen_height = boundedU32(surfaceHeightPixels_);
    size.cell_width = boundedU32(cellWidthPixels_);
    size.cell_height = boundedU32(cellHeightPixels_);
    ghostty_mouse_encoder_setopt(mouseEncoder_, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &size);
    ghostty_mouse_encoder_setopt(mouseEncoder_, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED,
                                 &input.anyButtonPressed);
    GhosttyMouseAction action = GHOSTTY_MOUSE_ACTION_MOTION;
    if (input.action == TerminalMouseInput::Press) {
        action = GHOSTTY_MOUSE_ACTION_PRESS;
    } else if (input.action == TerminalMouseInput::Release) {
        action = GHOSTTY_MOUSE_ACTION_RELEASE;
    }
    ghostty_mouse_event_set_action(mouseEvent_, action);
    const GhosttyMouseButton button = mapQtMouseButton(input.button);
    if (button == GHOSTTY_MOUSE_BUTTON_UNKNOWN) {
        ghostty_mouse_event_clear_button(mouseEvent_);
    } else {
        ghostty_mouse_event_set_button(mouseEvent_, button);
    }
    ghostty_mouse_event_set_mods(mouseEvent_, mapQtModifiers(input.modifiers));
    ghostty_mouse_event_set_position(mouseEvent_, GhosttyMousePosition{input.x, input.y});

    QByteArray encoded(128, Qt::Uninitialized);
    size_t written = 0;
    GhosttyResult result = ghostty_mouse_encoder_encode(
        mouseEncoder_, mouseEvent_, encoded.data(), static_cast<size_t>(encoded.size()), &written);
    if (result == GHOSTTY_OUT_OF_SPACE) {
        encoded.resize(static_cast<qsizetype>(written));
        result = ghostty_mouse_encoder_encode(mouseEncoder_, mouseEvent_, encoded.data(),
                                              static_cast<size_t>(encoded.size()), &written);
    }
    if (result != GHOSTTY_SUCCESS) {
        return {};
    }
    encoded.resize(static_cast<qsizetype>(written));
    return encoded;
}

void SessionWorker::setFocused(bool focused)
{
    if (terminal_ == nullptr) {
        return;
    }
    bool reportFocus = false;
    if (ghostty_terminal_mode_get(terminal_, GHOSTTY_MODE_FOCUS_EVENT, &reportFocus)
            != GHOSTTY_SUCCESS
        || !reportFocus) {
        return;
    }

    std::array<char, 16> buffer{};
    size_t written = 0;
    if (ghostty_focus_encode(focused ? GHOSTTY_FOCUS_GAINED : GHOSTTY_FOCUS_LOST,
                             buffer.data(), buffer.size(), &written)
        == GHOSTTY_SUCCESS) {
        queuePtyWrite(QByteArray(buffer.data(), static_cast<qsizetype>(written)));
    }
}

void SessionWorker::paste(const QString &text)
{
    if (terminal_ == nullptr || text.isEmpty()) {
        return;
    }

    bool bracketed = false;
    ghostty_terminal_mode_get(terminal_, GHOSTTY_MODE_BRACKETED_PASTE, &bracketed);
    QByteArray mutableInput = text.toUtf8();
    QByteArray encoded(mutableInput.size() + 32, Qt::Uninitialized);
    size_t written = 0;
    GhosttyResult result = ghostty_paste_encode(
        mutableInput.data(), static_cast<size_t>(mutableInput.size()), bracketed,
        encoded.data(), static_cast<size_t>(encoded.size()), &written);
    if (result == GHOSTTY_OUT_OF_SPACE) {
        encoded.resize(static_cast<qsizetype>(written));
        result = ghostty_paste_encode(
            mutableInput.data(), static_cast<size_t>(mutableInput.size()), bracketed,
            encoded.data(), static_cast<size_t>(encoded.size()), &written);
    }
    if (result == GHOSTTY_SUCCESS) {
        encoded.resize(static_cast<qsizetype>(written));
        queuePtyWrite(encoded);
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
    if (terminal_ == nullptr) {
        return {};
    }
    GhosttyTerminalSelectionFormatOptions options{};
    options.size = sizeof(options);
    options.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
    options.unwrap = true;
    options.trim = true;
    options.selection = nullptr;

    size_t required = 0;
    GhosttyResult result = ghostty_terminal_selection_format_buf(
        terminal_, options, nullptr, 0, &required);
    if (result == GHOSTTY_NO_VALUE) {
        return {};
    }
    if (result != GHOSTTY_OUT_OF_SPACE && result != GHOSTTY_SUCCESS) {
        return {};
    }
    if (required == 0) {
        return QStringLiteral("");
    }

    QByteArray output(static_cast<qsizetype>(required), Qt::Uninitialized);
    size_t written = 0;
    result = ghostty_terminal_selection_format_buf(
        terminal_, options, reinterpret_cast<uint8_t *>(output.data()),
        static_cast<size_t>(output.size()), &written);
    if (result != GHOSTTY_SUCCESS) {
        return {};
    }
    output.resize(static_cast<qsizetype>(written));
    return QString::fromUtf8(output);
}

void SessionWorker::clearSelection()
{
    if (terminal_ == nullptr) {
        return;
    }
    ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_SELECTION, nullptr);
    ghostty_selection_gesture_reset(selectionGesture_, terminal_);
    scheduleFrame();
}

bool SessionWorker::pointToGridRef(int column, int row, GhosttyGridRef *out) const
{
    if (terminal_ == nullptr || out == nullptr) {
        return false;
    }
    GhosttyPoint point{};
    point.tag = GHOSTTY_POINT_TAG_VIEWPORT;
    point.value.coordinate.x = static_cast<uint16_t>(std::clamp(column, 0, columns_ - 1));
    point.value.coordinate.y = static_cast<uint32_t>(std::clamp(row, 0, rows_ - 1));
    *out = GhosttyGridRef{};
    out->size = sizeof(*out);
    return ghostty_terminal_grid_ref(terminal_, point, out) == GHOSTTY_SUCCESS;
}

void SessionWorker::beginSelection(int column, int row, int clickCount, bool rectangular)
{
    Q_UNUSED(rectangular)
    if (terminal_ == nullptr) {
        return;
    }

    GhosttyGridRef reference{};
    if (!pointToGridRef(column, row, &reference)) {
        return;
    }

    ghostty_selection_gesture_reset(selectionGesture_, terminal_);
    const int boundedClickCount = std::clamp(clickCount, 1, 3);
    const GhosttySelectionGestureBehavior behavior = boundedClickCount == 2
        ? GHOSTTY_SELECTION_GESTURE_BEHAVIOR_WORD
        : boundedClickCount >= 3 ? GHOSTTY_SELECTION_GESTURE_BEHAVIOR_LINE
                                 : GHOSTTY_SELECTION_GESTURE_BEHAVIOR_CELL;
    const GhosttySelectionGestureBehaviors behaviors{behavior, behavior, behavior};
    if (ghostty_selection_gesture_event_set(
            selectionPressEvent_, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, &reference)
            != GHOSTTY_SUCCESS
        || ghostty_selection_gesture_event_set(
               selectionPressEvent_, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_BEHAVIORS, &behaviors)
               != GHOSTTY_SUCCESS) {
        return;
    }

    GhosttySelection selection{};
    selection.size = sizeof(selection);
    if (ghostty_selection_gesture_event(
            selectionGesture_, terminal_, selectionPressEvent_, &selection)
        == GHOSTTY_SUCCESS) {
        installSelection(selection);
    }
}

void SessionWorker::updateSelection(int column, int row, bool rectangular)
{
    if (terminal_ == nullptr) {
        return;
    }
    GhosttyGridRef end{};
    if (!pointToGridRef(column, row, &end)) {
        return;
    }

    const GhosttySelectionGestureGeometry geometry{
        .columns = boundedU32(columns_),
        .cell_width = boundedU32(cellWidthPixels_),
        .padding_left = 0,
        .screen_height = boundedU32(surfaceHeightPixels_),
    };
    if (ghostty_selection_gesture_event_set(
            selectionDragEvent_, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, &end)
            != GHOSTTY_SUCCESS
        || ghostty_selection_gesture_event_set(
               selectionDragEvent_, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_GEOMETRY, &geometry)
               != GHOSTTY_SUCCESS
        || ghostty_selection_gesture_event_set(
               selectionDragEvent_, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_RECTANGLE, &rectangular)
               != GHOSTTY_SUCCESS) {
        return;
    }

    GhosttySelection selection{};
    selection.size = sizeof(selection);
    if (ghostty_selection_gesture_event(
            selectionGesture_, terminal_, selectionDragEvent_, &selection)
        == GHOSTTY_SUCCESS) {
        installSelection(selection);
    }
}

void SessionWorker::endSelection(int column, int row)
{
    if (terminal_ == nullptr || selectionGesture_ == nullptr) {
        return;
    }

    GhosttyGridRef reference{};
    const void *value = pointToGridRef(column, row, &reference) ? &reference : nullptr;
    if (ghostty_selection_gesture_event_set(
            selectionReleaseEvent_, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, value)
        == GHOSTTY_SUCCESS) {
        ghostty_selection_gesture_event(
            selectionGesture_, terminal_, selectionReleaseEvent_, nullptr);
    }
}

void SessionWorker::installSelection(const GhosttySelection &selection)
{
    if (ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_SELECTION, &selection)
        == GHOSTTY_SUCCESS) {
        scheduleFrame();
    }
}

void SessionWorker::scrollViewport(int rows)
{
    if (terminal_ == nullptr || rows == 0) {
        return;
    }
    GhosttyTerminalScrollViewport scroll{};
    scroll.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
    scroll.value.delta = static_cast<intptr_t>(rows);
    ghostty_terminal_scroll_viewport(terminal_, scroll);
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
    if (terminal_ == nullptr || compressionTimer_ == nullptr) {
        return;
    }
    uint64_t activity = 0;
    if (ghostty_terminal_compression_activity(terminal_, &activity) == GHOSTTY_SUCCESS
        && activity != compressionActivity_) {
        compressionActivity_ = activity;
        compressionTimer_->start();
    }
}

void SessionWorker::syncMouseEncoder()
{
    if (terminal_ == nullptr || mouseEncoder_ == nullptr) {
        return;
    }
    const std::array<GhosttyMode, 8> modes{
        GHOSTTY_MODE_X10_MOUSE,
        GHOSTTY_MODE_NORMAL_MOUSE,
        GHOSTTY_MODE_BUTTON_MOUSE,
        GHOSTTY_MODE_ANY_MOUSE,
        GHOSTTY_MODE_UTF8_MOUSE,
        GHOSTTY_MODE_SGR_MOUSE,
        GHOSTTY_MODE_URXVT_MOUSE,
        GHOSTTY_MODE_SGR_PIXELS_MOUSE,
    };
    uint32_t fingerprint = 0;
    for (size_t index = 0; index < modes.size(); ++index) {
        bool enabled = false;
        if (ghostty_terminal_mode_get(terminal_, modes[index], &enabled) == GHOSTTY_SUCCESS
            && enabled) {
            fingerprint |= uint32_t{1} << static_cast<uint32_t>(index);
        }
    }
    if (!mouseEncoderConfigured_ || fingerprint != mouseModeFingerprint_) {
        ghostty_mouse_encoder_setopt_from_terminal(mouseEncoder_, terminal_);
        mouseModeFingerprint_ = fingerprint;
        mouseEncoderConfigured_ = true;
    }
}

void SessionWorker::compressScrollback()
{
    if (terminal_ == nullptr || compressionTimer_ == nullptr) {
        return;
    }
    GhosttyTerminalCompressionResult result =
        GHOSTTY_TERMINAL_COMPRESSION_RESULT_COMPLETE;
    if (ghostty_terminal_compress(
            terminal_, GHOSTTY_TERMINAL_COMPRESSION_MODE_INCREMENTAL, &result)
            == GHOSTTY_SUCCESS
        && result == GHOSTTY_TERMINAL_COMPRESSION_RESULT_PENDING) {
        compressionTimer_->start(0);
    }
}

void SessionWorker::publishFrame()
{
    if (terminal_ == nullptr || renderState_ == nullptr || rowIterator_ == nullptr
        || rowCells_ == nullptr) {
        return;
    }
    if (ghostty_render_state_update(renderState_, terminal_) != GHOSTTY_SUCCESS) {
        return;
    }

    TerminalFrame frame;
    uint16_t columns = 0;
    uint16_t rows = 0;
    ghostty_render_state_get(renderState_, GHOSTTY_RENDER_STATE_DATA_COLS, &columns);
    ghostty_render_state_get(renderState_, GHOSTTY_RENDER_STATE_DATA_ROWS, &rows);
    frame.columns = static_cast<int>(columns);
    frame.rows = static_cast<int>(rows);
    frame.cells.resize(frame.columns * frame.rows);

    GhosttyRenderStateColors colors{};
    colors.size = sizeof(colors);
    if (ghostty_render_state_colors_get(renderState_, &colors) == GHOSTTY_SUCCESS) {
        frame.foreground = toQColor(colors.foreground);
        frame.background = toQColor(colors.background);
        frame.cursorColor = colors.cursor_has_value ? toQColor(colors.cursor) : frame.foreground;
    }

    ghostty_render_state_get(renderState_, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE,
                             &frame.cursorVisible);
    bool cursorInViewport = false;
    ghostty_render_state_get(renderState_,
                             GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE,
                             &cursorInViewport);
    frame.cursorVisible = frame.cursorVisible && cursorInViewport;
    ghostty_render_state_get(renderState_, GHOSTTY_RENDER_STATE_DATA_CURSOR_BLINKING,
                             &frame.cursorBlinking);
    uint16_t cursorColumn = 0;
    uint16_t cursorRow = 0;
    bool cursorOnWideTail = false;
    GhosttyRenderStateCursorVisualStyle cursorStyle =
        GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK;
    if (cursorInViewport) {
        ghostty_render_state_get(renderState_,
                                 GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X,
                                 &cursorColumn);
        ghostty_render_state_get(renderState_,
                                 GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y,
                                 &cursorRow);
        ghostty_render_state_get(renderState_,
                                 GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_WIDE_TAIL,
                                 &cursorOnWideTail);
    }
    ghostty_render_state_get(renderState_,
                             GHOSTTY_RENDER_STATE_DATA_CURSOR_VISUAL_STYLE,
                             &cursorStyle);
    frame.cursorColumn = static_cast<int>(cursorColumn);
    frame.cursorRow = static_cast<int>(cursorRow);
    frame.cursorStyle = static_cast<int>(cursorStyle);

    GhosttyTerminalScrollbar scrollbar{};
    if (ghostty_terminal_get(terminal_, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scrollbar)
        == GHOSTTY_SUCCESS) {
        frame.scrollTotal = scrollbar.total;
        frame.scrollOffset = scrollbar.offset;
        frame.scrollLength = scrollbar.len;
    }

    if (ghostty_render_state_get(renderState_, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                 &rowIterator_)
        != GHOSTTY_SUCCESS) {
        scheduleFrame();
        return;
    }
    int rowIndex = 0;
    while (rowIndex < frame.rows && ghostty_render_state_row_iterator_next(rowIterator_)) {
        GhosttyRenderStateRowSelection rowSelection{};
        rowSelection.size = sizeof(rowSelection);
        const bool hasSelection = ghostty_render_state_row_get(
                                      rowIterator_,
                                      GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION,
                                      &rowSelection)
            == GHOSTTY_SUCCESS;
        if (ghostty_render_state_row_get(rowIterator_,
                                         GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                         &rowCells_)
            != GHOSTTY_SUCCESS) {
            // Keep both the row and render state dirty. Clearing only the
            // global flag would discard an update that we failed to copy.
            scheduleFrame();
            return;
        }

        int columnIndex = 0;
        while (columnIndex < frame.columns
               && ghostty_render_state_row_cells_next(rowCells_)) {
            TerminalCell &cell = frame.cells[rowIndex * frame.columns + columnIndex];

            GhosttyCell rawCell = 0;
            GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
            if (ghostty_render_state_row_cells_get(
                    rowCells_, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &rawCell)
                    == GHOSTTY_SUCCESS
                && ghostty_cell_get(rawCell, GHOSTTY_CELL_DATA_WIDE, &wide)
                    == GHOSTTY_SUCCESS) {
                cell.columnSpan = wide == GHOSTTY_CELL_WIDE_WIDE ? 2 : 1;
                cell.spacer = wide == GHOSTTY_CELL_WIDE_SPACER_TAIL
                    || wide == GHOSTTY_CELL_WIDE_SPACER_HEAD;
            }

            std::array<uint8_t, 64> graphemeStorage{};
            GhosttyBuffer graphemeBuffer{
                .ptr = graphemeStorage.data(),
                .cap = graphemeStorage.size(),
                .len = 0,
            };
            GhosttyResult graphemeResult = ghostty_render_state_row_cells_get(
                rowCells_, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8,
                &graphemeBuffer);
            QByteArray dynamicGrapheme;
            if (graphemeResult == GHOSTTY_OUT_OF_SPACE) {
                dynamicGrapheme.resize(static_cast<qsizetype>(graphemeBuffer.len));
                graphemeBuffer.ptr = reinterpret_cast<uint8_t *>(dynamicGrapheme.data());
                graphemeBuffer.cap = static_cast<size_t>(dynamicGrapheme.size());
                graphemeResult = ghostty_render_state_row_cells_get(
                    rowCells_, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8,
                    &graphemeBuffer);
            }
            if (graphemeResult == GHOSTTY_SUCCESS && graphemeBuffer.len > 0) {
                cell.text = QString::fromUtf8(
                    reinterpret_cast<const char *>(graphemeBuffer.ptr),
                    static_cast<qsizetype>(graphemeBuffer.len));
            }

            GhosttyStyle style{};
            style.size = sizeof(style);
            ghostty_render_state_row_cells_get(
                rowCells_, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);
            GhosttyColorRgb foreground = colors.foreground;
            GhosttyColorRgb background = colors.background;
            GhosttyColorRgb explicitColor{};
            if (ghostty_render_state_row_cells_get(
                    rowCells_, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR,
                    &explicitColor)
                == GHOSTTY_SUCCESS) {
                foreground = explicitColor;
            }
            if (ghostty_render_state_row_cells_get(
                    rowCells_, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR,
                    &explicitColor)
                == GHOSTTY_SUCCESS) {
                background = explicitColor;
            }
            if (style.inverse) {
                std::swap(foreground, background);
            }

            cell.foreground = toQColor(foreground);
            cell.background = toQColor(background);
            cell.underlineColor = toQColor(resolveStyleColor(
                style.underline_color, colors, foreground));
            cell.bold = style.bold;
            cell.italic = style.italic;
            cell.faint = style.faint;
            cell.underline = style.underline != GHOSTTY_SGR_UNDERLINE_NONE;
            cell.strikeThrough = style.strikethrough;
            cell.overline = style.overline;
            cell.selected = hasSelection
                && columnIndex >= static_cast<int>(rowSelection.start_x)
                && columnIndex <= static_cast<int>(rowSelection.end_x);
            if (style.invisible) {
                cell.text.clear();
            }
            if (cell.spacer) {
                cell.text.clear();
            }

            ++columnIndex;
        }

        const bool clean = false;
        ghostty_render_state_row_set(rowIterator_,
                                     GHOSTTY_RENDER_STATE_ROW_OPTION_DIRTY,
                                     &clean);
        ++rowIndex;
    }

    const GhosttyRenderStateDirty cleanState = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    ghostty_render_state_set(renderState_, GHOSTTY_RENDER_STATE_OPTION_DIRTY,
                             &cleanState);

    if (frame.cursorVisible) {
        if (cursorOnWideTail && frame.cursorColumn > 0) {
            --frame.cursorColumn;
            frame.cursorColumnSpan = 2;
        } else {
            const int cursorIndex = frame.cursorRow * frame.columns + frame.cursorColumn;
            if (cursorIndex >= 0 && cursorIndex < frame.cells.size()) {
                frame.cursorColumnSpan = frame.cells.at(cursorIndex).columnSpan;
            }
        }
    }

    bool tracking = false;
    ghostty_terminal_get(terminal_, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &tracking);
    if (tracking != mouseTracking_) {
        mouseTracking_ = tracking;
        Q_EMIT mouseTrackingChanged(mouseTracking_);
    }
    Q_EMIT frameReady(frame);
}

void SessionWorker::processDeferredEffects()
{
    if (terminal_ == nullptr) {
        return;
    }
    if (titleDirty_) {
        titleDirty_ = false;
        GhosttyString title{};
        if (ghostty_terminal_get(terminal_, GHOSTTY_TERMINAL_DATA_TITLE, &title)
            == GHOSTTY_SUCCESS) {
            Q_EMIT titleChanged(QString::fromUtf8(
                reinterpret_cast<const char *>(title.ptr),
                static_cast<qsizetype>(title.len)));
        }
    }
    if (pwdDirty_) {
        pwdDirty_ = false;
        GhosttyString pwd{};
        if (ghostty_terminal_get(terminal_, GHOSTTY_TERMINAL_DATA_PWD, &pwd)
            == GHOSTTY_SUCCESS) {
            QString directory = QString::fromUtf8(
                reinterpret_cast<const char *>(pwd.ptr),
                static_cast<qsizetype>(pwd.len));
            const QUrl url(directory);
            if (url.isLocalFile()) {
                directory = url.toLocalFile();
            }
            Q_EMIT currentDirectoryChanged(directory);
        }
    }
    if (bellPending_) {
        bellPending_ = false;
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
    } else if (result < 0 && errno == ECHILD) {
        readFromPty();
        running_ = false;
        childPid_ = -1;
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

void SessionWorker::handleChildStatus(int status)
{
    // A process may exit before Qt delivers the final readability event. Drain
    // the PTY and publish synchronously so short-lived commands cannot lose
    // their last screen update when the master descriptor is closed below.
    readFromPty();
    if (frameTimer_ != nullptr && frameTimer_->isActive()) {
        frameTimer_->stop();
    }
    publishFrame();

    running_ = false;
    childPid_ = -1;
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
    if (shuttingDown_ && terminal_ == nullptr) {
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
        readFromPty();
        childPid_ = -1;
    }
    running_ = false;
    closePty();
    destroyTerminal();
}

void SessionWorker::destroyTerminal()
{
    if (selectionReleaseEvent_ != nullptr) {
        ghostty_selection_gesture_event_free(selectionReleaseEvent_);
        selectionReleaseEvent_ = nullptr;
    }
    if (selectionDragEvent_ != nullptr) {
        ghostty_selection_gesture_event_free(selectionDragEvent_);
        selectionDragEvent_ = nullptr;
    }
    if (selectionPressEvent_ != nullptr) {
        ghostty_selection_gesture_event_free(selectionPressEvent_);
        selectionPressEvent_ = nullptr;
    }
    if (selectionGesture_ != nullptr) {
        ghostty_selection_gesture_free(selectionGesture_, terminal_);
        selectionGesture_ = nullptr;
    }
    if (mouseEvent_ != nullptr) {
        ghostty_mouse_event_free(mouseEvent_);
        mouseEvent_ = nullptr;
    }
    if (mouseEncoder_ != nullptr) {
        ghostty_mouse_encoder_free(mouseEncoder_);
        mouseEncoder_ = nullptr;
    }
    if (keyEvent_ != nullptr) {
        ghostty_key_event_free(keyEvent_);
        keyEvent_ = nullptr;
    }
    if (keyEncoder_ != nullptr) {
        ghostty_key_encoder_free(keyEncoder_);
        keyEncoder_ = nullptr;
    }
    if (rowCells_ != nullptr) {
        ghostty_render_state_row_cells_free(rowCells_);
        rowCells_ = nullptr;
    }
    if (rowIterator_ != nullptr) {
        ghostty_render_state_row_iterator_free(rowIterator_);
        rowIterator_ = nullptr;
    }
    if (renderState_ != nullptr) {
        ghostty_render_state_free(renderState_);
        renderState_ = nullptr;
    }
    if (terminal_ != nullptr) {
        ghostty_terminal_free(terminal_);
        terminal_ = nullptr;
    }
}

GhosttyKey SessionWorker::mapQtKey(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return static_cast<GhosttyKey>(GHOSTTY_KEY_A + key - Qt::Key_A);
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return static_cast<GhosttyKey>(GHOSTTY_KEY_DIGIT_0 + key - Qt::Key_0);
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        return static_cast<GhosttyKey>(GHOSTTY_KEY_F1 + key - Qt::Key_F1);
    }

    switch (key) {
    case Qt::Key_QuoteLeft:
    case Qt::Key_AsciiTilde: return GHOSTTY_KEY_BACKQUOTE;
    case Qt::Key_Backslash:
    case Qt::Key_Bar: return GHOSTTY_KEY_BACKSLASH;
    case Qt::Key_BracketLeft:
    case Qt::Key_BraceLeft: return GHOSTTY_KEY_BRACKET_LEFT;
    case Qt::Key_BracketRight:
    case Qt::Key_BraceRight: return GHOSTTY_KEY_BRACKET_RIGHT;
    case Qt::Key_Comma:
    case Qt::Key_Less: return GHOSTTY_KEY_COMMA;
    case Qt::Key_Equal:
    case Qt::Key_Plus: return GHOSTTY_KEY_EQUAL;
    case Qt::Key_Minus:
    case Qt::Key_Underscore: return GHOSTTY_KEY_MINUS;
    case Qt::Key_Period:
    case Qt::Key_Greater: return GHOSTTY_KEY_PERIOD;
    case Qt::Key_Apostrophe:
    case Qt::Key_QuoteDbl: return GHOSTTY_KEY_QUOTE;
    case Qt::Key_Semicolon:
    case Qt::Key_Colon: return GHOSTTY_KEY_SEMICOLON;
    case Qt::Key_Slash:
    case Qt::Key_Question: return GHOSTTY_KEY_SLASH;
    case Qt::Key_Backspace: return GHOSTTY_KEY_BACKSPACE;
    case Qt::Key_Return:
    case Qt::Key_Enter: return GHOSTTY_KEY_ENTER;
    case Qt::Key_Space: return GHOSTTY_KEY_SPACE;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return GHOSTTY_KEY_TAB;
    case Qt::Key_Delete: return GHOSTTY_KEY_DELETE;
    case Qt::Key_End: return GHOSTTY_KEY_END;
    case Qt::Key_Home: return GHOSTTY_KEY_HOME;
    case Qt::Key_Insert: return GHOSTTY_KEY_INSERT;
    case Qt::Key_PageDown: return GHOSTTY_KEY_PAGE_DOWN;
    case Qt::Key_PageUp: return GHOSTTY_KEY_PAGE_UP;
    case Qt::Key_Down: return GHOSTTY_KEY_ARROW_DOWN;
    case Qt::Key_Left: return GHOSTTY_KEY_ARROW_LEFT;
    case Qt::Key_Right: return GHOSTTY_KEY_ARROW_RIGHT;
    case Qt::Key_Up: return GHOSTTY_KEY_ARROW_UP;
    case Qt::Key_Escape: return GHOSTTY_KEY_ESCAPE;
    case Qt::Key_Pause: return GHOSTTY_KEY_PAUSE;
    case Qt::Key_Print: return GHOSTTY_KEY_PRINT_SCREEN;
    case Qt::Key_ScrollLock: return GHOSTTY_KEY_SCROLL_LOCK;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
}

GhosttyMods SessionWorker::mapQtModifiers(int modifiers)
{
    const auto qtModifiers = static_cast<Qt::KeyboardModifiers>(modifiers);
    GhosttyMods result = 0;
    if (qtModifiers.testFlag(Qt::ShiftModifier)) result |= GHOSTTY_MODS_SHIFT;
    if (qtModifiers.testFlag(Qt::ControlModifier)) result |= GHOSTTY_MODS_CTRL;
    if (qtModifiers.testFlag(Qt::AltModifier)) result |= GHOSTTY_MODS_ALT;
    if (qtModifiers.testFlag(Qt::MetaModifier)) result |= GHOSTTY_MODS_SUPER;
    return result;
}

GhosttyMouseButton SessionWorker::mapQtMouseButton(int button)
{
    switch (button) {
    case 1: return GHOSTTY_MOUSE_BUTTON_LEFT;
    case 2: return GHOSTTY_MOUSE_BUTTON_RIGHT;
    case 3: return GHOSTTY_MOUSE_BUTTON_MIDDLE;
    case 4: return GHOSTTY_MOUSE_BUTTON_FOUR;
    case 5: return GHOSTTY_MOUSE_BUTTON_FIVE;
    case 6: return GHOSTTY_MOUSE_BUTTON_SIX;
    case 7: return GHOSTTY_MOUSE_BUTTON_SEVEN;
    case 8: return GHOSTTY_MOUSE_BUTTON_EIGHT;
    case 9: return GHOSTTY_MOUSE_BUTTON_NINE;
    default: return GHOSTTY_MOUSE_BUTTON_UNKNOWN;
    }
}

void SessionWorker::writePtyCallback(GhosttyTerminal, void *userdata,
                                     const uint8_t *data, size_t length)
{
    auto *worker = static_cast<SessionWorker *>(userdata);
    if (worker != nullptr && data != nullptr && length > 0) {
        worker->queuePtyWrite(QByteArray(
            reinterpret_cast<const char *>(data), static_cast<qsizetype>(length)));
    }
}

void SessionWorker::bellCallback(GhosttyTerminal, void *userdata)
{
    if (auto *worker = static_cast<SessionWorker *>(userdata)) {
        worker->bellPending_ = true;
    }
}

void SessionWorker::titleCallback(GhosttyTerminal, void *userdata)
{
    if (auto *worker = static_cast<SessionWorker *>(userdata)) {
        worker->titleDirty_ = true;
    }
}

void SessionWorker::pwdCallback(GhosttyTerminal, void *userdata)
{
    if (auto *worker = static_cast<SessionWorker *>(userdata)) {
        worker->pwdDirty_ = true;
    }
}

bool SessionWorker::sizeCallback(GhosttyTerminal, void *userdata,
                                 GhosttySizeReportSize *size)
{
    auto *worker = static_cast<SessionWorker *>(userdata);
    if (worker == nullptr || size == nullptr) {
        return false;
    }
    size->rows = boundedU16(worker->rows_);
    size->columns = boundedU16(worker->columns_);
    size->cell_width = boundedU32(worker->cellWidthPixels_);
    size->cell_height = boundedU32(worker->cellHeightPixels_);
    return true;
}

bool SessionWorker::colorSchemeCallback(GhosttyTerminal, void *,
                                        GhosttyColorScheme *scheme)
{
    if (scheme == nullptr) {
        return false;
    }
    *scheme = GHOSTTY_COLOR_SCHEME_DARK;
    return true;
}

bool SessionWorker::deviceAttributesCallback(GhosttyTerminal, void *,
                                             GhosttyDeviceAttributes *attributes)
{
    if (attributes == nullptr) {
        return false;
    }

    *attributes = GhosttyDeviceAttributes{};
    attributes->primary.conformance_level = GHOSTTY_DA_CONFORMANCE_VT220;
    attributes->primary.features[0] = GHOSTTY_DA_FEATURE_ANSI_COLOR;
    attributes->primary.num_features = 1;
    attributes->secondary.device_type = GHOSTTY_DA_DEVICE_TYPE_VT220;
    return true;
}

GhosttyClipboardWriteResult SessionWorker::clipboardWriteCallback(
    GhosttyTerminal, void *, const GhosttyClipboardWrite *)
{
    return GHOSTTY_CLIPBOARD_WRITE_RESULT_DENIED;
}
