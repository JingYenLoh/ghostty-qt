#include "session_worker.h"
#include "ghostty_link_matcher.h"
#include "ghostty_vt_adapter.h"
#include "terminfo_paths.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSet>
#include <QSocketNotifier>
#include <QStandardPaths>
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
constexpr int kCursorBlinkResetThrottleMilliseconds = 500;
constexpr int kShutdownGraceMilliseconds = 2000;
constexpr int kPotentialActivityGraceMilliseconds = 250;
constexpr int kSearchRowsPerChunk = 8;
constexpr int kSearchChunkBudgetMilliseconds = 2;
constexpr int kSearchPublishIntervalMilliseconds = 33;
constexpr quint64 kSearchRowsPerCompressionPass = 64;

char foldSearchByte(char value)
{
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value + ('a' - 'A'))
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

QVector<QPoint> visibleSearchCells(
    TerminalSearchRange range,
    const GhosttyVtAdapter::SearchExtent &extent)
{
    if (range.end < range.start) {
        std::swap(range.start, range.end);
    }
    if (extent.columns <= 0 || extent.viewportLength == 0
        || range.start.column >= extent.columns
        || range.end.column >= extent.columns
        || range.start.screenRow >= extent.totalRows
        || range.end.screenRow >= extent.totalRows) {
        return {};
    }

    const quint64 viewportStart = extent.viewportOffset;
    const quint64 viewportEnd = viewportStart + extent.viewportLength;
    const quint64 firstRow = std::max<quint64>(
        range.start.screenRow, viewportStart);
    const quint64 lastRowExclusive = std::min<quint64>(
        static_cast<quint64>(range.end.screenRow) + 1U, viewportEnd);
    if (firstRow >= lastRowExclusive) {
        return {};
    }

    QVector<QPoint> result;
    for (quint64 row = firstRow; row < lastRowExclusive; ++row) {
        const int firstColumn = row == range.start.screenRow
            ? static_cast<int>(range.start.column) : 0;
        const int lastColumn = row == range.end.screenRow
            ? static_cast<int>(range.end.column) : extent.columns - 1;
        const int viewportRow = static_cast<int>(row - viewportStart);
        for (int column = firstColumn; column <= lastColumn; ++column) {
            result.append(QPoint(column, viewportRow));
        }
    }
    return result;
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

int hexDigitValue(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool appendUtf8(QByteArray *output, quint32 codepoint)
{
    if (codepoint <= 0x7fU) {
        output->append(static_cast<char>(codepoint));
        return true;
    }
    if (codepoint <= 0x7ffU) {
        output->append(static_cast<char>(0xc0U | (codepoint >> 6U)));
        output->append(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        return true;
    }
    if (codepoint >= 0xd800U && codepoint <= 0xdfffU) {
        return false;
    }
    if (codepoint <= 0xffffU) {
        output->append(static_cast<char>(0xe0U | (codepoint >> 12U)));
        output->append(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output->append(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        return true;
    }
    if (codepoint <= 0x10ffffU) {
        output->append(static_cast<char>(0xf0U | (codepoint >> 18U)));
        output->append(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
        output->append(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output->append(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        return true;
    }
    return false;
}

enum class HexEscapeEncoding {
    RawByte,
    Utf8Codepoint,
};

std::optional<QByteArray> decodeZigEscapes(
    const QByteArray &serialized, HexEscapeEncoding hexEscapeEncoding)
{
    QByteArray decoded;
    decoded.reserve(serialized.size());
    qsizetype index = 0;
    while (index < serialized.size()) {
        const char value = serialized.at(index++);
        if (value != '\\') {
            decoded.append(value);
            continue;
        }
        if (index >= serialized.size()) {
            return std::nullopt;
        }

        const char escaped = serialized.at(index++);
        quint32 codepoint = 0;
        switch (escaped) {
        case 'n': codepoint = '\n'; break;
        case 'r': codepoint = '\r'; break;
        case 't': codepoint = '\t'; break;
        case '\\': codepoint = '\\'; break;
        case '\'': codepoint = '\''; break;
        case '"': codepoint = '"'; break;
        case 'x': {
            if (index + 2 > serialized.size()) {
                return std::nullopt;
            }
            const int high = hexDigitValue(serialized.at(index));
            const int low = hexDigitValue(serialized.at(index + 1));
            if (high < 0 || low < 0) {
                return std::nullopt;
            }
            codepoint = static_cast<quint32>((high << 4) | low);
            index += 2;
            if (hexEscapeEncoding == HexEscapeEncoding::RawByte) {
                decoded.append(static_cast<char>(codepoint));
                continue;
            }
            break;
        }
        case 'u': {
            if (index >= serialized.size() || serialized.at(index++) != '{'
                || index >= serialized.size()
                || serialized.at(index) == '}') {
                return std::nullopt;
            }
            bool closed = false;
            while (index < serialized.size()) {
                const char digit = serialized.at(index++);
                if (digit == '}') {
                    closed = true;
                    break;
                }
                const int nibble = hexDigitValue(digit);
                if (nibble < 0) {
                    return std::nullopt;
                }
                codepoint = codepoint * 16U + static_cast<quint32>(nibble);
                if (codepoint > 0x10ffffU) {
                    return std::nullopt;
                }
            }
            if (!closed) {
                return std::nullopt;
            }
            break;
        }
        default:
            return std::nullopt;
        }

        // Ghostty's config string parser treats every escape as a Unicode
        // codepoint and UTF-8 encodes it. The Action.format boundary is the
        // one exception: std.zig.stringEscape uses \xNN to preserve each raw
        // byte, so its inverse appends those bytes in the branch above.
        if (!appendUtf8(&decoded, codepoint)) {
            return std::nullopt;
        }
    }
    return decoded;
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
    return static_cast<uint16_t>(std::clamp(value, 1, static_cast<int>(UINT16_MAX)));
}

using TrackedTerminalLink = std::variant<
    GhosttyVtAdapter::TrackedHyperlink,
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

QVector<QPoint> osc8Candidates(const TerminalFrame &frame)
{
    QVector<QPoint> result;
    if (frame.columns <= 0) {
        return result;
    }
    result.reserve(frame.cells.size());
    for (int index = 0; index < frame.cells.size(); ++index) {
        if (frame.cells.at(index).hasHyperlink) {
            result.append(QPoint(index % frame.columns, index / frame.columns));
        }
    }
    return result;
}

std::optional<ResolvedTerminalLink> resolveTrackedTerminalLink(
    GhosttyVtAdapter &adapter, const TrackedTerminalLink &tracked,
    const TerminalFrame &frame)
{
    if (const auto *osc8 = std::get_if<
            GhosttyVtAdapter::TrackedHyperlink>(&tracked)) {
        const auto match = adapter.resolveHyperlink(
            *osc8, osc8Candidates(frame));
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

bool trackedTerminalLinkValid(
    GhosttyVtAdapter &adapter, const TrackedTerminalLink &tracked)
{
    if (const auto *osc8 = std::get_if<
            GhosttyVtAdapter::TrackedHyperlink>(&tracked)) {
        return adapter.trackedHyperlinkValid(*osc8);
    }
    return adapter.trackedTextRangeValid(
        std::get<GhosttyVtAdapter::TrackedTextRange>(tracked));
}

std::optional<DetectedTerminalLink> detectTerminalLinkAt(
    GhosttyVtAdapter &adapter, GhosttyLinkMatcher *matcher,
    bool linkUrl, const TerminalFrame &frame, int column, int row)
{
    if (column < 0 || column >= frame.columns
        || row < 0 || row >= frame.rows) {
        return std::nullopt;
    }
    const int index = row * frame.columns + column;
    if (index < 0 || index >= frame.cells.size()) {
        return std::nullopt;
    }

    // Ghostty gives explicit OSC 8 destinations priority over configured
    // regex links at the same cell. If URI extraction fails, continue to the
    // default matcher just as Surface.linkAtPos does.
    auto trackedOsc8 = adapter.trackHyperlinkAt(column, row);
    if (trackedOsc8.has_value()) {
        TrackedTerminalLink target(std::in_place_type<
            GhosttyVtAdapter::TrackedHyperlink>, std::move(*trackedOsc8));
        auto resolved = resolveTrackedTerminalLink(adapter, target, frame);
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
        const GhosttyLinkMatchResult result = matcher->findNext(
            line->text(), searchOffset, &byteMatch);
        if (result != GhosttyLinkMatchResult::Match) {
            break;
        }
        if (line->byteRangeContainsTarget(
                byteMatch.beginByte, byteMatch.endByte)) {
            foundTargetMatch = true;
            break;
        }
        searchOffset = byteMatch.endByte;
    }
    if (!foundTargetMatch) {
        return std::nullopt;
    }
    auto tracked = adapter.trackTextRange(
        *line, byteMatch.beginByte, byteMatch.endByte);
    if (!tracked.has_value()) {
        return std::nullopt;
    }
    TrackedTerminalLink target(std::in_place_type<
        GhosttyVtAdapter::TrackedTextRange>, std::move(*tracked));
    auto resolved = resolveTrackedTerminalLink(adapter, target, frame);
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

    TerminalFrame frame;
    bool hasFrame = false;
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
    QSet<QPoint> visibleCells;
    QVector<QPoint> selectedCells;
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
    : QObject(parent)
    , hyperlinkState_(std::make_unique<HyperlinkState>())
    , searchState_(std::make_unique<SearchState>())
{
}

SessionWorker::~SessionWorker()
{
    shutdown();
}

bool SessionWorker::canonicalBytesMayStartProcess(const QByteArray &payload)
{
    const std::optional<QByteArray> decoded = decodeZigEscapes(
        payload, HexEscapeEncoding::RawByte);
    return decoded.has_value() && bytesMayStartProcess(*decoded);
}

bool SessionWorker::canonicalTextMayStartProcess(const QByteArray &payload)
{
    const std::optional<QByteArray> actionPayload = decodeZigEscapes(
        payload, HexEscapeEncoding::RawByte);
    if (!actionPayload.has_value()) {
        return false;
    }
    const std::optional<QByteArray> text = decodeZigEscapes(
        *actionPayload, HexEscapeEncoding::Utf8Codepoint);
    return text.has_value() && bytesMayStartProcess(*text);
}

void SessionWorker::initialize(const TerminalSessionLaunchOptions &options)
{
    if (vt_ != nullptr || running_) {
        return;
    }

    options_ = options;
    shuttingDown_ = false;
    potentialActivityTimer_.invalidate();
    cursorBlinkResetTimer_.invalidate();
    cursorBlinkResetPending_ = false;
    stagedSequenceBytes_.clear();
    newestSequenceToken_ = 0;
    activeSequenceToken_ = 0;
    stagedSequencePotentialActivity_ = false;
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
    hyperlinkState_->frame = TerminalFrame{};
    hyperlinkState_->hasFrame = false;

    linkMatcher_ = std::make_unique<GhosttyLinkMatcher>();

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
        Q_EMIT sessionExited(127, 0, options_.hold);
        return;
    }

    publishFrame();
    if (!spawnChild()) {
        Q_EMIT sessionExited(127, 0, options_.hold);
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
        .appearance = options_.runtime.appearance,
    };
    GhosttyVtAdapter::Callbacks callbacks;
    callbacks.writePty = [this](const QByteArray &data) { queuePtyWrite(data); };
    vt_ = GhosttyVtAdapter::create(options, std::move(callbacks));
    if (vt_ != nullptr) {
        compressionActivity_ = vt_->compressionActivity();
    }
    return vt_ != nullptr;
}

void SessionWorker::applyRuntimeOptions(
    const TerminalSessionRuntimeOptions &options)
{
    const bool appearanceChanged =
        options_.runtime.appearance != options.appearance;
    const bool linkUrlChanged = options_.runtime.linkUrl != options.linkUrl;
    options_.runtime = options;

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
                Q_EMIT hyperlinkResolved(
                    requestId, terminalContentRevision_,
                    TerminalHyperlinkState::Invalid,
                    TerminalLinkKind::Regex, {}, QPoint(-1, -1), {});
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
    // limits remain pane-owned and apply when a new pane is constructed.
    if (vt_ != nullptr && appearanceChanged) {
        if (!vt_->setAppearance(options.appearance)) {
            Q_EMIT errorOccurred(
                QStringLiteral("Failed to apply terminal appearance to libghostty-vt."));
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
        markTerminalContentChanged();
        processDeferredEffects();
        syncSelectionAvailability();
        noteCompressionActivity();
        scheduleFrame();
    }

    columns_ = nextColumns;
    rows_ = nextRows;
    cellWidthPixels_ = nextCellWidth;
    cellHeightPixels_ = nextCellHeight;
    surfaceWidthPixels_ = nextSurfaceWidth;
    surfaceHeightPixels_ = nextSurfaceHeight;

    if (vt_ != nullptr) {
        markSearchContentChanged();
    }

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
        markTerminalContentChanged();
        markSearchContentChanged();
        // VT input may change mouse tracking or encoding modes. Synchronize
        // once per output batch without resetting motion deduplication for
        // every individual pointer event.
        syncMouseEncoder();
        syncSelectionAvailability();
        processDeferredEffects();
        noteCompressionActivity();
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
    if (vt_ == nullptr || masterFd_ < 0) {
        return;
    }
    const GhosttyVtAdapter::EncodedKey encoded = vt_->encodeKey(input);
    if (encoded.bytes.isEmpty()) {
        return;
    }
    queuePtyWrite(encoded.bytes);
    clearSelectionAfterKey(encoded.modifier, encoded.escape);
}

void SessionWorker::stageSequenceKey(quint64 token,
                                     const TerminalKeyInput &input)
{
    if (token == 0) {
        return;
    }
    if (token != activeSequenceToken_) {
        if (!sequenceTokenIsNewer(token, newestSequenceToken_)) {
            return;
        }
        // A newer sequence supersedes an unresolved one. Its held bytes must
        // not leak into the new match attempt.
        newestSequenceToken_ = token;
        activeSequenceToken_ = token;
        stagedSequenceBytes_.clear();
        stagedSequencePotentialActivity_ = false;
    }

    if (vt_ == nullptr) {
        return;
    }
    const QByteArray encoded = vt_->encodeKey(input).bytes;
    if (encoded.isEmpty()) {
        return;
    }
    stagedSequenceBytes_.append(encoded);
    stagedSequencePotentialActivity_ =
        stagedSequencePotentialActivity_ || keyMayStartProcess(input);
}

void SessionWorker::resolveSequence(quint64 token,
                                    TerminalSequenceResolution resolution,
                                    bool hasCurrent,
                                    const TerminalKeyInput &current)
{
    if (token == 0 || token != activeSequenceToken_) {
        return;
    }

    QByteArray bytes;
    bool potentialActivity = false;
    bool currentModifier = true;
    bool currentEscape = false;
    bool currentEncoded = false;
    if (resolution == TerminalSequenceResolution::Flush
        || resolution == TerminalSequenceResolution::FlushAndSendCurrent) {
        bytes = std::move(stagedSequenceBytes_);
        potentialActivity = stagedSequencePotentialActivity_
            && !bytes.isEmpty();
    }
    if (resolution == TerminalSequenceResolution::FlushAndSendCurrent
        && hasCurrent) {
        const GhosttyVtAdapter::EncodedKey encodedCurrent =
            vt_ != nullptr ? vt_->encodeKey(current)
                           : GhosttyVtAdapter::EncodedKey{};
        if (!encodedCurrent.bytes.isEmpty()) {
            bytes.append(encodedCurrent.bytes);
            potentialActivity = potentialActivity
                || keyMayStartProcess(current);
            currentModifier = encodedCurrent.modifier;
            currentEscape = encodedCurrent.escape;
            currentEncoded = true;
        }
    }

    activeSequenceToken_ = 0;
    stagedSequenceBytes_.clear();
    stagedSequencePotentialActivity_ = false;

    // Append once so the staged leaders and the resolving key cannot be
    // interleaved by another queued operation on this worker thread.
    if (!bytes.isEmpty() && masterFd_ >= 0) {
        if (potentialActivity) {
            notePotentialActivity();
        }
        queuePtyWrite(bytes);
        if (currentEncoded) {
            clearSelectionAfterKey(currentModifier, currentEscape);
        }
    }
}

void SessionWorker::sendInputMethod(const TerminalInputMethodInput &input)
{
    if (input.preeditTransition
        && options_.runtime.selectionClipboard.clearOnTyping) {
        clearSelectionState();
    }

    if (input.commitText.isEmpty() || vt_ == nullptr || masterFd_ < 0) {
        return;
    }
    if (input.commitText.contains(u'\n')
        || input.commitText.contains(u'\r')) {
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
    queuePtyWrite(encoded.bytes);
    clearSelectionAfterKey(encoded.modifier, encoded.escape);
}

void SessionWorker::sendCsi(const QByteArray &payload)
{
    const std::optional<QByteArray> decoded = decodeZigEscapes(
        payload, HexEscapeEncoding::RawByte);
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
    const std::optional<QByteArray> decoded = decodeZigEscapes(
        payload, HexEscapeEncoding::RawByte);
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
    const std::optional<QByteArray> actionPayload = decodeZigEscapes(
        serializedText, HexEscapeEncoding::RawByte);
    if (!actionPayload.has_value()) {
        return;
    }
    const std::optional<QByteArray> text = decodeZigEscapes(
        *actionPayload, HexEscapeEncoding::Utf8Codepoint);
    if (!text.has_value()) {
        return;
    }
    sendRawAction(*text);
}

void SessionWorker::sendRawAction(const QByteArray &data)
{
    if (bytesMayStartProcess(data)) {
        notePotentialActivity();
    }
    queuePtyWrite(data);
    scrollToBottomForInput();
}

void SessionWorker::resetTerminal()
{
    if (vt_ == nullptr) {
        return;
    }

    vt_->reset();
    markTerminalContentChanged();
    markSearchContentChanged();
    syncMouseEncoder();
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

void SessionWorker::clearSelectionAfterKey(bool modifier, bool escape)
{
    if (!modifier
        && (options_.runtime.selectionClipboard.clearOnTyping || escape)) {
        clearSelectionState();
    }
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
    if (text.isEmpty() || vt_ == nullptr || masterFd_ < 0) {
        return;
    }

    const TerminalClipboardPasteOptions &options =
        options_.runtime.clipboardPaste;
    const GhosttyVtAdapter::PreparedPaste prepared = vt_->preparePaste(
        text, {
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
        text, {
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

void SessionWorker::commitPaste(const QString &text,
                                const QByteArray &encoded)
{
    if (encoded.isEmpty()) {
        return;
    }
    if (text.contains(u'\n') || text.contains(u'\r')) {
        notePotentialActivity();
    }
    // Ghostty returns to the active screen before making accepted paste data
    // visible to the child. Rejected and cancelled requests never get here.
    scrollToBottomForInput();
    queuePtyWrite(encoded);
}

void SessionWorker::scrollToBottomForInput()
{
    scrollViewport({.kind = TerminalViewportRequest::Kind::Bottom});
}

void SessionWorker::copySelection()
{
    copySelectionTo(TerminalClipboardDestination::Standard,
                    options_.runtime.selectionClipboard.clearOnCopy);
}

void SessionWorker::copySelectionTo(
    TerminalClipboardDestination destination, bool clearAfterCopy)
{
    if (vt_ == nullptr) {
        return;
    }

    const QString text = vt_->selectedText(
        options_.runtime.selectionClipboard.trimTrailingSpaces);
    if (text.isNull()) {
        return;
    }

    Q_EMIT clipboardTextReady(text, destination);
    if (clearAfterCopy) {
        clearSelectionState();
    }
}

void SessionWorker::copySelectionOnSelect()
{
    switch (options_.runtime.selectionClipboard.copyOnSelect) {
    case TerminalCopyOnSelectMode::Disabled:
        return;
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
        // Selection dragging may scroll the viewport atomically inside
        // libghostty, invalidating viewport-relative hyperlink coordinates.
        markTerminalContentChanged();
        if (searchState_->active) {
            rebuildSearchVisibleCells();
            publishSearchUpdate();
        }
        syncSelectionAvailability();
        scheduleFrame();
    }
}

void SessionWorker::endSelection(int column, int row)
{
    if (vt_ != nullptr) {
        vt_->endSelection(column, row);
        syncSelectionAvailability();
        copySelectionOnSelect();
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

void SessionWorker::adjustSelection(TerminalSelectionAdjustment adjustment)
{
    if (vt_ != nullptr && vt_->adjustSelection(adjustment)) {
        markTerminalContentChanged();
        if (searchState_->active) {
            rebuildSearchVisibleCells();
            publishSearchUpdate();
        }
        syncSelectionAvailability();
        scheduleFrame();
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
        scheduleFrame();
    }
}

void SessionWorker::beginSearch(quint64 generation, const QByteArray &needle)
{
    if (generation == 0) {
        return;
    }
    if (searchState_->generation != 0
        && generation != searchState_->generation
        && !sequenceTokenIsNewer(generation, searchState_->generation)) {
        return;
    }
    if (generation != searchState_->generation
        && searchState_->active
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
        compressionTimer_->start(0);
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

    searchState_->active = true;
    searchState_->totalRows = extent->totalRows;
    searchState_->columns = extent->columns;
    searchState_->rows = extent->rows;
    searchState_->viewportOffset = extent->viewportOffset;
    searchState_->viewportLength = extent->viewportLength;
    searchState_->activeScreen = extent->activeScreen;
    searchState_->nextScreenRow = extent->totalRows == 0
        ? -1 : static_cast<qint64>(extent->totalRows - 1U);
    searchState_->reversedNeedle = reversedFoldedSearchNeedle(needle);
    searchState_->prefix = searchPrefixTable(searchState_->reversedNeedle);
    publishSearchUpdate();
    scheduleSearchChunk();
}

void SessionWorker::search(quint64 generation, const QByteArray &needle)
{
    beginSearch(generation, needle);
}

void SessionWorker::searchSerialized(
    quint64 generation, const QByteArray &serializedNeedle)
{
    const std::optional<QByteArray> needle = decodeZigEscapes(
        serializedNeedle, HexEscapeEncoding::RawByte);
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
    if (searchState_->generation != 0
        && generation != searchState_->generation
        && !sequenceTokenIsNewer(generation, searchState_->generation)) {
        return;
    }
    const bool chunkScheduled = searchState_->chunkScheduled;
    if ((searchState_->rowsSinceCompressionPass > 0
         || searchState_->currentRow.has_value())
        && compressionTimer_ != nullptr) {
        compressionTimer_->start(0);
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
    update.visibleCells = searchState_->visibleCells.values();
    update.selectedCells = searchState_->selectedCells;
    Q_EMIT searchUpdated(update);
    searchState_->lastPublication.restart();
}

void SessionWorker::rebuildSearchVisibleCells()
{
    searchState_->visibleCells.clear();
    searchState_->selectedCells.clear();
    if (!searchState_->active || vt_ == nullptr) {
        return;
    }
    const std::optional<GhosttyVtAdapter::SearchExtent> extent =
        vt_->searchExtent();
    if (!extent.has_value()
        || extent->totalRows != searchState_->totalRows
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
        const QVector<QPoint> cells = visibleSearchCells(range, *extent);
        for (const QPoint &cell : cells) {
            searchState_->visibleCells.insert(cell);
        }
    }
    if (searchState_->selectedMatch >= 0
        && searchState_->selectedMatch < searchState_->matches.size()) {
        searchState_->selectedCells = visibleSearchCells(
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
        const QVector<QPoint> visible = visibleSearchCells(range, extent);
        for (const QPoint &visibleCell : visible) {
            state.visibleCells.insert(visibleCell);
        }
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
            const quint32 screenRow = static_cast<quint32>(
                searchState_->nextScreenRow--);
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
        compressionTimer_->start(0);
    }
    if (searchState_->complete
        || !searchState_->lastPublication.isValid()
        || searchState_->lastPublication.elapsed()
            >= kSearchPublishIntervalMilliseconds) {
        publishSearchUpdate();
    }
    scheduleSearchChunk();
}

void SessionWorker::navigateSearch(
    quint64 generation, TerminalSearchDirection direction)
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
            ? 0 : (searchState_->selectedMatch + 1) % count;
    } else {
        searchState_->selectedMatch = searchState_->selectedMatch < 0
            ? count - 1
            : (searchState_->selectedMatch + count - 1) % count;
    }

    const TerminalSearchRange &range = searchState_->matches.at(
        searchState_->selectedMatch);
    const bool viewportChanged = vt_ != nullptr
        && vt_->scrollSearchRangeIntoView(range);
    if (viewportChanged) {
        markTerminalContentChanged();
        scheduleFrame();
    }
    rebuildSearchVisibleCells();
    publishSearchUpdate();
}

void SessionWorker::requestSearchSelection(quint64 requestId)
{
    if (requestId == 0) {
        return;
    }
    const bool available = vt_ != nullptr && vt_->hasSelection();
    const QString text = available ? vt_->selectedText(false) : QString{};
    Q_EMIT searchSelectionReady(requestId, available, text);
}

void SessionWorker::queryHyperlink(
    quint64 requestId, quint64 contentRevision, int column, int row)
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
        QTimer::singleShot(
            0, this, &SessionWorker::processPendingHyperlinkQuery);
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
    const bool coordinateIsStale = query.contentRevision
            != terminalContentRevision_
        || !hyperlinkState_->hasFrame
        || hyperlinkState_->frame.contentRevision
            != terminalContentRevision_;
    if (coordinateIsStale) {
        state = TerminalHyperlinkState::Stale;
    } else if (vt_ != nullptr) {
        detected = detectTerminalLinkAt(
            *vt_, linkMatcher_.get(), options_.runtime.linkUrl,
            hyperlinkState_->frame, query.column, query.row);
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
        hyperlinkState_->publishedColumns = hyperlinkState_->frame.columns;
        hyperlinkState_->publishedRows = hyperlinkState_->frame.rows;
    }

    Q_EMIT hyperlinkResolved(query.requestId, terminalContentRevision_, state,
                             kind, uri, targetCell, matchingCells);
}

void SessionWorker::prepareHyperlinkActivation(
    quint64 requestId, quint64 contentRevision, int column, int row)
{
    hyperlinkState_->activationRequestId = requestId;
    hyperlinkState_->activationKind = TerminalLinkKind::Osc8;
    hyperlinkState_->trackedActivation.reset();
    if (requestId == 0 || vt_ == nullptr
        || !hyperlinkState_->hasFrame) {
        return;
    }
    bool coordinateIsCurrent = contentRevision == terminalContentRevision_
        && hyperlinkState_->frame.contentRevision
            == terminalContentRevision_;
    if (!coordinateIsCurrent && hyperlinkState_->trackedHover.has_value()) {
        // An accepted hover already identifies the logical target on the
        // worker. It can safely bridge an unrelated frame revision between
        // the GUI press and this queued operation without trusting a stale
        // viewport coordinate on its own.
        const QPoint pressedCell(column, row);
        const auto hoverMatch = resolveTrackedTerminalLink(
            *vt_, *hyperlinkState_->trackedHover, hyperlinkState_->frame);
        coordinateIsCurrent = hoverMatch.has_value()
            && hoverMatch->cells.contains(pressedCell);
    }
    if (!coordinateIsCurrent) {
        return;
    }
    auto detected = detectTerminalLinkAt(
        *vt_, linkMatcher_.get(), options_.runtime.linkUrl,
        hyperlinkState_->frame, column, row);
    if (!detected.has_value()) {
        return;
    }
    hyperlinkState_->activationKind = detected->resolved.kind;
    hyperlinkState_->trackedActivation = std::move(detected->tracked);
}

void SessionWorker::commitHyperlinkActivation(
    quint64 requestId, int column, int row)
{
    QByteArray uri;
    const TerminalLinkKind kind = hyperlinkState_->activationKind;
    if (requestId != 0
        && requestId == hyperlinkState_->activationRequestId
        && hyperlinkState_->trackedActivation.has_value() && vt_ != nullptr) {
        const QPoint releaseCell(column, row);
        const auto match = resolveTrackedTerminalLink(
            *vt_, *hyperlinkState_->trackedActivation,
            hyperlinkState_->frame);
        bool stillMatches = match.has_value()
            && match->kind == kind
            && match->targetCell == releaseCell
            && match->cells.contains(releaseCell);

        // A tracked text range detects edits within its endpoints. Re-run the
        // configured matcher as well so text appended immediately beside the
        // range cannot silently change the URL opened by this release.
        if (stillMatches && kind == TerminalLinkKind::Regex) {
            const auto current = detectTerminalLinkAt(
                *vt_, linkMatcher_.get(), options_.runtime.linkUrl,
                hyperlinkState_->frame, column, row);
            stillMatches = current.has_value()
                && current->resolved.kind == kind
                && current->resolved.uri == match->uri
                && current->resolved.targetCell == releaseCell
                && current->resolved.cells == match->cells;
        }
        if (stillMatches) {
            uri = match->uri;
        }
    }
    if (requestId == hyperlinkState_->activationRequestId) {
        hyperlinkState_->activationRequestId = 0;
        hyperlinkState_->activationKind = TerminalLinkKind::Osc8;
        hyperlinkState_->trackedActivation.reset();
    }
    Q_EMIT hyperlinkActivationResolved(
        requestId, terminalContentRevision_, kind, uri);
}

void SessionWorker::cancelHyperlinkActivation(quint64 requestId)
{
    if (requestId == 0
        || requestId != hyperlinkState_->activationRequestId) {
        return;
    }
    hyperlinkState_->activationRequestId = 0;
    hyperlinkState_->activationKind = TerminalLinkKind::Osc8;
    hyperlinkState_->trackedActivation.reset();
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
        || !hyperlinkState_->hasFrame) {
        return;
    }

    TerminalHyperlinkState state = TerminalHyperlinkState::Invalid;
    TerminalLinkKind kind = hyperlinkState_->publishedKind;
    QByteArray uri;
    QPoint targetCell(-1, -1);
    QVector<QPoint> matchingCells;
    QVector<int> relevantRows;

    const auto match = resolveTrackedTerminalLink(
        *vt_, *hyperlinkState_->trackedHover, hyperlinkState_->frame);
    if (match.has_value()) {
        bool stillMatches = true;
        if (match->kind == TerminalLinkKind::Regex) {
            const auto current = detectTerminalLinkAt(
                *vt_, linkMatcher_.get(), options_.runtime.linkUrl,
                hyperlinkState_->frame,
                match->targetCell.x(), match->targetCell.y());
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
    } else if (trackedTerminalLinkValid(
                   *vt_, *hyperlinkState_->trackedHover)) {
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
            != hyperlinkState_->frame.columns
        || hyperlinkState_->publishedRows != hyperlinkState_->frame.rows;
    if (!changed) {
        return;
    }

    hyperlinkState_->publishedState = state;
    hyperlinkState_->publishedKind = kind;
    hyperlinkState_->publishedUri = uri;
    hyperlinkState_->publishedTarget = targetCell;
    hyperlinkState_->publishedCells = matchingCells;
    hyperlinkState_->publishedRelevantRows = std::move(relevantRows);
    hyperlinkState_->publishedColumns = hyperlinkState_->frame.columns;
    hyperlinkState_->publishedRows = hyperlinkState_->frame.rows;
    const quint64 requestId = hyperlinkState_->activeRequestId;
    if (state == TerminalHyperlinkState::Invalid) {
        hyperlinkState_->activeRequestId = 0;
        hyperlinkState_->trackedHover.reset();
    }
    Q_EMIT hyperlinkResolved(requestId, terminalContentRevision_, state,
                             kind, uri, targetCell, matchingCells);
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
    TerminalUpdate update = std::move(snapshot.update);
    update.contentRevision = terminalContentRevision_;
    update.resetCursorBlink = cursorBlinkResetPending_;
    bool hyperlinkMayHaveChanged = update.fullFrame
        || update.scrollbarChanged || !hyperlinkState_->hasFrame
        || hyperlinkState_->frame.columns != update.columns
        || hyperlinkState_->frame.rows != update.rows;
    if (!hyperlinkMayHaveChanged
        && hyperlinkState_->activeRequestId != 0) {
        for (const TerminalRowUpdate &row : update.dirtyRows) {
            if (hyperlinkState_->publishedRelevantRows.contains(row.row)
                || row.row == hyperlinkState_->publishedTarget.y()
                || std::ranges::any_of(
                    std::as_const(hyperlinkState_->publishedCells),
                    [&row](const QPoint &cell) {
                        return cell.y() == row.row;
                    })
                || std::ranges::any_of(
                    row.cells,
                    [](const TerminalCell &cell) {
                        return cell.hasHyperlink;
                    })) {
                hyperlinkMayHaveChanged = true;
                break;
            }
        }
    }
    if (hyperlinkState_->hasFrame || update.fullFrame) {
        hyperlinkState_->hasFrame = applyTerminalUpdate(
            &hyperlinkState_->frame, update);
    }
    const bool revisionChanged =
        publishedContentRevision_ != terminalContentRevision_;
    if (update.hasChanges() || revisionChanged) {
        cursorBlinkResetPending_ = false;
        publishedContentRevision_ = terminalContentRevision_;
        Q_EMIT terminalUpdated(update);
        if (hyperlinkMayHaveChanged) {
            refreshTrackedHyperlink(
                hyperlinkState_->publishedState
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
            Q_EMIT sessionExited(127, 0, options_.hold);
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
        Q_EMIT sessionExited(exitCode, signalNumber, options_.hold);
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
    pendingPastes_.clear();
    stagedSequenceBytes_.clear();
    activeSequenceToken_ = 0;
    stagedSequencePotentialActivity_ = false;
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
    hyperlinkState_->frame = TerminalFrame{};
    hyperlinkState_->hasFrame = false;
    *searchState_ = SearchState{};
    vt_.reset();
    linkMatcher_.reset();
}
