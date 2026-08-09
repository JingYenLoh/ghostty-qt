#include "ghostty_shell_integration.h"
#include "ghostty_shell_integration_p.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <barrier>
#include <optional>
#include <ranges>
#include <thread>
#include <utility>
#include <vector>

namespace {

using ShellIntegrationPreparation =
    std::expected<GhosttyShellIntegrationResult, QString>;

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

bool writeBytes(const QString &path, QByteArrayView contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(contents.data(), contents.size()) == contents.size();
}

qsizetype invocationCount(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return 0;
    return file.readAll().size();
}

QByteArray cacheFixtureScript(QByteArrayView suffix = {})
{
    QByteArray result =
        QByteArrayLiteral("#!/bin/sh\n"
                          "IFS= read -r request || :\n"
                          "printf x >> \"$GHOSTTY_QT_TEST_COUNT\"\n"
                          "if [ -n \"$GHOSTTY_QT_TEST_STARTED\" ]; then\n"
                          "  printf x >> \"$GHOSTTY_QT_TEST_STARTED\"\n"
                          "  while [ ! -e \"$GHOSTTY_QT_TEST_RELEASE\" ]; do\n"
                          "    /usr/bin/sleep 0.01\n"
                          "  done\n"
                          "fi\n"
                          "if [ -n \"$GHOSTTY_QT_TEST_EXIT\" ]; then\n"
                          "  printf 'fixture cached failure' >&2\n"
                          "  exit \"$GHOSTTY_QT_TEST_EXIT\"\n"
                          "fi\n"
                          "/usr/bin/cat \"$GHOSTTY_QT_TEST_RESPONSE\"\n");
    result.append(suffix.data(), suffix.size());
    return result;
}

GhosttyShellIntegrationProcessOptions
cacheFixtureOptions(const QString &helper, const QString &response,
                    const QString &count, QString started = {},
                    QString release = {}, QString exitCode = {},
                    int timeout = 2'000)
{
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("HOME"), QStringLiteral("/tmp"));
    environment.insert(QStringLiteral("GHOSTTY_QT_TEST_RESPONSE"), response);
    environment.insert(QStringLiteral("GHOSTTY_QT_TEST_COUNT"), count);
    if (!started.isEmpty()) {
        environment.insert(QStringLiteral("GHOSTTY_QT_TEST_STARTED"), started);
        environment.insert(QStringLiteral("GHOSTTY_QT_TEST_RELEASE"), release);
    }
    if (!exitCode.isEmpty()) {
        environment.insert(QStringLiteral("GHOSTTY_QT_TEST_EXIT"), exitCode);
    }
    return {
        .helperPath = helper,
        .environment = environment,
        .timeoutMilliseconds = timeout,
    };
}

GhosttyShellIntegrationRequest cacheFixtureRequest()
{
    return {
        .command = TerminalCommand::shell(QByteArrayLiteral("sh"), true),
        .mode = GhosttyShellIntegrationMode::None,
    };
}

bool prepareCacheFixture(const QTemporaryDir &temporary, QString *helper,
                         QString *response, QString *count)
{
    *helper = temporary.filePath(QStringLiteral("cache-helper"));
    *response = temporary.filePath(QStringLiteral("response.json"));
    *count = temporary.filePath(QStringLiteral("invocations"));
    return writeExecutableScript(*helper, cacheFixtureScript())
        && writeBytes(
               *response,
               responseJson(shellCommand(QByteArrayLiteral("sh"), true), {}))
        && setGhosttyShellIntegrationTrustedHelperForTest(*helper);
}

class ScopedGateRelease final {
public:
    explicit ScopedGateRelease(QString path)
        : path_(std::move(path))
    {}

    ~ScopedGateRelease() { release(); }

    bool release()
    {
        if (released_) return true;
        released_ = writeBytes(path_, QByteArrayView{});
        return released_;
    }

private:
    QString path_;
    bool released_ = false;
};

} // namespace

class GhosttyShellIntegrationTest final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init();
    void serializesByteExactRequest();
    void rejectsInvalidRequest();
    void parsesStrictResponse();
    void rejectsMalformedResponse();
    void resolvesResourceRoots();
    void reportsHelperProcessFailures();
    void cachesSequentialEquivalentPreparations();
    void invalidatesBehaviorAffectingIdentities();
    void coalescesConcurrentEquivalentPreparations();
    void launchesConcurrentDistinctPreparations();
    void fansOutFailureWithoutRetainingIt();
    void rejectsPreparationWhenIdentityChangesInFlight();
    void boundsSuccessfulEntriesAndPayloads();
    void bypassesUntrustedProcessIdentity();
#ifdef GHOSTTY_QT_TEST_REAL_HELPER
    void realHelperCachesThroughCanonicalSymlink();
    void realHelperSetsFeaturesWhenInjectionDisabled();
    void realHelperRunsPinnedForcedSetup();
    void realHelperUsesStagedBashResources();
#endif
};

void GhosttyShellIntegrationTest::init()
{
    QVERIFY(resetGhosttyShellIntegrationCacheForTest());
}

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

void GhosttyShellIntegrationTest::cachesSequentialEquivalentPreparations()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QString helper;
    QString response;
    QString count;
    QVERIFY(prepareCacheFixture(temporary, &helper, &response, &count));

    const GhosttyShellIntegrationRequest request = cacheFixtureRequest();
    const GhosttyShellIntegrationProcessOptions options =
        cacheFixtureOptions(helper, response, count);
    auto first = prepareCachedGhosttyShellIntegration(options, request);
    QVERIFY2(first.has_value(), first ? "" : qPrintable(first.error()));
    const auto second = prepareCachedGhosttyShellIntegration(options, request);
    QVERIFY2(second.has_value(), second ? "" : qPrintable(second.error()));
    QVERIFY(*second == *first);

    first->environment.append({
        .key = QByteArrayLiteral("CALLER_MUTATION"),
        .value = QByteArrayLiteral("must detach"),
    });
    const auto isolated =
        prepareCachedGhosttyShellIntegration(options, request);
    QVERIFY2(isolated.has_value(),
             isolated ? "" : qPrintable(isolated.error()));
    QVERIFY(isolated->environment == second->environment);

    GhosttyShellIntegrationProcessOptions reordered = options;
    QProcessEnvironment reverseEnvironment;
    reverseEnvironment.insert(QStringLiteral("GHOSTTY_QT_TEST_COUNT"), count);
    reverseEnvironment.insert(QStringLiteral("GHOSTTY_QT_TEST_RESPONSE"),
                              response);
    reverseEnvironment.insert(QStringLiteral("HOME"), QStringLiteral("/tmp"));
    reordered.environment = reverseEnvironment;
    QVERIFY(
        prepareCachedGhosttyShellIntegration(reordered, request).has_value());
    QCOMPARE(invocationCount(count), 1);

    const auto snapshot = ghosttyShellIntegrationCacheSnapshotForTest();
    QCOMPARE(snapshot.hits, 3);
    QCOMPARE(snapshot.misses, 1);
    QCOMPARE(snapshot.launches, 1);
    QCOMPARE(snapshot.insertions, 1);
    QCOMPARE(snapshot.entries, 1);
    QVERIFY(snapshot.retainedBytes > 0);
}

void GhosttyShellIntegrationTest::invalidatesBehaviorAffectingIdentities()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QString helper;
    QString response;
    QString count;
    QVERIFY(prepareCacheFixture(temporary, &helper, &response, &count));

    GhosttyShellIntegrationRequest request = cacheFixtureRequest();
    GhosttyShellIntegrationProcessOptions options =
        cacheFixtureOptions(helper, response, count);
    QVERIFY(prepareCachedGhosttyShellIntegration(options, request).has_value());
    QVERIFY(prepareCachedGhosttyShellIntegration(options, request).has_value());
    QCOMPARE(invocationCount(count), 1);

    request.features.cursor = !request.features.cursor;
    QVERIFY(prepareCachedGhosttyShellIntegration(options, request).has_value());
    QCOMPARE(invocationCount(count), 2);

    options.environment.insert(QStringLiteral("UNUSED_FIXTURE_KEY"),
                               QStringLiteral("one"));
    QVERIFY(prepareCachedGhosttyShellIntegration(options, request).has_value());
    QCOMPARE(invocationCount(count), 3);

    ++options.timeoutMilliseconds;
    QVERIFY(prepareCachedGhosttyShellIntegration(options, request).has_value());
    QCOMPARE(invocationCount(count), 4);

    QVERIFY(writeExecutableScript(
        helper, cacheFixtureScript(QByteArrayLiteral("# replacement\n"))));
    QVERIFY(prepareCachedGhosttyShellIntegration(options, request).has_value());
    QCOMPARE(invocationCount(count), 5);

    const QString integration =
        temporary.filePath(QStringLiteral("resources/shell-integration"));
    QVERIFY(QDir().mkpath(QDir(integration).filePath(QStringLiteral("bash"))));
    QVERIFY(QDir().mkpath(QDir(integration).filePath(QStringLiteral("zsh"))));
    const QString bashScript =
        QDir(integration).filePath(QStringLiteral("bash/ghostty.bash"));
    QVERIFY(writeBytes(bashScript, QByteArrayLiteral("first")));
    request.mode = GhosttyShellIntegrationMode::Zsh;
    request.resourceDirectory =
        QFile::encodeName(temporary.filePath(QStringLiteral("resources")));
    QVERIFY(prepareCachedGhosttyShellIntegration(options, request).has_value());
    QVERIFY(prepareCachedGhosttyShellIntegration(options, request).has_value());
    QCOMPARE(invocationCount(count), 6);

    QVERIFY(QDir(integration).rmdir(QStringLiteral("zsh")));
    QVERIFY(prepareCachedGhosttyShellIntegration(options, request).has_value());
    QCOMPARE(invocationCount(count), 7);
    QVERIFY(QDir().mkpath(QDir(integration).filePath(QStringLiteral("zsh"))));
    QVERIFY(prepareCachedGhosttyShellIntegration(options, request).has_value());
    QCOMPARE(invocationCount(count), 8);
}

void GhosttyShellIntegrationTest::coalescesConcurrentEquivalentPreparations()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QString helper;
    QString response;
    QString count;
    QVERIFY(prepareCacheFixture(temporary, &helper, &response, &count));
    const QString started = temporary.filePath(QStringLiteral("started"));
    const QString release = temporary.filePath(QStringLiteral("release"));

    constexpr qsizetype ThreadCount = 8;
    const GhosttyShellIntegrationRequest request = cacheFixtureRequest();
    const GhosttyShellIntegrationProcessOptions options = cacheFixtureOptions(
        helper, response, count, started, release, {}, 5'000);
    std::barrier<> start(static_cast<std::ptrdiff_t>(ThreadCount));
    std::vector<std::optional<ShellIntegrationPreparation>> outcomes(
        static_cast<size_t>(ThreadCount));
    std::vector<std::jthread> threads;
    threads.reserve(static_cast<size_t>(ThreadCount));
    ScopedGateRelease gate(release);
    for (qsizetype index = 0; index < ThreadCount; ++index) {
        threads.emplace_back([&, index] {
            start.arrive_and_wait();
            outcomes.at(static_cast<size_t>(index)) =
                prepareCachedGhosttyShellIntegration(options, request);
        });
    }
    QTRY_VERIFY_WITH_TIMEOUT(
        ghosttyShellIntegrationCacheSnapshotForTest().coalesced
                == ThreadCount - 1
            && ghosttyShellIntegrationCacheSnapshotForTest().inFlight == 1,
        3'000);
    QVERIFY(gate.release());
    threads.clear();

    QCOMPARE(invocationCount(count), 1);
    for (const auto &outcome : outcomes) {
        QVERIFY(outcome.has_value());
        QVERIFY2(outcome->has_value(),
                 outcome->has_value() ? "" : qPrintable(outcome->error()));
    }
    const auto snapshot = ghosttyShellIntegrationCacheSnapshotForTest();
    QCOMPARE(snapshot.misses, 1);
    QCOMPARE(snapshot.coalesced, 7);
    QCOMPARE(snapshot.launches, 1);
    QCOMPARE(snapshot.insertions, 1);
    QCOMPARE(snapshot.inFlight, 0);
}

void GhosttyShellIntegrationTest::launchesConcurrentDistinctPreparations()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QString helper;
    QString response;
    QString count;
    QVERIFY(prepareCacheFixture(temporary, &helper, &response, &count));
    const QString started = temporary.filePath(QStringLiteral("started"));
    const QString release = temporary.filePath(QStringLiteral("release"));

    constexpr qsizetype ThreadCount = 8;
    QVector<GhosttyShellIntegrationRequest> requests;
    requests.reserve(ThreadCount);
    for (qsizetype index = 0; index < ThreadCount; ++index) {
        GhosttyShellIntegrationRequest request = cacheFixtureRequest();
        request.environment.append({
            .key = QByteArrayLiteral("DISTINCT"),
            .value = QByteArray::number(index),
        });
        requests.append(std::move(request));
    }
    const GhosttyShellIntegrationProcessOptions options = cacheFixtureOptions(
        helper, response, count, started, release, {}, 5'000);
    std::barrier<> start(static_cast<std::ptrdiff_t>(ThreadCount));
    std::vector<std::optional<ShellIntegrationPreparation>> outcomes(
        static_cast<size_t>(ThreadCount));
    std::vector<std::jthread> threads;
    threads.reserve(static_cast<size_t>(ThreadCount));
    ScopedGateRelease gate(release);
    for (qsizetype index = 0; index < ThreadCount; ++index) {
        threads.emplace_back([&, index] {
            start.arrive_and_wait();
            outcomes.at(static_cast<size_t>(index)) =
                prepareCachedGhosttyShellIntegration(options,
                                                     requests.at(index));
        });
    }
    QTRY_VERIFY_WITH_TIMEOUT(
        invocationCount(started) == ThreadCount
            && ghosttyShellIntegrationCacheSnapshotForTest().inFlight
                == ThreadCount,
        3'000);
    QVERIFY(gate.release());
    threads.clear();

    QCOMPARE(invocationCount(count), ThreadCount);
    for (const auto &outcome : outcomes) {
        QVERIFY(outcome.has_value());
        QVERIFY(outcome->has_value());
    }
    const auto snapshot = ghosttyShellIntegrationCacheSnapshotForTest();
    QCOMPARE(snapshot.misses, 8);
    QCOMPARE(snapshot.coalesced, 0);
    QCOMPARE(snapshot.launches, 8);
    QCOMPARE(snapshot.entries, 8);
}

void GhosttyShellIntegrationTest::fansOutFailureWithoutRetainingIt()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QString helper;
    QString response;
    QString count;
    QVERIFY(prepareCacheFixture(temporary, &helper, &response, &count));
    const QString started = temporary.filePath(QStringLiteral("started"));
    const QString release = temporary.filePath(QStringLiteral("release"));

    constexpr qsizetype ThreadCount = 8;
    const GhosttyShellIntegrationRequest request = cacheFixtureRequest();
    const GhosttyShellIntegrationProcessOptions options = cacheFixtureOptions(
        helper, response, count, started, release, QStringLiteral("7"), 5'000);
    std::barrier<> start(static_cast<std::ptrdiff_t>(ThreadCount));
    std::vector<std::optional<ShellIntegrationPreparation>> outcomes(
        static_cast<size_t>(ThreadCount));
    std::vector<std::jthread> threads;
    threads.reserve(static_cast<size_t>(ThreadCount));
    ScopedGateRelease gate(release);
    for (qsizetype index = 0; index < ThreadCount; ++index) {
        threads.emplace_back([&, index] {
            start.arrive_and_wait();
            outcomes.at(static_cast<size_t>(index)) =
                prepareCachedGhosttyShellIntegration(options, request);
        });
    }
    QTRY_VERIFY_WITH_TIMEOUT(
        ghosttyShellIntegrationCacheSnapshotForTest().coalesced
                == ThreadCount - 1
            && ghosttyShellIntegrationCacheSnapshotForTest().inFlight == 1,
        3'000);
    QVERIFY(gate.release());
    threads.clear();

    QCOMPARE(invocationCount(count), 1);
    for (const auto &outcome : outcomes) {
        QVERIFY(outcome.has_value());
        QVERIFY(!outcome->has_value());
        QVERIFY(outcome->error().contains(QStringLiteral("exit code 7")));
    }
    const auto retry = prepareCachedGhosttyShellIntegration(options, request);
    QVERIFY(!retry.has_value());
    QCOMPARE(invocationCount(count), 2);

    const auto snapshot = ghosttyShellIntegrationCacheSnapshotForTest();
    QCOMPARE(snapshot.misses, 2);
    QCOMPARE(snapshot.coalesced, 7);
    QCOMPARE(snapshot.launches, 2);
    QCOMPARE(snapshot.entries, 0);
}

void GhosttyShellIntegrationTest::
    rejectsPreparationWhenIdentityChangesInFlight()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QString helper;
    QString response;
    QString count;
    QVERIFY(prepareCacheFixture(temporary, &helper, &response, &count));
    const QString started = temporary.filePath(QStringLiteral("started"));
    const QString release = temporary.filePath(QStringLiteral("release"));

    const QString integration =
        temporary.filePath(QStringLiteral("resources/shell-integration"));
    QVERIFY(QDir().mkpath(QDir(integration).filePath(QStringLiteral("bash"))));
    QVERIFY(QDir().mkpath(QDir(integration).filePath(QStringLiteral("zsh"))));
    QVERIFY(writeBytes(
        QDir(integration).filePath(QStringLiteral("bash/ghostty.bash")),
        QByteArrayLiteral("fixture")));

    GhosttyShellIntegrationRequest request = cacheFixtureRequest();
    request.mode = GhosttyShellIntegrationMode::Zsh;
    request.resourceDirectory =
        QFile::encodeName(temporary.filePath(QStringLiteral("resources")));
    const GhosttyShellIntegrationProcessOptions options = cacheFixtureOptions(
        helper, response, count, started, release, {}, 5'000);

    std::optional<ShellIntegrationPreparation> outcome;
    std::jthread worker;
    ScopedGateRelease gate(release);
    worker = std::jthread([&] {
        outcome = prepareCachedGhosttyShellIntegration(options, request);
    });
    QTRY_VERIFY_WITH_TIMEOUT(invocationCount(started) == 1, 3'000);
    QVERIFY(QDir(integration).rmdir(QStringLiteral("zsh")));
    QVERIFY(gate.release());
    worker.join();

    QVERIFY(outcome.has_value());
    QVERIFY2(outcome->has_value(),
             outcome->has_value() ? "" : qPrintable(outcome->error()));
    auto snapshot = ghosttyShellIntegrationCacheSnapshotForTest();
    QCOMPARE(snapshot.unstableIdentities, 1);
    QCOMPARE(snapshot.insertions, 0);
    QCOMPARE(snapshot.entries, 0);

    QVERIFY(prepareCachedGhosttyShellIntegration(options, request).has_value());
    QCOMPARE(invocationCount(count), 2);
    snapshot = ghosttyShellIntegrationCacheSnapshotForTest();
    QCOMPARE(snapshot.insertions, 1);
    QCOMPARE(snapshot.entries, 1);
}

void GhosttyShellIntegrationTest::boundsSuccessfulEntriesAndPayloads()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QString helper;
    QString response;
    QString count;
    QVERIFY(prepareCacheFixture(temporary, &helper, &response, &count));

    const GhosttyShellIntegrationProcessOptions options =
        cacheFixtureOptions(helper, response, count);
    QVector<GhosttyShellIntegrationRequest> requests;
    requests.reserve(33);
    for (int index = 0; index < 33; ++index) {
        GhosttyShellIntegrationRequest request = cacheFixtureRequest();
        request.environment.append({
            .key = QByteArrayLiteral("CACHE_VARIANT"),
            .value = QByteArray::number(index),
        });
        requests.append(request);
    }
    for (int index = 0; index < 32; ++index) {
        QVERIFY(
            prepareCachedGhosttyShellIntegration(options, requests.at(index))
                .has_value());
    }
    QCOMPARE(invocationCount(count), 32);
    auto snapshot = ghosttyShellIntegrationCacheSnapshotForTest();
    QCOMPARE(snapshot.entries, 32);
    QCOMPARE(snapshot.evictions, 0);

    QVERIFY(prepareCachedGhosttyShellIntegration(options, requests.constFirst())
                .has_value());
    QVERIFY(prepareCachedGhosttyShellIntegration(options, requests.constLast())
                .has_value());
    QCOMPARE(invocationCount(count), 33);
    snapshot = ghosttyShellIntegrationCacheSnapshotForTest();
    QCOMPARE(snapshot.entries, 32);
    QCOMPARE(snapshot.evictions, 1);

    QVERIFY(prepareCachedGhosttyShellIntegration(options, requests.constFirst())
                .has_value());
    QCOMPARE(invocationCount(count), 33);
    QVERIFY(prepareCachedGhosttyShellIntegration(options, requests.at(1))
                .has_value());
    QCOMPARE(invocationCount(count), 34);
    snapshot = ghosttyShellIntegrationCacheSnapshotForTest();
    QCOMPARE(snapshot.evictions, 2);

    QVERIFY(resetGhosttyShellIntegrationCacheForTest());
    QVERIFY(setGhosttyShellIntegrationTrustedHelperForTest(helper));
    QVERIFY(writeBytes(count, QByteArrayView{}));
    const QByteArray budgetValue(900 * 1024, 'b');
    QVERIFY(writeBytes(
        response,
        responseJson(
            shellCommand(QByteArrayLiteral("sh"), true),
            {environmentEntry(QByteArrayLiteral("BUDGET"), budgetValue)})));
    for (int index = 0; index < 10; ++index) {
        QVERIFY(
            prepareCachedGhosttyShellIntegration(options, requests.at(index))
                .has_value());
    }
    QCOMPARE(invocationCount(count), 10);
    snapshot = ghosttyShellIntegrationCacheSnapshotForTest();
    QVERIFY(snapshot.entries < 10);
    QVERIFY(snapshot.evictions > 0);
    QVERIFY(snapshot.retainedBytes <= 8 * 1024 * 1024);

    QVERIFY(resetGhosttyShellIntegrationCacheForTest());
    QVERIFY(setGhosttyShellIntegrationTrustedHelperForTest(helper));
    QVERIFY(writeBytes(count, QByteArrayView{}));
    const QByteArray largeValue(1024 * 1024 + 4'096, 'x');
    QVERIFY(writeBytes(
        response,
        responseJson(
            shellCommand(QByteArrayLiteral("sh"), true),
            {environmentEntry(QByteArrayLiteral("LARGE"), largeValue)})));
    QVERIFY(prepareCachedGhosttyShellIntegration(options, requests.constFirst())
                .has_value());
    QVERIFY(prepareCachedGhosttyShellIntegration(options, requests.constFirst())
                .has_value());
    QCOMPARE(invocationCount(count), 2);
    snapshot = ghosttyShellIntegrationCacheSnapshotForTest();
    QCOMPARE(snapshot.entries, 0);
    QCOMPARE(snapshot.oversizedResults, 2);
    QCOMPARE(snapshot.launches, 2);
}

void GhosttyShellIntegrationTest::bypassesUntrustedProcessIdentity()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QString helper;
    QString response;
    QString count;
    QVERIFY(prepareCacheFixture(temporary, &helper, &response, &count));

    GhosttyShellIntegrationProcessOptions options =
        cacheFixtureOptions(helper, response, count);
    options.environment.insert(QStringLiteral("LD_PRELOAD"), QString{});
    const GhosttyShellIntegrationRequest request = cacheFixtureRequest();
    QVERIFY(prepareCachedGhosttyShellIntegration(options, request).has_value());
    QVERIFY(prepareCachedGhosttyShellIntegration(options, request).has_value());
    QCOMPARE(invocationCount(count), 2);
    const auto snapshot = ghosttyShellIntegrationCacheSnapshotForTest();
    QCOMPARE(snapshot.bypasses, 2);
    QCOMPARE(snapshot.launches, 2);
    QCOMPARE(snapshot.entries, 0);

    QVERIFY(resetGhosttyShellIntegrationCacheForTest());
    QVERIFY(writeBytes(count, QByteArrayView{}));
    options.environment.remove(QStringLiteral("LD_PRELOAD"));
    QVERIFY(prepareCachedGhosttyShellIntegration(options, request).has_value());
    QVERIFY(prepareCachedGhosttyShellIntegration(options, request).has_value());
    QCOMPARE(invocationCount(count), 2);
    const auto unrecognized = ghosttyShellIntegrationCacheSnapshotForTest();
    QCOMPARE(unrecognized.bypasses, 2);
    QCOMPARE(unrecognized.entries, 0);
}

#ifdef GHOSTTY_QT_TEST_REAL_HELPER
void GhosttyShellIntegrationTest::realHelperCachesThroughCanonicalSymlink()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString helper =
        temporary.filePath(QStringLiteral("ghostty-qt-config-helper"));
    QVERIFY(QFile::link(QStringLiteral(GHOSTTY_QT_TEST_REAL_HELPER), helper));

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("LD_PRELOAD"));
    environment.remove(QStringLiteral("LD_LIBRARY_PATH"));
    environment.remove(QStringLiteral("LD_AUDIT"));
    const GhosttyShellIntegrationProcessOptions options{
        .helperPath = helper,
        .environment = environment,
    };
    const GhosttyShellIntegrationRequest request = cacheFixtureRequest();
    const auto first = prepareCachedGhosttyShellIntegration(options, request);
    QVERIFY2(first.has_value(), first ? "" : qPrintable(first.error()));
    const auto second = prepareCachedGhosttyShellIntegration(options, request);
    QVERIFY2(second.has_value(), second ? "" : qPrintable(second.error()));
    QCOMPARE(*second, *first);

    const auto snapshot = ghosttyShellIntegrationCacheSnapshotForTest();
    QCOMPARE(snapshot.misses, 1);
    QCOMPARE(snapshot.hits, 1);
    QCOMPARE(snapshot.launches, 1);
    QCOMPARE(snapshot.bypasses, 0);
    QCOMPARE(snapshot.entries, 1);
}

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
