#include "session_worker.h"
#include "terminal_types.h"

#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QTemporaryDir>
#include <QTimer>

#include <linux/input-event-codes.h>

#include <algorithm>
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
    void writesPersistentTerminalFiles();
    void skipsUnavailableTerminalFiles();
    void reportsTerminalInitializationSeparatelyFromChildSpawn();
    void reportsTerminalInitializationFailure();
    void initializesGeometryBeforeSpawningChild();
    void stripsDesktopActivationFromChildEnvironment();
    void fallsBackFromUnavailableWorkingDirectory_data();
    void fallsBackFromUnavailableWorkingDirectory();
    void preservesInheritedLogicalPwd();
    void preservesSymlinkSensitiveWorkingDirectory();
    void resolvesRelativePathEntriesFromChildWorkingDirectory_data();
    void resolvesRelativePathEntriesFromChildWorkingDirectory();
    void skipsEmptyPathEntries();
    void continuesPathLookupAfterMissingInterpreter();
    void usesPinnedDefaultPathWhenUnset();
    void drainsLargeFinalOutputBeforeClosingPty();
    void sendsBracketedPasteThroughPty();
    void protectsPasteWithCorrelatedWorkerConfirmation();
    void sendsTerminalControlActionsThroughPty();
    void pastesTerminalFilePathAsRawOrderedInput();
    void readOnlyBlocksSurfaceInputButPreservesProtocolReplies();
    void stagesAndResolvesSequenceBytes();
    void stagesSequenceKeysUsingModesAtStageTime();
    void appliesReloadedAppearanceToExistingTerminal();
    void clearsSelectionOnlyForUpstreamTypingPaths();
    void clearsSelectionForReportedMouseButtonsAndWheels();
    void copiesSelectionWithRuntimeFormattingAndAtomicClear();
    void autoCopiesOnlyCommittedSelectionsAndSelectAll();
    void retainsSelectionAvailabilityOutsideViewport();
    void routesTypedViewportAndSelectionOperations();
    void searchesIncrementallyAndNavigates();
    void preservesFormattedSearchBoundaries();
    void resetsTerminalStateAndWorkerCaches();
    void resolvesCorrelatedHyperlinkQueries();
    void resolvesRegexLinksAcrossUtf8WrapsAndOsc8Precedence();
    void retainsRegexHoverAcrossViewportScrolling();
    void revalidatesRegexActivationAcrossUnrelatedOutput();
    void explicitProgramIsActiveForItsLifetime();
    void interactiveShellTracksForegroundJobs();
};

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
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "stty raw -echo; "
            "printf '\\033[c'; "
            "response=$(dd bs=1 count=9 2>/dev/null); "
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
                 QStringLiteral("device-response:1b5b3f36323b323263")),
             qPrintable(finalContents));
    QVERIFY2(finalContents.contains(QStringLiteral("ghostty-qt-final")),
             qPrintable(finalContents));
    QVERIFY(containsCursorBlinkReset(updateSpy));
    worker.shutdown();
}

void SessionWorkerTest::writesPersistentTerminalFiles()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalClipboardDestination>();
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
        QSignalSpy clipboardSpy(
            &worker, &SessionWorker::clipboardTextReady);
        QSignalSpy openSpy(
            &worker, &SessionWorker::terminalFileOpenRequested);
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

        worker.writeTerminalFile({
            .location = TerminalFileLocation::Screen,
            .disposition = TerminalFileDisposition::Copy,
        });
        QCOMPARE(clipboardSpy.count(), 1);
        QCOMPARE(qvariant_cast<TerminalClipboardDestination>(
                     clipboardSpy.constFirst().at(1)),
                 TerminalClipboardDestination::Standard);
        screenPath = clipboardSpy.constFirst().at(0).toString();

        worker.writeTerminalFile({
            .location = TerminalFileLocation::Scrollback,
            .disposition = TerminalFileDisposition::Open,
        });
        QCOMPARE(openSpy.count(), 1);
        historyPath = openSpy.constFirst().constFirst().toString();

        worker.beginSelection(0, 0, 1, true);
        worker.updateSelection(8, 1, true);
        worker.endSelection(8, 1);
        worker.writeTerminalFile({
            .location = TerminalFileLocation::Selection,
            .disposition = TerminalFileDisposition::Copy,
        });
        QCOMPARE(clipboardSpy.count(), 2);
        selectionPath = clipboardSpy.at(1).constFirst().toString();

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
    qRegisterMetaType<TerminalClipboardDestination>();
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
    QSignalSpy clipboardSpy(
        &worker, &SessionWorker::clipboardTextReady);
    QSignalSpy openSpy(
        &worker, &SessionWorker::terminalFileOpenRequested);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory = controlDirectory.path();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    QVERIFY(worker.initialize(options));
    QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 5000);

    worker.writeTerminalFile({
        .location = TerminalFileLocation::Scrollback,
        .disposition = TerminalFileDisposition::Copy,
    });
    worker.writeTerminalFile({
        .location = TerminalFileLocation::Selection,
        .disposition = TerminalFileDisposition::Open,
    });

    QVERIFY(clipboardSpy.isEmpty());
    QVERIFY(openSpy.isEmpty());
    QVERIFY(QDir(artifactRoot)
                .entryList(QDir::Dirs | QDir::NoDotAndDotDot)
                .isEmpty());
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty()
                 ? ""
                 : qPrintable(
                       errorSpy.constFirst().constFirst().toString()));
    worker.shutdown();
}

void SessionWorkerTest::reportsTerminalInitializationSeparatelyFromChildSpawn()
{
    SessionWorker worker;
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    TerminalSessionLaunchOptions options;
    options.workingDirectory =
        QDir::current().filePath(QStringLiteral("tmp"));
    QVERIFY(QDir().mkpath(options.workingDirectory));
    options.program = {
        QStringLiteral("/ghostty-qt-test/nonexistent-child"),
    };
    options.hold = true;

    // Worker initialization is accepted as soon as libghostty-vt exists. A
    // later child-spawn failure is reported independently and does not turn the
    // accepted initialization into a false result.
    std::optional<bool> initializationResult;
    int errorsAtInitialization = -1;
    QVERIFY(worker.initialize(
        options, [&](bool initialized) {
            initializationResult = initialized;
            errorsAtInitialization = errorSpy.count();
        }));
    QCOMPARE(initializationResult, std::optional(true));
    QCOMPARE(errorsAtInitialization, 0);
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(errorSpy.constFirst().constFirst().toString().contains(
        QStringLiteral("Program is not executable")));
    QCOMPARE(exitSpy.count(), 1);
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 127);
    QCOMPARE(exitSpy.constFirst().at(1).toInt(), 0);
    QVERIFY(exitSpy.constFirst().at(2).toBool());

    // The existing terminal makes a repeated attempt ineligible. It must not
    // replay either the child diagnostic or the synthetic exit notification.
    initializationResult.reset();
    QVERIFY(!worker.initialize(
        options, [&](bool initialized) {
            initializationResult = initialized;
        }));
    QCOMPARE(initializationResult, std::optional(false));
    QCOMPARE(errorSpy.count(), 1);
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
        // proves forkpty receives the explicit surface pixel dimensions.
        .surfaceWidthPixels = 487,
        .surfaceHeightPixels = 337,
    };
    worker.initialize(options);

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
                 QStringLiteral(
                     "ghostty-qt-pty-geometry:43:17:487:337")),
             qPrintable(finalContents));
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
    QVERIFY2(contents.contains(QStringLiteral(
                 "xdg=unset startup=unset sentinel=preserved")),
             qPrintable(contents));
    worker.shutdown();
}

void SessionWorkerTest::fallsBackFromUnavailableWorkingDirectory_data()
{
    QTest::addColumn<bool>("existingNonDirectory");
    QTest::newRow("missing") << false;
    QTest::newRow("existing-non-directory") << true;
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
    worker.writeTerminalFile({
        .location = TerminalFileLocation::Screen,
        .disposition = TerminalFileDisposition::Paste,
    });
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
    worker.writeTerminalFile({
        .location = TerminalFileLocation::Screen,
        .disposition = TerminalFileDisposition::Paste,
    });
    artifactDirectories =
        QDir(artifactRoot).entryList(
            QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(artifactDirectories.size(), 2);
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
        worker.beginSelection(0, 0, 1, false);
        worker.updateSelection(7, 0, false);
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
        worker.beginSelection(0, 0, 1, false);
        worker.updateSelection(7, 0, false);
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

    // Cursor motion never clears a selection, but an encoded button event
    // and a protocol wheel press do so before their PTY bytes are queued.
    selectTarget();
    worker.sendMouse(motion);
    QVERIFY(!spyContainsBool(selectionSpy, false));
    worker.sendMouse(press);
    QVERIFY(spyContainsBool(selectionSpy, false));
    selectionSpy.clear();
    worker.updateSelection(10, 0, false);
    QVERIFY(!spyContainsBool(selectionSpy, true));

    // X10 consumes wheel routing but intentionally encodes no wheel bytes.
    // It still clears the range without resetting a physical drag gesture.
    selectTarget();
    worker.sendMouse(wheel);
    QVERIFY(spyContainsBool(selectionSpy, false));
    selectionSpy.clear();
    worker.updateSelection(10, 0, false);
    QVERIFY(spyContainsBool(selectionSpy, true));

    // Read-only suppresses only the PTY write. Terminal-local selection clear,
    // gesture reset, and encoder bookkeeping still occur first.
    selectTarget();
    worker.setReadOnly(true);
    worker.sendMouse(press);
    QVERIFY(spyContainsBool(selectionSpy, false));
    selectionSpy.clear();
    worker.updateSelection(10, 0, false);
    QVERIFY(!spyContainsBool(selectionSpy, true));
    worker.setReadOnly(false);
    worker.clearSelection();

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
    SessionWorker worker;
    worker.resizeTerminal(16, 4, 8, 16, 128, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy clipboardSpy(&worker, &SessionWorker::clipboardTextReady);
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

    worker.beginSelection(0, 0, 1, false);
    worker.updateSelection(6, 0, false);
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
    worker.applyRuntimeOptions(options.runtime);
    clipboardSpy.clear();
    selectionSpy.clear();
    updateSpy.clear();
    QStringList lifecycle;
    const QMetaObject::Connection copiedConnection = connect(
        &worker, &SessionWorker::clipboardTextReady, &worker,
        [&lifecycle](const QString &,
                     TerminalClipboardDestination) {
            lifecycle.append(QStringLiteral("copied"));
        });
    const QMetaObject::Connection selectionConnection = connect(
        &worker, &SessionWorker::selectionAvailableChanged, &worker,
        [&lifecycle](bool available) {
            lifecycle.append(available ? QStringLiteral("selected")
                                       : QStringLiteral("cleared"));
        });

    worker.copySelection();
    QCOMPARE(clipboardSpy.count(), 1);
    QCOMPARE(clipboardSpy.constFirst().at(0).toString(),
             QStringLiteral("abc   "));
    QCOMPARE(qvariant_cast<TerminalClipboardDestination>(
                 clipboardSpy.constFirst().at(1)),
             TerminalClipboardDestination::Standard);
    QCOMPARE(lifecycle,
             QStringList({QStringLiteral("copied"),
                          QStringLiteral("cleared")}));
    QVERIFY(spyContainsBool(selectionSpy, false));
    QTRY_VERIFY_WITH_TIMEOUT(!updateSpy.isEmpty(), 1000);

    worker.copySelection();
    QCOMPARE(clipboardSpy.count(), 1);
    disconnect(copiedConnection);
    disconnect(selectionConnection);
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
    SessionWorker worker;
    worker.resizeTerminal(16, 4, 8, 16, 128, 64);
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy clipboardSpy(&worker, &SessionWorker::clipboardTextReady);
    QSignalSpy selectionSpy(&worker,
                            &SessionWorker::selectionAvailableChanged);
    QSignalSpy selectAllSpy(&worker, &SessionWorker::selectAllCompleted);
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

    worker.beginSelection(0, 0, 1, false);
    worker.updateSelection(9, 0, false);
    QCOMPARE(clipboardSpy.count(), 0);
    worker.endSelection(9, 0);
    QCOMPARE(clipboardSpy.count(), 0);
    QVERIFY(spyContainsBool(selectionSpy, true));

    worker.clearSelection();
    selectionSpy.clear();
    options.runtime.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::Primary;
    worker.applyRuntimeOptions(options.runtime);
    QCOMPARE(clipboardSpy.count(), 0);

    worker.beginSelection(0, 0, 1, false);
    worker.updateSelection(9, 0, false);
    QCOMPARE(clipboardSpy.count(), 0);
    worker.endSelection(9, 0);
    QCOMPARE(clipboardSpy.count(), 1);
    QCOMPARE(clipboardSpy.constFirst().at(0).toString(),
             QStringLiteral("auto-copy"));
    QCOMPARE(qvariant_cast<TerminalClipboardDestination>(
                 clipboardSpy.constFirst().at(1)),
             TerminalClipboardDestination::Primary);
    QVERIFY(spyContainsBool(selectionSpy, true));
    QVERIFY(!spyContainsBool(selectionSpy, false));

    clipboardSpy.clear();
    selectionSpy.clear();
    worker.beginSelection(0, 1, 1, false);
    QVERIFY(spyContainsBool(selectionSpy, false));
    worker.endSelection(0, 1);
    QCOMPARE(clipboardSpy.count(), 0);
    selectionSpy.clear();

    options.runtime.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::PrimaryAndClipboard;
    worker.applyRuntimeOptions(options.runtime);
    QCOMPARE(clipboardSpy.count(), 0);

    worker.selectAll();
    QCOMPARE(selectAllSpy.count(), 1);
    QVERIFY(selectAllSpy.constFirst().constFirst().toBool());
    QCOMPARE(clipboardSpy.count(), 1);
    QVERIFY(clipboardSpy.constFirst().at(0).toString().contains(
        QStringLiteral("auto-copy")));
    QCOMPARE(qvariant_cast<TerminalClipboardDestination>(
                 clipboardSpy.constFirst().at(1)),
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

    worker.beginSelection(0, 20, 1, false);
    worker.updateSelection(6, 20, false);
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
