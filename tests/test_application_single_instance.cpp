#include "private_session_bus.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

namespace {

bool waitForMarker(QProcess &process, QByteArray &output,
                   QByteArrayView marker, int timeoutMilliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMilliseconds) {
        output += process.readAllStandardOutput();
        if (output.contains(marker)) return true;
        if (process.state() == QProcess::NotRunning) break;
        const int remaining = timeoutMilliseconds
            - static_cast<int>(timer.elapsed());
        (void) process.waitForReadyRead(std::clamp(remaining, 1, 100));
    }
    output += process.readAllStandardOutput();
    return output.contains(marker);
}

QString processFailure(QProcess &process, const QByteArray &output)
{
    return QStringLiteral("state=%1 exit=%2 error=%3 stdout=%4 stderr=%5")
        .arg(process.state())
        .arg(process.exitCode())
        .arg(process.errorString(), QString::fromUtf8(output),
             QString::fromUtf8(process.readAllStandardError()));
}

} // namespace

class ApplicationSingleInstanceTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void residentPrimaryIsReactivatedByBareSecondLaunch();
};

void ApplicationSingleInstanceTest::residentPrimaryIsReactivatedByBareSecondLaunch()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon")).isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir configHome(
        QDir::current().filePath(
            QStringLiteral("tmp/application-single-instance-XXXXXX")));
    QVERIFY(configHome.isValid());
    const QString ghosttyDirectory =
        QDir(configHome.path()).filePath(QStringLiteral("ghostty"));
    QVERIFY(QDir().mkpath(ghosttyDirectory));
    QFile config(QDir(ghosttyDirectory).filePath(
        QStringLiteral("config.ghostty")));
    QVERIFY(config.open(QIODevice::WriteOnly));
    const QByteArray configContents =
        "gtk-single-instance = true\n"
        "quit-after-last-window-closed = false\n"
        "confirm-close-surface = false\n";
    QCOMPARE(config.write(configContents), configContents.size());
    config.close();

    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("TERM_PROGRAM"));
    environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
                       bus.address());
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"),
                       configHome.path());
    environment.insert(QStringLiteral("GHOSTTY_QT_ALLOW_NON_WAYLAND"),
                       QStringLiteral("1"));
    environment.insert(
        QStringLiteral("GHOSTTY_QT_TEST_APPLICATION_LIFETIME"),
        QStringLiteral("external-activation"));
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"),
                       QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("QT_QUICK_BACKEND"),
                       QStringLiteral("software"));

    QProcess primary;
    primary.setProgram(QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE));
    primary.setProcessEnvironment(environment);
    primary.start();
    QVERIFY(primary.waitForStarted(3000));
    const auto cleanup = qScopeGuard([&primary] {
        if (primary.state() == QProcess::NotRunning) return;
        primary.kill();
        primary.waitForFinished(3000);
    });

    QByteArray primaryOutput;
    const QByteArrayView ready("GHOSTTY_QT_ACTIVATION_READY");
    QVERIFY2(waitForMarker(primary, primaryOutput, ready, 10'000),
             qPrintable(processFailure(primary, primaryOutput)));
    QCOMPARE(primary.state(), QProcess::Running);

    QProcess secondary;
    secondary.setProgram(QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE));
    secondary.setProcessEnvironment(environment);
    secondary.start();
    QVERIFY(secondary.waitForStarted(3000));
    QVERIFY2(secondary.waitForFinished(10'000),
             qPrintable(processFailure(
                 secondary, secondary.readAllStandardOutput())));
    QCOMPARE(secondary.exitStatus(), QProcess::NormalExit);
    QCOMPARE(secondary.exitCode(), 0);

    const QByteArrayView accepted("GHOSTTY_QT_ACTIVATION_ACCEPTED");
    QVERIFY2(waitForMarker(primary, primaryOutput, accepted, 10'000),
             qPrintable(processFailure(primary, primaryOutput)));
    QVERIFY2(primary.waitForFinished(10'000),
             qPrintable(processFailure(primary, primaryOutput)));
    QCOMPARE(primary.exitStatus(), QProcess::NormalExit);
    QCOMPARE(primary.exitCode(), 0);
}

QTEST_GUILESS_MAIN(ApplicationSingleInstanceTest)

#include "test_application_single_instance.moc"
