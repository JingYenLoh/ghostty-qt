#include "ghostty_shell_integration.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <ranges>

namespace {

QString encoded(QByteArrayView value)
{
    return QString::fromLatin1(
        QByteArray(value.data(), value.size()).toBase64());
}

QJsonObject shellCommand(QByteArrayView value, bool defaultShell)
{
    return {
        {QStringLiteral("kind"), QStringLiteral("shell")},
        {QStringLiteral("value"), encoded(value)},
        {QStringLiteral("default-shell"), defaultShell},
    };
}

QJsonObject environmentEntry(QByteArrayView key, QByteArrayView value)
{
    return {
        {QStringLiteral("key"), encoded(key)},
        {QStringLiteral("value"), encoded(value)},
    };
}

QByteArray responseJson(const QJsonObject &command,
                        const QJsonArray &environment,
                        const QJsonValue &shell = QJsonValue::Null)
{
    return QJsonDocument(QJsonObject{
                             {QStringLiteral("version"), 1},
                             {QStringLiteral("command"), command},
                             {QStringLiteral("environment"), environment},
                             {QStringLiteral("shell"), shell},
                         })
        .toJson(QJsonDocument::Compact);
}

bool writeExecutableScript(const QString &path, QByteArrayView contents)
{
    QFile script(path);
    if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || script.write(contents.data(), contents.size()) != contents.size()) {
        return false;
    }
    script.close();
    return QFile::setPermissions(path,
                                 QFileDevice::ReadOwner
                                     | QFileDevice::WriteOwner
                                     | QFileDevice::ExeOwner);
}

} // namespace

class GhosttyShellIntegrationTest final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void serializesByteExactRequest();
    void rejectsInvalidRequest();
    void parsesStrictResponse();
    void rejectsMalformedResponse();
    void resolvesResourceRoots();
    void reportsHelperProcessFailures();
#ifdef GHOSTTY_QT_TEST_REAL_HELPER
    void realHelperSetsFeaturesWhenInjectionDisabled();
    void realHelperRunsPinnedForcedSetup();
    void realHelperUsesStagedBashResources();
#endif
};

void GhosttyShellIntegrationTest::serializesByteExactRequest()
{
    GhosttyShellIntegrationRequest request{
        .command =
            TerminalCommand::shell(QByteArrayLiteral("/bin/zsh -l"), true),
        .environment =
            {
                {
                    .key = QByteArrayLiteral("NON_UTF8"),
                    .value = QByteArray::fromHex("ff807f"),
                },
                {
                    .key = QByteArrayLiteral("EMPTY"),
                    .value = {},
                },
            },
        .mode = GhosttyShellIntegrationMode::Zsh,
        .features =
            {
                .cursor = false,
                .sudo = true,
                .title = false,
                .sshEnvironment = true,
                .sshTerminfo = true,
                .path = false,
            },
        .cursorBlink = false,
        .resourceDirectory = QByteArrayLiteral("/tmp/ghostty resources"),
    };
    auto serialized = serializeGhosttyShellIntegrationRequest(request);
    QVERIFY2(serialized.has_value(),
             serialized ? "" : qPrintable(serialized.error()));

    const QJsonObject root = QJsonDocument::fromJson(*serialized).object();
    QCOMPARE(root.keys().size(), 7);
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 1);
    QCOMPARE(root.value(QStringLiteral("mode")).toString(),
             QStringLiteral("zsh"));
    QCOMPARE(
        QByteArray::fromBase64(
            root.value(QStringLiteral("resource-dir")).toString().toLatin1()),
        request.resourceDirectory);
    const QJsonObject command =
        root.value(QStringLiteral("command")).toObject();
    QCOMPARE(QByteArray::fromBase64(
                 command.value(QStringLiteral("value")).toString().toLatin1()),
             request.command.shellCommand);
    QVERIFY(command.value(QStringLiteral("default-shell")).toBool());
    const QJsonObject features =
        root.value(QStringLiteral("features")).toObject();
    QCOMPARE(features.value(QStringLiteral("sudo")).toBool(), true);
    QCOMPARE(features.value(QStringLiteral("ssh-env")).toBool(), true);
    QCOMPARE(features.value(QStringLiteral("ssh-terminfo")).toBool(), true);
    QCOMPARE(features.value(QStringLiteral("cursor")).toBool(), false);
    QCOMPARE(root.value(QStringLiteral("cursor-blink")).toBool(), false);
}

void GhosttyShellIntegrationTest::rejectsInvalidRequest()
{
    GhosttyShellIntegrationRequest request{
        .command = TerminalCommand::shell(QByteArrayLiteral("zsh"), true),
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
    };
    auto duplicate = serializeGhosttyShellIntegrationRequest(request);
    QVERIFY(!duplicate.has_value());
    QVERIFY(duplicate.error().contains(QStringLiteral("duplicate")));

    request.environment.removeLast();
    request.resourceDirectory = QByteArrayLiteral("relative/resources");
    auto relative = serializeGhosttyShellIntegrationRequest(request);
    QVERIFY(!relative.has_value());
    QVERIFY(relative.error().contains(QStringLiteral("absolute")));

    request.resourceDirectory.clear();
    request.command.shellCommand.append('\0');
    auto embeddedNul = serializeGhosttyShellIntegrationRequest(request);
    QVERIFY(!embeddedNul.has_value());
    QVERIFY(embeddedNul.error().contains(QStringLiteral("shell command")));
}

void GhosttyShellIntegrationTest::parsesStrictResponse()
{
    // Environment values may be empty but cannot contain NUL.
    const QJsonArray environment{
        environmentEntry(QByteArrayLiteral("FEATURES"),
                         QByteArrayLiteral("cursor:steady,title")),
        environmentEntry(QByteArrayLiteral("EMPTY"), QByteArray{}),
    };
    auto parsed = parseGhosttyShellIntegrationResult(
        responseJson(shellCommand(QByteArrayLiteral("zsh"), true), environment,
                     QStringLiteral("zsh")));
    QVERIFY2(parsed.has_value(), parsed ? "" : qPrintable(parsed.error()));
    QCOMPARE(parsed->command,
             TerminalCommand::shell(QByteArrayLiteral("zsh"), true));
    QCOMPARE(parsed->environment.size(), 2);
    QCOMPARE(parsed->environment.at(1).value, QByteArray{});
    QCOMPARE(parsed->shell, GhosttyIntegratedShell::Zsh);
}

void GhosttyShellIntegrationTest::rejectsMalformedResponse()
{
    QJsonObject root =
        QJsonDocument::fromJson(
            responseJson(shellCommand(QByteArrayLiteral("zsh"), true), {}))
            .object();
    root.insert(QStringLiteral("unexpected"), true);
    auto unknown = parseGhosttyShellIntegrationResult(
        QJsonDocument(root).toJson(QJsonDocument::Compact));
    QVERIFY(!unknown.has_value());
    QVERIFY(unknown.error().contains(QStringLiteral("field set")));

    QJsonObject command = shellCommand(QByteArrayLiteral("zsh"), true);
    command.insert(QStringLiteral("value"), QStringLiteral("eg"));
    auto nonCanonical =
        parseGhosttyShellIntegrationResult(responseJson(command, {}));
    QVERIFY(!nonCanonical.has_value());
    QVERIFY(nonCanonical.error().contains(QStringLiteral("canonical")));

    auto duplicateEnvironment = parseGhosttyShellIntegrationResult(
        responseJson(shellCommand(QByteArrayLiteral("zsh"), true),
                     {
                         environmentEntry(QByteArrayLiteral("DUP"),
                                          QByteArrayLiteral("one")),
                         environmentEntry(QByteArrayLiteral("DUP"),
                                          QByteArrayLiteral("two")),
                     }));
    QVERIFY(!duplicateEnvironment.has_value());
    QVERIFY(duplicateEnvironment.error().contains(QStringLiteral("duplicate")));
}

void GhosttyShellIntegrationTest::resolvesResourceRoots()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString applicationDirectory =
        temporary.filePath(QStringLiteral("build"));
    QVERIFY(QDir().mkpath(QDir(applicationDirectory)
                              .filePath(QStringLiteral("shell-integration"))));

    auto build =
        resolveShellIntegrationResourceDirectory(applicationDirectory, {});
    QVERIFY2(build.has_value(), build ? "" : qPrintable(build.error()));
    QCOMPARE(*build, QFileInfo(applicationDirectory).canonicalFilePath());

    const QString overrideDirectory =
        temporary.filePath(QStringLiteral("override"));
    QVERIFY(QDir().mkpath(
        QDir(overrideDirectory).filePath(QStringLiteral("shell-integration"))));
    auto override = resolveShellIntegrationResourceDirectory(
        applicationDirectory, overrideDirectory);
    QVERIFY2(override.has_value(),
             override ? "" : qPrintable(override.error()));
    QCOMPARE(*override, QFileInfo(overrideDirectory).canonicalFilePath());

    auto invalidOverride = resolveShellIntegrationResourceDirectory(
        applicationDirectory, temporary.filePath(QStringLiteral("missing")));
    QVERIFY(!invalidOverride.has_value());
    QVERIFY(invalidOverride.error().contains(
        QStringLiteral("GHOSTTY_QT_SHELL_INTEGRATION_RESOURCES")));

    const QString installedApplicationDirectory =
        temporary.filePath(QStringLiteral("relocated/bin"));
    const QString installedRoot =
        QDir(installedApplicationDirectory)
            .absoluteFilePath(
                QStringLiteral(GHOSTTY_QT_TEST_INSTALL_RESOURCES_RELATIVE_DIR));
    QVERIFY(QDir().mkpath(
        QDir(installedRoot).filePath(QStringLiteral("shell-integration"))));
    auto installed = resolveShellIntegrationResourceDirectory(
        installedApplicationDirectory, {});
    QVERIFY2(installed.has_value(),
             installed ? "" : qPrintable(installed.error()));
    QCOMPARE(*installed, QFileInfo(installedRoot).canonicalFilePath());
}

void GhosttyShellIntegrationTest::reportsHelperProcessFailures()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const GhosttyShellIntegrationRequest request{
        .command = TerminalCommand::shell(QByteArrayLiteral("sh"), true),
        .mode = GhosttyShellIntegrationMode::None,
    };

    auto missing = prepareGhosttyShellIntegration(
        {
            .helperPath = temporary.filePath(QStringLiteral("missing-helper")),
            .timeoutMilliseconds = 100,
        },
        request);
    QVERIFY(!missing.has_value());
    QVERIFY(missing.error().contains(QStringLiteral("could not be started")));

    const QString failingHelper =
        temporary.filePath(QStringLiteral("failing-helper"));
    QVERIFY(
        writeExecutableScript(failingHelper,
                              QByteArrayLiteral("#!/bin/sh\n"
                                                "printf 'fixture failure' >&2\n"
                                                "exit 7\n")));
    auto failed = prepareGhosttyShellIntegration(
        {
            .helperPath = failingHelper,
            .timeoutMilliseconds = 1'000,
        },
        request);
    QVERIFY(!failed.has_value());
    QVERIFY(failed.error().contains(QStringLiteral("exit code 7")));
    QVERIFY(failed.error().contains(QStringLiteral("fixture failure")));

    const QString hangingHelper =
        temporary.filePath(QStringLiteral("hanging-helper"));
    QVERIFY(writeExecutableScript(hangingHelper,
                                  QByteArrayLiteral("#!/bin/sh\n"
                                                    "exec sleep 5\n")));
    auto timedOut = prepareGhosttyShellIntegration(
        {
            .helperPath = hangingHelper,
            .timeoutMilliseconds = 50,
        },
        request);
    QVERIFY(!timedOut.has_value());
    QVERIFY(timedOut.error().contains(QStringLiteral("timed out")));

    const QString excessiveHelper =
        temporary.filePath(QStringLiteral("excessive-helper"));
    QVERIFY(writeExecutableScript(
        excessiveHelper,
        QByteArrayLiteral("#!/bin/sh\n"
                          "dd if=/dev/zero bs=1048576 count=9 2>/dev/null\n")));
    auto excessive = prepareGhosttyShellIntegration(
        {
            .helperPath = excessiveHelper,
            .timeoutMilliseconds = 2'000,
        },
        request);
    QVERIFY(!excessive.has_value());
    QVERIFY(excessive.error().contains(
        QStringLiteral("exceeds the 8 MiB protocol limit")));
}

#ifdef GHOSTTY_QT_TEST_REAL_HELPER
void GhosttyShellIntegrationTest::realHelperSetsFeaturesWhenInjectionDisabled()
{
    GhosttyShellIntegrationRequest request{
        .command = TerminalCommand::shell(QByteArrayLiteral("zsh"), true),
        .environment = {},
        .mode = GhosttyShellIntegrationMode::None,
        .features =
            {
                .cursor = true,
                .sudo = false,
                .title = true,
                .sshEnvironment = false,
                .sshTerminfo = false,
                .path = true,
            },
        .cursorBlink = false,
    };
    auto prepared = prepareGhosttyShellIntegration(
        {
            .helperPath = QStringLiteral(GHOSTTY_QT_TEST_REAL_HELPER),
        },
        request);
    QVERIFY2(prepared.has_value(),
             prepared ? "" : qPrintable(prepared.error()));
    QCOMPARE(prepared->command, request.command);
    QVERIFY(!prepared->shell.has_value());
    const auto features = std::ranges::find(
        prepared->environment, QByteArrayLiteral("GHOSTTY_SHELL_FEATURES"),
        &TerminalEnvironmentEntry::key);
    QVERIFY(features != prepared->environment.cend());
    QCOMPARE(features->value, QByteArrayLiteral("cursor:steady,path,title"));
}

void GhosttyShellIntegrationTest::realHelperRunsPinnedForcedSetup()
{
    QTemporaryDir resources;
    QVERIFY(resources.isValid());
    QVERIFY(QDir().mkpath(
        resources.filePath(QStringLiteral("shell-integration/zsh"))));

    GhosttyShellIntegrationRequest request{
        .command = TerminalCommand::shell(QByteArrayLiteral("zsh"), true),
        .environment = {{
            .key = QByteArrayLiteral("ZDOTDIR"),
            .value = QByteArrayLiteral("/original/zsh"),
        }},
        .mode = GhosttyShellIntegrationMode::Zsh,
        .resourceDirectory = QFile::encodeName(resources.path()),
    };
    auto prepared = prepareGhosttyShellIntegration(
        {
            .helperPath = QStringLiteral(GHOSTTY_QT_TEST_REAL_HELPER),
        },
        request);
    QVERIFY2(prepared.has_value(),
             prepared ? "" : qPrintable(prepared.error()));
    QCOMPARE(prepared->shell, GhosttyIntegratedShell::Zsh);
    const auto zdotdir =
        std::ranges::find(prepared->environment, QByteArrayLiteral("ZDOTDIR"),
                          &TerminalEnvironmentEntry::key);
    QVERIFY(zdotdir != prepared->environment.cend());
    QCOMPARE(zdotdir->value,
             QFile::encodeName(
                 resources.filePath(QStringLiteral("shell-integration/zsh"))));
    const auto preserved = std::ranges::find(
        prepared->environment, QByteArrayLiteral("GHOSTTY_ZSH_ZDOTDIR"),
        &TerminalEnvironmentEntry::key);
    QVERIFY(preserved != prepared->environment.cend());
    QCOMPARE(preserved->value, QByteArrayLiteral("/original/zsh"));
}

void GhosttyShellIntegrationTest::realHelperUsesStagedBashResources()
{
    const QString resources = QStringLiteral(GHOSTTY_QT_TEST_RESOURCES);
    GhosttyShellIntegrationRequest request{
        .command = TerminalCommand::shell(QByteArrayLiteral("bash"), true),
        .mode = GhosttyShellIntegrationMode::Bash,
        .resourceDirectory = QFile::encodeName(resources),
    };
    auto prepared = prepareGhosttyShellIntegration(
        {
            .helperPath = QStringLiteral(GHOSTTY_QT_TEST_REAL_HELPER),
        },
        request);
    QVERIFY2(prepared.has_value(),
             prepared ? "" : qPrintable(prepared.error()));
    QCOMPARE(prepared->shell, GhosttyIntegratedShell::Bash);
    QCOMPARE(prepared->command,
             TerminalCommand::shell(QByteArrayLiteral("bash --posix"), true));

    const auto environmentValue =
        [&prepared](QByteArrayView key) -> QByteArray {
        const auto entry = std::ranges::find(prepared->environment,
                                             QByteArray(key.data(), key.size()),
                                             &TerminalEnvironmentEntry::key);
        return entry == prepared->environment.cend() ? QByteArray{}
                                                     : entry->value;
    };
    QCOMPARE(environmentValue(QByteArrayLiteral("ENV")),
             QFile::encodeName(QDir(resources).filePath(
                 QStringLiteral("shell-integration/bash/ghostty.bash"))));
    QCOMPARE(environmentValue(QByteArrayLiteral("GHOSTTY_BASH_INJECT")),
             QByteArrayLiteral("1"));
}
#endif

QTEST_GUILESS_MAIN(GhosttyShellIntegrationTest)

#include "test_ghostty_shell_integration.moc"
