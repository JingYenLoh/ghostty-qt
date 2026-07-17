#include "launch_options.h"
#include "session_worker.h"
#include "terminal_types.h"

#include <QDir>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

#include <algorithm>

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
    void resetsTerminalStateAndWorkerCaches();
    void resolvesCorrelatedHyperlinkQueries();
    void explicitProgramIsActiveForItsLifetime();
    void interactiveShellTracksForegroundJobs();
};

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
    qRegisterMetaType<QVector<QPoint>>();
    SessionWorker worker;
    QSignalSpy updateSpy(&worker, &SessionWorker::terminalUpdated);
    QSignalSpy hyperlinkSpy(&worker, &SessionWorker::hyperlinkResolved);
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
                          candidates.constFirst().y(), candidates);
    QCOMPARE(hyperlinkSpy.count(), 1);
    QCOMPARE(hyperlinkSpy.at(0).at(0).toULongLong(), quint64(41));
    QCOMPARE(hyperlinkSpy.at(0).at(1).toULongLong(), frame.contentRevision);
    QCOMPARE(hyperlinkSpy.at(0).at(2).toByteArray(), uri);
    QCOMPARE(qvariant_cast<QVector<QPoint>>(hyperlinkSpy.at(0).at(3)),
             candidates);

    worker.queryHyperlink(42, frame.contentRevision, 11, 0, candidates);
    QCOMPARE(hyperlinkSpy.count(), 2);
    QCOMPARE(hyperlinkSpy.at(1).at(0).toULongLong(), quint64(42));
    QVERIFY(hyperlinkSpy.at(1).at(2).toByteArray().isEmpty());

    worker.queryHyperlink(43, frame.contentRevision - 1,
                          candidates.constFirst().x(),
                          candidates.constFirst().y(), candidates);
    QCOMPARE(hyperlinkSpy.count(), 3);
    QCOMPARE(hyperlinkSpy.at(2).at(0).toULongLong(), quint64(43));
    QCOMPARE(hyperlinkSpy.at(2).at(1).toULongLong(), frame.contentRevision);
    QVERIFY(hyperlinkSpy.at(2).at(2).toByteArray().isEmpty());

    // A valid raw action follows the viewport back to the live screen even
    // when its decoded payload is empty. That viewport mutation advances the
    // revision, so a query using the old scrollback coordinates is rejected.
    worker.sendRawText(QByteArray{});
    QTRY_VERIFY_WITH_TIMEOUT(
        accumulatedFrame(updateSpy).contentRevision > frame.contentRevision,
        1000);
    const TerminalFrame liveFrame = accumulatedFrame(updateSpy);
    QVERIFY(liveFrame.scrollOffset > 0);
    worker.queryHyperlink(44, frame.contentRevision,
                          candidates.constFirst().x(),
                          candidates.constFirst().y(), candidates);
    QCOMPARE(hyperlinkSpy.count(), 4);
    QCOMPARE(hyperlinkSpy.at(3).at(0).toULongLong(), quint64(44));
    QCOMPARE(hyperlinkSpy.at(3).at(1).toULongLong(),
             liveFrame.contentRevision);
    QVERIFY(hyperlinkSpy.at(3).at(2).toByteArray().isEmpty());

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
