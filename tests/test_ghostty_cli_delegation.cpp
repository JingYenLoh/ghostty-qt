#include "ghostty_cli_delegation.h"

#include <QByteArrayView>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <array>
#include <csignal>
#include <expected>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/stat.h>

namespace {

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

    [[nodiscard]] std::span<char *const> span() noexcept
    {
        return pointers_;
    }

private:
    std::vector<std::string> storage_;
    std::vector<char *> pointers_;
};

struct ProcessResult final {
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    int exitCode = -1;
    qint64 processId = 0;
    QByteArray standardOutput;
    QByteArray standardError;
};

std::expected<ProcessResult, QString> runProcess(
    const QString &program,
    const QStringList &arguments,
    const QProcessEnvironment &environment,
    const QString &workingDirectory,
    const QByteArray &standardInput = {})
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(workingDirectory);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(10'000)) {
        return std::unexpected(
            QStringLiteral("process did not start: %1")
                .arg(process.errorString()));
    }
    const qint64 processId = process.processId();
    const auto cleanup = qScopeGuard([&process] {
        if (process.state() == QProcess::NotRunning) return;
        process.kill();
        process.waitForFinished(3'000);
    });
    if (!standardInput.isEmpty()) {
        if (process.write(standardInput) != standardInput.size()
            || !process.waitForBytesWritten(10'000)) {
            return std::unexpected(
                QStringLiteral("process did not accept standard input: %1")
                    .arg(process.errorString()));
        }
    }
    process.closeWriteChannel();
    if (!process.waitForFinished(30'000)) {
        return std::unexpected(
            QStringLiteral("process did not finish: %1")
                .arg(process.errorString()));
    }
    return ProcessResult{
        .exitStatus = process.exitStatus(),
        .exitCode = process.exitCode(),
        .processId = processId,
        .standardOutput = process.readAllStandardOutput(),
        .standardError = process.readAllStandardError(),
    };
}

struct FakeHelperReport final {
    qint64 processId = 0;
    QList<QByteArray> arguments;
    QByteArray workingDirectory;
    QByteArray environmentSentinel;
    QByteArray standardInput;
};

std::expected<FakeHelperReport, QString> parseFakeHelperReport(
    const QByteArray &output)
{
    qsizetype offset = 0;
    const auto takeLine = [&]() -> std::expected<QByteArray, QString> {
        const qsizetype newline = output.indexOf('\n', offset);
        if (newline < 0) {
            return std::unexpected(QStringLiteral("missing framed line"));
        }
        const QByteArray result = output.mid(offset, newline - offset);
        offset = newline + 1;
        return result;
    };
    const auto takeField = [&](QByteArrayView expectedName)
        -> std::expected<QByteArray, QString> {
        auto header = takeLine();
        if (!header) return std::unexpected(header.error());
        const QByteArray prefix = expectedName.toByteArray() + ' ';
        if (!header->startsWith(prefix)) {
            return std::unexpected(QStringLiteral("unexpected field header: %1")
                .arg(QString::fromLatin1(*header)));
        }
        bool validLength = false;
        const qlonglong length = header->mid(prefix.size())
                                     .toLongLong(&validLength);
        if (!validLength || length < 0
            || length > output.size() - offset) {
            return std::unexpected(QStringLiteral("invalid field length"));
        }
        const QByteArray result = output.mid(
            offset, static_cast<qsizetype>(length));
        offset += static_cast<qsizetype>(length);
        if (offset >= output.size() || output.at(offset) != '\n') {
            return std::unexpected(QStringLiteral("missing field terminator"));
        }
        ++offset;
        return result;
    };

    auto pidLine = takeLine();
    auto argcLine = takeLine();
    if (!pidLine || !argcLine
        || !pidLine->startsWith("PID ")
        || !argcLine->startsWith("ARGC ")) {
        return std::unexpected(QStringLiteral("missing fake-helper header"));
    }
    bool validPid = false;
    bool validArgc = false;
    const qint64 processId = pidLine->mid(4).toLongLong(&validPid);
    const int argumentCount = argcLine->mid(5).toInt(&validArgc);
    if (!validPid || processId <= 0 || !validArgc || argumentCount < 1) {
        return std::unexpected(QStringLiteral("invalid fake-helper header"));
    }

    FakeHelperReport report{
        .processId = processId,
    };
    report.arguments.reserve(argumentCount);
    for (int index = 0; index < argumentCount; ++index) {
        auto argument = takeField("ARG");
        if (!argument) return std::unexpected(argument.error());
        report.arguments.append(std::move(*argument));
    }
    auto workingDirectory = takeField("CWD");
    auto environmentSentinel = takeField("ENV");
    auto standardInput = takeField("STDIN");
    if (!workingDirectory || !environmentSentinel || !standardInput) {
        return std::unexpected(QStringLiteral("incomplete fake-helper report"));
    }
    if (offset != output.size()) {
        return std::unexpected(QStringLiteral("trailing fake-helper output"));
    }
    report.workingDirectory = std::move(*workingDirectory);
    report.environmentSentinel = std::move(*environmentSentinel);
    report.standardInput = std::move(*standardInput);
    return report;
}

bool copyExecutable(const QString &source, const QString &destination)
{
    return QFile::copy(source, destination)
        && QFile::setPermissions(destination, QFileInfo(source).permissions());
}

QProcessEnvironment controlledEnvironment(const QString &configHome)
{
    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), configHome);
    environment.insert(QStringLiteral("XDG_STATE_HOME"),
                       QDir(configHome).filePath(QStringLiteral("state")));
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"),
                       QStringLiteral("ghostty-cli-test-must-not-load-qt"));
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    environment.insert(QStringLiteral("PAGER"), QStringLiteral("cat"));
    environment.remove(QStringLiteral("DISPLAY"));
    environment.remove(QStringLiteral("EDITOR"));
    environment.remove(QStringLiteral("FONTCONFIG_FILE"));
    environment.remove(QStringLiteral("FONTCONFIG_PATH"));
    environment.remove(QStringLiteral("GHOSTTY_RESOURCES_DIR"));
    environment.remove(QStringLiteral("VISUAL"));
    environment.remove(QStringLiteral("WAYLAND_DISPLAY"));
    return environment;
}

bool configureIsolatedFontconfig(const QString &root,
                                 QProcessEnvironment &environment)
{
    const QFileInfo font(QStringLiteral(GHOSTTY_QT_TEST_FONT_PATH));
    const QString cache = QDir(root).filePath(QStringLiteral("font-cache"));
    const QString config = QDir(root).filePath(QStringLiteral("fonts.conf"));
    if (!font.isFile() || !QDir().mkpath(cache)) return false;

    const QString document =
        QStringLiteral(
            "<?xml version=\"1.0\"?>\n"
            "<!DOCTYPE fontconfig SYSTEM \"urn:fontconfig:fonts.dtd\">\n"
            "<fontconfig>\n"
            "  <dir>%1</dir>\n"
            "  <cachedir>%2</cachedir>\n"
            "</fontconfig>\n")
            .arg(font.absolutePath().toHtmlEscaped(), cache.toHtmlEscaped());
    QFile file(config);
    const QByteArray encodedDocument = document.toUtf8();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(encodedDocument) != encodedDocument.size()) {
        return false;
    }
    file.close();
    environment.insert(QStringLiteral("FONTCONFIG_FILE"), config);
    return true;
}

QString shellQuote(const QString &value)
{
    QString escaped = value;
    escaped.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
    return QStringLiteral("'") + escaped + QStringLiteral("'");
}

bool isRegularFile(const QString &path)
{
    const QFileInfo information(path);
    return information.isFile();
}

std::optional<QByteArray> readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    return file.readAll();
}

bool writeFile(const QString &path, QByteArrayView contents)
{
    if (!QDir().mkpath(QFileInfo(path).path())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(contents.data(), static_cast<qint64>(contents.size()))
        == contents.size();
}

std::optional<unsigned int> fileMode(const QString &path)
{
    const QByteArray encoded = QFile::encodeName(path);
    struct stat information {};
    if (::stat(encoded.constData(), &information) != 0) {
        return std::nullopt;
    }
    return static_cast<unsigned int>(information.st_mode & 0777U);
}

QList<QByteArray> forwardedSshArguments(
    const QString &program, QByteArrayView term,
    const QList<QByteArray> &arguments)
{
    QList<QByteArray> result{
        QFile::encodeName(program),
        QByteArrayLiteral("-o"),
        QByteArrayLiteral("SetEnv=TERM=") + term.toByteArray(),
        QByteArrayLiteral("-o"),
        QByteArrayLiteral("SendEnv=COLORTERM"),
        QByteArrayLiteral("-o"),
        QByteArrayLiteral("SendEnv=TERM_PROGRAM"),
        QByteArrayLiteral("-o"),
        QByteArrayLiteral("SendEnv=TERM_PROGRAM_VERSION"),
    };
    result.append(arguments);
    return result;
}

} // namespace

class GhosttyCliDelegationTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void classifiesRawArguments();
    void replacementPreservesProcessContract();
    void matchesPinnedHelper_data();
    void matchesPinnedHelper();
    void delegatesPinnedSshChildContract();
    void exercisesPinnedSshTerminfoCache();
    void managesPinnedSshCache();
    void editConfigUsesPinnedEditorContract();
    void listsPinnedThemes();
    void enforcesBuildConfigurationBoundary();
};

void GhosttyCliDelegationTest::classifiesRawArguments()
{
    const auto expect = [](std::initializer_list<std::string_view> arguments,
                           GhosttyCliActionDisposition disposition,
                           std::string_view selected = {}) {
        RawArguments raw(arguments);
        const GhosttyCliActionSelection actual =
            selectGhosttyCliAction(raw.span());
        QCOMPARE(static_cast<int>(actual.disposition),
                 static_cast<int>(disposition));
        QCOMPARE(QByteArray(actual.argument.data(),
                            static_cast<qsizetype>(actual.argument.size())),
                 QByteArray(selected.data(),
                            static_cast<qsizetype>(selected.size())));
    };

    for (const GhosttyCliActionCatalogEntry &entry
         : GhosttyPinnedCliActions) {
        const GhosttyCliActionDisposition disposition = entry.isDelegated()
            ? GhosttyCliActionDisposition::Delegate
            : (entry.isApplicationIpc()
                   ? GhosttyCliActionDisposition::ApplicationIpc
                   : GhosttyCliActionDisposition::Unsupported);
        expect({"ghostty-qt", entry.argument}, disposition, entry.argument);
    }
    expect({"ghostty-qt", "--plain", "+list-colors"},
           GhosttyCliActionDisposition::Delegate, "+list-colors");
    expect({"ghostty-qt", "--bogus", "+list-colors", "--plain"},
           GhosttyCliActionDisposition::Delegate, "+list-colors");
    expect({"ghostty-qt", "--help", "+show-config"},
           GhosttyCliActionDisposition::Delegate, "+show-config");
    expect({"ghostty-qt", "+show-config", "--help"},
           GhosttyCliActionDisposition::Delegate, "+show-config");
    expect({"ghostty-qt", "-efoo", "+help"},
           GhosttyCliActionDisposition::Delegate, "+help");
    expect({"ghostty-qt", "+help", "-e", "/bin/true"},
           GhosttyCliActionDisposition::Delegate, "+help");
    expect({"ghostty-qt", "-e", "ghostty", "+help"},
           GhosttyCliActionDisposition::None);
    expect({"ghostty-qt", "--", "+help"},
           GhosttyCliActionDisposition::None);
    expect({"ghostty-qt", "--help"},
           GhosttyCliActionDisposition::None);
    expect({"ghostty-qt", "+help", "--version"},
           GhosttyCliActionDisposition::None);
    expect({"ghostty-qt", "+help", "+list-colors"},
           GhosttyCliActionDisposition::Multiple, "+list-colors");
    expect({"ghostty-qt", "+help", "-e", "+list-colors"},
           GhosttyCliActionDisposition::Multiple, "+list-colors");
    expect({"ghostty-qt", "+list-fonts", "+help"},
           GhosttyCliActionDisposition::Multiple, "+help");
    expect({"ghostty-qt", "+help", "+list-fonts"},
           GhosttyCliActionDisposition::Multiple, "+list-fonts");
    expect({"ghostty-qt", "+list-fonts", "--version"},
           GhosttyCliActionDisposition::None);
    expect({"ghostty-qt", "+unknown", "+help"},
           GhosttyCliActionDisposition::Unsupported, "+unknown");
    expect({"ghostty-qt", "+ssh", "--", "destination"},
           GhosttyCliActionDisposition::Delegate, "+ssh");
    expect({"ghostty-qt", "+new-window", "--title=remote"},
           GhosttyCliActionDisposition::ApplicationIpc, "+new-window");
    expect({"ghostty-qt", "+new-window", "-e", "+not-an-action"},
           GhosttyCliActionDisposition::Multiple, "+not-an-action");
    expect({"ghostty-qt", "+toggle-quick-terminal", "--class=ignored"},
           GhosttyCliActionDisposition::ApplicationIpc,
           "+toggle-quick-terminal");

    constexpr std::array InvalidActions{
        "+edit-config=now",
        "+show-config-json",
        "+unknown",
        "+Help",
        "+helpful",
        "+help=now",
    };
    for (const std::string_view action : InvalidActions) {
        expect({"ghostty-qt", action},
               GhosttyCliActionDisposition::Unsupported, action);
    }
}

void GhosttyCliDelegationTest::replacementPreservesProcessContract()
{
#if !GHOSTTY_QT_TEST_CONFIG_ENABLED
    QSKIP("The configuration-disabled application deliberately has no CLI helper");
#else
    QVERIFY(QDir().mkpath(QStringLiteral("tmp")));
    QTemporaryDir temporary(QDir::current().filePath(
        QStringLiteral("tmp/ghostty-cli-exec-XXXXXX")));
    QVERIFY(temporary.isValid());
    const QString application = temporary.filePath(QStringLiteral("ghostty-qt"));
    const QString helper = temporary.filePath(
        QStringLiteral("ghostty-qt-config-helper"));
    QVERIFY(copyExecutable(
        QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE), application));
    QVERIFY(copyExecutable(
        QStringLiteral(GHOSTTY_QT_TEST_FAKE_HELPER), helper));
    const QString workingDirectory = temporary.filePath(QStringLiteral("cwd"));
    QVERIFY(QDir().mkpath(workingDirectory));

    QProcessEnvironment environment =
        controlledEnvironment(temporary.filePath(QStringLiteral("config")));
    environment.insert(QStringLiteral("GHOSTTY_QT_CLI_SENTINEL"),
                       QStringLiteral("preserved-environment"));
    const QStringList arguments{
        QStringLiteral("--fake-before"),
        QStringLiteral("+list-colors"),
        QStringLiteral("--plain"),
        QString{},
        QStringLiteral("argument with spaces"),
    };
    const QByteArray standardInput("input\0payload\n", 14);
    auto executed = runProcess(application, arguments, environment,
                               workingDirectory, standardInput);
    QVERIFY2(executed.has_value(),
             qPrintable(executed.has_value() ? QString{} : executed.error()));
    QCOMPARE(executed->exitStatus, QProcess::NormalExit);
    QCOMPARE(executed->exitCode, 73);
    QCOMPARE(executed->standardError,
             QByteArray("fake-stderr\0binary", 18));

    auto report = parseFakeHelperReport(executed->standardOutput);
    QVERIFY2(report.has_value(),
             qPrintable(report.has_value() ? QString{} : report.error()));
    QCOMPARE(report->processId, executed->processId);
    QList<QByteArray> expectedArguments{
        QFile::encodeName(QFileInfo(helper).absoluteFilePath()),
    };
    for (const QString &argument : arguments) {
        expectedArguments.append(argument.toLocal8Bit());
    }
    QCOMPARE(report->arguments, expectedArguments);
    QCOMPARE(report->workingDirectory,
             QFile::encodeName(QDir(workingDirectory).absolutePath()));
    QCOMPARE(report->environmentSentinel,
             QByteArrayLiteral("preserved-environment"));
    QCOMPARE(report->standardInput, standardInput);

    QVERIFY(QFile::setPermissions(
        helper, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    auto unexecutable = runProcess(application, {QStringLiteral("+help")},
                                   environment, workingDirectory);
    QVERIFY2(unexecutable.has_value(),
             qPrintable(unexecutable.has_value()
                 ? QString{} : unexecutable.error()));
    QCOMPARE(unexecutable->exitStatus, QProcess::NormalExit);
    QCOMPARE(unexecutable->exitCode, 126);
    QVERIFY(unexecutable->standardOutput.isEmpty());
    QVERIFY(unexecutable->standardError.contains(
        QByteArrayLiteral("could not execute CLI helper")));

    QVERIFY(QFile::remove(helper));
    auto missing = runProcess(application, {QStringLiteral("+help")},
                              environment, workingDirectory);
    QVERIFY2(missing.has_value(),
             qPrintable(missing.has_value() ? QString{} : missing.error()));
    QCOMPARE(missing->exitStatus, QProcess::NormalExit);
    QCOMPARE(missing->exitCode, 127);
    QVERIFY(missing->standardOutput.isEmpty());
    QVERIFY(missing->standardError.contains(
        QByteArrayLiteral("could not execute CLI helper")));
    QVERIFY(missing->standardError.contains(
        QByteArrayLiteral("ghostty-qt-config-helper")));
#endif
}

void GhosttyCliDelegationTest::matchesPinnedHelper_data()
{
    QTest::addColumn<QStringList>("arguments");
    QTest::addColumn<QByteArray>("marker");
    QTest::addColumn<int>("expectedExitCode");

    QTest::newRow("version") << QStringList{QStringLiteral("+version")}
                             << QByteArrayLiteral("Build Config") << 0;
    QTest::newRow("help")
        << QStringList{QStringLiteral("+help")}
        << QByteArrayLiteral("Available actions:") << 0;
    QTest::newRow("explain-config")
        << QStringList{QStringLiteral("+explain-config"),
                       QStringLiteral("--no-pager"),
                       QStringLiteral("font-size")}
        << QByteArrayLiteral("Font size in points") << 0;
    QTest::newRow("edit-config-help")
        << QStringList{QStringLiteral("+edit-config"),
                       QStringLiteral("--help")}
        << QByteArrayLiteral("$VISUAL") << 0;
    QTest::newRow("list-actions")
        << QStringList{QStringLiteral("+list-actions")}
        << QByteArrayLiteral("copy_to_clipboard") << 0;
    QTest::newRow("list-colors")
        << QStringList{QStringLiteral("--plain"),
                       QStringLiteral("+list-colors")}
        << QByteArrayLiteral("AliceBlue = #f0f8ff") << 0;
    QTest::newRow("list-fonts")
        << QStringList{QStringLiteral("+list-fonts"),
                       QStringLiteral("--family=Fira Code")}
        << QByteArrayLiteral("Fira Code\n  Fira Code Regular\n") << 0;
    QTest::newRow("list-themes")
        << QStringList{QStringLiteral("+list-themes"),
                       QStringLiteral("--plain")}
        << QByteArrayLiteral("Dracula (resources)\n") << 0;
    QTest::newRow("show-face")
        << QStringList{QStringLiteral("+show-face"),
                       QStringLiteral("--font-family=Fira Code"),
                       QStringLiteral("--cp=65")}
        << QByteArrayLiteral("U+41 « A » found in face “Fira Code”.\n") << 0;
    QTest::newRow("show-face-missing-query")
        << QStringList{QStringLiteral("+show-face")}
        << QByteArrayLiteral(
               "You must specify a codepoint with --cp or a string with "
               "--string\n")
        << 1;
    QTest::newRow("crash-report-help")
        << QStringList{QStringLiteral("+crash-report"),
                       QStringLiteral("--help")}
        << QByteArrayLiteral("crash-report") << 0;
    QTest::newRow("crash-report-empty")
        << QStringList{QStringLiteral("+crash-report")} << QByteArray{} << 0;
    QTest::newRow("boo-help")
        << QStringList{QStringLiteral("+boo"), QStringLiteral("--help")}
        << QByteArrayLiteral("boo") << 0;
    QTest::newRow("list-keybinds")
        << QStringList{QStringLiteral("+list-keybinds"),
                       QStringLiteral("--default"),
                       QStringLiteral("--plain")}
        << QByteArrayLiteral("reload_config") << 0;
    QTest::newRow("show-config")
        << QStringList{QStringLiteral("+show-config"),
                       QStringLiteral("--default"),
                       QStringLiteral("--no-pager")}
        << QByteArrayLiteral("font-size = 12") << 0;
    QTest::newRow("ssh-help")
        << QStringList{QStringLiteral("+ssh"),
                       QStringLiteral("--help")}
        << QByteArrayLiteral("Wrap `ssh` to automatically configure") << 0;
    QTest::newRow("ssh-cache-help")
        << QStringList{QStringLiteral("+ssh-cache"),
                       QStringLiteral("--help")}
        << QByteArrayLiteral("Manage the SSH terminfo cache") << 0;
    QTest::newRow("ssh-child-success")
        << QStringList{QStringLiteral("+ssh"),
                       QStringLiteral("--terminfo=false"),
                       QStringLiteral("--forward-env=false"),
                       QStringLiteral("--ssh=/bin/true"),
                       QStringLiteral("--"),
                       QStringLiteral("fixture.example")}
        << QByteArray{} << 0;
    QTest::newRow("validate-config")
        << QStringList{QStringLiteral("+validate-config")}
        << QByteArray{} << 0;
    QTest::newRow("action-help-before")
        << QStringList{QStringLiteral("--help"),
                       QStringLiteral("+validate-config")}
        << QByteArrayLiteral("validate-config") << 0;
    QTest::newRow("action-help-after")
        << QStringList{QStringLiteral("+validate-config"),
                       QStringLiteral("--help")}
        << QByteArrayLiteral("validate-config") << 0;
    QTest::newRow("invalid-option")
        << QStringList{QStringLiteral("+list-colors"),
                       QStringLiteral("--definitely-invalid")}
        << QByteArray{} << 1;
    QTest::newRow("edit-config-invalid-option")
        << QStringList{QStringLiteral("+edit-config"),
                       QStringLiteral("--definitely-invalid")}
        << QByteArray{} << 1;
}

void GhosttyCliDelegationTest::matchesPinnedHelper()
{
#if !GHOSTTY_QT_TEST_CONFIG_ENABLED
    QSKIP("The pinned CLI helper is disabled in this build");
#else
    QFETCH(QStringList, arguments);
    QFETCH(QByteArray, marker);
    QFETCH(int, expectedExitCode);
    QVERIFY(QDir().mkpath(QStringLiteral("tmp")));
    QTemporaryDir temporary(QDir::current().filePath(
        QStringLiteral("tmp/ghostty-cli-real-XXXXXX")));
    QVERIFY(temporary.isValid());
    QProcessEnvironment environment = controlledEnvironment(temporary.path());
    if (arguments.contains(QStringLiteral("+list-fonts"))
        || arguments.contains(QStringLiteral("+show-face"))) {
        QVERIFY(configureIsolatedFontconfig(temporary.path(), environment));
    }
    auto helper = runProcess(
        QStringLiteral(GHOSTTY_QT_TEST_REAL_HELPER), arguments,
        environment, temporary.path());
    auto application = runProcess(
        QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE), arguments,
        environment, temporary.path());
    QVERIFY2(helper.has_value(),
             qPrintable(helper.has_value() ? QString{} : helper.error()));
    QVERIFY2(application.has_value(),
             qPrintable(application.has_value()
                 ? QString{} : application.error()));
    QCOMPARE(helper->exitStatus, QProcess::NormalExit);
    QCOMPARE(helper->exitCode, expectedExitCode);
    QCOMPARE(application->exitStatus, QProcess::NormalExit);
    QCOMPARE(application->exitCode, expectedExitCode);
    QCOMPARE(application->exitStatus, helper->exitStatus);
    QCOMPARE(application->exitCode, helper->exitCode);
    QCOMPARE(application->standardOutput, helper->standardOutput);
    QCOMPARE(application->standardError, helper->standardError);
    if (!marker.isEmpty()) {
        QVERIFY2(application->standardOutput.contains(marker),
                 application->standardOutput.constData());
    }
#endif
}

void GhosttyCliDelegationTest::delegatesPinnedSshChildContract()
{
#if !GHOSTTY_QT_TEST_CONFIG_ENABLED
    QSKIP("The pinned CLI helper is disabled in this build");
#else
    QVERIFY(QDir().mkpath(QStringLiteral("tmp")));
    QTemporaryDir temporary(QDir::current().filePath(
        QStringLiteral("tmp/ghostty-cli-ssh-child-XXXXXX")));
    QVERIFY(temporary.isValid());

    const QString processTmp = temporary.filePath(QStringLiteral("process-tmp"));
    const QString stateHome = temporary.filePath(QStringLiteral("state"));
    QVERIFY(QDir().mkpath(processTmp));
    QVERIFY(QDir().mkpath(stateHome));

    const QString fakeSsh = QFileInfo(
        QStringLiteral(GHOSTTY_QT_TEST_FAKE_HELPER)).absoluteFilePath();
    QProcessEnvironment environment = controlledEnvironment(
        temporary.filePath(QStringLiteral("config")));
    environment.insert(QStringLiteral("TMPDIR"), processTmp);
    environment.insert(QStringLiteral("XDG_STATE_HOME"), stateHome);
    environment.insert(QStringLiteral("GHOSTTY_QT_CLI_SENTINEL"),
                       QStringLiteral("ssh-child-environment"));

    const QStringList sshArguments{
        QStringLiteral("+ssh"),
        QStringLiteral("--terminfo=false"),
        QStringLiteral("--ssh=") + fakeSsh,
        QStringLiteral("--"),
        QStringLiteral("--rare-ssh-option"),
        QStringLiteral("fixture.example"),
        QString{},
        QStringLiteral("argument with spaces"),
    };
    const QByteArray standardInput("ssh-input\0payload\n", 18);
    auto executed = runProcess(
        QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE), sshArguments,
        environment, temporary.path(), standardInput);
    QVERIFY2(executed.has_value(),
             qPrintable(executed.has_value()
                 ? QString{} : executed.error()));
    QCOMPARE(executed->exitStatus, QProcess::NormalExit);
    QCOMPARE(executed->exitCode, 73);
    QCOMPARE(executed->standardError,
             QByteArray("fake-stderr\0binary", 18));

    auto report = parseFakeHelperReport(executed->standardOutput);
    QVERIFY2(report.has_value(),
             qPrintable(report.has_value() ? QString{} : report.error()));
    QCOMPARE(report->arguments, forwardedSshArguments(
        fakeSsh, "xterm-256color",
        {
            QByteArrayLiteral("--rare-ssh-option"),
            QByteArrayLiteral("fixture.example"),
            QByteArray{},
            QByteArrayLiteral("argument with spaces"),
        }));
    QCOMPARE(report->workingDirectory,
             QFile::encodeName(temporary.path()));
    QCOMPARE(report->environmentSentinel,
             QByteArrayLiteral("ssh-child-environment"));
    QCOMPARE(report->standardInput, standardInput);

    auto signaled = runProcess(
        QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE),
        {
            QStringLiteral("+ssh"),
            QStringLiteral("--terminfo=false"),
            QStringLiteral("--forward-env=false"),
            QStringLiteral("--ssh=/bin/sh"),
            QStringLiteral("--"),
            QStringLiteral("-c"),
            QStringLiteral("kill -TERM $$"),
        },
        environment, temporary.path());
    QVERIFY2(signaled.has_value(),
             qPrintable(signaled.has_value()
                 ? QString{} : signaled.error()));
    QCOMPARE(signaled->exitStatus, QProcess::NormalExit);
    QCOMPARE(signaled->exitCode, 128 + SIGTERM);
    QVERIFY(signaled->standardOutput.isEmpty());
#endif
}

void GhosttyCliDelegationTest::exercisesPinnedSshTerminfoCache()
{
#if !GHOSTTY_QT_TEST_CONFIG_ENABLED
    QSKIP("The pinned CLI helper is disabled in this build");
#else
    QVERIFY(QDir().mkpath(QStringLiteral("tmp")));
    QTemporaryDir temporary(QDir::current().filePath(
        QStringLiteral("tmp/ghostty-cli-ssh-terminfo-XXXXXX")));
    QVERIFY(temporary.isValid());

    const QString stateHome = temporary.filePath(QStringLiteral("state"));
    const QString processTmp = temporary.filePath(QStringLiteral("process-tmp"));
    const QString home = temporary.filePath(QStringLiteral("home"));
    const QString cacheDecoy = temporary.filePath(QStringLiteral("cache-decoy"));
    for (const QString &directory
         : {stateHome, processTmp, home, cacheDecoy}) {
        QVERIFY(QDir().mkpath(directory));
    }

    const QString fakeSsh = QFileInfo(
        QStringLiteral(GHOSTTY_QT_TEST_FAKE_HELPER)).absoluteFilePath();
    const QString phaseLog = temporary.filePath(QStringLiteral("phases"));
    const QString payload = temporary.filePath(QStringLiteral("terminfo"));
    const QString cachePath = QDir(stateHome).filePath(
        QStringLiteral("ghostty/ssh_cache"));

    QProcessEnvironment environment = controlledEnvironment(
        temporary.filePath(QStringLiteral("config")));
    environment.insert(QStringLiteral("HOME"), home);
    environment.insert(QStringLiteral("TMPDIR"), processTmp);
    environment.insert(QStringLiteral("XDG_CACHE_HOME"), cacheDecoy);
    environment.insert(QStringLiteral("XDG_STATE_HOME"), stateHome);
    environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_SSH_PHASE_LOG"),
                       phaseLog);
    environment.insert(
        QStringLiteral("GHOSTTY_QT_FAKE_SSH_TERMINFO_PAYLOAD"), payload);
    environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_SSH_FINAL_EXIT"),
                       QStringLiteral("0"));

    const QStringList arguments{
        QStringLiteral("+ssh"),
        QStringLiteral("--ssh=") + fakeSsh,
        QStringLiteral("--"),
        QStringLiteral("fixture-alias"),
    };
    const auto runSsh = [&] {
        return runProcess(QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE),
                          arguments, environment, temporary.path());
    };
    const auto clearArtifacts = [&] {
        (void) QFile::remove(phaseLog);
        (void) QFile::remove(payload);
    };

    environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_SSH_INSTALL_EXIT"),
                       QStringLiteral("17"));
    auto failedInstall = runSsh();
    QVERIFY2(failedInstall.has_value(),
             qPrintable(failedInstall.has_value()
                 ? QString{} : failedInstall.error()));
    QCOMPARE(failedInstall->exitStatus, QProcess::NormalExit);
    QCOMPARE(failedInstall->exitCode, 0);
    QCOMPARE(readFile(phaseLog),
             std::optional<QByteArray>("resolve\ninstall\nfinal\n"));
    const auto failedPayload = readFile(payload);
    QVERIFY(failedPayload.has_value());
    QVERIFY(!failedPayload->isEmpty());
    QVERIFY(!QFileInfo::exists(cachePath));
    QVERIFY(failedInstall->standardError.contains(
        QByteArrayLiteral("failed to install terminfo")));
    auto failedReport = parseFakeHelperReport(
        failedInstall->standardOutput);
    QVERIFY2(failedReport.has_value(),
             qPrintable(failedReport.has_value()
                 ? QString{} : failedReport.error()));
    QCOMPARE(failedReport->arguments, forwardedSshArguments(
        fakeSsh, "xterm-256color",
        {QByteArrayLiteral("fixture-alias")}));

    clearArtifacts();
    environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_SSH_INSTALL_EXIT"),
                       QStringLiteral("0"));
    auto installed = runSsh();
    QVERIFY2(installed.has_value(),
             qPrintable(installed.has_value()
                 ? QString{} : installed.error()));
    QCOMPARE(installed->exitStatus, QProcess::NormalExit);
    QCOMPARE(installed->exitCode, 0);
    QCOMPARE(readFile(phaseLog),
             std::optional<QByteArray>("resolve\ninstall\nfinal\n"));
    const auto installedPayload = readFile(payload);
    QVERIFY(installedPayload.has_value());
    QVERIFY(!installedPayload->isEmpty());
    auto installedReport = parseFakeHelperReport(installed->standardOutput);
    QVERIFY2(installedReport.has_value(),
             qPrintable(installedReport.has_value()
                 ? QString{} : installedReport.error()));
    QCOMPARE(installedReport->arguments, forwardedSshArguments(
        fakeSsh, "xterm-ghostty",
        {QByteArrayLiteral("fixture-alias")}));
    QVERIFY(isRegularFile(cachePath));
    QCOMPARE(fileMode(cachePath), std::optional<unsigned int>(0600U));
    const auto cacheContents = readFile(cachePath);
    QVERIFY(cacheContents.has_value());
    QVERIFY(cacheContents->startsWith(
        QByteArrayLiteral("fixture-user@fixture.example|")));

    clearArtifacts();
    auto cached = runSsh();
    QVERIFY2(cached.has_value(),
             qPrintable(cached.has_value() ? QString{} : cached.error()));
    QCOMPARE(cached->exitStatus, QProcess::NormalExit);
    QCOMPARE(cached->exitCode, 0);
    QCOMPARE(readFile(phaseLog),
             std::optional<QByteArray>("resolve\nfinal\n"));
    QVERIFY(!QFileInfo::exists(payload));
    auto cachedReport = parseFakeHelperReport(cached->standardOutput);
    QVERIFY2(cachedReport.has_value(),
             qPrintable(cachedReport.has_value()
                 ? QString{} : cachedReport.error()));
    QCOMPARE(cachedReport->arguments, forwardedSshArguments(
        fakeSsh, "xterm-ghostty",
        {QByteArrayLiteral("fixture-alias")}));
    QVERIFY(!cached->standardError.contains(
        QByteArrayLiteral("Setting up xterm-ghostty")));
    QVERIFY(!QFileInfo::exists(cacheDecoy + QStringLiteral("/ghostty/ssh_cache")));
#endif
}

void GhosttyCliDelegationTest::managesPinnedSshCache()
{
#if !GHOSTTY_QT_TEST_CONFIG_ENABLED
    QSKIP("The pinned CLI helper is disabled in this build");
#else
    QVERIFY(QDir().mkpath(QStringLiteral("tmp")));
    QTemporaryDir temporary(QDir::current().filePath(
        QStringLiteral("tmp/ghostty-cli-ssh-cache-XXXXXX")));
    QVERIFY(temporary.isValid());

    const QString stateHome = temporary.filePath(QStringLiteral("state"));
    const QString processTmp = temporary.filePath(QStringLiteral("process-tmp"));
    const QString home = temporary.filePath(QStringLiteral("home"));
    const QString cacheDecoy = temporary.filePath(QStringLiteral("cache-decoy"));
    for (const QString &directory
         : {stateHome, processTmp, home, cacheDecoy}) {
        QVERIFY(QDir().mkpath(directory));
    }
    const QString cachePath = QDir(stateHome).filePath(
        QStringLiteral("ghostty/ssh_cache"));

    QProcessEnvironment environment = controlledEnvironment(
        temporary.filePath(QStringLiteral("config")));
    environment.insert(QStringLiteral("HOME"), home);
    environment.insert(QStringLiteral("TMPDIR"), processTmp);
    environment.insert(QStringLiteral("XDG_CACHE_HOME"), cacheDecoy);
    environment.insert(QStringLiteral("XDG_STATE_HOME"), stateHome);
    const auto runCache = [&](QStringList arguments) {
        arguments.prepend(QStringLiteral("+ssh-cache"));
        return runProcess(QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE),
                          arguments, environment, temporary.path());
    };

    auto empty = runCache({});
    QVERIFY2(empty.has_value(),
             qPrintable(empty.has_value() ? QString{} : empty.error()));
    QCOMPARE(empty->exitCode, 0);
    QVERIFY(empty->standardOutput.isEmpty());
    QVERIFY(!QFileInfo::exists(cachePath));

    for (const QString &destination
         : {QStringLiteral("alice@example.test"),
            QStringLiteral("alice@example.test"),
            QStringLiteral("root@example.test"),
            QStringLiteral("carol@other.test")}) {
        auto added = runCache(
            {QStringLiteral("--add=") + destination});
        QVERIFY2(added.has_value(),
                 qPrintable(added.has_value()
                     ? QString{} : added.error()));
        QCOMPARE(added->exitCode, 0);
    }
    QVERIFY(isRegularFile(cachePath));
    QCOMPARE(fileMode(cachePath), std::optional<unsigned int>(0600U));

    const QByteArray encodedCachePath = QFile::encodeName(cachePath);
    QCOMPARE(::chmod(encodedCachePath.constData(), 0644), 0);
    auto repaired = runCache(
        {QStringLiteral("--add=dave@repair.test")});
    QVERIFY2(repaired.has_value(),
             qPrintable(repaired.has_value()
                 ? QString{} : repaired.error()));
    QCOMPARE(repaired->exitCode, 0);
    QCOMPARE(fileMode(cachePath), std::optional<unsigned int>(0600U));

    auto listed = runCache({});
    QVERIFY2(listed.has_value(),
             qPrintable(listed.has_value() ? QString{} : listed.error()));
    QCOMPARE(listed->exitCode, 0);
    const QList<QByteArray> lines = listed->standardOutput.trimmed().split('\n');
    QCOMPARE(lines.size(), 4);
    QVERIFY(lines.at(0).startsWith("alice@example.test  "));
    QVERIFY(lines.at(1).startsWith("carol@other.test  "));
    QVERIFY(lines.at(2).startsWith("dave@repair.test  "));
    QVERIFY(lines.at(3).startsWith("root@example.test  "));

    auto exact = runCache({QStringLiteral("alice@example.test")});
    QVERIFY2(exact.has_value(),
             qPrintable(exact.has_value() ? QString{} : exact.error()));
    QCOMPARE(exact->exitCode, 0);
    QCOMPARE(exact->standardOutput.count('\n'), 1);
    QVERIFY(exact->standardOutput.startsWith("alice@example.test  "));

    auto host = runCache({QStringLiteral("example.test")});
    QVERIFY2(host.has_value(),
             qPrintable(host.has_value() ? QString{} : host.error()));
    QCOMPARE(host->exitCode, 0);
    QCOMPARE(host->standardOutput.count('\n'), 2);
    QVERIFY(host->standardOutput.startsWith("alice@example.test  "));
    QVERIFY(host->standardOutput.contains("root@example.test  "));
    QVERIFY(!host->standardOutput.contains("carol@other.test"));

    auto missing = runCache({QStringLiteral("missing.example")});
    QVERIFY2(missing.has_value(),
             qPrintable(missing.has_value()
                 ? QString{} : missing.error()));
    QCOMPARE(missing->exitCode, 1);

    auto removed = runCache(
        {QStringLiteral("--remove=alice@example.test")});
    QVERIFY2(removed.has_value(),
             qPrintable(removed.has_value()
                 ? QString{} : removed.error()));
    QCOMPARE(removed->exitCode, 0);
    auto removedAgain = runCache(
        {QStringLiteral("--remove=alice@example.test")});
    QVERIFY2(removedAgain.has_value(),
             qPrintable(removedAgain.has_value()
                 ? QString{} : removedAgain.error()));
    QCOMPARE(removedAgain->exitCode, 1);

    auto invalid = runCache(
        {QStringLiteral("--add=not a destination")});
    QVERIFY2(invalid.has_value(),
             qPrintable(invalid.has_value()
                 ? QString{} : invalid.error()));
    QCOMPARE(invalid->exitCode, 2);
    auto conflicting = runCache({
        QStringLiteral("--clear"),
        QStringLiteral("--add=conflict.example"),
    });
    QVERIFY2(conflicting.has_value(),
             qPrintable(conflicting.has_value()
                 ? QString{} : conflicting.error()));
    QCOMPARE(conflicting->exitCode, 2);

    QFile seeded(cachePath);
    QVERIFY(seeded.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray seed(
        "old.example|0|xterm-ghostty\n"
        "future.example|253402300799|xterm-ghostty\n");
    QCOMPARE(seeded.write(seed), seed.size());
    seeded.close();
    auto pruned = runCache({QStringLiteral("--prune=1d")});
    QVERIFY2(pruned.has_value(),
             qPrintable(pruned.has_value()
                 ? QString{} : pruned.error()));
    QCOMPARE(pruned->exitCode, 0);
    QCOMPARE(pruned->standardOutput,
             QByteArrayLiteral("Pruned cache entries: 1\n"));
    const auto prunedContents = readFile(cachePath);
    QVERIFY(prunedContents.has_value());
    QVERIFY(!prunedContents->contains("old.example"));
    QVERIFY(prunedContents->contains("future.example"));
    QCOMPARE(fileMode(cachePath), std::optional<unsigned int>(0600U));

    auto subsecondPrune = runCache({QStringLiteral("--prune=500ms")});
    QVERIFY2(subsecondPrune.has_value(),
             qPrintable(subsecondPrune.has_value()
                 ? QString{} : subsecondPrune.error()));
    QCOMPARE(subsecondPrune->exitCode, 2);

    auto cleared = runCache({QStringLiteral("--clear")});
    QVERIFY2(cleared.has_value(),
             qPrintable(cleared.has_value()
                 ? QString{} : cleared.error()));
    QCOMPARE(cleared->exitCode, 0);
    QVERIFY(!QFileInfo::exists(cachePath));
    QVERIFY(!QFileInfo::exists(
        cacheDecoy + QStringLiteral("/ghostty/ssh_cache")));
#endif
}

void GhosttyCliDelegationTest::editConfigUsesPinnedEditorContract()
{
#if !GHOSTTY_QT_TEST_CONFIG_ENABLED
    QSKIP("The pinned CLI helper is disabled in this build");
#else
    QVERIFY(QDir().mkpath(QStringLiteral("tmp")));
    QTemporaryDir temporary(QDir::current().filePath(
        QStringLiteral("tmp/ghostty-cli-edit-config-XXXXXX")));
    QVERIFY(temporary.isValid());

    const QString editor = temporary.filePath(
        QStringLiteral("fake editor's executable"));
    QVERIFY(copyExecutable(
        QStringLiteral(GHOSTTY_QT_TEST_FAKE_HELPER), editor));
    const QString editorCommand =
        QStringLiteral("exec ") + shellQuote(editor);
    const QByteArray standardInput("editor-input\0payload\n", 21);

    const QString preferredHome = temporary.filePath(
        QStringLiteral("preferred config home's files"));
    QVERIFY(QDir().mkpath(preferredHome));
    const QString preferredPath = QDir(preferredHome).filePath(
        QStringLiteral("ghostty/config.ghostty"));
    QProcessEnvironment preferredEnvironment =
        controlledEnvironment(preferredHome);
    preferredEnvironment.insert(QStringLiteral("VISUAL"), editorCommand);
    preferredEnvironment.insert(QStringLiteral("EDITOR"),
                                QStringLiteral("exec /bin/false"));
    preferredEnvironment.insert(QStringLiteral("GHOSTTY_QT_CLI_SENTINEL"),
                                QStringLiteral("visual-won"));

    auto preferred = runProcess(
        QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE),
        {QStringLiteral("+edit-config")}, preferredEnvironment,
        temporary.path(), standardInput);
    QVERIFY2(preferred.has_value(),
             qPrintable(preferred.has_value()
                 ? QString{} : preferred.error()));
    QCOMPARE(preferred->exitStatus, QProcess::NormalExit);
    QCOMPARE(preferred->exitCode, 73);
    QCOMPARE(preferred->standardError,
             QByteArray("fake-stderr\0binary", 18));
    auto preferredReport = parseFakeHelperReport(
        preferred->standardOutput);
    QVERIFY2(preferredReport.has_value(),
             qPrintable(preferredReport.has_value()
                 ? QString{} : preferredReport.error()));
    QCOMPARE(preferredReport->processId, preferred->processId);
    const QList<QByteArray> expectedPreferredArguments{
        QFile::encodeName(editor),
        QFile::encodeName(preferredPath),
    };
    QCOMPARE(preferredReport->arguments, expectedPreferredArguments);
    QCOMPARE(preferredReport->workingDirectory,
             QFile::encodeName(temporary.path()));
    QCOMPARE(preferredReport->environmentSentinel,
             QByteArrayLiteral("visual-won"));
    QCOMPARE(preferredReport->standardInput, standardInput);
    QVERIFY(isRegularFile(preferredPath));
    QCOMPARE(QFileInfo(preferredPath).size(), qint64{0});

    const QString legacyHome = temporary.filePath(
        QStringLiteral("legacy config home's files"));
    const QString legacyDirectory = QDir(legacyHome).filePath(
        QStringLiteral("ghostty"));
    QVERIFY(QDir().mkpath(legacyDirectory));
    const QString legacyPath = QDir(legacyDirectory).filePath(
        QStringLiteral("config"));
    QFile legacyFile(legacyPath);
    QVERIFY(legacyFile.open(QIODevice::WriteOnly));
    QCOMPARE(legacyFile.write("font-size = 13\n"), qint64{15});
    legacyFile.close();

    QProcessEnvironment legacyEnvironment =
        controlledEnvironment(legacyHome);
    legacyEnvironment.insert(QStringLiteral("VISUAL"), QString{});
    legacyEnvironment.insert(QStringLiteral("EDITOR"), editorCommand);
    legacyEnvironment.insert(QStringLiteral("GHOSTTY_QT_CLI_SENTINEL"),
                             QStringLiteral("editor-fallback"));
    auto legacy = runProcess(
        QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE),
        {QStringLiteral("+edit-config")}, legacyEnvironment,
        temporary.path());
    QVERIFY2(legacy.has_value(),
             qPrintable(legacy.has_value() ? QString{} : legacy.error()));
    QCOMPARE(legacy->exitStatus, QProcess::NormalExit);
    QCOMPARE(legacy->exitCode, 73);
    QCOMPARE(legacy->standardError,
             QByteArray("fake-stderr\0binary", 18));
    auto legacyReport = parseFakeHelperReport(legacy->standardOutput);
    QVERIFY2(legacyReport.has_value(),
             qPrintable(legacyReport.has_value()
                 ? QString{} : legacyReport.error()));
    QCOMPARE(legacyReport->processId, legacy->processId);
    const QList<QByteArray> expectedLegacyArguments{
        QFile::encodeName(editor),
        QFile::encodeName(legacyPath),
    };
    QCOMPARE(legacyReport->arguments, expectedLegacyArguments);
    QCOMPARE(legacyReport->environmentSentinel,
             QByteArrayLiteral("editor-fallback"));
    QVERIFY(!QFileInfo::exists(QDir(legacyHome).filePath(
        QStringLiteral("ghostty/config.ghostty"))));

    const QString missingEditorHome = temporary.filePath(
        QStringLiteral("missing editor config"));
    QVERIFY(QDir().mkpath(missingEditorHome));
    const QString missingEditorPath = QDir(missingEditorHome).filePath(
        QStringLiteral("ghostty/config.ghostty"));
    const QProcessEnvironment missingEditorEnvironment =
        controlledEnvironment(missingEditorHome);
    auto missingEditor = runProcess(
        QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE),
        {QStringLiteral("+edit-config")}, missingEditorEnvironment,
        temporary.path());
    auto directMissingEditor = runProcess(
        QStringLiteral(GHOSTTY_QT_TEST_REAL_HELPER),
        {QStringLiteral("+edit-config")}, missingEditorEnvironment,
        temporary.path());
    QVERIFY2(missingEditor.has_value(),
             qPrintable(missingEditor.has_value()
                 ? QString{} : missingEditor.error()));
    QVERIFY2(directMissingEditor.has_value(),
             qPrintable(directMissingEditor.has_value()
                 ? QString{} : directMissingEditor.error()));
    QCOMPARE(missingEditor->exitStatus, QProcess::NormalExit);
    QCOMPARE(missingEditor->exitCode, 1);
    QVERIFY(missingEditor->standardOutput.isEmpty());
    QVERIFY(missingEditor->standardError.contains(
        QByteArrayLiteral("$EDITOR or $VISUAL")));
    QVERIFY(missingEditor->standardError.contains(
        QByteArrayLiteral("\x1b]8;;file://")
            + QFile::encodeName(missingEditorPath)));
    QCOMPARE(missingEditor->exitStatus, directMissingEditor->exitStatus);
    QCOMPARE(missingEditor->exitCode, directMissingEditor->exitCode);
    QCOMPARE(missingEditor->standardOutput,
             directMissingEditor->standardOutput);
    QCOMPARE(missingEditor->standardError,
             directMissingEditor->standardError);
    QVERIFY(isRegularFile(missingEditorPath));
    QCOMPARE(QFileInfo(missingEditorPath).size(), qint64{0});
#endif
}

void GhosttyCliDelegationTest::listsPinnedThemes()
{
#if !GHOSTTY_QT_TEST_CONFIG_ENABLED
    QSKIP("The pinned CLI helper is disabled in this build");
#else
    QVERIFY(QDir().mkpath(QStringLiteral("tmp")));
    QTemporaryDir temporary(QDir::current().filePath(
        QStringLiteral("tmp/ghostty-cli-themes-XXXXXX")));
    QVERIFY(temporary.isValid());
    const QString application = QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE);
    const QString configHome = temporary.filePath(QStringLiteral("config"));
    QProcessEnvironment environment = controlledEnvironment(configHome);

    const auto run = [&](const QStringList &arguments,
                         const QProcessEnvironment &processEnvironment =
                             QProcessEnvironment{})
        -> std::expected<ProcessResult, QString> {
        return runProcess(application, arguments,
                          processEnvironment.isEmpty() ? environment
                                                       : processEnvironment,
                          temporary.path());
    };

    auto all = run({QStringLiteral("+list-themes"), QStringLiteral("--plain")});
    QVERIFY2(all.has_value(),
             qPrintable(all.has_value() ? QString{} : all.error()));
    QCOMPARE(all->exitStatus, QProcess::NormalExit);
    QCOMPARE(all->exitCode, 0);
    QVERIFY(all->standardError.isEmpty());
    QCOMPARE(all->standardOutput.count('\n'), 592);
    QVERIFY(all->standardOutput.startsWith(
        QByteArrayLiteral("0x96f (resources)\n")));
    QVERIFY(all->standardOutput.contains(
        QByteArrayLiteral("3024 Day (resources)\n")));
    QVERIFY(all->standardOutput.contains(
        QByteArrayLiteral("3024 Night (resources)\n")));
    QVERIFY(all->standardOutput.contains(
        QByteArrayLiteral("Dracula (resources)\n")));

    auto dark = run({QStringLiteral("+list-themes"), QStringLiteral("--plain"),
                     QStringLiteral("--color=dark")});
    QVERIFY2(dark.has_value(),
             qPrintable(dark.has_value() ? QString{} : dark.error()));
    QCOMPARE(dark->exitCode, 0);
    QVERIFY(dark->standardError.isEmpty());
    QCOMPARE(dark->standardOutput.count('\n'), 466);
    QVERIFY(dark->standardOutput.contains(
        QByteArrayLiteral("Dracula (resources)\n")));

    auto light = run({QStringLiteral("+list-themes"), QStringLiteral("--plain"),
                      QStringLiteral("--color=light")});
    QVERIFY2(light.has_value(),
             qPrintable(light.has_value() ? QString{} : light.error()));
    QCOMPARE(light->exitCode, 0);
    QVERIFY(light->standardError.isEmpty());
    QCOMPARE(light->standardOutput.count('\n'), 126);
    QVERIFY(light->standardOutput.contains(
        QByteArrayLiteral("3024 Day (resources)\n")));

    auto paths = run({QStringLiteral("+list-themes"), QStringLiteral("--plain"),
                      QStringLiteral("--path")});
    QVERIFY2(paths.has_value(),
             qPrintable(paths.has_value() ? QString{} : paths.error()));
    QCOMPARE(paths->exitCode, 0);
    QVERIFY(paths->standardError.isEmpty());
    const QByteArray expectedDraculaPath =
        QByteArrayLiteral("Dracula (resources) ")
        + QFile::encodeName(QDir(QStringLiteral(GHOSTTY_QT_TEST_THEMES_PATH))
                                .filePath(QStringLiteral("Dracula")))
        + '\n';
    QVERIFY(paths->standardOutput.contains(expectedDraculaPath));

    const QString userTheme =
        QDir(configHome).filePath(QStringLiteral("ghostty/themes/Qt Fixture"));
    QVERIFY(writeFile(userTheme,
                      QByteArrayLiteral("background = #000000\n"
                                        "foreground = #ffffff\n")));
    auto withUser =
        run({QStringLiteral("+list-themes"), QStringLiteral("--plain")});
    QVERIFY2(withUser.has_value(),
             qPrintable(withUser.has_value() ? QString{} : withUser.error()));
    QCOMPARE(withUser->exitCode, 0);
    QVERIFY(withUser->standardError.isEmpty());
    QCOMPARE(withUser->standardOutput.count('\n'), 593);
    QVERIFY(withUser->standardOutput.contains(
        QByteArrayLiteral("Qt Fixture (user)\n")));

    const QString overrideRoot = temporary.filePath(QStringLiteral("override"));
    QVERIFY(writeFile(
        QDir(overrideRoot).filePath(QStringLiteral("themes/Override Fixture")),
        QByteArrayLiteral("background = #101010\n"
                          "foreground = #f0f0f0\n")));
    QVERIFY(writeFile(
        QDir(overrideRoot).filePath(QStringLiteral("themes/.DS_Store")),
        QByteArrayLiteral("ignored")));
    QVERIFY(QDir().mkpath(
        QDir(overrideRoot)
            .filePath(QStringLiteral("themes/ignored-directory"))));
    QProcessEnvironment overrideEnvironment = controlledEnvironment(
        temporary.filePath(QStringLiteral("override-config")));
    overrideEnvironment.insert(QStringLiteral("GHOSTTY_RESOURCES_DIR"),
                               overrideRoot);
    auto overridden =
        run({QStringLiteral("+list-themes"), QStringLiteral("--plain")},
            overrideEnvironment);
    QVERIFY2(
        overridden.has_value(),
        qPrintable(overridden.has_value() ? QString{} : overridden.error()));
    QCOMPARE(overridden->exitCode, 0);
    QVERIFY(overridden->standardError.isEmpty());
    QCOMPARE(overridden->standardOutput,
             QByteArrayLiteral("Override Fixture (resources)\n"));
#endif
}

void GhosttyCliDelegationTest::enforcesBuildConfigurationBoundary()
{
    QVERIFY(QDir().mkpath(QStringLiteral("tmp")));
    QTemporaryDir temporary(QDir::current().filePath(
        QStringLiteral("tmp/ghostty-cli-boundary-XXXXXX")));
    QVERIFY(temporary.isValid());
    const QProcessEnvironment environment =
        controlledEnvironment(temporary.path());

    auto privateAction = runProcess(
        QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE),
        {QStringLiteral("+show-config-json")},
        environment, temporary.path());
    QVERIFY2(privateAction.has_value(),
             qPrintable(privateAction.has_value()
                 ? QString{} : privateAction.error()));
    QCOMPARE(privateAction->exitStatus, QProcess::NormalExit);
    QCOMPARE(privateAction->exitCode, 2);
    QVERIFY(privateAction->standardOutput.isEmpty());
    QVERIFY(privateAction->standardError.contains(
        QByteArrayLiteral("unsupported Ghostty CLI action")));

    auto multipleActions = runProcess(
        QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE),
        {QStringLiteral("+help"), QStringLiteral("+list-fonts")},
        environment, temporary.path());
    QVERIFY2(multipleActions.has_value(),
             qPrintable(multipleActions.has_value()
                 ? QString{} : multipleActions.error()));
    QCOMPARE(multipleActions->exitStatus, QProcess::NormalExit);
    QCOMPARE(multipleActions->exitCode, 2);
    QVERIFY(multipleActions->standardOutput.isEmpty());
    QVERIFY(multipleActions->standardError.contains(
        QByteArrayLiteral("multiple Ghostty CLI actions")));

    auto mixedVersion = runProcess(
        QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE),
        {QStringLiteral("+help"), QStringLiteral("--version")},
        environment, temporary.path());
    QVERIFY2(mixedVersion.has_value(),
             qPrintable(mixedVersion.has_value()
                 ? QString{} : mixedVersion.error()));
    QCOMPARE(mixedVersion->exitStatus, QProcess::NormalExit);
    QCOMPARE(mixedVersion->exitCode, 0);
    QVERIFY(mixedVersion->standardOutput.startsWith(
        QByteArrayLiteral("ghostty-qt ")));
    QVERIFY(!mixedVersion->standardOutput.contains(
        QByteArrayLiteral("app runtime")));

    auto frontendHelp = runProcess(
        QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE),
        {QStringLiteral("--help")}, environment, temporary.path());
    QVERIFY2(frontendHelp.has_value(),
             qPrintable(frontendHelp.has_value()
                 ? QString{} : frontendHelp.error()));
    QCOMPARE(frontendHelp->exitStatus, QProcess::NormalExit);
    QCOMPARE(frontendHelp->exitCode, 0);
    QVERIFY(frontendHelp->standardOutput.contains(
        QByteArrayLiteral("Linux Wayland terminal emulator")));
    QVERIFY(frontendHelp->standardOutput.contains(
        QByteArrayLiteral("-e PROGRAM [ARGUMENTS...]")));

#if GHOSTTY_QT_TEST_CONFIG_ENABLED
    for (const GhosttyCliActionCatalogEntry &entry
         : GhosttyPinnedCliActions) {
        if (!entry.isDelegated()) continue;
        QVERIFY(frontendHelp->standardOutput.contains(
            QByteArray(entry.argument.data(),
                       static_cast<qsizetype>(entry.argument.size()))));
    }
    auto helperUnsupported = runProcess(
        QStringLiteral(GHOSTTY_QT_TEST_REAL_HELPER),
        {QStringLiteral("+new-window")}, environment, temporary.path());
    QVERIFY2(helperUnsupported.has_value(),
             qPrintable(helperUnsupported.has_value()
                 ? QString{} : helperUnsupported.error()));
    QCOMPARE(helperUnsupported->exitStatus, QProcess::NormalExit);
    QCOMPARE(helperUnsupported->exitCode, 64);
    QVERIFY(helperUnsupported->standardOutput.isEmpty());
    QVERIFY(helperUnsupported->standardError.contains(
        QByteArrayLiteral("no supported public CLI action")));

    auto helperVersion = runProcess(
        QStringLiteral(GHOSTTY_QT_TEST_REAL_HELPER),
        {QStringLiteral("+help"), QStringLiteral("--version")},
        environment, temporary.path());
    QVERIFY2(helperVersion.has_value(),
             qPrintable(helperVersion.has_value()
                 ? QString{} : helperVersion.error()));
    QCOMPARE(helperVersion->exitStatus, QProcess::NormalExit);
    QCOMPARE(helperVersion->exitCode, 64);
    QVERIFY(helperVersion->standardOutput.isEmpty());
    QVERIFY(helperVersion->standardError.contains(
        QByteArrayLiteral("no supported public CLI action")));

    auto helperPrivateMix = runProcess(
        QStringLiteral(GHOSTTY_QT_TEST_REAL_HELPER),
        {QStringLiteral("+show-config-json"), QStringLiteral("+help")},
        environment, temporary.path());
    QVERIFY2(helperPrivateMix.has_value(),
             qPrintable(helperPrivateMix.has_value()
                 ? QString{} : helperPrivateMix.error()));
    QCOMPARE(helperPrivateMix->exitStatus, QProcess::NormalExit);
    QCOMPARE(helperPrivateMix->exitCode, 64);
    QVERIFY(helperPrivateMix->standardOutput.isEmpty());

    auto helperPrivateMissingScheme = runProcess(
        QStringLiteral(GHOSTTY_QT_TEST_REAL_HELPER),
        {QStringLiteral("+show-config-json")}, environment, temporary.path());
    QVERIFY2(helperPrivateMissingScheme.has_value(),
             qPrintable(helperPrivateMissingScheme.has_value()
                            ? QString{}
                            : helperPrivateMissingScheme.error()));
    QCOMPARE(helperPrivateMissingScheme->exitStatus, QProcess::NormalExit);
    QCOMPARE(helperPrivateMissingScheme->exitCode, 64);
    QVERIFY(helperPrivateMissingScheme->standardOutput.isEmpty());
    QVERIFY(helperPrivateMissingScheme->standardError.contains(
        QByteArrayLiteral("exactly one --ghostty-qt-color-scheme=light|dark")));

    auto helperPrivateInvalidScheme =
        runProcess(QStringLiteral(GHOSTTY_QT_TEST_REAL_HELPER),
                   {QStringLiteral("+show-config-json"),
                    QStringLiteral("--ghostty-qt-color-scheme=sepia")},
                   environment, temporary.path());
    QVERIFY2(helperPrivateInvalidScheme.has_value(),
             qPrintable(helperPrivateInvalidScheme.has_value()
                            ? QString{}
                            : helperPrivateInvalidScheme.error()));
    QCOMPARE(helperPrivateInvalidScheme->exitStatus, QProcess::NormalExit);
    QCOMPARE(helperPrivateInvalidScheme->exitCode, 64);
    QVERIFY(helperPrivateInvalidScheme->standardOutput.isEmpty());
    QVERIFY(helperPrivateInvalidScheme->standardError.contains(
        QByteArrayLiteral("exactly one --ghostty-qt-color-scheme=light|dark")));

    auto helperPrivateDuplicateScheme =
        runProcess(QStringLiteral(GHOSTTY_QT_TEST_REAL_HELPER),
                   {QStringLiteral("+show-config-json"),
                    QStringLiteral("--ghostty-qt-color-scheme=light"),
                    QStringLiteral("--ghostty-qt-color-scheme=dark")},
                   environment, temporary.path());
    QVERIFY2(helperPrivateDuplicateScheme.has_value(),
             qPrintable(helperPrivateDuplicateScheme.has_value()
                            ? QString{}
                            : helperPrivateDuplicateScheme.error()));
    QCOMPARE(helperPrivateDuplicateScheme->exitStatus, QProcess::NormalExit);
    QCOMPARE(helperPrivateDuplicateScheme->exitCode, 64);
    QVERIFY(helperPrivateDuplicateScheme->standardOutput.isEmpty());

    auto helperPrivateValidScheme =
        runProcess(QStringLiteral(GHOSTTY_QT_TEST_REAL_HELPER),
                   {QStringLiteral("+show-config-json"),
                    QStringLiteral("--ghostty-qt-color-scheme=dark")},
                   environment, temporary.path());
    QVERIFY2(helperPrivateValidScheme.has_value(),
             qPrintable(helperPrivateValidScheme.has_value()
                            ? QString{}
                            : helperPrivateValidScheme.error()));
    QCOMPARE(helperPrivateValidScheme->exitStatus, QProcess::NormalExit);
    QCOMPARE(helperPrivateValidScheme->exitCode, 0);
    QVERIFY(helperPrivateValidScheme->standardOutput.startsWith('{'));

    auto obsoletePrivateOption =
        runProcess(QStringLiteral(GHOSTTY_QT_TEST_REAL_HELPER),
                   {QStringLiteral("+show-config-json"),
                    QStringLiteral("--ghostty-qt-color-scheme=light"),
                    QStringLiteral("--default")},
                   environment, temporary.path());
    QVERIFY2(obsoletePrivateOption.has_value(),
             qPrintable(obsoletePrivateOption.has_value()
                 ? QString{} : obsoletePrivateOption.error()));
    QCOMPARE(obsoletePrivateOption->exitStatus, QProcess::NormalExit);
    QCOMPARE(obsoletePrivateOption->exitCode, 64);
    QVERIFY(obsoletePrivateOption->standardOutput.isEmpty());
    QVERIFY(obsoletePrivateOption->standardError.contains(
        QByteArrayLiteral("+show-config-json takes no options")));
#else
    for (const GhosttyCliActionCatalogEntry &entry
        : GhosttyPinnedCliActions) {
        if (!entry.isDelegated()) continue;
        const QByteArray actionName(
            entry.argument.data(),
            static_cast<qsizetype>(entry.argument.size()));
        QVERIFY(!frontendHelp->standardOutput.contains(actionName));
        auto action = runProcess(
            QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE),
            {QString::fromLatin1(actionName)}, environment,
            temporary.path());
        QVERIFY2(action.has_value(),
                 qPrintable(action.has_value()
                     ? QString{} : action.error()));
        QCOMPARE(action->exitStatus, QProcess::NormalExit);
        QCOMPARE(action->exitCode, 1);
        QVERIFY(action->standardOutput.isEmpty());
        QVERIFY(action->standardError.contains(
            QByteArrayLiteral("GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG")));
        QVERIFY(action->standardError.contains(actionName));
    }
#endif
}

QTEST_APPLESS_MAIN(GhosttyCliDelegationTest)

#include "test_ghostty_cli_delegation.moc"
