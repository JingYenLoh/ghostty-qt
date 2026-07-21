#include "ghostty_cli_delegation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <array>
#include <expected>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"),
                       QStringLiteral("ghostty-cli-test-must-not-load-qt"));
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    environment.insert(QStringLiteral("PAGER"), QStringLiteral("cat"));
    environment.remove(QStringLiteral("DISPLAY"));
    environment.remove(QStringLiteral("EDITOR"));
    environment.remove(QStringLiteral("VISUAL"));
    environment.remove(QStringLiteral("WAYLAND_DISPLAY"));
    return environment;
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

} // namespace

class GhosttyCliDelegationTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void classifiesRawArguments();
    void replacementPreservesProcessContract();
    void matchesPinnedHelper_data();
    void matchesPinnedHelper();
    void editConfigUsesPinnedEditorContract();
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

    for (const std::string_view action : GhosttyQtDelegatedCliActions) {
        expect({"ghostty-qt", action},
               GhosttyCliActionDisposition::Delegate, action);
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

    constexpr std::array DeferredActions{
        "+boo",
        "+crash-report",
        "+edit-config=now",
        "+list-fonts",
        "+list-themes",
        "+new-window",
        "+show-face",
        "+ssh",
        "+ssh-cache",
        "+toggle-quick-terminal",
        "+version",
        "+show-config-json",
        "+unknown",
        "+Help",
        "+helpful",
        "+help=now",
    };
    for (const std::string_view action : DeferredActions) {
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
    const QProcessEnvironment environment =
        controlledEnvironment(temporary.path());
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

#if GHOSTTY_QT_TEST_CONFIG_ENABLED
    QVERIFY(frontendHelp->standardOutput.contains(
        QByteArrayLiteral("+validate-config")));
    QVERIFY(frontendHelp->standardOutput.contains(
        QByteArrayLiteral("+edit-config")));
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
#else
    QVERIFY(!frontendHelp->standardOutput.contains(
        QByteArrayLiteral("+validate-config")));
    auto action = runProcess(
        QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE),
        {QStringLiteral("+edit-config")}, environment, temporary.path());
    QVERIFY2(action.has_value(),
             qPrintable(action.has_value() ? QString{} : action.error()));
    QCOMPARE(action->exitStatus, QProcess::NormalExit);
    QCOMPARE(action->exitCode, 1);
    QVERIFY(action->standardOutput.isEmpty());
    QVERIFY(action->standardError.contains(
        QByteArrayLiteral("GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG")));
    QVERIFY(action->standardError.contains(
        QByteArrayLiteral("+edit-config")));
#endif
}

QTEST_APPLESS_MAIN(GhosttyCliDelegationTest)

#include "test_ghostty_cli_delegation.moc"
