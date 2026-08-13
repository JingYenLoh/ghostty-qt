#include "ghostty_application_ipc.h"
#include "private_session_bus.h"

#include <QDBusMessage>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QVariantList>
#include <QVariantMap>

#include <chrono>
#include <future>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr auto DefaultApplicationId = "io.github.JingYenLoh.ghostty_qt";

class LocalTemporaryDirectory final {
public:
    LocalTemporaryDirectory()
    {
        const QString root = QDir::current().filePath(QStringLiteral("tmp"));
        valid_ = QDir().mkpath(root);
        if (valid_) {
            directory_ = std::make_unique<QTemporaryDir>(
                QDir(root).filePath(QStringLiteral("ipc-XXXXXX")));
            valid_ = directory_->isValid();
        }
    }

    [[nodiscard]] bool isValid() const noexcept { return valid_; }
    [[nodiscard]] QString path() const { return directory_->path(); }

private:
    std::unique_ptr<QTemporaryDir> directory_;
    bool valid_ = false;
};

GhosttyApplicationIpcParseContext contextFor(const QString &workingDirectory)
{
    return {
        .defaultApplicationId = QString::fromLatin1(DefaultApplicationId),
        .workingDirectory = workingDirectory,
        .homeDirectory =
            QDir(workingDirectory).filePath(QStringLiteral("home")),
    };
}

const QStringList &payload(const GhosttyApplicationIpcRequest &request)
{
    Q_ASSERT(request.stringArrayParameter.has_value());
    return *request.stringArrayParameter;
}

const GhosttyNewTabIpcParameter &
newTabPayload(const GhosttyApplicationIpcRequest &request)
{
    Q_ASSERT(request.newTabParameter.has_value());
    return *request.newTabParameter;
}

class RawArguments final {
public:
    RawArguments(std::initializer_list<std::string_view> arguments)
    {
        storage_.reserve(arguments.size());
        for (const std::string_view argument : arguments) {
            storage_.emplace_back(argument);
        }
        pointers_.reserve(storage_.size());
        for (std::string &argument : storage_) {
            pointers_.push_back(argument.data());
        }
    }

    [[nodiscard]] std::span<char *const> span() noexcept { return pointers_; }

private:
    std::vector<std::string> storage_;
    std::vector<char *> pointers_;
};

QString uniqueServiceName()
{
    QString suffix =
        QUuid::createUuid().toString(QUuid::WithoutBraces).remove(u'-');
    return QStringLiteral("io.github.JingYenLoh.ghostty_qt.IpcTest.t%1")
        .arg(suffix);
}

class ActionsEndpoint final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.gtk.Actions")

public:
    int calls = 0;
    QString actionName;
    QVariantList parameters;
    QVariantMap platformData;

public Q_SLOTS:
    Q_SCRIPTABLE void Activate(const QString &action,
                               const QVariantList &actionParameters,
                               const QVariantMap &platform)
    {
        ++calls;
        actionName = action;
        parameters = actionParameters;
        platformData = platform;
    }
};

} // namespace

class GhosttyApplicationIpcTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void newTabTargetsExplicitOrEnvironmentSurface();
    void injectsCanonicalCallerDirectoryAndForwardsArguments();
    void canonicalizesConcreteDirectoryAndTargetsCustomClass();
    void expandsHomeForConcreteDirectory();
    void homeAndInheritDoNotSuppressCallerDirectory();
    void executeBoundaryMakesRemainingArgumentsOpaque();
    void rejectsInvalidInput();
    void toggleUsesDefaultTargetAndIgnoresExtras();
    void rawArgumentOverloadPreservesEmptyValues();
    void decodesKnownOverridesWithLastValueWinning();
    void decodesGhosttyCommandGrammar();
    void executeArgumentsOverrideOnlyWhenNonEmpty();
    void receiverRejectsImpossibleStrings();
    void sendsExactGtkActionsPayload();
    void reportsDbusFailureWithoutFallback();
};

void GhosttyApplicationIpcTest::newTabTargetsExplicitOrEnvironmentSurface()
{
    LocalTemporaryDirectory temporary;
    QVERIFY(temporary.isValid());
    GhosttyApplicationIpcParseContext context = contextFor(temporary.path());
    context.surfaceIdEnvironment = QByteArrayLiteral("0x2a");

    auto parsed = parseGhosttyApplicationIpcRequest(
        GhosttyApplicationIpcAction::NewTab,
        QList<QByteArray>{
            QByteArrayLiteral("ghostty"),
            QByteArrayLiteral("+new-tab"),
            QByteArrayLiteral("--surface-id= 0x1234 "),
            QByteArrayLiteral("--shell-integration=fish"),
            QByteArrayLiteral("--title=remote"),
        },
        context);
    QVERIFY2(parsed.has_value(),
             parsed ? "" : qPrintable(parsed.error().diagnostic));
    QCOMPARE(parsed->actionName, QStringLiteral("new-tab"));
    QVERIFY(!parsed->stringArrayParameter.has_value());
    QCOMPARE(newTabPayload(*parsed).surfaceId, quint64{0x1234});
    QCOMPARE(newTabPayload(*parsed).arguments,
             QStringList({
                 QStringLiteral("--working-directory=%1")
                     .arg(QFileInfo(temporary.path()).canonicalFilePath()),
                 QStringLiteral("--shell-integration=fish"),
                 QStringLiteral("--title=remote"),
             }));

    parsed = parseGhosttyApplicationIpcRequest(
        GhosttyApplicationIpcAction::NewTab,
        QList<QByteArray>{QByteArrayLiteral("ghostty"),
                          QByteArrayLiteral("+new-tab")},
        context);
    QVERIFY(parsed.has_value());
    QCOMPARE(newTabPayload(*parsed).surfaceId, quint64{42});

    parsed = parseGhosttyApplicationIpcRequest(
        GhosttyApplicationIpcAction::NewTab,
        QList<QByteArray>{QByteArrayLiteral("ghostty"),
                          QByteArrayLiteral("+new-tab"),
                          QByteArrayLiteral("--surface-id=invalid")},
        context);
    QVERIFY(!parsed.has_value());
    QVERIFY(parsed.error().diagnostic.contains(QStringLiteral("Surface ID")));
}

void GhosttyApplicationIpcTest::
    injectsCanonicalCallerDirectoryAndForwardsArguments()
{
    LocalTemporaryDirectory temporary;
    QVERIFY(temporary.isValid());
    const QString canonical = QFileInfo(temporary.path()).canonicalFilePath();

    const auto parsed = parseGhosttyApplicationIpcRequest(
        GhosttyApplicationIpcAction::NewWindow,
        QList<QByteArray>{QByteArrayLiteral("ghostty"),
                          QByteArrayLiteral("--title=before"),
                          QByteArrayLiteral("+new-window"), QByteArray{},
                          QByteArrayLiteral("--unknown=value")},
        contextFor(temporary.path()));
    QVERIFY2(parsed.has_value(),
             parsed ? "" : qPrintable(parsed.error().diagnostic));
    QCOMPARE(parsed->applicationId, QString::fromLatin1(DefaultApplicationId));
    QCOMPARE(parsed->objectPath,
             QStringLiteral("/io/github/JingYenLoh/ghostty_qt"));
    QCOMPARE(parsed->actionName, QStringLiteral("new-window-command"));
    QCOMPARE(payload(*parsed),
             QStringList({
                 QStringLiteral("--working-directory=%1").arg(canonical),
                 QStringLiteral("--title=before"),
                 QString{},
                 QStringLiteral("--unknown=value"),
             }));
}

void GhosttyApplicationIpcTest::
    canonicalizesConcreteDirectoryAndTargetsCustomClass()
{
    LocalTemporaryDirectory temporary;
    QVERIFY(temporary.isValid());
    const QString target =
        QDir(temporary.path()).filePath(QStringLiteral("target"));
    QVERIFY(QDir().mkpath(target));

    const auto parsed = parseGhosttyApplicationIpcRequest(
        GhosttyApplicationIpcAction::NewWindow,
        QList<QByteArray>{
            QByteArrayLiteral("ghostty"),
            QByteArrayLiteral("--class=  org.example-Ghostty.Test \t"),
            QByteArrayLiteral("+new-window"),
            QByteArrayLiteral("--working-directory= target/../target \t"),
            QByteArrayLiteral("--title=kept"),
        },
        contextFor(temporary.path()));
    QVERIFY2(parsed.has_value(),
             parsed ? "" : qPrintable(parsed.error().diagnostic));
    QCOMPARE(parsed->applicationId, QStringLiteral("org.example-Ghostty.Test"));
    QCOMPARE(parsed->objectPath, QStringLiteral("/org/example_Ghostty/Test"));
    QCOMPARE(payload(*parsed),
             QStringList({
                 QStringLiteral("--working-directory=%1")
                     .arg(QFileInfo(target).canonicalFilePath()),
                 QStringLiteral("--title=kept"),
             }));
}

void GhosttyApplicationIpcTest::expandsHomeForConcreteDirectory()
{
    LocalTemporaryDirectory temporary;
    QVERIFY(temporary.isValid());
    const QString home =
        QDir(temporary.path()).filePath(QStringLiteral("home"));
    const QString project = QDir(home).filePath(QStringLiteral("project"));
    QVERIFY(QDir().mkpath(project));

    const auto parsed = parseGhosttyApplicationIpcRequest(
        GhosttyApplicationIpcAction::NewWindow,
        QList<QByteArray>{
            QByteArrayLiteral("ghostty"),
            QByteArrayLiteral("+new-window"),
            QByteArrayLiteral("--working-directory=~/project"),
        },
        contextFor(temporary.path()));
    QVERIFY2(parsed.has_value(),
             parsed ? "" : qPrintable(parsed.error().diagnostic));
    QCOMPARE(payload(*parsed),
             QStringList({
                 QStringLiteral("--working-directory=%1")
                     .arg(QFileInfo(project).canonicalFilePath()),
             }));
}

void GhosttyApplicationIpcTest::homeAndInheritDoNotSuppressCallerDirectory()
{
    LocalTemporaryDirectory temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(
        QDir().mkpath(QDir(temporary.path()).filePath(QStringLiteral("home"))));
    const QString canonical = QFileInfo(temporary.path()).canonicalFilePath();

    const auto parsed = parseGhosttyApplicationIpcRequest(
        GhosttyApplicationIpcAction::NewWindow,
        QList<QByteArray>{
            QByteArrayLiteral("ghostty"),
            QByteArrayLiteral("+new-window"),
            QByteArrayLiteral("--working-directory= home "),
            QByteArrayLiteral("--working-directory=inherit"),
        },
        contextFor(temporary.path()));
    QVERIFY2(parsed.has_value(),
             parsed ? "" : qPrintable(parsed.error().diagnostic));
    QCOMPARE(payload(*parsed),
             QStringList({
                 QStringLiteral("--working-directory=%1").arg(canonical),
                 QStringLiteral("--working-directory= home "),
                 QStringLiteral("--working-directory=inherit"),
             }));
}

void GhosttyApplicationIpcTest::executeBoundaryMakesRemainingArgumentsOpaque()
{
    LocalTemporaryDirectory temporary;
    QVERIFY(temporary.isValid());
    const QString canonical = QFileInfo(temporary.path()).canonicalFilePath();

    const auto parsed = parseGhosttyApplicationIpcRequest(
        GhosttyApplicationIpcAction::NewWindow,
        QList<QByteArray>{
            QByteArrayLiteral("ghostty"),
            QByteArrayLiteral("--class=org.before.Action"),
            QByteArrayLiteral("+new-window"),
            QByteArrayLiteral("-e"),
            QByteArrayLiteral("--class=org.after.Ignored"),
            QByteArrayLiteral("--working-directory=missing"),
            QByteArrayLiteral("+new-window"),
            QByteArray{},
        },
        contextFor(temporary.path()));
    QVERIFY2(parsed.has_value(),
             parsed ? "" : qPrintable(parsed.error().diagnostic));
    QCOMPARE(parsed->applicationId, QStringLiteral("org.before.Action"));
    QCOMPARE(payload(*parsed),
             QStringList({
                 QStringLiteral("--working-directory=%1").arg(canonical),
                 QStringLiteral("-e"),
                 QStringLiteral("--class=org.after.Ignored"),
                 QStringLiteral("--working-directory=missing"),
                 QStringLiteral("+new-window"),
                 QString{},
             }));
}

void GhosttyApplicationIpcTest::rejectsInvalidInput()
{
    LocalTemporaryDirectory temporary;
    QVERIFY(temporary.isValid());
    const auto context = contextFor(temporary.path());

    const QList<QList<QByteArray>> invalidArguments{
        {},
        {QByteArrayLiteral("ghostty")},
        {QByteArrayLiteral("ghostty"), QByteArrayLiteral("+new-window"),
         QByteArrayLiteral("--class=not-an-id")},
        {QByteArrayLiteral("ghostty"), QByteArrayLiteral("+new-window"),
         QByteArrayLiteral("--working-directory=missing")},
        {QByteArrayLiteral("ghostty"), QByteArrayLiteral("+new-window"),
         QByteArray::fromHex("ff")},
        {QByteArrayLiteral("ghostty"), QByteArrayLiteral("+new-window"),
         QByteArray::fromRawData("a\0b", 3)},
    };
    for (const QList<QByteArray> &arguments : invalidArguments) {
        const auto parsed = parseGhosttyApplicationIpcRequest(
            GhosttyApplicationIpcAction::NewWindow, arguments, context);
        QVERIFY(!parsed.has_value());
        QVERIFY(!parsed.error().diagnostic.isEmpty());
        QCOMPARE(parsed.error().exitCode(), 1);
    }

    auto invalidDefault = context;
    invalidDefault.defaultApplicationId = QStringLiteral("not-an-id");
    const auto parsed = parseGhosttyApplicationIpcRequest(
        GhosttyApplicationIpcAction::NewWindow,
        QList<QByteArray>{QByteArrayLiteral("ghostty"),
                          QByteArrayLiteral("+new-window")},
        invalidDefault);
    QVERIFY(!parsed.has_value());
}

void GhosttyApplicationIpcTest::toggleUsesDefaultTargetAndIgnoresExtras()
{
    auto context = contextFor(QStringLiteral("/definitely/missing"));
    const auto parsed = parseGhosttyApplicationIpcRequest(
        GhosttyApplicationIpcAction::ToggleQuickTerminal,
        QList<QByteArray>{
            QByteArrayLiteral("ghostty"),
            QByteArrayLiteral("--class=org.example.Ignored"),
            QByteArrayLiteral("+toggle-quick-terminal"),
            QByteArrayLiteral("--working-directory=missing"),
            QByteArray::fromHex("ff"),
        },
        context);
    QVERIFY2(parsed.has_value(),
             parsed ? "" : qPrintable(parsed.error().diagnostic));
    QCOMPARE(parsed->applicationId, QString::fromLatin1(DefaultApplicationId));
    QCOMPARE(parsed->objectPath,
             QStringLiteral("/io/github/JingYenLoh/ghostty_qt"));
    QCOMPARE(parsed->actionName, QStringLiteral("toggle-quick-terminal"));
    QVERIFY(!parsed->stringArrayParameter.has_value());
}

void GhosttyApplicationIpcTest::rawArgumentOverloadPreservesEmptyValues()
{
    LocalTemporaryDirectory temporary;
    QVERIFY(temporary.isValid());
    RawArguments arguments{
        "ghostty", "+new-window", "-e", "printf", "", "tail", "+new-window",
    };
    const auto parsed = parseGhosttyApplicationIpcRequest(
        GhosttyApplicationIpcAction::NewWindow, arguments.span(),
        contextFor(temporary.path()));
    QVERIFY2(parsed.has_value(),
             parsed ? "" : qPrintable(parsed.error().diagnostic));
    QCOMPARE(
        payload(*parsed).sliced(1),
        QStringList({QStringLiteral("-e"), QStringLiteral("printf"), QString{},
                     QStringLiteral("tail"), QStringLiteral("+new-window")}));
}

void GhosttyApplicationIpcTest::decodesKnownOverridesWithLastValueWinning()
{
    const auto decoded = decodeGhosttyNewWindowArguments({
        QStringLiteral("--title= first \t"),
        QStringLiteral("--unknown=ignored"),
        QStringLiteral("--working-directory= /first \n"),
        QStringLiteral("--title= second "),
        QStringLiteral("--working-directory=inherit"),
        QStringLiteral("--command=echo first"),
        QStringLiteral("--command=shell: echo second "),
        QStringLiteral("--shell-integration=fish"),
        QStringLiteral("--shell-integration=invalid"),
    });
    QVERIFY2(decoded.has_value(),
             decoded ? "" : qPrintable(decoded.error().diagnostic));
    QVERIFY(decoded->titleOverride.has_value());
    QCOMPARE(*decoded->titleOverride, QStringLiteral("second"));
    QVERIFY(decoded->workingDirectory.has_value());
    QCOMPARE(*decoded->workingDirectory, QStringLiteral("inherit"));
    QVERIFY(decoded->command.has_value());
    QCOMPARE(decoded->command->kind, TerminalCommandKind::Shell);
    QCOMPARE(decoded->command->shellCommand, QByteArrayLiteral("echo second"));
    QVERIFY(!decoded->command->defaultShell);
    QCOMPARE(decoded->shellIntegration,
             std::optional{GhosttyShellIntegrationMode::Fish});
}

void GhosttyApplicationIpcTest::decodesGhosttyCommandGrammar()
{
    auto decoded = decodeGhosttyNewWindowArguments({
        QStringLiteral("--command= direct:  echo  hello "),
    });
    QVERIFY(decoded.has_value());
    QVERIFY(decoded->command.has_value());
    QCOMPARE(decoded->command->kind, TerminalCommandKind::Direct);
    QCOMPARE(decoded->command->directArguments,
             QVector<QByteArray>({QByteArrayLiteral("echo"), QByteArray{},
                                  QByteArrayLiteral("hello")}));

    decoded = decodeGhosttyNewWindowArguments({
        QStringLiteral("--command=kept"),
        QStringLiteral("--command=   "),
    });
    QVERIFY(decoded.has_value());
    QVERIFY(decoded->command.has_value());
    QCOMPARE(decoded->command->kind, TerminalCommandKind::Shell);
    QCOMPARE(decoded->command->shellCommand, QByteArrayLiteral("kept"));

    decoded = decodeGhosttyNewWindowArguments({
        QStringLiteral("--command=unknown:value"),
    });
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->command->kind, TerminalCommandKind::Shell);
    QCOMPARE(decoded->command->shellCommand,
             QByteArrayLiteral("unknown:value"));
}

void GhosttyApplicationIpcTest::executeArgumentsOverrideOnlyWhenNonEmpty()
{
    auto decoded = decodeGhosttyNewWindowArguments({
        QStringLiteral("--command=before"),
        QStringLiteral("-e"),
    });
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->command->kind, TerminalCommandKind::Shell);
    QCOMPARE(decoded->command->shellCommand, QByteArrayLiteral("before"));

    decoded = decodeGhosttyNewWindowArguments({
        QStringLiteral("--command=before"),
        QStringLiteral("-e"),
        QString{},
        QStringLiteral("--title=opaque"),
        QStringLiteral("-e"),
    });
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->command->kind, TerminalCommandKind::Direct);
    QCOMPARE(
        decoded->command->directArguments,
        QVector<QByteArray>({QByteArray{}, QByteArrayLiteral("--title=opaque"),
                             QByteArrayLiteral("-e")}));
    QVERIFY(!decoded->titleOverride.has_value());
}

void GhosttyApplicationIpcTest::receiverRejectsImpossibleStrings()
{
    QString embeddedNull = QStringLiteral("a");
    embeddedNull += QChar{};
    embeddedNull += u'b';
    auto decoded = decodeGhosttyNewWindowArguments({embeddedNull});
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().exitCode(), 1);

    QString unmatchedSurrogate;
    unmatchedSurrogate += QChar(0xd800);
    decoded = decodeGhosttyNewWindowArguments({unmatchedSurrogate});
    QVERIFY(!decoded.has_value());
}

void GhosttyApplicationIpcTest::sendsExactGtkActionsPayload()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));
    const QString service = uniqueServiceName();
    const QString path = QStringLiteral("/")
        + QString(service).replace(u'.', u'/').replace(u'-', u'_');
    ActionsEndpoint endpoint;
    QVERIFY(bus.server().registerService(service));
    QVERIFY(bus.server().registerObject(
        path, &endpoint, QDBusConnection::ExportScriptableSlots));

    const GhosttyApplicationIpcRequest request{
        .applicationId = service,
        .objectPath = path,
        .actionName = QStringLiteral("new-window-command"),
        .stringArrayParameter = QStringList(
            {QStringLiteral("--working-directory=/work"), QStringLiteral("-e"),
             QStringLiteral("printf"), QString{}}),
    };
    auto future = std::async(std::launch::async, [&] {
        return sendGhosttyApplicationIpcRequest(request, bus.client(), 2s);
    });
    QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(0ms) == std::future_status::ready,
                             3000);
    const auto sent = future.get();
    QVERIFY2(sent.has_value(), sent ? "" : qPrintable(sent.error().diagnostic));
    QCOMPARE(endpoint.calls, 1);
    QCOMPARE(endpoint.actionName, QStringLiteral("new-window-command"));
    QCOMPARE(endpoint.parameters.size(), 1);
    QCOMPARE(endpoint.parameters.front().value<QStringList>(),
             *request.stringArrayParameter);
    QVERIFY(endpoint.platformData.isEmpty());

    const GhosttyApplicationIpcRequest newTab{
        .applicationId = service,
        .objectPath = path,
        .actionName = QStringLiteral("new-tab"),
        .newTabParameter =
            GhosttyNewTabIpcParameter{
                .surfaceId = 0x1234,
                .arguments =
                    {
                        QStringLiteral("--working-directory=/tab"),
                        QStringLiteral("--shell-integration=zsh"),
                    },
            },
    };
    auto tabFuture = std::async(std::launch::async, [&] {
        return sendGhosttyApplicationIpcRequest(newTab, bus.client(), 2s);
    });
    QTRY_VERIFY_WITH_TIMEOUT(
        tabFuture.wait_for(0ms) == std::future_status::ready, 3000);
    const auto tabSent = tabFuture.get();
    QVERIFY2(tabSent.has_value(),
             tabSent ? "" : qPrintable(tabSent.error().diagnostic));
    QCOMPARE(endpoint.calls, 2);
    QCOMPARE(endpoint.actionName, QStringLiteral("new-tab"));
    QCOMPARE(endpoint.parameters.size(), 1);
    QCOMPARE(endpoint.parameters.front().metaType(),
             QMetaType::fromType<QDBusArgument>());
    const QDBusArgument tabArgument =
        endpoint.parameters.front().value<QDBusArgument>();
    QCOMPARE(tabArgument.currentSignature(), QStringLiteral("(tas)"));
    GhosttyNewTabIpcParameter receivedTab;
    tabArgument >> receivedTab;
    QCOMPARE(receivedTab, *newTab.newTabParameter);
    QVERIFY(endpoint.platformData.isEmpty());

    const GhosttyApplicationIpcRequest toggle{
        .applicationId = service,
        .objectPath = path,
        .actionName = QStringLiteral("toggle-quick-terminal"),
        .stringArrayParameter = std::nullopt,
    };
    auto toggleFuture = std::async(std::launch::async, [&] {
        return sendGhosttyApplicationIpcRequest(toggle, bus.client(), 2s);
    });
    QTRY_VERIFY_WITH_TIMEOUT(
        toggleFuture.wait_for(0ms) == std::future_status::ready, 3000);
    const auto toggleSent = toggleFuture.get();
    QVERIFY2(toggleSent.has_value(),
             toggleSent ? "" : qPrintable(toggleSent.error().diagnostic));
    QCOMPARE(endpoint.calls, 3);
    QCOMPARE(endpoint.actionName, QStringLiteral("toggle-quick-terminal"));
    QVERIFY(endpoint.parameters.isEmpty());
    QVERIFY(endpoint.platformData.isEmpty());
}

void GhosttyApplicationIpcTest::reportsDbusFailureWithoutFallback()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    const GhosttyApplicationIpcRequest request{
        .applicationId = service,
        .objectPath = QStringLiteral("/")
            + QString(service).replace(u'.', u'/').replace(u'-', u'_'),
        .actionName = QStringLiteral("toggle-quick-terminal"),
        .stringArrayParameter = std::nullopt,
    };
    const auto sent =
        sendGhosttyApplicationIpcRequest(request, bus.client(), 500ms);
    QVERIFY(!sent.has_value());
    QVERIFY(sent.error().diagnostic.contains(
        QStringLiteral("toggle-quick-terminal")));
    QVERIFY(sent.error().diagnostic.contains(service));
    QCOMPARE(sent.error().exitCode(), 1);
}

QTEST_GUILESS_MAIN(GhosttyApplicationIpcTest)

#include "test_ghostty_application_ipc.moc"
