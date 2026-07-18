#include "launch_options.h"
#include "session_worker.h"
#include "terminal_types.h"

#include <QDir>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <optional>

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
        applyTerminalUpdate(
            &frame, qvariant_cast<TerminalUpdate>(arguments.constFirst()));
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

} // namespace

class SessionWorkerTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void runsCommandThroughPty();
    void drainsLargeFinalOutputBeforeClosingPty();
    void sendsBracketedPasteThroughPty();
    void sendsTerminalControlActionsThroughPty();
    void stagesAndResolvesSequenceBytes();
    void stagesSequenceKeysUsingModesAtStageTime();
    void appliesReloadedAppearanceToExistingTerminal();
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

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "i=0; while [ $i -lt 500 ]; do "
            "printf 'scan-row-%03d needle\\n' \"$i\"; "
            "i=$((i + 1)); done; "
            "printf 'AAAA\\nleft\\nright\\nÄ ä\\n'")};
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
    QCOMPARE(latestSearchUpdate(searchSpy, 1)->totalMatches, quint64(3));

    worker.navigateSearch(1, TerminalSearchDirection::Next);
    QCOMPARE(latestSearchUpdate(searchSpy, 1)->selectedMatch, qint64(0));
    worker.navigateSearch(1, TerminalSearchDirection::Previous);
    QCOMPARE(latestSearchUpdate(searchSpy, 1)->selectedMatch, qint64(2));
    worker.navigateSearch(1, TerminalSearchDirection::Next);
    QCOMPARE(latestSearchUpdate(searchSpy, 1)->selectedMatch, qint64(0));

    worker.search(2, QByteArrayLiteral("left\nright"));
    QTRY_VERIFY_WITH_TIMEOUT(
        latestSearchUpdate(searchSpy, 2).has_value()
            && latestSearchUpdate(searchSpy, 2)->complete,
        5000);
    QCOMPARE(latestSearchUpdate(searchSpy, 2)->totalMatches, quint64(1));

    worker.search(3, QStringLiteral("Ä").toUtf8());
    QTRY_VERIFY_WITH_TIMEOUT(
        latestSearchUpdate(searchSpy, 3).has_value()
            && latestSearchUpdate(searchSpy, 3)->complete,
        5000);
    QCOMPARE(latestSearchUpdate(searchSpy, 3)->totalMatches, quint64(1));

    worker.searchSerialized(4, QByteArrayLiteral("A\\x41"));
    QTRY_VERIFY_WITH_TIMEOUT(
        latestSearchUpdate(searchSpy, 4).has_value()
            && latestSearchUpdate(searchSpy, 4)->complete,
        5000);
    QCOMPARE(latestSearchUpdate(searchSpy, 4)->totalMatches, quint64(3));

    // The oldest match is wholly outside the live viewport. Previous starts
    // there, while Next wraps back to the newest result and follows it down.
    worker.search(5, QByteArrayLiteral("needle"));
    QTRY_VERIFY_WITH_TIMEOUT(
        latestSearchUpdate(searchSpy, 5).has_value()
            && latestSearchUpdate(searchSpy, 5)->complete,
        5000);
    QCOMPARE(latestSearchUpdate(searchSpy, 5)->totalMatches, quint64(500));
    worker.navigateSearch(5, TerminalSearchDirection::Previous);
    QCOMPARE(latestSearchUpdate(searchSpy, 5)->selectedMatch, qint64(499));
    QVERIFY(!latestSearchUpdate(searchSpy, 5)->selectedCells.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset == 0,
                             1000);
    worker.navigateSearch(5, TerminalSearchDirection::Next);
    QCOMPARE(latestSearchUpdate(searchSpy, 5)->selectedMatch, qint64(0));
    QTRY_VERIFY_WITH_TIMEOUT(accumulatedFrame(updateSpy).scrollOffset > 0,
                             1000);

    worker.search(6, QByteArrayLiteral("needle"));
    worker.search(7, QByteArrayLiteral("aa"));
    worker.cancelSearch(8);
    QTRY_VERIFY_WITH_TIMEOUT(
        latestSearchUpdate(searchSpy, 8).has_value()
            && latestSearchUpdate(searchSpy, 8)->complete,
        1000);
    QVERIFY(!latestSearchUpdate(searchSpy, 8)->active);
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

    LaunchOptions options;
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

    LaunchOptions options;
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
    worker.initialize(options);

    QTRY_VERIFY_WITH_TIMEOUT(exitSpy.count() > 0, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(updateSpy.count() > 0, 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty() ? "" : qPrintable(errorSpy.constFirst().constFirst().toString()));

    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
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

    LaunchOptions options;
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

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStandardPaths::findExecutable(QStringLiteral("printf")),
        QString::fromUtf8(output),
    };
    QVERIFY(!options.program.constFirst().isEmpty());
    options.hold = true;
    options.linkUrl = true;
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
    LaunchOptions disabled = options;
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

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStandardPaths::findExecutable(QStringLiteral("printf")),
        QString::fromUtf8(output),
    };
    QVERIFY(!options.program.constFirst().isEmpty());
    options.hold = true;
    options.linkUrl = true;
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

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"), QStringLiteral("-c"), script,
    };
    options.hold = true;
    options.linkUrl = true;
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

    LaunchOptions options;
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

    LaunchOptions options;
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

void SessionWorkerTest::sendsTerminalControlActionsThroughPty()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    LaunchOptions options;
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

void SessionWorkerTest::stagesAndResolvesSequenceBytes()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy exitSpy(&worker, &SessionWorker::sessionExited);
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    LaunchOptions options;
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

    LaunchOptions options;
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

    LaunchOptions options;
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

    options.appearance.foregroundColor = QColor(QStringLiteral("#abcdef"));
    options.appearance.backgroundColor = QColor(QStringLiteral("#102030"));
    options.appearance.cursorColor = TerminalColorValue::fromColor(
        QColor(QStringLiteral("#fedcba")));
    options.appearance.palette.resize(256);
    for (int index = 0; index < options.appearance.palette.size(); ++index) {
        options.appearance.palette[index] =
            QColor::fromRgb(index, 255 - index, index / 2);
    }
    options.appearance.cursorStyle = TerminalCursorStyle::Underline;
    options.appearance.cursorBlink = false;
    worker.applyRuntimeOptions(options);

    QTRY_VERIFY_WITH_TIMEOUT(
        accumulatedFrame(updateSpy).foreground
            == options.appearance.foregroundColor
            && accumulatedFrame(updateSpy).palette.size() == 256,
        2000);
    const TerminalFrame frame = accumulatedFrame(updateSpy);
    QCOMPARE(frame.background, options.appearance.backgroundColor);
    QCOMPARE(frame.cursorColor, options.appearance.cursorColor.color);
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

void SessionWorkerTest::retainsSelectionAvailabilityOutsideViewport()
{
    qRegisterMetaType<TerminalUpdate>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy selectionSpy(&worker,
                            &SessionWorker::selectionAvailableChanged);

    LaunchOptions options;
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

    LaunchOptions options;
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

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "stty raw -echo; "
            "i=0; while [ $i -lt 40 ]; do "
            "printf 'reset-row-%03d\\r\\n' \"$i\"; i=$((i + 1)); done; "
            "printf '\\033]0;reset-worker-title\\007"
            "\\033]7;file:///tmp/reset-worker-cwd\\007"
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
    QTRY_VERIFY_WITH_TIMEOUT(
        !titleSpy.isEmpty()
            && titleSpy.constLast().constFirst().toString().isEmpty(),
        1000);
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

    LaunchOptions options;
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
    QSignalSpy errorSpy(&worker, &SessionWorker::errorOccurred);

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.hold = true;
    worker.initialize(options);
    QTRY_VERIFY_WITH_TIMEOUT(startedSpy.count() > 0, 1000);

    // Give the shell time to take the foreground group and settle at its
    // prompt. Its live child alone is not close-confirmation-worthy.
    QTRY_VERIFY_WITH_TIMEOUT(
        !activitySpy.isEmpty()
            && !activitySpy.constLast().constFirst().toBool(),
        1000);
    activitySpy.clear();

    const QString command = QStringLiteral("sleep 1");
    for (const QChar character : command) {
        worker.sendText(QString(character));
    }
    TerminalKeyInput enter;
    enter.key = Qt::Key_Return;
    enter.pressed = true;
    worker.stageSequenceKey(1, enter);
    QTest::qWait(50);
    QVERIFY(!spyContainsBool(activitySpy, true));
    worker.resolveSequence(1, TerminalSequenceResolution::Drop, false, {});
    QVERIFY(!spyContainsBool(activitySpy, true));

    worker.stageSequenceKey(2, enter);
    QVERIFY(!spyContainsBool(activitySpy, true));
    worker.resolveSequence(2, TerminalSequenceResolution::Flush, false, {});
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
        worker.sendText(QString(character));
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
