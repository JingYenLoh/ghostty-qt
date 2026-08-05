#include "ghostty_vt_adapter.h"
#include "session_worker.h"
#include "terminal_types.h"
#include "terminfo_paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStringView>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <linux/input-event-codes.h>

#include <algorithm>
#include <csignal>
#include <limits>
#include <optional>
#include <utility>

#include <sys/stat.h>

namespace {

QString frameText(const TerminalFrame &frame)
{
    QString text;
    text.reserve(frame.cells.size());
    for (const TerminalCell &cell : frame.cells) {
        text.append(cell.text);
    }
    return text;
}

TerminalFrame accumulatedFrame(const QSignalSpy &spy)
{
    TerminalFrame frame;
    for (const QList<QVariant> &arguments : spy) {
        const bool applied = applyTerminalUpdate(
            frame, qvariant_cast<TerminalUpdate>(arguments.constFirst()));
        Q_ASSERT(applied);
    }
    return frame;
}

bool updatesContain(const QSignalSpy &spy, const QString &needle)
{
    return frameText(accumulatedFrame(spy)).contains(needle);
}

bool containsCursorBlinkReset(const QSignalSpy &spy)
{
    return std::any_of(spy.cbegin(), spy.cend(), [](const QList<QVariant> &args) {
        return !args.isEmpty()
            && qvariant_cast<TerminalUpdate>(args.constFirst()).resetCursorBlink;
    });
}

bool containsFullFrame(const QSignalSpy &spy)
{
    return std::any_of(spy.cbegin(), spy.cend(), [](const QList<QVariant> &args) {
        return !args.isEmpty()
            && qvariant_cast<TerminalUpdate>(args.constFirst()).fullFrame;
    });
}

bool spyContainsBool(const QSignalSpy &spy, bool expected)
{
    return std::any_of(spy.cbegin(), spy.cend(), [expected](const QList<QVariant> &args) {
        return !args.isEmpty() && args.constFirst().toBool() == expected;
    });
}

TerminalActionResult terminalActionResultAt(
    const QSignalSpy &spy, qsizetype index)
{
    Q_ASSERT(index >= 0 && index < spy.size());
    Q_ASSERT(!spy.at(index).isEmpty());
    return qvariant_cast<TerminalActionResult>(
        spy.at(index).constFirst());
}

TerminalRightClickResult rightClickResultAt(const QSignalSpy &spy,
                                            qsizetype index)
{
    Q_ASSERT(index >= 0 && index < spy.size());
    Q_ASSERT(!spy.at(index).isEmpty());
    return qvariant_cast<TerminalRightClickResult>(spy.at(index).constFirst());
}

std::optional<TerminalSearchUpdate> latestSearchUpdate(
    const QSignalSpy &spy, quint64 generation)
{
    for (auto iterator = spy.crbegin(); iterator != spy.crend(); ++iterator) {
        const TerminalSearchUpdate update =
            qvariant_cast<TerminalSearchUpdate>(iterator->constFirst());
        if (update.generation == generation) {
            return update;
        }
    }
    return std::nullopt;
}

bool maskIsSubset(const QBitArray &subset, const QBitArray &superset)
{
    if (subset.size() != superset.size()) return false;
    for (qsizetype index = 0; index < subset.size(); ++index) {
        if (subset.testBit(index) && !superset.testBit(index)) return false;
    }
    return true;
}

bool writeExecutableScript(const QString &path, QByteArrayView contents)
{
    QFile script(path);
    if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || script.write(contents.data(), contents.size()) != contents.size()) {
        return false;
    }
    script.close();
    return QFile::setPermissions(
        path,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
            | QFileDevice::ExeOwner);
}

TerminalSelectionPressInput
selectionPress(int column, int row,
               std::optional<quint64> timestampNanoseconds = std::nullopt,
               bool controlModifier = false,
               bool extendExistingSelection = false, bool rectangular = false)
{
    return {
        .column = column,
        .row = row,
        .surfaceX = (static_cast<double>(column) + 0.5) * 8.0,
        .surfaceY = (static_cast<double>(row) + 0.5) * 16.0,
        .timestampNanoseconds = timestampNanoseconds.value_or(0),
        .timestampValid = timestampNanoseconds.has_value(),
        .controlModifier = controlModifier,
        .extendExistingSelection = extendExistingSelection,
        .rectangular = rectangular,
    };
}

TerminalSelectionDragInput selectionDrag(int column, int row,
                                         bool rectangular = false)
{
    return {
        .column = column,
        .row = row,
        .surfaceX = (static_cast<double>(column) + 0.5) * 8.0,
        .surfaceY = (static_cast<double>(row) + 0.5) * 16.0,
        .rectangular = rectangular,
    };
}

void beginWordSelection(SessionWorker &worker, int column, int row,
                        quint64 firstTimestampNanoseconds)
{
    worker.beginSelection(
        selectionPress(column, row, firstTimestampNanoseconds));
    worker.endSelection(column, row);
    worker.beginSelection(
        selectionPress(column, row, firstTimestampNanoseconds + 100'000'000));
}

class ScopedEnvironmentVariable final {
public:
    ScopedEnvironmentVariable(QByteArray name, const QByteArray &value)
        : name_(std::move(name))
        , wasSet_(qEnvironmentVariableIsSet(name_.constData()))
        , previousValue_(qgetenv(name_.constData()))
    {
        qputenv(name_.constData(), value);
    }

    explicit ScopedEnvironmentVariable(QByteArray name)
        : name_(std::move(name))
        , wasSet_(qEnvironmentVariableIsSet(name_.constData()))
        , previousValue_(qgetenv(name_.constData()))
    {
        qunsetenv(name_.constData());
    }

    ~ScopedEnvironmentVariable()
    {
        if (wasSet_) {
            qputenv(name_.constData(), previousValue_);
        } else {
            qunsetenv(name_.constData());
        }
    }

    Q_DISABLE_COPY_MOVE(ScopedEnvironmentVariable)

private:
    QByteArray name_;
    bool wasSet_ = false;
    QByteArray previousValue_;
};

class CurrentDirectoryRestore final {
public:
    CurrentDirectoryRestore()
        : previousDirectory_(QDir::currentPath())
    {}

    ~CurrentDirectoryRestore()
    {
        QDir::setCurrent(previousDirectory_);
    }

    Q_DISABLE_COPY_MOVE(CurrentDirectoryRestore)

private:
    QString previousDirectory_;
};

} // namespace

class SessionWorkerTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void runsCommandThroughPty();
    void publishesDesktopNotificationsInParserOrder();
    void publishesProgressReportsInParserOrder();
    void runsTaggedShellAndDirectCommands();
    void appliesPinnedShellIntegrationLaunchOrdering();
    void classifiesAbnormalCommandExitBoundaries();
    void appliesLiveAbnormalExitPolicy();
    void waitsForEncodedKeyUsingLiveExitPolicy();
    void preservesStagedSequenceWhileWaitingAfterCommand();
    void writesPersistentTerminalFiles();
    void skipsUnavailableTerminalFiles();
    void reportsTerminalInitializationSeparatelyFromChildExec();
    void reportsTerminalInitializationFailure();
    void publishesCorrelatedInspectorSnapshots();
    void publishesCorrelatedInspectorCells();
    void publishesBoundedKeyboardTraceResults();
    void initializesGeometryBeforeSpawningChild();
    void resizesPtyWithPaddingExcludedPixels();
    void injectsInitialInputAtomicallyInOrder();
    void rejectsInvalidInitialInputBeforeSpawning();
    void enforcesInitialInputFileSizeLimit();
    void injectsRawTerminalIdentity();
    void usesConfiguredTerminalEnvironment();
    void appliesConfiguredEnvironmentPwdPrecedence();
    void rejectsInvalidConfiguredEnvironment();
    void cgroupGateMovesChildBeforeExec();
    void cgroupSoftFailureContinuesLaunch();
    void cgroupHardFailurePreventsExec();
    void disabledCgroupSkipsScopeMove();
    void stripsDesktopActivationFromChildEnvironment();
    void fallsBackFromUnavailableWorkingDirectory_data();
    void fallsBackFromUnavailableWorkingDirectory();
    void preservesInheritedLogicalPwd();
    void preservesSymlinkSensitiveWorkingDirectory();
    void preservesNonUtf8WorkingDirectoryBytes();
    void rejectsNulWorkingDirectoryBeforePosixCalls();
    void resolvesRelativePathEntriesFromChildWorkingDirectory_data();
    void resolvesRelativePathEntriesFromChildWorkingDirectory();
    void skipsEmptyPathEntries();
    void continuesPathLookupAfterMissingInterpreter();
    void usesPinnedDefaultPathWhenUnset();
    void drainsLargeFinalOutputBeforeClosingPty();
    void drainsLargeQueuedInputAfterPtyBackpressure();
    void routesTerminalClipboardWritesUsingLivePolicy();
    void sendsBracketedPasteThroughPty();
    void protectsPasteWithCorrelatedWorkerConfirmation();
    void sendsTerminalControlActionsThroughPty();
    void pastesTerminalFilePathAsRawOrderedInput();
    void readOnlyBlocksSurfaceInputButPreservesProtocolReplies();
    void releasesHeldModifiersBeforeFocusOut();
    void appliesLiveEnquiryResponse();
    void stagesAndResolvesSequenceBytes();
    void sendsConsumedShiftTextLiterallyInKittyMode();
    void stagesSequenceKeysUsingModesAtStageTime();
    void gatesKeyboardAndImeWithLiveKamPolicy();
    void appliesReloadedAppearanceToExistingTerminal();
    void ordersLiveColorSchemeReportsThroughPty();
    void appliesLiveScrollbackCompressionPolicy();
    void appliesLiveScrollToBottomPolicy();
    void appliesReloadedWordBoundariesToExistingGesture();
    void appliesClickRepeatIntervalAtLaunchAndReload();
    void extendsSelectionUsingReloadedClickInterval();
    void clearsSelectionOnlyForUpstreamTypingPaths();
    void clearsSelectionForReportedMouseButtonsAndWheels();
    void convertsAlternateScreenWheelRowsAtomically();
    void reportsHorizontalWheelButtonsInStableOrder();
    void copiesSelectionWithRuntimeFormattingAndAtomicClear();
    void resolvesConfiguredRightClickActions();
    void autoCopiesOnlyCommittedSelectionsAndSelectAll();
    void retainsSelectionAvailabilityOutsideViewport();
    void continuouslyAutoscrollsSelectionAtEdges();
    void routesTypedViewportAndSelectionOperations();
    void resolvesCorrelatedSelectionActions();
    void searchesIncrementallyAndNavigates();
    void preservesFormattedSearchBoundaries();
    void resetsTerminalStateAndWorkerCaches();
    void resolvesCorrelatedHyperlinkQueries();
    void resolvesRegexLinksAcrossUtf8WrapsAndOsc8Precedence();
    void retainsRegexHoverAcrossViewportScrolling();
    void revalidatesRegexActivationAcrossUnrelatedOutput();
    void explicitProgramIsActiveForItsLifetime();
    void integratedShellStartupRemainsActiveUntilPrompt();
    void semanticPromptsTrackSameProcessShellActivity();
    void interactiveShellTracksForegroundJobs();
};

void SessionWorkerTest::runsTaggedShellAndDirectCommands()
{
    qRegisterMetaType<TerminalUpdate>();

    {
        SessionWorker worker;
        QSignalSpy updates(&worker, &SessionWorker::terminalUpdated);
        QSignalSpy exited(&worker, &SessionWorker::sessionExited);
        QSignalSpy errors(&worker, &SessionWorker::errorOccurred);

        TerminalSessionLaunchOptions options;
        options.workingDirectory = QDir::tempPath();
        options.command = TerminalCommand::shell(
            QByteArrayLiteral("printf 'tagged-shell:%s\\n' \"$((20 + 22))\""));
        options.hold = true;
        QVERIFY(worker.initialize(options));
        QTRY_COMPARE_WITH_TIMEOUT(exited.count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(
            updatesContain(updates, QStringLiteral("tagged-shell:42")), 5000);
        QVERIFY2(errors.isEmpty(),
                 errors.isEmpty()
                     ? ""
                     : qPrintable(errors.constFirst().constFirst().toString()));
        worker.shutdown();
    }

    {
        SessionWorker worker;
        QSignalSpy updates(&worker, &SessionWorker::terminalUpdated);
        QSignalSpy exited(&worker, &SessionWorker::sessionExited);
        QSignalSpy errors(&worker, &SessionWorker::errorOccurred);

        const QByteArray script =
            QByteArrayLiteral("printf 'count=%s first=<%s> second=<%s> raw=' "
                              "\"$#\" \"$1\" \"$2\"; "
                              "printf '%s' \"$3\" | /usr/bin/od -An -tx1 "
                              "| /usr/bin/tr -d ' \\n'; printf '\\n'");
        TerminalSessionLaunchOptions options;
        options.workingDirectory = QDir::tempPath();
        options.command = TerminalCommand::direct({
            QByteArrayLiteral("sh"),
            QByteArrayLiteral("-c"),
            script,
            QByteArrayLiteral("ghostty-qt-direct"),
            QByteArrayLiteral("two words"),
            QByteArray{},
            QByteArray::fromHex("80ff"),
        });
        options.hold = true;
        QVERIFY(worker.initialize(options));
        QTRY_COMPARE_WITH_TIMEOUT(exited.count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(
            updatesContain(
                updates,
                QStringLiteral("count=3 first=<two words> second=<> raw=80ff")),
            5000);
        QVERIFY2(errors.isEmpty(),
                 errors.isEmpty()
                     ? ""
                     : qPrintable(errors.constFirst().constFirst().toString()));
        worker.shutdown();
    }

    {
        SessionWorker worker;
        QSignalSpy exited(&worker, &SessionWorker::sessionExited);
        QSignalSpy errors(&worker, &SessionWorker::errorOccurred);

        TerminalSessionLaunchOptions options;
        options.workingDirectory = QDir::tempPath();
        QByteArray invalid = QByteArrayLiteral("bad");
        invalid.append('\0');
        invalid.append("argument");
        options.command = TerminalCommand::direct(
            {QByteArrayLiteral("/bin/printf"), std::move(invalid)});
        options.hold = true;
        QVERIFY(worker.initialize(options));
        QCOMPARE(exited.count(), 1);
        QCOMPARE(exited.constFirst().at(0).toInt(), 127);
        QVERIFY(errors.constFirst().constFirst().toString().contains(
            QStringLiteral("NUL byte")));
        worker.shutdown();
    }
}

void SessionWorkerTest::classifiesAbnormalCommandExitBoundaries()
{
    QVERIFY(!SessionWorker::isAbnormalCommandExit(0, 0, 0, 0));
    QVERIFY(SessionWorker::isAbnormalCommandExit(1, 0, 0, 0));
    QVERIFY(SessionWorker::isAbnormalCommandExit(7, 0, 250, 250));
    QVERIFY(!SessionWorker::isAbnormalCommandExit(7, 0, 251, 250));
    QVERIFY(SessionWorker::isAbnormalCommandExit(143, 15, 250, 250));
    QVERIFY(!SessionWorker::isAbnormalCommandExit(0, 0, 250, 250));
    QVERIFY(SessionWorker::isAbnormalCommandExit(
        1, 0, std::numeric_limits<quint32>::max(),
        std::numeric_limits<quint32>::max()));
}

void SessionWorkerTest::publishesCorrelatedInspectorSnapshots()
{
    qRegisterMetaType<TerminalInspectorSnapshot>();

    SessionWorker worker;
    QSignalSpy snapshots(&worker,
                         &SessionWorker::terminalInspectorSnapshotReady);
    worker.inspectTerminal(0);
    QCOMPARE(snapshots.count(), 0);

    worker.inspectTerminal(1);
    QCOMPARE(snapshots.count(), 1);
    QCOMPARE(snapshots.constLast().at(0).toULongLong(), quint64{1});
    const TerminalInspectorSnapshot unavailable =
        snapshots.constLast().at(1).value<TerminalInspectorSnapshot>();
    QCOMPARE(unavailable.status, TerminalInspectorStatus::Unavailable);

    TerminalSessionLaunchOptions options;
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    QVERIFY(worker.initialize(options));
    worker.inspectTerminal(2);
    QCOMPARE(snapshots.count(), 2);
    QCOMPARE(snapshots.constLast().at(0).toULongLong(), quint64{2});
    const TerminalInspectorSnapshot ready =
        snapshots.constLast().at(1).value<TerminalInspectorSnapshot>();
    QCOMPARE(ready.status, TerminalInspectorStatus::Ready);
    QVERIFY(ready.contentRevision > 0);
    QCOMPARE(ready.modes.size(), 42);

    worker.resetTerminal();
    worker.inspectTerminal(3);
    QCOMPARE(snapshots.count(), 3);
    const TerminalInspectorSnapshot reset =
        snapshots.constLast().at(1).value<TerminalInspectorSnapshot>();
    QCOMPARE(reset.status, TerminalInspectorStatus::Ready);
    QVERIFY(reset.contentRevision > ready.contentRevision);
    worker.shutdown();
}

void SessionWorkerTest::publishesCorrelatedInspectorCells()
{
    qRegisterMetaType<TerminalInspectorCellSnapshot>();

    SessionWorker worker;
    QSignalSpy cells(&worker, &SessionWorker::terminalInspectorCellReady);
    worker.inspectTerminalCell(0, 0, 2, 1);
    QCOMPARE(cells.count(), 0);

    worker.inspectTerminalCell(1, 0, 2, 1);
    QCOMPARE(cells.count(), 1);
    QCOMPARE(cells.constLast().at(0).toULongLong(), quint64{1});
    const TerminalInspectorCellSnapshot unavailable =
        cells.constLast().at(1).value<TerminalInspectorCellSnapshot>();
    QCOMPARE(unavailable.status, TerminalInspectorCellStatus::Unavailable);
    QCOMPARE(unavailable.viewportColumn, 2);
    QCOMPARE(unavailable.viewportRow, 1);

    TerminalSessionLaunchOptions options;
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    QVERIFY(worker.initialize(options));

    worker.inspectTerminalCell(2, std::numeric_limits<quint64>::max(), 0, 0);
    QCOMPARE(cells.count(), 2);
    QCOMPARE(cells.constLast().at(0).toULongLong(), quint64{2});
    const TerminalInspectorCellSnapshot stale =
        cells.constLast().at(1).value<TerminalInspectorCellSnapshot>();
    QCOMPARE(stale.status, TerminalInspectorCellStatus::Stale);
    QVERIFY(stale.contentRevision > 0);

    worker.inspectTerminalCell(3, stale.contentRevision, 0, 0);
    QCOMPARE(cells.count(), 3);
    QCOMPARE(cells.constLast().at(0).toULongLong(), quint64{3});
    const TerminalInspectorCellSnapshot ready =
        cells.constLast().at(1).value<TerminalInspectorCellSnapshot>();
    QCOMPARE(ready.status, TerminalInspectorCellStatus::Ready);
    QCOMPARE(ready.contentRevision, stale.contentRevision);
    QCOMPARE(ready.viewportColumn, 0);
    QCOMPARE(ready.viewportRow, 0);

    worker.inspectTerminalCell(4, ready.contentRevision,
                               std::numeric_limits<int>::max(), 0);
    QCOMPARE(cells.count(), 4);
    const TerminalInspectorCellSnapshot outside =
        cells.constLast().at(1).value<TerminalInspectorCellSnapshot>();
    QCOMPARE(outside.status, TerminalInspectorCellStatus::OutOfBounds);

    worker.resetTerminal();
    worker.inspectTerminalCell(5, ready.contentRevision, 0, 0);
    QCOMPARE(cells.count(), 5);
    const TerminalInspectorCellSnapshot resetStale =
        cells.constLast().at(1).value<TerminalInspectorCellSnapshot>();
    QCOMPARE(resetStale.status, TerminalInspectorCellStatus::Stale);
    QVERIFY(resetStale.contentRevision > ready.contentRevision);

    worker.inspectTerminalCell(6, resetStale.contentRevision, 0, 0);
    QCOMPARE(cells.count(), 6);
    const TerminalInspectorCellSnapshot resetReady =
        cells.constLast().at(1).value<TerminalInspectorCellSnapshot>();
    QCOMPARE(resetReady.status, TerminalInspectorCellStatus::Ready);
    worker.shutdown();
}

void SessionWorkerTest::publishesBoundedKeyboardTraceResults()
{
    qRegisterMetaType<TerminalKeyboardTraceResult>();

    SessionWorker worker;
    QSignalSpy traces(&worker, &SessionWorker::keyboardTraceResult);
    QSignalSpy updates(&worker, &SessionWorker::terminalUpdated);
    const auto key = [](QChar character, quint64 generation, quint64 traceId) {
        TerminalKeyInput input;
        input.key = character.toUpper().unicode();
        input.text = QString(character);
        input.unshiftedCodepoint = character.toLower().unicode();
        input.pressed = true;
        input.inspectorTraceGeneration = generation;
        input.inspectorTraceId = traceId;
        return input;
    };
    const auto resultAt = [&traces](qsizetype index) {
        return qvariant_cast<TerminalKeyboardTraceResult>(
            traces.at(index).constFirst());
    };

    constexpr quint64 generation = 17;
    TerminalKeyInput ordinary = key(u'a', generation, 41);

    // Supplying correlation metadata alone must not activate tracing. The
    // default generation is zero so closed inspectors impose no result or
    // bounded-preview work on ordinary input.
    worker.sendKey(ordinary);
    QCOMPARE(traces.count(), 0);

    worker.setKeyboardTraceGeneration(generation);
    worker.sendKey(ordinary);
    QCOMPARE(traces.count(), 1);
    TerminalKeyboardTraceResult result = resultAt(0);
    QCOMPARE(result.generation, generation);
    QCOMPARE(result.traceId, quint64{41});
    QCOMPARE(result.sequenceToken, quint64{0});
    QCOMPARE(result.operation, TerminalKeyboardTraceOperation::Key);
    QCOMPARE(result.disposition,
             TerminalKeyboardTraceDisposition::TerminalUnavailable);
    QCOMPARE(result.encodedByteCount, qint64{0});
    QVERIFY(result.encodedPrefix.isEmpty());
    QVERIFY(!result.prefixTruncated);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/cat")};
    options.hold = true;
    QVERIFY(worker.initialize(options));

    traces.clear();
    worker.sendKey(ordinary);
    QCOMPARE(traces.count(), 1);
    result = resultAt(0);
    QCOMPARE(result.generation, generation);
    QCOMPARE(result.traceId, quint64{41});
    QCOMPARE(result.sequenceToken, quint64{0});
    QCOMPARE(result.operation, TerminalKeyboardTraceOperation::Key);
    QCOMPARE(result.disposition, TerminalKeyboardTraceDisposition::Queued);
    QCOMPARE(result.encodedByteCount, qint64{1});
    QCOMPARE(result.encodedPrefix, QByteArrayLiteral("a"));
    QVERIFY(!result.prefixTruncated);

    // A different or disabled capture generation may still deliver terminal
    // input, but it must never publish diagnostic results into this capture.
    ordinary.inspectorTraceGeneration = generation + 1;
    ordinary.inspectorTraceId = 42;
    worker.sendKey(ordinary);
    QCOMPARE(traces.count(), 1);
    worker.setKeyboardTraceGeneration(0);
    ordinary.inspectorTraceGeneration = generation;
    ordinary.inspectorTraceId = 43;
    worker.sendKey(ordinary);
    QCOMPARE(traces.count(), 1);

    // One key can encode more than the retained diagnostic prefix. Preserve
    // its exact byte count without asking any queued GUI consumer to copy the
    // complete payload.
    worker.setKeyboardTraceGeneration(generation);
    traces.clear();
    TerminalKeyInput longText = key(u'x', generation, 50);
    constexpr qsizetype longByteCount =
        TerminalKeyboardTraceResult::MaximumEncodedPrefix * 3;
    longText.text = QString(longByteCount, u'x');
    constexpr quint64 longToken = 22;
    worker.stageSequenceKey(longToken, longText);
    QCOMPARE(traces.count(), 1);
    result = resultAt(0);
    QCOMPARE(result.traceId, quint64{50});
    QCOMPARE(result.operation, TerminalKeyboardTraceOperation::SequenceStage);
    QCOMPARE(result.disposition, TerminalKeyboardTraceDisposition::Staged);
    QCOMPARE(result.encodedByteCount, static_cast<qint64>(longByteCount));
    QCOMPARE(
        result.encodedPrefix,
        QByteArray(TerminalKeyboardTraceResult::MaximumEncodedPrefix, 'x'));
    QVERIFY(result.prefixTruncated);
    traces.clear();
    worker.resolveSequence(longToken, TerminalSequenceResolution::Flush, false,
                           {});
    QCOMPARE(traces.count(), 1);
    result = resultAt(0);
    QCOMPARE(result.traceId, quint64{50});
    QCOMPARE(result.operation,
             TerminalKeyboardTraceOperation::SequenceResolution);
    QCOMPARE(result.disposition, TerminalKeyboardTraceDisposition::Queued);
    QCOMPARE(result.encodedByteCount, static_cast<qint64>(longByteCount));
    QCOMPARE(
        result.encodedPrefix,
        QByteArray(TerminalKeyboardTraceResult::MaximumEncodedPrefix, 'x'));
    QVERIFY(result.prefixTruncated);

    // Sequence leaders receive both an immediate staging result and their own
    // final fate. The resolving key is attributed only its own encoded bytes,
    // even though the worker writes the combined payload atomically.
    traces.clear();
    constexpr quint64 token = 23;
    worker.stageSequenceKey(token, key(u'x', generation, 100));
    QCOMPARE(traces.count(), 1);
    result = resultAt(0);
    QCOMPARE(result.traceId, quint64{100});
    QCOMPARE(result.operation, TerminalKeyboardTraceOperation::SequenceStage);
    QCOMPARE(result.disposition, TerminalKeyboardTraceDisposition::Staged);
    worker.stageSequenceKey(token, key(u'w', generation, 101));
    QCOMPARE(traces.count(), 2);
    result = resultAt(1);
    QCOMPARE(result.traceId, quint64{101});
    QCOMPARE(result.operation, TerminalKeyboardTraceOperation::SequenceStage);
    QCOMPARE(result.disposition, TerminalKeyboardTraceDisposition::Staged);
    traces.clear();

    const TerminalKeyInput current = key(u'y', generation, 999);
    worker.resolveSequence(
        token, TerminalSequenceResolution::FlushAndSendCurrent, true, current);
    QCOMPARE(traces.count(), 3);
    result = resultAt(0);
    QCOMPARE(result.generation, generation);
    QCOMPARE(result.traceId, quint64{100});
    QCOMPARE(result.sequenceToken, token);
    QCOMPARE(result.operation,
             TerminalKeyboardTraceOperation::SequenceResolution);
    QCOMPARE(result.disposition, TerminalKeyboardTraceDisposition::Queued);
    QCOMPARE(result.encodedByteCount, qint64{1});
    QCOMPARE(result.encodedPrefix, QByteArrayLiteral("x"));
    QVERIFY(!result.prefixTruncated);
    result = resultAt(1);
    QCOMPARE(result.traceId, quint64{101});
    QCOMPARE(result.operation,
             TerminalKeyboardTraceOperation::SequenceResolution);
    QCOMPARE(result.disposition, TerminalKeyboardTraceDisposition::Queued);
    QCOMPARE(result.encodedByteCount, qint64{1});
    QCOMPARE(result.encodedPrefix, QByteArrayLiteral("w"));
    result = resultAt(2);
    QCOMPARE(result.traceId, quint64{999});
    QCOMPARE(result.operation,
             TerminalKeyboardTraceOperation::SequenceResolution);
    QCOMPARE(result.disposition, TerminalKeyboardTraceDisposition::Queued);
    QCOMPARE(result.encodedByteCount, qint64{1});
    QCOMPARE(result.encodedPrefix, QByteArrayLiteral("y"));

    // Disabling capture discards only copied trace metadata. Terminal input
    // semantics retain a staged leader, but reopening with a fresh generation
    // cannot reveal or misattribute its bytes to the resolving key.
    constexpr quint64 privateToken = 24;
    traces.clear();
    worker.stageSequenceKey(privateToken, key(u's', generation, 200));
    QCOMPARE(traces.count(), 1);
    worker.setKeyboardTraceGeneration(0);
    worker.setKeyboardTraceGeneration(generation + 1);
    traces.clear();
    worker.resolveSequence(privateToken,
                           TerminalSequenceResolution::FlushAndSendCurrent,
                           true, key(u'z', generation + 1, 201));
    QCOMPARE(traces.count(), 1);
    result = resultAt(0);
    QCOMPARE(result.generation, generation + 1);
    QCOMPARE(result.traceId, quint64{201});
    QCOMPARE(result.encodedByteCount, qint64{1});
    QCOMPARE(result.encodedPrefix, QByteArrayLiteral("z"));
    QTRY_VERIFY_WITH_TIMEOUT(updatesContain(updates, QStringLiteral("sz")),
                             1000);

    // Replacing an unresolved token gives every retained leader an explicit
    // final disposition before staging the new sequence.
    constexpr quint64 supersededToken = 25;
    constexpr quint64 replacementToken = 26;
    traces.clear();
    worker.stageSequenceKey(supersededToken, key(u'a', generation + 1, 300));
    traces.clear();
    worker.stageSequenceKey(replacementToken, key(u'b', generation + 1, 301));
    QCOMPARE(traces.count(), 2);
    result = resultAt(0);
    QCOMPARE(result.traceId, quint64{300});
    QCOMPARE(result.sequenceToken, supersededToken);
    QCOMPARE(result.operation,
             TerminalKeyboardTraceOperation::SequenceResolution);
    QCOMPARE(result.disposition, TerminalKeyboardTraceDisposition::Superseded);
    result = resultAt(1);
    QCOMPARE(result.traceId, quint64{301});
    QCOMPARE(result.sequenceToken, replacementToken);
    QCOMPARE(result.operation, TerminalKeyboardTraceOperation::SequenceStage);
    QCOMPARE(result.disposition, TerminalKeyboardTraceDisposition::Staged);

    traces.clear();
    worker.resolveSequence(replacementToken, TerminalSequenceResolution::Drop,
                           false, key(u'c', generation + 1, 302));
    QCOMPARE(traces.count(), 2);
    result = resultAt(0);
    QCOMPARE(result.traceId, quint64{301});
    QCOMPARE(result.disposition, TerminalKeyboardTraceDisposition::Dropped);
    result = resultAt(1);
    QCOMPARE(result.traceId, quint64{302});
    QCOMPARE(result.encodedByteCount, qint64{0});
    QCOMPARE(result.disposition, TerminalKeyboardTraceDisposition::Dropped);

    // Destruction is not a sequence resolution and must not enqueue a final
    // diagnostic event while the worker is tearing down.
    constexpr quint64 shutdownToken = 27;
    traces.clear();
    worker.stageSequenceKey(shutdownToken, key(u'd', generation + 1, 400));
    QCOMPARE(traces.count(), 1);
    traces.clear();
    worker.shutdown();
    QCOMPARE(traces.count(), 0);
}

void SessionWorkerTest::appliesLiveAbnormalExitPolicy()
{
    TerminalKeyInput text;
    text.key = Qt::Key_A;
    text.text = QStringLiteral("a");
    text.pressed = true;

    {
        SessionWorker worker;
        QSignalSpy exited(&worker, &SessionWorker::sessionExited);
        QSignalSpy dismissed(&worker, &SessionWorker::exitKeyDismissed);

        TerminalSessionLaunchOptions options;
        options.workingDirectory = QDir::tempPath();
        options.program = {
            QStringLiteral("/bin/sh"),
            QStringLiteral("-c"),
            QStringLiteral("sleep 0.15; exit 9"),
        };
        options.runtime.abnormalCommandExitRuntimeMilliseconds = 0;
        QVERIFY(worker.initialize(options));

        // The threshold is live surface state. Its newest value at observed
        // child exit, rather than the launch snapshot, controls retention.
        TerminalSessionRuntimeOptions reloaded = options.runtime;
        reloaded.abnormalCommandExitRuntimeMilliseconds =
            std::numeric_limits<quint32>::max();
        worker.applyRuntimeOptions(reloaded);

        QTRY_COMPARE_WITH_TIMEOUT(exited.count(), 1, 5000);
        QCOMPARE(exited.constFirst().at(0).toInt(), 9);
        QVERIFY(!exited.constFirst().at(2).toBool());
        QVERIFY(exited.constFirst().at(3).toBool());
        QVERIFY(exited.constFirst().at(4).toULongLong() >= 100);
        QVERIFY(exited.constFirst().at(5).toBool());

        worker.sendKey(text);
        QCOMPARE(dismissed.count(), 1);
        worker.sendKey(text);
        QCOMPARE(dismissed.count(), 1);
        worker.shutdown();
    }

    {
        SessionWorker worker;
        QSignalSpy exited(&worker, &SessionWorker::sessionExited);
        QSignalSpy dismissed(&worker, &SessionWorker::exitKeyDismissed);

        TerminalSessionLaunchOptions options;
        options.workingDirectory = QDir::tempPath();
        options.program = {
            QStringLiteral("/bin/sh"),
            QStringLiteral("-c"),
            QStringLiteral("sleep 0.05; exit 7"),
        };
        options.runtime.abnormalCommandExitRuntimeMilliseconds = 0;
        QVERIFY(worker.initialize(options));
        QTRY_COMPARE_WITH_TIMEOUT(exited.count(), 1, 5000);
        QVERIFY(exited.constFirst().at(4).toULongLong() > 0);
        QVERIFY(!exited.constFirst().at(3).toBool());
        QVERIFY(!exited.constFirst().at(5).toBool());
        worker.sendKey(text);
        QCOMPARE(dismissed.count(), 0);
        worker.shutdown();
    }

    {
        SessionWorker worker;
        QSignalSpy exited(&worker, &SessionWorker::sessionExited);

        TerminalSessionLaunchOptions options;
        options.workingDirectory = QDir::tempPath();
        options.program = {QStringLiteral("/bin/true")};
        options.runtime.abnormalCommandExitRuntimeMilliseconds =
            std::numeric_limits<quint32>::max();
        QVERIFY(worker.initialize(options));
        QTRY_COMPARE_WITH_TIMEOUT(exited.count(), 1, 5000);
        QCOMPARE(exited.constFirst().at(0).toInt(), 0);
        QVERIFY(!exited.constFirst().at(3).toBool());
        QVERIFY(!exited.constFirst().at(5).toBool());
        worker.shutdown();
    }

    {
        SessionWorker worker;
        QSignalSpy exited(&worker, &SessionWorker::sessionExited);
        QSignalSpy dismissed(&worker, &SessionWorker::exitKeyDismissed);

        TerminalSessionLaunchOptions options;
        options.workingDirectory = QDir::tempPath();
        options.program = {
            QStringLiteral("/bin/sh"),
            QStringLiteral("-c"),
            QStringLiteral("exit 17"),
        };
        options.hold = true;
        options.runtime.abnormalCommandExitRuntimeMilliseconds =
            std::numeric_limits<quint32>::max();
        QVERIFY(worker.initialize(options));
        QTRY_COMPARE_WITH_TIMEOUT(exited.count(), 1, 5000);
        QVERIFY(exited.constFirst().at(2).toBool());
        QVERIFY(!exited.constFirst().at(3).toBool());
        QVERIFY(exited.constFirst().at(5).toBool());
        worker.sendKey(text);
        QCOMPARE(dismissed.count(), 0);
        worker.shutdown();
    }

    {
        SessionWorker worker;
        QSignalSpy exited(&worker, &SessionWorker::sessionExited);

        TerminalSessionLaunchOptions options;
        options.workingDirectory = QDir::tempPath();
        options.program = {
            QStringLiteral("/bin/sh"),
            QStringLiteral("-c"),
            QStringLiteral("kill -TERM $$"),
        };
        options.runtime.abnormalCommandExitRuntimeMilliseconds =
            std::numeric_limits<quint32>::max();
        QVERIFY(worker.initialize(options));
        QTRY_COMPARE_WITH_TIMEOUT(exited.count(), 1, 5000);
        QCOMPARE(exited.constFirst().at(1).toInt(), SIGTERM);
        QVERIFY(exited.constFirst().at(3).toBool());
        QVERIFY(exited.constFirst().at(5).toBool());
        worker.shutdown();
    }
}

void SessionWorkerTest::waitsForEncodedKeyUsingLiveExitPolicy()
{
    SessionWorker worker;
    QSignalSpy exited(&worker, &SessionWorker::sessionExited);
    QSignalSpy dismissed(&worker, &SessionWorker::exitKeyDismissed);
    QSignalSpy errors(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        // Leave Kitty report-all-keys and ANSI KAM enabled. Ghostty
        // normalizes both on child exit, so a modifier-only press still
        // encodes no bytes and KAM cannot block the later dismissal key.
        QStringLiteral("printf '\\033[=15u\\033[2h'; sleep 0.2"),
    };
    options.runtime.vtKamAllowed = true;
    QVERIFY(worker.initialize(options));

    TerminalSessionRuntimeOptions reloaded = options.runtime;
    reloaded.waitAfterCommand = true;
    worker.applyRuntimeOptions(reloaded);
    QTRY_COMPARE_WITH_TIMEOUT(exited.count(), 1, 5000);
    QVERIFY(!exited.constFirst().at(2).toBool());
    QVERIFY(exited.constFirst().at(3).toBool());

    TerminalKeyInput modifier;
    modifier.key = Qt::Key_Control;
    modifier.modifiers = Qt::ControlModifier;
    modifier.pressed = true;
    worker.sendKey(modifier);
    QCOMPARE(dismissed.count(), 0);

    TerminalKeyInput text;
    text.key = Qt::Key_A;
    text.text = QStringLiteral("a");
    text.pressed = true;
    worker.stageSequenceKey(1, text);
    worker.resolveSequence(1, TerminalSequenceResolution::Drop, false, {});
    QCOMPARE(dismissed.count(), 0);

    worker.stageSequenceKey(2, text);
    worker.resolveSequence(2, TerminalSequenceResolution::Flush, false, {});
    QCOMPARE(dismissed.count(), 1);
    worker.sendKey(text);
    QCOMPARE(dismissed.count(), 1);
    QVERIFY2(errors.isEmpty(),
             errors.isEmpty()
                 ? ""
                 : qPrintable(errors.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::preservesStagedSequenceWhileWaitingAfterCommand()
{
    SessionWorker worker;
    QSignalSpy exited(&worker, &SessionWorker::sessionExited);
    QSignalSpy dismissed(&worker, &SessionWorker::exitKeyDismissed);
    QSignalSpy errors(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("sleep 0.2"),
    };
    options.runtime.waitAfterCommand = true;
    QVERIFY(worker.initialize(options));

    TerminalKeyInput leader;
    leader.key = Qt::Key_A;
    leader.text = QStringLiteral("a");
    leader.pressed = true;
    worker.stageSequenceKey(41, leader);

    QTRY_COMPARE_WITH_TIMEOUT(exited.count(), 1, 5000);
    QVERIFY(exited.constFirst().at(3).toBool());

    TerminalKeyInput continuation;
    continuation.key = Qt::Key_B;
    continuation.text = QStringLiteral("b");
    continuation.pressed = true;
    worker.resolveSequence(41, TerminalSequenceResolution::FlushAndSendCurrent,
                           true, continuation);
    QCOMPARE(dismissed.count(), 1);
    QVERIFY2(errors.isEmpty(),
             errors.isEmpty()
                 ? ""
                 : qPrintable(errors.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::searchesIncrementallyAndNavigates()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalSearchUpdate>();
    SessionWorker worker;
    worker.resizeTerminal(32, 6, 8, 16, 256, 96);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy searchSpy(&worker, &SessionWorker::searchUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "i=0; while [ $i -lt 500 ]; do "
            "printf 'scan-row-%03d needle\\n' \"$i\"; "
            "i=$((i + 1)); done; "
            "printf 'AAAA\\nleft\\nright\\nÄ ä\\n界\\n'")};
    options.hold = true;
    worker.initialize(options);

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!updateSpy.isEmpty(), 5000);
    QVERIFY(errorSpy.isEmpty());

    bool eventLoopMarker = false;
    worker.search(1, QByteArrayLiteral("aa"));
    QTimer::singleShot(0, &worker, [&eventLoopMarker] {
        eventLoopMarker = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(eventLoopMarker, 1000);
    const std::optional<TerminalSearchUpdate> partial =
        latestSearchUpdate(searchSpy, 1);
    QVERIFY(partial.has_value());
    QVERIFY(partial->active);
    QVERIFY(!partial->complete);
    QVERIFY(partial->scannedRows < partial->totalRows);

    QTRY_VERIFY_WITH_TIMEOUT(
        latestSearchUpdate(searchSpy, 1).has_value()
            && latestSearchUpdate(searchSpy, 1)->complete,
        5000);
    const TerminalSearchUpdate completedAa = *latestSearchUpdate(searchSpy, 1);
    QCOMPARE(completedAa.totalMatches, quint64(3));
    const qsizetype maskSize = static_cast<qsizetype>(completedAa.columns)
        * completedAa.rows;
    QCOMPARE(completedAa.visibleCellMask.size(), maskSize);
    QCOMPARE(completedAa.selectedCellMask.size(), maskSize);
    QCOMPARE(completedAa.visibleCellMask.count(true), qsizetype(4));
    QCOMPARE(completedAa.selectedCellMask.count(true), qsizetype(0));

    worker.navigateSearch(1, TerminalSearchDirection::Next);
    const TerminalSearchUpdate firstAa = *latestSearchUpdate(searchSpy, 1);
    QCOMPARE(firstAa.selectedMatch, qint64(0));
    QCOMPARE(firstAa.selectedCellMask.count(true), qsizetype(2));
    QVERIFY(maskIsSubset(firstAa.selectedCellMask, firstAa.visibleCellMask));
    worker.navigateSearch(1, TerminalSearchDirection::Previous);
    const TerminalSearchUpdate lastAa = *latestSearchUpdate(searchSpy, 1);
    QCOMPARE(lastAa.selectedMatch, qint64(2));
    QCOMPARE(lastAa.visibleCellMask, firstAa.visibleCellMask);
    QCOMPARE(lastAa.selectedCellMask.count(true), qsizetype(2));
    QVERIFY(lastAa.selectedCellMask != firstAa.selectedCellMask);
    QVERIFY(maskIsSubset(lastAa.selectedCellMask, lastAa.visibleCellMask));
    worker.navigateSearch(1, TerminalSearchDirection::Next);
    QCOMPARE(latestSearchUpdate(searchSpy, 1)->selectedMatch, qint64(0));

    worker.search(2, QByteArrayLiteral("left\nright"));
    QTRY_VERIFY_WITH_TIMEOUT(
        latestSearchUpdate(searchSpy, 2).has_value()
            && latestSearchUpdate(searchSpy, 2)->complete,
        5000);
    const TerminalSearchUpdate multiline = *latestSearchUpdate(searchSpy, 2);
    QCOMPARE(multiline.totalMatches, quint64(1));
    QCOMPARE(multiline.visibleCellMask.size(), maskSize);
    int firstDecoratedRow = -1;
    bool spansRows = false;
    for (qsizetype index = 0; index < multiline.visibleCellMask.size();
         ++index) {
        if (!multiline.visibleCellMask.testBit(index)) continue;
        const int row = static_cast<int>(index / multiline.columns);
        if (firstDecoratedRow < 0) {
            firstDecoratedRow = row;
        } else if (row != firstDecoratedRow) {
            spansRows = true;
        }
    }
    QVERIFY(spansRows);
    worker.navigateSearch(2, TerminalSearchDirection::Next);
    const TerminalSearchUpdate selectedMultiline =
        *latestSearchUpdate(searchSpy, 2);
    QVERIFY(selectedMultiline.selectedCellMask.count(true) > 0);
    QVERIFY(maskIsSubset(selectedMultiline.selectedCellMask,
                         selectedMultiline.visibleCellMask));

    worker.search(3, QStringLiteral("Ä").toUtf8());
    QTRY_VERIFY_WITH_TIMEOUT(
        latestSearchUpdate(searchSpy, 3).has_value()
            && latestSearchUpdate(searchSpy, 3)->complete,
        5000);
    QCOMPARE(latestSearchUpdate(searchSpy, 3)->totalMatches, quint64(1));

    worker.search(4, QStringLiteral("界").toUtf8());
    QTRY_VERIFY_WITH_TIMEOUT(
        latestSearchUpdate(searchSpy, 4).has_value()
            && latestSearchUpdate(searchSpy, 4)->complete,
        5000);
    QCOMPARE(latestSearchUpdate(searchSpy, 4)->totalMatches, quint64(1));
    QCOMPARE(latestSearchUpdate(searchSpy, 4)->visibleCellMask.count(true),
             qsizetype(1));
    const TerminalFrame wideFrame = accumulatedFrame(updateSpy);
    const auto wideHead = std::ranges::find_if(
        wideFrame.cells,
        [](const TerminalCell &cell) { return cell.text == u"界"; });
    QVERIFY(wideHead != wideFrame.cells.cend());
    const qsizetype wideHeadIndex =
        std::ranges::distance(wideFrame.cells.cbegin(), wideHead);
    QCOMPARE(wideHead->columnSpan, 2);
    QVERIFY(wideHeadIndex + 1 < wideFrame.cells.size());
    QVERIFY(wideFrame.cells.at(wideHeadIndex + 1).spacer);
    QVERIFY(latestSearchUpdate(searchSpy, 4)->visibleCellMask.testBit(
        wideHeadIndex));
    worker.navigateSearch(4, TerminalSearchDirection::Next);
    QCOMPARE(latestSearchUpdate(searchSpy, 4)->selectedCellMask.count(true),
             qsizetype(1));

    worker.searchSerialized(5, QByteArrayLiteral("A\\x41"));
    QTRY_VERIFY_WITH_TIMEOUT(
        latestSearchUpdate(searchSpy, 5).has_value()
            && latestSearchUpdate(searchSpy, 5)->complete,
        5000);
    QCOMPARE(latestSearchUpdate(searchSpy, 5)->totalMatches, quint64(3));

    // The oldest match is wholly outside the live viewport. Previous starts
    // there, while Next wraps back to the newest result and follows it down.
    worker.search(6, QByteArrayLiteral("needle"));
    QTRY_VERIFY_WITH_TIMEOUT(
        latestSearchUpdate(searchSpy, 6).has_value()
            && latestSearchUpdate(searchSpy, 6)->complete,
        5000);
    QCOMPARE(latestSearchUpdate(searchSpy, 6)->totalMatches, quint64(500));
    worker.navigateSearch(6, TerminalSearchDirection::Previous);
    QCOMPARE(latestSearchUpdate(searchSpy, 6)->selectedMatch, qint64(499));
    QVERIFY(latestSearchUpdate(searchSpy, 6)->selectedCellMask.count(true) > 0);
    QTRY_VERIFY_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset == 0,
                             1000);
    worker.navigateSearch(6, TerminalSearchDirection::Next);
    QCOMPARE(latestSearchUpdate(searchSpy, 6)->selectedMatch, qint64(0));
    QTRY_VERIFY_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset > 0,
                             1000);

    worker.search(7, QByteArrayLiteral("needle"));
    worker.search(8, QByteArrayLiteral("aa"));
    worker.cancelSearch(9);
    QTRY_VERIFY_WITH_TIMEOUT(
        latestSearchUpdate(searchSpy, 9).has_value()
            && latestSearchUpdate(searchSpy, 9)->complete,
        1000);
    const TerminalSearchUpdate cancelled = *latestSearchUpdate(searchSpy, 9);
    QVERIFY(!cancelled.active);
    QCOMPARE(cancelled.columns, 0);
    QCOMPARE(cancelled.rows, 0);
    QVERIFY(cancelled.visibleCellMask.isEmpty());
    QVERIFY(cancelled.selectedCellMask.isEmpty());
    const int afterCancel = searchSpy.count();
    QTest::qWait(50);
    QCOMPARE(searchSpy.count(), afterCancel);

    worker.shutdown();
}

void SessionWorkerTest::preservesFormattedSearchBoundaries()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalSearchUpdate>();
    SessionWorker worker;
    worker.resizeTerminal(16, 4, 8, 16, 128, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy searchSpy(&worker, &SessionWorker::searchUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "printf 'abc%14s\\n12345678901234  tail\\nfinal-line\\n' ''")};
    options.hold = true;
    worker.initialize(options);

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!updateSpy.isEmpty(), 5000);
    QVERIFY(errorSpy.isEmpty());

    worker.search(1, QByteArrayLiteral("final-line\n"));
    const std::optional<TerminalSearchUpdate> initial =
        latestSearchUpdate(searchSpy, 1);
    QVERIFY(initial.has_value());
    QVERIFY(!initial->complete);
    QCOMPARE(initial->totalMatches, quint64(0));

    // Navigation is an instantaneous mailbox operation. A request made
    // before the first progressive chunk finds a result must not be replayed.
    worker.navigateSearch(1, TerminalSearchDirection::Next);
    QTRY_VERIFY_WITH_TIMEOUT(
        latestSearchUpdate(searchSpy, 1).has_value()
            && latestSearchUpdate(searchSpy, 1)->complete,
        5000);
    QCOMPARE(latestSearchUpdate(searchSpy, 1)->totalMatches, quint64(1));
    QCOMPARE(latestSearchUpdate(searchSpy, 1)->selectedMatch, qint64(-1));

    // The two spaces fill the final cells of the first physical row. They
    // remain part of the logical line when the following text soft-wraps.
    worker.search(2, QByteArrayLiteral("12345678901234  tail"));
    QTRY_VERIFY_WITH_TIMEOUT(
        latestSearchUpdate(searchSpy, 2).has_value()
            && latestSearchUpdate(searchSpy, 2)->complete,
        5000);
    QCOMPARE(latestSearchUpdate(searchSpy, 2)->totalMatches, quint64(1));

    // The first line wraps after a run of spaces but has no later nonblank
    // continuation. PageFormatter drops that pending run rather than making
    // it searchable.
    worker.search(3, QByteArrayLiteral("abc "));
    QTRY_VERIFY_WITH_TIMEOUT(
        latestSearchUpdate(searchSpy, 3).has_value()
            && latestSearchUpdate(searchSpy, 3)->complete,
        5000);
    QCOMPARE(latestSearchUpdate(searchSpy, 3)->totalMatches, quint64(0));

    worker.shutdown();
}

void SessionWorkerTest::runsCommandThroughPty()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"), QStringLiteral("-c"),
        QStringLiteral(
            "stty raw -echo; "
            "printf '\\033[c'; "
            "response=$(dd bs=1 count=12 2>/dev/null); "
            "stty sane; "
            "printf 'device-response:'; "
            "printf '%s' \"$response\" | od -An -tx1 | tr -d ' \\n'; "
            "printf '\\nghostty-qt-final\\n'")};
    options.hold = true;
    QVERIFY(worker.initialize(options));

    QTRY_VERIFY_WITH_TIMEOUT(exitSpy.count() > 0, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(updateSpy.count() > 0, 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty() ? "" : qPrintable(errorSpy.constFirst().constFirst().toString()));

    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    QVERIFY(exitSpy.constFirst().at(2).toBool());
    const TerminalFrame finalFrame = accumulatedFrame(updateSpy);
    const QString finalContents = frameText(finalFrame);
    QVERIFY2(finalContents.contains(
                 QStringLiteral("device-response:1b5b3f36323b32323b353263")),
             qPrintable(finalContents));
    QVERIFY2(finalContents.contains(QStringLiteral("ghostty-qt-final")),
             qPrintable(finalContents));
    QVERIFY(containsCursorBlinkReset(updateSpy));
    worker.shutdown();
}

void SessionWorkerTest::publishesDesktopNotificationsInParserOrder()
{
    qRegisterMetaType<TerminalDesktopNotification>();
    SessionWorker worker;
    QSignalSpy notificationSpy(&worker,
                               &SessionWorker::desktopNotificationRequested);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.command = TerminalCommand::direct({
        QByteArrayLiteral("/bin/printf"),
        QByteArrayLiteral("\033]9;worker-first\033\\"
                          "\033]777;notify;Worker;second\007"),
    });
    options.hold = true;
    QVERIFY(worker.initialize(options));

    QTRY_COMPARE_WITH_TIMEOUT(notificationSpy.count(), 2, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QCOMPARE(notificationSpy.at(0)
                 .constFirst()
                 .value<TerminalDesktopNotification>()
                 .title,
             QString{});
    QCOMPARE(notificationSpy.at(0)
                 .constFirst()
                 .value<TerminalDesktopNotification>()
                 .body,
             QStringLiteral("worker-first"));
    QCOMPARE(notificationSpy.at(1)
                 .constFirst()
                 .value<TerminalDesktopNotification>()
                 .title,
             QStringLiteral("Worker"));
    QCOMPARE(notificationSpy.at(1)
                 .constFirst()
                 .value<TerminalDesktopNotification>()
                 .body,
             QStringLiteral("second"));
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::publishesProgressReportsInParserOrder()
{
    qRegisterMetaType<TerminalProgressReport>();
    SessionWorker worker;
    QSignalSpy progressSpy(&worker, &SessionWorker::progressReportRequested);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.command = TerminalCommand::direct({
        QByteArrayLiteral("/bin/printf"),
        QByteArrayLiteral("\033]9;4;0;\033\\"
                          "\033]9;4;1;42\007"
                          "\033]9;4;2;7\033\\"
                          "\033]9;4;3\033\\"
                          "\033]9;4;4;75\033\\"),
    });
    options.hold = true;
    QVERIFY(worker.initialize(options));

    QTRY_COMPARE_WITH_TIMEOUT(progressSpy.count(), 5, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    const auto reportAt = [&progressSpy](qsizetype index) {
        return progressSpy.at(index)
            .constFirst()
            .value<TerminalProgressReport>();
    };
    QCOMPARE(reportAt(0).state, TerminalProgressState::Remove);
    QVERIFY(!reportAt(0).progress.has_value());
    QCOMPARE(reportAt(1).state, TerminalProgressState::Set);
    QVERIFY(reportAt(1).progress.has_value());
    QCOMPARE(*reportAt(1).progress, quint8{42});
    QCOMPARE(reportAt(2).state, TerminalProgressState::Error);
    QVERIFY(reportAt(2).progress.has_value());
    QCOMPARE(*reportAt(2).progress, quint8{7});
    QCOMPARE(reportAt(3).state, TerminalProgressState::Indeterminate);
    QVERIFY(!reportAt(3).progress.has_value());
    QCOMPARE(reportAt(4).state, TerminalProgressState::Pause);
    QVERIFY(reportAt(4).progress.has_value());
    QCOMPARE(*reportAt(4).progress, quint8{75});
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::writesPersistentTerminalFiles()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalActionResult>();
    QString screenPath;
    QString historyPath;
    QString selectionPath;
    const auto cleanupArtifacts = qScopeGuard([&] {
        for (const QString &path :
             {screenPath, historyPath, selectionPath}) {
            if (path.isEmpty()) continue;
            static_cast<void>(QFile::remove(path));
            static_cast<void>(
                QDir().rmdir(QFileInfo(path).absolutePath()));
        }
    });

    {
        SessionWorker worker;
        worker.resizeTerminal(12, 3, 8, 16, 96, 48);
        QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
        QSignalSpy actionSpy(
            &worker, &SessionWorker::terminalActionFinished);
        QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
        QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

        TerminalSessionLaunchOptions options;
        options.workingDirectory = QDir::tempPath();
        options.program = {
            QStringLiteral("/bin/sh"),
            QStringLiteral("-c"),
            QStringLiteral(
                "printf 'history-0  \\r\\n"
                "history-1\\r\\n"
                "screen-0  \\r\\n"
                "screen-1\\r\\n"
                "screen-2  '"),
        };
        options.hold = true;
        options.runtime.selectionClipboard.copyOnSelect =
            TerminalCopyOnSelectMode::Disabled;
        QVERIFY(worker.initialize(options));
        QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(
            updatesContain(updateSpy, QStringLiteral("screen-2")), 1000);

        worker.writeTerminalFile(101, {
            .location = TerminalFileLocation::Screen,
            .disposition = TerminalFileDisposition::Copy,
        });
        QCOMPARE(actionSpy.count(), 1);
        const TerminalActionResult screenResult =
            terminalActionResultAt(actionSpy, 0);
        QCOMPARE(screenResult.requestId, quint64{101});
        QCOMPARE(screenResult.outcome, TerminalActionOutcome::Success);
        QCOMPARE(screenResult.effect, TerminalActionEffect::Clipboard);
        QVERIFY(screenResult.performed);
        QCOMPARE(screenResult.clipboardDestination,
                 TerminalClipboardDestination::Standard);
        screenPath = screenResult.payload;

        worker.writeTerminalFile(202, {
            .location = TerminalFileLocation::Scrollback,
            .disposition = TerminalFileDisposition::Open,
        });
        QCOMPARE(actionSpy.count(), 2);
        const TerminalActionResult historyResult =
            terminalActionResultAt(actionSpy, 1);
        QCOMPARE(historyResult.requestId, quint64{202});
        QCOMPARE(historyResult.outcome, TerminalActionOutcome::Success);
        QCOMPARE(historyResult.effect, TerminalActionEffect::OpenFile);
        QVERIFY(historyResult.performed);
        QCOMPARE(historyResult.clipboardDestination,
                 TerminalClipboardDestination::Standard);
        historyPath = historyResult.payload;

        worker.beginSelection(selectionPress(0, 0));
        worker.updateSelection(selectionDrag(8, 1, true));
        worker.endSelection(8, 1);
        worker.writeTerminalFile(303, {
            .location = TerminalFileLocation::Selection,
            .disposition = TerminalFileDisposition::Copy,
        });
        QCOMPARE(actionSpy.count(), 3);
        const TerminalActionResult selectionResult =
            terminalActionResultAt(actionSpy, 2);
        QCOMPARE(selectionResult.requestId, quint64{303});
        QCOMPARE(selectionResult.outcome,
                 TerminalActionOutcome::Success);
        QCOMPARE(selectionResult.effect,
                 TerminalActionEffect::Clipboard);
        QVERIFY(selectionResult.performed);
        QCOMPARE(selectionResult.clipboardDestination,
                 TerminalClipboardDestination::Standard);
        selectionPath = selectionResult.payload;

        QCOMPARE(QFileInfo(screenPath).fileName(),
                 QStringLiteral("screen.txt"));
        QCOMPARE(QFileInfo(historyPath).fileName(),
                 QStringLiteral("history.txt"));
        QCOMPARE(QFileInfo(selectionPath).fileName(),
                 QStringLiteral("selection.txt"));
        QVERIFY(QFileInfo(screenPath).isAbsolute());
        QVERIFY(QFileInfo(historyPath).isAbsolute());
        QVERIFY(QFileInfo(selectionPath).isAbsolute());
        QVERIFY(QFileInfo(screenPath).absolutePath()
                != QFileInfo(historyPath).absolutePath());
        QVERIFY(QFileInfo(screenPath).absolutePath()
                != QFileInfo(selectionPath).absolutePath());
        QVERIFY(QFileInfo(historyPath).absolutePath()
                != QFileInfo(selectionPath).absolutePath());

        QFile screenFile(screenPath);
        QVERIFY(screenFile.open(QIODevice::ReadOnly));
        QCOMPARE(screenFile.readAll(), QByteArrayLiteral(
            "history-0  \n"
            "history-1\n"
            "screen-0  \n"
            "screen-1\n"
            "screen-2  "));
        screenFile.close();

        QFile historyFile(historyPath);
        QVERIFY(historyFile.open(QIODevice::ReadOnly));
        QCOMPARE(historyFile.readAll(), QByteArrayLiteral(
            "history-0  \n"
            "history-1"));
        historyFile.close();

        QFile selectionFile(selectionPath);
        QVERIFY(selectionFile.open(QIODevice::ReadOnly));
        QCOMPARE(selectionFile.readAll(), QByteArrayLiteral(
            "screen-0\n"
            "screen-1"));
        selectionFile.close();

        struct stat status {};
        const QByteArray encodedScreenPath = QFile::encodeName(screenPath);
        QVERIFY(::stat(encodedScreenPath.constData(), &status) == 0);
        QCOMPARE(status.st_mode & 0777, mode_t{0600});
        const QByteArray encodedScreenDirectory =
            QFile::encodeName(QFileInfo(screenPath).absolutePath());
        QVERIFY(::stat(encodedScreenDirectory.constData(), &status) == 0);
        QCOMPARE(status.st_mode & 0777, mode_t{0700});

        const QByteArray encodedHistoryPath = QFile::encodeName(historyPath);
        QVERIFY(::stat(encodedHistoryPath.constData(), &status) == 0);
        QCOMPARE(status.st_mode & 0777, mode_t{0600});
        const QByteArray encodedHistoryDirectory =
            QFile::encodeName(QFileInfo(historyPath).absolutePath());
        QVERIFY(::stat(encodedHistoryDirectory.constData(), &status) == 0);
        QCOMPARE(status.st_mode & 0777, mode_t{0700});

        const QByteArray encodedSelectionPath =
            QFile::encodeName(selectionPath);
        QVERIFY(::stat(encodedSelectionPath.constData(), &status) == 0);
        QCOMPARE(status.st_mode & 0777, mode_t{0600});
        const QByteArray encodedSelectionDirectory =
            QFile::encodeName(QFileInfo(selectionPath).absolutePath());
        QVERIFY(::stat(encodedSelectionDirectory.constData(), &status) == 0);
        QCOMPARE(status.st_mode & 0777, mode_t{0700});

        QVERIFY2(errorSpy.isEmpty(),
                 errorSpy.isEmpty()
                     ? ""
                     : qPrintable(
                           errorSpy.constFirst().constFirst().toString()));
        worker.shutdown();
    }

    // Successful artifacts deliberately outlive both the action and worker.
    QVERIFY(QFileInfo::exists(screenPath));
    QVERIFY(QFileInfo::exists(historyPath));
    QVERIFY(QFileInfo::exists(selectionPath));
    const QString screenDirectory = QFileInfo(screenPath).absolutePath();
    const QString historyDirectory = QFileInfo(historyPath).absolutePath();
    const QString selectionDirectory =
        QFileInfo(selectionPath).absolutePath();
    QVERIFY(QFile::remove(screenPath));
    QVERIFY(QFile::remove(historyPath));
    QVERIFY(QFile::remove(selectionPath));
    QVERIFY(QDir().rmdir(screenDirectory));
    QVERIFY(QDir().rmdir(historyDirectory));
    QVERIFY(QDir().rmdir(selectionDirectory));
}

void SessionWorkerTest::skipsUnavailableTerminalFiles()
{
    qRegisterMetaType<TerminalActionResult>();
    QVERIFY(QDir().mkpath(
        QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir controlDirectory(
        QDir::current().filePath(
            QStringLiteral("tmp/write-file-missing-XXXXXX")));
    QVERIFY(controlDirectory.isValid());
    const QString artifactRoot =
        QDir(controlDirectory.path()).filePath(QStringLiteral("artifacts"));
    QVERIFY(QDir().mkpath(artifactRoot));
    const ScopedEnvironmentVariable temporaryDirectory(
        QByteArrayLiteral("TMPDIR"), QFile::encodeName(artifactRoot));
    QCOMPARE(QFileInfo(QDir::tempPath()).canonicalFilePath(),
             QFileInfo(artifactRoot).canonicalFilePath());

    SessionWorker worker;
    QSignalSpy actionSpy(
        &worker, &SessionWorker::terminalActionFinished);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = controlDirectory.path();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    QVERIFY(worker.initialize(options));
    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);

    worker.writeTerminalFile(404, {
        .location = TerminalFileLocation::Scrollback,
        .disposition = TerminalFileDisposition::Copy,
    });
    QCOMPARE(actionSpy.count(), 1);
    const TerminalActionResult historyResult =
        terminalActionResultAt(actionSpy, 0);
    QCOMPARE(historyResult.requestId, quint64{404});
    QCOMPARE(historyResult.outcome,
             TerminalActionOutcome::Unavailable);
    QCOMPARE(historyResult.effect, TerminalActionEffect::None);
    QVERIFY(historyResult.performed);
    QVERIFY(historyResult.payload.isEmpty());
    QCOMPARE(historyResult.clipboardDestination,
             TerminalClipboardDestination::Standard);

    worker.writeTerminalFile(505, {
        .location = TerminalFileLocation::Selection,
        .disposition = TerminalFileDisposition::Open,
    });
    QCOMPARE(actionSpy.count(), 2);
    const TerminalActionResult selectionResult =
        terminalActionResultAt(actionSpy, 1);
    QCOMPARE(selectionResult.requestId, quint64{505});
    QCOMPARE(selectionResult.outcome,
             TerminalActionOutcome::Unavailable);
    QCOMPARE(selectionResult.effect, TerminalActionEffect::None);
    QVERIFY(selectionResult.performed);
    QVERIFY(selectionResult.payload.isEmpty());
    QCOMPARE(selectionResult.clipboardDestination,
             TerminalClipboardDestination::Standard);

    worker.writeTerminalFile(606, {
        .location = TerminalFileLocation::Screen,
        .disposition = TerminalFileDisposition::Copy,
        .format = static_cast<TerminalFileFormat>(0xff),
    });
    QCOMPARE(actionSpy.count(), 3);
    const TerminalActionResult failedResult =
        terminalActionResultAt(actionSpy, 2);
    QCOMPARE(failedResult.requestId, quint64{606});
    QCOMPARE(failedResult.outcome, TerminalActionOutcome::Failed);
    QCOMPARE(failedResult.effect, TerminalActionEffect::None);
    QVERIFY(!failedResult.performed);
    QVERIFY(failedResult.payload.isEmpty());
    QCOMPARE(failedResult.clipboardDestination,
             TerminalClipboardDestination::Standard);

    QVERIFY(QDir(artifactRoot)
                .entryList(QDir::Dirs | QDir::NoDotAndDotDot)
                .isEmpty());
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.constFirst().constFirst().toString(),
             QStringLiteral("Unsupported terminal file format"));
    worker.shutdown();
}

void SessionWorkerTest::reportsTerminalInitializationSeparatelyFromChildExec()
{
    SessionWorker worker;
    QSignalSpy startedSpy(&worker, &SessionWorker::started);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory =
        QDir::current().filePath(QStringLiteral("tmp"));
    QVERIFY(QDir().mkpath(options.workingDirectory.displayString()));
    options.program = {
        QStringLiteral("/ghostty-qt-test/nonexistent-child"),
    };
    options.hold = true;
    options.runtime.abnormalCommandExitRuntimeMilliseconds =
        std::numeric_limits<quint32>::max();

    // Worker initialization is accepted as soon as libghostty-vt exists. A
    // later child-side exec failure is an observed process exit and does not
    // turn the accepted terminal initialization into a false result.
    std::optional<bool> initializationResult;
    int errorsAtInitialization = -1;
    QVERIFY(worker.initialize(
        options, [&](bool initialized) {
            initializationResult = initialized;
            errorsAtInitialization = errorSpy.count();
        }));
    QCOMPARE(initializationResult, std::optional(true));
    QCOMPARE(errorsAtInitialization, 0);
    QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QVERIFY(errorSpy.isEmpty());
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 127);
    QCOMPARE(exitSpy.constFirst().at(1).toInt(), 0);
    QVERIFY(exitSpy.constFirst().at(2).toBool());
    QVERIFY(!exitSpy.constFirst().at(3).toBool());
    QVERIFY(exitSpy.constFirst().at(5).toBool());

    // The existing terminal makes a repeated attempt ineligible. It must not
    // replay either process-start or exit notification.
    initializationResult.reset();
    QVERIFY(!worker.initialize(
        options, [&](bool initialized) {
            initializationResult = initialized;
        }));
    QCOMPARE(initializationResult, std::optional(false));
    QVERIFY(errorSpy.isEmpty());
    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(exitSpy.count(), 1);

    worker.shutdown();
}

void SessionWorkerTest::reportsTerminalInitializationFailure()
{
    SessionWorker worker;
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.runtime.appearance.foregroundColor = QColor{};
    options.hold = true;

    std::optional<bool> initializationResult;
    int errorsAtInitialization = -1;
    QVERIFY(!worker.initialize(
        options, [&](bool initialized) {
            initializationResult = initialized;
            errorsAtInitialization = errorSpy.count();
        }));

    QCOMPARE(initializationResult, std::optional(false));
    QCOMPARE(errorsAtInitialization, 0);
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(errorSpy.constFirst().constFirst().toString().contains(
        QStringLiteral("Failed to initialize libghostty-vt")));
    QCOMPARE(exitSpy.count(), 1);
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 127);
    QCOMPARE(exitSpy.constFirst().at(1).toInt(), 0);
    QVERIFY(exitSpy.constFirst().at(2).toBool());

    worker.shutdown();
}

void SessionWorkerTest::initializesGeometryBeforeSpawningChild()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy startedSpy(&worker, &SessionWorker::started);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral(GHOSTTY_QT_TEST_PTY_GEOMETRY_PROBE),
    };
    options.hold = true;
    options.initialGeometry = TerminalSessionGeometry{
        .columns = 43,
        .rows = 17,
        .cellWidthPixels = 11,
        .cellHeightPixels = 19,
        // Deliberately differ from the exact cell-grid product so this also
        // proves forkpty receives the surface extent after asymmetric padding
        // has been removed.
        .surfaceWidthPixels = 487,
        .surfaceHeightPixels = 337,
        .padding = {.top = 7, .right = 23, .bottom = 29, .left = 13},
    };
    QCOMPARE(options.initialGeometry->terminalWidthPixels(), 451);
    QCOMPARE(options.initialGeometry->terminalHeightPixels(), 301);
    QVERIFY(worker.initialize(options));

    QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!updateSpy.isEmpty(), 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);

    const TerminalFrame finalFrame = accumulatedFrame(updateSpy);
    QCOMPARE(finalFrame.columns, 43);
    QCOMPARE(finalFrame.rows, 17);
    const QString finalContents = frameText(finalFrame);
    QVERIFY2(finalContents.contains(
                 QStringLiteral("ghostty-qt-pty-geometry:43:17:451:301")),
             qPrintable(finalContents));
    worker.shutdown();
}

void SessionWorkerTest::resizesPtyWithPaddingExcludedPixels()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy startedSpy(&worker, &SessionWorker::started);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("printf 'resize-geometry-ready\\n'; "
                       "IFS= read -r ignored; exec \"$1\""),
        QStringLiteral("resize-geometry-test"),
        QStringLiteral(GHOSTTY_QT_TEST_PTY_GEOMETRY_PROBE),
    };
    options.hold = true;
    options.initialGeometry = TerminalSessionGeometry{
        .columns = 20,
        .rows = 5,
        .cellWidthPixels = 8,
        .cellHeightPixels = 16,
        .surfaceWidthPixels = 180,
        .surfaceHeightPixels = 100,
        .padding = {.top = 3, .right = 5, .bottom = 7, .left = 11},
    };
    QVERIFY(worker.initialize(options));
    QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("resize-geometry-ready")),
        5000);

    const TerminalSessionGeometry resized{
        .columns = 31,
        .rows = 9,
        .cellWidthPixels = 7,
        .cellHeightPixels = 13,
        .surfaceWidthPixels = 333,
        .surfaceHeightPixels = 211,
        .padding = {.top = 11, .right = 17, .bottom = 19, .left = 23},
    };
    QCOMPARE(resized.terminalWidthPixels(), 293);
    QCOMPARE(resized.terminalHeightPixels(), 181);
    worker.resizeTerminal(resized);
    worker.sendKey({
        .key = Qt::Key_Return,
        .text = QStringLiteral("\r"),
        .pressed = true,
    });

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy,
                       QStringLiteral("ghostty-qt-pty-geometry:31:9:293:181")),
        5000);
    const TerminalFrame finalFrame = accumulatedFrame(updateSpy);
    QCOMPARE(finalFrame.columns, 31);
    QCOMPARE(finalFrame.rows, 9);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    worker.shutdown();
}

void SessionWorkerTest::stripsDesktopActivationFromChildEnvironment()
{
    const ScopedEnvironmentVariable activationToken(
        QByteArrayLiteral("XDG_ACTIVATION_TOKEN"),
        QByteArrayLiteral("must-not-leak"));
    const ScopedEnvironmentVariable startupId(
        QByteArrayLiteral("DESKTOP_STARTUP_ID"),
        QByteArrayLiteral("must-not-leak"));
    const ScopedEnvironmentVariable sentinel(
        QByteArrayLiteral("GHOSTTY_QT_CHILD_ENV_SENTINEL"),
        QByteArrayLiteral("preserved"));

    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    worker.resizeTerminal(120, 4, 8, 16, 960, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "printf 'xdg=%s startup=%s sentinel=%s\\n' "
            "\"${XDG_ACTIVATION_TOKEN-unset}\" "
            "\"${DESKTOP_STARTUP_ID-unset}\" "
            "\"${GHOSTTY_QT_CHILD_ENV_SENTINEL-unset}\""),
    };
    options.hold = true;
    worker.initialize(options);

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!updateSpy.isEmpty(), 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    const QString contents = frameText(accumulatedFrame(updateSpy));
    QVERIFY2(contents.contains(
                 QStringLiteral("xdg=unset startup=unset sentinel=preserved")),
             qPrintable(contents));
    worker.shutdown();
}

void SessionWorkerTest::injectsInitialInputAtomicallyInOrder()
{
    qRegisterMetaType<TerminalUpdate>();
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir processDirectory(QDir::current().filePath(
        QStringLiteral("tmp/initial-input-cwd-XXXXXX")));
    QTemporaryDir childDirectory(QDir::current().filePath(
        QStringLiteral("tmp/initial-input-child-cwd-XXXXXX")));
    QVERIFY(processDirectory.isValid());
    QVERIFY(childDirectory.isValid());

    const QByteArray pathBytes = QByteArray::fromHex("002d706174682dff");
    QFile source(processDirectory.filePath(QStringLiteral("source.bin")));
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(pathBytes), pathBytes.size());
    source.close();

    const QByteArray expectedInput =
        QByteArrayLiteral("raw") + pathBytes + QByteArrayLiteral("tail\n");
    const QString od = QStandardPaths::findExecutable(QStringLiteral("od"));
    const QString tr = QStandardPaths::findExecutable(QStringLiteral("tr"));
    QVERIFY(!od.isEmpty());
    QVERIFY(!tr.isEmpty());

    CurrentDirectoryRestore restoreDirectory;
    QVERIFY(QDir::setCurrent(processDirectory.path()));

    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy startedSpy(&worker, &SessionWorker::started);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    // Path input intentionally resolves against the process directory above,
    // not this distinct child directory.
    options.workingDirectory = childDirectory.path();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("stty raw -echo; "
                       "hex=$(\"$1\" -An -tx1 -N \"$3\" | \"$2\" -d ' \\n'); "
                       "printf 'initial-input:%s\\n' \"$hex\"; "
                       "printf 'ordinary-ready\\n'; "
                       "next=$(\"$1\" -An -tx1 -N 8 | \"$2\" -d ' \\n'); "
                       "stty sane; "
                       "printf 'ordinary-input:%s\\n' \"$next\""),
        QStringLiteral("initial-input-test"),
        od,
        tr,
        QString::number(expectedInput.size()),
    };
    options.initialInput = {
        TerminalInitialInputs::Raw{QByteArrayLiteral("raw")},
        TerminalInitialInputs::Path{QByteArrayLiteral("source.bin")},
        TerminalInitialInputs::Raw{QByteArrayLiteral("tail\n")},
    };
    options.hold = true;
    QVERIFY(worker.initialize(options));
    QCOMPARE(startedSpy.count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("ordinary-ready")), 5000);

    // This ordinary input is submitted after startup; the worker must have
    // already queued every configured chunk exactly once.
    worker.sendInputMethod({.commitText = QStringLiteral("ordinary")});

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy,
                       QStringLiteral("initial-input:%1")
                           .arg(QString::fromLatin1(expectedInput.toHex()))),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy,
                       QStringLiteral("ordinary-input:%1")
                           .arg(QString::fromLatin1(
                               QByteArrayLiteral("ordinary").toHex()))),
        5000);
    const QString contents = frameText(accumulatedFrame(updateSpy));
    QCOMPARE(contents.count(QStringLiteral("initial-input:")), 1);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    worker.shutdown();
}

void SessionWorkerTest::rejectsInvalidInitialInputBeforeSpawning()
{
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/initial-input-invalid-XXXXXX")));
    QVERIFY(directory.isValid());

    const QString validPath =
        directory.filePath(QStringLiteral("valid-input.bin"));
    QFile valid(validPath);
    QVERIFY(valid.open(QIODevice::WriteOnly));
    QCOMPARE(valid.write("valid\n"), qint64(6));
    valid.close();

    const QString touch =
        QStandardPaths::findExecutable(QStringLiteral("touch"));
    QVERIFY(!touch.isEmpty());
    const QString fifoPath =
        directory.filePath(QStringLiteral("non-regular-input"));
    const QByteArray encodedFifoPath = QFile::encodeName(fifoPath);
    QCOMPARE(::mkfifo(encodedFifoPath.constData(), 0600), 0);

    const auto rejects = [&](QByteArray invalidPath,
                             QStringView expectedDiagnostic) {
        const QString marker = directory.filePath(
            QStringLiteral("child-%1")
                .arg(
                    QFileInfo(QString::fromLocal8Bit(invalidPath)).fileName()));
        QFile::remove(marker);

        int cgroupMoveCalls = 0;
        SessionWorker worker(
            [&](qint64, const LinuxCgroupConfig &,
                bool) -> std::expected<void, QString> {
                ++cgroupMoveCalls;
                return {};
            },
            nullptr);
        QSignalSpy startedSpy(&worker, &SessionWorker::started);
        QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
        QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

        TerminalSessionLaunchOptions options;
        options.workingDirectory = directory.path();
        options.program = {touch, marker};
        options.linuxCgroup.mode = LinuxCgroupMode::Always;
        options.linuxCgroup.hardFail = true;
        options.initialInput = {
            TerminalInitialInputs::Raw{QByteArrayLiteral("prefix")},
            TerminalInitialInputs::Path{QFile::encodeName(validPath)},
            TerminalInitialInputs::Path{std::move(invalidPath)},
            TerminalInitialInputs::Raw{QByteArrayLiteral("suffix\n")},
        };
        options.hold = true;
        QVERIFY(worker.initialize(options));

        QCOMPARE(startedSpy.count(), 0);
        QCOMPARE(cgroupMoveCalls, 0);
        QCOMPARE(exitSpy.count(), 1);
        QCOMPARE(exitSpy.constFirst().at(0).toInt(), 127);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY2(errorSpy.constFirst().constFirst().toString().contains(
                     expectedDiagnostic, Qt::CaseInsensitive),
                 qPrintable(errorSpy.constFirst().constFirst().toString()));
        QVERIFY(!QFileInfo::exists(marker));
        worker.shutdown();
    };

    rejects(QFile::encodeName(
                directory.filePath(QStringLiteral("missing-input.bin"))),
            u"open");
    rejects(encodedFifoPath, u"regular file");
    // This procfs file is regular and openable by the current process, but a
    // sequential read at offset zero deterministically fails with EIO.
    rejects(QByteArrayLiteral("/proc/self/mem"), u"read");
}

void SessionWorkerTest::enforcesInitialInputFileSizeLimit()
{
    constexpr qint64 maximumBytes = 10 * 1024 * 1024;

    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/initial-input-limits-XXXXXX")));
    QVERIFY(directory.isValid());

    const QString maximumPath =
        directory.filePath(QStringLiteral("exactly-10-mib.bin"));
    QFile maximum(maximumPath);
    QVERIFY(maximum.open(QIODevice::WriteOnly));
    QVERIFY(maximum.resize(maximumBytes));
    maximum.close();

    {
        SessionWorker worker;
        QSignalSpy startedSpy(&worker, &SessionWorker::started);
        QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

        TerminalSessionLaunchOptions options;
        options.workingDirectory = directory.path();
        // Keep the child alive without consuming input. The acceptance
        // boundary is observable synchronously through `started`; shutdown
        // then discards any remaining nonblocking PTY queue.
        options.program = {
            QStringLiteral("/bin/sh"),
            QStringLiteral("-c"),
            QStringLiteral("sleep 30"),
        };
        options.initialInput = {
            TerminalInitialInputs::Path{QFile::encodeName(maximumPath)},
        };
        options.hold = true;
        QVERIFY(worker.initialize(options));
        QCOMPARE(startedSpy.count(), 1);
        QVERIFY2(
            errorSpy.isEmpty(),
            errorSpy.isEmpty()
                ? ""
                : qPrintable(errorSpy.constFirst().constFirst().toString()));
        worker.shutdown();
    }

    const QString oversizedPath =
        directory.filePath(QStringLiteral("over-10-mib.bin"));
    QFile oversized(oversizedPath);
    QVERIFY(oversized.open(QIODevice::WriteOnly));
    QVERIFY(oversized.resize(maximumBytes + 1));
    oversized.close();

    const QString marker =
        directory.filePath(QStringLiteral("oversized-child-started"));
    const QString touch =
        QStandardPaths::findExecutable(QStringLiteral("touch"));
    QVERIFY(!touch.isEmpty());

    SessionWorker worker;
    QSignalSpy startedSpy(&worker, &SessionWorker::started);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);
    TerminalSessionLaunchOptions options;
    options.workingDirectory = directory.path();
    options.program = {touch, marker};
    options.initialInput = {
        TerminalInitialInputs::Path{QFile::encodeName(oversizedPath)},
    };
    options.hold = true;
    QVERIFY(worker.initialize(options));

    QCOMPARE(startedSpy.count(), 0);
    QCOMPARE(exitSpy.count(), 1);
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(errorSpy.constFirst().constFirst().toString().contains(
        QStringLiteral("10 MiB")));
    QVERIFY(!QFileInfo::exists(marker));
    worker.shutdown();
}

void SessionWorkerTest::injectsRawTerminalIdentity()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    worker.resizeTerminal(120, 4, 8, 16, 960, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.term = QByteArrayLiteral("ghostty-qt-");
    options.term.append(char(0x80));
    options.term.append(char(0xff));
    options.workingDirectory = QDir::currentPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("printf 'term='; "
                       "printf '%s' \"$TERM\" "
                       "| /usr/bin/od -An -tx1 "
                       "| /usr/bin/tr -d ' \\n'; "
                       "printf '\\n'"),
    };
    options.hold = true;
    worker.initialize(options);

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!updateSpy.isEmpty(), 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    const QString contents = frameText(accumulatedFrame(updateSpy));
    QVERIFY2(
        contents.contains(QStringLiteral("term=67686f737474792d71742d80ff")),
        qPrintable(contents));
    worker.shutdown();
}

void SessionWorkerTest::usesConfiguredTerminalEnvironment()
{
    const TerminfoResolution terminfo = resolveRuntimeTerminfoDirectory();
    QVERIFY2(terminfo.has_value(),
             terminfo ? "" : qPrintable(terminfo.error()));
    const ScopedEnvironmentVariable inherited(
        QByteArrayLiteral("GHOSTTY_QT_CHILD_ENV_SENTINEL"),
        QByteArrayLiteral("inherited"));
    const ScopedEnvironmentVariable removedOverride(
        QByteArrayLiteral("GHOSTTY_QT_REMOVED_ENV"),
        QByteArrayLiteral("still-inherited"));
    const ScopedEnvironmentVariable activationToken(
        QByteArrayLiteral("XDG_ACTIVATION_TOKEN"),
        QByteArrayLiteral("sanitized"));
    const ScopedEnvironmentVariable parentPath(
        QByteArrayLiteral("PATH"), QByteArrayLiteral("/bin:/usr/bin"));

    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    worker.resizeTerminal(240, 14, 8, 16, 1920, 224);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.term = QByteArray("invalid\0term", 12);
    options.workingDirectory = QDir::currentPath();
    QByteArray configuredTerm = QByteArrayLiteral("configured-term-");
    configuredTerm.append(char(0x80));
    configuredTerm.append(char(0xff));
    QByteArray rawValue = QByteArrayLiteral("raw-");
    rawValue.append(char(0xfe));
    rawValue.append(char(0x81));
    rawValue.append(QByteArrayLiteral("=tail"));
    QByteArray rawKey = QByteArrayLiteral("raw-key-");
    rawKey.append(char(0x80));
    rawKey.append(char(0xff));
    options.environment = {
        {
            .key = QByteArrayLiteral("GHOSTTY_QT_CHILD_ENV_SENTINEL"),
            .value = QByteArrayLiteral("configured"),
        },
        {
            .key = QByteArrayLiteral("XDG_ACTIVATION_TOKEN"),
            .value = QByteArrayLiteral("configured-token"),
        },
        {.key = QByteArrayLiteral("TERM"), .value = configuredTerm},
        {
            .key = QByteArrayLiteral("TERMINFO"),
            .value = QByteArrayLiteral("configured-terminfo"),
        },
        {
            .key = QByteArrayLiteral("COLORTERM"),
            .value = QByteArrayLiteral("configured-color"),
        },
        {
            .key = QByteArrayLiteral("TERM_PROGRAM"),
            .value = QByteArrayLiteral("configured-program"),
        },
        {
            .key = QByteArrayLiteral("TERM_PROGRAM_VERSION"),
            .value = QByteArrayLiteral("configured-version"),
        },
        {
            .key = QByteArrayLiteral("PATH"),
            .value = QByteArrayLiteral("/configured/child/path"),
        },
        {
            .key = QByteArrayLiteral("PWD"),
            .value = QByteArrayLiteral("/configured/pwd/must-not-win"),
        },
        {
            .key = QByteArrayLiteral("GHOSTTY_QT_RAW_ENV"),
            .value = rawValue,
        },
        {
            .key = rawKey,
            .value = QByteArrayLiteral("RAW_KEY_VALUE"),
        },
    };
    options.program = {
        QStringLiteral("sh"),
        QStringLiteral("-c"),
        QStringLiteral("printf 'sentinel=%s xdg=%s removed=%s\\n' "
                       "\"$GHOSTTY_QT_CHILD_ENV_SENTINEL\" "
                       "\"$XDG_ACTIVATION_TOKEN\" \"$GHOSTTY_QT_REMOVED_ENV\"; "
                       "printf 'terminfo=%s color=%s program=%s version=%s\\n' "
                       "\"$TERMINFO\" \"$COLORTERM\" \"$TERM_PROGRAM\" "
                       "\"$TERM_PROGRAM_VERSION\"; "
                       "printf 'path=%s\\npwd=%s\\n' \"$PATH\" \"$PWD\"; "
                       "printf 'term='; printf '%s' \"$TERM\" "
                       "| /usr/bin/od -An -tx1 | /usr/bin/tr -d ' \\n'; "
                       "printf '\\nraw='; printf '%s' \"$GHOSTTY_QT_RAW_ENV\" "
                       "| /usr/bin/od -An -tx1 | /usr/bin/tr -d ' \\n'; "
                       "printf '\\nrawkey='; "
                       "/usr/bin/env | /usr/bin/grep -a -F 'RAW_KEY_VALUE' "
                       "| /usr/bin/od -An -tx1 | /usr/bin/tr -d ' \\n'; "
                       "printf '\\n'"),
    };
    options.hold = true;
    worker.initialize(options);

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!updateSpy.isEmpty(), 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    const QString contents = frameText(accumulatedFrame(updateSpy));
    QVERIFY2(contents.contains(
                 QStringLiteral("sentinel=configured xdg=configured-token "
                                "removed=still-inherited")),
             qPrintable(contents));
    QVERIFY2(contents.contains(QStringLiteral(
                 "terminfo=configured-terminfo color=configured-color "
                 "program=configured-program version=configured-version")),
             qPrintable(contents));
    QVERIFY2(contents.contains(QStringLiteral("path=/configured/child/path")),
             qPrintable(contents));
    QVERIFY2(
        contents.contains(QStringLiteral("pwd=%1").arg(QDir::currentPath())),
        qPrintable(contents));
    QVERIFY2(contents.contains(
                 QStringLiteral("term=636f6e666967757265642d7465726d2d80ff")),
             qPrintable(contents));
    QVERIFY2(contents.contains(QStringLiteral("raw=7261772dfe813d7461696c")),
             qPrintable(contents));
    QVERIFY2(contents.contains(QStringLiteral(
                 "rawkey=7261772d6b65792d80ff3d5241575f4b45595f56414c55450a")),
             qPrintable(contents));
    worker.shutdown();
}

void SessionWorkerTest::appliesConfiguredEnvironmentPwdPrecedence()
{
    const TerminfoResolution terminfo = resolveRuntimeTerminfoDirectory();
    QVERIFY2(terminfo.has_value(),
             terminfo ? "" : qPrintable(terminfo.error()));
    qRegisterMetaType<TerminalUpdate>();
    const QString concreteDirectory = QDir::currentPath();

    for (const bool inheritWorkingDirectory : {true, false}) {
        SessionWorker worker;
        worker.resizeTerminal(160, 16, 8, 16, 1280, 256);
        QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
        QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
        QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

        TerminalSessionLaunchOptions options;
        options.inheritWorkingDirectory = inheritWorkingDirectory;
        options.workingDirectory = concreteDirectory;
        options.environment = {{
            .key = QByteArrayLiteral("PWD"),
            .value = QByteArrayLiteral("/configured/logical/pwd"),
        }};
        options.program = {QStringLiteral("/usr/bin/env")};
        options.hold = true;
        worker.initialize(options);

        QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!updateSpy.isEmpty(), 5000);
        QVERIFY2(
            errorSpy.isEmpty(),
            errorSpy.isEmpty()
                ? ""
                : qPrintable(errorSpy.constFirst().constFirst().toString()));
        QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
        const QString contents = frameText(accumulatedFrame(updateSpy));
        const QString expected = inheritWorkingDirectory
            ? QStringLiteral("PWD=/configured/logical/pwd")
            : QStringLiteral("PWD=%1").arg(concreteDirectory);
        QVERIFY2(contents.contains(expected), qPrintable(contents));
        QVERIFY2(contents.contains(QStringLiteral("TERM=xterm-ghostty")),
                 qPrintable(contents));
        QVERIFY2(
            contents.contains(QStringLiteral("TERMINFO=%1").arg(*terminfo)),
            qPrintable(contents));
        QVERIFY2(contents.contains(QStringLiteral("COLORTERM=truecolor")),
                 qPrintable(contents));
        QVERIFY2(contents.contains(QStringLiteral("TERM_PROGRAM=ghostty-qt")),
                 qPrintable(contents));
        QVERIFY2(
            contents.contains(QStringLiteral("TERM_PROGRAM_VERSION=%1")
                                  .arg(QStringLiteral(GHOSTTY_QT_VERSION))),
            qPrintable(contents));
        worker.shutdown();
    }
}

void SessionWorkerTest::rejectsInvalidConfiguredEnvironment()
{
    struct InvalidCase {
        TerminalEnvironment environment;
        QString diagnostic;
    };

    QByteArray nulKey("A\0B", 3);
    QByteArray nulValue("A\0B", 3);
    const QVector<InvalidCase> cases = {
        {
            .environment = {{.key = QByteArrayLiteral("A=B"),
                             .value = QByteArrayLiteral("value")}},
            .diagnostic = QStringLiteral("not representable by execve"),
        },
        {
            .environment = {{.key = nulKey,
                             .value = QByteArrayLiteral("value")}},
            .diagnostic = QStringLiteral("not representable by execve"),
        },
        {
            .environment = {{.key = QByteArrayLiteral("EMPTY"), .value = {}}},
            .diagnostic = QStringLiteral("not representable by execve"),
        },
        {
            .environment = {{.key = QByteArrayLiteral("NUL"),
                             .value = nulValue}},
            .diagnostic = QStringLiteral("not representable by execve"),
        },
        {
            .environment =
                {
                    {
                        .key = QByteArrayLiteral("DUPLICATE"),
                        .value = QByteArrayLiteral("first"),
                    },
                    {
                        .key = QByteArrayLiteral("DUPLICATE"),
                        .value = QByteArrayLiteral("second"),
                    },
                },
            .diagnostic = QStringLiteral("duplicate key"),
        },
    };

    for (const InvalidCase &testCase : cases) {
        SessionWorker worker;
        QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
        QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

        TerminalSessionLaunchOptions options;
        options.environment = testCase.environment;
        options.program = {QStringLiteral("/bin/true")};
        options.hold = true;

        std::optional<bool> initializationResult;
        QVERIFY(worker.initialize(options, [&](bool initialized) {
            initializationResult = initialized;
        }));
        QCOMPARE(initializationResult, std::optional(true));
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY2(errorSpy.constFirst().constFirst().toString().contains(
                     testCase.diagnostic),
                 qPrintable(errorSpy.constFirst().constFirst().toString()));
        QCOMPARE(exitSpy.count(), 1);
        QCOMPARE(exitSpy.constFirst().at(0).toInt(), 127);
        QCOMPARE(exitSpy.constFirst().at(1).toInt(), 0);
        QVERIFY(exitSpy.constFirst().at(2).toBool());
        worker.shutdown();
    }
}

void SessionWorkerTest::cgroupGateMovesChildBeforeExec()
{
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/cgroup-gate-success-XXXXXX")));
    QVERIFY(directory.isValid());
    const QString childMarker =
        directory.filePath(QStringLiteral("child-executed"));

    int moveCalls = 0;
    qint64 movedPid = -1;
    LinuxCgroupConfig observedConfig;
    bool markerAbsentDuringMove = false;
    SessionWorker worker(
        [&](qint64 pid, const LinuxCgroupConfig &config,
            bool singleInstance) -> std::expected<void, QString> {
            ++moveCalls;
            movedPid = pid;
            observedConfig = config;
            QThread::msleep(100);
            markerAbsentDuringMove = !QFileInfo::exists(childMarker);
            if (!singleInstance) {
                return std::unexpected(
                    QStringLiteral("single-instance role was lost"));
            }
            return {};
        },
        nullptr);
    QSignalSpy startedSpy(&worker, &SessionWorker::started);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.linuxCgroup = {
        .mode = LinuxCgroupMode::Always,
        .memoryLimitBytes = quint64{4096},
        .processesLimit = quint64{7},
        .hardFail = true,
    };
    options.processUsesSingleInstance = true;
    options.workingDirectory = directory.path();
    options.program = {
        QStandardPaths::findExecutable(QStringLiteral("touch")),
        childMarker,
    };
    options.hold = true;
    QVERIFY(worker.initialize(options));

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(moveCalls, 1);
    QVERIFY(movedPid > 0);
    QCOMPARE(observedConfig, options.linuxCgroup);
    QVERIFY(markerAbsentDuringMove);
    QVERIFY(QFileInfo::exists(childMarker));
    QVERIFY(errorSpy.isEmpty());
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    worker.shutdown();
}

void SessionWorkerTest::cgroupSoftFailureContinuesLaunch()
{
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/cgroup-gate-soft-XXXXXX")));
    QVERIFY(directory.isValid());
    const QString childMarker =
        directory.filePath(QStringLiteral("child-executed"));

    int moveCalls = 0;
    SessionWorker worker(
        [&](qint64, const LinuxCgroupConfig &,
            bool) -> std::expected<void, QString> {
            ++moveCalls;
            QThread::msleep(100);
            return std::unexpected(QStringLiteral("synthetic soft failure"));
        },
        nullptr);
    QSignalSpy startedSpy(&worker, &SessionWorker::started);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.linuxCgroup.mode = LinuxCgroupMode::Always;
    options.linuxCgroup.hardFail = false;
    options.workingDirectory = directory.path();
    options.program = {
        QStandardPaths::findExecutable(QStringLiteral("touch")),
        childMarker,
    };
    options.hold = true;
    QTest::ignoreMessage(
        QtWarningMsg,
        "Could not isolate terminal child in a transient systemd scope: "
        "synthetic soft failure (continuing because "
        "linux-cgroup-hard-fail is false)");
    QVERIFY(worker.initialize(options));

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QCOMPARE(moveCalls, 1);
    QCOMPARE(startedSpy.count(), 1);
    QVERIFY(QFileInfo::exists(childMarker));
    QVERIFY(errorSpy.isEmpty());
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    worker.shutdown();
}

void SessionWorkerTest::cgroupHardFailurePreventsExec()
{
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/cgroup-gate-hard-XXXXXX")));
    QVERIFY(directory.isValid());
    const QString childMarker =
        directory.filePath(QStringLiteral("child-executed"));

    int moveCalls = 0;
    SessionWorker worker(
        [&](qint64, const LinuxCgroupConfig &,
            bool) -> std::expected<void, QString> {
            ++moveCalls;
            QThread::msleep(100);
            return std::unexpected(QStringLiteral("synthetic hard failure"));
        },
        nullptr);
    QSignalSpy startedSpy(&worker, &SessionWorker::started);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.linuxCgroup.mode = LinuxCgroupMode::Always;
    options.linuxCgroup.hardFail = true;
    options.workingDirectory = directory.path();
    options.program = {
        QStandardPaths::findExecutable(QStringLiteral("touch")),
        childMarker,
    };
    options.hold = true;
    QVERIFY(worker.initialize(options));

    QCOMPARE(moveCalls, 1);
    QCOMPARE(startedSpy.count(), 0);
    QCOMPARE(exitSpy.count(), 1);
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 127);
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(errorSpy.constFirst().constFirst().toString().contains(
        QStringLiteral("synthetic hard failure")));
    QVERIFY(!QFileInfo::exists(childMarker));
    worker.shutdown();
}

void SessionWorkerTest::disabledCgroupSkipsScopeMove()
{
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(
        QDir::current().filePath(QStringLiteral("tmp/cgroup-disabled-XXXXXX")));
    QVERIFY(directory.isValid());
    const QString childMarker =
        directory.filePath(QStringLiteral("child-executed"));

    int moveCalls = 0;
    SessionWorker worker(
        [&](qint64, const LinuxCgroupConfig &,
            bool) -> std::expected<void, QString> {
            ++moveCalls;
            return std::unexpected(
                QStringLiteral("disabled policy invoked the mover"));
        },
        nullptr);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.linuxCgroup.mode = LinuxCgroupMode::Never;
    options.processUsesSingleInstance = true;
    options.workingDirectory = directory.path();
    options.program = {
        QStandardPaths::findExecutable(QStringLiteral("touch")),
        childMarker,
    };
    options.hold = true;
    QVERIFY(worker.initialize(options));

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QCOMPARE(moveCalls, 0);
    QVERIFY(QFileInfo::exists(childMarker));
    QVERIFY(errorSpy.isEmpty());
    worker.shutdown();
}

void SessionWorkerTest::fallsBackFromUnavailableWorkingDirectory_data()
{
    QTest::addColumn<bool>("existingNonDirectory");
    QTest::newRow("missing") << false;
    QTest::newRow("existing-non-directory") << true;
}

void SessionWorkerTest::appliesPinnedShellIntegrationLaunchOrdering()
{
#if !GHOSTTY_QT_TEST_SHELL_INTEGRATION_ENABLED
    QSKIP("The pinned shell transformer is disabled with Ghostty config");
#else
    {
        SessionWorker worker;
        QSignalSpy updates(&worker, &SessionWorker::terminalUpdated);
        QSignalSpy exited(&worker, &SessionWorker::sessionExited);
        QSignalSpy errors(&worker, &SessionWorker::errorOccurred);

        TerminalSessionLaunchOptions options;
        options.workingDirectory = QDir::tempPath();
        options.command = TerminalCommand::shell(
            QByteArrayLiteral(
                "printf 'features=<%s> program=<%s> resources=<%s>\\n' "
                "\"$GHOSTTY_SHELL_FEATURES\" \"$TERM_PROGRAM\" "
                "\"${GHOSTTY_RESOURCES_DIR:+set}\""),
            true);
        options.environment = {{
            .key = QByteArrayLiteral("TERM_PROGRAM"),
            .value = QByteArrayLiteral("configured-after-integration"),
        }};
        options.shellIntegration = GhosttyShellIntegrationMode::None;
        options.shellIntegrationFeatures = {
            .cursor = true,
            .sudo = true,
            .title = false,
            .sshEnvironment = false,
            .sshTerminfo = false,
            .path = false,
        };
        options.shellIntegrationAvailable = true;
        options.runtime.appearance.cursorBlink = false;
        options.hold = true;

        QVERIFY(worker.initialize(options));
        QTRY_COMPARE_WITH_TIMEOUT(exited.count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(
            updatesContain(
                updates,
                QStringLiteral(
                    "features=<cursor:steady,sudo> "
                    "program=<configured-after-integration> resources=<set>")),
            5000);
        QVERIFY2(errors.isEmpty(),
                 errors.isEmpty()
                     ? ""
                     : qPrintable(errors.constFirst().constFirst().toString()));
        worker.shutdown();
    }

    // A frontend explicit program has Ghostty `-e` semantics: a configured
    // forced shell becomes detection, so arbitrary programs do not inherit
    // shell-specific launch mutations.
    ScopedEnvironmentVariable zdotdir(QByteArrayLiteral("ZDOTDIR"),
                                      QByteArrayLiteral("/original/zsh"));
    ScopedEnvironmentVariable preserved(
        QByteArrayLiteral("GHOSTTY_ZSH_ZDOTDIR"), QByteArrayLiteral("absent"));
    qunsetenv("GHOSTTY_ZSH_ZDOTDIR");

    SessionWorker worker;
    QSignalSpy updates(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exited(&worker, &SessionWorker::sessionExited);
    QSignalSpy errors(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("printf 'zdotdir=<%s> preserved=<%s>\\n' "
                       "\"$ZDOTDIR\" \"${GHOSTTY_ZSH_ZDOTDIR-unset}\""),
    };
    options.shellIntegration = GhosttyShellIntegrationMode::Zsh;
    options.shellIntegrationAvailable = true;
    options.hold = true;

    QVERIFY(worker.initialize(options));
    QTRY_COMPARE_WITH_TIMEOUT(exited.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(
            updates,
            QStringLiteral("zdotdir=</original/zsh> preserved=<unset>")),
        5000);
    QVERIFY2(errors.isEmpty(),
             errors.isEmpty()
                 ? ""
                 : qPrintable(errors.constFirst().constFirst().toString()));
    worker.shutdown();
#endif
}

void SessionWorkerTest::fallsBackFromUnavailableWorkingDirectory()
{
    QFETCH(bool, existingNonDirectory);
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    worker.resizeTerminal(240, 4, 8, 16, 1920, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);
    QSignalSpy directorySpy(&worker,
                            &SessionWorker::currentDirectoryChanged);

    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/working-directory-fallback-XXXXXX")));
    QVERIFY(directory.isValid());
    const QString requested =
        directory.filePath(QStringLiteral("no-longer-present"));
    if (existingNonDirectory) {
        QFile file(requested);
        QVERIFY(file.open(QIODevice::WriteOnly));
    }

    TerminalSessionLaunchOptions options;
    options.workingDirectory = requested;
    options.program = {
        QStringLiteral("/usr/bin/printenv"),
        QStringLiteral("PWD"),
    };
    options.hold = true;
    worker.initialize(options);

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!updateSpy.isEmpty(), 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    const QString contents = frameText(accumulatedFrame(updateSpy));
    QCOMPARE(directorySpy.count(), 1);
    QCOMPARE(directorySpy.constFirst().constFirst().toString(),
             requested);
    QVERIFY2(contents.contains(requested), qPrintable(contents));
    worker.shutdown();
}

void SessionWorkerTest::preservesInheritedLogicalPwd()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    worker.resizeTerminal(240, 4, 8, 16, 1920, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);
    QSignalSpy directorySpy(&worker,
                            &SessionWorker::currentDirectoryChanged);

    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/inherited-pwd-XXXXXX")));
    QVERIFY(directory.isValid());
    const QString logicalDirectory =
        directory.filePath(QStringLiteral("logical-cwd"));
    QVERIFY(QFile::link(QDir::currentPath(), logicalDirectory));

    const ScopedEnvironmentVariable pwd(
        QByteArrayLiteral("PWD"), QFile::encodeName(logicalDirectory));

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QStringLiteral("/ignored-in-inherit-mode");
    options.inheritWorkingDirectory = true;
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("printf 'environment=%s\\nactual=' \"$PWD\"; /bin/pwd -P"),
    };
    options.hold = true;
    worker.initialize(options);

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!updateSpy.isEmpty(), 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    QVERIFY(directorySpy.isEmpty());
    const QString contents = frameText(accumulatedFrame(updateSpy));
    QVERIFY2(contents.contains(
                 QStringLiteral("environment=%1").arg(logicalDirectory)),
             qPrintable(contents));
    QVERIFY2(contents.contains(
                 QStringLiteral("actual=%1").arg(QDir::currentPath())),
             qPrintable(contents));
    worker.shutdown();
}

void SessionWorkerTest::preservesSymlinkSensitiveWorkingDirectory()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    worker.resizeTerminal(240, 4, 8, 16, 1920, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);
    QSignalSpy directorySpy(&worker,
                            &SessionWorker::currentDirectoryChanged);

    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/symlink-working-directory-XXXXXX")));
    QVERIFY(directory.isValid());
    const QDir root(directory.path());
    const QString base = root.filePath(QStringLiteral("base"));
    const QString linkedDirectory =
        root.filePath(QStringLiteral("other/directory"));
    const QString actualTarget =
        root.filePath(QStringLiteral("other/target"));
    const QString lexicallyCleanedTarget =
        root.filePath(QStringLiteral("base/target"));
    for (const QString &path : {
             base, linkedDirectory, actualTarget, lexicallyCleanedTarget,
         }) {
        QVERIFY(QDir().mkpath(path));
    }
    QVERIFY(QFile::link(linkedDirectory,
                        QDir(base).filePath(QStringLiteral("link"))));
    const QString requested =
        QDir(base).filePath(QStringLiteral("link/../target"));

    constexpr QLatin1StringView reporterName("report-location.sh");
    QVERIFY(writeExecutableScript(
        QDir(actualTarget).filePath(reporterName),
        QByteArrayLiteral(
            "#!/bin/sh\nprintf 'marker=kernel\\nenvironment=%s\\nactual=' \"$PWD\"\n/bin/pwd -P\n")));
    QVERIFY(writeExecutableScript(
        QDir(lexicallyCleanedTarget).filePath(reporterName),
        QByteArrayLiteral(
            "#!/bin/sh\nprintf 'marker=lexical\\nenvironment=%s\\nactual=' \"$PWD\"\n/bin/pwd -P\n")));

    TerminalSessionLaunchOptions options;
    options.workingDirectory = requested;
    options.program = {
        QStringLiteral("./report-location.sh"),
    };
    options.hold = true;
    worker.initialize(options);

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!updateSpy.isEmpty(), 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    QCOMPARE(directorySpy.count(), 1);
    QCOMPARE(directorySpy.constFirst().constFirst().toString(), requested);
    const QString contents = frameText(accumulatedFrame(updateSpy));
    QVERIFY2(contents.contains(QStringLiteral("marker=kernel")),
             qPrintable(contents));
    QVERIFY2(!contents.contains(QStringLiteral("marker=lexical")),
             qPrintable(contents));
    QVERIFY2(contents.contains(
                 QStringLiteral("environment=%1").arg(requested)),
             qPrintable(contents));
    QVERIFY2(contents.contains(
                 QStringLiteral("actual=%1").arg(actualTarget)),
             qPrintable(contents));
    QVERIFY2(!contents.contains(lexicallyCleanedTarget),
             qPrintable(contents));
    worker.shutdown();
}

void SessionWorkerTest::preservesNonUtf8WorkingDirectoryBytes()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    worker.resizeTerminal(320, 4, 8, 16, 2560, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);
    QSignalSpy directorySpy(&worker, &SessionWorker::currentDirectoryChanged);

    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir root(QDir::current().filePath(
        QStringLiteral("tmp/raw-working-directory-XXXXXX")));
    QVERIFY(root.isValid());
    QByteArray directory = QFile::encodeName(root.path());
    directory.append(QByteArrayLiteral("/bytes-"));
    directory.append(char(0x80));
    directory.append(char(0xff));
    QVERIFY2(::mkdir(directory.constData(), 0700) == 0, std::strerror(errno));

    TerminalSessionLaunchOptions options;
    options.workingDirectory = directory;
    options.command = TerminalCommand::direct({
        QByteArrayLiteral("/bin/sh"),
        QByteArrayLiteral("-c"),
        QByteArrayLiteral(
            "printf 'pwd='; printf '%s' \"$PWD\" | /usr/bin/od -An -v -tx1 | /usr/bin/tr -d ' \\n'; printf '\\ncwd='; /bin/pwd | /usr/bin/od -An -v -tx1 | /usr/bin/tr -d ' \\n'"),
    });
    options.hold = true;
    QVERIFY(worker.initialize(options));

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!updateSpy.isEmpty(), 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    QCOMPARE(directorySpy.count(), 1);
    QCOMPARE(directorySpy.constFirst().constFirst().toByteArray(), directory);
    const QString contents = frameText(accumulatedFrame(updateSpy));
    QVERIFY2(contents.contains(QStringLiteral("pwd=")
                               + QString::fromLatin1(directory.toHex())),
             qPrintable(contents));
    QVERIFY2(
        contents.contains(QStringLiteral("cwd=")
                          + QString::fromLatin1((directory + '\n').toHex())),
        qPrintable(contents));
    worker.shutdown();
    QCOMPARE(::rmdir(directory.constData()), 0);
}

void SessionWorkerTest::rejectsNulWorkingDirectoryBeforePosixCalls()
{
    SessionWorker worker;
    worker.resizeTerminal(80, 4, 8, 16, 640, 64);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);
    QSignalSpy directorySpy(&worker, &SessionWorker::currentDirectoryChanged);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QByteArray("/invalid\0suffix", 15);
    options.command = TerminalCommand::direct({QByteArrayLiteral("/bin/true")});
    QVERIFY(worker.initialize(options));
    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 1000);
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(errorSpy.constFirst().constFirst().toString().contains(
        QStringLiteral("contains a NUL byte")));
    QVERIFY(directorySpy.isEmpty());
    worker.shutdown();
}

void SessionWorkerTest::resolvesRelativePathEntriesFromChildWorkingDirectory_data()
{
    QTest::addColumn<QByteArray>("pathValue");
    QTest::addColumn<QString>("toolDirectory");
    QTest::newRow("dot")
        << QByteArrayLiteral(".") << QString{};
    QTest::newRow("relative-directory")
        << QByteArrayLiteral("tools") << QStringLiteral("tools");
}

void SessionWorkerTest::resolvesRelativePathEntriesFromChildWorkingDirectory()
{
    QFETCH(QByteArray, pathValue);
    QFETCH(QString, toolDirectory);
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    worker.resizeTerminal(120, 4, 8, 16, 960, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/relative-path-entry-XXXXXX")));
    QVERIFY(directory.isValid());
    const QDir root(directory.path());
    const QString parentDirectory =
        root.filePath(QStringLiteral("parent"));
    const QString childDirectory =
        QDir(parentDirectory).filePath(QStringLiteral("child"));
    QVERIFY(QDir().mkpath(childDirectory));
    const QString parentToolDirectory = toolDirectory.isEmpty()
        ? parentDirectory
        : QDir(parentDirectory).filePath(toolDirectory);
    const QString childToolDirectory = toolDirectory.isEmpty()
        ? childDirectory
        : QDir(childDirectory).filePath(toolDirectory);
    QVERIFY(QDir().mkpath(parentToolDirectory));
    QVERIFY(QDir().mkpath(childToolDirectory));
    constexpr QLatin1StringView toolName("working-directory-tool");
    QVERIFY(writeExecutableScript(
        QDir(parentToolDirectory).filePath(toolName),
        QByteArrayLiteral("#!/bin/sh\nprintf 'selected=parent\\n'\n")));
    QVERIFY(writeExecutableScript(
        QDir(childToolDirectory).filePath(toolName),
        QByteArrayLiteral("#!/bin/sh\nprintf 'selected=child\\n'\n")));

    const CurrentDirectoryRestore currentDirectory;
    QVERIFY(QDir::setCurrent(parentDirectory));
    const ScopedEnvironmentVariable path(
        QByteArrayLiteral("PATH"), pathValue);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = childDirectory;
    options.program = {toolName.toString()};
    options.hold = true;
    worker.initialize(options);

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!updateSpy.isEmpty(), 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    const QString contents = frameText(accumulatedFrame(updateSpy));
    QVERIFY2(contents.contains(QStringLiteral("selected=child")),
             qPrintable(contents));
    QVERIFY2(!contents.contains(QStringLiteral("selected=parent")),
             qPrintable(contents));
    worker.shutdown();
}

void SessionWorkerTest::skipsEmptyPathEntries()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    worker.resizeTerminal(120, 4, 8, 16, 960, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/empty-path-entry-XXXXXX")));
    QVERIFY(directory.isValid());
    const QDir root(directory.path());
    const QString childDirectory = root.filePath(QStringLiteral("child"));
    const QString fallbackDirectory =
        root.filePath(QStringLiteral("absolute-fallback"));
    QVERIFY(QDir().mkpath(childDirectory));
    QVERIFY(QDir().mkpath(fallbackDirectory));
    constexpr QLatin1StringView toolName("empty-entry-tool");
    QVERIFY(writeExecutableScript(
        QDir(childDirectory).filePath(toolName),
        QByteArrayLiteral("#!/bin/sh\nprintf 'selected=child\\n'\n")));
    QVERIFY(writeExecutableScript(
        QDir(fallbackDirectory).filePath(toolName),
        QByteArrayLiteral("#!/bin/sh\nprintf 'selected=fallback\\n'\n")));

    const ScopedEnvironmentVariable path(
        QByteArrayLiteral("PATH"),
        QByteArrayLiteral(":") + QFile::encodeName(fallbackDirectory));
    TerminalSessionLaunchOptions options;
    options.workingDirectory = childDirectory;
    options.program = {toolName.toString()};
    options.hold = true;
    worker.initialize(options);

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!updateSpy.isEmpty(), 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    const QString contents = frameText(accumulatedFrame(updateSpy));
    QVERIFY2(contents.contains(QStringLiteral("selected=fallback")),
             qPrintable(contents));
    QVERIFY2(!contents.contains(QStringLiteral("selected=child")),
             qPrintable(contents));
    worker.shutdown();
}

void SessionWorkerTest::continuesPathLookupAfterMissingInterpreter()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    worker.resizeTerminal(120, 4, 8, 16, 960, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/path-exec-fallback-XXXXXX")));
    QVERIFY(directory.isValid());
    const QDir root(directory.path());
    const QString childDirectory = root.filePath(QStringLiteral("child"));
    const QString firstDirectory =
        QDir(childDirectory).filePath(QStringLiteral("first"));
    const QString secondDirectory =
        QDir(childDirectory).filePath(QStringLiteral("second"));
    QVERIFY(QDir().mkpath(firstDirectory));
    QVERIFY(QDir().mkpath(secondDirectory));
    constexpr QLatin1StringView toolName("exec-fallback-tool");
    QVERIFY(writeExecutableScript(
        QDir(firstDirectory).filePath(toolName),
        QByteArrayLiteral("#!/definitely/missing\n")));
    QVERIFY(writeExecutableScript(
        QDir(secondDirectory).filePath(toolName),
        QByteArrayLiteral("#!/bin/sh\nprintf 'selected=second\\n'\n")));

    const ScopedEnvironmentVariable path(
        QByteArrayLiteral("PATH"), QByteArrayLiteral("first:second"));
    TerminalSessionLaunchOptions options;
    options.workingDirectory = childDirectory;
    options.program = {toolName.toString()};
    options.hold = true;
    worker.initialize(options);

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!updateSpy.isEmpty(), 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    const QString contents = frameText(accumulatedFrame(updateSpy));
    QVERIFY2(contents.contains(QStringLiteral("selected=second")),
             qPrintable(contents));
    worker.shutdown();
}

void SessionWorkerTest::usesPinnedDefaultPathWhenUnset()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    worker.resizeTerminal(120, 4, 8, 16, 960, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/unset-path-default-XXXXXX")));
    QVERIFY(directory.isValid());
    const QString childDirectory =
        QDir(directory.path()).filePath(QStringLiteral("child"));
    QVERIFY(QDir().mkpath(childDirectory));
    QVERIFY(writeExecutableScript(
        QDir(childDirectory).filePath(QStringLiteral("sh")),
        QByteArrayLiteral("#!/bin/sh\nprintf 'selected=child\\n'\n")));
    const ScopedEnvironmentVariable path(QByteArrayLiteral("PATH"));

    TerminalSessionLaunchOptions options;
    options.workingDirectory = childDirectory;
    options.program = {
        QStringLiteral("sh"),
        QStringLiteral("-c"),
        QStringLiteral("printf 'default-path-selected\\n'"),
    };
    options.hold = true;
    worker.initialize(options);

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!updateSpy.isEmpty(), 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    const QString contents = frameText(accumulatedFrame(updateSpy));
    QVERIFY2(contents.contains(QStringLiteral("default-path-selected")),
             qPrintable(contents));
    QVERIFY2(!contents.contains(QStringLiteral("selected=child")),
             qPrintable(contents));
    worker.shutdown();
}

void SessionWorkerTest::resolvesCorrelatedHyperlinkQueries()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalHyperlinkState>();
    qRegisterMetaType<TerminalLinkKind>();
    qRegisterMetaType<QVector<QPoint>>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy hyperlinkSpy(&worker, &SessionWorker::hyperlinkResolved);
    QSignalSpy activationSpy(
        &worker, &SessionWorker::hyperlinkActivationResolved);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    const QByteArray uri = QStringLiteral(
        "https://example.test/worker-👻").toUtf8();
    QByteArray output = QByteArrayLiteral("\033]8;id=worker;");
    output += uri;
    output += QByteArrayLiteral("\033\\LINK\033]8;;\033\\ plain\r\n");
    for (int row = 0; row < 40; ++row) {
        output += QByteArrayLiteral("worker-row-");
        output += QByteArray::number(row).rightJustified(2, '0');
        output += QByteArrayLiteral("\r\n");
    }

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.hold = true;
    options.program = {
        QStandardPaths::findExecutable(QStringLiteral("printf")),
        QString::fromUtf8(output),
    };
    QVERIFY(!options.program.constFirst().isEmpty());
    worker.initialize(options);

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!updateSpy.isEmpty(), 5000);
    QVERIFY(errorSpy.isEmpty());

    worker.scrollViewport({.kind = TerminalViewportRequest::Kind::Top});
    QTRY_VERIFY_WITH_TIMEOUT(
        frameText(accumulatedFrame(updateSpy)).contains(QStringLiteral("LINK")),
        1000);
    const TerminalFrame frame = accumulatedFrame(updateSpy);
    QVector<QPoint> candidates;
    for (int index = 0; index < frame.cells.size(); ++index) {
        if (frame.cells.at(index).hasHyperlink) {
            candidates.append(QPoint(index % frame.columns,
                                     index / frame.columns));
        }
    }
    QCOMPARE(candidates.size(), 4);
    QVERIFY(frame.contentRevision != 0);

    worker.queryHyperlink(41, frame.contentRevision,
                          candidates.constFirst().x(),
                          candidates.constFirst().y());
    QTRY_COMPARE_WITH_TIMEOUT(hyperlinkSpy.count(), 1, 1000);
    QCOMPARE(hyperlinkSpy.at(0).at(0).toULongLong(), quint64(41));
    QCOMPARE(hyperlinkSpy.at(0).at(1).toULongLong(), frame.contentRevision);
    QCOMPARE(qvariant_cast<TerminalHyperlinkState>(
                 hyperlinkSpy.at(0).at(2)),
             TerminalHyperlinkState::Visible);
    QCOMPARE(qvariant_cast<TerminalLinkKind>(hyperlinkSpy.at(0).at(3)),
             TerminalLinkKind::Osc8);
    QCOMPARE(hyperlinkSpy.at(0).at(4).toByteArray(), uri);
    QCOMPARE(hyperlinkSpy.at(0).at(5).toPoint(), candidates.constFirst());
    QCOMPARE(qvariant_cast<QVector<QPoint>>(hyperlinkSpy.at(0).at(6)),
             candidates);

    // A tracked hover becomes hidden when its logical target leaves the
    // viewport, then reappears without a new pointer query.
    worker.scrollViewport({.kind = TerminalViewportRequest::Kind::Bottom});
    QTRY_COMPARE_WITH_TIMEOUT(hyperlinkSpy.count(), 2, 1000);
    QCOMPARE(hyperlinkSpy.at(1).at(0).toULongLong(), quint64(41));
    QCOMPARE(qvariant_cast<TerminalHyperlinkState>(
                 hyperlinkSpy.at(1).at(2)),
             TerminalHyperlinkState::Hidden);
    worker.scrollViewport({.kind = TerminalViewportRequest::Kind::Top});
    QTRY_COMPARE_WITH_TIMEOUT(hyperlinkSpy.count(), 3, 1000);
    QCOMPARE(hyperlinkSpy.at(2).at(0).toULongLong(), quint64(41));
    QCOMPARE(qvariant_cast<TerminalHyperlinkState>(
                 hyperlinkSpy.at(2).at(2)),
             TerminalHyperlinkState::Visible);
    QCOMPARE(qvariant_cast<TerminalLinkKind>(hyperlinkSpy.at(2).at(3)),
             TerminalLinkKind::Osc8);
    QCOMPARE(hyperlinkSpy.at(2).at(4).toByteArray(), uri);

    worker.cancelHyperlinkQuery(41);
    const int cancelledCount = hyperlinkSpy.count();
    worker.scrollViewport({.kind = TerminalViewportRequest::Kind::Bottom});
    QTest::qWait(30);
    QCOMPARE(hyperlinkSpy.count(), cancelledCount);
    worker.scrollViewport({.kind = TerminalViewportRequest::Kind::Top});
    QTRY_VERIFY_WITH_TIMEOUT(
        accumulatedFrame(updateSpy).scrollOffset == 0, 1000);
    const TerminalFrame restoredFrame = accumulatedFrame(updateSpy);

    worker.queryHyperlink(42, restoredFrame.contentRevision, 11, 0);
    QTRY_COMPARE_WITH_TIMEOUT(hyperlinkSpy.count(), cancelledCount + 1, 1000);
    const QList<QVariant> invalidPosition = hyperlinkSpy.constLast();
    QCOMPARE(invalidPosition.at(0).toULongLong(), quint64(42));
    QCOMPARE(qvariant_cast<TerminalHyperlinkState>(invalidPosition.at(2)),
             TerminalHyperlinkState::Invalid);
    QCOMPARE(qvariant_cast<TerminalLinkKind>(invalidPosition.at(3)),
             TerminalLinkKind::Osc8);
    QVERIFY(invalidPosition.at(4).toByteArray().isEmpty());

    worker.queryHyperlink(43, restoredFrame.contentRevision - 1,
                          candidates.constFirst().x(),
                          candidates.constFirst().y());
    QTRY_COMPARE_WITH_TIMEOUT(hyperlinkSpy.count(), cancelledCount + 2, 1000);
    const QList<QVariant> staleQuery = hyperlinkSpy.constLast();
    QCOMPARE(staleQuery.at(0).toULongLong(), quint64(43));
    QCOMPARE(staleQuery.at(1).toULongLong(), restoredFrame.contentRevision);
    QCOMPARE(qvariant_cast<TerminalHyperlinkState>(staleQuery.at(2)),
             TerminalHyperlinkState::Stale);

    // Many unresolved pointer events share one zero-delay dispatch. Only the
    // newest request reaches libghostty and produces a reply.
    const int beforeFlood = hyperlinkSpy.count();
    for (quint64 requestId = 100; requestId <= 200; ++requestId) {
        QMetaObject::invokeMethod(
            &worker,
            [&worker, requestId, revision = restoredFrame.contentRevision,
             cell = candidates.constFirst()] {
                worker.queryHyperlink(
                    requestId, revision, cell.x(), cell.y());
            },
            Qt::QueuedConnection);
    }
    QTRY_COMPARE_WITH_TIMEOUT(hyperlinkSpy.count(), beforeFlood + 1, 1000);
    QCOMPARE(hyperlinkSpy.constLast().at(0).toULongLong(), quint64(200));
    QCOMPARE(qvariant_cast<TerminalHyperlinkState>(
                 hyperlinkSpy.constLast().at(2)),
             TerminalHyperlinkState::Visible);

    worker.queryHyperlink(
        201, restoredFrame.contentRevision,
        candidates.constFirst().x(), candidates.constFirst().y());
    worker.cancelHyperlinkQuery(201);
    QTest::qWait(30);
    QCOMPARE(hyperlinkSpy.count(), beforeFlood + 1);

    // Press validation is a separate tracked lane, so hover replacement and
    // cancellation cannot consume or retarget an activation.
    worker.prepareHyperlinkActivation(
        301, restoredFrame.contentRevision,
        candidates.constFirst().x(), candidates.constFirst().y());
    worker.queryHyperlink(302, restoredFrame.contentRevision, 11, 0);
    worker.commitHyperlinkActivation(
        301, candidates.constFirst().x(), candidates.constFirst().y());
    QCOMPARE(activationSpy.count(), 1);
    QCOMPARE(activationSpy.constFirst().at(0).toULongLong(), quint64(301));
    QCOMPARE(qvariant_cast<TerminalLinkKind>(
                 activationSpy.constFirst().at(2)),
             TerminalLinkKind::Osc8);
    QCOMPARE(activationSpy.constFirst().at(3).toByteArray(), uri);

    // A valid raw action follows the viewport back to the live screen even
    // when its decoded payload is empty. That viewport mutation advances the
    // revision, so a query using the old scrollback coordinates is rejected.
    worker.sendRawText(QByteArray{});
    QTRY_VERIFY_WITH_TIMEOUT(
        accumulatedFrame(updateSpy).contentRevision
            > restoredFrame.contentRevision,
        1000);
    const TerminalFrame liveFrame = accumulatedFrame(updateSpy);
    QVERIFY(liveFrame.scrollOffset > 0);
    worker.queryHyperlink(44, restoredFrame.contentRevision,
                          candidates.constFirst().x(),
                          candidates.constFirst().y());
    QTRY_VERIFY_WITH_TIMEOUT(
        !hyperlinkSpy.isEmpty()
            && hyperlinkSpy.constLast().at(0).toULongLong() == quint64(44),
        1000);
    QCOMPARE(hyperlinkSpy.constLast().at(1).toULongLong(),
             liveFrame.contentRevision);
    QCOMPARE(qvariant_cast<TerminalHyperlinkState>(
                 hyperlinkSpy.constLast().at(2)),
             TerminalHyperlinkState::Stale);

    worker.shutdown();
}

void SessionWorkerTest::resolvesRegexLinksAcrossUtf8WrapsAndOsc8Precedence()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalHyperlinkState>();
    qRegisterMetaType<TerminalLinkKind>();
    qRegisterMetaType<QVector<QPoint>>();

    SessionWorker worker;
    worker.resizeTerminal(16, 8, 8, 16, 128, 128);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy hyperlinkSpy(&worker, &SessionWorker::hyperlinkResolved);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    const QByteArray regexUri = QByteArrayLiteral(
        "https://example.test/wrapped");
    const QByteArray oscUri = QByteArrayLiteral(
        "https://example.test/osc-target");
    QByteArray output = QStringLiteral("e\u0301界 ").toUtf8();
    output += regexUri;
    output += QByteArrayLiteral("\r\n\033]8;;");
    output += oscUri;
    output += QByteArrayLiteral(
        "\033\\https://visible.test/regex-text\033]8;;\033\\");

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStandardPaths::findExecutable(QStringLiteral("printf")),
        QString::fromUtf8(output),
    };
    QVERIFY(!options.program.constFirst().isEmpty());
    options.hold = true;
    options.runtime.linkUrl = true;
    worker.initialize(options);

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!updateSpy.isEmpty(), 5000);
    QVERIFY(errorSpy.isEmpty());
    TerminalFrame frame = accumulatedFrame(updateSpy);
    QCOMPARE(frame.columns, 16);
    QCOMPARE(frame.cells.at(4).text, QStringLiteral("h"));

    // The regex byte offset follows a combining grapheme plus a wide cell,
    // then the matched cells continue across the terminal's soft wrap.
    worker.queryHyperlink(401, frame.contentRevision, 4, 0);
    QTRY_COMPARE_WITH_TIMEOUT(hyperlinkSpy.count(), 1, 1000);
    const QList<QVariant> wrapped = hyperlinkSpy.constLast();
    QCOMPARE(qvariant_cast<TerminalHyperlinkState>(wrapped.at(2)),
             TerminalHyperlinkState::Visible);
    QCOMPARE(qvariant_cast<TerminalLinkKind>(wrapped.at(3)),
             TerminalLinkKind::Regex);
    QCOMPARE(wrapped.at(4).toByteArray(), regexUri);
    const QVector<QPoint> wrappedCells =
        qvariant_cast<QVector<QPoint>>(wrapped.at(6));
    QVERIFY(wrappedCells.contains(QPoint(4, 0)));
    QVERIFY(std::any_of(
        wrappedCells.cbegin(), wrappedCells.cend(),
        [](const QPoint &cell) { return cell.y() > 0; }));

    // Both endpoints are logical anchors, so reflow changes the decoration
    // coordinates without losing the exact matched bytes.
    const int beforeReflow = hyperlinkSpy.count();
    const quint64 beforeReflowRevision = frame.contentRevision;
    worker.resizeTerminal(24, 8, 8, 16, 192, 128);
    QTRY_VERIFY_WITH_TIMEOUT(
        (frame = accumulatedFrame(updateSpy)).columns == 24
            && frame.contentRevision > beforeReflowRevision,
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(hyperlinkSpy.count() > beforeReflow, 1000);
    const QList<QVariant> reflowed = hyperlinkSpy.constLast();
    QCOMPARE(qvariant_cast<TerminalHyperlinkState>(reflowed.at(2)),
             TerminalHyperlinkState::Visible);
    QCOMPARE(qvariant_cast<TerminalLinkKind>(reflowed.at(3)),
             TerminalLinkKind::Regex);
    QCOMPARE(reflowed.at(4).toByteArray(), regexUri);
    QVERIFY(qvariant_cast<QVector<QPoint>>(reflowed.at(6)) != wrappedCells);
    worker.cancelHyperlinkQuery(401);

    // Disabling link-url suppresses regex candidates but leaves explicit OSC
    // 8 destinations independently queryable. An OSC 8 label that itself
    // looks like a URL must resolve to the explicit destination.
    TerminalSessionRuntimeOptions disabled = options.runtime;
    disabled.linkUrl = false;
    worker.applyRuntimeOptions(disabled);
    frame = accumulatedFrame(updateSpy);
    const int beforeDisabledQuery = hyperlinkSpy.count();
    worker.queryHyperlink(402, frame.contentRevision, 4, 0);
    QTRY_COMPARE_WITH_TIMEOUT(
        hyperlinkSpy.count(), beforeDisabledQuery + 1, 1000);
    QCOMPARE(qvariant_cast<TerminalHyperlinkState>(
                 hyperlinkSpy.constLast().at(2)),
             TerminalHyperlinkState::Invalid);

    QPoint oscCell(-1, -1);
    for (int index = 0; index < frame.cells.size(); ++index) {
        if (frame.cells.at(index).hasHyperlink) {
            oscCell = QPoint(index % frame.columns, index / frame.columns);
            break;
        }
    }
    QVERIFY(oscCell.x() >= 0);
    worker.queryHyperlink(
        403, frame.contentRevision, oscCell.x(), oscCell.y());
    QTRY_COMPARE_WITH_TIMEOUT(
        hyperlinkSpy.count(), beforeDisabledQuery + 2, 1000);
    const QList<QVariant> explicitLink = hyperlinkSpy.constLast();
    QCOMPARE(qvariant_cast<TerminalHyperlinkState>(explicitLink.at(2)),
             TerminalHyperlinkState::Visible);
    QCOMPARE(qvariant_cast<TerminalLinkKind>(explicitLink.at(3)),
             TerminalLinkKind::Osc8);
    QCOMPARE(explicitLink.at(4).toByteArray(), oscUri);

    worker.shutdown();
}

void SessionWorkerTest::retainsRegexHoverAcrossViewportScrolling()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalHyperlinkState>();
    qRegisterMetaType<TerminalLinkKind>();
    qRegisterMetaType<QVector<QPoint>>();

    SessionWorker worker;
    worker.resizeTerminal(40, 8, 8, 16, 320, 128);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy hyperlinkSpy(&worker, &SessionWorker::hyperlinkResolved);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    const QByteArray uri = QByteArrayLiteral(
        "https://example.test/regex-scroll");
    QByteArray output = uri + QByteArrayLiteral("\r\n");
    for (int row = 0; row < 40; ++row) {
        output += QByteArrayLiteral("regex-scroll-row-");
        output += QByteArray::number(row).rightJustified(2, '0');
        output += QByteArrayLiteral("\r\n");
    }

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStandardPaths::findExecutable(QStringLiteral("printf")),
        QString::fromUtf8(output),
    };
    QVERIFY(!options.program.constFirst().isEmpty());
    options.hold = true;
    options.runtime.linkUrl = true;
    worker.initialize(options);

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!updateSpy.isEmpty(), 5000);
    QVERIFY(errorSpy.isEmpty());

    worker.scrollViewport({.kind = TerminalViewportRequest::Kind::Top});
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QString::fromUtf8(uri)), 1000);
    TerminalFrame frame = accumulatedFrame(updateSpy);
    QCOMPARE(frame.scrollOffset, 0);

    constexpr quint64 requestId = 451;
    worker.queryHyperlink(requestId, frame.contentRevision, 0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(hyperlinkSpy.count(), 1, 1000);
    const QList<QVariant> initial = hyperlinkSpy.constLast();
    QCOMPARE(initial.at(0).toULongLong(), requestId);
    QCOMPARE(qvariant_cast<TerminalHyperlinkState>(initial.at(2)),
             TerminalHyperlinkState::Visible);
    QCOMPARE(qvariant_cast<TerminalLinkKind>(initial.at(3)),
             TerminalLinkKind::Regex);
    QCOMPARE(initial.at(4).toByteArray(), uri);

    // The text-range lease, like an OSC 8 lease, survives leaving the
    // viewport. No second query is issued: refreshes retain the original
    // request ID and exact matched bytes while reporting Hidden then Visible.
    worker.scrollViewport({.kind = TerminalViewportRequest::Kind::Bottom});
    QTRY_COMPARE_WITH_TIMEOUT(hyperlinkSpy.count(), 2, 1000);
    const QList<QVariant> hidden = hyperlinkSpy.constLast();
    QCOMPARE(hidden.at(0).toULongLong(), requestId);
    QCOMPARE(qvariant_cast<TerminalHyperlinkState>(hidden.at(2)),
             TerminalHyperlinkState::Hidden);
    QCOMPARE(qvariant_cast<TerminalLinkKind>(hidden.at(3)),
             TerminalLinkKind::Regex);
    QCOMPARE(hidden.at(4).toByteArray(), uri);

    worker.scrollViewport({.kind = TerminalViewportRequest::Kind::Top});
    QTRY_COMPARE_WITH_TIMEOUT(hyperlinkSpy.count(), 3, 1000);
    const QList<QVariant> restored = hyperlinkSpy.constLast();
    QCOMPARE(restored.at(0).toULongLong(), requestId);
    QCOMPARE(qvariant_cast<TerminalHyperlinkState>(restored.at(2)),
             TerminalHyperlinkState::Visible);
    QCOMPARE(qvariant_cast<TerminalLinkKind>(restored.at(3)),
             TerminalLinkKind::Regex);
    QCOMPARE(restored.at(4).toByteArray(), uri);

    worker.shutdown();
}

void SessionWorkerTest::revalidatesRegexActivationAcrossUnrelatedOutput()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalHyperlinkState>();
    qRegisterMetaType<TerminalLinkKind>();

    SessionWorker worker;
    worker.resizeTerminal(40, 4, 8, 16, 320, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy hyperlinkSpy(&worker, &SessionWorker::hyperlinkResolved);
    QSignalSpy activationSpy(
        &worker, &SessionWorker::hyperlinkActivationResolved);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    const QByteArray original = QByteArrayLiteral(
        "https://example.test/live");
    const QByteArray replacement = QByteArrayLiteral(
        "https://example.test/gone");
    const QString script = QStringLiteral(
        "printf '\033[2J\033[Hhttps://example.test/live'; "
        "sleep 0.5; "
        "i=0; while [ $i -lt 80 ]; do "
        "printf '\0337\033[4;1Htick-%02d\0338' \"$i\"; "
        "i=$((i + 1)); sleep 0.02; done; "
        "sleep 0.3; "
        "printf '\0337\033[1;1Hhttps://example.test/gone\0338'");

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"), QStringLiteral("-c"), script,
    };
    options.hold = true;
    options.runtime.linkUrl = true;
    worker.initialize(options);

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QString::fromUtf8(original)), 5000);
    TerminalFrame frame = accumulatedFrame(updateSpy);
    worker.queryHyperlink(501, frame.contentRevision, 0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(hyperlinkSpy.count(), 1, 1000);
    QCOMPARE(qvariant_cast<TerminalHyperlinkState>(
                 hyperlinkSpy.constLast().at(2)),
             TerminalHyperlinkState::Visible);
    QCOMPARE(qvariant_cast<TerminalLinkKind>(hyperlinkSpy.constLast().at(3)),
             TerminalLinkKind::Regex);
    QCOMPARE(hyperlinkSpy.constLast().at(4).toByteArray(), original);

    const int stableResolutionCount = hyperlinkSpy.count();
    const quint64 stableRevision = frame.contentRevision;
    QTRY_VERIFY_WITH_TIMEOUT(
        (frame = accumulatedFrame(updateSpy)).contentRevision
            >= stableRevision + 3,
        1500);
    QTest::qWait(50);
    QCOMPARE(hyperlinkSpy.count(), stableResolutionCount);

    // A press owns a distinct text-range lease. Unrelated output can advance
    // the broad frame revision, but replacing the covered bytes must make the
    // eventual release fail closed rather than open the new text.
    worker.prepareHyperlinkActivation(
        502, frame.contentRevision, 0, 0);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QString::fromUtf8(replacement)), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !hyperlinkSpy.isEmpty()
            && qvariant_cast<TerminalHyperlinkState>(
                   hyperlinkSpy.constLast().at(2))
                == TerminalHyperlinkState::Invalid,
        1000);
    worker.commitHyperlinkActivation(502, 0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(activationSpy.count(), 1, 1000);
    QCOMPARE(qvariant_cast<TerminalLinkKind>(
                 activationSpy.constLast().at(2)),
             TerminalLinkKind::Regex);
    QVERIFY(activationSpy.constLast().at(3).toByteArray().isEmpty());

    frame = accumulatedFrame(updateSpy);
    worker.queryHyperlink(503, frame.contentRevision, 0, 0);
    QTRY_VERIFY_WITH_TIMEOUT(
        !hyperlinkSpy.isEmpty()
            && hyperlinkSpy.constLast().at(0).toULongLong() == quint64(503),
        1000);
    QCOMPARE(qvariant_cast<TerminalHyperlinkState>(
                 hyperlinkSpy.constLast().at(2)),
             TerminalHyperlinkState::Visible);
    QCOMPARE(qvariant_cast<TerminalLinkKind>(hyperlinkSpy.constLast().at(3)),
             TerminalLinkKind::Regex);
    QCOMPARE(hyperlinkSpy.constLast().at(4).toByteArray(), replacement);
    QVERIFY(errorSpy.isEmpty());

    worker.shutdown();
}

void SessionWorkerTest::drainsLargeFinalOutputBeforeClosingPty()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "yes 0123456789abcdef | head -c 1500000; "
            "printf '\\nlarge-output-final\\n'"),
    };
    options.hold = true;
    worker.initialize(options);

    QTRY_VERIFY_WITH_TIMEOUT(exitSpy.count() > 0, 8000);
    const QString finalContents = frameText(accumulatedFrame(updateSpy));
    QVERIFY2(finalContents.contains(QStringLiteral("large-output-final")),
             qPrintable(finalContents));
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::drainsLargeQueuedInputAfterPtyBackpressure()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    constexpr qsizetype payloadSize = 512 * 1024;
    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {
        QStringLiteral("/bin/sh"), QStringLiteral("-c"),
        QStringLiteral("stty raw -echo; "
                       "printf 'large-input-ready\\r\\n'; "
                       "sleep 0.2; "
                       "count=$(head -c 524288 | wc -c); "
                       "stty sane; "
                       "printf '\\r\\nlarge-input-bytes:%s\\r\\n' \"$count\"")};
    options.hold = true;
    QVERIFY(worker.initialize(options));

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("large-input-ready")), 5000);
    worker.paste(QString(payloadSize, u'x'));

    QTRY_VERIFY_WITH_TIMEOUT(exitSpy.count() > 0, 8000);
    const QString finalContents = frameText(accumulatedFrame(updateSpy));
    QVERIFY2(finalContents.contains(QStringLiteral("large-input-bytes:524288")),
             qPrintable(finalContents));
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::routesTerminalClipboardWritesUsingLivePolicy()
{
    qRegisterMetaType<TerminalClipboardWriteRequest>();

    SessionWorker worker;
    QSignalSpy updates(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy writes(&worker, &SessionWorker::terminalClipboardWriteRequested);
    QSignalSpy started(&worker, &SessionWorker::started);
    QSignalSpy errors(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("IFS= read -r line; "
                       "printf '\\033]52;c;YXNr\\007ask-done\\n'; "
                       "IFS= read -r line; "
                       "printf '\\033]52;c;ZGVueQ==\\007deny-done\\n'; "
                       "IFS= read -r line; "
                       "printf '\\033]52;p;YWxsb3c=\\007allow-done\\n'; "
                       "exec /bin/sleep 30"),
    };
    options.runtime.clipboardWrite = TerminalClipboardAccess::Ask;
    QVERIFY(worker.initialize(options));
    QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 1000);

    const TerminalKeyInput enter{
        .key = Qt::Key_Return,
        .text = QStringLiteral("\r"),
        .pressed = true,
    };
    worker.sendKey(enter);
    QTRY_COMPARE_WITH_TIMEOUT(writes.count(), 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("ask-done")), 3000);
    TerminalClipboardWriteRequest request =
        qvariant_cast<TerminalClipboardWriteRequest>(
            writes.constFirst().constFirst());
    QVERIFY(request.confirmationRequired);
    QCOMPARE(request.write.location, TerminalClipboardLocation::Standard);
    QCOMPARE(request.write.contents.constFirst().data,
             QByteArrayLiteral("ask"));

    TerminalSessionRuntimeOptions runtime = options.runtime;
    runtime.clipboardWrite = TerminalClipboardAccess::Deny;
    worker.applyRuntimeOptions(runtime);
    worker.sendKey(enter);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("deny-done")), 3000);
    QCOMPARE(writes.count(), 1);

    runtime.clipboardWrite = TerminalClipboardAccess::Allow;
    worker.applyRuntimeOptions(runtime);
    worker.sendKey(enter);
    QTRY_COMPARE_WITH_TIMEOUT(writes.count(), 2, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("allow-done")), 3000);
    request = qvariant_cast<TerminalClipboardWriteRequest>(
        writes.constLast().constFirst());
    QVERIFY(!request.confirmationRequired);
    QCOMPARE(request.write.location, TerminalClipboardLocation::Primary);
    QCOMPARE(request.write.contents.constFirst().data,
             QByteArrayLiteral("allow"));

    QVERIFY2(errors.isEmpty(),
             errors.isEmpty()
                 ? ""
                 : qPrintable(errors.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::sendsBracketedPasteThroughPty()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "stty raw -echo; "
            "printf '\\033[?2004hpaste-ready'; "
            "payload=$(dd bs=1 count=19 2>/dev/null); "
            "stty sane; "
            "printf '\\033[?2004lpaste-bytes:'; "
            "printf '%s' \"$payload\" | od -An -tx1 | tr -d ' \\n'; "
            "printf '\\n'")};
    options.hold = true;
    worker.initialize(options);

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("paste-ready")), 5000);
    worker.paste(QStringLiteral("one\ntwo"));

    QTRY_VERIFY_WITH_TIMEOUT(exitSpy.count() > 0, 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty() ? "" : qPrintable(errorSpy.constFirst().constFirst().toString()));
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    const TerminalFrame finalFrame = accumulatedFrame(updateSpy);
    const QString finalContents = frameText(finalFrame);
    QVERIFY2(finalContents.contains(QStringLiteral(
                 "paste-bytes:1b5b3230307e6f6e650a74776f1b5b3230317e")),
             qPrintable(finalContents));
    worker.shutdown();
}

void SessionWorkerTest::protectsPasteWithCorrelatedWorkerConfirmation()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    worker.resizeTerminal(32, 4, 8, 16, 256, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy unsafeSpy(
        &worker, &SessionWorker::unsafePasteConfirmationRequested);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir controlDirectory(
        QDir::current().filePath(QStringLiteral("tmp/paste-safety-XXXXXX")));
    QVERIFY(controlDirectory.isValid());
    const QString modeMarker =
        QDir(controlDirectory.path()).filePath(QStringLiteral("enable-mode"));

    TerminalSessionLaunchOptions options;
    options.workingDirectory = controlDirectory.path();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "stty raw -echo; "
            "i=0; while [ $i -lt 24 ]; do "
            "printf 'paste-history-%02d\\r\\n' \"$i\"; i=$((i + 1)); done; "
            "printf 'paste-worker-ready'; "
            "payload=$(dd bs=1 count=4 2>/dev/null); "
            "printf '\\r\\npaste-first:'; "
            "printf '%s' \"$payload\" | od -An -v -tx1 | tr -d ' \\n'; "
            "while [ ! -e \"$1\" ]; do sleep 0.01; done; "
            "printf '\\033[?2004hmode-ready'; "
            "payload=$(dd bs=1 count=19 2>/dev/null); "
            "printf '\\033[?2004l\\r\\npaste-second:'; "
            "printf '%s' \"$payload\" | od -An -v -tx1 | tr -d ' \\n'; "
            "payload=$(dd bs=1 count=7 2>/dev/null); "
            "printf '\\r\\npaste-third:'; "
            "printf '%s' \"$payload\" | od -An -v -tx1 | tr -d ' \\n'; "
            "payload=$(dd bs=1 count=4 2>/dev/null); "
            "printf '\\r\\npaste-fourth:'; "
            "printf '%s' \"$payload\" | od -An -v -tx1 | tr -d ' \\n'; "
            "printf '\\r\\n'"),
        QStringLiteral("paste-safety-test"),
        modeMarker,
    };
    options.hold = true;
    worker.initialize(options);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("paste-worker-ready")), 5000);

    worker.scrollViewport({.kind = TerminalViewportRequest::Kind::Top});
    QTRY_COMPARE_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset,
                              quint64{0}, 1000);

    const QString unsafeText = QStringLiteral("one\ntwo");
    worker.paste(unsafeText);
    QCOMPARE(unsafeSpy.count(), 1);
    const quint64 cancelledId = unsafeSpy.constFirst().at(0).toULongLong();
    QVERIFY(cancelledId != 0);
    QCOMPARE(unsafeSpy.constFirst().at(1).toString(), unsafeText);
    const QString independentlyPending = QStringLiteral("red\nblue");
    worker.paste(independentlyPending);
    QCOMPARE(unsafeSpy.count(), 2);
    const quint64 independentlyPendingId =
        unsafeSpy.constLast().at(0).toULongLong();
    QVERIFY(independentlyPendingId != 0);
    QVERIFY(independentlyPendingId != cancelledId);
    QCOMPARE(unsafeSpy.constLast().at(1).toString(), independentlyPending);
    QTest::qWait(100);
    QVERIFY(!updatesContain(updateSpy, QStringLiteral("paste-first:")));
    QCOMPARE(accumulatedFrame(updateSpy).scrollOffset, quint64{0});

    worker.cancelPaste(cancelledId);
    worker.confirmPaste(cancelledId);
    worker.cancelPaste(independentlyPendingId);
    worker.confirmPaste(independentlyPendingId);
    worker.paste(QStringLiteral("SAFE"));
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("paste-first:53414645")),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset > 0,
                             1000);

    worker.scrollViewport({.kind = TerminalViewportRequest::Kind::Top});
    QTRY_COMPARE_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset,
                              quint64{0}, 1000);
    worker.paste(unsafeText);
    QCOMPARE(unsafeSpy.count(), 3);
    const quint64 confirmedId = unsafeSpy.constLast().at(0).toULongLong();
    QVERIFY(confirmedId != 0);
    QVERIFY(confirmedId != cancelledId);

    QFile marker(modeMarker);
    QVERIFY(marker.open(QIODevice::WriteOnly));
    marker.close();
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        worker.scrollViewport({
            .kind = TerminalViewportRequest::Kind::Bottom,
        });
        return updatesContain(updateSpy, QStringLiteral("mode-ready"));
    })(), 5000);
    worker.confirmPaste(confirmedId);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(
            updateSpy,
            QStringLiteral(
                "paste-second:1b5b3230307e6f6e650a74776f1b5b3230317e")),
        5000);

    // The ID was consumed exactly once. A duplicate cannot feed the child's
    // next fixed-size read or authorize another payload.
    worker.confirmPaste(confirmedId);
    options.runtime.clipboardPaste.protection = false;
    worker.applyRuntimeOptions(options.runtime);
    worker.paste(unsafeText);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy,
                       QStringLiteral("paste-third:6f6e650d74776f")),
        5000);
    QCOMPARE(unsafeSpy.count(), 3);

    worker.paste(QStringLiteral("DONE"));
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("paste-fourth:444f4e45")),
        5000);
    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::sendsTerminalControlActionsThroughPty()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "stty raw -echo; "
            "i=0; while [ $i -lt 40 ]; do "
            "printf 'control-row-%03d\\r\\n' \"$i\"; i=$((i + 1)); done; "
            "printf 'control-ready\\r\\n'; "
            "printf 'control-bytes:'; "
            "dd bs=1 count=36 2>/dev/null | od -An -v -tx1 | tr -d ' \\n'; "
            "printf '\\r\\ncontrol-done\\r\\n'")};
    options.hold = true;
    worker.initialize(options);

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("control-ready")), 5000);
    worker.scrollViewport({.kind = TerminalViewportRequest::Kind::Top});
    QTRY_COMPARE_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset,
                              quint64{0}, 1000);

    // The malformed text action is performed but contributes no bytes and
    // does not disturb the exact ordering of the valid actions that follow.
    worker.sendRawText(QByteArrayLiteral(R"(\\q)"));
    QCOMPARE(accumulatedFrame(updateSpy).scrollOffset, quint64{0});
    worker.sendCsi(QByteArrayLiteral("31m"));
    worker.sendEscape(QByteArrayLiteral("7"));
    worker.sendCsi(QByteArray{});
    worker.sendEscape(QByteArray{});
    worker.sendCsi(QByteArrayLiteral(R"(\xc3\xa9)"));
    // ESC payloads only undo Action.format; they do not apply text's second
    // config decode. This therefore sends the literal four-byte string \x7f.
    worker.sendEscape(QByteArrayLiteral(R"(\\x7f)"));
    worker.sendCsi(QByteArrayLiteral(R"(\x00)"));

    // These are canonical Binding.Action.format payloads. The first decode
    // restores the action's original bytes; text then applies Ghostty's
    // config string decoder, including NUL and Unicode escapes.
    worker.sendRawText(QByteArrayLiteral(R"(A\\n\\x00\\u{1F601}B)"));
    worker.sendRawText(QByteArrayLiteral(R"(\xf0\x9f\x91\xbb)"));
    worker.sendRawText(QByteArrayLiteral(R"(\\x80)"));

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(
            updateSpy,
            QStringLiteral(
                "control-bytes:1b5b33316d1b371b5b1b1b5bc3a9"
                "1b5c7837661b5b00410a00"
                "f09f988142f09f91bbc280")),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        accumulatedFrame(updateSpy).scrollOffset > 0, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(exitSpy.count() > 0, 5000);

    // Ghostty scrolls for a valid empty text action even though it writes no
    // bytes. This remains useful after the held child has exited.
    worker.scrollViewport({.kind = TerminalViewportRequest::Kind::Top});
    QTRY_COMPARE_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset,
                              quint64{0}, 1000);
    worker.sendRawText(QByteArray{});
    QTRY_VERIFY_WITH_TIMEOUT(
        accumulatedFrame(updateSpy).scrollOffset > 0, 1000);

    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty() ? ""
                                : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::pastesTerminalFilePathAsRawOrderedInput()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalActionResult>();
    QVERIFY(QDir().mkpath(
        QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir controlDirectory(
        QDir::current().filePath(
            QStringLiteral("tmp/write-file-paste-XXXXXX")));
    QVERIFY(controlDirectory.isValid());
    const QString artifactRoot =
        QDir(controlDirectory.path()).filePath(QStringLiteral("artifacts"));
    QVERIFY(QDir().mkpath(artifactRoot));
    const ScopedEnvironmentVariable temporaryDirectory(
        QByteArrayLiteral("TMPDIR"), QFile::encodeName(artifactRoot));
    const QString canonicalArtifactRoot =
        QFileInfo(artifactRoot).canonicalFilePath();
    QCOMPARE(QFileInfo(QDir::tempPath()).canonicalFilePath(),
             canonicalArtifactRoot);

    const QString representativePath =
        QDir(canonicalArtifactRoot).filePath(
            QStringLiteral("ghostty-qt-XXXXXX/screen.txt"));
    const qsizetype pathSize = QFile::encodeName(representativePath).size();
    const qsizetype orderedPayloadSize =
        QByteArrayLiteral("BEFORE").size() + pathSize
        + QByteArrayLiteral("AFTER").size();

    SessionWorker worker;
    worker.resizeTerminal(80, 8, 8, 16, 640, 128);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy actionSpy(
        &worker, &SessionWorker::terminalActionFinished);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = controlDirectory.path();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "stty raw -echo; "
            "printf '\\033[?2004hwrite-file-ready'; "
            "payload=$(dd bs=1 count=\"$1\" 2>/dev/null); "
            "printf '\\033[?2004l\\r\\nwrite-file-bytes:'; "
            "printf '%s' \"$payload\" | od -An -v -tx1 | tr -d ' \\n'; "
            "printf '\\r\\nreadonly-ready'; "
            "payload=$(dd bs=1 count=1 2>/dev/null); "
            "printf '\\r\\nreadonly-byte:'; "
            "printf '%s' \"$payload\" | od -An -v -tx1 | tr -d ' \\n'; "
            "printf '\\r\\n'"),
        QStringLiteral("write-file-paste-test"),
        QString::number(orderedPayloadSize),
    };
    options.hold = true;
    QVERIFY(worker.initialize(options));
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("write-file-ready")),
        5000);

    worker.sendRawText(QByteArrayLiteral("BEFORE"));
    worker.writeTerminalFile(707, {
        .location = TerminalFileLocation::Screen,
        .disposition = TerminalFileDisposition::Paste,
    });
    QCOMPARE(actionSpy.count(), 1);
    const TerminalActionResult pasteResult =
        terminalActionResultAt(actionSpy, 0);
    QCOMPARE(pasteResult.requestId, quint64{707});
    QCOMPARE(pasteResult.outcome, TerminalActionOutcome::Success);
    QCOMPARE(pasteResult.effect, TerminalActionEffect::None);
    QVERIFY(pasteResult.performed);
    QCOMPARE(pasteResult.clipboardDestination,
             TerminalClipboardDestination::Standard);
    QStringList artifactDirectories =
        QDir(artifactRoot).entryList(
            QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(artifactDirectories.size(), 1);
    const QString firstPath =
        QDir(QDir(canonicalArtifactRoot)
                 .filePath(artifactDirectories.constFirst()))
            .filePath(QStringLiteral("screen.txt"));
    QVERIFY(QFileInfo::exists(firstPath));
    QCOMPARE(QFile::encodeName(firstPath).size(), pathSize);
    QCOMPARE(pasteResult.payload, firstPath);
    worker.sendRawText(QByteArrayLiteral("AFTER"));

    const QByteArray expectedOrderedBytes =
        QByteArrayLiteral("BEFORE") + QFile::encodeName(firstPath)
        + QByteArrayLiteral("AFTER");
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(
            updateSpy,
            QStringLiteral("write-file-bytes:")
                + QString::fromLatin1(expectedOrderedBytes.toHex())),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("readonly-ready")),
        1000);

    worker.setReadOnly(true);
    worker.writeTerminalFile(808, {
        .location = TerminalFileLocation::Screen,
        .disposition = TerminalFileDisposition::Paste,
    });
    QCOMPARE(actionSpy.count(), 2);
    const TerminalActionResult readOnlyResult =
        terminalActionResultAt(actionSpy, 1);
    QCOMPARE(readOnlyResult.requestId, quint64{808});
    QCOMPARE(readOnlyResult.outcome, TerminalActionOutcome::Success);
    QCOMPARE(readOnlyResult.effect, TerminalActionEffect::None);
    QVERIFY(readOnlyResult.performed);
    QCOMPARE(readOnlyResult.clipboardDestination,
             TerminalClipboardDestination::Standard);
    artifactDirectories =
        QDir(artifactRoot).entryList(
            QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(artifactDirectories.size(), 2);
    QVERIFY(QFileInfo::exists(readOnlyResult.payload));
    QVERIFY(readOnlyResult.payload != pasteResult.payload);
    QTest::qWait(100);
    QVERIFY(!updatesContain(
        updateSpy, QStringLiteral("readonly-byte:")));

    // The rejected path is never replayed after the policy changes.
    worker.setReadOnly(false);
    worker.sendRawText(QByteArrayLiteral("Z"));
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("readonly-byte:5a")),
        5000);
    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(
                       errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();

    for (const QString &directoryName :
         std::as_const(artifactDirectories)) {
        const QString directoryPath =
            QDir(artifactRoot).filePath(directoryName);
        const QString artifactPath =
            QDir(directoryPath).filePath(QStringLiteral("screen.txt"));
        QVERIFY(QFile::remove(artifactPath));
        QVERIFY(QDir().rmdir(directoryPath));
    }
    QVERIFY(QDir(artifactRoot)
                .entryList(QDir::Dirs | QDir::NoDotAndDotDot)
                .isEmpty());
}

void SessionWorkerTest::readOnlyBlocksSurfaceInputButPreservesProtocolReplies()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy mouseSpy(&worker, &SessionWorker::mouseTrackingChanged);
    QSignalSpy unsafeSpy(
        &worker, &SessionWorker::unsafePasteConfirmationRequested);
    QSignalSpy startedSpy(&worker, &SessionWorker::started);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir controlDirectory(
        QDir::current().filePath(QStringLiteral("tmp/readonly-pty-XXXXXX")));
    QVERIFY(controlDirectory.isValid());
    const QString startMarker =
        QDir(controlDirectory.path()).filePath(QStringLiteral("start"));
    const QString readyMarker =
        QDir(controlDirectory.path()).filePath(QStringLiteral("ready"));

    TerminalSessionLaunchOptions options;
    options.workingDirectory = controlDirectory.path();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "stty raw -echo; "
            "printf '\\033[?1000h\\033[?1004hreadonly-ready'; "
            ": > \"$2\"; "
            "while [ ! -e \"$1\" ]; do sleep 0.01; done; "
            "printf '\\033[H\\033[6n'; "
            "payload=$(dd bs=1 count=6 2>/dev/null); "
            "printf '\\r\\ncpr-bytes:'; "
            "printf '%s' \"$payload\" | od -An -v -tx1 | tr -d ' \\n'; "
            "printf '\\r\\nfocus-ready'; "
            "payload=$(dd bs=1 count=6 2>/dev/null); "
            "printf '\\r\\nfocus-bytes:'; "
            "printf '%s' \"$payload\" | od -An -v -tx1 | tr -d ' \\n'; "
            "printf '\\r\\ninput-ready'; "
            "payload=$(dd bs=1 count=1 2>/dev/null); "
            "printf '\\r\\ninput-byte:'; "
            "printf '%s' \"$payload\" | od -An -v -tx1 | tr -d ' \\n'; "
            "printf '\\r\\nreadonly-done\\r\\n'"),
        QStringLiteral("readonly-pty-test"),
        startMarker,
        readyMarker,
    };
    options.hold = true;
    options.runtime.clipboardPaste.bracketedSafe = false;
    worker.initialize(options);

    QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(readyMarker), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(spyContainsBool(mouseSpy, true), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("readonly-ready")), 1000);

    worker.setReadOnly(true);
    QFile marker(startMarker);
    QVERIFY(marker.open(QIODevice::WriteOnly));
    marker.close();

    // Device-status replies are emitted by the terminal stream itself rather
    // than by a surface input action. Pinned Ghostty lets them cross the PTY
    // boundary in read-only mode so applications cannot deadlock on a query.
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy,
                       QStringLiteral("cpr-bytes:1b5b313b3152")),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("focus-ready")), 1000);

    // Focus is likewise a non-write Surface message upstream. Termio may turn
    // it into focus-report bytes, but read-only mode must not suppress them.
    worker.setFocused(false);
    worker.setFocused(true);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy,
                       QStringLiteral("focus-bytes:1b5b4f1b5b49")),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("input-ready")), 1000);

    TerminalKeyInput enter;
    enter.key = Qt::Key_Return;
    enter.text = QStringLiteral("\r");
    enter.pressed = true;
    worker.sendKey(enter);

    const auto letter = [](QChar character) {
        TerminalKeyInput input;
        input.key = character.toUpper().unicode();
        input.text = QString(character);
        input.unshiftedCodepoint = character.toLower().unicode();
        input.pressed = true;
        return input;
    };
    worker.stageSequenceKey(1, letter(u's'));
    worker.resolveSequence(1,
                           TerminalSequenceResolution::FlushAndSendCurrent,
                           true, letter(u'q'));
    worker.sendInputMethod({.commitText = QStringLiteral("blocked-ime")});
    worker.sendCsi(QByteArrayLiteral("31m"));
    worker.sendEscape(QByteArrayLiteral("7"));
    worker.sendRawText(QByteArrayLiteral(R"(blocked-raw\\n)"));
    worker.sendMouse({
        .action = TerminalMouseInput::Press,
        .button = 1,
        .x = 8.0F,
        .y = 8.0F,
    });
    worker.paste(QStringLiteral("blocked-safe-paste"));
    worker.paste(QStringLiteral("blocked\npaste"));
    QCOMPARE(unsafeSpy.count(), 1);
    const quint64 pasteId = unsafeSpy.constFirst().at(0).toULongLong();
    QVERIFY(pasteId != 0);
    worker.confirmPaste(pasteId);

    // The child is already waiting for one byte. If any surface-originated
    // path above escaped the policy boundary it would finish immediately.
    QTest::qWait(100);
    QVERIFY(exitSpy.isEmpty());
    QVERIFY(!updatesContain(updateSpy, QStringLiteral("input-byte:")));

    // Re-enabling input accepts new data but never replays bytes rejected by
    // the preceding read-only interval. Z must therefore be the first byte.
    worker.setReadOnly(false);
    worker.sendRawText(QByteArrayLiteral("Z"));
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("input-byte:5a")), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("readonly-done")), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::releasesHeldModifiersBeforeFocusOut()
{
    qRegisterMetaType<TerminalUpdate>();

    auto expectedAdapter = GhosttyVtAdapter::create({});
    QVERIFY(expectedAdapter != nullptr);
    expectedAdapter->writeVt(QByteArrayLiteral("\033[>11u\033[?1004h"));

    TerminalKeyInput control;
    control.key = Qt::Key_Control;
    control.modifiers = Qt::ControlModifier;
    control.nativeScanCode = KEY_LEFTCTRL + 8U;
    control.pressed = true;

    TerminalKeyInput shift;
    shift.key = Qt::Key_Shift;
    shift.modifiers = Qt::ControlModifier | Qt::ShiftModifier;
    shift.nativeScanCode = KEY_LEFTSHIFT + 8U;
    shift.pressed = true;

    QByteArray expected = expectedAdapter->encodeKey(control).bytes;
    expected.append(expectedAdapter->encodeKey(shift).bytes);
    const auto appendRelease =
        [&expected, &expectedAdapter](int key, quint32 scanCode,
                                      Qt::KeyboardModifiers modifiers) {
            TerminalKeyInput input;
            input.key = key;
            input.modifiers = modifiers;
            input.nativeScanCode = scanCode;
            input.pressed = false;
            expected.append(expectedAdapter->encodeKey(input).bytes);
        };
    appendRelease(Qt::Key_Shift, KEY_RIGHTSHIFT + 8U, Qt::ControlModifier);
    appendRelease(Qt::Key_Shift, KEY_LEFTSHIFT + 8U, Qt::ControlModifier);
    appendRelease(Qt::Key_Control, KEY_RIGHTCTRL + 8U, Qt::NoModifier);
    appendRelease(Qt::Key_Control, KEY_LEFTCTRL + 8U, Qt::NoModifier);
    const QByteArray focusOnly = expectedAdapter->encodeFocus(false);
    expected.append(focusOnly);
    QVERIFY(!focusOnly.isEmpty());
    QVERIFY(!expected.isEmpty());

    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir controlDirectory(
        QDir::current().filePath(QStringLiteral("tmp/focus-mods-XXXXXX")));
    QVERIFY(controlDirectory.isValid());
    const QString readyMarker =
        QDir(controlDirectory.path()).filePath(QStringLiteral("ready"));

    TerminalSessionLaunchOptions options;
    options.workingDirectory = controlDirectory.path();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "stty raw -echo; "
            "printf '\\033[>11u\\033[?1004h\\033[2hkam-ready'; "
            ": > \"$3\"; "
            "payload=$(dd bs=1 count=\"$1\" 2>/dev/null); "
            "printf '\\r\\nkam-bytes:'; "
            "printf '%s' \"$payload\" | od -An -v -tx1 | tr -d ' \\n'; "
            "printf '\\r\\n\\033[2lreadonly-ready'; "
            "payload=$(dd bs=1 count=\"$1\" 2>/dev/null); "
            "printf '\\r\\nreadonly-bytes:'; "
            "printf '%s' \"$payload\" | od -An -v -tx1 | tr -d ' \\n'; "
            "printf '\\r\\nsequence-ready'; "
            "payload=$(dd bs=1 count=\"$1\" 2>/dev/null); "
            "printf '\\r\\nsequence-bytes:'; "
            "printf '%s' \"$payload\" | od -An -v -tx1 | tr -d ' \\n'; "
            "printf '\\r\\nmodifier-ready'; "
            "payload=$(dd bs=1 count=\"$2\" 2>/dev/null); "
            "printf '\\r\\nmodifier-bytes:'; "
            "printf '%s' \"$payload\" | od -An -v -tx1 | tr -d ' \\n'; "
            "printf '\\r\\nmodifier-done\\r\\n'"),
        QStringLiteral("focus-modifier-test"),
        QString::number(focusOnly.size()),
        QString::number(expected.size()),
        readyMarker,
    };
    options.hold = true;
    options.runtime.vtKamAllowed = true;

    SessionWorker worker;
    QSignalSpy updates(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy errors(&worker, &SessionWorker::errorOccurred);
    QVERIFY(worker.initialize(options));
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(readyMarker), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("kam-ready")), 5000);

    // KAM, read-only input, and a dropped leader never reached the terminal,
    // so none may manufacture a later modifier release.
    worker.sendKey(control);
    worker.setFocused(false);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates,
                       QStringLiteral("kam-bytes:%1")
                           .arg(QString::fromLatin1(focusOnly.toHex()))),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("readonly-ready")), 1000);

    worker.setReadOnly(true);
    worker.sendKey(control);
    worker.setReadOnly(false);
    worker.setFocused(false);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates,
                       QStringLiteral("readonly-bytes:%1")
                           .arg(QString::fromLatin1(focusOnly.toHex()))),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("sequence-ready")), 1000);

    worker.stageSequenceKey(1, control);
    worker.resolveSequence(1, TerminalSequenceResolution::Drop, false, {});
    worker.setFocused(false);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates,
                       QStringLiteral("sequence-bytes:%1")
                           .arg(QString::fromLatin1(focusOnly.toHex()))),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("modifier-ready")), 1000);

    worker.sendKey(control);
    worker.sendKey(shift);
    worker.setFocused(false);

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates,
                       QStringLiteral("modifier-bytes:%1")
                           .arg(QString::fromLatin1(expected.toHex()))),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("modifier-done")), 1000);
    QVERIFY2(errors.isEmpty(),
             errors.isEmpty()
                 ? ""
                 : qPrintable(errors.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::appliesLiveEnquiryResponse()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir controlDirectory(
        QDir::current().filePath(QStringLiteral("tmp/enquiry-XXXXXX")));
    QVERIFY(controlDirectory.isValid());
    const QDir controls(controlDirectory.path());
    const QString firstResponsePath =
        controls.filePath(QStringLiteral("first-response"));
    const QString reloadMarker = controls.filePath(QStringLiteral("reload"));
    const QString secondResponsePath =
        controls.filePath(QStringLiteral("second-response"));
    const QString readOnlyMarker =
        controls.filePath(QStringLiteral("readonly"));
    const QString thirdResponsePath =
        controls.filePath(QStringLiteral("third-response"));

    QByteArray initialResponse(1024, '\0');
    for (qsizetype index = 0; index < initialResponse.size(); ++index) {
        initialResponse[index] = static_cast<char>(index);
    }
    const QByteArray reloadedResponse = QByteArrayLiteral("NEXT");
    const QByteArray readOnlyResponse = QByteArray::fromHex("0052fe");

    TerminalSessionLaunchOptions options;
    options.workingDirectory = controlDirectory.path();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("stty raw -echo; "
                       "printf 'enquiry-ready\\005'; "
                       "dd bs=1 count=1024 of=\"$1\" 2>/dev/null; "
                       "printf '\\r\\nfirst-complete'; "
                       "while [ ! -e \"$2\" ]; do sleep 0.01; done; "
                       "printf '\\005'; "
                       "dd bs=1 count=4 of=\"$3\" 2>/dev/null; "
                       "printf '\\r\\nsecond-complete'; "
                       "while [ ! -e \"$4\" ]; do sleep 0.01; done; "
                       "printf '\\005'; "
                       "dd bs=1 count=3 of=\"$5\" 2>/dev/null; "
                       "printf '\\r\\nreadonly-complete'; "
                       "sleep 30"),
        QStringLiteral("enquiry-test"),
        firstResponsePath,
        reloadMarker,
        secondResponsePath,
        readOnlyMarker,
        thirdResponsePath,
    };
    options.hold = true;
    options.runtime.enquiryResponse = initialResponse;
    QVERIFY(worker.initialize(options));

    const auto readFile = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };
    const auto touch = [](const QString &path) {
        QFile marker(path);
        return marker.open(QIODevice::WriteOnly | QIODevice::Truncate);
    };

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("enquiry-ready")), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(readFile(firstResponsePath), initialResponse,
                              5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("first-complete")), 1000);

    TerminalSessionRuntimeOptions runtime = options.runtime;
    runtime.enquiryResponse = reloadedResponse;
    worker.applyRuntimeOptions(runtime);
    QVERIFY(touch(reloadMarker));
    QTRY_COMPARE_WITH_TIMEOUT(readFile(secondResponsePath), reloadedResponse,
                              5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("second-complete")), 1000);

    // ENQ is terminal protocol traffic. It bypasses the surface-input
    // read-only gate just like cursor-position and focus-report responses.
    runtime.enquiryResponse = readOnlyResponse;
    worker.applyRuntimeOptions(runtime);
    worker.setReadOnly(true);
    QVERIFY(touch(readOnlyMarker));
    QTRY_COMPARE_WITH_TIMEOUT(readFile(thirdResponsePath), readOnlyResponse,
                              5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("readonly-complete")), 1000);

    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::stagesAndResolvesSequenceBytes()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "stty raw -echo; "
            "printf 'sequence-ready'; "
            "payload=$(dd bs=1 count=4 2>/dev/null); "
            "stty sane; "
            "printf 'sequence-bytes:'; "
            "printf '%s' \"$payload\" | od -An -tx1 | tr -d ' \\n'; "
            "printf '\\n'")};
    options.hold = true;
    worker.initialize(options);

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("sequence-ready")), 5000);

    const auto key = [](QChar character) {
        TerminalKeyInput input;
        input.key = character.toUpper().unicode();
        input.text = QString(character);
        input.pressed = true;
        return input;
    };

    // Dropped bytes never reach the child. A newer token supersedes an
    // unresolved sequence, and operations from either older token are stale.
    worker.stageSequenceKey(1, key(u'x'));
    worker.resolveSequence(1, TerminalSequenceResolution::Drop, false, {});
    worker.sendKey(key(u'y'));
    worker.stageSequenceKey(2, key(u'x'));
    worker.stageSequenceKey(3, key(u'a'));
    worker.stageSequenceKey(2, key(u'q'));
    worker.stageSequenceKey(3, key(u'b'));
    worker.resolveSequence(2, TerminalSequenceResolution::Flush, false, {});
    QTest::qWait(50);
    QVERIFY(exitSpy.isEmpty());
    worker.resolveSequence(3,
                           TerminalSequenceResolution::FlushAndSendCurrent,
                           true, key(u'c'));

    QTRY_VERIFY_WITH_TIMEOUT(exitSpy.count() > 0, 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    const QString finalContents = frameText(accumulatedFrame(updateSpy));
    QVERIFY2(finalContents.contains(
                 QStringLiteral("sequence-bytes:79616263")),
             qPrintable(finalContents));
    worker.shutdown();
}

void SessionWorkerTest::sendsConsumedShiftTextLiterallyInKittyMode()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"), QStringLiteral("-c"),
        QStringLiteral(
            "stty raw -echo; "
            "printf '\\033[>3ukitty-quit-ready'; "
            "payload=$(dd bs=1 count=3 2>/dev/null); "
            "stty sane; "
            "printf '\\r\\nkitty-quit-bytes:'; "
            "printf '%s' \"$payload\" | od -An -v -tx1 | tr -d ' \\n'; "
            "printf '\\r\\n'")};
    options.hold = true;
    QVERIFY(worker.initialize(options));

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("kitty-quit-ready")), 5000);

    TerminalKeyInput colon;
    colon.key = Qt::Key_Colon;
    colon.modifiers = Qt::ShiftModifier;
    colon.consumedModifiers = Qt::ShiftModifier;
    colon.text = QStringLiteral(":");
    colon.nativeScanCode = KEY_SEMICOLON + 8U;
    colon.unshiftedCodepoint = ';';
    worker.sendKey(colon);

    TerminalKeyInput q;
    q.key = Qt::Key_Q;
    q.text = QStringLiteral("q");
    q.nativeScanCode = KEY_Q + 8U;
    q.unshiftedCodepoint = 'q';
    worker.sendKey(q);

    TerminalKeyInput enter;
    enter.key = Qt::Key_Return;
    enter.text = QStringLiteral("\r");
    enter.nativeScanCode = KEY_ENTER + 8U;
    worker.sendKey(enter);

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    const QString finalContents = frameText(accumulatedFrame(updateSpy));
    QVERIFY2(finalContents.contains(QStringLiteral("kitty-quit-bytes:3a710d")),
             qPrintable(finalContents));
    worker.shutdown();
}

void SessionWorkerTest::stagesSequenceKeysUsingModesAtStageTime()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "stty raw -echo; "
            "printf 'normal-ready'; "
            "sleep 1; "
            "printf '\\033[?1happlication-ready'; "
            "payload=$(dd bs=1 count=3 2>/dev/null); "
            "stty sane; "
            "printf 'staged-mode-bytes:'; "
            "printf '%s' \"$payload\" | od -An -tx1 | tr -d ' \\n'; "
            "printf '\\n'")};
    options.hold = true;
    worker.initialize(options);

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("normal-ready")), 5000);
    TerminalKeyInput up;
    up.key = Qt::Key_Up;
    up.pressed = true;
    worker.stageSequenceKey(1, up);

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("application-ready")), 5000);
    worker.resolveSequence(1, TerminalSequenceResolution::Flush, false, {});

    QTRY_VERIFY_WITH_TIMEOUT(exitSpy.count() > 0, 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    const QString finalContents = frameText(accumulatedFrame(updateSpy));
    // Normal cursor mode is ESC [ A. Encoding only at resolution would have
    // observed DECCKM and incorrectly emitted ESC O A instead.
    QVERIFY2(finalContents.contains(
                 QStringLiteral("staged-mode-bytes:1b5b41")),
             qPrintable(finalContents));
    worker.shutdown();
}

void SessionWorkerTest::gatesKeyboardAndImeWithLiveKamPolicy()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy selectionSpy(&worker, &SessionWorker::selectionAvailableChanged);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"), QStringLiteral("-c"),
        QStringLiteral(
            "stty raw -echo; "
            "printf '\\033[2hkam-enabled'; "
            "first=$(dd bs=1 count=3 2>/dev/null | "
            "od -An -v -tx1 | tr -d ' \\n'); "
            "printf '\\r\\nkam-blocked:%s\\r\\n' \"$first\"; "
            "second=$(dd bs=1 count=2 2>/dev/null | "
            "od -An -v -tx1 | tr -d ' \\n'); "
            "printf '\\033[2l\\r\\nkam-policy-bypassed:%s\\r\\n' \"$second\"; "
            "third=$(dd bs=1 count=2 2>/dev/null | "
            "od -An -v -tx1 | tr -d ' \\n'); "
            "stty sane; "
            "printf '\\r\\nkam-mode-disabled:%s\\r\\n' \"$third\"; ")};
    options.runtime.vtKamAllowed = true;
    QVERIFY(worker.initialize(options));

    const auto key = [](QChar character) {
        TerminalKeyInput input;
        input.key = character.toUpper().unicode();
        input.text = QString(character);
        input.unshiftedCodepoint = character.toLower().unicode();
        input.pressed = true;
        return input;
    };

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("kam-enabled")), 5000);

    // Preedit lifecycle remains terminal-local and clears selection before
    // KAM gates only the committed IME key event.
    worker.selectAll();
    QTRY_VERIFY_WITH_TIMEOUT(spyContainsBool(selectionSpy, true), 1000);
    worker.sendInputMethod({.preeditTransition = true});
    QTRY_VERIFY_WITH_TIMEOUT(spyContainsBool(selectionSpy, false), 1000);

    worker.sendKey(key(u'a'));
    // Ghostty handles keybinding leaders before KAM. Their encoded bytes
    // remain staged and can be flushed even though the separately resolving
    // ordinary key is suppressed.
    worker.stageSequenceKey(1, key(u'b'));
    worker.resolveSequence(1, TerminalSequenceResolution::FlushAndSendCurrent,
                           true, key(u'c'));
    worker.sendInputMethod({.commitText = QStringLiteral("d")});

    // Keybinding-produced raw input and accepted paste bypass KAM exactly as
    // they bypass Ghostty's ordinary physical-key path.
    worker.sendRawText(QByteArrayLiteral("R"));
    worker.paste(QStringLiteral("P"));
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("kam-blocked:625250")), 5000);

    TerminalSessionRuntimeOptions runtime = options.runtime;
    runtime.vtKamAllowed = false;
    worker.applyRuntimeOptions(runtime);
    worker.sendKey(key(u'e'));
    worker.sendInputMethod({.commitText = QStringLiteral("f")});
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("kam-policy-bypassed:6566")),
        5000);

    // The child has now reset ANSI mode 2. Re-enabling the live policy does
    // not suppress input unless the terminal-owned mode is active.
    runtime.vtKamAllowed = true;
    worker.applyRuntimeOptions(runtime);
    worker.sendKey(key(u'g'));
    worker.sendInputMethod({.commitText = QStringLiteral("h")});

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    const QString finalContents = frameText(accumulatedFrame(updateSpy));
    QVERIFY2(finalContents.contains(QStringLiteral("kam-mode-disabled:6768")),
             qPrintable(finalContents));
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::appliesReloadedAppearanceToExistingTerminal()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("printf 'color-reload-ready'; sleep 5"),
    };
    options.hold = true;
    worker.initialize(options);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("color-reload-ready")), 5000);
    updateSpy.clear();

    TerminalAppearance &appearance = options.runtime.appearance;
    appearance.foregroundColor = QColor(QStringLiteral("#abcdef"));
    appearance.backgroundColor = QColor(QStringLiteral("#102030"));
    appearance.cursorColor = TerminalColorValue::fromColor(
        QColor(QStringLiteral("#fedcba")));
    appearance.palette.resize(256);
    for (int index = 0; index < appearance.palette.size(); ++index) {
        appearance.palette[index] =
            QColor::fromRgb(index, 255 - index, index / 2);
    }
    appearance.cursorStyle = TerminalCursorStyle::Underline;
    appearance.cursorBlink = false;
    worker.applyRuntimeOptions(options.runtime);

    QTRY_VERIFY_WITH_TIMEOUT(
        accumulatedFrame(updateSpy).foreground == appearance.foregroundColor
            && accumulatedFrame(updateSpy).palette.size() == 256,
        2000);
    const TerminalFrame frame = accumulatedFrame(updateSpy);
    QCOMPARE(frame.background, appearance.backgroundColor);
    QCOMPARE(frame.cursorColor, appearance.cursorColor.color);
    QVERIFY(frame.cursorColorExplicit);
    QCOMPARE(frame.palette.at(42), QColor::fromRgb(42, 213, 21));
    QCOMPARE(frame.cursorStyle, 2);
    QVERIFY(!frame.cursorBlinking);
    QVERIFY(!containsCursorBlinkReset(updateSpy));
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty() ? ""
                                : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::ordersLiveColorSchemeReportsThroughPty()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("stty raw -echo; "
                       "printf '\\033[?996n'; "
                       "initial=$(dd bs=1 count=9 2>/dev/null); "
                       "printf '\\033[?2031hcolor-scheme-ready'; "
                       "reports=$(dd bs=1 count=18 2>/dev/null); "
                       "stty sane; "
                       "printf '\\r\\ncolor-scheme-bytes:'; "
                       "printf '%s%s' \"$initial\" \"$reports\" "
                       "| od -An -tx1 | tr -d ' \\n'; "
                       "printf '\\r\\n'"),
    };
    options.hold = true;
    options.runtime.colorScheme = TerminalColorScheme::Dark;
    QVERIFY(worker.initialize(options));
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("color-scheme-ready")), 5000);

    TerminalSessionRuntimeOptions runtime = options.runtime;
    runtime.colorScheme = TerminalColorScheme::Light;
    worker.applyRuntimeOptions(runtime);
    worker.applyRuntimeOptions(runtime);
    runtime.colorScheme = TerminalColorScheme::Dark;
    worker.applyRuntimeOptions(runtime);

    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    const QString finalContents = frameText(accumulatedFrame(updateSpy));
    QVERIFY2(finalContents.contains(QStringLiteral("color-scheme-bytes:"
                                                   "1b5b3f3939373b316e"
                                                   "1b5b3f3939373b326e"
                                                   "1b5b3f3939373b316e")),
             qPrintable(finalContents));
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::appliesLiveScrollbackCompressionPolicy()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalSearchUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy searchSpy(&worker, &SessionWorker::searchUpdated);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("i=0; while [ $i -lt 1000 ]; do "
                       "printf 'compression-row-%03d\\r\\n' \"$i\"; "
                       "i=$((i + 1)); done; "
                       "printf 'compression-ready'; sleep 30"),
    };
    options.hold = true;
    options.runtime.scrollbackCompression = false;
    QVERIFY(worker.initialize(options));
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("compression-ready")), 5000);

    QTimer *compressionTimer =
        worker.findChild<QTimer *>(QStringLiteral("scrollbackCompressionTimer"),
                                   Qt::FindDirectChildrenOnly);
    QVERIFY(compressionTimer != nullptr);
    QVERIFY(compressionTimer->isSingleShot());
    QCOMPARE(compressionTimer->interval(), 250);
    QVERIFY(!compressionTimer->isActive());

    // Mutations while disabled must not leak through the ordinary activity
    // scheduler.
    worker.resizeTerminal(81, 24, 8, 16, 648, 384);
    QVERIFY(!compressionTimer->isActive());

    // Search owns three direct scheduling paths because its public row reads
    // restore compressed pages without changing the activity token. Query
    // progress, replacement, and cancellation must all honor the same gate.
    worker.search(1, QByteArrayLiteral("compression-row"));
    for (int chunk = 0; chunk < 20; ++chunk) {
        QVERIFY(QMetaObject::invokeMethod(&worker, "processSearchChunk",
                                          Qt::DirectConnection));
        QVERIFY(!compressionTimer->isActive());
    }
    worker.search(2, QByteArrayLiteral("row"));
    QVERIFY(!compressionTimer->isActive());
    QVERIFY(QMetaObject::invokeMethod(&worker, "processSearchChunk",
                                      Qt::DirectConnection));
    worker.cancelSearch(3);
    QVERIFY(!compressionTimer->isActive());

    TerminalSessionRuntimeOptions runtime = options.runtime;
    runtime.scrollbackCompression = true;
    worker.applyRuntimeOptions(runtime);
    QVERIFY(compressionTimer->isActive());
    QVERIFY(compressionTimer->remainingTime() > 0);
    QCOMPARE(compressionTimer->interval(), 250);

    // Restored-page recovery must not shorten an already-armed activity idle
    // deadline. The existing pass will see the pages when it runs.
    worker.search(4, QByteArrayLiteral("compression-row"));
    for (int chunk = 0; chunk < 20; ++chunk) {
        QVERIFY(QMetaObject::invokeMethod(&worker, "processSearchChunk",
                                          Qt::DirectConnection));
    }
    worker.cancelSearch(5);
    QVERIFY(compressionTimer->isActive());
    QCOMPARE(compressionTimer->interval(), 250);

    // Disabling cancels an already-armed idle pass. A timeout already queued
    // by Qt is harmless because the callback rechecks the live policy.
    runtime.scrollbackCompression = false;
    worker.applyRuntimeOptions(runtime);
    QVERIFY(!compressionTimer->isActive());
    QVERIFY(QMetaObject::invokeMethod(&worker, "compressScrollback",
                                      Qt::DirectConnection));
    QVERIFY(!compressionTimer->isActive());

    // Re-enabling deliberately forgets the cached activity token so resident
    // pages are reconsidered even without another terminal mutation.
    runtime.scrollbackCompression = true;
    worker.applyRuntimeOptions(runtime);
    QVERIFY(compressionTimer->isActive());
    QVERIFY(compressionTimer->remainingTime() > 0);

    // Drive the public incremental bridge to completion without waiting on
    // wall-clock timers, then prove that restored history remains readable.
    // Enough rows were emitted above to span multiple Ghostty pages.
    compressionTimer->stop();
    int compressionPasses = 0;
    for (; compressionPasses < 4096; ++compressionPasses) {
        QVERIFY(QMetaObject::invokeMethod(&worker, "compressScrollback",
                                          Qt::DirectConnection));
        if (!compressionTimer->isActive()) {
            break;
        }
        QCOMPARE(compressionTimer->interval(), 1);
        compressionTimer->stop();
    }
    QVERIFY(compressionPasses > 0);
    QVERIFY(compressionPasses < 4096);

    // Begin another metadata traversal over the now-compressed pages, then
    // restore history before its continuation. The worker must latch a fresh
    // replay instead of letting an already-inspected page escape the current
    // verification cursor.
    QVERIFY(QMetaObject::invokeMethod(&worker, "compressScrollback",
                                      Qt::DirectConnection));
    QVERIFY(compressionTimer->isActive());
    QCOMPARE(compressionTimer->interval(), 1);
    compressionTimer->stop();

    worker.search(6, QByteArrayLiteral("compression-row"));
    for (int chunk = 0; chunk < 1024; ++chunk) {
        const std::optional<TerminalSearchUpdate> update =
            latestSearchUpdate(searchSpy, 6);
        if (update.has_value() && update->complete) {
            break;
        }
        QVERIFY(QMetaObject::invokeMethod(&worker, "processSearchChunk",
                                          Qt::DirectConnection));
    }
    const std::optional<TerminalSearchUpdate> compressedSearch =
        latestSearchUpdate(searchSpy, 6);
    QVERIFY(compressedSearch.has_value());
    QVERIFY(compressedSearch->complete);
    QCOMPARE(compressedSearch->totalMatches, quint64(1000));
    QVERIFY(compressionTimer->isActive());
    QCOMPARE(compressionTimer->interval(), 1);

    int recoveryPasses = 0;
    for (; recoveryPasses < 4096; ++recoveryPasses) {
        compressionTimer->stop();
        QVERIFY(QMetaObject::invokeMethod(&worker, "compressScrollback",
                                          Qt::DirectConnection));
        if (!compressionTimer->isActive()) {
            break;
        }
        QCOMPARE(compressionTimer->interval(), 1);
    }
    QVERIFY(recoveryPasses > 0);
    QVERIFY(recoveryPasses < 4096);

    runtime.scrollbackCompression = false;
    worker.applyRuntimeOptions(runtime);
    QVERIFY(!compressionTimer->isActive());
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::appliesLiveScrollToBottomPolicy()
{
    qRegisterMetaType<TerminalUpdate>();

    const auto letter = [](QChar character) {
        TerminalKeyInput input;
        input.key = character.toUpper().unicode();
        input.text = QString(character);
        input.unshiftedCodepoint = character.toLower().unicode();
        input.pressed = true;
        return input;
    };

    {
        SessionWorker worker;
        worker.resizeTerminal(32, 4, 8, 16, 256, 64);
        QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
        QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

        TerminalSessionLaunchOptions options;
        options.workingDirectory = QDir::tempPath();
        options.program = {
            QStringLiteral("/bin/sh"),
            QStringLiteral("-c"),
            QStringLiteral("stty raw -echo; "
                           "i=0; while [ $i -lt 48 ]; do "
                           "printf 'keystroke-history-%02d\\r\\n' \"$i\"; "
                           "i=$((i + 1)); done; "
                           "printf 'keystroke-ready'; sleep 30"),
        };
        options.hold = true;
        QVERIFY(worker.initialize(options));
        QTRY_VERIFY_WITH_TIMEOUT(
            updatesContain(updateSpy, QStringLiteral("keystroke-ready")), 5000);

        const auto scrollToTop = [&] {
            worker.scrollViewport({.kind = TerminalViewportRequest::Kind::Top});
            QTRY_COMPARE_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset,
                                      quint64{0}, 1000);
        };
        const auto expectBottom = [&] {
            QTRY_VERIFY_WITH_TIMEOUT(
                accumulatedFrame(updateSpy).scrollOffset > 0, 1000);
        };
        const auto expectStillAtTop = [&] {
            QTest::qWait(50);
            QCOMPARE(accumulatedFrame(updateSpy).scrollOffset, quint64{0});
        };

        // The default follows Ghostty: an encoded ordinary key returns the
        // viewport to the active screen.
        scrollToTop();
        worker.sendKey(letter(u'a'));
        expectBottom();

        TerminalSessionRuntimeOptions runtime = options.runtime;
        runtime.scrollToBottom.keystroke = false;
        worker.applyRuntimeOptions(runtime);
        scrollToTop();
        worker.sendKey(letter(u'b'));
        expectStillAtTop();
        worker.sendInputMethod({.commitText = QStringLiteral("ime")});
        expectStillAtTop();

        // Keybind payloads and accepted paste are intentionally independent
        // of the physical-keystroke policy. A valid empty text action still
        // has the upstream viewport side effect without writing PTY bytes.
        worker.sendRawText({});
        expectBottom();

        runtime.scrollToBottom.keystroke = true;
        worker.applyRuntimeOptions(runtime);
        scrollToTop();
        worker.sendInputMethod({.commitText = QStringLiteral("ime")});
        expectBottom();

        // Replayed leaders never retroactively become keystrokes. Only a
        // separately encoded, non-modifier current key participates.
        scrollToTop();
        worker.stageSequenceKey(1, letter(u'c'));
        worker.resolveSequence(1, TerminalSequenceResolution::Flush, false, {});
        expectStillAtTop();
        worker.stageSequenceKey(2, letter(u'd'));
        worker.resolveSequence(2,
                               TerminalSequenceResolution::FlushAndSendCurrent,
                               true, letter(u'e'));
        expectBottom();

        // Read-only mode suppresses the bytes at the PTY boundary, not the
        // surface-level viewport behavior.
        worker.setReadOnly(true);
        scrollToTop();
        worker.sendKey(letter(u'f'));
        expectBottom();

        QVERIFY2(
            errorSpy.isEmpty(),
            errorSpy.isEmpty()
                ? ""
                : qPrintable(errorSpy.constFirst().constFirst().toString()));
        worker.shutdown();
    }

    {
        QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
        QTemporaryDir controlDirectory(QDir::current().filePath(
            QStringLiteral("tmp/scroll-output-XXXXXX")));
        QVERIFY(controlDirectory.isValid());
        const QDir controls(controlDirectory.path());
        const QString rewriteMarker =
            controls.filePath(QStringLiteral("rewrite"));
        const QString advanceMarker =
            controls.filePath(QStringLiteral("advance"));
        const QString disabledMarker =
            controls.filePath(QStringLiteral("disabled"));

        SessionWorker worker;
        worker.resizeTerminal(32, 4, 8, 16, 256, 64);
        QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
        QSignalSpy titleSpy(&worker, &SessionWorker::titleChanged);
        QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

        TerminalSessionLaunchOptions options;
        options.workingDirectory = controlDirectory.path();
        options.program = {
            QStringLiteral("/bin/sh"),
            QStringLiteral("-c"),
            QStringLiteral(
                "i=0; while [ $i -lt 48 ]; do "
                "printf 'output-history-%02d\\r\\n' \"$i\"; "
                "i=$((i + 1)); done; "
                "printf 'output-ready'; "
                "while [ ! -e \"$1\" ]; do sleep 0.01; done; "
                "printf '\\rrewrite-done\\033]2;rewrite-observed\\007'; "
                "while [ ! -e \"$2\" ]; do sleep 0.01; done; "
                "printf '\\r\\nadvance-done"
                "\\033]2;advance-observed\\007'; "
                "while [ ! -e \"$3\" ]; do sleep 0.01; done; "
                "printf '\\r\\ndisabled-done"
                "\\033]2;disabled-observed\\007'; "
                "sleep 30"),
            QStringLiteral("scroll-output-test"),
            rewriteMarker,
            advanceMarker,
            disabledMarker,
        };
        options.hold = true;
        options.runtime.scrollToBottom.output = true;
        QVERIFY(worker.initialize(options));
        QTRY_VERIFY_WITH_TIMEOUT(
            updatesContain(updateSpy, QStringLiteral("output-ready")), 5000);

        const auto touch = [](const QString &path) {
            QFile marker(path);
            return marker.open(QIODevice::WriteOnly | QIODevice::Truncate);
        };
        const auto scrollToTop = [&] {
            worker.scrollViewport({.kind = TerminalViewportRequest::Kind::Top});
            QTRY_COMPARE_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset,
                                      quint64{0}, 1000);
        };

        // Rewriting the active final line, including an OSC title side
        // effect, preserves the bottom node/y anchor and must not jump.
        scrollToTop();
        const int rewriteFrame = updateSpy.count();
        QVERIFY(touch(rewriteMarker));
        QTRY_VERIFY_WITH_TIMEOUT(
            std::any_of(titleSpy.cbegin(), titleSpy.cend(),
                        [](const QList<QVariant> &arguments) {
                            return arguments.constFirst().toString()
                                == QStringLiteral("rewrite-observed");
                        }),
            5000);
        QTRY_VERIFY_WITH_TIMEOUT(updateSpy.count() > rewriteFrame, 1000);
        QCOMPARE(accumulatedFrame(updateSpy).scrollOffset, quint64{0});

        // Advancing the physical final row changes the opaque anchor and
        // returns the viewport to the active screen before publication.
        QVERIFY(touch(advanceMarker));
        QTRY_VERIFY_WITH_TIMEOUT(
            std::any_of(titleSpy.cbegin(), titleSpy.cend(),
                        [](const QList<QVariant> &arguments) {
                            return arguments.constFirst().toString()
                                == QStringLiteral("advance-observed");
                        }),
            5000);
        QTRY_VERIFY_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset > 0,
                                 1000);

        TerminalSessionRuntimeOptions runtime = options.runtime;
        runtime.scrollToBottom.output = false;
        worker.applyRuntimeOptions(runtime);
        scrollToTop();
        const int disabledFrame = updateSpy.count();
        QVERIFY(touch(disabledMarker));
        QTRY_VERIFY_WITH_TIMEOUT(
            std::any_of(titleSpy.cbegin(), titleSpy.cend(),
                        [](const QList<QVariant> &arguments) {
                            return arguments.constFirst().toString()
                                == QStringLiteral("disabled-observed");
                        }),
            5000);
        QTRY_VERIFY_WITH_TIMEOUT(updateSpy.count() > disabledFrame, 1000);
        QCOMPARE(accumulatedFrame(updateSpy).scrollOffset, quint64{0});

        // Match the pinned renderer's live-reload behavior: observations are
        // paused while disabled, so enabling compares the stale anchor on the
        // config-triggered frame and catches up immediately.
        runtime.scrollToBottom.output = true;
        worker.applyRuntimeOptions(runtime);
        QTRY_VERIFY_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset > 0,
                                 1000);

        QVERIFY2(
            errorSpy.isEmpty(),
            errorSpy.isEmpty()
                ? ""
                : qPrintable(errorSpy.constFirst().constFirst().toString()));
        worker.shutdown();
    }
}

void SessionWorkerTest::appliesReloadedWordBoundariesToExistingGesture()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalClipboardDestination>();
    SessionWorker worker;
    worker.resizeTerminal(24, 3, 8, 16, 192, 48);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy clipboardSpy(&worker, &SessionWorker::clipboardTextReady);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("printf 'alpha;beta gamma'; sleep 5"),
    };
    options.hold = true;
    options.runtime.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::Disabled;
    options.runtime.selectionWordChars = {0, static_cast<quint32>(' ')};
    worker.initialize(options);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("alpha;beta gamma")), 5000);

    beginWordSelection(worker, 7, 0, 1'000'000'000);
    worker.copySelection();
    QCOMPARE(clipboardSpy.size(), 1);
    QCOMPARE(clipboardSpy.constLast().at(0).toString(),
             QStringLiteral("alpha;beta"));

    // Reload does not reset or retroactively reshape the installed range.
    // The next word drag samples the replacement list for both endpoints.
    options.runtime.selectionWordChars = {0, static_cast<quint32>(' '),
                                          static_cast<quint32>(';')};
    worker.applyRuntimeOptions(options.runtime);
    QCOMPARE(clipboardSpy.constLast().at(0).toString(),
             QStringLiteral("alpha;beta"));
    worker.updateSelection(selectionDrag(12, 0));
    worker.copySelection();
    QCOMPARE(clipboardSpy.size(), 2);
    QCOMPARE(clipboardSpy.constLast().at(0).toString(),
             QStringLiteral("beta gamma"));
    worker.endSelection(12, 0);

    beginWordSelection(worker, 7, 0, 2'000'000'000);
    worker.copySelection();
    QCOMPARE(clipboardSpy.size(), 3);
    QCOMPARE(clipboardSpy.constLast().at(0).toString(), QStringLiteral("beta"));
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::appliesClickRepeatIntervalAtLaunchAndReload()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalClipboardDestination>();
    SessionWorker worker;
    worker.resizeTerminal(24, 3, 8, 16, 192, 48);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy clipboardSpy(&worker, &SessionWorker::clipboardTextReady);
    QSignalSpy selectionSpy(&worker, &SessionWorker::selectionAvailableChanged);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("printf 'alpha beta'; sleep 5"),
    };
    options.hold = true;
    options.runtime.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::Disabled;
    options.runtime.clickRepeatIntervalMilliseconds = 100;
    worker.initialize(options);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("alpha beta")), 5000);

    // The launch-time 100 ms interval rejects this 150 ms repeat, leaving the
    // second press as the new single-click history anchor.
    worker.beginSelection(selectionPress(2, 0, 1'000'000'000));
    worker.endSelection(2, 0);
    worker.beginSelection(selectionPress(2, 0, 1'150'000'000));
    worker.endSelection(2, 0);
    QVERIFY(!spyContainsBool(selectionSpy, true));
    worker.copySelection();
    QCOMPARE(clipboardSpy.count(), 0);

    // Reloading only the interval preserves that 1.15 s anchor. The next
    // press is now a 250 ms repeat and therefore selects the word.
    options.runtime.clickRepeatIntervalMilliseconds = 300;
    worker.applyRuntimeOptions(options.runtime);
    worker.beginSelection(selectionPress(2, 0, 1'400'000'000));
    worker.copySelection();
    QCOMPARE(clipboardSpy.count(), 1);
    QCOMPARE(clipboardSpy.constFirst().at(0).toString(),
             QStringLiteral("alpha"));
    QVERIFY(spyContainsBool(selectionSpy, true));
    worker.endSelection(2, 0);

    // Strict configuration rejects zero, but the worker still defends its
    // direct runtime API. A rejected update reports the failure and leaves the
    // prior 300 ms adapter policy and click history intact.
    TerminalSessionRuntimeOptions invalidRuntime = options.runtime;
    invalidRuntime.clickRepeatIntervalMilliseconds = 0;
    worker.applyRuntimeOptions(invalidRuntime);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.constFirst().constFirst().toString(),
             QStringLiteral(
                 "Failed to apply click repeat interval to libghostty-vt."));
    worker.beginSelection(selectionPress(2, 0, 1'700'000'000));
    worker.copySelection();
    QCOMPARE(clipboardSpy.count(), 2);
    QCOMPARE(clipboardSpy.constLast().at(0).toString(),
             QStringLiteral("alpha beta"));
    worker.shutdown();
}

void SessionWorkerTest::extendsSelectionUsingReloadedClickInterval()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalClipboardDestination>();
    SessionWorker worker;
    worker.resizeTerminal(12, 3, 8, 16, 96, 48);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy clipboardSpy(&worker, &SessionWorker::clipboardTextReady);
    QSignalSpy selectionSpy(&worker, &SessionWorker::selectionAvailableChanged);
    QSignalSpy activationSpy(&worker,
                             &SessionWorker::hyperlinkActivationResolved);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("printf '\\033]8;;https://example.com\\007alpha beta"
                       "\\033]8;;\\007'; sleep 5"),
    };
    options.hold = true;
    options.runtime.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::Disabled;
    options.runtime.clickRepeatIntervalMilliseconds = 200;
    worker.initialize(options);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("alpha beta")), 5000);

    constexpr quint64 firstTimestamp = 1'000'000'000;
    worker.beginSelection(selectionPress(1, 0, firstTimestamp));
    worker.updateSelection(selectionDrag(3, 0));
    worker.endSelection(3, 0);
    QVERIFY(spyContainsBool(selectionSpy, true));
    const qsizetype availabilityCount = selectionSpy.size();

    // This 150 ms Shift press would still be an ordinary press under the
    // launch-time interval. The live 100 ms reload makes it a delayed drag
    // from the retained cell-one anchor instead.
    options.runtime.clickRepeatIntervalMilliseconds = 100;
    worker.applyRuntimeOptions(options.runtime);
    worker.beginSelection(
        selectionPress(5, 0, firstTimestamp + 150'000'000, false, true));
    QCOMPARE(selectionSpy.size(), availabilityCount);
    worker.copySelection();
    QCOMPARE(clipboardSpy.size(), 1);
    QCOMPARE(clipboardSpy.constFirst().at(0).toString(),
             QStringLiteral("lpha"));
    worker.endSelection(5, 0);

    // Re-anchor with a double click so the current gesture has word behavior
    // and has not been dragged. A delayed Shift press on the armed link
    // extends by whole words and the release-stable drag state prevents that
    // same press from also opening the link.
    constexpr quint64 wordTimestamp = 2'000'000'000;
    worker.beginSelection(selectionPress(1, 0, wordTimestamp));
    worker.endSelection(1, 0);
    worker.beginSelection(selectionPress(1, 0, wordTimestamp + 1'000'000));
    worker.endSelection(1, 0);
    const TerminalFrame frame = accumulatedFrame(updateSpy);
    worker.prepareHyperlinkActivation(77, frame.contentRevision, 8, 0);
    worker.beginSelection(
        selectionPress(8, 0, wordTimestamp + 150'000'000, false, true));
    worker.endSelection(8, 0);
    worker.copySelection();
    QCOMPARE(clipboardSpy.size(), 2);
    QCOMPARE(clipboardSpy.constLast().at(0).toString(),
             QStringLiteral("alpha beta"));
    worker.commitHyperlinkActivation(77, 8, 0);
    QCOMPARE(activationSpy.size(), 1);
    QCOMPARE(activationSpy.constFirst().at(0).toULongLong(), quint64{77});
    QVERIFY(activationSpy.constFirst().at(3).toByteArray().isEmpty());

    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::clearsSelectionOnlyForUpstreamTypingPaths()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalClipboardDestination>();
    SessionWorker worker;
    worker.resizeTerminal(32, 4, 8, 16, 256, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy selectionSpy(&worker,
                            &SessionWorker::selectionAvailableChanged);
    QSignalSpy clipboardSpy(&worker, &SessionWorker::clipboardTextReady);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir controlDirectory(
        QDir::current().filePath(QStringLiteral("tmp/selection-typing-XXXXXX")));
    QVERIFY(controlDirectory.isValid());
    const QString kittyMarker =
        QDir(controlDirectory.path()).filePath(QStringLiteral("enable-kitty"));
    options.workingDirectory = controlDirectory.path();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "stty raw -echo; "
            "printf 'selection-target\\r\\nlegacy-ready'; "
            "while [ ! -e \"$1\" ]; do sleep 0.01; done; "
            "printf '\\033[>11ukitty-ready'; "
            "sleep 5"),
        QStringLiteral("selection-typing-test"),
        kittyMarker,
    };
    options.hold = true;
    options.runtime.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::Disabled;
    worker.initialize(options);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("legacy-ready")), 5000);

    const auto letter = [](QChar character, bool pressed = true) {
        TerminalKeyInput input;
        input.key = character.toUpper().unicode();
        input.text = QString(character);
        input.unshiftedCodepoint = character.toLower().unicode();
        input.pressed = pressed;
        return input;
    };
    const auto selectTarget = [&] {
        worker.clearSelection();
        selectionSpy.clear();
        worker.beginSelection(selectionPress(0, 0));
        worker.updateSelection(selectionDrag(7, 0));
        worker.endSelection(7, 0);
        QVERIFY(spyContainsBool(selectionSpy, true));
        selectionSpy.clear();
    };
    const auto expectClearedWithoutCopy = [&] {
        QVERIFY(spyContainsBool(selectionSpy, false));
        const qsizetype copies = clipboardSpy.size();
        worker.copySelection();
        QCOMPARE(clipboardSpy.size(), copies);
        selectionSpy.clear();
    };
    const auto expectPreservedWithoutCopy = [&] {
        QVERIFY(!spyContainsBool(selectionSpy, false));
        const qsizetype copies = clipboardSpy.size();
        worker.copySelection();
        QCOMPARE(clipboardSpy.size(), copies + 1);
        worker.clearSelection();
        selectionSpy.clear();
    };

    // The default applies only after a non-modifier key actually encodes.
    selectTarget();
    worker.sendKey(letter(u'a'));
    expectClearedWithoutCopy();

    options.runtime.selectionClipboard.clearOnTyping = false;
    worker.applyRuntimeOptions(options.runtime);
    selectTarget();
    worker.sendKey(letter(u'b'));
    expectPreservedWithoutCopy();

    // A physical Escape reaching ordinary encoding is the sole config-false
    // override. A legacy release and an unidentified empty key encode nothing.
    selectTarget();
    TerminalKeyInput escape;
    escape.key = Qt::Key_Escape;
    escape.nativeScanCode = KEY_ESC + 8U;
    worker.sendKey(escape);
    expectClearedWithoutCopy();

    selectTarget();
    worker.sendKey(letter(u'c', false));
    expectPreservedWithoutCopy();

    selectTarget();
    worker.sendKey({});
    expectPreservedWithoutCopy();

    options.runtime.selectionClipboard.clearOnTyping = true;
    worker.applyRuntimeOptions(options.runtime);

    // Data-writing actions and paste bypass the physical-key policy.
    selectTarget();
    worker.sendCsi(QByteArrayLiteral("31m"));
    worker.sendEscape(QByteArrayLiteral("7"));
    worker.sendRawText(QByteArrayLiteral("action-text"));
    worker.paste(QStringLiteral("paste-text"));
    expectPreservedWithoutCopy();

    // IME commits are typed unidentified keys; preedit transitions clear
    // independently and carry no clipboard side effect.
    selectTarget();
    worker.sendInputMethod({.commitText = QStringLiteral("commit")});
    expectClearedWithoutCopy();

    selectTarget();
    worker.sendInputMethod({.preeditTransition = true});
    expectClearedWithoutCopy();

    options.runtime.selectionClipboard.clearOnTyping = false;
    worker.applyRuntimeOptions(options.runtime);
    selectTarget();
    worker.sendInputMethod({.commitText = QStringLiteral("commit")});
    expectPreservedWithoutCopy();
    selectTarget();
    worker.sendInputMethod({.preeditTransition = true});
    expectPreservedWithoutCopy();

    QFile marker(kittyMarker);
    QVERIFY(marker.open(QIODevice::WriteOnly));
    marker.close();
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("kitty-ready")), 5000);
    options.runtime.selectionClipboard.clearOnTyping = true;
    worker.applyRuntimeOptions(options.runtime);

    // Kitty report-all makes the physical modifier encode, but it remains
    // excluded. Kitty report-events makes a non-modifier release eligible.
    TerminalKeyInput shift;
    shift.key = Qt::Key_A;
    shift.nativeScanCode = KEY_LEFTSHIFT + 8U;
    selectTarget();
    worker.sendKey(shift);
    expectPreservedWithoutCopy();

    selectTarget();
    worker.sendKey(letter(u'd', false));
    expectClearedWithoutCopy();

    // Leaders never retroactively become typing when replayed. Only a
    // separately encoded current key in FlushAndSendCurrent can clear.
    selectTarget();
    worker.stageSequenceKey(1, letter(u'e'));
    QVERIFY(!spyContainsBool(selectionSpy, false));
    worker.resolveSequence(1, TerminalSequenceResolution::Drop, false, {});
    expectPreservedWithoutCopy();

    selectTarget();
    worker.stageSequenceKey(2, letter(u'f'));
    worker.resolveSequence(2, TerminalSequenceResolution::Flush, false, {});
    expectPreservedWithoutCopy();

    selectTarget();
    worker.stageSequenceKey(3, letter(u'g'));
    worker.resolveSequence(3,
                           TerminalSequenceResolution::FlushAndSendCurrent,
                           true, letter(u'h'));
    expectClearedWithoutCopy();

    selectTarget();
    worker.stageSequenceKey(4, letter(u'i'));
    worker.resolveSequence(4,
                           TerminalSequenceResolution::FlushAndSendCurrent,
                           true, shift);
    expectPreservedWithoutCopy();

    options.runtime.selectionClipboard.clearOnTyping = false;
    worker.applyRuntimeOptions(options.runtime);
    selectTarget();
    worker.stageSequenceKey(5, letter(u'j'));
    worker.resolveSequence(5,
                           TerminalSequenceResolution::FlushAndSendCurrent,
                           true, escape);
    expectClearedWithoutCopy();

    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::clearsSelectionForReportedMouseButtonsAndWheels()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    worker.resizeTerminal(24, 4, 8, 16, 192, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy selectionSpy(&worker,
                            &SessionWorker::selectionAvailableChanged);
    QSignalSpy mouseSpy(&worker, &SessionWorker::mouseTrackingChanged);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "stty -echo; "
            "printf 'mouse-selection-target\\r\\n\\033[?9hmouse-ready'; "
            "IFS= read -r _; "
            "printf '\\033[?9lmouse-off-ready'; "
            "exec cat >/dev/null"),
    };
    options.hold = true;
    options.runtime.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::Disabled;
    worker.initialize(options);

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("mouse-ready")), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(spyContainsBool(mouseSpy, true), 1000);

    const auto selectTarget = [&] {
        worker.clearSelection();
        selectionSpy.clear();
        worker.beginSelection(selectionPress(0, 0));
        worker.updateSelection(selectionDrag(7, 0));
        QVERIFY(spyContainsBool(selectionSpy, true));
        selectionSpy.clear();
    };
    const TerminalMouseInput motion{
        .action = TerminalMouseInput::Motion,
        .button = 1,
        .x = 8.0F,
        .y = 8.0F,
        .anyButtonPressed = true,
    };
    const TerminalMouseInput press{
        .action = TerminalMouseInput::Press,
        .button = 1,
        .x = 8.0F,
        .y = 8.0F,
        .anyButtonPressed = true,
    };
    const TerminalMouseInput wheel{
        .action = TerminalMouseInput::Press,
        .button = 4,
        .x = 8.0F,
        .y = 8.0F,
    };

    // A captured fractional wheel has no protocol event yet, but its
    // selection side effect still runs only after this worker-owned DEC-state
    // check.
    selectTarget();
    worker.clearSelectionIfMouseTracking();
    QVERIFY(spyContainsBool(selectionSpy, false));
    selectionSpy.clear();
    worker.updateSelection(selectionDrag(10, 0));
    QVERIFY(spyContainsBool(selectionSpy, true));

    // Cursor motion never clears a selection, but an encoded button event
    // and a protocol wheel press do so before their PTY bytes are queued. A
    // reported button also discards repeat-click history: the later timed
    // press must remain a single click rather than selecting its word.
    worker.clearSelection();
    selectionSpy.clear();
    worker.beginSelection(selectionPress(0, 0, 1'000'000'000));
    worker.updateSelection(selectionDrag(7, 0));
    QVERIFY(spyContainsBool(selectionSpy, true));
    selectionSpy.clear();
    worker.sendMouse(motion);
    QVERIFY(!spyContainsBool(selectionSpy, false));
    worker.sendMouse(press);
    QVERIFY(spyContainsBool(selectionSpy, false));
    selectionSpy.clear();
    worker.updateSelection(selectionDrag(10, 0));
    QVERIFY(!spyContainsBool(selectionSpy, true));
    worker.beginSelection(selectionPress(0, 0, 1'100'000'000));
    QVERIFY(!spyContainsBool(selectionSpy, true));
    worker.endSelection(0, 0);

    // X10 consumes wheel routing but intentionally encodes no wheel bytes.
    // Surface.scrollCallback clears the range without resetting the physical
    // drag gesture, even when X10 ultimately emits no wheel bytes.
    selectTarget();
    worker.sendMouse(wheel);
    QVERIFY(spyContainsBool(selectionSpy, false));
    selectionSpy.clear();
    worker.updateSelection(selectionDrag(10, 0));
    QVERIFY(spyContainsBool(selectionSpy, true));

    // Read-only suppresses only the PTY write. Terminal-local selection clear,
    // gesture reset, and encoder bookkeeping still occur first.
    selectTarget();
    worker.setReadOnly(true);
    worker.sendMouse(press);
    QVERIFY(spyContainsBool(selectionSpy, false));
    selectionSpy.clear();
    worker.updateSelection(selectionDrag(10, 0));
    QVERIFY(!spyContainsBool(selectionSpy, true));
    worker.setReadOnly(false);
    worker.clearSelection();

    // The queued fractional-wheel side effect must also honor a DEC mode
    // reset that reaches the worker before it. With capture disabled, the
    // same conditional request preserves the current selection.
    TerminalKeyInput enter;
    enter.key = Qt::Key_Return;
    enter.text = QStringLiteral("\r");
    enter.pressed = true;
    worker.sendKey(enter);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("mouse-off-ready")), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(spyContainsBool(mouseSpy, false), 1000);
    selectTarget();
    worker.clearSelectionIfMouseTracking();
    QVERIFY(!spyContainsBool(selectionSpy, false));

    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::convertsAlternateScreenWheelRowsAtomically()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    worker.resizeTerminal(32, 4, 8, 16, 256, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy selectionSpy(&worker, &SessionWorker::selectionAvailableChanged);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"), QStringLiteral("-c"),
        QStringLiteral(
            "stty raw -echo; "
            "printf 'wheel-primary-ready'; "
            "dd bs=1 count=1 of=/dev/null 2>/dev/null; "
            "printf '\\033[?1049halt-normal-ready\\r\\n'; "
            "payload=$(dd bs=1 count=6 2>/dev/null); "
            "printf '\\r\\nnormal-wheel:'; "
            "printf '%s' \"$payload\" | od -An -v -tx1 | tr -d ' \\n'; "
            "printf '\\033[?1h\\r\\nalt-application-ready'; "
            "payload=$(dd bs=1 count=3 2>/dev/null); "
            "printf '\\r\\napplication-wheel:'; "
            "printf '%s' \"$payload\" | od -An -v -tx1 | tr -d ' \\n'; "
            "dd bs=1 count=1 of=/dev/null 2>/dev/null; "
            "stty sane; printf '\\033[?1049l\\r\\nwheel-done\\r\\n'")};
    options.hold = true;
    options.runtime.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::Disabled;
    QVERIFY(worker.initialize(options));

    const auto selectVisibleText = [&] {
        worker.clearSelection();
        selectionSpy.clear();
        worker.beginSelection(selectionPress(0, 0));
        worker.updateSelection(selectionDrag(8, 0));
        QVERIFY(spyContainsBool(selectionSpy, true));
        selectionSpy.clear();
    };

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("wheel-primary-ready")), 5000);
    worker.sendRawText(QByteArrayLiteral("x"));
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("alt-normal-ready")), 5000);
    selectVisibleText();
    worker.sendWheel({
        .columns = 1,
        .modifiers = Qt::AltModifier,
        .x = 12.0F,
        .y = 20.0F,
        .mouseReportingEnabled = false,
    });
    // DECSET 1007 owns and discards horizontal-only travel without applying
    // the vertical alternate-scroll selection side effect.
    QVERIFY(!spyContainsBool(selectionSpy, false));
    worker.sendWheel({
        .rows = 2,
        .columns = -3,
        .modifiers = Qt::ShiftModifier,
        .x = 12.0F,
        .y = 20.0F,
        .mouseReportingEnabled = false,
    });
    QVERIFY(spyContainsBool(selectionSpy, false));
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("normal-wheel:1b5b411b5b41")),
        5000);

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("alt-application-ready")),
        1000);
    selectVisibleText();
    worker.setReadOnly(true);
    worker.sendWheel({
        .rows = -1,
        .mouseReportingEnabled = true,
    });
    // Alternate-scroll selection semantics run before the ordinary input
    // write boundary even though read-only suppresses the cursor-key bytes.
    QVERIFY(spyContainsBool(selectionSpy, false));
    QTest::qWait(100);
    QVERIFY(!updatesContain(updateSpy, QStringLiteral("application-wheel:")));

    worker.setReadOnly(false);
    worker.sendWheel({
        .rows = -1,
        .mouseReportingEnabled = true,
    });
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("application-wheel:1b4f42")),
        5000);
    worker.sendRawText(QByteArrayLiteral("x"));
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("wheel-done")), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::reportsHorizontalWheelButtonsInStableOrder()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    worker.resizeTerminal(32, 4, 8, 16, 256, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy selectionSpy(&worker, &SessionWorker::selectionAvailableChanged);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"), QStringLiteral("-c"),
        QStringLiteral(
            "stty raw -echo; "
            "printf '\\033[?1002h\\033[?1006hwheel-report-ready'; "
            "payload=$(dd bs=1 count=30 2>/dev/null); "
            "printf '\\033[?1002l\\033[?1006l\\r\\nwheel-report:'; "
            "printf '%s' \"$payload\" | od -An -v -tx1 | tr -d ' \\n'; "
            "dd bs=1 count=1 of=/dev/null 2>/dev/null; "
            "stty sane; printf '\\r\\nwheel-report-done\\r\\n'")};
    options.hold = true;
    options.runtime.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::Disabled;
    QVERIFY(worker.initialize(options));

    const auto selectVisibleText = [&] {
        worker.clearSelection();
        selectionSpy.clear();
        worker.beginSelection(selectionPress(0, 0));
        worker.updateSelection(selectionDrag(8, 0));
        QVERIFY(spyContainsBool(selectionSpy, true));
        selectionSpy.clear();
    };

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("wheel-report-ready")), 5000);

    // A pane-disabled report ignores horizontal movement locally and keeps
    // selection state even though the terminal's raw DEC mode is active.
    selectVisibleText();
    worker.sendWheel({
        .columns = 1,
        .mouseReportingEnabled = false,
    });
    QVERIFY(!spyContainsBool(selectionSpy, false));

    // Read-only suppresses only bytes: reported horizontal wheel selection
    // semantics still run before the queue policy boundary.
    worker.setReadOnly(true);
    worker.sendWheel({
        .columns = 1,
        .mouseReportingEnabled = true,
    });
    QVERIFY(spyContainsBool(selectionSpy, false));
    worker.setReadOnly(false);

    // A diagonal request reports all vertical events before horizontal ones.
    // Positive Y is button 4 and negative X is button 7.
    worker.sendWheel({
        .rows = 2,
        .columns = -1,
        .x = 0.0F,
        .y = 0.0F,
        .mouseReportingEnabled = true,
    });
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy,
                       QStringLiteral("wheel-report:"
                                      "1b5b3c36343b313b314d"
                                      "1b5b3c36343b313b314d"
                                      "1b5b3c36373b313b314d")),
        5000);
    worker.sendRawText(QByteArrayLiteral("x"));
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("wheel-report-done")), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::copiesSelectionWithRuntimeFormattingAndAtomicClear()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalClipboardDestination>();
    qRegisterMetaType<TerminalActionResult>();
    SessionWorker worker;
    worker.resizeTerminal(16, 4, 8, 16, 128, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy clipboardSpy(&worker, &SessionWorker::clipboardTextReady);
    QSignalSpy actionSpy(
        &worker, &SessionWorker::terminalActionFinished);
    QSignalSpy selectionSpy(&worker,
                            &SessionWorker::selectionAvailableChanged);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("printf 'abc   \\r\\ncopy-ready'; sleep 5"),
    };
    options.hold = true;
    options.runtime.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::Disabled;
    worker.initialize(options);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("copy-ready")), 5000);

    worker.beginSelection(selectionPress(0, 0));
    worker.updateSelection(selectionDrag(6, 0));
    worker.endSelection(6, 0);
    QCOMPARE(clipboardSpy.count(), 0);
    QVERIFY(spyContainsBool(selectionSpy, true));

    worker.copySelection();
    QCOMPARE(clipboardSpy.count(), 1);
    QCOMPARE(clipboardSpy.constFirst().at(0).toString(),
             QStringLiteral("abc"));
    QCOMPARE(qvariant_cast<TerminalClipboardDestination>(
                 clipboardSpy.constFirst().at(1)),
             TerminalClipboardDestination::Standard);
    QVERIFY(!spyContainsBool(selectionSpy, false));

    options.runtime.selectionClipboard.trimTrailingSpaces = false;
    options.runtime.selectionClipboard.clearOnCopy = true;
    options.runtime.selectionClipboard.codepointMap = {
        {.first = 'a', .last = 'a', .replacement = QStringLiteral("A!")},
        {.first = 'c', .last = 'c', .replacement = QString{}},
        {.first = ' ', .last = ' ', .replacement = quint32{'_'}},
    };
    worker.applyRuntimeOptions(options.runtime);
    clipboardSpy.clear();
    selectionSpy.clear();
    updateSpy.clear();
    QStringList lifecycle;
    const QMetaObject::Connection completedConnection = connect(
        &worker, &SessionWorker::terminalActionFinished, &worker,
        [&lifecycle](const TerminalActionResult &) {
            lifecycle.append(QStringLiteral("completed"));
        });
    const QMetaObject::Connection selectionConnection = connect(
        &worker, &SessionWorker::selectionAvailableChanged, &worker,
        [&lifecycle](bool available) {
            lifecycle.append(available ? QStringLiteral("selected")
                                       : QStringLiteral("cleared"));
        });

    worker.copySelectionAction(909);
    QCOMPARE(actionSpy.count(), 1);
    QCOMPARE(clipboardSpy.count(), 0);
    const TerminalActionResult copiedResult =
        terminalActionResultAt(actionSpy, 0);
    QCOMPARE(copiedResult.requestId, quint64{909});
    QCOMPARE(copiedResult.outcome, TerminalActionOutcome::Success);
    QCOMPARE(copiedResult.effect, TerminalActionEffect::Clipboard);
    QVERIFY(copiedResult.performed);
    QCOMPARE(copiedResult.payload, QStringLiteral("A!b___"));
    QCOMPARE(copiedResult.clipboardDestination,
             TerminalClipboardDestination::Standard);
    QCOMPARE(lifecycle,
             QStringList({QStringLiteral("cleared"),
                          QStringLiteral("completed")}));
    QVERIFY(spyContainsBool(selectionSpy, false));
    QTRY_VERIFY_WITH_TIMEOUT(!updateSpy.isEmpty(), 1000);

    worker.copySelectionAction(1'010);
    QCOMPARE(actionSpy.count(), 2);
    QCOMPARE(clipboardSpy.count(), 0);
    const TerminalActionResult unavailableResult =
        terminalActionResultAt(actionSpy, 1);
    QCOMPARE(unavailableResult.requestId, quint64{1'010});
    QCOMPARE(unavailableResult.outcome,
             TerminalActionOutcome::Unavailable);
    QCOMPARE(unavailableResult.effect, TerminalActionEffect::None);
    QVERIFY(!unavailableResult.performed);
    QVERIFY(unavailableResult.payload.isEmpty());
    QCOMPARE(unavailableResult.clipboardDestination,
             TerminalClipboardDestination::Standard);

    disconnect(completedConnection);
    disconnect(selectionConnection);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();

    worker.copySelectionAction(1'111);
    QCOMPARE(actionSpy.count(), 3);
    const TerminalActionResult failedResult =
        terminalActionResultAt(actionSpy, 2);
    QCOMPARE(failedResult.requestId, quint64{1'111});
    QCOMPARE(failedResult.outcome, TerminalActionOutcome::Failed);
    QCOMPARE(failedResult.effect, TerminalActionEffect::None);
    QVERIFY(!failedResult.performed);
    QVERIFY(failedResult.payload.isEmpty());
    QCOMPARE(failedResult.clipboardDestination,
             TerminalClipboardDestination::Standard);
}

void SessionWorkerTest::resolvesConfiguredRightClickActions()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalRightClickResult>();
    qRegisterMetaType<TerminalClipboardDestination>();
    SessionWorker worker;
    worker.resizeTerminal(40, 4, 8, 16, 320, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy rightClickSpy(&worker, &SessionWorker::rightClickFinished);
    QSignalSpy clipboardSpy(&worker, &SessionWorker::clipboardTextReady);
    QSignalSpy selectionSpy(&worker, &SessionWorker::selectionAvailableChanged);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "printf 'alpha beta \\033]8;;https://osc.test\\033\\\\LINK\\033]8;;\\033\\\\ https://example.test\\r\\nright-ready'"),
    };
    options.hold = true;
    options.runtime.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::Disabled;
    worker.initialize(options);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("right-ready")), 5000);
    // Let a split PTY read publish its final revision before using that
    // retained GUI-frame revision for correlated coordinate operations.
    QTest::qWait(50);
    quint64 contentRevision = accumulatedFrame(updateSpy).contentRevision;
    QVERIFY(contentRevision != 0);
    const auto refreshContentRevision = [&] {
        QTest::qWait(25);
        contentRevision = accumulatedFrame(updateSpy).contentRevision;
    };

    const auto selectRange = [&worker](int start, int end) {
        worker.clearSelection();
        worker.beginSelection(selectionPress(start, 0));
        worker.updateSelection(selectionDrag(end + 1, 0));
        worker.endSelection(end + 1, 0);
    };
    quint64 nextRequestId = 1;
    const auto resolve = [&](int column, int modifiers = 0,
                             std::optional<quint64> revision = std::nullopt,
                             bool shiftBypassedMouseCapture = false) {
        const qsizetype previous = rightClickSpy.size();
        worker.resolveRightClick({
            .requestId = nextRequestId++,
            .contentRevision = revision.value_or(contentRevision),
            .column = column,
            .row = 0,
            .modifiers = modifiers,
            .shiftBypassedMouseCapture = shiftBypassedMouseCapture,
        });
        return rightClickResultAt(rightClickSpy, previous);
    };
    const auto applyAction = [&](RightClickAction action) {
        options.runtime.rightClickAction = action;
        worker.applyRuntimeOptions(options.runtime);
    };

    selectRange(0, 4);
    applyAction(RightClickAction::Ignore);
    TerminalRightClickResult result = resolve(20);
    QCOMPARE(result.effect, TerminalRightClickEffect::None);
    QVERIFY(result.selectionAvailable);
    worker.copySelection();
    QCOMPARE(clipboardSpy.constLast().at(0).toString(),
             QStringLiteral("alpha"));

    clipboardSpy.clear();
    selectionSpy.clear();
    applyAction(RightClickAction::Copy);
    result = resolve(20);
    QCOMPARE(result.effect, TerminalRightClickEffect::None);
    QVERIFY(!result.selectionAvailable);
    QCOMPARE(clipboardSpy.size(), 1);
    QCOMPARE(clipboardSpy.constFirst().at(0).toString(),
             QStringLiteral("alpha"));
    QCOMPARE(qvariant_cast<TerminalClipboardDestination>(
                 clipboardSpy.constFirst().at(1)),
             TerminalClipboardDestination::Standard);
    QVERIFY(spyContainsBool(selectionSpy, false));

    selectRange(6, 9);
    clipboardSpy.clear();
    applyAction(RightClickAction::CopyOrPaste);
    result = resolve(2);
    QCOMPARE(result.effect, TerminalRightClickEffect::None);
    QVERIFY(!result.selectionAvailable);
    QCOMPARE(clipboardSpy.size(), 1);
    QCOMPARE(clipboardSpy.constFirst().at(0).toString(),
             QStringLiteral("beta"));
    QCOMPARE(qvariant_cast<TerminalClipboardDestination>(
                 clipboardSpy.constFirst().at(1)),
             TerminalClipboardDestination::Standard);

    clipboardSpy.clear();
    result = resolve(2);
    QCOMPARE(result.effect, TerminalRightClickEffect::Paste);
    QVERIFY(!result.selectionAvailable);
    QVERIFY(clipboardSpy.isEmpty());

    selectRange(6, 9);
    selectionSpy.clear();
    applyAction(RightClickAction::Paste);
    result = resolve(2);
    QCOMPARE(result.effect, TerminalRightClickEffect::Paste);
    QVERIFY(!result.selectionAvailable);
    QVERIFY(spyContainsBool(selectionSpy, false));

    clipboardSpy.clear();
    applyAction(RightClickAction::ContextMenu);
    refreshContentRevision();
    result = resolve(6);
    QCOMPARE(result.effect, TerminalRightClickEffect::ContextMenu);
    QCOMPARE(result.contentRevision, contentRevision);
    QVERIFY(result.selectionAvailable);
    worker.copySelection();
    QCOMPARE(clipboardSpy.constLast().at(0).toString(), QStringLiteral("beta"));

    selectRange(0, 9);
    refreshContentRevision();
    clipboardSpy.clear();
    result = resolve(2);
    QCOMPARE(result.effect, TerminalRightClickEffect::ContextMenu);
    QVERIFY(result.selectionAvailable);
    worker.copySelection();
    QCOMPARE(clipboardSpy.constLast().at(0).toString(),
             QStringLiteral("alpha beta"));

    // A blank context click cannot derive a replacement word and therefore
    // leaves the existing selection intact.
    selectRange(0, 4);
    refreshContentRevision();
    clipboardSpy.clear();
    result = resolve(39);
    QVERIFY(result.selectionAvailable);
    worker.copySelection();
    QCOMPARE(clipboardSpy.constLast().at(0).toString(),
             QStringLiteral("alpha"));

    worker.clearSelection();
    clipboardSpy.clear();
    result =
        resolve(20, static_cast<int>(Qt::ControlModifier | Qt::ShiftModifier),
                std::nullopt, true);
    QVERIFY(result.selectionAvailable);
    worker.copySelection();
    QCOMPARE(clipboardSpy.constLast().at(0).toString(),
             QStringLiteral("https://example.test"));

    // OSC 8 remains a context-link candidate when link-url disables the
    // default regex matcher, and upstream selects the exact clicked cell.
    worker.clearSelection();
    options.runtime.linkUrl = false;
    worker.applyRuntimeOptions(options.runtime);
    clipboardSpy.clear();
    result = resolve(11, static_cast<int>(Qt::ControlModifier));
    QVERIFY(result.selectionAvailable);
    worker.copySelection();
    QCOMPARE(clipboardSpy.constLast().at(0).toString(), QStringLiteral("L"));

    worker.clearSelection();
    result = resolve(2, 0, contentRevision - 1);
    QCOMPARE(result.effect, TerminalRightClickEffect::ContextMenu);
    QVERIFY(!result.selectionAvailable);

    options.runtime.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::Primary;
    options.runtime.selectionClipboard.codepointMap = {
        {.first = 'a', .last = 'a', .replacement = quint32{'A'}},
        {.first = '-', .last = '-', .replacement = QString::fromUtf8("→")},
    };
    worker.applyRuntimeOptions(options.runtime);
    clipboardSpy.clear();
    result = resolve(6);
    QVERIFY(result.selectionAvailable);
    QCOMPARE(clipboardSpy.size(), 1);
    QCOMPARE(clipboardSpy.constFirst().at(0).toString(),
             QStringLiteral("betA"));
    QCOMPARE(qvariant_cast<TerminalClipboardDestination>(
                 clipboardSpy.constFirst().at(1)),
             TerminalClipboardDestination::Primary);

    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::autoCopiesOnlyCommittedSelectionsAndSelectAll()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalClipboardDestination>();
    qRegisterMetaType<TerminalActionResult>();
    SessionWorker worker;
    worker.resizeTerminal(16, 4, 8, 16, 128, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy clipboardSpy(&worker, &SessionWorker::clipboardTextReady);
    QSignalSpy selectionSpy(&worker,
                            &SessionWorker::selectionAvailableChanged);
    QSignalSpy selectAllSpy(&worker, &SessionWorker::selectAllCompleted);
    QSignalSpy actionSpy(
        &worker, &SessionWorker::terminalActionFinished);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("printf 'auto-copy\\r\\nauto-ready'; sleep 5"),
    };
    options.hold = true;
    options.runtime.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::Disabled;
    options.runtime.selectionClipboard.clearOnCopy = true;
    worker.initialize(options);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("auto-ready")), 5000);

    worker.beginSelection(selectionPress(0, 0));
    worker.updateSelection(selectionDrag(9, 0));
    QCOMPARE(clipboardSpy.count(), 0);
    worker.endSelection(9, 0);
    QCOMPARE(clipboardSpy.count(), 0);
    QVERIFY(spyContainsBool(selectionSpy, true));

    worker.clearSelection();
    selectionSpy.clear();
    options.runtime.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::Primary;
    options.runtime.selectionClipboard.codepointMap = {
        {.first = 'a', .last = 'a', .replacement = quint32{'A'}},
        {.first = '-', .last = '-', .replacement = QString::fromUtf8("→")},
    };
    worker.applyRuntimeOptions(options.runtime);
    QCOMPARE(clipboardSpy.count(), 0);

    worker.beginSelection(selectionPress(0, 0));
    worker.updateSelection(selectionDrag(9, 0));
    QCOMPARE(clipboardSpy.count(), 0);
    worker.endSelection(9, 0);
    QCOMPARE(clipboardSpy.count(), 1);
    QCOMPARE(clipboardSpy.constFirst().at(0).toString(),
             QString::fromUtf8("Auto→copy"));
    QCOMPARE(qvariant_cast<TerminalClipboardDestination>(
                 clipboardSpy.constFirst().at(1)),
             TerminalClipboardDestination::Primary);
    QVERIFY(spyContainsBool(selectionSpy, true));
    QVERIFY(!spyContainsBool(selectionSpy, false));

    clipboardSpy.clear();
    selectionSpy.clear();
    worker.beginSelection(selectionPress(0, 1));
    QVERIFY(spyContainsBool(selectionSpy, false));
    worker.endSelection(0, 1);
    QCOMPARE(clipboardSpy.count(), 0);
    selectionSpy.clear();

    options.runtime.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::PrimaryAndClipboard;
    worker.applyRuntimeOptions(options.runtime);
    QCOMPARE(clipboardSpy.count(), 0);

    worker.selectAllAction(1'212);
    QCOMPARE(selectAllSpy.count(), 1);
    QVERIFY(selectAllSpy.constFirst().constFirst().toBool());
    QCOMPARE(clipboardSpy.count(), 0);
    QCOMPARE(actionSpy.count(), 1);
    const TerminalActionResult selectAllResult =
        terminalActionResultAt(actionSpy, 0);
    QCOMPARE(selectAllResult.requestId, quint64{1'212});
    QCOMPARE(selectAllResult.outcome, TerminalActionOutcome::Success);
    QCOMPARE(selectAllResult.effect, TerminalActionEffect::Clipboard);
    QVERIFY(selectAllResult.performed);
    QVERIFY(selectAllResult.payload.contains(QString::fromUtf8("Auto→copy")));
    QVERIFY(!selectAllResult.payload.contains(QStringLiteral("auto-copy")));
    QCOMPARE(selectAllResult.clipboardDestination,
             TerminalClipboardDestination::PrimaryAndStandard);
    QVERIFY(spyContainsBool(selectionSpy, true));
    QVERIFY(!spyContainsBool(selectionSpy, false));
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::retainsSelectionAvailabilityOutsideViewport()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy selectionSpy(&worker,
                            &SessionWorker::selectionAvailableChanged);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "i=0; while [ $i -lt 100 ]; do printf 'row-%03d\\n' $i; "
            "i=$((i + 1)); done; sleep 5"),
    };
    options.hold = true;
    worker.initialize(options);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("row-099")), 5000);

    worker.beginSelection(selectionPress(0, 20));
    worker.updateSelection(selectionDrag(6, 20));
    worker.endSelection(6, 20);
    QTRY_VERIFY_WITH_TIMEOUT(spyContainsBool(selectionSpy, true), 1000);

    selectionSpy.clear();
    worker.scrollViewport({
        .kind = TerminalViewportRequest::Kind::Delta,
        .delta = -50,
    });
    QTest::qWait(100);
    QVERIFY(!spyContainsBool(selectionSpy, false));

    worker.clearSelection();
    QVERIFY(spyContainsBool(selectionSpy, false));
    worker.shutdown();
}

void SessionWorkerTest::continuouslyAutoscrollsSelectionAtEdges()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    worker.resizeTerminal(16, 3, 8, 16, 128, 48);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy selectionSpy(&worker, &SessionWorker::selectionAvailableChanged);
    QSignalSpy clipboardSpy(&worker, &SessionWorker::clipboardTextReady);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "i=0; while [ $i -lt 40 ]; do printf 'edge-row-%03d\\n' $i; "
            "i=$((i + 1)); done; printf 'edge-ready'; sleep 5"),
    };
    options.hold = true;
    QVERIFY(worker.initialize(options));
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("edge-ready")), 5000);

    QTimer *const timer =
        worker.findChild<QTimer *>(QStringLiteral("selectionAutoscrollTimer"));
    QVERIFY(timer != nullptr);
    QCOMPARE(timer->interval(), 15);
    QCOMPARE(timer->timerType(), Qt::PreciseTimer);
    // Invoke exact ticks below without a concurrent production timeout.
    timer->setInterval(60'000);

    worker.scrollViewport({
        .kind = TerminalViewportRequest::Kind::Row,
        .row = 10,
    });
    QTRY_COMPARE_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset,
                              quint64{10}, 1000);

    worker.beginSelection(selectionPress(4, 1));
    TerminalSelectionDragInput edge = selectionDrag(4, 0);
    edge.surfaceY = 0.0;
    worker.updateSelection(edge);
    QVERIFY(timer->isActive());
    QTRY_VERIFY_WITH_TIMEOUT(spyContainsBool(selectionSpy, true), 1000);

    QVERIFY(QMetaObject::invokeMethod(&worker, "selectionAutoscrollTick",
                                      Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset,
                              quint64{9}, 1000);
    QVERIFY(timer->isActive());

    QVERIFY(QMetaObject::invokeMethod(&worker, "selectionAutoscrollTick",
                                      Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset,
                              quint64{8}, 1000);

    // Returning to the interior stops immediately. A queued or manually
    // invoked stale timeout must not move the viewport.
    worker.updateSelection(selectionDrag(4, 1));
    QVERIFY(!timer->isActive());
    QVERIFY(QMetaObject::invokeMethod(&worker, "selectionAutoscrollTick",
                                      Qt::DirectConnection));
    QTest::qWait(20);
    QCOMPARE(accumulatedFrame(updateSpy).scrollOffset, quint64{8});

    worker.updateSelection(edge);
    QVERIFY(timer->isActive());
    worker.cancelSelectionGesture();
    QVERIFY(!timer->isActive());
    worker.copySelection();
    QCOMPARE(clipboardSpy.count(), 1);
    QVERIFY(!clipboardSpy.constFirst().constFirst().toString().isEmpty());
    QVERIFY(QMetaObject::invokeMethod(&worker, "selectionAutoscrollTick",
                                      Qt::DirectConnection));
    QTest::qWait(20);
    QCOMPARE(accumulatedFrame(updateSpy).scrollOffset, quint64{8});
    worker.updateSelection(edge);
    QVERIFY(!timer->isActive());

    worker.beginSelection(selectionPress(4, 1));
    worker.updateSelection(edge);
    QVERIFY(timer->isActive());
    worker.endSelection(edge.column, edge.row);
    QVERIFY(!timer->isActive());

    // A delayed Shift press extends the retained gesture inside beginSelection
    // itself. It must arm the same timer even when there is no intervening
    // mouse-move event.
    constexpr quint64 firstPressNanoseconds = 1'000'000'000;
    worker.beginSelection(selectionPress(2, 1, firstPressNanoseconds));
    worker.updateSelection(selectionDrag(6, 1));
    worker.endSelection(6, 1);
    TerminalSelectionPressInput shiftEdge =
        selectionPress(2, 0, firstPressNanoseconds + 600'000'000, false, true);
    shiftEdge.surfaceY = 0.0;
    worker.beginSelection(shiftEdge);
    QVERIFY(timer->isActive());
    worker.endSelection(shiftEdge.column, shiftEdge.row);
    QVERIFY(!timer->isActive());

    worker.beginSelection(selectionPress(4, 1));
    worker.updateSelection(edge);
    QVERIFY(timer->isActive());
    worker.resetTerminal();
    QVERIFY(!timer->isActive());

    worker.shutdown();
    QVERIFY(!timer->isActive());
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
}

void SessionWorkerTest::routesTypedViewportAndSelectionOperations()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy selectionSpy(&worker,
                            &SessionWorker::selectionAvailableChanged);
    QSignalSpy selectAllCompletedSpy(&worker,
                                     &SessionWorker::selectAllCompleted);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "i=0; while [ $i -lt 40 ]; do printf 'typed-row-%03d\\n' $i; "
            "i=$((i + 1)); done; sleep 5"),
    };
    options.hold = true;
    worker.initialize(options);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("typed-row-039")), 5000);

    worker.selectAll();
    QCOMPARE(selectAllCompletedSpy.count(), 1);
    QCOMPARE(selectAllCompletedSpy.constFirst().constFirst().toBool(), true);
    QTRY_VERIFY_WITH_TIMEOUT(spyContainsBool(selectionSpy, true), 1000);

    worker.scrollViewport({
        .kind = TerminalViewportRequest::Kind::Top,
    });
    QTRY_VERIFY_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset == 0, 1000);

    worker.scrollViewport({
        .kind = TerminalViewportRequest::Kind::Row,
        .row = 10,
    });
    QTRY_VERIFY_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset == 10, 1000);

    worker.scrollViewport({
        .kind = TerminalViewportRequest::Kind::Delta,
        .delta = -3,
    });
    QTRY_VERIFY_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset == 7, 1000);

    worker.scrollViewport({
        .kind = TerminalViewportRequest::Kind::Bottom,
    });
    QTRY_VERIFY_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset > 10, 1000);

    worker.scrollViewport({
        .kind = TerminalViewportRequest::Kind::Selection,
    });
    QTRY_VERIFY_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset == 0, 1000);

    worker.adjustSelection(TerminalSelectionAdjustment::Left);
    QTRY_VERIFY_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset > 10, 1000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty() ? ""
                                : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::resolvesCorrelatedSelectionActions()
{
    qRegisterMetaType<TerminalActionResult>();
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    worker.resizeTerminal(32, 6, 8, 16, 256, 96);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy actionSpy(&worker, &SessionWorker::terminalActionFinished);
    QSignalSpy selectionSpy(&worker,
                            &SessionWorker::selectionAvailableChanged);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "printf 'selected text   \\r\\n'; "
            "i=0; while [ $i -lt 40 ]; do printf 'selection-row-%03d\\n' $i; "
            "i=$((i + 1)); done; printf 'selection-ready'; sleep 5"),
    };
    options.hold = true;
    worker.initialize(options);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("selection-ready")), 5000);

    worker.adjustSelectionAction(1'301, TerminalSelectionAdjustment::Left);
    worker.scrollToSelectionAction(1'302);
    worker.searchSelectionAction(1'303);
    QCOMPARE(actionSpy.count(), 3);
    for (qsizetype index = 0; index < 3; ++index) {
        const TerminalActionResult result =
            terminalActionResultAt(actionSpy, index);
        QCOMPARE(result.requestId, quint64{1'301} + index);
        QCOMPARE(result.outcome, TerminalActionOutcome::Unavailable);
        QCOMPARE(result.effect, TerminalActionEffect::None);
        QVERIFY(!result.performed);
        QVERIFY(result.payload.isEmpty());
    }

    worker.scrollViewport({
        .kind = TerminalViewportRequest::Kind::Top,
    });
    QTRY_VERIFY_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset == 0, 1000);
    worker.beginSelection(selectionPress(0, 0));
    worker.updateSelection(selectionDrag(16, 0));
    worker.endSelection(16, 0);
    QTRY_VERIFY_WITH_TIMEOUT(spyContainsBool(selectionSpy, true), 1000);

    worker.searchSelectionAction(1'304);
    worker.searchSelectionAction(1'305);
    QCOMPARE(actionSpy.count(), 5);
    for (qsizetype index = 3; index < 5; ++index) {
        const TerminalActionResult result =
            terminalActionResultAt(actionSpy, index);
        QCOMPARE(result.requestId, quint64{1'301} + index);
        QCOMPARE(result.outcome, TerminalActionOutcome::Success);
        QCOMPARE(result.effect, TerminalActionEffect::StartSearch);
        QVERIFY(result.performed);
        QCOMPARE(result.payload, QStringLiteral("selected text   "));
    }

    worker.adjustSelectionAction(1'306, TerminalSelectionAdjustment::Left);
    QCOMPARE(actionSpy.count(), 6);
    const TerminalActionResult adjusted =
        terminalActionResultAt(actionSpy, 5);
    QCOMPARE(adjusted.requestId, quint64{1'306});
    QCOMPARE(adjusted.outcome, TerminalActionOutcome::Success);
    QCOMPARE(adjusted.effect, TerminalActionEffect::None);
    QVERIFY(adjusted.performed);
    QVERIFY(adjusted.payload.isEmpty());

    worker.scrollViewport({
        .kind = TerminalViewportRequest::Kind::Bottom,
    });
    QTRY_VERIFY_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset > 10, 1000);
    worker.scrollToSelectionAction(1'307);
    QCOMPARE(actionSpy.count(), 7);
    const TerminalActionResult scrolled =
        terminalActionResultAt(actionSpy, 6);
    QCOMPARE(scrolled.requestId, quint64{1'307});
    QCOMPARE(scrolled.outcome, TerminalActionOutcome::Success);
    QCOMPARE(scrolled.effect, TerminalActionEffect::None);
    QVERIFY(scrolled.performed);
    QVERIFY(scrolled.payload.isEmpty());

    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();

    worker.adjustSelectionAction(1'308, TerminalSelectionAdjustment::Right);
    worker.scrollToSelectionAction(1'309);
    worker.searchSelectionAction(1'310);
    QCOMPARE(actionSpy.count(), 10);
    for (qsizetype index = 7; index < 10; ++index) {
        const TerminalActionResult result =
            terminalActionResultAt(actionSpy, index);
        QCOMPARE(result.requestId, quint64{1'301} + index);
        QCOMPARE(result.outcome, TerminalActionOutcome::Failed);
        QCOMPARE(result.effect, TerminalActionEffect::None);
        QVERIFY(!result.performed);
        QVERIFY(result.payload.isEmpty());
    }
}

void SessionWorkerTest::resetsTerminalStateAndWorkerCaches()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy mouseSpy(&worker, &SessionWorker::mouseTrackingChanged);
    QSignalSpy selectionSpy(&worker,
                            &SessionWorker::selectionAvailableChanged);
    QSignalSpy titleSpy(&worker, &SessionWorker::titleChanged);
    QSignalSpy directorySpy(&worker, &SessionWorker::currentDirectoryChanged);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "stty raw -echo; "
            "i=0; while [ $i -lt 40 ]; do "
            "printf 'reset-row-%03d\\r\\n' \"$i\"; i=$((i + 1)); done; "
            "printf '\\033]0;reset-worker-title\\007"
            "\\033]7;file://localhost/tmp/reset-worker-cwd\\007"
            "\\033[?1049h\\033[?1003hreset-alt-ready\\r\\n'; "
            "reset_byte=$(dd bs=1 count=1 2>/dev/null | "
            "od -An -v -tx1 | tr -d ' \\n'); "
            "stty sane; printf '\\r\\nreset-input:%s\\r\\n' \"$reset_byte\"; "
            "sleep 5")};
    options.hold = true;
    worker.initialize(options);

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("reset-alt-ready")), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(spyContainsBool(mouseSpy, true), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !titleSpy.isEmpty()
            && titleSpy.constLast().constFirst().toString()
                == QStringLiteral("reset-worker-title"),
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !directorySpy.isEmpty()
            && directorySpy.constLast().constFirst().toString()
                == QStringLiteral("/tmp/reset-worker-cwd"),
        1000);
    worker.selectAll();
    QTRY_VERIFY_WITH_TIMEOUT(spyContainsBool(selectionSpy, true), 1000);

    updateSpy.clear();
    mouseSpy.clear();
    selectionSpy.clear();
    titleSpy.clear();
    directorySpy.clear();
    worker.resetTerminal();

    QTRY_VERIFY_WITH_TIMEOUT(spyContainsBool(selectionSpy, false), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(spyContainsBool(mouseSpy, false), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(containsFullFrame(updateSpy), 1000);
    // Upstream reset has no application-title effect. The frontend retains
    // whichever base title was last published by OSC or set_surface_title.
    QVERIFY(titleSpy.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(
        !directorySpy.isEmpty()
            && directorySpy.constLast().constFirst().toString().isEmpty(),
        1000);
    const TerminalFrame resetFrame = accumulatedFrame(updateSpy);
    QVERIFY(!frameText(resetFrame).contains(QStringLiteral("reset-row")));
    QVERIFY(!frameText(resetFrame).contains(QStringLiteral("reset-alt-ready")));
    QCOMPARE(resetFrame.scrollOffset, quint64{0});
    QCOMPARE(resetFrame.scrollTotal, resetFrame.scrollLength);

    // A reset is an emulator-only mutation. A sentinel sent afterwards must
    // be the first byte observed by the child; any synthesized reset input
    // would displace it from this one-byte read.
    worker.sendRawText(QByteArrayLiteral("Z"));
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("reset-input:5a")), 1000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty() ? ""
                                : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::explicitProgramIsActiveForItsLifetime()
{
    SessionWorker worker;
    QSignalSpy activitySpy(&worker, &SessionWorker::activeProcessChanged);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("sleep 0.5"),
    };
    options.hold = true;
    worker.initialize(options);

    QTRY_VERIFY_WITH_TIMEOUT(spyContainsBool(activitySpy, true), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(exitSpy.count() > 0, 3000);
    QVERIFY(spyContainsBool(activitySpy, false));
    worker.shutdown();
}

void SessionWorkerTest::integratedShellStartupRemainsActiveUntilPrompt()
{
#if !GHOSTTY_QT_TEST_SHELL_INTEGRATION_ENABLED
    QSKIP("The pinned shell transformer is disabled with Ghostty config");
#else
    const QString bash = QStandardPaths::findExecutable(QStringLiteral("bash"));
    if (bash.isEmpty()) {
        QSKIP("bash is unavailable");
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString startupFile =
        temporary.filePath(QStringLiteral("delayed-startup.bash"));
    QVERIFY(writeExecutableScript(
        startupFile,
        QByteArrayLiteral(
            "IFS= read -r -t 1 _ghostty_qt_delay || :\n"
            "printf '\\033]133;A\\aintegrated-prompt \\033]133;B\\a'\n")));

    SessionWorker worker;
    QSignalSpy startedSpy(&worker, &SessionWorker::started);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy activitySpy(&worker, &SessionWorker::activeProcessChanged);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.command = TerminalCommand::shell(QFile::encodeName(bash), true);
    // The helper first installs Ghostty's Bash integration and reports that
    // semantic prompts are expected. The finalized env override then supplies
    // a deterministic same-process startup hook whose first prompt is delayed
    // beyond the ordinary input grace period.
    options.environment = {{
        .key = QByteArrayLiteral("ENV"),
        .value = QFile::encodeName(startupFile),
    }};
    options.shellIntegration = GhosttyShellIntegrationMode::Bash;
    options.shellIntegrationAvailable = true;
    options.hold = true;
    QVERIFY(worker.initialize(options));

    QTRY_VERIFY_WITH_TIMEOUT(startedSpy.count() > 0, 1'000);
    QVERIFY(spyContainsBool(activitySpy, true));
    QTest::qWait(450);
    QVERIFY(!activitySpy.isEmpty());
    QVERIFY(activitySpy.constLast().constFirst().toBool());
    QVERIFY(exitSpy.isEmpty());

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("integrated-prompt")), 2'000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !activitySpy.isEmpty()
            && !activitySpy.constLast().constFirst().toBool(),
        1'000);

    worker.sendRawText(QByteArrayLiteral("exit\n"));
    QTRY_VERIFY_WITH_TIMEOUT(!exitSpy.isEmpty(), 2'000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
#endif
}

void SessionWorkerTest::semanticPromptsTrackSameProcessShellActivity()
{
    qRegisterMetaType<TerminalUpdate>();

    SessionWorker worker;
    QSignalSpy startedSpy(&worker, &SessionWorker::started);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy activitySpy(&worker, &SessionWorker::activeProcessChanged);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    // Every phase runs in the original /bin/sh process group. Newlines from
    // the test advance the script's builtin reads, avoiding wall-clock races
    // while covering activity that tcgetpgrp alone cannot distinguish.
    const QByteArray script =
        QByteArrayLiteral("printf '\\033]133;A\\afirst-prompt \\033]133;B\\a'; "
                          "IFS= read -r phase; "
                          "printf '\\033]133;C\\a\\r\\nbuiltin-running\\r\\n'; "
                          "IFS= read -r phase; "
                          "printf '\\033]133;D;0\\a\\033]133;A\\asecond-prompt "
                          "\\033]133;B\\a'; "
                          "IFS= read -r phase; "
                          "printf '\\033[?1049halternate-running'; "
                          "IFS= read -r phase; "
                          "printf '\\033[?1049l'; "
                          "IFS= read -r phase");

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.command = TerminalCommand::shell(script, true);
    options.hold = true;
    QVERIFY(worker.initialize(options));

    QTRY_VERIFY_WITH_TIMEOUT(startedSpy.count() > 0, 1000);
    QVERIFY(spyContainsBool(activitySpy, true));
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("first-prompt")), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !activitySpy.isEmpty()
            && !activitySpy.constLast().constFirst().toBool(),
        1000);
    QVERIFY(exitSpy.isEmpty());

    activitySpy.clear();
    worker.sendRawText(QByteArrayLiteral("\n"));
    QVERIFY(spyContainsBool(activitySpy, true));
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("builtin-running")), 1000);
    // The input activity grace period is 250 ms. Remaining active beyond it
    // proves the latched semantic Away state, rather than the grace timer,
    // protects same-process-group shell work.
    QTest::qWait(400);
    QVERIFY(!activitySpy.isEmpty());
    QVERIFY(activitySpy.constLast().constFirst().toBool());
    QVERIFY(exitSpy.isEmpty());

    activitySpy.clear();
    worker.sendRawText(QByteArrayLiteral("\n"));
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("second-prompt")), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !activitySpy.isEmpty()
            && !activitySpy.constLast().constFirst().toBool(),
        1000);
    QVERIFY(exitSpy.isEmpty());

    activitySpy.clear();
    worker.sendRawText(QByteArrayLiteral("\n"));
    QVERIFY(spyContainsBool(activitySpy, true));
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("alternate-running")), 1000);
    QTest::qWait(400);
    QVERIFY(!activitySpy.isEmpty());
    QVERIFY(activitySpy.constLast().constFirst().toBool());
    QVERIFY(exitSpy.isEmpty());

    activitySpy.clear();
    worker.sendRawText(QByteArrayLiteral("\n"));
    QTRY_VERIFY_WITH_TIMEOUT(
        !activitySpy.isEmpty()
            && !activitySpy.constLast().constFirst().toBool(),
        1000);
    QVERIFY(exitSpy.isEmpty());
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::interactiveShellTracksForegroundJobs()
{
    const bool shellWasSet = qEnvironmentVariableIsSet("SHELL");
    const QByteArray previousShell = qgetenv("SHELL");
    struct ShellEnvironmentRestore {
        bool wasSet;
        QByteArray value;
        ~ShellEnvironmentRestore()
        {
            if (wasSet) {
                qputenv("SHELL", value);
            } else {
                qunsetenv("SHELL");
            }
        }
    } restore{shellWasSet, previousShell};
    qputenv("SHELL", QByteArrayLiteral("/bin/sh"));

    SessionWorker worker;
    QSignalSpy startedSpy(&worker, &SessionWorker::started);
    QSignalSpy activitySpy(&worker, &SessionWorker::activeProcessChanged);
    QSignalSpy unsafeSpy(
        &worker, &SessionWorker::unsafePasteConfirmationRequested);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.hold = true;
    options.runtime.clipboardPaste.bracketedSafe = false;
    worker.initialize(options);
    QTRY_VERIFY_WITH_TIMEOUT(startedSpy.count() > 0, 1000);

    // This shell deliberately emits no OSC 133 markers, retaining coverage
    // for the tcgetpgrp and short input-grace fallback path.
    // Give the shell time to take the foreground group and settle at its
    // prompt. Its live child alone is not close-confirmation-worthy.
    QTRY_VERIFY_WITH_TIMEOUT(
        !activitySpy.isEmpty()
            && !activitySpy.constLast().constFirst().toBool(),
        1000);
    activitySpy.clear();

    // Merely rejecting or cancelling an unsafe request is not process
    // activity. Confirmation applies the hint only when bytes are accepted.
    worker.paste(QStringLiteral("cancelled\n"));
    QCOMPARE(unsafeSpy.count(), 1);
    QVERIFY(!spyContainsBool(activitySpy, true));
    worker.cancelPaste(unsafeSpy.constLast().at(0).toULongLong());
    QTest::qWait(50);
    QVERIFY(!spyContainsBool(activitySpy, true));

    worker.paste(QStringLiteral("\n"));
    QCOMPARE(unsafeSpy.count(), 2);
    QVERIFY(!spyContainsBool(activitySpy, true));
    worker.confirmPaste(unsafeSpy.constLast().at(0).toULongLong());
    QVERIFY(spyContainsBool(activitySpy, true));
    QTRY_VERIFY_WITH_TIMEOUT(
        !activitySpy.isEmpty()
            && !activitySpy.constLast().constFirst().toBool(),
        3000);
    activitySpy.clear();

    TerminalKeyInput enter;
    enter.key = Qt::Key_Return;
    enter.pressed = true;

    // Rejected read-only input must not trip either the worker's grace timer
    // or its active-process signal, across every newline-aware input path.
    worker.setReadOnly(true);
    worker.sendKey(enter);
    worker.stageSequenceKey(1, enter);
    worker.resolveSequence(1, TerminalSequenceResolution::Flush, false, {});
    worker.sendInputMethod({.commitText = QStringLiteral("\n")});
    worker.sendRawText(QByteArrayLiteral(R"(\\n)"));
    worker.paste(QStringLiteral("\n"));
    QCOMPARE(unsafeSpy.count(), 3);
    worker.confirmPaste(unsafeSpy.constLast().at(0).toULongLong());
    QTest::qWait(50);
    QVERIFY(!spyContainsBool(activitySpy, true));

    // Staging is terminal-local and remains live. If read-only is lifted
    // before resolution, the accepted flush must regain normal activity.
    worker.stageSequenceKey(2, enter);
    worker.setReadOnly(false);
    worker.resolveSequence(2, TerminalSequenceResolution::Flush, false, {});
    QVERIFY(spyContainsBool(activitySpy, true));
    QTRY_VERIFY_WITH_TIMEOUT(
        !activitySpy.isEmpty()
            && !activitySpy.constLast().constFirst().toBool(),
        3000);
    activitySpy.clear();

    const QString command = QStringLiteral("sleep 1");
    for (const QChar character : command) {
        worker.sendInputMethod({.commitText = QString(character)});
    }
    worker.stageSequenceKey(3, enter);
    QTest::qWait(50);
    QVERIFY(!spyContainsBool(activitySpy, true));
    worker.resolveSequence(3, TerminalSequenceResolution::Drop, false, {});
    QVERIFY(!spyContainsBool(activitySpy, true));

    worker.stageSequenceKey(4, enter);
    QVERIFY(!spyContainsBool(activitySpy, true));
    worker.resolveSequence(4, TerminalSequenceResolution::Flush, false, {});
    // The activity hint is synchronous at flush, closing in the interval
    // before the next tcgetpgrp poll cannot lose foreground-job confirmation.
    QVERIFY(spyContainsBool(activitySpy, true));
    QTRY_VERIFY_WITH_TIMEOUT(
        !activitySpy.isEmpty()
            && !activitySpy.constLast().constFirst().toBool(),
        3000);

    // Keep the direct-key path covered independently of sequence staging.
    activitySpy.clear();
    for (const QChar character : command) {
        worker.sendInputMethod({.commitText = QString(character)});
    }
    worker.sendKey(enter);
    QVERIFY(spyContainsBool(activitySpy, true));
    QTRY_VERIFY_WITH_TIMEOUT(
        !activitySpy.isEmpty()
            && !activitySpy.constLast().constFirst().toBool(),
        3000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty() ? ""
                                : qPrintable(errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

QTEST_GUILESS_MAIN(SessionWorkerTest)

#include "test_session_worker.moc"
