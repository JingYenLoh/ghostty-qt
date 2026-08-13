#include "desktop_activation.h"

#include <QByteArray>
#include <QTest>
#include <QVariantMap>
#include <QWindow>

#include <utility>

namespace {

class ScopedEnvironmentVariable final {
public:
    ScopedEnvironmentVariable(QByteArray name, const QByteArray &value)
        : name_(std::move(name))
        , wasSet_(qEnvironmentVariableIsSet(name_.constData()))
        , previousValue_(qgetenv(name_.constData()))
    {
        (void)qputenv(name_.constData(), value);
    }

    ~ScopedEnvironmentVariable()
    {
        if (wasSet_) {
            (void)qputenv(name_.constData(), previousValue_);
        } else {
            (void)qunsetenv(name_.constData());
        }
    }

    Q_DISABLE_COPY_MOVE(ScopedEnvironmentVariable)

private:
    QByteArray name_;
    bool wasSet_ = false;
    QByteArray previousValue_;
};

} // namespace

class DesktopActivationTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void parsesOnlySupportedStringPlatformData();
    void capturesAndClearsLauncherEnvironment();
    void scopesPlatformDataToWindowPresentation();
};

void DesktopActivationTest::parsesOnlySupportedStringPlatformData()
{
    const QVariantMap platformData{
        {QStringLiteral("activation-token"), QStringLiteral("token:one")},
        {QStringLiteral("desktop-startup-id"), QStringLiteral("startup:two")},
        {QStringLiteral("unknown"), QStringLiteral("ignored")},
    };
    const DesktopActivationContext context =
        DesktopActivationContext::fromPlatformData(platformData);
    QCOMPARE(context.xdgActivationToken, QStringLiteral("token:one"));
    QCOMPARE(context.desktopStartupId, QStringLiteral("startup:two"));
    QCOMPARE(
        context.toPlatformData(),
        QVariantMap({
            {QStringLiteral("activation-token"), QStringLiteral("token:one")},
            {QStringLiteral("desktop-startup-id"),
             QStringLiteral("startup:two")},
        }));

    const DesktopActivationContext invalid =
        DesktopActivationContext::fromPlatformData({
            {QStringLiteral("activation-token"), 42},
            {QStringLiteral("desktop-startup-id"),
             QByteArrayLiteral("convertible")},
        });
    QVERIFY(invalid.isEmpty());
    QVERIFY(invalid.toPlatformData().isEmpty());

    QString embeddedNull = QStringLiteral("token");
    embeddedNull.append(QChar::Null);
    embeddedNull.append(QStringLiteral("suffix"));
    QVERIFY(DesktopActivationContext::fromPlatformData(
                {
                    {QStringLiteral("activation-token"), embeddedNull},
                })
                .isEmpty());
}

void DesktopActivationTest::capturesAndClearsLauncherEnvironment()
{
    const ScopedEnvironmentVariable activationToken(
        QByteArrayLiteral("XDG_ACTIVATION_TOKEN"),
        QByteArrayLiteral("launch-token"));
    const ScopedEnvironmentVariable startupId(
        QByteArrayLiteral("DESKTOP_STARTUP_ID"),
        QByteArrayLiteral("launch-startup"));

    const DesktopActivationContext context =
        DesktopActivationContext::takeFromEnvironment();
    QCOMPARE(context.xdgActivationToken, QStringLiteral("launch-token"));
    QCOMPARE(context.desktopStartupId, QStringLiteral("launch-startup"));
    QVERIFY(!qEnvironmentVariableIsSet("XDG_ACTIVATION_TOKEN"));
    QVERIFY(!qEnvironmentVariableIsSet("DESKTOP_STARTUP_ID"));
}

void DesktopActivationTest::scopesPlatformDataToWindowPresentation()
{
    const ScopedEnvironmentVariable activationToken(
        QByteArrayLiteral("XDG_ACTIVATION_TOKEN"),
        QByteArrayLiteral("previous-token"));
    const ScopedEnvironmentVariable startupId(
        QByteArrayLiteral("DESKTOP_STARTUP_ID"),
        QByteArrayLiteral("previous-startup"));
    const ScopedEnvironmentVariable inheritedSentinel(
        QByteArrayLiteral("GHOSTTY_QT_ACTIVATION_SENTINEL"),
        QByteArrayLiteral("before-show"));
    const ScopedEnvironmentVariable notifySocket(
        QByteArrayLiteral("NOTIFY_SOCKET"),
        QByteArrayLiteral("@ghostty-qt-test-notify"));
    const ScopedEnvironmentVariable invocationId(
        QByteArrayLiteral("INVOCATION_ID"), QByteArrayLiteral("service-id"));
    const ScopedEnvironmentVariable dbusStarter(
        QByteArrayLiteral("DBUS_STARTER_ADDRESS"),
        QByteArrayLiteral("unix:path=/service-bus"));

    QByteArray tokenDuringShow;
    QByteArray startupDuringShow;
    QProcessEnvironment childDuringShow;
    int presentations = 0;
    QWindow window;
    connect(&window, &QWindow::visibleChanged, &window, [&](bool visible) {
        if (!visible) return;
        ++presentations;
        tokenDuringShow = qgetenv("XDG_ACTIVATION_TOKEN");
        startupDuringShow = qgetenv("DESKTOP_STARTUP_ID");
        const ScopedEnvironmentVariable lateMutation(
            QByteArrayLiteral("GHOSTTY_QT_ACTIVATION_LATE"),
            QByteArrayLiteral("during-show"));
        childDuringShow = sanitizedChildEnvironment();
    });

    showWindowWithActivation(
        window,
        {
            .xdgActivationToken = QStringLiteral("incoming-token"),
            .desktopStartupId = QStringLiteral("incoming-startup"),
        });

    QCOMPARE(presentations, 1);
    QCOMPARE(tokenDuringShow, QByteArrayLiteral("incoming-token"));
    QCOMPARE(startupDuringShow, QByteArrayLiteral("incoming-startup"));
    QCOMPARE(
        childDuringShow.value(QStringLiteral("GHOSTTY_QT_ACTIVATION_SENTINEL")),
        QStringLiteral("before-show"));
    QVERIFY(!childDuringShow.contains(
        QStringLiteral("GHOSTTY_QT_ACTIVATION_LATE")));
    QVERIFY(!childDuringShow.contains(QStringLiteral("XDG_ACTIVATION_TOKEN")));
    QVERIFY(!childDuringShow.contains(QStringLiteral("DESKTOP_STARTUP_ID")));
    QVERIFY(!childDuringShow.contains(QStringLiteral("NOTIFY_SOCKET")));
    QVERIFY(!childDuringShow.contains(QStringLiteral("INVOCATION_ID")));
    QVERIFY(!childDuringShow.contains(QStringLiteral("DBUS_STARTER_ADDRESS")));
    QVERIFY(!qEnvironmentVariableIsSet("XDG_ACTIVATION_TOKEN"));
    QVERIFY(!qEnvironmentVariableIsSet("DESKTOP_STARTUP_ID"));
    QCOMPARE(qgetenv("NOTIFY_SOCKET"),
             QByteArrayLiteral("@ghostty-qt-test-notify"));
}

QTEST_MAIN(DesktopActivationTest)

#include "test_desktop_activation.moc"
