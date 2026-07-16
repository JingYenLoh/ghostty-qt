#include "launch_options.h"
#include "ghostty_application_keybindings.h"
#include "terminal_workspace.h"

#if GHOSTTY_QT_CONFIG_ENABLED
#include "ghostty_config_process_loader.h"
#include "ghostty_config_service.h"
#endif

#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTextStream>
#include <QTimer>
#include <QtQml>

namespace {

void printHelp()
{
    QTextStream output(stdout);
    output << "Usage: ghostty-qt [options] [-- program [arguments...]]\n\n"
              "Linux Wayland terminal emulator powered by libghostty-vt.\n\n"
              "Options:\n"
              "  -h, --help                    Show this help.\n"
              "  -v, --version                 Show version information.\n"
              "      --working-directory DIR   Start the command in DIR.\n"
              "      --font-family FAMILY      Use FAMILY for terminal text.\n"
              "      --font-size POINTS        Set font size (default: 12).\n"
              "      --scrollback-lines LINES  Estimate capacity for LINES (default: 10000).\n"
              "      --hold                    Keep the pane after the command exits.\n";
}

#if GHOSTTY_QT_CONFIG_ENABLED
void reportConfigDiagnostics(const GhosttyConfigSnapshot &snapshot)
{
    for (const GhosttyConfigDiagnostic &diagnostic : snapshot.diagnostics) {
        QString location;
        if (!diagnostic.sourcePath.isEmpty()) {
            location = diagnostic.sourcePath;
            if (diagnostic.line > 0) {
                location += QStringLiteral(":%1").arg(diagnostic.line);
                if (diagnostic.column > 0) {
                    location += QStringLiteral(":%1").arg(diagnostic.column);
                }
            }
            location += QStringLiteral(": ");
        }
        qWarning().noquote() << location + diagnostic.message;
    }
}
#endif

} // namespace

int main(int argc, char *argv[])
{
    QStringList arguments;
    arguments.reserve(argc);
    for (int index = 0; index < argc; ++index) {
        arguments.append(QString::fromLocal8Bit(argv[index]));
    }

    LaunchOptions options;
    QString parseError;
    if (!parseLaunchOptions(arguments, &options, &parseError)) {
        QTextStream(stderr) << "ghostty-qt: " << parseError << '\n'
                            << "Try 'ghostty-qt --help' for usage.\n";
        return 2;
    }
    if (options.showHelp) {
        printHelp();
        return 0;
    }
    if (options.showVersion) {
        QTextStream(stdout) << "ghostty-qt " << GHOSTTY_QT_VERSION << '\n';
        return 0;
    }

    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("wayland"));
    }

    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("ghostty-qt"));
    QCoreApplication::setApplicationVersion(QStringLiteral(GHOSTTY_QT_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("ghostty-qt"));

    const bool allowNonWayland = qEnvironmentVariableIntValue(
        "GHOSTTY_QT_ALLOW_NON_WAYLAND") == 1;
    if (QGuiApplication::platformName() != QStringLiteral("wayland") && !allowNonWayland) {
        QTextStream(stderr)
            << "ghostty-qt supports the Qt Wayland platform only (active platform: "
            << QGuiApplication::platformName() << ").\n";
        return 2;
    }

    TerminalWorkspace::setDefaultLaunchOptions(options);
    qmlRegisterType<TerminalWorkspace>("GhosttyQt", 1, 0, "TerminalWorkspace");

#if GHOSTTY_QT_CONFIG_ENABLED
    const QString configHelperPath = QDir(QCoreApplication::applicationDirPath())
                                         .filePath(QStringLiteral(
                                             GHOSTTY_QT_CONFIG_HELPER_NAME));
    GhosttyConfigService configService(makeGhosttyConfigProcessLoader({
        .helperPath = configHelperPath,
    }));
    if (!configService.hasSnapshot()) {
        qWarning().noquote()
            << "Ghostty configuration is unavailable; using built-in and command-line defaults"
            << QStringLiteral("(helper: %1):").arg(configHelperPath)
            << configService.lastError();
    } else {
        reportConfigDiagnostics(configService.snapshot());
    }
    QObject::connect(&configService, &GhosttyConfigService::reloadFailed,
                     &application, [](const QString &message) {
                         qWarning().noquote()
                             << "Ghostty configuration reload failed; keeping the last valid configuration:"
                             << message;
                     });
#endif

    QQmlApplicationEngine engine;
    engine.loadFromModule(QStringLiteral("GhosttyQt"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    TerminalWorkspace *workspace =
        engine.rootObjects().constFirst()->findChild<TerminalWorkspace *>();
    if (workspace == nullptr) {
        qCritical() << "QML root does not contain a TerminalWorkspace";
        return 1;
    }

    LaunchOptions effectiveApplicationOptions = options;
#if GHOSTTY_QT_CONFIG_ENABLED
    if (configService.hasSnapshot()) {
        workspace->applyConfigSnapshot(configService.snapshot());
        effectiveApplicationOptions = applyGhosttyConfigSnapshot(
            options, configService.snapshot());
    }
#endif

    // Declared after the QML engine so the process-level portal and event
    // filter are torn down before any registered workspace objects.
    GhosttyApplicationKeybindings applicationKeybindings(
        effectiveApplicationOptions);
    applicationKeybindings.registerWorkspace(workspace);

#if GHOSTTY_QT_CONFIG_ENABLED
    QObject::connect(&configService, &GhosttyConfigService::changed,
                     workspace, &TerminalWorkspace::applyConfigSnapshot);
    QObject::connect(
        &configService, &GhosttyConfigService::changed,
        &applicationKeybindings,
        [&applicationKeybindings, options](
            const GhosttyConfigSnapshot &snapshot) {
            applicationKeybindings.applyLaunchOptions(
                applyGhosttyConfigSnapshot(options, snapshot));
        });
    QObject::connect(&configService, &GhosttyConfigService::changed,
                     &application, &reportConfigDiagnostics);
    QObject::connect(workspace, &TerminalWorkspace::configReloadRequested,
                     &configService, &GhosttyConfigService::requestReload);
#endif

    // Headless regression hook: exercise the real QML confirmation dialog and
    // complete shutdown without synthesizing compositor input. It is inert in
    // every normal launch.
    if (qEnvironmentVariableIntValue(
            "GHOSTTY_QT_TEST_CONFIRM_CLOSE_DIALOG") == 1) {
        QTimer::singleShot(100, workspace, &TerminalWorkspace::requestQuit);
        QTimer::singleShot(300, workspace, &TerminalWorkspace::confirmClose);
    }

    return application.exec();
}
