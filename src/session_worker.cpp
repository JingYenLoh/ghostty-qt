#include "session_worker.h"
#include "desktop_activation.h"
#include "ghostty_link_matcher.h"
#include "ghostty_shell_integration.h"
#include "ghostty_vt_adapter.h"
#include "linux_cgroup.h"
#include "posix_regular_file.h"
#include "terminal_clipboard.h"
#include "terminal_osc8_index.h"
#include "terminfo_paths.h"
#include "zig_string_escape.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSocketNotifier>
#include <QStringTokenizer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>
#include <ranges>
#include <utility>
#include <variant>

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <pthread.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr qsizetype kReadBufferSize = 64 * 1024;
constexpr qsizetype kMaximumReadPerActivation = 1024 * 1024;
constexpr qsizetype kMaximumFinalRead = 8 * 1024 * 1024;
constexpr int kFrameCoalesceMilliseconds = 8;
constexpr int kCompressionIdleMilliseconds = 250;
constexpr int kCompressionStepMilliseconds = 1;
constexpr int kSelectionAutoscrollMilliseconds = 15;
constexpr int kCursorBlinkResetThrottleMilliseconds = 500;
constexpr int kShutdownGraceMilliseconds = 2000;
constexpr int kPotentialActivityGraceMilliseconds = 250;
constexpr int kSearchRowsPerChunk = 8;
constexpr int kSearchChunkBudgetMilliseconds = 2;
constexpr int kSearchPublishIntervalMilliseconds = 33;
constexpr quint64 kSearchRowsPerCompressionPass = 64;
constexpr qsizetype kMaximumInitialInputFileSize = 10 * 1024 * 1024;
constexpr Qt::KeyboardModifiers kTrackedTerminalModifiers = Qt::ShiftModifier
    | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;

QString initialInputPathLabel(QByteArrayView path)
{
    return QString::fromLocal8Bit(path.data(), path.size());
}

std::expected<QByteArray, QString> readInitialInputFile(QByteArrayView path,
                                                        qsizetype index)
{
    auto contents =
        readBoundedPosixRegularFile(path, kMaximumInitialInputFileSize);
    if (contents) return std::move(*contents);

    const PosixRegularFileError error = contents.error();
    if (error.kind == PosixRegularFileErrorKind::InvalidPath) {
        return std::unexpected(
            QStringLiteral(
                "Initial input path at entry %1 contains a NUL byte.")
                .arg(index));
    }
    if (error.kind == PosixRegularFileErrorKind::NotRegular) {
        return std::unexpected(
            QStringLiteral("Initial input path \"%1\" at entry %2 is not a "
                           "regular file.")
                .arg(initialInputPathLabel(path))
                .arg(index));
    }
    if (error.kind == PosixRegularFileErrorKind::TooLarge) {
        return std::unexpected(
            QStringLiteral("Initial input path \"%1\" at entry %2 exceeds the "
                           "10 MiB limit.")
                .arg(initialInputPathLabel(path))
                .arg(index));
    }

    const QString operation = error.kind == PosixRegularFileErrorKind::Read
        ? QStringLiteral("read")
        : error.kind == PosixRegularFileErrorKind::Inspect
        ? QStringLiteral("inspect")
        : QStringLiteral("open");
    return std::unexpected(
        QStringLiteral("Unable to %1 initial input path \"%2\" at entry %3: %4")
            .arg(operation, initialInputPathLabel(path))
            .arg(index)
            .arg(QString::fromLocal8Bit(std::strerror(error.systemError))));
}

std::expected<QVector<QByteArray>, QString>
prepareInitialInput(QVector<TerminalInitialInput> sources)
{
    QVector<QByteArray> chunks;
    chunks.reserve(sources.size());
    for (qsizetype index = 0; index < sources.size(); ++index) {
        TerminalInitialInput &source = sources[index];
        if (auto *raw = std::get_if<TerminalInitialInputs::Raw>(&source)) {
            chunks.push_back(std::move(raw->bytes));
            continue;
        }

        const auto &path = std::get<TerminalInitialInputs::Path>(source);
        std::expected<QByteArray, QString> contents =
            readInitialInputFile(path.path, index);
        if (!contents.has_value()) {
            return std::unexpected(std::move(contents.error()));
        }
        chunks.push_back(std::move(*contents));
    }
    return chunks;
}

int openChildExitFd(pid_t pid)
{
#ifdef SYS_pidfd_open
    long result = -1;
    do {
        result = ::syscall(SYS_pidfd_open, pid, 0);
    } while (result < 0 && errno == EINTR);
    if (result >= 0 && result <= std::numeric_limits<int>::max()) {
        return static_cast<int>(result);
    }
    if (result >= 0) {
        (void)::close(static_cast<int>(result));
        errno = EMFILE;
    }
#else
    Q_UNUSED(pid);
    errno = ENOSYS;
#endif
    return -1;
}

void setEnvironmentEntry(TerminalEnvironment &environment,
                         const QByteArray &key, const QByteArray &value)
{
    environment.removeIf([&key](const TerminalEnvironmentEntry &entry) {
        return entry.key == key;
    });
    environment.push_back({
        .key = key,
        .value = value,
    });
}

void removeEnvironmentEntry(TerminalEnvironment &environment,
                            const QByteArray &key)
{
    environment.removeIf([&key](const TerminalEnvironmentEntry &entry) {
        return entry.key == key;
    });
}

std::optional<QByteArray>
environmentValue(const TerminalEnvironment &environment, const QByteArray &key)
{
    const auto entry =
        std::ranges::find(environment, key, &TerminalEnvironmentEntry::key);
    if (entry == environment.cend()) return std::nullopt;
    return entry->value;
}

TerminalEnvironment inheritedTerminalEnvironment()
{
    const QStringList inherited = sanitizedChildEnvironment().toStringList();
    TerminalEnvironment result;
    result.reserve(inherited.size());
    for (const QString &serialized : inherited) {
        const QByteArray bytes = serialized.toLocal8Bit();
        const qsizetype separator = bytes.indexOf('=');
        if (separator <= 0) continue;
        setEnvironmentEntry(result, bytes.first(separator),
                            bytes.sliced(separator + 1));
    }
    return result;
}

void appendExecutableDirectory(TerminalEnvironment &environment,
                               const QByteArray &directory)
{
    const std::optional<QByteArray> current =
        environmentValue(environment, QByteArrayLiteral("PATH"));
    if (current.has_value()) {
        const QList<QByteArray> entries = current->split(':');
        if (entries.contains(directory)) return;
        QByteArray extended = *current;
        if (!extended.isEmpty()) extended.append(':');
        extended.append(directory);
        setEnvironmentEntry(environment, QByteArrayLiteral("PATH"), extended);
        return;
    }
    setEnvironmentEntry(environment, QByteArrayLiteral("PATH"), directory);
}

ssize_t writeWithoutSigpipe(int fd, const void *data, size_t size)
{
    sigset_t blocked;
    sigset_t previous;
    sigset_t pending;
    if (::sigemptyset(&blocked) != 0 || ::sigaddset(&blocked, SIGPIPE) != 0) {
        return -1;
    }
    const int blockError = ::pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (blockError != 0) {
        errno = blockError;
        return -1;
    }
    if (::sigpending(&pending) != 0) {
        const int pendingError = errno;
        (void)::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        errno = pendingError;
        return -1;
    }
    const int pendingState = ::sigismember(&pending, SIGPIPE);
    if (pendingState < 0) {
        const int pendingError = errno;
        (void)::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        errno = pendingError;
        return -1;
    }

    ssize_t written = -1;
    do {
        written = ::write(fd, data, size);
    } while (written < 0 && errno == EINTR);
    const int writeError = written < 0 ? errno : 0;

    if (written < 0 && writeError == EPIPE && pendingState == 0) {
        const struct timespec noWait{};
        while (::sigtimedwait(&blocked, nullptr, &noWait) < 0
               && errno == EINTR) {}
    }

    const int restoreError = ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    if (written >= 0 && restoreError != 0) {
        errno = restoreError;
        return -1;
    }
    if (written < 0) errno = writeError;
    return written;
}

QString terminalFileBaseName(TerminalFileLocation location)
{
    switch (location) {
    case TerminalFileLocation::Screen: return QStringLiteral("screen.txt");
    case TerminalFileLocation::Scrollback: return QStringLiteral("history.txt");
    case TerminalFileLocation::Selection:
        return QStringLiteral("selection.txt");
    }
    Q_UNREACHABLE_RETURN({});
}

bool terminalDirectoryHasPrivateMode(const QString &path)
{
    struct stat status{};
    const QByteArray encodedPath = QFile::encodeName(path);
    return ::stat(encodedPath.constData(), &status) == 0
        && S_ISDIR(status.st_mode) && (status.st_mode & 0777) == 0700;
}

bool terminalFileHasPrivateMode(const QFile &file)
{
    struct stat status{};
    return file.isOpen() && file.handle() >= 0
        && ::fstat(file.handle(), &status) == 0 && S_ISREG(status.st_mode)
        && (status.st_mode & 0777) == 0600;
}

QByteArray appendPath(QByteArray directory, QByteArrayView path)
{
    if (!directory.endsWith('/')) {
        directory += '/';
    }
    directory.append(path.data(), path.size());
    return directory;
}

QByteArray absolutePathFromWorkingDirectory(QByteArray workingDirectory,
                                            QByteArrayView relativePath)
{
    if (!workingDirectory.startsWith('/')) {
        workingDirectory = appendPath(QFile::encodeName(QDir::currentPath()),
                                      workingDirectory);
    }
    return appendPath(std::move(workingDirectory), relativePath);
}

QVector<QByteArray> executableCandidates(QByteArrayView name)
{
    if (name.contains('/')) {
        return {QByteArray(name.data(), name.size())};
    }

    const QByteArray path = qEnvironmentVariableIsSet("PATH")
        ? qgetenv("PATH")
        : QByteArrayLiteral("/usr/local/bin:/bin/:/usr/bin");
    // Pinned Zig's tokenizeScalar skips empty entries rather than treating
    // them as the current directory.
    QVector<QByteArray> candidates;
    candidates.reserve(path.count(':') + 1);
    for (const QByteArray &directory : path.split(':')) {
        if (directory.isEmpty()) continue;
        candidates.append(appendPath(directory, name));
    }
    return candidates;
}

bool hasExecutableCandidate(const QVector<QByteArray> &candidates,
                            const QByteArray &workingDirectory)
{
    return std::ranges::any_of(candidates, [&](const QByteArray &candidate) {
        if (candidate.contains('\0')) return false;
        const QByteArray validationPath = candidate.startsWith('/')
            ? candidate
            : absolutePathFromWorkingDirectory(workingDirectory, candidate);
        struct stat status{};
        return ::stat(validationPath.constData(), &status) == 0
            && S_ISREG(status.st_mode)
            && ::access(validationPath.constData(), X_OK) == 0;
    });
}

char foldSearchByte(char value)
{
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A'))
                                        : value;
}

QByteArray reversedFoldedSearchNeedle(QByteArrayView needle)
{
    QByteArray result;
    result.reserve(needle.size());
    for (char value : needle | std::views::reverse) {
        result.append(foldSearchByte(value));
    }
    return result;
}

bool searchNeedlesEqual(QByteArrayView left, QByteArrayView right)
{
    return std::ranges::equal(left, right, std::ranges::equal_to{},
                              foldSearchByte, foldSearchByte);
}

QVector<qsizetype> searchPrefixTable(QByteArrayView pattern)
{
    QVector<qsizetype> prefix(pattern.size(), 0);
    for (qsizetype index = 1, matched = 0; index < pattern.size(); ++index) {
        while (matched > 0 && pattern.at(index) != pattern.at(matched)) {
            matched = prefix.at(matched - 1);
        }
        if (pattern.at(index) == pattern.at(matched)) {
            ++matched;
        }
        prefix[index] = matched;
    }
    return prefix;
}

std::optional<qsizetype> gridCellCount(int columns, int rows)
{
    if (columns <= 0 || rows <= 0) return std::nullopt;
    const qsizetype columnCount = columns;
    const qsizetype rowCount = rows;
    if (columnCount > std::numeric_limits<qsizetype>::max() / rowCount) {
        return std::nullopt;
    }
    return columnCount * rowCount;
}

void addVisibleSearchCells(QBitArray &destination, TerminalSearchRange range,
                           const GhosttyVtAdapter::SearchExtent &extent)
{
    const std::optional<qsizetype> cellCount =
        gridCellCount(extent.columns, extent.rows);
    if (range.end < range.start) {
        std::swap(range.start, range.end);
    }
    if (!cellCount.has_value() || destination.size() != *cellCount
        || extent.viewportLength == 0 || range.start.column >= extent.columns
        || range.end.column >= extent.columns
        || range.start.screenRow >= extent.totalRows
        || range.end.screenRow >= extent.totalRows) {
        return;
    }

    const quint64 viewportStart = extent.viewportOffset;
    const quint64 viewportEnd = viewportStart + extent.viewportLength;
    const quint64 firstRow =
        std::max<quint64>(range.start.screenRow, viewportStart);
    const quint64 lastRowExclusive = std::min<quint64>(
        static_cast<quint64>(range.end.screenRow) + 1U, viewportEnd);
    if (firstRow >= lastRowExclusive) {
        return;
    }

    for (quint64 row = firstRow; row < lastRowExclusive; ++row) {
        const int firstColumn = row == range.start.screenRow
            ? static_cast<int>(range.start.column)
            : 0;
        const int lastColumn = row == range.end.screenRow
            ? static_cast<int>(range.end.column)
            : extent.columns - 1;
        const quint64 relativeRow = row - viewportStart;
        if (relativeRow >= static_cast<quint64>(extent.rows)) {
            continue;
        }
        const int viewportRow = static_cast<int>(relativeRow);
        for (int column = firstColumn; column <= lastColumn; ++column) {
            destination.setBit(
                static_cast<qsizetype>(viewportRow) * extent.columns + column);
        }
    }
}

bool keyMayStartProcess(const TerminalKeyInput &input)
{
    return input.pressed
        && (input.key == Qt::Key_Return || input.key == Qt::Key_Enter
            || input.text.contains(u'\n') || input.text.contains(u'\r'));
}

bool bytesMayStartProcess(const QByteArray &data)
{
    return data.contains('\n') || data.contains('\r');
}

bool sequenceTokenIsNewer(quint64 candidate, quint64 current)
{
    if (candidate == 0 || candidate == current) {
        return false;
    }
    if (current == 0) {
        return true;
    }

    // Tokens are controller-local unsigned counters. Compare them in modular
    // space so an eventual wrap from UINT64_MAX to one remains newer while a
    // delayed token from the previous half of the range remains stale.
    const quint64 distance = candidate - current;
    return distance < (quint64{1} << 63U);
}

uint16_t boundedU16(int value)
{
    return static_cast<uint16_t>(
        std::clamp(value, 1, static_cast<int>(UINT16_MAX)));
}

uint16_t boundedPixelU16(int value)
{
    return static_cast<uint16_t>(
        std::clamp(value, 0, static_cast<int>(UINT16_MAX)));
}

using TrackedTerminalLink = std::variant<GhosttyVtAdapter::TrackedHyperlink,
                                         GhosttyVtAdapter::TrackedTextRange>;

struct ResolvedTerminalLink {
    TerminalLinkKind kind = TerminalLinkKind::Osc8;
    QByteArray uri;
    QPoint targetCell{-1, -1};
    QVector<QPoint> cells;
    QVector<int> relevantRows;
};

struct DetectedTerminalLink {
    TrackedTerminalLink tracked;
    ResolvedTerminalLink resolved;
};

std::optional<ResolvedTerminalLink>
resolveTrackedTerminalLink(GhosttyVtAdapter &adapter,
                           const TrackedTerminalLink &tracked,
                           const TerminalOsc8Index &viewport)
{
    if (const auto *osc8 =
            std::get_if<GhosttyVtAdapter::TrackedHyperlink>(&tracked)) {
        const auto match =
            adapter.resolveHyperlink(*osc8, viewport.candidates());
        if (!match.has_value()) {
            return std::nullopt;
        }
        QVector<int> relevantRows;
        relevantRows.reserve(match->cells.size());
        for (const QPoint &cell : match->cells) {
            if (!relevantRows.contains(cell.y())) {
                relevantRows.append(cell.y());
            }
        }
        return ResolvedTerminalLink{
            .kind = TerminalLinkKind::Osc8,
            .uri = match->uri,
            .targetCell = match->targetCell,
            .cells = match->cells,
            .relevantRows = std::move(relevantRows),
        };
    }

    const auto &range = std::get<GhosttyVtAdapter::TrackedTextRange>(tracked);
    const auto match = adapter.resolveTextRange(range);
    if (!match.has_value()) {
        return std::nullopt;
    }
    return ResolvedTerminalLink{
        .kind = TerminalLinkKind::Regex,
        .uri = match->text,
        .targetCell = match->targetCell,
        .cells = match->cells,
        .relevantRows = match->logicalLineRows,
    };
}

bool trackedTerminalLinkValid(GhosttyVtAdapter &adapter,
                              const TrackedTerminalLink &tracked)
{
    if (const auto *osc8 =
            std::get_if<GhosttyVtAdapter::TrackedHyperlink>(&tracked)) {
        return adapter.trackedHyperlinkValid(*osc8);
    }
    return adapter.trackedTextRangeValid(
        std::get<GhosttyVtAdapter::TrackedTextRange>(tracked));
}

std::optional<DetectedTerminalLink>
detectTerminalLinkAt(GhosttyVtAdapter &adapter, GhosttyLinkMatcher *matcher,
                     bool linkUrl, const TerminalOsc8Index &viewport,
                     int column, int row)
{
    if (!viewport.containsCoordinate(column, row)) {
        return std::nullopt;
    }

    // Ghostty gives explicit OSC 8 destinations priority over configured
    // regex links at the same cell. If URI extraction fails, continue to the
    // default matcher just as Surface.linkAtPos does.
    auto trackedOsc8 = adapter.trackHyperlinkAt(column, row);
    if (trackedOsc8.has_value()) {
        TrackedTerminalLink target(
            std::in_place_type<GhosttyVtAdapter::TrackedHyperlink>,
            std::move(*trackedOsc8));
        auto resolved = resolveTrackedTerminalLink(adapter, target, viewport);
        if (resolved.has_value()) {
            return DetectedTerminalLink{
                .tracked = std::move(target),
                .resolved = std::move(*resolved),
            };
        }
    }

    if (!linkUrl || matcher == nullptr || !matcher->isValid()) {
        return std::nullopt;
    }
    auto line = adapter.snapshotLogicalLineAt(column, row);
    if (!line.has_value()) {
        return std::nullopt;
    }
    GhosttyLinkMatch byteMatch;
    bool foundTargetMatch = false;
    qsizetype searchOffset = 0;
    while (searchOffset < line->text().size()) {
        const GhosttyLinkMatchResult result =
            matcher->findNext(line->text(), searchOffset, &byteMatch);
        if (result != GhosttyLinkMatchResult::Match) {
            break;
        }
        if (line->byteRangeContainsTarget(byteMatch.beginByte,
                                          byteMatch.endByte)) {
            foundTargetMatch = true;
            break;
        }
        searchOffset = byteMatch.endByte;
    }
    if (!foundTargetMatch) {
        return std::nullopt;
    }
    auto tracked =
        adapter.trackTextRange(*line, byteMatch.beginByte, byteMatch.endByte);
    if (!tracked.has_value()) {
        return std::nullopt;
    }
    TrackedTerminalLink target(
        std::in_place_type<GhosttyVtAdapter::TrackedTextRange>,
        std::move(*tracked));
    auto resolved = resolveTrackedTerminalLink(adapter, target, viewport);
    if (!resolved.has_value()) {
        return std::nullopt;
    }
    return DetectedTerminalLink{
        .tracked = std::move(target),
        .resolved = std::move(*resolved),
    };
}

} // namespace

struct SessionWorker::HyperlinkState {
    struct PendingQuery {
        quint64 requestId = 0;
        quint64 contentRevision = 0;
        int column = -1;
        int row = -1;
    };

    std::optional<PendingQuery> pendingQuery;
    bool queryDispatchScheduled = false;
    quint64 activeRequestId = 0;
    std::optional<TrackedTerminalLink> trackedHover;
    TerminalHyperlinkState publishedState = TerminalHyperlinkState::Invalid;
    TerminalLinkKind publishedKind = TerminalLinkKind::Osc8;
    QByteArray publishedUri;
    QPoint publishedTarget{-1, -1};
    QVector<QPoint> publishedCells;
    QVector<int> publishedRelevantRows;
    int publishedColumns = 0;
    int publishedRows = 0;

    quint64 activationRequestId = 0;
    TerminalLinkKind activationKind = TerminalLinkKind::Osc8;
    std::optional<TrackedTerminalLink> trackedActivation;

    TerminalOsc8Index viewport;
};

struct SessionWorker::SearchState {
    quint64 generation = 0;
    quint64 dataRevision = 0;
    QByteArray needle;
    QByteArray reversedNeedle;
    QVector<qsizetype> prefix;
    qsizetype matched = 0;
    std::deque<TerminalSearchCell> recentCells;
    QVector<TerminalSearchRange> matches;
    QBitArray visibleCellMask;
    QBitArray selectedCellMask;
    qint64 selectedMatch = -1;
    qint64 nextScreenRow = -1;
    std::optional<GhosttyVtAdapter::SearchRowSnapshot> currentRow;
    qsizetype currentRowByteIndex = 0;
    bool currentRowBoundaryPending = false;
    TerminalSearchCell currentRowBoundaryCell;
    bool currentRowLogicalHasText = false;
    bool newerLogicalContent = false;
    bool finalBoundaryObserved = false;
    bool finalBoundaryPending = false;
    TerminalSearchCell finalBoundaryCell;
    quint64 scannedRows = 0;
    quint64 totalRows = 0;
    quint64 rowsSinceCompressionPass = 0;
    int columns = 0;
    int rows = 0;
    quint64 viewportOffset = 0;
    quint64 viewportLength = 0;
    GhosttyVtAdapter::SearchScreen activeScreen =
        GhosttyVtAdapter::SearchScreen::Primary;
    bool active = false;
    bool complete = false;
    bool seenFormattedContent = false;
    bool chunkScheduled = false;
    QElapsedTimer lastPublication;
};

SessionWorker::SessionWorker(QObject *parent)
    : SessionWorker(
          [](qint64 pid, const LinuxCgroupConfig &config,
             bool singleInstance) -> std::expected<void, QString> {
              if (pid <= 0
                  || static_cast<quint64>(pid)
                      > std::numeric_limits<quint32>::max()) {
                  return std::unexpected(
                      QStringLiteral("The child PID is outside uint32 range"));
              }
              return moveProcessToLinuxCgroup(static_cast<quint32>(pid), config,
                                              singleInstance);
          },
          parent)
{}

SessionWorker::SessionWorker(LinuxCgroupMover cgroupMover, QObject *parent)
    : QObject(parent)
    , cgroupMover_(std::move(cgroupMover))
    , hyperlinkState_(std::make_unique<HyperlinkState>())
    , searchState_(std::make_unique<SearchState>())
{
    Q_ASSERT(cgroupMover_);
}

SessionWorker::~SessionWorker()
{
    shutdown();
}

bool SessionWorker::canonicalBytesMayStartProcess(const QByteArray &payload)
{
    const std::optional<QByteArray> decoded =
        decodeGhosttyActionString(payload);
    return decoded.has_value() && bytesMayStartProcess(*decoded);
}

bool SessionWorker::canonicalTextMayStartProcess(const QByteArray &payload)
{
    const std::optional<QByteArray> actionPayload =
        decodeGhosttyActionString(payload);
    if (!actionPayload.has_value()) {
        return false;
    }
    const std::optional<QByteArray> text =
        decodeGhosttyConfigString(*actionPayload);
    return text.has_value() && bytesMayStartProcess(*text);
}

bool SessionWorker::isAbnormalCommandExit(
    int exitCode, int signalNumber, quint64 runtimeMilliseconds,
    quint32 thresholdMilliseconds) noexcept
{
    return (exitCode != 0 || signalNumber != 0)
        && runtimeMilliseconds <= thresholdMilliseconds;
}

bool SessionWorker::initialize(const TerminalSessionLaunchOptions &options,
                               InitializationObserver observer)
{
    if (vt_ != nullptr || running_) {
        if (observer) observer(false);
        return false;
    }

    options_ = options;
    if (options.initialGeometry.has_value()) {
        const TerminalSessionGeometry &geometry = *options.initialGeometry;
        // With neither libghostty nor a PTY created yet, the normal resize
        // path is a side-effect-free geometry seed and keeps all bounds in
        // one place.
        resizeTerminal(geometry);
    }
    shuttingDown_ = false;
    waitingForExitKey_ = false;
    semanticPromptObserved_ = false;
    semanticPromptExpected_ = false;
    childRuntimeTimer_.invalidate();
    potentialActivityTimer_.invalidate();
    cursorBlinkResetTimer_.invalidate();
    cursorBlinkResetPending_ = false;
    stagedSequenceBytes_.clear();
    stagedSequenceModifiers_ = Qt::NoModifier;
    newestSequenceToken_ = 0;
    activeSequenceToken_ = 0;
    stagedSequencePotentialActivity_ = false;
    stagedSequenceTraceResults_.clear();
    heldTerminalModifiers_ = Qt::NoModifier;
    terminalContentRevision_ = 1;
    searchContentRevision_ = 1;
    publishedContentRevision_ = 0;
    *searchState_ = SearchState{};
    hyperlinkState_->pendingQuery.reset();
    hyperlinkState_->queryDispatchScheduled = false;
    hyperlinkState_->activeRequestId = 0;
    hyperlinkState_->trackedHover.reset();
    hyperlinkState_->publishedState = TerminalHyperlinkState::Invalid;
    hyperlinkState_->publishedKind = TerminalLinkKind::Osc8;
    hyperlinkState_->publishedUri.clear();
    hyperlinkState_->publishedTarget = QPoint(-1, -1);
    hyperlinkState_->publishedCells.clear();
    hyperlinkState_->publishedRelevantRows.clear();
    hyperlinkState_->publishedColumns = 0;
    hyperlinkState_->publishedRows = 0;
    hyperlinkState_->activationRequestId = 0;
    hyperlinkState_->activationKind = TerminalLinkKind::Osc8;
    hyperlinkState_->trackedActivation.reset();
    hyperlinkState_->viewport.clear();

    linkMatcher_ = std::make_unique<GhosttyLinkMatcher>();

    frameTimer_ = new QTimer(this);
    frameTimer_->setSingleShot(true);
    frameTimer_->setInterval(kFrameCoalesceMilliseconds);
    connect(frameTimer_, &QTimer::timeout, this, &SessionWorker::publishFrame);

    childTimer_ = new QTimer(this);
    childTimer_->setInterval(100);
    connect(childTimer_, &QTimer::timeout, this, &SessionWorker::checkChild);

    compressionTimer_ = new QTimer(this);
    compressionTimer_->setObjectName(
        QStringLiteral("scrollbackCompressionTimer"));
    compressionTimer_->setSingleShot(true);
    compressionTimer_->setInterval(kCompressionIdleMilliseconds);
    connect(compressionTimer_, &QTimer::timeout, this,
            &SessionWorker::compressScrollback);

    selectionAutoscrollTimer_ = new QTimer(this);
    selectionAutoscrollTimer_->setObjectName(
        QStringLiteral("selectionAutoscrollTimer"));
    selectionAutoscrollTimer_->setTimerType(Qt::PreciseTimer);
    selectionAutoscrollTimer_->setInterval(kSelectionAutoscrollMilliseconds);
    connect(selectionAutoscrollTimer_, &QTimer::timeout, this,
            &SessionWorker::selectionAutoscrollTick);

    if (!createTerminal()) {
        if (observer) observer(false);
        Q_EMIT errorOccurred(
            QStringLiteral("Failed to initialize libghostty-vt."));
        Q_EMIT sessionExited(127, 0, options_.hold, false, 0, false);
        return false;
    }

    if (observer) observer(true);
    publishFrame();
    if (!spawnChild()) {
        Q_EMIT sessionExited(127, 0, options_.hold, false, 0, false);
    }
    return true;
}

bool SessionWorker::createTerminal()
{
    const GhosttyVtAdapter::Options options{
        .geometry = geometry_,
        .scrollbackBytes = options_.scrollbackLimits.bytes,
        .scrollbackLines = options_.scrollbackLimits.lines,
        .kittyImageStorageLimitBytes =
            options_.runtime.kittyImageStorageLimitBytes,
        .appearance = options_.runtime.appearance,
        .colorScheme = options_.runtime.colorScheme,
        .clipboardWriteAccess = options_.runtime.clipboardWrite,
        .initialWorkingDirectory = options_.inheritWorkingDirectory
            ? std::nullopt
            : std::optional(options_.workingDirectory.bytes()),
        .enquiryResponse = options_.runtime.enquiryResponse,
    };
    GhosttyVtAdapter::Callbacks callbacks;
    callbacks.writePty = [this](const QByteArray &data) {
        queuePtyWrite(data);
    };
    vt_ = GhosttyVtAdapter::create(options, std::move(callbacks));
    if (vt_ != nullptr) {
        if (!vt_->setSelectionWordChars(options_.runtime.selectionWordChars)
            || !vt_->setClickRepeatIntervalMilliseconds(
                options_.runtime.clickRepeatIntervalMilliseconds)) {
            vt_.reset();
            return false;
        }
        compressionActivity_ = vt_->compressionActivity();
        compressionTraversalPending_ = false;
        compressionReplayPending_ = false;
    }
    return vt_ != nullptr;
}

void SessionWorker::applyRuntimeOptions(
    const TerminalSessionRuntimeOptions &options)
{
    const bool compressionWasEnabled = options_.runtime.scrollbackCompression;
    const bool scrollToBottomOutputChanged =
        options_.runtime.scrollToBottom.output != options.scrollToBottom.output;
    const bool appearanceChanged =
        options_.runtime.appearance != options.appearance;
    const bool kittyImageStorageLimitChanged =
        options_.runtime.kittyImageStorageLimitBytes
        != options.kittyImageStorageLimitBytes;
    const quint32 previousKittyImageStorageLimit =
        options_.runtime.kittyImageStorageLimitBytes;
    const bool linkUrlChanged = options_.runtime.linkUrl != options.linkUrl;
    const QVector<quint32> previousSelectionWordChars =
        options_.runtime.selectionWordChars;
    const bool selectionWordCharsChanged =
        previousSelectionWordChars != options.selectionWordChars;
    const quint32 previousClickRepeatInterval =
        options_.runtime.clickRepeatIntervalMilliseconds;
    const bool clickRepeatIntervalChanged =
        previousClickRepeatInterval != options.clickRepeatIntervalMilliseconds;
    const bool selectionWordCharsApplied = vt_ == nullptr
        || !selectionWordCharsChanged
        || vt_->setSelectionWordChars(options.selectionWordChars);
    const bool clickRepeatIntervalApplied = vt_ == nullptr
        || !clickRepeatIntervalChanged
        || vt_->setClickRepeatIntervalMilliseconds(
            options.clickRepeatIntervalMilliseconds);
    const bool kittyImageStorageLimitApplied = vt_ == nullptr
        || !kittyImageStorageLimitChanged
        || vt_->setKittyImageStorageLimit(options.kittyImageStorageLimitBytes);
    if (vt_ != nullptr) {
        vt_->setColorScheme(options.colorScheme);
        vt_->setClipboardWriteAccess(options.clipboardWrite);
        vt_->setEnquiryResponse(options.enquiryResponse);
    }
    options_.runtime = options;
    if (vt_ != nullptr && scrollToBottomOutputChanged) {
        // Ghostty's output policy lives in the renderer. A live change marks
        // that renderer dirty, so enabling the policy immediately compares
        // its last observed bottom anchor with the active screen.
        scheduleFrame();
    }

    if (compressionWasEnabled && !options_.runtime.scrollbackCompression) {
        if (compressionTimer_ != nullptr) {
            compressionTimer_->stop();
        }
        compressionTraversalPending_ = false;
        compressionReplayPending_ = false;
    } else if (!compressionWasEnabled
               && options_.runtime.scrollbackCompression) {
        // Match Ghostty's null activity marker on re-enable: pages made
        // resident while compression was disabled must be reconsidered even
        // when no subsequent terminal mutation changes the activity token.
        compressionActivity_.reset();
        noteCompressionActivity();
        // The PageList-owned incremental cursor can survive a disabled
        // interval. Force one fresh traversal after it next completes so
        // reads performed while disabled cannot remain behind that cursor.
        compressionReplayPending_ = true;
    }

    if (linkUrlChanged && !options_.runtime.linkUrl) {
        if (hyperlinkState_->trackedHover.has_value()
            && std::holds_alternative<GhosttyVtAdapter::TrackedTextRange>(
                *hyperlinkState_->trackedHover)) {
            const quint64 requestId = hyperlinkState_->activeRequestId;
            hyperlinkState_->activeRequestId = 0;
            hyperlinkState_->trackedHover.reset();
            hyperlinkState_->publishedState = TerminalHyperlinkState::Invalid;
            hyperlinkState_->publishedKind = TerminalLinkKind::Regex;
            hyperlinkState_->publishedUri.clear();
            hyperlinkState_->publishedTarget = QPoint(-1, -1);
            hyperlinkState_->publishedCells.clear();
            hyperlinkState_->publishedRelevantRows.clear();
            hyperlinkState_->publishedColumns = 0;
            hyperlinkState_->publishedRows = 0;
            if (requestId != 0) {
                Q_EMIT hyperlinkResolved(requestId, terminalContentRevision_,
                                         TerminalHyperlinkState::Invalid,
                                         TerminalLinkKind::Regex, {},
                                         QPoint(-1, -1), {});
            }
        }
        if (hyperlinkState_->trackedActivation.has_value()
            && std::holds_alternative<GhosttyVtAdapter::TrackedTextRange>(
                *hyperlinkState_->trackedActivation)) {
            hyperlinkState_->activationRequestId = 0;
            hyperlinkState_->activationKind = TerminalLinkKind::Osc8;
            hyperlinkState_->trackedActivation.reset();
        }
    }

    // libghostty-vt cannot resize an existing scrollback allocation. Reloaded
    // limits remain workspace-owned and apply when a new pane is constructed.
    if (!selectionWordCharsApplied) {
        options_.runtime.selectionWordChars = previousSelectionWordChars;
        Q_EMIT errorOccurred(QStringLiteral(
            "Failed to apply selection word boundaries to libghostty-vt."));
    }
    if (!clickRepeatIntervalApplied) {
        options_.runtime.clickRepeatIntervalMilliseconds =
            previousClickRepeatInterval;
        Q_EMIT errorOccurred(QStringLiteral(
            "Failed to apply click repeat interval to libghostty-vt."));
    }
    if (!kittyImageStorageLimitApplied) {
        options_.runtime.kittyImageStorageLimitBytes =
            previousKittyImageStorageLimit;
        Q_EMIT errorOccurred(QStringLiteral(
            "Failed to apply Kitty image storage limit to libghostty-vt."));
    } else if (vt_ != nullptr && kittyImageStorageLimitChanged) {
        markTerminalContentChanged();
        scheduleFrame();
    }
    if (vt_ != nullptr && appearanceChanged) {
        if (!vt_->setAppearance(options.appearance)) {
            Q_EMIT errorOccurred(QStringLiteral(
                "Failed to apply terminal appearance to libghostty-vt."));
            return;
        }
        scheduleFrame();
    }
}

bool SessionWorker::spawnChild()
{
    // Resolve every source before creating launch pipes or a child. Keeping
    // the ordered chunks distinct avoids a second aggregate allocation while
    // still making the operation all-or-nothing.
    std::expected<QVector<QByteArray>, QString> initialInput =
        prepareInitialInput(std::exchange(options_.initialInput, {}));
    if (!initialInput.has_value()) {
        Q_EMIT errorOccurred(initialInput.error());
        return false;
    }

    const QByteArray processWorkingDirectory =
        QFile::encodeName(QDir::currentPath());
    const QByteArray &requestedWorkingDirectory =
        options_.workingDirectory.bytes();
    QByteArray childWorkingDirectory = processWorkingDirectory;
    bool attemptWorkingDirectory = false;
    if (!options_.inheritWorkingDirectory) {
        if (requestedWorkingDirectory.contains('\0')) {
            Q_EMIT errorOccurred(QStringLiteral(
                "Configured working directory contains a NUL byte and cannot be passed to POSIX APIs."));
            return false;
        }
        // Pinned Ghostty drops a missing cwd but passes any existing path to
        // the child, where chdir failure is deliberately non-fatal.
        attemptWorkingDirectory = !requestedWorkingDirectory.isEmpty()
            && ::access(requestedWorkingDirectory.constData(), F_OK) == 0;
        struct stat directoryStatus{};
        if (attemptWorkingDirectory
            && ::stat(requestedWorkingDirectory.constData(), &directoryStatus)
                == 0
            && S_ISDIR(directoryStatus.st_mode)
            && ::access(requestedWorkingDirectory.constData(), X_OK) == 0) {
            // This expected effective directory is used only to decide
            // whether the legacy SHELL fallback should apply. The child still
            // attempts the exact requested spelling below.
            childWorkingDirectory = requestedWorkingDirectory;
        }
    }

    TerminalCommand launchCommand;
    bool legacyInteractiveFallback = false;
    if (!options_.program.isEmpty()) {
        QVector<QByteArray> arguments;
        arguments.reserve(options_.program.size());
        for (const QString &argument : options_.program) {
            arguments.push_back(argument.toLocal8Bit());
        }
        launchCommand = TerminalCommand::direct(std::move(arguments));
        interactiveShell_ = false;
    } else if (options_.command.has_value()) {
        launchCommand = *options_.command;
        interactiveShell_ = launchCommand.defaultShell;
    } else {
        // Generic callers without a finalized Ghostty snapshot retain the
        // frontend's historical interactive-shell fallback. Config projection
        // materializes Ghostty's distinct shell-form `sh` fallback.
        QByteArray executable = qgetenv("SHELL");
        if (executable.isEmpty()) executable = QByteArrayLiteral("/bin/sh");
        launchCommand =
            TerminalCommand::direct({executable, QByteArrayLiteral("-i")});
        interactiveShell_ = true;
        legacyInteractiveFallback = true;
    }

    const TerminfoResolution terminfo = resolveRuntimeTerminfoDirectory();
    if (!terminfo) {
        Q_EMIT errorOccurred(terminfo.error());
        return false;
    }
    const bool environmentOverridesTerm = std::ranges::any_of(
        options_.environment, [](const TerminalEnvironmentEntry &entry) {
            return entry.key == QByteArrayLiteral("TERM");
        });
    // Pinned Ghostty applies env_override after injecting the finalized term.
    // An exact TERM entry therefore makes only that later value observable.
    if (!environmentOverridesTerm
        && (options_.term.isEmpty() || options_.term.contains('\0'))) {
        Q_EMIT errorOccurred(
            QStringLiteral("Configured TERM must be non-empty and contain no "
                           "NUL bytes."));
        return false;
    }
    for (qsizetype index = 0; index < options_.environment.size(); ++index) {
        const TerminalEnvironmentEntry &entry = options_.environment.at(index);
        if (entry.key.contains('=') || entry.key.contains('\0')
            || entry.value.isEmpty() || entry.value.contains('\0')) {
            Q_EMIT errorOccurred(
                QStringLiteral(
                    "Configured environment entry %1 is not representable by execve.")
                    .arg(index));
            return false;
        }
        for (qsizetype previous = 0; previous < index; ++previous) {
            if (options_.environment.at(previous).key == entry.key) {
                Q_EMIT errorOccurred(
                    QStringLiteral(
                        "Configured environment contains duplicate key at entry %1.")
                        .arg(index));
                return false;
            }
        }
    }

    TerminalEnvironment childEnvironment = inheritedTerminalEnvironment();
    // Apply frontend-injected entries directly to the byte representation so
    // Ghostty's raw finalized TERM does not cross QString. Configured env
    // entries then replace these and inherited values byte-for-byte, matching
    // pinned Ghostty's late env_override merge.
    setEnvironmentEntry(childEnvironment, QByteArrayLiteral("TERM"),
                        options_.term);
    setEnvironmentEntry(childEnvironment, QByteArrayLiteral("TERMINFO"),
                        terminfo->toLocal8Bit());
    setEnvironmentEntry(childEnvironment, QByteArrayLiteral("COLORTERM"),
                        QByteArrayLiteral("truecolor"));
    setEnvironmentEntry(childEnvironment, QByteArrayLiteral("TERM_PROGRAM"),
                        QByteArrayLiteral("ghostty-qt"));
    setEnvironmentEntry(childEnvironment,
                        QByteArrayLiteral("TERM_PROGRAM_VERSION"),
                        QByteArrayLiteral(GHOSTTY_QT_VERSION));
    removeEnvironmentEntry(childEnvironment, QByteArrayLiteral("VTE_VERSION"));

    if (options_.shellIntegrationAvailable) {
        const QByteArray executableDirectory =
            QFile::encodeName(QCoreApplication::applicationDirPath());
        setEnvironmentEntry(childEnvironment,
                            QByteArrayLiteral("GHOSTTY_BIN_DIR"),
                            executableDirectory);
        appendExecutableDirectory(childEnvironment, executableDirectory);

        QByteArray resourceDirectory;
        const std::expected<QString, QString> resources =
            resolveRuntimeShellIntegrationResourceDirectory();
        if (resources.has_value()) {
            resourceDirectory = QFile::encodeName(*resources);
            setEnvironmentEntry(childEnvironment,
                                QByteArrayLiteral("GHOSTTY_RESOURCES_DIR"),
                                resourceDirectory);
        } else {
            qWarning().noquote()
                << "Ghostty shell-integration resources are unavailable:"
                << resources.error();
        }

        // Ghostty's `-e` finalization downgrades every non-none forced mode to
        // detection so an explicit program cannot accidentally receive, for
        // example, ZDOTDIR intended for a configured zsh. The Qt positional
        // program is held outside the config helper, so reproduce that rule
        // at this boundary.
        const GhosttyShellIntegrationMode integrationMode =
            !options_.program.isEmpty()
                && options_.shellIntegration
                    != GhosttyShellIntegrationMode::None
            ? GhosttyShellIntegrationMode::Detect
            : options_.shellIntegration;
        const GhosttyShellIntegrationRequest request{
            .command = launchCommand,
            .environment = childEnvironment,
            .mode = integrationMode,
            .features = options_.shellIntegrationFeatures,
            .cursorBlink =
                options_.runtime.appearance.cursorBlink.value_or(true),
            .resourceDirectory = resourceDirectory,
        };
        const QString applicationDirectory =
            QCoreApplication::applicationDirPath();
        QString helperPath =
            QDir(applicationDirectory)
                .filePath(QStringLiteral("ghostty-qt-config-helper"));
        // CTest executables live one directory below the application and its
        // helper. This fallback is harmless for installations (where the
        // sibling exists) and gives worker-level integration tests the exact
        // production helper rather than a protocol fake.
        if (!QFileInfo::exists(helperPath)) {
            helperPath =
                QDir(applicationDirectory)
                    .filePath(QStringLiteral("../ghostty-qt-config-helper"));
        }
        const std::expected<GhosttyShellIntegrationResult, QString> prepared =
            prepareGhosttyShellIntegration(
                {
                    .helperPath = helperPath,
                    .environment = QProcessEnvironment::systemEnvironment(),
                    .timeoutMilliseconds = 2'000,
                },
                request);
        if (prepared.has_value()) {
            semanticPromptExpected_ =
                interactiveShell_ && prepared->shell.has_value();
            launchCommand = prepared->command;
            childEnvironment = prepared->environment;
        } else {
            // Integration is an enhancement, never a reason to strand a pane
            // at startup. Retain the unmodified command/environment while
            // making the helper boundary failure visible to developers.
            qWarning().noquote()
                << "Ghostty shell integration could not be prepared:"
                << prepared.error();
        }
    }

    for (const TerminalEnvironmentEntry &entry : options_.environment) {
        setEnvironmentEntry(childEnvironment, entry.key, entry.value);
    }
    if (!options_.inheritWorkingDirectory) {
        // Pinned Ghostty applies concrete cwd-derived PWD after env overrides,
        // retaining the requested logical spelling even when chdir falls back.
        setEnvironmentEntry(childEnvironment, QByteArrayLiteral("PWD"),
                            requestedWorkingDirectory);
    }

    QVector<QByteArray> argumentStorage;
    switch (launchCommand.kind) {
    case TerminalCommandKind::Shell:
        argumentStorage = {
            QByteArrayLiteral("/bin/sh"),
            QByteArrayLiteral("-c"),
            launchCommand.shellCommand,
        };
        break;
    case TerminalCommandKind::Direct:
        argumentStorage = launchCommand.directArguments;
        break;
    }
    if (argumentStorage.isEmpty()) {
        Q_EMIT errorOccurred(
            QStringLiteral("Configured direct command has no argv entries."));
        return false;
    }
    for (qsizetype index = 0; index < argumentStorage.size(); ++index) {
        if (argumentStorage.at(index).contains('\0')) {
            Q_EMIT errorOccurred(
                QStringLiteral(
                    "Command argument %1 contains a NUL byte and cannot be passed to execve.")
                    .arg(index));
            return false;
        }
    }

    QByteArray requestedExecutable = argumentStorage.constFirst();
    QVector<QByteArray> executablePaths =
        executableCandidates(requestedExecutable);
    // Do not preselect a candidate: the child attempts the complete ordered
    // list below. The availability probe exists only for the frontend's
    // legacy invalid-SHELL fallback.
    const bool executableAvailable =
        hasExecutableCandidate(executablePaths, childWorkingDirectory);
    if (legacyInteractiveFallback && !executableAvailable
        && requestedExecutable != QByteArrayLiteral("/bin/sh")) {
        requestedExecutable = QByteArrayLiteral("/bin/sh");
        argumentStorage[0] = requestedExecutable;
        executablePaths = executableCandidates(requestedExecutable);
    }

    QVector<QByteArray> executableStorage = std::move(executablePaths);
    QVector<char *> argv;
    argv.reserve(argumentStorage.size() + 1);
    for (QByteArray &argument : argumentStorage) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    QVector<QByteArray> environmentStorage;
    environmentStorage.reserve(childEnvironment.size());
    for (const TerminalEnvironmentEntry &entry : childEnvironment) {
        QByteArray serialized;
        serialized.reserve(entry.key.size() + entry.value.size() + 1);
        serialized.append(entry.key);
        serialized.append('=');
        serialized.append(entry.value);
        environmentStorage.push_back(std::move(serialized));
    }
    QVector<char *> envp;
    envp.reserve(environmentStorage.size() + 1);
    for (QByteArray &entry : environmentStorage) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);
    char *const *const argvData = argv.constData();
    char *const *const envpData = envp.constData();

    struct winsize size{};
    size.ws_col = boundedU16(geometry_.columns);
    size.ws_row = boundedU16(geometry_.rows);
    size.ws_xpixel = boundedPixelU16(geometry_.terminalWidthPixels());
    size.ws_ypixel = boundedPixelU16(geometry_.terminalHeightPixels());

    bool gateChild = linuxCgroupEnabled(options_.linuxCgroup.mode,
                                        options_.processUsesSingleInstance);
    std::array<int, 2> cgroupGate{-1, -1};
    if (gateChild
        && ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0,
                        cgroupGate.data())
            < 0) {
        const QString message =
            QStringLiteral("Unable to create cgroup launch gate: %1")
                .arg(QString::fromLocal8Bit(std::strerror(errno)));
        if (options_.linuxCgroup.hardFail) {
            Q_EMIT errorOccurred(message);
            return false;
        }
        qWarning().noquote()
            << message
            << QStringLiteral(
                   "(continuing because linux-cgroup-hard-fail is false)");
        gateChild = false;
    }

    std::array<int, 2> childReadyPipe{-1, -1};
    if (::pipe2(childReadyPipe.data(), O_CLOEXEC) < 0) {
        if (gateChild) {
            (void)::close(cgroupGate[0]);
            (void)::close(cgroupGate[1]);
        }
        Q_EMIT errorOccurred(
            QStringLiteral("Unable to create child-ready pipe: %1")
                .arg(QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }

    int ptyFd = -1;
    const pid_t pid = ::forkpty(&ptyFd, nullptr, nullptr, &size);
    if (pid < 0) {
        (void)::close(childReadyPipe[0]);
        (void)::close(childReadyPipe[1]);
        if (gateChild) {
            (void)::close(cgroupGate[0]);
            (void)::close(cgroupGate[1]);
        }
        Q_EMIT errorOccurred(
            QStringLiteral("Unable to create PTY: %1")
                .arg(QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }

    if (pid == 0) {
        (void)::close(childReadyPipe[0]);
        if (gateChild) (void)::close(cgroupGate[0]);
        struct sigaction defaultAction{};
        defaultAction.sa_handler = SIG_DFL;
        if (::sigemptyset(&defaultAction.sa_mask) != 0
            || ::sigaction(SIGHUP, &defaultAction, nullptr) != 0) {
            _exit(126);
        }

        // forkpty returns to the parent before the child necessarily finishes
        // login_tty. Publish readiness only after SIGHUP can no longer invoke
        // an inherited Qt/application handler during an immediate close.
        constexpr char ready = 1;
        ssize_t written = -1;
        do {
            written = ::write(childReadyPipe[1], &ready, sizeof(ready));
        } while (written < 0 && errno == EINTR);
        (void)::close(childReadyPipe[1]);
        if (written != 1) _exit(126);

        if (gateChild) {
            char launchDecision = 0;
            ssize_t decisionBytes = -1;
            do {
                decisionBytes = ::read(cgroupGate[1], &launchDecision,
                                       sizeof(launchDecision));
            } while (decisionBytes < 0 && errno == EINTR);
            (void)::close(cgroupGate[1]);
            if (decisionBytes != 1 || launchDecision != 1) _exit(127);
        }

        if (attemptWorkingDirectory) {
            // Ghostty treats the directory as a hint. An existing file,
            // permission failure, or post-preflight race must not prevent the
            // command from starting in the inherited process directory.
            (void)::chdir(requestedWorkingDirectory.constData());
        }
        // Search after chdir, just like pinned Zig's execvpeZ. A stat-only
        // parent lookup cannot detect an executable whose interpreter is
        // missing, so try every candidate and preserve its fallback rules.
        bool sawAccessDenied = false;
        for (const QByteArray &path : std::as_const(executableStorage)) {
            ::execve(path.constData(), argvData, envpData);
            const int execError = errno;
            if (execError == EACCES) {
                sawAccessDenied = true;
                continue;
            }
            if (execError == ENOENT || execError == ENOTDIR) {
                continue;
            }
            _exit(126);
        }
        _exit(sawAccessDenied ? 126 : 127);
    }

    (void)::close(childReadyPipe[1]);
    if (gateChild) (void)::close(cgroupGate[1]);
    char ready = 0;
    ssize_t received = -1;
    do {
        received = ::read(childReadyPipe[0], &ready, sizeof(ready));
    } while (received < 0 && errno == EINTR);
    (void)::close(childReadyPipe[0]);
    if (received != 1 || ready != 1) {
        if (gateChild) (void)::close(cgroupGate[0]);
        (void)::close(ptyFd);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        Q_EMIT errorOccurred(
            QStringLiteral("Child exited before completing PTY setup"));
        return false;
    }

    if (gateChild) {
        const std::expected<void, QString> moved =
            cgroupMover_(static_cast<qint64>(pid), options_.linuxCgroup,
                         options_.processUsesSingleInstance);
        const bool permitLaunch =
            moved.has_value() || !options_.linuxCgroup.hardFail;
        if (!moved.has_value()) {
            const QString message =
                QStringLiteral(
                    "Could not isolate terminal child in a transient systemd scope: %1")
                    .arg(moved.error());
            if (options_.linuxCgroup.hardFail) {
                Q_EMIT errorOccurred(message);
            } else {
                qWarning().noquote()
                    << message
                    << QStringLiteral(
                           "(continuing because linux-cgroup-hard-fail is false)");
            }
        }

        const char decision = permitLaunch ? 1 : 0;
        const ssize_t sent =
            writeWithoutSigpipe(cgroupGate[0], &decision, sizeof(decision));
        const int sendError = sent < 0 ? errno : EIO;
        (void)::close(cgroupGate[0]);

        if (sent != 1 || !permitLaunch) {
            if (sent != 1) {
                Q_EMIT errorOccurred(
                    QStringLiteral("Unable to release cgroup launch gate: %1")
                        .arg(QString::fromLocal8Bit(std::strerror(sendError))));
            }
            (void)::close(ptyFd);
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            return false;
        }
    }

    masterFd_ = ptyFd;
    childPid_ = static_cast<qint64>(pid);
    running_ = true;
    childExitFd_ = openChildExitFd(pid);
    if (childExitFd_ >= 0) {
        childExitNotifier_ =
            new QSocketNotifier(childExitFd_, QSocketNotifier::Read, this);
        connect(childExitNotifier_, &QSocketNotifier::activated, this,
                &SessionWorker::checkChild);
    }
    // Match Ghostty's runtime provenance: begin after the parent has a
    // watchable, successfully launched child. pidfd readiness minimizes the
    // observation latency; the existing timer remains the kernel fallback.
    childRuntimeTimer_.start();
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
    connect(readNotifier_, &QSocketNotifier::activated, this,
            &SessionWorker::readFromPty);
    writeNotifier_ =
        new QSocketNotifier(masterFd_, QSocketNotifier::Write, this);
    writeNotifier_->setEnabled(false);
    connect(writeNotifier_, &QSocketNotifier::activated, this,
            &SessionWorker::flushPtyWrites);

    // Initial input precedes `started`, so direct signal handlers and queued
    // GUI input cannot overtake it. queuePtyWrite bypasses the runtime
    // read-only policy because these bytes are launch configuration.
    for (const QByteArray &chunk : std::as_const(*initialInput)) {
        queuePtyWrite(chunk);
    }

    childTimer_->start();
    if (!options_.inheritWorkingDirectory) {
        Q_EMIT currentDirectoryChanged(requestedWorkingDirectory);
    }
    Q_EMIT started(childPid_);
    return true;
}

void SessionWorker::resizeTerminal(const TerminalSessionGeometry &geometry)
{
    const TerminalSessionGeometry normalized =
        normalizedTerminalSessionGeometry(geometry);

    if (vt_ != nullptr) {
        if (!vt_->resize(normalized)) {
            Q_EMIT errorOccurred(
                QStringLiteral("libghostty rejected the terminal resize."));
            return;
        }
        markTerminalContentChanged();
        processDeferredEffects();
        syncSelectionAvailability();
        noteCompressionActivity();
        scheduleFrame();
    }

    geometry_ = normalized;

    if (vt_ != nullptr) {
        markSearchContentChanged();
    }

    if (masterFd_ >= 0) {
        struct winsize size{};
        size.ws_col = boundedU16(geometry_.columns);
        size.ws_row = boundedU16(geometry_.rows);
        size.ws_xpixel = boundedPixelU16(geometry_.terminalWidthPixels());
        size.ws_ypixel = boundedPixelU16(geometry_.terminalHeightPixels());
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

    // read() initializes every byte exposed through the resulting view, so
    // zero-filling this 64 KiB buffer on every notifier activation is wasted.
    std::array<uint8_t, static_cast<size_t>(kReadBufferSize)> buffer;
    qsizetype totalRead = 0;
    bool receivedData = false;

    const qsizetype readLimit =
        finalDrain ? kMaximumFinalRead : kMaximumReadPerActivation;
    while (totalRead < readLimit) {
        const ssize_t count = ::read(masterFd_, buffer.data(), buffer.size());
        if (count > 0) {
            const auto size = static_cast<size_t>(count);
            vt_->writeVt(
                QByteArrayView(reinterpret_cast<const char *>(buffer.data()),
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
        Q_EMIT errorOccurred(
            QStringLiteral("PTY read failed: %1")
                .arg(QString::fromLocal8Bit(std::strerror(errno))));
        break;
    }

    if (receivedData) {
        markTerminalContentChanged();
        markSearchContentChanged();
        // VT input may change mouse tracking or encoding modes. Synchronize
        // once per output batch without resetting motion deduplication for
        // every individual pointer event.
        syncMouseEncoder();
        syncKeyboardActionMode();
        syncSelectionAvailability();
        processDeferredEffects();
        noteCompressionActivity();
        // OSC 133 prompt markers can classify shell builtins that never
        // create a new foreground process group. Re-evaluate immediately
        // after the complete output batch so a returned prompt clears the
        // close-confirmation state without waiting for the polling timer.
        updateProcessActivity();
        if (!cursorBlinkResetTimer_.isValid()
            || cursorBlinkResetTimer_.elapsed()
                > kCursorBlinkResetThrottleMilliseconds) {
            cursorBlinkResetTimer_.restart();
            cursorBlinkResetPending_ = true;
        }
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
    pendingWrites_.append(QByteArrayView(data));
    flushPtyWrites();
}

void SessionWorker::queueInputWrite(const QByteArray &data)
{
    if (!readOnly_) {
        queuePtyWrite(data);
    }
}

std::optional<TerminalKeyboardTraceResult>
SessionWorker::makeKeyboardTraceResult(
    const TerminalKeyInput &input, TerminalKeyboardTraceOperation operation,
    TerminalKeyboardTraceDisposition disposition, const QByteArray &encoded,
    quint64 sequenceToken) const
{
    if (input.inspectorTraceGeneration == 0 || input.inspectorTraceId == 0
        || input.inspectorTraceGeneration != keyboardTraceGeneration_) {
        return std::nullopt;
    }

    const qsizetype prefixLength = std::min(
        encoded.size(), TerminalKeyboardTraceResult::MaximumEncodedPrefix);
    return TerminalKeyboardTraceResult{
        .generation = input.inspectorTraceGeneration,
        .traceId = input.inspectorTraceId,
        .sequenceToken = sequenceToken,
        .operation = operation,
        .disposition = disposition,
        .encodedByteCount = encoded.size(),
        .encodedPrefix = encoded.first(prefixLength),
        .prefixTruncated = prefixLength != encoded.size(),
    };
}

void SessionWorker::publishKeyboardTraceResult(
    const TerminalKeyInput &input, TerminalKeyboardTraceOperation operation,
    TerminalKeyboardTraceDisposition disposition, const QByteArray &encoded,
    quint64 sequenceToken)
{
    const std::optional<TerminalKeyboardTraceResult> result =
        makeKeyboardTraceResult(input, operation, disposition, encoded,
                                sequenceToken);
    if (result.has_value()) Q_EMIT keyboardTraceResult(*result);
}

void SessionWorker::finalizeStagedKeyboardTraceResults(
    TerminalKeyboardTraceDisposition disposition)
{
    QVector<TerminalKeyboardTraceResult> results =
        std::move(stagedSequenceTraceResults_);
    stagedSequenceTraceResults_.clear();
    for (TerminalKeyboardTraceResult &result : results) {
        if (result.generation != keyboardTraceGeneration_) continue;
        result.operation = TerminalKeyboardTraceOperation::SequenceResolution;
        result.disposition = disposition;
        Q_EMIT keyboardTraceResult(result);
    }
}

void SessionWorker::setReadOnly(bool readOnly)
{
    readOnly_ = readOnly;
}

void SessionWorker::setKeyboardTraceGeneration(quint64 generation)
{
    if (keyboardTraceGeneration_ == generation) return;
    keyboardTraceGeneration_ = generation;
    // Actual leader bytes remain part of terminal input semantics, but copied
    // diagnostic metadata must never cross an inspector pause/close boundary.
    stagedSequenceTraceResults_.clear();
}

void SessionWorker::flushPtyWrites()
{
    while (masterFd_ >= 0 && !pendingWrites_.isEmpty()) {
        const QByteArrayView pending = pendingWrites_.bytes();
        const ssize_t count = ::write(masterFd_, pending.data(),
                                      static_cast<size_t>(pending.size()));
        if (count > 0) {
            pendingWrites_.consume(static_cast<qsizetype>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (count < 0) {
            Q_EMIT errorOccurred(
                QStringLiteral("PTY write failed: %1")
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
    if (keyboardInputSuppressed()) {
        publishKeyboardTraceResult(
            input, TerminalKeyboardTraceOperation::Key,
            TerminalKeyboardTraceDisposition::KeyboardActionMode);
        Q_EMIT inputActivityReconciled(activeProcess_);
        return;
    }
    if (!readOnly_ && masterFd_ >= 0 && input.pressed
        && (input.key == Qt::Key_Return || input.key == Qt::Key_Enter
            || input.text.contains(u'\n') || input.text.contains(u'\r'))) {
        notePotentialActivity();
    }
    if (vt_ == nullptr) {
        publishKeyboardTraceResult(
            input, TerminalKeyboardTraceOperation::Key,
            TerminalKeyboardTraceDisposition::TerminalUnavailable);
        return;
    }
    const GhosttyVtAdapter::EncodedKey encoded = vt_->encodeKey(input);
    if (!encoded.success) {
        publishKeyboardTraceResult(
            input, TerminalKeyboardTraceOperation::Key,
            TerminalKeyboardTraceDisposition::EncoderFailed);
        return;
    }
    if (encoded.bytes.isEmpty()) {
        publishKeyboardTraceResult(
            input, TerminalKeyboardTraceOperation::Key,
            TerminalKeyboardTraceDisposition::EncoderEmpty);
        return;
    }
    if (waitingForExitKey_ && input.pressed) {
        publishKeyboardTraceResult(
            input, TerminalKeyboardTraceOperation::Key,
            TerminalKeyboardTraceDisposition::ExitWaitConsumed, encoded.bytes);
        waitingForExitKey_ = false;
        Q_EMIT exitKeyDismissed();
        return;
    }
    if (masterFd_ < 0) {
        publishKeyboardTraceResult(
            input, TerminalKeyboardTraceOperation::Key,
            TerminalKeyboardTraceDisposition::SessionUnavailable,
            encoded.bytes);
        return;
    }
    queueInputWrite(encoded.bytes);
    publishKeyboardTraceResult(input, TerminalKeyboardTraceOperation::Key,
                               readOnly_
                                   ? TerminalKeyboardTraceDisposition::ReadOnly
                                   : TerminalKeyboardTraceDisposition::Queued,
                               encoded.bytes);
    if (!readOnly_) {
        heldTerminalModifiers_ = modifiersAfterTerminalKey(input);
    }
    clearSelectionAfterKey(encoded.modifier, encoded.escape);
    scrollToBottomForKeystroke(encoded.modifier);
}

void SessionWorker::stageSequenceKey(quint64 token,
                                     const TerminalKeyInput &input)
{
    if (token == 0) {
        publishKeyboardTraceResult(
            input, TerminalKeyboardTraceOperation::SequenceStage,
            TerminalKeyboardTraceDisposition::StaleSequence, {}, token);
        return;
    }
    if (token != activeSequenceToken_) {
        if (!sequenceTokenIsNewer(token, newestSequenceToken_)) {
            publishKeyboardTraceResult(
                input, TerminalKeyboardTraceOperation::SequenceStage,
                TerminalKeyboardTraceDisposition::StaleSequence, {}, token);
            return;
        }
        // A newer sequence supersedes an unresolved one. Its held bytes must
        // not leak into the new match attempt.
        finalizeStagedKeyboardTraceResults(
            TerminalKeyboardTraceDisposition::Superseded);
        newestSequenceToken_ = token;
        activeSequenceToken_ = token;
        stagedSequenceBytes_.clear();
        stagedSequenceModifiers_ = heldTerminalModifiers_;
        stagedSequencePotentialActivity_ = false;
    }

    if (vt_ == nullptr) {
        publishKeyboardTraceResult(
            input, TerminalKeyboardTraceOperation::SequenceStage,
            TerminalKeyboardTraceDisposition::TerminalUnavailable, {}, token);
        return;
    }
    const GhosttyVtAdapter::EncodedKey encoded = vt_->encodeKey(input);
    if (!encoded.success) {
        publishKeyboardTraceResult(
            input, TerminalKeyboardTraceOperation::SequenceStage,
            TerminalKeyboardTraceDisposition::EncoderFailed, {}, token);
        return;
    }
    if (encoded.bytes.isEmpty()) {
        publishKeyboardTraceResult(
            input, TerminalKeyboardTraceOperation::SequenceStage,
            TerminalKeyboardTraceDisposition::EncoderEmpty, {}, token);
        return;
    }
    stagedSequenceBytes_.append(encoded.bytes);
    stagedSequenceModifiers_ = modifiersAfterTerminalKey(input);
    stagedSequencePotentialActivity_ =
        stagedSequencePotentialActivity_ || keyMayStartProcess(input);
    const std::optional<TerminalKeyboardTraceResult> result =
        makeKeyboardTraceResult(
            input, TerminalKeyboardTraceOperation::SequenceStage,
            TerminalKeyboardTraceDisposition::Staged, encoded.bytes, token);
    if (result.has_value()) {
        stagedSequenceTraceResults_.append(*result);
        Q_EMIT keyboardTraceResult(*result);
    }
}

void SessionWorker::resolveSequence(quint64 token,
                                    TerminalSequenceResolution resolution,
                                    bool hasCurrent,
                                    const TerminalKeyInput &current)
{
    if (token == 0 || token != activeSequenceToken_) {
        publishKeyboardTraceResult(
            current, TerminalKeyboardTraceOperation::SequenceResolution,
            TerminalKeyboardTraceDisposition::StaleSequence, {}, token);
        return;
    }

    QByteArray bytes;
    bool potentialActivity = false;
    bool currentModifier = true;
    bool currentEscape = false;
    bool currentEncoded = false;
    QByteArray currentBytes;
    bool currentTracePublished = false;
    Qt::KeyboardModifiers modifiersAfterBytes = heldTerminalModifiers_;
    if (resolution == TerminalSequenceResolution::Flush
        || resolution == TerminalSequenceResolution::FlushAndSendCurrent) {
        bytes = std::move(stagedSequenceBytes_);
        modifiersAfterBytes = stagedSequenceModifiers_;
        potentialActivity =
            stagedSequencePotentialActivity_ && !bytes.isEmpty();
    }
    if (resolution == TerminalSequenceResolution::FlushAndSendCurrent
        && hasCurrent) {
        if (keyboardInputSuppressed()) {
            publishKeyboardTraceResult(
                current, TerminalKeyboardTraceOperation::SequenceResolution,
                TerminalKeyboardTraceDisposition::KeyboardActionMode, {},
                token);
            currentTracePublished = true;
            Q_EMIT inputActivityReconciled(activeProcess_);
        } else if (vt_ == nullptr) {
            publishKeyboardTraceResult(
                current, TerminalKeyboardTraceOperation::SequenceResolution,
                TerminalKeyboardTraceDisposition::TerminalUnavailable, {},
                token);
            currentTracePublished = true;
        } else {
            const GhosttyVtAdapter::EncodedKey encodedCurrent =
                vt_->encodeKey(current);
            if (!encodedCurrent.success) {
                publishKeyboardTraceResult(
                    current, TerminalKeyboardTraceOperation::SequenceResolution,
                    TerminalKeyboardTraceDisposition::EncoderFailed, {}, token);
                currentTracePublished = true;
            } else if (!encodedCurrent.bytes.isEmpty()) {
                bytes.append(encodedCurrent.bytes);
                currentBytes = encodedCurrent.bytes;
                potentialActivity =
                    potentialActivity || keyMayStartProcess(current);
                currentModifier = encodedCurrent.modifier;
                currentEscape = encodedCurrent.escape;
                currentEncoded = true;
                modifiersAfterBytes = modifiersAfterTerminalKey(current);
            } else {
                publishKeyboardTraceResult(
                    current, TerminalKeyboardTraceOperation::SequenceResolution,
                    TerminalKeyboardTraceDisposition::EncoderEmpty, {}, token);
                currentTracePublished = true;
            }
        }
    }

    activeSequenceToken_ = 0;
    stagedSequenceBytes_.clear();
    stagedSequenceModifiers_ = heldTerminalModifiers_;
    stagedSequencePotentialActivity_ = false;

    if (resolution == TerminalSequenceResolution::Drop) {
        finalizeStagedKeyboardTraceResults(
            TerminalKeyboardTraceDisposition::Dropped);
        publishKeyboardTraceResult(
            current, TerminalKeyboardTraceOperation::SequenceResolution,
            TerminalKeyboardTraceDisposition::Dropped, {}, token);
        currentTracePublished = true;
    }

    if (!bytes.isEmpty() && waitingForExitKey_) {
        finalizeStagedKeyboardTraceResults(
            TerminalKeyboardTraceDisposition::ExitWaitConsumed);
        if (currentEncoded && !currentTracePublished) {
            publishKeyboardTraceResult(
                current, TerminalKeyboardTraceOperation::SequenceResolution,
                TerminalKeyboardTraceDisposition::ExitWaitConsumed,
                currentBytes, token);
        }
        waitingForExitKey_ = false;
        Q_EMIT exitKeyDismissed();
        return;
    }

    // Append once so the staged leaders and the resolving key cannot be
    // interleaved by another queued operation on this worker thread.
    if (!bytes.isEmpty() && masterFd_ >= 0) {
        if (!readOnly_ && potentialActivity) {
            notePotentialActivity();
        }
        queueInputWrite(bytes);
        const TerminalKeyboardTraceDisposition disposition = readOnly_
            ? TerminalKeyboardTraceDisposition::ReadOnly
            : TerminalKeyboardTraceDisposition::Queued;
        finalizeStagedKeyboardTraceResults(disposition);
        if (currentEncoded && !currentTracePublished) {
            publishKeyboardTraceResult(
                current, TerminalKeyboardTraceOperation::SequenceResolution,
                disposition, currentBytes, token);
        }
        if (!readOnly_) {
            heldTerminalModifiers_ = modifiersAfterBytes;
        }
        if (currentEncoded) {
            clearSelectionAfterKey(currentModifier, currentEscape);
            scrollToBottomForKeystroke(currentModifier);
        }
    } else if (!bytes.isEmpty()) {
        finalizeStagedKeyboardTraceResults(
            TerminalKeyboardTraceDisposition::SessionUnavailable);
        if (currentEncoded && !currentTracePublished) {
            publishKeyboardTraceResult(
                current, TerminalKeyboardTraceOperation::SequenceResolution,
                TerminalKeyboardTraceDisposition::SessionUnavailable,
                currentBytes, token);
        }
    } else {
        // Successfully encoded staged entries imply non-empty aggregate bytes,
        // so this normally just releases an empty trace vector after an
        // encoder/KAM outcome for the current key.
        stagedSequenceTraceResults_.clear();
    }
}

void SessionWorker::sendInputMethod(const TerminalInputMethodInput &input)
{
    if (input.preeditTransition
        && options_.runtime.selectionClipboard.clearOnTyping) {
        clearSelectionState();
    }

    if (input.commitText.isEmpty() || vt_ == nullptr) {
        return;
    }
    if (keyboardInputSuppressed()) {
        Q_EMIT inputActivityReconciled(activeProcess_);
        return;
    }
    if (!readOnly_ && masterFd_ >= 0
        && (input.commitText.contains(u'\n')
            || input.commitText.contains(u'\r'))) {
        notePotentialActivity();
    }
    TerminalKeyInput key;
    key.key = Qt::Key_unknown;
    key.text = input.commitText;
    key.pressed = true;
    const GhosttyVtAdapter::EncodedKey encoded = vt_->encodeKey(key);
    if (encoded.bytes.isEmpty()) {
        return;
    }
    if (waitingForExitKey_) {
        waitingForExitKey_ = false;
        Q_EMIT exitKeyDismissed();
        return;
    }
    if (masterFd_ < 0) {
        return;
    }
    queueInputWrite(encoded.bytes);
    clearSelectionAfterKey(encoded.modifier, encoded.escape);
    scrollToBottomForKeystroke(encoded.modifier);
}

void SessionWorker::sendCsi(const QByteArray &payload)
{
    const std::optional<QByteArray> decoded =
        decodeGhosttyActionString(payload);
    if (!decoded.has_value()) {
        return;
    }
    QByteArray sequence;
    sequence.reserve(decoded->size() + 2);
    sequence.append('\x1b');
    sequence.append('[');
    sequence.append(*decoded);
    sendRawAction(sequence);
}

void SessionWorker::sendEscape(const QByteArray &payload)
{
    const std::optional<QByteArray> decoded =
        decodeGhosttyActionString(payload);
    if (!decoded.has_value()) {
        return;
    }
    QByteArray sequence;
    sequence.reserve(decoded->size() + 1);
    sequence.append('\x1b');
    sequence.append(*decoded);
    sendRawAction(sequence);
}

void SessionWorker::sendRawText(const QByteArray &serializedText)
{
    // Structured config exports Action.format output. Undo that byte escape
    // first, then apply the text action's config string-literal semantics.
    const std::optional<QByteArray> actionPayload =
        decodeGhosttyActionString(serializedText);
    if (!actionPayload.has_value()) {
        return;
    }
    const std::optional<QByteArray> text =
        decodeGhosttyConfigString(*actionPayload);
    if (!text.has_value()) {
        return;
    }
    sendRawAction(*text);
}

void SessionWorker::sendRawAction(const QByteArray &data)
{
    if (!readOnly_ && bytesMayStartProcess(data)) {
        notePotentialActivity();
    }
    queueInputWrite(data);
    scrollToBottomForInput();
}

void SessionWorker::resetTerminal()
{
    stopSelectionAutoscroll();
    heldTerminalModifiers_ = Qt::NoModifier;
    stagedSequenceModifiers_ = Qt::NoModifier;
    if (vt_ == nullptr) {
        return;
    }

    vt_->reset();
    markTerminalContentChanged();
    markSearchContentChanged();
    syncMouseEncoder();
    syncKeyboardActionMode();
    syncSelectionAvailability();
    processDeferredEffects();
    noteCompressionActivity();
    scheduleFrame();
}

void SessionWorker::clearSelectionState()
{
    if (vt_ == nullptr || !selectionAvailable_) {
        return;
    }

    vt_->clearSelection();
    syncSelectionAvailability();
    scheduleFrame();
}

void SessionWorker::clearSelectionAndResetGestureState()
{
    stopSelectionAutoscroll();
    if (vt_ == nullptr) {
        return;
    }
    const bool hadSelection = selectionAvailable_;
    vt_->clearSelectionAndResetGesture();
    if (hadSelection) {
        syncSelectionAvailability();
        scheduleFrame();
    }
}

void SessionWorker::clearSelectionAfterKey(bool modifier, bool escape)
{
    if (!modifier
        && (options_.runtime.selectionClipboard.clearOnTyping || escape)) {
        clearSelectionState();
    }
}

bool SessionWorker::keyboardInputSuppressed() const
{
    return options_.runtime.vtKamAllowed && vt_ != nullptr
        && vt_->keyboardActionMode();
}

Qt::KeyboardModifiers
SessionWorker::modifiersAfterTerminalKey(const TerminalKeyInput &input) const
{
    Qt::KeyboardModifiers result =
        static_cast<Qt::KeyboardModifiers>(input.modifiers)
        & kTrackedTerminalModifiers;
    Qt::KeyboardModifier triggered = Qt::NoModifier;
    switch (input.key) {
    case Qt::Key_Shift: triggered = Qt::ShiftModifier; break;
    case Qt::Key_Control: triggered = Qt::ControlModifier; break;
    case Qt::Key_Alt:
    case Qt::Key_AltGr: triggered = Qt::AltModifier; break;
    case Qt::Key_Meta: triggered = Qt::MetaModifier; break;
    default: return result;
    }

    if (input.pressed) {
        result |= triggered;
    } else {
        result &= ~triggered;
    }
    return result;
}

void SessionWorker::releaseHeldTerminalModifiers()
{
    Qt::KeyboardModifiers remaining =
        std::exchange(heldTerminalModifiers_, Qt::NoModifier);
    if (remaining == Qt::NoModifier || vt_ == nullptr) {
        return;
    }

    struct ModifierRelease {
        Qt::KeyboardModifier modifier;
        int key;
        quint32 rightScanCode;
        quint32 leftScanCode;
    };
    static constexpr std::array releases{
        ModifierRelease{Qt::ShiftModifier, Qt::Key_Shift, KEY_RIGHTSHIFT + 8U,
                        KEY_LEFTSHIFT + 8U},
        ModifierRelease{Qt::ControlModifier, Qt::Key_Control,
                        KEY_RIGHTCTRL + 8U, KEY_LEFTCTRL + 8U},
        ModifierRelease{Qt::AltModifier, Qt::Key_Alt, KEY_RIGHTALT + 8U,
                        KEY_LEFTALT + 8U},
        ModifierRelease{Qt::MetaModifier, Qt::Key_Meta, KEY_RIGHTMETA + 8U,
                        KEY_LEFTMETA + 8U},
    };

    QByteArray encodedReleases;
    for (const ModifierRelease &release : releases) {
        if (!remaining.testFlag(release.modifier)) {
            continue;
        }
        remaining &= ~release.modifier;
        TerminalKeyInput input;
        input.key = release.key;
        input.modifiers = remaining;
        input.pressed = false;
        input.nativeScanCode = release.rightScanCode;
        encodedReleases.append(vt_->encodeKey(input).bytes);
        input.nativeScanCode = release.leftScanCode;
        encodedReleases.append(vt_->encodeKey(input).bytes);
    }
    queuePtyWrite(encodedReleases);
}

void SessionWorker::syncKeyboardActionMode()
{
    const bool enabled = vt_ != nullptr && vt_->keyboardActionMode();
    if (keyboardActionMode_ == enabled) {
        return;
    }
    keyboardActionMode_ = enabled;
    Q_EMIT keyboardActionModeChanged(enabled);
}

void SessionWorker::sendMouse(const TerminalMouseInput &input)
{
    // The GUI policy decides whether an event is routed here; recheck the
    // worker-owned DEC state so a queued raw-mode transition cannot apply
    // reported-event side effects under a stale GUI snapshot.
    if (vt_ == nullptr || !vt_->mouseTracking()) {
        return;
    }
    if (input.action != TerminalMouseInput::Motion) {
        const bool wheel = input.button >= 4 && input.button <= 7;
        if (wheel) {
            clearSelectionState();
        } else {
            clearSelectionAndResetGestureState();
        }
    }
    // Encoding still runs in read-only mode and for events that produce no
    // bytes (for example an X10 wheel). queueInputWrite is the sole PTY-write
    // policy boundary, matching Ghostty's Surface::queueIo ordering.
    queueInputWrite(encodeMouse(input));
}

void SessionWorker::sendWheel(const TerminalWheelInput &input)
{
    if (vt_ == nullptr || (input.rows == 0 && input.columns == 0)) {
        return;
    }

    // DECSET 1007 is terminal-owned. Resolve it before frontend mouse policy
    // so an alternate-screen transition ordered immediately before this
    // request cannot be mistaken for a local viewport scroll.
    if (const auto sequence = vt_->alternateScrollSequence(input.rows);
        sequence.has_value()) {
        if (input.rows != 0) {
            clearSelectionState();
        }
        // As with every other surface-originated input path, terminal-local
        // side effects precede the read-only PTY-write policy boundary.
        if (!sequence->isEmpty()) {
            queueInputWrite(*sequence);
        }
        return;
    }

    if (input.mouseReportingEnabled && vt_->mouseTracking()) {
        TerminalMouseInput mouse;
        mouse.action = TerminalMouseInput::Press;
        mouse.modifiers = input.modifiers;
        mouse.x = input.x;
        mouse.y = input.y;
        const auto reportAxis = [this, &mouse](qint64 amount,
                                               int positiveButton,
                                               int negativeButton) {
            if (amount == 0) {
                return;
            }
            mouse.button = amount > 0 ? positiveButton : negativeButton;
            const quint64 magnitude = amount > 0
                ? static_cast<quint64>(amount)
                : static_cast<quint64>(-(amount + 1)) + 1U;
            for (quint64 remaining = magnitude; remaining > 0; --remaining) {
                sendMouse(mouse);
            }
        };
        // Match Ghostty's stable axis ordering for a diagonal gesture.
        reportAxis(input.rows, 4, 5);
        reportAxis(input.columns, 6, 7);
        return;
    }

    if (input.rows == 0) {
        // Horizontal wheel movement has no local viewport equivalent.
        return;
    }
    TerminalViewportRequest request;
    request.kind = TerminalViewportRequest::Kind::Delta;
    request.delta = input.rows == std::numeric_limits<qint64>::min()
        ? std::numeric_limits<qint64>::max()
        : -input.rows;
    scrollViewport(request);
}

void SessionWorker::resolveRightClick(const TerminalRightClickInput &input)
{
    TerminalRightClickResult result{
        .requestId = input.requestId,
        .contentRevision = terminalContentRevision_,
        .effect = TerminalRightClickEffect::None,
        .selectionAvailable = false,
    };
    const auto completion =
        qScopeGuard([this, &result] { Q_EMIT rightClickFinished(result); });
    if (input.requestId == 0 || vt_ == nullptr) {
        return;
    }

    syncSelectionAvailability();
    switch (options_.runtime.rightClickAction) {
    case RightClickAction::Ignore: break;
    case RightClickAction::Paste:
        clearSelectionState();
        result.effect = TerminalRightClickEffect::Paste;
        break;
    case RightClickAction::Copy:
        if (selectionAvailable_) {
            copySelectionTo(TerminalClipboardDestination::Standard, false);
        }
        clearSelectionState();
        break;
    case RightClickAction::CopyOrPaste:
        if (selectionAvailable_) {
            copySelectionTo(TerminalClipboardDestination::Standard, false);
            clearSelectionState();
        } else {
            result.effect = TerminalRightClickEffect::Paste;
        }
        break;
    case RightClickAction::ContextMenu: {
        result.effect = TerminalRightClickEffect::ContextMenu;
        const bool coordinateIsCurrent =
            input.contentRevision == terminalContentRevision_
            && hyperlinkState_->viewport.hasFrame()
            && hyperlinkState_->viewport.contentRevision()
                == terminalContentRevision_;
        if (!coordinateIsCurrent
            || vt_->selectionContains(input.column, input.row)) {
            break;
        }

        Qt::KeyboardModifiers modifiers =
            static_cast<Qt::KeyboardModifiers>(input.modifiers);
        if (input.shiftBypassedMouseCapture) {
            modifiers &= ~Qt::ShiftModifier;
        }
        std::optional<DetectedTerminalLink> detected;
        if (modifiers == Qt::ControlModifier) {
            detected = detectTerminalLinkAt(
                *vt_, linkMatcher_.get(), options_.runtime.linkUrl,
                hyperlinkState_->viewport, input.column, input.row);
        }

        bool selected = false;
        if (detected.has_value()) {
            if (detected->resolved.kind == TerminalLinkKind::Osc8) {
                // libghostty exposes an OSC 8 URI but not the link's logical
                // identity/range. Ghostty selects only the clicked cell for
                // this context-menu affordance.
                selected = vt_->selectCell(input.column, input.row);
            } else if (const auto *range =
                           std::get_if<GhosttyVtAdapter::TrackedTextRange>(
                               &detected->tracked)) {
                selected = vt_->installTextRange(*range);
            }
        }
        if (!selected) {
            selected = vt_->selectWord(input.column, input.row);
        }
        syncSelectionAvailability();
        if (selected) {
            copySelectionOnSelect();
            scheduleFrame();
        }
        break;
    }
    }
    result.selectionAvailable = selectionAvailable_;
}

QByteArray SessionWorker::encodeMouse(const TerminalMouseInput &input)
{
    return vt_ != nullptr ? vt_->encodeMouse(input) : QByteArray{};
}

void SessionWorker::setFocused(bool focused)
{
    if (vt_ != nullptr) {
        if (!focused) {
            releaseHeldTerminalModifiers();
        }
        queuePtyWrite(vt_->encodeFocus(focused));
    }
}

void SessionWorker::paste(const QString &text)
{
    if (text.isEmpty() || vt_ == nullptr || masterFd_ < 0) {
        return;
    }

    const TerminalClipboardPasteOptions &options =
        options_.runtime.clipboardPaste;
    const GhosttyVtAdapter::PreparedPaste prepared =
        vt_->preparePaste(text,
                          {
                              .protection = options.protection,
                              .bracketedSafe = options.bracketedSafe,
                          });
    if (prepared.disposition
        == GhosttyVtAdapter::PasteDisposition::ConfirmationRequired) {
        do {
            ++nextPasteRequestId_;
        } while (nextPasteRequestId_ == 0
                 || pendingPastes_.contains(nextPasteRequestId_));
        pendingPastes_.insert(nextPasteRequestId_, text);
        Q_EMIT unsafePasteConfirmationRequested(nextPasteRequestId_, text);
        return;
    }
    if (prepared.disposition == GhosttyVtAdapter::PasteDisposition::Ready) {
        commitPaste(text, prepared.bytes);
    }
}

void SessionWorker::confirmPaste(quint64 requestId)
{
    const auto pending = pendingPastes_.find(requestId);
    if (pending == pendingPastes_.end()) {
        return;
    }
    const QString text = pending.value();
    pendingPastes_.erase(pending);
    if (vt_ == nullptr || masterFd_ < 0) {
        return;
    }

    const TerminalClipboardPasteOptions &options =
        options_.runtime.clipboardPaste;
    const GhosttyVtAdapter::PreparedPaste prepared = vt_->preparePaste(
        text,
        {
            .protection = options.protection,
            .bracketedSafe = options.bracketedSafe,
            .authorization = GhosttyVtAdapter::PasteAuthorization::Confirmed,
        });
    if (prepared.disposition == GhosttyVtAdapter::PasteDisposition::Ready) {
        commitPaste(text, prepared.bytes);
    }
}

void SessionWorker::cancelPaste(quint64 requestId)
{
    pendingPastes_.remove(requestId);
}

void SessionWorker::commitPaste(const QString &text, const QByteArray &encoded)
{
    if (encoded.isEmpty()) {
        return;
    }
    if (!readOnly_ && (text.contains(u'\n') || text.contains(u'\r'))) {
        notePotentialActivity();
    }
    // Ghostty returns to the active screen before making accepted paste data
    // visible to the child. Rejected and cancelled requests never get here.
    scrollToBottomForInput();
    queueInputWrite(encoded);
}

void SessionWorker::scrollToBottomForInput()
{
    scrollViewport({.kind = TerminalViewportRequest::Kind::Bottom});
}

void SessionWorker::scrollToBottomForKeystroke(bool modifier)
{
    if (!modifier && options_.runtime.scrollToBottom.keystroke) {
        scrollToBottomForInput();
    }
}

void SessionWorker::copySelection()
{
    copySelectionTo(TerminalClipboardDestination::Standard,
                    options_.runtime.selectionClipboard.clearOnCopy);
}

void SessionWorker::copySelectionAction(quint64 requestId)
{
    TerminalActionResult result{
        .requestId = requestId,
        .outcome = TerminalActionOutcome::Failed,
        .effect = TerminalActionEffect::None,
        .performed = false,
        .payload = {},
        .clipboardDestination = TerminalClipboardDestination::Standard,
    };
    const auto completion =
        qScopeGuard([this, &result] { Q_EMIT terminalActionFinished(result); });
    if (vt_ == nullptr) {
        return;
    }
    if (!vt_->hasSelection()) {
        result.outcome = TerminalActionOutcome::Unavailable;
        return;
    }

    const QString text = clipboardSelectionText();
    if (text.isNull()) {
        Q_EMIT errorOccurred(
            QStringLiteral("Failed to format terminal selection"));
        return;
    }

    result.outcome = TerminalActionOutcome::Success;
    result.effect = TerminalActionEffect::Clipboard;
    result.performed = true;
    result.payload = text;
    result.clipboardDestination = TerminalClipboardDestination::Standard;
    if (options_.runtime.selectionClipboard.clearOnCopy) {
        // Publish the reconciled selection state before the correlated result
        // reaches the GUI and resumes a dependent action-chain entry.
        clearSelectionState();
    }
}

void SessionWorker::writeTerminalFile(quint64 requestId,
                                      const TerminalWriteFileAction &action)
{
    TerminalActionResult result{
        .requestId = requestId,
        .outcome = TerminalActionOutcome::Failed,
        .effect = TerminalActionEffect::None,
        .performed = false,
        .payload = {},
        .clipboardDestination = TerminalClipboardDestination::Standard,
    };
    const auto completion =
        qScopeGuard([this, &result] { Q_EMIT terminalActionFinished(result); });
    if (vt_ == nullptr) {
        return;
    }
    if (action.format != TerminalFileFormat::Plain) {
        Q_EMIT errorOccurred(
            QStringLiteral("Unsupported terminal file format"));
        return;
    }
    switch (action.location) {
    case TerminalFileLocation::Screen:
    case TerminalFileLocation::Scrollback:
    case TerminalFileLocation::Selection: break;
    default:
        Q_EMIT errorOccurred(QStringLiteral("Invalid terminal file location"));
        return;
    }
    switch (action.disposition) {
    case TerminalFileDisposition::Copy:
    case TerminalFileDisposition::Paste:
    case TerminalFileDisposition::Open: break;
    default:
        Q_EMIT errorOccurred(
            QStringLiteral("Invalid terminal file disposition"));
        return;
    }

    const GhosttyVtAdapter::PlainFileSnapshot snapshot =
        vt_->snapshotPlainFile(action.location);
    // Formatting restores compressed pages without changing Ghostty's
    // activity token. Schedule a bounded follow-up pass directly so the live
    // policy can reclaim them.
    scheduleRestoredPageCompression();
    switch (snapshot.status) {
    case GhosttyVtAdapter::PlainFileSnapshotStatus::Unavailable:
        result.outcome = TerminalActionOutcome::Unavailable;
        result.performed = true;
        return;
    case GhosttyVtAdapter::PlainFileSnapshotStatus::Failed:
        Q_EMIT errorOccurred(
            QStringLiteral("Failed to format terminal contents"));
        return;
    case GhosttyVtAdapter::PlainFileSnapshotStatus::Ready: break;
    }

    QTemporaryDir directory(
        QDir::temp().filePath(QStringLiteral("ghostty-qt-XXXXXX")));
    if (!directory.isValid()) {
        Q_EMIT errorOccurred(
            QStringLiteral("Failed to create terminal file directory: %1")
                .arg(directory.errorString()));
        return;
    }
    if (!QFile::setPermissions(directory.path(),
                               QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                   | QFileDevice::ExeOwner)) {
        Q_EMIT errorOccurred(
            QStringLiteral("Failed to secure terminal file directory '%1'")
                .arg(directory.path()));
        return;
    }
    if (!terminalDirectoryHasPrivateMode(directory.path())) {
        Q_EMIT errorOccurred(
            QStringLiteral("Terminal file directory '%1' is not private")
                .arg(directory.path()));
        return;
    }

    const QString path =
        QDir(directory.path()).filePath(terminalFileBaseName(action.location));
    QFile file(path);
    const QFileDevice::Permissions filePermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly,
                   filePermissions)) {
        Q_EMIT errorOccurred(
            QStringLiteral("Failed to create terminal file '%1': %2")
                .arg(path, file.errorString()));
        return;
    }
    if (!file.setPermissions(filePermissions)) {
        Q_EMIT errorOccurred(
            QStringLiteral("Failed to secure terminal file '%1': %2")
                .arg(path, file.errorString()));
        return;
    }
    if (!terminalFileHasPrivateMode(file)) {
        Q_EMIT errorOccurred(
            QStringLiteral("Terminal file '%1' is not private").arg(path));
        return;
    }
    if (file.write(snapshot.bytes) != snapshot.bytes.size() || !file.flush()) {
        Q_EMIT errorOccurred(
            QStringLiteral("Failed to write terminal file '%1': %2")
                .arg(path, file.errorString()));
        return;
    }
    file.close();
    if (file.error() != QFileDevice::NoError) {
        Q_EMIT errorOccurred(
            QStringLiteral("Failed to close terminal file '%1': %2")
                .arg(path, file.errorString()));
        return;
    }

    const QString canonicalPath = QFileInfo(path).canonicalFilePath();
    if (canonicalPath.isEmpty()) {
        Q_EMIT errorOccurred(
            QStringLiteral("Failed to resolve terminal file '%1'").arg(path));
        return;
    }

    // Upstream intentionally keeps successful artifacts available after the
    // action completes. Failures above retain QTemporaryDir's cleanup.
    directory.setAutoRemove(false);
    result.outcome = TerminalActionOutcome::Success;
    result.performed = true;
    result.payload = canonicalPath;
    switch (action.disposition) {
    case TerminalFileDisposition::Copy:
        result.effect = TerminalActionEffect::Clipboard;
        result.clipboardDestination = TerminalClipboardDestination::Standard;
        return;
    case TerminalFileDisposition::Paste:
        // This is deliberately raw path input: no bracketed-paste framing,
        // newline, quoting, viewport change, or selection side effect.
        queueInputWrite(QFile::encodeName(canonicalPath));
        result.effect = TerminalActionEffect::None;
        return;
    case TerminalFileDisposition::Open:
        result.effect = TerminalActionEffect::OpenFile;
        return;
    }
}

void SessionWorker::copySelectionTo(TerminalClipboardDestination destination,
                                    bool clearAfterCopy)
{
    if (vt_ == nullptr) {
        return;
    }

    const QString text = clipboardSelectionText();
    if (text.isNull()) {
        return;
    }

    Q_EMIT clipboardTextReady(text, destination);
    if (clearAfterCopy) {
        clearSelectionState();
    }
}

QString SessionWorker::clipboardSelectionText()
{
    QString text = vt_->selectedText(
        options_.runtime.selectionClipboard.trimTrailingSpaces);
    scheduleRestoredPageCompression();
    if (text.isNull()) {
        return {};
    }
    return applyTerminalClipboardCodepointMap(
        std::move(text),
        std::span{options_.runtime.selectionClipboard.codepointMap});
}

void SessionWorker::copySelectionOnSelect()
{
    switch (options_.runtime.selectionClipboard.copyOnSelect) {
    case TerminalCopyOnSelectMode::Disabled: return;
    case TerminalCopyOnSelectMode::Primary:
        copySelectionTo(TerminalClipboardDestination::Primary, false);
        return;
    case TerminalCopyOnSelectMode::PrimaryAndClipboard:
        copySelectionTo(TerminalClipboardDestination::PrimaryAndStandard,
                        false);
        return;
    }
}

void SessionWorker::clearSelection()
{
    clearSelectionState();
}

void SessionWorker::clearSelectionIfMouseTracking()
{
    if (vt_ != nullptr && vt_->mouseTracking()) {
        clearSelectionState();
    }
}

void SessionWorker::beginSelection(const TerminalSelectionPressInput &input)
{
    stopSelectionAutoscroll();
    if (vt_ == nullptr) {
        return;
    }

    // A delayed Shift press can be resolved by Ghostty as a continuation of
    // the retained drag gesture. Preserve its pointer metadata so an edge
    // extension can arm autoscroll without requiring another mouse move.
    selectionAutoscrollInput_ = {
        .column = input.column,
        .row = input.row,
        .surfaceX = input.surfaceX,
        .surfaceY = input.surfaceY,
        .rectangular = input.rectangular,
    };
    const bool selectionChanged = vt_->beginSelection(input);
    syncSelectionAutoscroll();
    if (selectionChanged) {
        syncSelectionAvailability();
        scheduleFrame();
    }
}

void SessionWorker::updateSelection(const TerminalSelectionDragInput &input)
{
    if (vt_ == nullptr) {
        stopSelectionAutoscroll();
        return;
    }

    selectionAutoscrollInput_ = input;
    const bool selectionChanged = vt_->updateSelection(input);
    syncSelectionAutoscroll();
    if (selectionChanged) {
        markTerminalContentChanged();
        if (searchState_->active) {
            rebuildSearchVisibleCells();
            publishSearchUpdate();
        }
        syncSelectionAvailability();
        noteCompressionActivity();
        scheduleFrame();
    }
}

void SessionWorker::endSelection(int column, int row)
{
    if (vt_ != nullptr) {
        vt_->endSelection(column, row);
        stopSelectionAutoscroll();
        syncSelectionAvailability();
        copySelectionOnSelect();
    } else {
        stopSelectionAutoscroll();
    }
}

void SessionWorker::cancelSelectionGesture()
{
    stopSelectionAutoscroll();
    if (vt_ != nullptr) {
        // Pointer capture was revoked without a release. Preserve the
        // selection reached so far, but discard the tracked anchor and
        // multi-click history so neither can resume on a later pointer event.
        vt_->resetSelectionGesture();
    }
}

void SessionWorker::selectionAutoscrollTick()
{
    if (vt_ == nullptr || !selectionAutoscrollInput_.has_value()
        || vt_->selectionAutoscrollTick(*selectionAutoscrollInput_)
            != GhosttyVtAdapter::SelectionAutoscrollTickResult::Mutated) {
        stopSelectionAutoscroll();
        return;
    }

    // Ghostty scrolls exactly one viewport row before resolving the selection
    // endpoint against the newly visible content. Treat each accepted tick as
    // both a viewport and selection mutation even when the installed range is
    // unchanged at a scroll boundary.
    markTerminalContentChanged();
    if (searchState_->active) {
        rebuildSearchVisibleCells();
        publishSearchUpdate();
    }
    syncSelectionAvailability();
    noteCompressionActivity();
    scheduleFrame();
    syncSelectionAutoscroll();
}

void SessionWorker::syncSelectionAutoscroll()
{
    const bool active = vt_ != nullptr && selectionAutoscrollInput_.has_value()
        && vt_->selectionAutoscrollDirection()
            != GhosttyVtAdapter::SelectionAutoscrollDirection::None;
    if (!active) {
        stopSelectionAutoscroll();
        return;
    }
    if (selectionAutoscrollTimer_ != nullptr
        && !selectionAutoscrollTimer_->isActive()) {
        selectionAutoscrollTimer_->start();
    }
}

void SessionWorker::stopSelectionAutoscroll()
{
    selectionAutoscrollInput_.reset();
    if (selectionAutoscrollTimer_ != nullptr) {
        selectionAutoscrollTimer_->stop();
    }
}

void SessionWorker::selectAll()
{
    const bool selected = vt_ != nullptr && vt_->selectAll();
    syncSelectionAvailability();
    if (selected) {
        copySelectionOnSelect();
    }
    Q_EMIT selectAllCompleted(selectionAvailable_);
    if (selected) {
        scheduleFrame();
    }
}

void SessionWorker::selectAllAction(quint64 requestId)
{
    TerminalActionResult result{
        .requestId = requestId,
        .outcome = TerminalActionOutcome::Failed,
        .effect = TerminalActionEffect::None,
        .performed = false,
        .payload = {},
        .clipboardDestination = TerminalClipboardDestination::Standard,
    };
    const auto completion = qScopeGuard([this, &result] {
        Q_EMIT selectAllCompleted(selectionAvailable_);
        Q_EMIT terminalActionFinished(result);
    });

    if (vt_ == nullptr) {
        syncSelectionAvailability();
        return;
    }

    const bool selected = vt_->selectAll();
    syncSelectionAvailability();
    result.performed = true;
    if (!selected) {
        result.outcome = TerminalActionOutcome::Unavailable;
        return;
    }

    result.outcome = TerminalActionOutcome::Success;
    std::optional<TerminalClipboardDestination> destination;
    switch (options_.runtime.selectionClipboard.copyOnSelect) {
    case TerminalCopyOnSelectMode::Disabled: break;
    case TerminalCopyOnSelectMode::Primary:
        destination = TerminalClipboardDestination::Primary;
        break;
    case TerminalCopyOnSelectMode::PrimaryAndClipboard:
        destination = TerminalClipboardDestination::PrimaryAndStandard;
        break;
    }
    if (destination.has_value()) {
        const QString text = clipboardSelectionText();
        if (!text.isNull()) {
            result.effect = TerminalActionEffect::Clipboard;
            result.payload = text;
            result.clipboardDestination = *destination;
        }
    }
    scheduleFrame();
}

void SessionWorker::adjustSelection(TerminalSelectionAdjustment adjustment)
{
    if (vt_ != nullptr && vt_->adjustSelection(adjustment)) {
        markTerminalContentChanged();
        if (searchState_->active) {
            rebuildSearchVisibleCells();
            publishSearchUpdate();
        }
        syncSelectionAvailability();
        noteCompressionActivity();
        scheduleFrame();
    }
}

void SessionWorker::adjustSelectionAction(
    quint64 requestId, TerminalSelectionAdjustment adjustment)
{
    TerminalActionResult result{
        .requestId = requestId,
        .outcome = TerminalActionOutcome::Failed,
        .effect = TerminalActionEffect::None,
        .performed = false,
        .payload = {},
        .clipboardDestination = TerminalClipboardDestination::Standard,
    };
    const auto completion =
        qScopeGuard([this, &result] { Q_EMIT terminalActionFinished(result); });
    if (vt_ == nullptr) {
        return;
    }
    if (!vt_->hasSelection()) {
        result.outcome = TerminalActionOutcome::Unavailable;
        return;
    }
    if (!vt_->adjustSelection(adjustment)) {
        Q_EMIT errorOccurred(
            QStringLiteral("Failed to adjust terminal selection"));
        return;
    }

    markTerminalContentChanged();
    if (searchState_->active) {
        rebuildSearchVisibleCells();
        publishSearchUpdate();
    }
    syncSelectionAvailability();
    noteCompressionActivity();
    scheduleFrame();
    result.outcome = TerminalActionOutcome::Success;
    result.performed = true;
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

void SessionWorker::markTerminalContentChanged()
{
    do {
        ++terminalContentRevision_;
    } while (terminalContentRevision_ == 0);
}

void SessionWorker::markSearchContentChanged()
{
    do {
        ++searchContentRevision_;
    } while (searchContentRevision_ == 0);
    restartSearch();
}

void SessionWorker::scrollViewport(const TerminalViewportRequest &request)
{
    if (vt_ != nullptr && vt_->scrollViewport(request)) {
        markTerminalContentChanged();
        if (searchState_->active) {
            rebuildSearchVisibleCells();
            publishSearchUpdate();
        }
        noteCompressionActivity();
        scheduleFrame();
    }
}

void SessionWorker::scrollToSelectionAction(quint64 requestId)
{
    TerminalActionResult result{
        .requestId = requestId,
        .outcome = TerminalActionOutcome::Failed,
        .effect = TerminalActionEffect::None,
        .performed = false,
        .payload = {},
        .clipboardDestination = TerminalClipboardDestination::Standard,
    };
    const auto completion =
        qScopeGuard([this, &result] { Q_EMIT terminalActionFinished(result); });
    if (vt_ == nullptr) {
        return;
    }
    if (!vt_->hasSelection()) {
        result.outcome = TerminalActionOutcome::Unavailable;
        return;
    }
    if (!vt_->scrollViewport({
            .kind = TerminalViewportRequest::Kind::Selection,
        })) {
        Q_EMIT errorOccurred(
            QStringLiteral("Failed to scroll to terminal selection"));
        return;
    }

    markTerminalContentChanged();
    if (searchState_->active) {
        rebuildSearchVisibleCells();
        publishSearchUpdate();
    }
    noteCompressionActivity();
    scheduleFrame();
    result.outcome = TerminalActionOutcome::Success;
    result.performed = true;
}

void SessionWorker::beginSearch(quint64 generation, const QByteArray &needle)
{
    if (generation == 0) {
        return;
    }
    if (searchState_->generation != 0 && generation != searchState_->generation
        && !sequenceTokenIsNewer(generation, searchState_->generation)) {
        return;
    }
    if (generation != searchState_->generation && searchState_->active
        && searchState_->dataRevision == searchContentRevision_
        && !needle.isEmpty()
        && searchNeedlesEqual(searchState_->needle, needle)) {
        // Pinned Ghostty treats ASCII-case-only needle changes as unchanged,
        // preserving progressive results and the selected match.
        searchState_->generation = generation;
        searchState_->needle = needle;
        publishSearchUpdate();
        return;
    }

    const bool chunkScheduled = searchState_->chunkScheduled;
    if ((searchState_->rowsSinceCompressionPass > 0
         || searchState_->currentRow.has_value())
        && compressionTimer_ != nullptr) {
        // Public screen-coordinate cell reads restore compressed pages. Start
        // an incremental verification pass when a query is replaced so those
        // cold pages do not remain resident indefinitely.
        scheduleRestoredPageCompression();
    }
    *searchState_ = SearchState{};
    searchState_->chunkScheduled = chunkScheduled;
    searchState_->generation = generation;
    searchState_->dataRevision = searchContentRevision_;
    searchState_->needle = needle;

    if (needle.isEmpty() || vt_ == nullptr) {
        searchState_->complete = true;
        publishSearchUpdate();
        return;
    }

    const std::optional<GhosttyVtAdapter::SearchExtent> extent =
        vt_->searchExtent();
    if (!extent.has_value()) {
        searchState_->complete = true;
        publishSearchUpdate();
        return;
    }
    const std::optional<qsizetype> cellCount =
        gridCellCount(extent->columns, extent->rows);
    if (!cellCount.has_value()) {
        searchState_->complete = true;
        publishSearchUpdate();
        return;
    }

    searchState_->active = true;
    searchState_->totalRows = extent->totalRows;
    searchState_->columns = extent->columns;
    searchState_->rows = extent->rows;
    searchState_->viewportOffset = extent->viewportOffset;
    searchState_->viewportLength = extent->viewportLength;
    searchState_->activeScreen = extent->activeScreen;
    searchState_->visibleCellMask.resize(*cellCount);
    searchState_->selectedCellMask.resize(*cellCount);
    searchState_->nextScreenRow = extent->totalRows == 0
        ? -1
        : static_cast<qint64>(extent->totalRows - 1U);
    searchState_->reversedNeedle = reversedFoldedSearchNeedle(needle);
    searchState_->prefix = searchPrefixTable(searchState_->reversedNeedle);
    publishSearchUpdate();
    scheduleSearchChunk();
}

void SessionWorker::search(quint64 generation, const QByteArray &needle)
{
    beginSearch(generation, needle);
}

void SessionWorker::searchSerialized(quint64 generation,
                                     const QByteArray &serializedNeedle)
{
    const std::optional<QByteArray> needle =
        decodeGhosttyActionString(serializedNeedle);
    if (!needle.has_value()) {
        cancelSearch(generation);
        return;
    }
    beginSearch(generation, *needle);
}

void SessionWorker::cancelSearch(quint64 generation)
{
    if (generation == 0) {
        return;
    }
    if (searchState_->generation != 0 && generation != searchState_->generation
        && !sequenceTokenIsNewer(generation, searchState_->generation)) {
        return;
    }
    const bool chunkScheduled = searchState_->chunkScheduled;
    if ((searchState_->rowsSinceCompressionPass > 0
         || searchState_->currentRow.has_value())
        && compressionTimer_ != nullptr) {
        scheduleRestoredPageCompression();
    }
    *searchState_ = SearchState{};
    searchState_->chunkScheduled = chunkScheduled;
    searchState_->generation = generation;
    searchState_->dataRevision = searchContentRevision_;
    searchState_->complete = true;
    publishSearchUpdate();
}

void SessionWorker::restartSearch()
{
    if (!searchState_->active || searchState_->generation == 0
        || searchState_->needle.isEmpty()) {
        return;
    }
    const quint64 generation = searchState_->generation;
    const QByteArray needle = searchState_->needle;
    beginSearch(generation, needle);
}

void SessionWorker::scheduleSearchChunk()
{
    if (!searchState_->active || searchState_->complete
        || searchState_->chunkScheduled) {
        return;
    }
    searchState_->chunkScheduled = true;
    QTimer::singleShot(0, this, &SessionWorker::processSearchChunk);
}

void SessionWorker::publishSearchUpdate()
{
    TerminalSearchUpdate update;
    update.generation = searchState_->generation;
    update.contentRevision = terminalContentRevision_;
    update.active = searchState_->active;
    update.complete = searchState_->complete;
    update.scannedRows = searchState_->scannedRows;
    update.totalRows = searchState_->totalRows;
    update.totalMatches = static_cast<quint64>(searchState_->matches.size());
    update.selectedMatch = searchState_->selectedMatch;
    update.columns = searchState_->columns;
    update.rows = searchState_->rows;
    update.visibleCellMask = searchState_->visibleCellMask;
    update.selectedCellMask = searchState_->selectedCellMask;
    Q_EMIT searchUpdated(update);
    searchState_->lastPublication.restart();
}

void SessionWorker::rebuildSearchVisibleCells()
{
    searchState_->visibleCellMask.fill(false);
    searchState_->selectedCellMask.fill(false);
    if (!searchState_->active || vt_ == nullptr) {
        return;
    }
    const std::optional<GhosttyVtAdapter::SearchExtent> extent =
        vt_->searchExtent();
    if (!extent.has_value() || extent->totalRows != searchState_->totalRows
        || extent->columns != searchState_->columns
        || extent->rows != searchState_->rows
        || extent->activeScreen != searchState_->activeScreen) {
        return;
    }
    searchState_->viewportOffset = extent->viewportOffset;
    searchState_->viewportLength = extent->viewportLength;
    const quint64 viewportStart = extent->viewportOffset;
    const quint64 viewportEnd = viewportStart + extent->viewportLength;
    const auto firstVisibleCandidate = std::lower_bound(
        searchState_->matches.cbegin(), searchState_->matches.cend(),
        viewportEnd,
        [](const TerminalSearchRange &range, quint64 rowExclusive) {
            return range.start.screenRow >= rowExclusive;
        });
    for (auto iterator = firstVisibleCandidate;
         iterator != searchState_->matches.cend(); ++iterator) {
        const TerminalSearchRange &range = *iterator;
        if (range.end.screenRow < viewportStart) {
            // Results are newest-to-oldest and a fixed-size match's end
            // moves monotonically upward with its start. No later result can
            // intersect this viewport.
            break;
        }
        addVisibleSearchCells(searchState_->visibleCellMask, range, *extent);
    }
    if (searchState_->selectedMatch >= 0
        && searchState_->selectedMatch < searchState_->matches.size()) {
        addVisibleSearchCells(
            searchState_->selectedCellMask,
            searchState_->matches.at(searchState_->selectedMatch), *extent);
    }
}

void SessionWorker::processSearchChunk()
{
    searchState_->chunkScheduled = false;
    if (!searchState_->active || searchState_->complete || vt_ == nullptr) {
        return;
    }
    if (searchState_->dataRevision != searchContentRevision_) {
        restartSearch();
        return;
    }

    const std::optional<GhosttyVtAdapter::SearchExtent> currentExtent =
        vt_->searchExtent();
    if (!currentExtent.has_value()
        || currentExtent->totalRows != searchState_->totalRows
        || currentExtent->columns != searchState_->columns
        || currentExtent->rows != searchState_->rows
        || currentExtent->activeScreen != searchState_->activeScreen) {
        restartSearch();
        return;
    }
    searchState_->viewportOffset = currentExtent->viewportOffset;
    searchState_->viewportLength = currentExtent->viewportLength;

    QElapsedTimer budget;
    budget.start();
    int rowsLoaded = 0;
    qsizetype bytesProcessed = 0;
    auto feedByte = [this, extent = *currentExtent](
                        char byte, const TerminalSearchCell &cell) {
        SearchState &state = *searchState_;
        const char folded = foldSearchByte(byte);
        while (state.matched > 0
               && state.reversedNeedle.at(state.matched) != folded) {
            state.matched = state.prefix.at(state.matched - 1);
        }
        if (state.reversedNeedle.at(state.matched) == folded) {
            ++state.matched;
        }

        state.recentCells.push_back(cell);
        if (state.recentCells.size()
            > static_cast<size_t>(state.reversedNeedle.size())) {
            state.recentCells.pop_front();
        }
        if (state.matched != state.reversedNeedle.size()) {
            return;
        }

        const TerminalSearchRange range{
            .start = cell,
            .end = state.recentCells.front(),
        };
        state.matches.append(range);
        addVisibleSearchCells(state.visibleCellMask, range, extent);
        state.matched = state.prefix.at(state.matched - 1);
    };

    while (!searchState_->complete) {
        if (!searchState_->currentRow.has_value()) {
            if (searchState_->nextScreenRow < 0) {
                // SlidingWindow appends one delimiter after the final
                // formatted page even when PageFormatter trims every row as
                // blank. Emit it only after the trailing rows have been
                // inspected so they do not become independently searchable.
                if (searchState_->finalBoundaryPending) {
                    feedByte('\n', searchState_->finalBoundaryCell);
                    searchState_->finalBoundaryPending = false;
                    ++bytesProcessed;
                }
                break;
            }
            if (rowsLoaded >= kSearchRowsPerChunk) {
                break;
            }
            const quint32 screenRow =
                static_cast<quint32>(searchState_->nextScreenRow--);
            std::optional<GhosttyVtAdapter::SearchRowSnapshot> row =
                vt_->snapshotSearchRow(screenRow);
            ++searchState_->rowsSinceCompressionPass;
            ++rowsLoaded;
            if (!row.has_value()) {
                ++searchState_->scannedRows;
                searchState_->complete = true;
                break;
            }
            if (!searchState_->finalBoundaryObserved) {
                // Pinned Ghostty appends a page-ending newline according to
                // the newest physical row's wrap flag, after formatting has
                // discarded trailing blank rows.
                searchState_->finalBoundaryObserved = true;
                searchState_->finalBoundaryPending = !row->wrapped;
                searchState_->finalBoundaryCell = row->newlineCell;
            }
            if (row->wrapped && !searchState_->newerLogicalContent) {
                // PageFormatter carries trailing blanks through a soft wrap,
                // but emits them only if a later continuation contains text.
                // Scanning newest-to-oldest tells us that before consuming
                // this row, so discard an orphaned pending blank run here.
                while (!row->text.isEmpty()
                       && row->text.at(row->text.size() - 1) == ' ') {
                    row->text.chop(1);
                    row->byteCells.removeLast();
                }
            }
            if (!searchState_->seenFormattedContent && row->text.isEmpty()) {
                searchState_->newerLogicalContent = false;
                ++searchState_->scannedRows;
                continue;
            }
            if (searchState_->seenFormattedContent) {
                searchState_->currentRowBoundaryPending = !row->wrapped;
                searchState_->currentRowBoundaryCell = row->newlineCell;
            } else {
                searchState_->currentRowBoundaryPending =
                    searchState_->finalBoundaryPending;
                // SlidingWindow maps its appended newline to the last byte
                // PageFormatter retained, not to any trimmed blank row below
                // it. This keeps a selected match anchored to visible text.
                searchState_->currentRowBoundaryCell = row->newlineCell;
                searchState_->finalBoundaryPending = false;
            }
            searchState_->currentRowLogicalHasText = !row->text.isEmpty()
                || (row->wrapped && searchState_->newerLogicalContent);
            searchState_->currentRowByteIndex = row->text.size();
            searchState_->currentRow = std::move(*row);
        }

        if (searchState_->currentRowBoundaryPending) {
            feedByte('\n', searchState_->currentRowBoundaryCell);
            searchState_->currentRowBoundaryPending = false;
            ++bytesProcessed;
        } else if (searchState_->currentRowByteIndex > 0) {
            const qsizetype index = --searchState_->currentRowByteIndex;
            feedByte(searchState_->currentRow->text.at(index),
                     searchState_->currentRow->byteCells.at(index));
            ++bytesProcessed;
        } else {
            searchState_->seenFormattedContent = true;
            searchState_->newerLogicalContent =
                searchState_->currentRowLogicalHasText;
            searchState_->currentRow.reset();
            ++searchState_->scannedRows;
            continue;
        }

        if ((bytesProcessed & 63) == 0
            && budget.elapsed() >= kSearchChunkBudgetMilliseconds) {
            break;
        }
    }

    if (searchState_->nextScreenRow < 0
        && !searchState_->currentRow.has_value()) {
        searchState_->complete = true;
    }
    if (compressionTimer_ != nullptr
        && (searchState_->complete
            || searchState_->rowsSinceCompressionPass
                >= kSearchRowsPerCompressionPass)) {
        // Unlike Ghostty's internal search window, the public grid-ref API
        // cannot decode a compressed page into temporary owned storage. Its
        // read restores the page, so interleave the library's bounded
        // compression traversal to cap residency and recover cold history.
        searchState_->rowsSinceCompressionPass = 0;
        scheduleRestoredPageCompression();
    }
    if (searchState_->complete || !searchState_->lastPublication.isValid()
        || searchState_->lastPublication.elapsed()
            >= kSearchPublishIntervalMilliseconds) {
        publishSearchUpdate();
    }
    scheduleSearchChunk();
}

void SessionWorker::navigateSearch(quint64 generation,
                                   TerminalSearchDirection direction)
{
    if (generation == 0 || generation != searchState_->generation
        || !searchState_->active) {
        return;
    }
    if (searchState_->matches.isEmpty()) {
        // Search navigation is an instantaneous mailbox action upstream. If
        // the progressive scan has not produced a result yet, the request is
        // discarded rather than replayed against a later result set.
        return;
    }

    const qint64 count = static_cast<qint64>(searchState_->matches.size());
    if (direction == TerminalSearchDirection::Next) {
        searchState_->selectedMatch = searchState_->selectedMatch < 0
            ? 0
            : (searchState_->selectedMatch + 1) % count;
    } else {
        searchState_->selectedMatch = searchState_->selectedMatch < 0
            ? count - 1
            : (searchState_->selectedMatch + count - 1) % count;
    }

    const TerminalSearchRange &range =
        searchState_->matches.at(searchState_->selectedMatch);
    const bool viewportChanged =
        vt_ != nullptr && vt_->scrollSearchRangeIntoView(range);
    if (viewportChanged) {
        markTerminalContentChanged();
        noteCompressionActivity();
        scheduleFrame();
    }
    rebuildSearchVisibleCells();
    publishSearchUpdate();
}

void SessionWorker::searchSelectionAction(quint64 requestId)
{
    TerminalActionResult result{
        .requestId = requestId,
        .outcome = TerminalActionOutcome::Failed,
        .effect = TerminalActionEffect::None,
        .performed = false,
        .payload = {},
        .clipboardDestination = TerminalClipboardDestination::Standard,
    };
    const auto completion =
        qScopeGuard([this, &result] { Q_EMIT terminalActionFinished(result); });
    if (vt_ == nullptr) {
        return;
    }
    if (!vt_->hasSelection()) {
        result.outcome = TerminalActionOutcome::Unavailable;
        return;
    }

    const QString text = vt_->selectedText(false);
    scheduleRestoredPageCompression();
    if (text.isNull()) {
        Q_EMIT errorOccurred(
            QStringLiteral("Failed to format terminal selection"));
        return;
    }

    result.outcome = TerminalActionOutcome::Success;
    result.effect = TerminalActionEffect::StartSearch;
    result.performed = true;
    result.payload = text;
}

void SessionWorker::queryHyperlink(quint64 requestId, quint64 contentRevision,
                                   int column, int row)
{
    if (requestId == 0) {
        return;
    }

    // The queued UI boundary may deliver many pointer positions before this
    // worker returns to its event loop. Store only the newest payload and do
    // the libghostty scan in a zero-delay dispatch, so superseded events stay
    // O(1) and never accumulate full viewport scans.
    hyperlinkState_->activeRequestId = 0;
    hyperlinkState_->trackedHover.reset();
    hyperlinkState_->publishedState = TerminalHyperlinkState::Invalid;
    hyperlinkState_->publishedKind = TerminalLinkKind::Osc8;
    hyperlinkState_->publishedUri.clear();
    hyperlinkState_->publishedTarget = QPoint(-1, -1);
    hyperlinkState_->publishedCells.clear();
    hyperlinkState_->publishedRelevantRows.clear();
    hyperlinkState_->publishedColumns = 0;
    hyperlinkState_->publishedRows = 0;
    hyperlinkState_->pendingQuery = HyperlinkState::PendingQuery{
        .requestId = requestId,
        .contentRevision = contentRevision,
        .column = column,
        .row = row,
    };
    if (!hyperlinkState_->queryDispatchScheduled) {
        hyperlinkState_->queryDispatchScheduled = true;
        QTimer::singleShot(0, this,
                           &SessionWorker::processPendingHyperlinkQuery);
    }
}

void SessionWorker::cancelHyperlinkQuery(quint64 requestId)
{
    if (requestId == 0) {
        return;
    }
    if (hyperlinkState_->pendingQuery.has_value()
        && hyperlinkState_->pendingQuery->requestId == requestId) {
        hyperlinkState_->pendingQuery.reset();
    }
    if (hyperlinkState_->activeRequestId != requestId) {
        return;
    }
    hyperlinkState_->activeRequestId = 0;
    hyperlinkState_->trackedHover.reset();
    hyperlinkState_->publishedState = TerminalHyperlinkState::Invalid;
    hyperlinkState_->publishedKind = TerminalLinkKind::Osc8;
    hyperlinkState_->publishedUri.clear();
    hyperlinkState_->publishedTarget = QPoint(-1, -1);
    hyperlinkState_->publishedCells.clear();
    hyperlinkState_->publishedRelevantRows.clear();
    hyperlinkState_->publishedColumns = 0;
    hyperlinkState_->publishedRows = 0;
}

void SessionWorker::processPendingHyperlinkQuery()
{
    hyperlinkState_->queryDispatchScheduled = false;
    if (!hyperlinkState_->pendingQuery.has_value()) {
        return;
    }
    const HyperlinkState::PendingQuery query =
        std::exchange(hyperlinkState_->pendingQuery, std::nullopt).value();

    TerminalHyperlinkState state = TerminalHyperlinkState::Invalid;
    TerminalLinkKind kind = TerminalLinkKind::Osc8;
    QByteArray uri;
    QPoint targetCell(-1, -1);
    QVector<QPoint> matchingCells;
    QVector<int> relevantRows;
    std::optional<DetectedTerminalLink> detected;
    const bool coordinateIsStale =
        query.contentRevision != terminalContentRevision_
        || !hyperlinkState_->viewport.hasFrame()
        || hyperlinkState_->viewport.contentRevision()
            != terminalContentRevision_;
    if (coordinateIsStale) {
        state = TerminalHyperlinkState::Stale;
    } else if (vt_ != nullptr) {
        detected = detectTerminalLinkAt(
            *vt_, linkMatcher_.get(), options_.runtime.linkUrl,
            hyperlinkState_->viewport, query.column, query.row);
    }

    if (detected.has_value()) {
        state = TerminalHyperlinkState::Visible;
        kind = detected->resolved.kind;
        uri = detected->resolved.uri;
        targetCell = detected->resolved.targetCell;
        matchingCells = detected->resolved.cells;
        relevantRows = detected->resolved.relevantRows;
        hyperlinkState_->activeRequestId = query.requestId;
        hyperlinkState_->trackedHover = std::move(detected->tracked);
        hyperlinkState_->publishedState = state;
        hyperlinkState_->publishedKind = kind;
        hyperlinkState_->publishedUri = uri;
        hyperlinkState_->publishedTarget = targetCell;
        hyperlinkState_->publishedCells = matchingCells;
        hyperlinkState_->publishedRelevantRows = std::move(relevantRows);
        hyperlinkState_->publishedColumns = hyperlinkState_->viewport.columns();
        hyperlinkState_->publishedRows = hyperlinkState_->viewport.rows();
    }

    Q_EMIT hyperlinkResolved(query.requestId, terminalContentRevision_, state,
                             kind, uri, targetCell, matchingCells);
}

void SessionWorker::prepareHyperlinkActivation(quint64 requestId,
                                               quint64 contentRevision,
                                               int column, int row)
{
    hyperlinkState_->activationRequestId = requestId;
    hyperlinkState_->activationKind = TerminalLinkKind::Osc8;
    hyperlinkState_->trackedActivation.reset();
    if (requestId == 0 || vt_ == nullptr
        || !hyperlinkState_->viewport.hasFrame()) {
        return;
    }
    bool coordinateIsCurrent = contentRevision == terminalContentRevision_
        && hyperlinkState_->viewport.contentRevision()
            == terminalContentRevision_;
    if (!coordinateIsCurrent && hyperlinkState_->trackedHover.has_value()) {
        // An accepted hover already identifies the logical target on the
        // worker. It can safely bridge an unrelated frame revision between
        // the GUI press and this queued operation without trusting a stale
        // viewport coordinate on its own.
        const QPoint pressedCell(column, row);
        const auto hoverMatch = resolveTrackedTerminalLink(
            *vt_, *hyperlinkState_->trackedHover, hyperlinkState_->viewport);
        coordinateIsCurrent =
            hoverMatch.has_value() && hoverMatch->cells.contains(pressedCell);
    }
    if (!coordinateIsCurrent) {
        return;
    }
    auto detected =
        detectTerminalLinkAt(*vt_, linkMatcher_.get(), options_.runtime.linkUrl,
                             hyperlinkState_->viewport, column, row);
    if (!detected.has_value()) {
        return;
    }
    hyperlinkState_->activationKind = detected->resolved.kind;
    hyperlinkState_->trackedActivation = std::move(detected->tracked);
}

void SessionWorker::commitHyperlinkActivation(quint64 requestId, int column,
                                              int row)
{
    QByteArray uri;
    const TerminalLinkKind kind = hyperlinkState_->activationKind;
    if (requestId != 0 && requestId == hyperlinkState_->activationRequestId
        && hyperlinkState_->trackedActivation.has_value() && vt_ != nullptr) {
        const QPoint releaseCell(column, row);
        const auto match = resolveTrackedTerminalLink(
            *vt_, *hyperlinkState_->trackedActivation,
            hyperlinkState_->viewport);
        bool stillMatches = match.has_value() && match->kind == kind
            && match->targetCell == releaseCell
            && match->cells.contains(releaseCell);

        // A tracked text range detects edits within its endpoints. Re-run the
        // configured matcher as well so text appended immediately beside the
        // range cannot silently change the URL opened by this release.
        if (stillMatches && kind == TerminalLinkKind::Regex) {
            const auto current = detectTerminalLinkAt(
                *vt_, linkMatcher_.get(), options_.runtime.linkUrl,
                hyperlinkState_->viewport, column, row);
            stillMatches = current.has_value() && current->resolved.kind == kind
                && current->resolved.uri == match->uri
                && current->resolved.targetCell == releaseCell
                && current->resolved.cells == match->cells;
        }
        // A delayed Shift-click extends through the worker without a GUI
        // motion event. Consult libghostty's release-stable gesture state so
        // that selection drag cannot also commit the armed link.
        if (stillMatches && !vt_->selectionGestureDragged()) {
            uri = match->uri;
        }
    }
    if (requestId == hyperlinkState_->activationRequestId) {
        hyperlinkState_->activationRequestId = 0;
        hyperlinkState_->activationKind = TerminalLinkKind::Osc8;
        hyperlinkState_->trackedActivation.reset();
    }
    Q_EMIT hyperlinkActivationResolved(requestId, terminalContentRevision_,
                                       kind, uri);
}

void SessionWorker::cancelHyperlinkActivation(quint64 requestId)
{
    if (requestId == 0 || requestId != hyperlinkState_->activationRequestId) {
        return;
    }
    hyperlinkState_->activationRequestId = 0;
    hyperlinkState_->activationKind = TerminalLinkKind::Osc8;
    hyperlinkState_->trackedActivation.reset();
}

void SessionWorker::inspectTerminal(quint64 requestId)
{
    if (requestId == 0) return;

    TerminalInspectorSnapshot snapshot =
        vt_ != nullptr ? vt_->inspectorSnapshot() : TerminalInspectorSnapshot{};
    snapshot.contentRevision = terminalContentRevision_;
    Q_EMIT terminalInspectorSnapshotReady(requestId, snapshot);
}

void SessionWorker::inspectTerminalCell(quint64 requestId,
                                        quint64 contentRevision,
                                        int viewportColumn, int viewportRow)
{
    if (requestId == 0) return;

    TerminalInspectorCellSnapshot snapshot;
    snapshot.contentRevision = terminalContentRevision_;
    snapshot.viewportColumn = viewportColumn;
    snapshot.viewportRow = viewportRow;
    if (vt_ != nullptr && contentRevision != terminalContentRevision_) {
        snapshot.status = TerminalInspectorCellStatus::Stale;
    } else if (vt_ != nullptr) {
        snapshot = vt_->inspectorCellSnapshot(viewportColumn, viewportRow);
        // Grid-reference reads can restore a compressed page without changing
        // Ghostty's activity token. Ensure the ordinary bounded compression
        // policy gets another chance to reclaim it.
        scheduleRestoredPageCompression();
        snapshot.contentRevision = terminalContentRevision_;
    }
    Q_EMIT terminalInspectorCellReady(requestId, snapshot);
}

void SessionWorker::scheduleFrame()
{
    if (frameTimer_ != nullptr && !frameTimer_->isActive()) {
        frameTimer_->start();
    }
}

void SessionWorker::refreshTrackedHyperlink(bool force)
{
    if (vt_ == nullptr || hyperlinkState_->activeRequestId == 0
        || !hyperlinkState_->trackedHover.has_value()
        || !hyperlinkState_->viewport.hasFrame()) {
        return;
    }

    TerminalHyperlinkState state = TerminalHyperlinkState::Invalid;
    TerminalLinkKind kind = hyperlinkState_->publishedKind;
    QByteArray uri;
    QPoint targetCell(-1, -1);
    QVector<QPoint> matchingCells;
    QVector<int> relevantRows;

    const auto match = resolveTrackedTerminalLink(
        *vt_, *hyperlinkState_->trackedHover, hyperlinkState_->viewport);
    if (match.has_value()) {
        bool stillMatches = true;
        if (match->kind == TerminalLinkKind::Regex) {
            const auto current = detectTerminalLinkAt(
                *vt_, linkMatcher_.get(), options_.runtime.linkUrl,
                hyperlinkState_->viewport, match->targetCell.x(),
                match->targetCell.y());
            stillMatches = current.has_value()
                && current->resolved.kind == match->kind
                && current->resolved.uri == match->uri
                && current->resolved.targetCell == match->targetCell
                && current->resolved.cells == match->cells;
        }
        if (stillMatches) {
            state = TerminalHyperlinkState::Visible;
            kind = match->kind;
            uri = match->uri;
            targetCell = match->targetCell;
            matchingCells = match->cells;
            relevantRows = match->relevantRows;
        }
    } else if (trackedTerminalLinkValid(*vt_, *hyperlinkState_->trackedHover)) {
        state = TerminalHyperlinkState::Hidden;
        uri = hyperlinkState_->publishedUri;
    }

    const bool changed = force || state != hyperlinkState_->publishedState
        || kind != hyperlinkState_->publishedKind
        || uri != hyperlinkState_->publishedUri
        || targetCell != hyperlinkState_->publishedTarget
        || matchingCells != hyperlinkState_->publishedCells
        || relevantRows != hyperlinkState_->publishedRelevantRows
        || hyperlinkState_->publishedColumns
            != hyperlinkState_->viewport.columns()
        || hyperlinkState_->publishedRows != hyperlinkState_->viewport.rows();
    if (!changed) {
        return;
    }

    hyperlinkState_->publishedState = state;
    hyperlinkState_->publishedKind = kind;
    hyperlinkState_->publishedUri = uri;
    hyperlinkState_->publishedTarget = targetCell;
    hyperlinkState_->publishedCells = matchingCells;
    hyperlinkState_->publishedRelevantRows = std::move(relevantRows);
    hyperlinkState_->publishedColumns = hyperlinkState_->viewport.columns();
    hyperlinkState_->publishedRows = hyperlinkState_->viewport.rows();
    const quint64 requestId = hyperlinkState_->activeRequestId;
    if (state == TerminalHyperlinkState::Invalid) {
        hyperlinkState_->activeRequestId = 0;
        hyperlinkState_->trackedHover.reset();
    }
    Q_EMIT hyperlinkResolved(requestId, terminalContentRevision_, state, kind,
                             uri, targetCell, matchingCells);
}

void SessionWorker::scheduleCompression(int delayMilliseconds)
{
    if (!options_.runtime.scrollbackCompression || vt_ == nullptr
        || compressionTimer_ == nullptr) {
        return;
    }
    compressionTimer_->start(delayMilliseconds);
}

void SessionWorker::scheduleRestoredPageCompression()
{
    // A frontend read must not pull an existing terminal-activity idle
    // deadline forward. The armed pass will reclaim the restored pages; only
    // an otherwise idle scheduler needs a new incremental deadline.
    if (!options_.runtime.scrollbackCompression || vt_ == nullptr
        || compressionTimer_ == nullptr) {
        return;
    }
    if (compressionTraversalPending_) {
        // A read can restore a page already inspected by libghostty's current
        // verification traversal without changing the activity token. The
        // current traversal may therefore report complete with that page
        // resident; remember to start one fresh pass afterward.
        compressionReplayPending_ = true;
    }
    if (compressionTimer_->isActive()) return;
    scheduleCompression(kCompressionStepMilliseconds);
}

void SessionWorker::noteCompressionActivity()
{
    if (vt_ == nullptr || compressionTimer_ == nullptr) {
        return;
    }
    const uint64_t activity = vt_->compressionActivity();
    if (!compressionActivity_.has_value()
        || activity != *compressionActivity_) {
        compressionActivity_ = activity;
        scheduleCompression(kCompressionIdleMilliseconds);
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
    if (!options_.runtime.scrollbackCompression || vt_ == nullptr
        || compressionTimer_ == nullptr) {
        compressionTraversalPending_ = false;
        compressionReplayPending_ = false;
        return;
    }
    compressionTraversalPending_ = vt_->compressScrollback();
    if (compressionTraversalPending_) {
        scheduleCompression(kCompressionStepMilliseconds);
        return;
    }
    if (compressionReplayPending_) {
        compressionReplayPending_ = false;
        scheduleCompression(kCompressionStepMilliseconds);
    }
}

void SessionWorker::publishFrame()
{
    if (vt_ == nullptr) {
        return;
    }
    if (options_.runtime.scrollToBottom.output
        && vt_->observeOutputBottomAnchorChanged()
        && vt_->scrollViewport(
            {.kind = TerminalViewportRequest::Kind::Bottom})) {
        // The output policy runs at the renderer boundary upstream. Apply the
        // viewport mutation before taking this frame snapshot so observers
        // never see a transient pre-scroll update.
        markTerminalContentChanged();
        if (searchState_->active) {
            rebuildSearchVisibleCells();
            publishSearchUpdate();
        }
        noteCompressionActivity();
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
    TerminalUpdate &update = snapshot.update;
    update.contentRevision = terminalContentRevision_;
    update.resetCursorBlink = cursorBlinkResetPending_;
    bool hyperlinkMayHaveChanged = update.fullFrame || update.scrollbarChanged
        || !hyperlinkState_->viewport.hasFrame()
        || hyperlinkState_->viewport.columns() != update.columns
        || hyperlinkState_->viewport.rows() != update.rows;
    if (!hyperlinkMayHaveChanged && hyperlinkState_->activeRequestId != 0) {
        for (const TerminalRowUpdate &row : update.dirtyRows) {
            if (hyperlinkState_->publishedRelevantRows.contains(row.row)
                || row.row == hyperlinkState_->publishedTarget.y()
                || std::ranges::any_of(
                    std::as_const(hyperlinkState_->publishedCells),
                    [&row](const QPoint &cell) { return cell.y() == row.row; })
                || std::ranges::any_of(row.cells, [](const TerminalCell &cell) {
                       return cell.hasHyperlink;
                   })) {
                hyperlinkMayHaveChanged = true;
                break;
            }
        }
    }
    if ((hyperlinkState_->viewport.hasFrame() || update.fullFrame)
        && !hyperlinkState_->viewport.apply(update)) {
        hyperlinkState_->viewport.clear();
    }
    const bool revisionChanged =
        publishedContentRevision_ != terminalContentRevision_;
    if (update.hasChanges() || revisionChanged) {
        cursorBlinkResetPending_ = false;
        publishedContentRevision_ = terminalContentRevision_;
        Q_EMIT terminalUpdated(update);
        if (hyperlinkMayHaveChanged) {
            refreshTrackedHyperlink(hyperlinkState_->publishedState
                                    == TerminalHyperlinkState::Hidden);
        }
        if (searchState_->active) {
            rebuildSearchVisibleCells();
            publishSearchUpdate();
        }
    }
}

void SessionWorker::processDeferredEffects()
{
    if (vt_ == nullptr) {
        return;
    }
    const GhosttyVtAdapter::DeferredEffects effects =
        vt_->takeDeferredEffects();
    if (!effects.title.isNull()) {
        Q_EMIT titleChanged(effects.title);
    }
    if (!effects.currentDirectory.isNull()) {
        Q_EMIT currentDirectoryChanged(effects.currentDirectory);
    }
    if (effects.bell) {
        Q_EMIT bell();
    }
    for (const TerminalClipboardWriteRequest &request :
         effects.clipboardWrites) {
        Q_EMIT terminalClipboardWriteRequested(request);
    }
}

void SessionWorker::checkChild()
{
    if (childPid_ <= 0) {
        return;
    }
    int status = 0;
    const pid_t result =
        ::waitpid(static_cast<pid_t>(childPid_), &status, WNOHANG);
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
        childRuntimeTimer_.invalidate();
        closePty();
        if (!shuttingDown_) {
            Q_EMIT errorOccurred(
                QStringLiteral("The child process was reaped unexpectedly."));
            Q_EMIT sessionExited(127, 0, options_.hold, false, 0, false);
        }
    } else if (result < 0 && errno != EINTR) {
        Q_EMIT errorOccurred(
            QStringLiteral("waitpid failed: %1")
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

    GhosttyVtAdapter::SemanticPromptState semanticState =
        GhosttyVtAdapter::SemanticPromptState::Unavailable;
    if (vt_ != nullptr) {
        semanticState = vt_->semanticPromptState();
        // Sample before every conservative early return. In particular, a
        // prompt reached inside the submission grace window must remain
        // latched if same-process-group work starts before the next poll.
        if (semanticState == GhosttyVtAdapter::SemanticPromptState::AtPrompt) {
            semanticPromptObserved_ = true;
        }
    }

    // forkpty makes the shell the session and process-group leader. While the
    // shell is reading at its prompt it owns the PTY foreground group; a job
    // launched by the shell temporarily replaces it with another group.
    // Treat query failures conservatively so an unusual PTY state cannot make
    // us silently discard active work.
    const pid_t foregroundGroup = masterFd_ >= 0 ? ::tcgetpgrp(masterFd_) : -1;
    if (foregroundGroup < 0
        || foregroundGroup != static_cast<pid_t>(childPid_)) {
        setActiveProcess(true);
        return;
    }

    // Preserve the conservative submission window even if OSC 133 C leaves
    // the current row's prompt marker in place until output advances. A
    // definitive returned prompt will clear activity on the first poll after
    // this short race-bridging interval.
    if (potentialActivityTimer_.isValid()
        && potentialActivityTimer_.elapsed()
            < kPotentialActivityGraceMilliseconds) {
        setActiveProcess(true);
        return;
    }
    potentialActivityTimer_.invalidate();

    switch (semanticState) {
    case GhosttyVtAdapter::SemanticPromptState::AtPrompt:
        setActiveProcess(false);
        return;
    case GhosttyVtAdapter::SemanticPromptState::Away:
    case GhosttyVtAdapter::SemanticPromptState::Unavailable:
        if (semanticPromptExpected_ || semanticPromptObserved_) {
            setActiveProcess(true);
            return;
        }
        break;
    }

    setActiveProcess(false);
}

void SessionWorker::notePotentialActivity()
{
    if (readOnly_ || !running_ || !interactiveShell_) {
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
    // Match Ghostty's process watcher: freeze the exit observation before
    // draining or rendering final PTY output. That post-exit work must not
    // inflate the command runtime across the abnormal-exit threshold.
    int exitCode = 0;
    int signalNumber = 0;
    if (WIFEXITED(status)) {
        exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        signalNumber = WTERMSIG(status);
        exitCode = 128 + signalNumber;
    }

    quint64 runtimeMilliseconds = 0;
    if (childRuntimeTimer_.isValid()) {
        runtimeMilliseconds = static_cast<quint64>(
            std::max<qint64>(0, childRuntimeTimer_.elapsed()));
    }
    childRuntimeTimer_.invalidate();
    const bool abnormal = isAbnormalCommandExit(
        exitCode, signalNumber, runtimeMilliseconds,
        options_.runtime.abnormalCommandExitRuntimeMilliseconds);

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
    waitingForExitKey_ = !shuttingDown_ && !options_.hold
        && (options_.runtime.waitAfterCommand || abnormal);
    closePty();

    if (!shuttingDown_) {
        if (waitingForExitKey_ && vt_ != nullptr) {
            vt_->normalizeKeyboardAfterCommandExit();
            syncKeyboardActionMode();
        }
        Q_EMIT sessionExited(exitCode, signalNumber, options_.hold,
                             waitingForExitKey_, runtimeMilliseconds, abnormal);
    }
}

void SessionWorker::closeChildExitWatcher()
{
    if (childExitNotifier_ != nullptr) {
        delete childExitNotifier_;
        childExitNotifier_ = nullptr;
    }
    if (childExitFd_ >= 0) {
        (void)::close(childExitFd_);
        childExitFd_ = -1;
    }
}

void SessionWorker::closePty()
{
    closeChildExitWatcher();
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
    pendingPastes_.clear();
    heldTerminalModifiers_ = Qt::NoModifier;
    // A keybinding leader may have been staged while the child was alive.
    // Ghostty closes a waited surface when the first post-exit continuation
    // resolves to encoded bytes, so retain that sequence across this one
    // lifecycle boundary. Drop/binding resolutions still consume it without
    // dismissing; every other close path clears it normally.
    if (!waitingForExitKey_) {
        if (shuttingDown_) {
            stagedSequenceTraceResults_.clear();
        } else {
            finalizeStagedKeyboardTraceResults(
                TerminalKeyboardTraceDisposition::SessionUnavailable);
        }
        stagedSequenceBytes_.clear();
        stagedSequenceModifiers_ = Qt::NoModifier;
        activeSequenceToken_ = 0;
        stagedSequencePotentialActivity_ = false;
    }
}

void SessionWorker::shutdown()
{
    if (shuttingDown_ && vt_ == nullptr) {
        return;
    }
    shuttingDown_ = true;
    waitingForExitKey_ = false;
    childRuntimeTimer_.invalidate();
    if (frameTimer_ != nullptr) {
        frameTimer_->stop();
    }
    if (childTimer_ != nullptr) {
        childTimer_->stop();
    }
    if (compressionTimer_ != nullptr) {
        compressionTimer_->stop();
    }
    stopSelectionAutoscroll();
    compressionTraversalPending_ = false;
    compressionReplayPending_ = false;

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
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
        drainPty(true);
        childPid_ = -1;
    }
    running_ = false;
    semanticPromptObserved_ = false;
    semanticPromptExpected_ = false;
    potentialActivityTimer_.invalidate();
    setActiveProcess(false);
    closePty();
    destroyTerminal();
}

void SessionWorker::destroyTerminal()
{
    stopSelectionAutoscroll();
    // Tracked references mutate libghostty bookkeeping when freed. Keep that
    // work serialized on this worker and release every lease before the
    // terminal handle itself is destroyed.
    hyperlinkState_->pendingQuery.reset();
    hyperlinkState_->queryDispatchScheduled = false;
    hyperlinkState_->activeRequestId = 0;
    hyperlinkState_->trackedHover.reset();
    hyperlinkState_->activationRequestId = 0;
    hyperlinkState_->activationKind = TerminalLinkKind::Osc8;
    hyperlinkState_->trackedActivation.reset();
    hyperlinkState_->publishedState = TerminalHyperlinkState::Invalid;
    hyperlinkState_->publishedKind = TerminalLinkKind::Osc8;
    hyperlinkState_->publishedUri.clear();
    hyperlinkState_->publishedTarget = QPoint(-1, -1);
    hyperlinkState_->publishedCells.clear();
    hyperlinkState_->publishedRelevantRows.clear();
    hyperlinkState_->publishedColumns = 0;
    hyperlinkState_->publishedRows = 0;
    hyperlinkState_->viewport.clear();
    *searchState_ = SearchState{};
    vt_.reset();
    linkMatcher_.reset();
}
