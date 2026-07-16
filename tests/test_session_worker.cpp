#include "launch_options.h"
#include "session_worker.h"
#include "terminal_types.h"

#include <QDir>
#include <QSignalSpy>
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
    void appliesReloadedAppearanceToExistingTerminal();
    void retainsSelectionAvailabilityOutsideViewport();
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
    worker.scrollViewport(-50);
    QTest::qWait(100);
    QVERIFY(!spyContainsBool(selectionSpy, false));

    worker.clearSelection();
    QVERIFY(spyContainsBool(selectionSpy, false));
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
    worker.sendKey(enter);
    // The activity hint is synchronous, closing in the interval before the
    // next tcgetpgrp poll cannot lose the foreground-job confirmation.
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
