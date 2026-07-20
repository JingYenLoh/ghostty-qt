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

bool writeGhosttyConfig(const QString &configHome,
                        const QByteArray &contents)
{
    const QString ghosttyDirectory =
        QDir(configHome).filePath(QStringLiteral("ghostty"));
    if (!QDir().mkpath(ghosttyDirectory)) return false;
    QFile config(QDir(ghosttyDirectory).filePath(
        QStringLiteral("config.ghostty")));
    return config.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && config.write(contents) == contents.size();
}

QProcessEnvironment applicationEnvironment(
    const PrivateSessionBus &bus,
    const QString &configHome)
{
    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("TERM_PROGRAM"));
    environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
                       bus.address());
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), configHome);
    environment.insert(QStringLiteral("GHOSTTY_QT_ALLOW_NON_WAYLAND"),
                       QStringLiteral("1"));
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"),
                       QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("QT_QUICK_BACKEND"),
                       QStringLiteral("software"));
    return environment;
}

} // namespace

class ApplicationSingleInstanceTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void residentPrimaryIsReactivatedByBareSecondLaunch();
    void falseLauncherLeavesPrimaryAtZeroUntilTrueLauncherActivates();
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
    const QByteArray configContents =
        "gtk-single-instance = true\n"
        "initial-window = true\n"
        "quit-after-last-window-closed = false\n"
        "confirm-close-surface = false\n";
    QVERIFY(writeGhosttyConfig(configHome.path(), configContents));

    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    QProcessEnvironment environment = applicationEnvironment(
        bus, configHome.path());
    environment.insert(
        QStringLiteral("GHOSTTY_QT_TEST_APPLICATION_LIFETIME"),
        QStringLiteral("external-activation"));

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

void ApplicationSingleInstanceTest::falseLauncherLeavesPrimaryAtZeroUntilTrueLauncherActivates()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon")).isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir falseConfigHome(
        QDir::current().filePath(
            QStringLiteral("tmp/application-initial-window-false-XXXXXX")));
    QTemporaryDir trueConfigHome(
        QDir::current().filePath(
            QStringLiteral("tmp/application-initial-window-true-XXXXXX")));
    QVERIFY(falseConfigHome.isValid());
    QVERIFY(trueConfigHome.isValid());
    QVERIFY(writeGhosttyConfig(
        falseConfigHome.path(),
        QByteArrayLiteral(
            "gtk-single-instance = true\n"
            "initial-window = false\n"
            "quit-after-last-window-closed = true\n"
            "confirm-close-surface = false\n")));
    QVERIFY(writeGhosttyConfig(
        trueConfigHome.path(),
        QByteArrayLiteral(
            "gtk-single-instance = true\n"
            "initial-window = true\n"
            "quit-after-last-window-closed = false\n"
            "confirm-close-surface = false\n")));

    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    QProcessEnvironment primaryEnvironment = applicationEnvironment(
        bus, falseConfigHome.path());
    primaryEnvironment.insert(QStringLiteral("GHOSTTY_QT_TEST_INITIAL_WINDOW"),
                              QStringLiteral("1"));
    QProcess primary;
    primary.setProgram(QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE));
    primary.setProcessEnvironment(primaryEnvironment);
    primary.start();
    QVERIFY(primary.waitForStarted(3000));
    const auto cleanup = qScopeGuard([&primary] {
        if (primary.state() == QProcess::NotRunning) return;
        primary.kill();
        primary.waitForFinished(3000);
    });

    QByteArray primaryOutput;
    const QByteArrayView ready("GHOSTTY_QT_INITIAL_WINDOW_READY");
    QVERIFY2(waitForMarker(primary, primaryOutput, ready, 10'000),
             qPrintable(processFailure(primary, primaryOutput)));
    QCOMPARE(primary.state(), QProcess::Running);

    QProcess falseSecondary;
    falseSecondary.setProgram(QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE));
    falseSecondary.setProcessEnvironment(applicationEnvironment(
        bus, falseConfigHome.path()));
    falseSecondary.start();
    QVERIFY(falseSecondary.waitForStarted(3000));
    QVERIFY2(falseSecondary.waitForFinished(10'000),
             qPrintable(processFailure(
                 falseSecondary, falseSecondary.readAllStandardOutput())));
    QCOMPARE(falseSecondary.exitStatus(), QProcess::NormalExit);
    QCOMPARE(falseSecondary.exitCode(), 0);
    QTest::qWait(100);
    primaryOutput += primary.readAllStandardOutput();
    QVERIFY(!primaryOutput.contains("GHOSTTY_QT_INITIAL_WINDOW_CREATED"));
    QCOMPARE(primary.state(), QProcess::Running);

    QProcess trueSecondary;
    trueSecondary.setProgram(QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE));
    trueSecondary.setProcessEnvironment(applicationEnvironment(
        bus, trueConfigHome.path()));
    trueSecondary.start();
    QVERIFY(trueSecondary.waitForStarted(3000));
    QVERIFY2(trueSecondary.waitForFinished(10'000),
             qPrintable(processFailure(
                 trueSecondary, trueSecondary.readAllStandardOutput())));
    QCOMPARE(trueSecondary.exitStatus(), QProcess::NormalExit);
    QCOMPARE(trueSecondary.exitCode(), 0);

    const QByteArrayView created("GHOSTTY_QT_INITIAL_WINDOW_CREATED");
    QVERIFY2(waitForMarker(primary, primaryOutput, created, 10'000),
             qPrintable(processFailure(primary, primaryOutput)));
    QVERIFY2(primary.waitForFinished(10'000),
             qPrintable(processFailure(primary, primaryOutput)));
    QCOMPARE(primary.exitStatus(), QProcess::NormalExit);
    QCOMPARE(primary.exitCode(), 0);
}

QTEST_GUILESS_MAIN(ApplicationSingleInstanceTest)

#include "test_application_single_instance.moc"
