#include "launch_options.h"
#include "session_worker.h"
#include "terminal_types.h"

#include <QDir>
#include <QSignalSpy>
#include <QTest>

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

bool framesContain(const QSignalSpy &spy, const QString &needle)
{
    for (const QList<QVariant> &arguments : spy) {
        if (frameText(qvariant_cast<TerminalFrame>(arguments.constFirst())).contains(needle)) {
            return true;
        }
    }
    return false;
}

} // namespace

class SessionWorkerTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void runsCommandThroughPty();
    void sendsBracketedPasteThroughPty();
};

void SessionWorkerTest::runsCommandThroughPty()
{
    qRegisterMetaType<TerminalFrame>();
    SessionWorker worker;
    QSignalSpy frameSpy(&worker, &SessionWorker::frameReady);
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
    QTRY_VERIFY_WITH_TIMEOUT(frameSpy.count() > 0, 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty() ? "" : qPrintable(errorSpy.constFirst().constFirst().toString()));

    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    const TerminalFrame finalFrame =
        qvariant_cast<TerminalFrame>(frameSpy.constLast().constFirst());
    const QString finalContents = frameText(finalFrame);
    QVERIFY2(finalContents.contains(
                 QStringLiteral("device-response:1b5b3f36323b323263")),
             qPrintable(finalContents));
    QVERIFY2(finalContents.contains(QStringLiteral("ghostty-qt-final")),
             qPrintable(finalContents));
    worker.shutdown();
}

void SessionWorkerTest::sendsBracketedPasteThroughPty()
{
    qRegisterMetaType<TerminalFrame>();
    SessionWorker worker;
    QSignalSpy frameSpy(&worker, &SessionWorker::frameReady);
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
        framesContain(frameSpy, QStringLiteral("paste-ready")), 5000);
    worker.paste(QStringLiteral("one\ntwo"));

    QTRY_VERIFY_WITH_TIMEOUT(exitSpy.count() > 0, 5000);
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty() ? "" : qPrintable(errorSpy.constFirst().constFirst().toString()));
    QCOMPARE(exitSpy.constFirst().at(0).toInt(), 0);
    const TerminalFrame finalFrame =
        qvariant_cast<TerminalFrame>(frameSpy.constLast().constFirst());
    const QString finalContents = frameText(finalFrame);
    QVERIFY2(finalContents.contains(QStringLiteral(
                 "paste-bytes:1b5b3230307e6f6e650a74776f1b5b3230317e")),
             qPrintable(finalContents));
    worker.shutdown();
}

QTEST_GUILESS_MAIN(SessionWorkerTest)

#include "test_session_worker.moc"
