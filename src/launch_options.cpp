#include "launch_options.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>

#include <cmath>
#include <utility>

namespace {

constexpr int kMaximumScrollbackLines = 10'000'000;

bool fail(QString *errorMessage, const QString &message)
{
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
    return false;
}

} // namespace

bool parseLaunchOptions(const QStringList &arguments, LaunchOptions *options,
                        QString *errorMessage)
{
    if (options == nullptr) {
        return fail(errorMessage, QStringLiteral("No output options object was provided."));
    }
    if (arguments.isEmpty()) {
        return fail(errorMessage, QStringLiteral("The argument list must include the application name."));
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("A Qt terminal emulator powered by libghostty."));

    const QCommandLineOption workingDirectoryOption(
        QStringLiteral("working-directory"),
        QStringLiteral("Start the command in <directory>."),
        QStringLiteral("directory"));
    const QCommandLineOption fontFamilyOption(
        QStringLiteral("font-family"),
        QStringLiteral("Use <family> for terminal text."),
        QStringLiteral("family"));
    const QCommandLineOption fontSizeOption(
        QStringLiteral("font-size"),
        QStringLiteral("Use a font size of <points>."),
        QStringLiteral("points"));
    const QCommandLineOption scrollbackLinesOption(
        QStringLiteral("scrollback-lines"),
        QStringLiteral("Keep up to <lines> lines of scrollback."),
        QStringLiteral("lines"));
    const QCommandLineOption holdOption(
        QStringLiteral("hold"),
        QStringLiteral("Keep the terminal open after the child process exits."));
    const QCommandLineOption helpOption(
        {QStringLiteral("h"), QStringLiteral("help")},
        QStringLiteral("Show command-line help."));
    const QCommandLineOption versionOption(
        {QStringLiteral("v"), QStringLiteral("version")},
        QStringLiteral("Show version information."));

    parser.addOptions({workingDirectoryOption, fontFamilyOption, fontSizeOption,
                       scrollbackLinesOption, holdOption, helpOption, versionOption});
    parser.addPositionalArgument(
        QStringLiteral("program"),
        QStringLiteral("Program and arguments to execute after --."),
        QStringLiteral("[program [arguments...]]"));

    if (!parser.parse(arguments)) {
        return fail(errorMessage, parser.errorText());
    }

    LaunchOptions parsed;
    parsed.workingDirectory = QDir::currentPath();

    if (parser.isSet(workingDirectoryOption)) {
        const QString directory = QDir::cleanPath(parser.value(workingDirectoryOption));
        const QFileInfo directoryInfo(directory);
        if (!directoryInfo.exists() || !directoryInfo.isDir()) {
            return fail(errorMessage,
                        QStringLiteral("Working directory does not exist or is not a directory: %1")
                            .arg(directory));
        }
        parsed.workingDirectory = directory;
    }

    if (parser.isSet(fontFamilyOption)) {
        parsed.fontFamily = parser.value(fontFamilyOption);
    }

    if (parser.isSet(fontSizeOption)) {
        const QString value = parser.value(fontSizeOption);
        bool ok = false;
        const double fontSize = QLocale::c().toDouble(value, &ok);
        if (!ok || !std::isfinite(fontSize) || fontSize <= 0.0) {
            return fail(errorMessage,
                        QStringLiteral("Invalid font size '%1': expected a finite number greater than 0.")
                            .arg(value));
        }
        parsed.fontSize = fontSize;
    }

    if (parser.isSet(scrollbackLinesOption)) {
        const QString value = parser.value(scrollbackLinesOption);
        bool ok = false;
        const qlonglong scrollbackLines = value.toLongLong(&ok);
        if (!ok || scrollbackLines < 0 || scrollbackLines > kMaximumScrollbackLines) {
            return fail(
                errorMessage,
                QStringLiteral("Invalid scrollback line count '%1': expected an integer from 0 to %2.")
                    .arg(value)
                    .arg(kMaximumScrollbackLines));
        }
        parsed.scrollbackLines = static_cast<int>(scrollbackLines);
    }

    parsed.hold = parser.isSet(holdOption);
    parsed.showHelp = parser.isSet(helpOption);
    parsed.showVersion = parser.isSet(versionOption);
    parsed.program = parser.positionalArguments();

    *options = std::move(parsed);
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

bool parseLaunchOptions(QCoreApplication &application, LaunchOptions *options,
                        QString *errorMessage)
{
    return parseLaunchOptions(application.arguments(), options, errorMessage);
}
