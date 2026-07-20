#include "launch_options.h"
#include "ghostty_application_keybindings.h"
#include "terminal_pane.h"
#include "terminal_workspace.h"

#if GHOSTTY_QT_CONFIG_ENABLED
#include "ghostty_config_process_loader.h"
#include "ghostty_config_service.h"
#endif

#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTextStream>
#include <QTimer>
#include <QtQml>

#include <memory>
#include <optional>
#include <utility>

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

bool installCloseDialogTestHook(QQmlApplicationEngine *engine,
                                TerminalWorkspace *workspace)
{
    QObject *const rootObject = engine->rootObjects().constFirst();
    QObject *const closeDialog =
        rootObject->findChild<QObject *>(QStringLiteral("closeDialog"));
    if (closeDialog == nullptr) {
        qCritical() << "Close-dialog test hook could not find the QML dialog";
        return false;
    }

    const auto acceptanceInvoked = std::make_shared<bool>(false);
    QObject::connect(
        workspace, &TerminalWorkspace::closeConfirmationRequested,
        closeDialog, [closeDialog, acceptanceInvoked](
                         quint64 confirmationId, const QString &) {
            QTimer::singleShot(0, closeDialog,
                               [closeDialog, acceptanceInvoked,
                                confirmationId] {
                if (!closeDialog->property("visible").toBool()) {
                    qCritical()
                        << "Close-dialog test hook observed a hidden QML dialog";
                    QCoreApplication::exit(1);
                    return;
                }
                if (confirmationId == 0
                    || closeDialog->property("confirmationId").toULongLong()
                        != confirmationId) {
                    qCritical()
                        << "Close-dialog test hook observed the wrong request ID";
                    QCoreApplication::exit(1);
                    return;
                }
                *acceptanceInvoked = true;
                if (!QMetaObject::invokeMethod(closeDialog, "accept")) {
                    *acceptanceInvoked = false;
                    qCritical()
                        << "Close-dialog test hook could not accept the QML dialog";
                    QCoreApplication::exit(1);
                }
            });
        });
    QObject::connect(
        workspace, &TerminalWorkspace::quitApproved,
        closeDialog, [acceptanceInvoked] {
            if (!*acceptanceInvoked) {
                qCritical() << "Close-dialog test hook quit without QML acceptance";
                QCoreApplication::exit(1);
            }
        });

    const auto requestSurfaceClose = [workspace] {
        TerminalPane *const pane = workspace->findChild<TerminalPane *>();
        if (pane == nullptr
            || !pane->executeConfiguredAction(
                QStringLiteral("toggle_readonly"))
            || !pane->executeConfiguredAction(
                QStringLiteral("close_surface"))) {
            qCritical()
                << "Close-dialog test hook could not request a surface close";
            QCoreApplication::exit(1);
        }
    };
    if (workspace->tabCount() > 0) {
        QTimer::singleShot(0, workspace, requestSurfaceClose);
    } else {
        QObject::connect(
            workspace, &TerminalWorkspace::tabTitlesChanged,
            workspace, requestSurfaceClose,
            Qt::SingleShotConnection);
    }
    return true;
}

enum class TitlePromptTestTarget {
    Surface,
    Tab,
};

bool installTitlePromptTestHook(QQmlApplicationEngine *engine,
                                TerminalWorkspace *workspace,
                                TitlePromptTestTarget target)
{
    QObject *const rootObject = engine->rootObjects().constFirst();
    QObject *const dialog =
        rootObject->findChild<QObject *>(QStringLiteral("titleDialog"));
    QObject *const field =
        rootObject->findChild<QObject *>(QStringLiteral("titleField"));
    if (dialog == nullptr || field == nullptr) {
        qCritical() << "Title-prompt test hook could not find the QML controls";
        return false;
    }

    const QString seed = QStringLiteral("existing title");
    const QString replacement = QStringLiteral("  QML 👻 title  ");
    const QString heading = target == TitlePromptTestTarget::Surface
        ? QStringLiteral("Change Terminal Title")
        : QStringLiteral("Change Tab Title");
    const QString setAction = target == TitlePromptTestTarget::Surface
        ? QStringLiteral("set_surface_title:existing title")
        : QStringLiteral("set_tab_title:existing title");
    const QString promptAction = target == TitlePromptTestTarget::Surface
        ? QStringLiteral("prompt_surface_title")
        : QStringLiteral("prompt_tab_title");
    const auto activePromptId = std::make_shared<quint64>(0);
    const auto acceptanceInvoked = std::make_shared<bool>(false);
    const auto focusedPane =
        std::make_shared<QPointer<TerminalPane>>(nullptr);
    QObject::connect(
        workspace, &TerminalWorkspace::titlePromptRequested,
        dialog,
        [dialog, field, seed, replacement, heading, activePromptId,
         acceptanceInvoked](quint64 promptId,
                            const QString &actualHeading,
                            const QString &initialTitle) {
            if (*activePromptId != 0 || promptId == 0
                || actualHeading != heading || initialTitle != seed) {
                qCritical()
                    << "Title-prompt test hook observed an invalid request";
                QCoreApplication::exit(1);
                return;
            }
            *activePromptId = promptId;
            QTimer::singleShot(
                0, dialog,
                [dialog, field, replacement, acceptanceInvoked] {
                    if (!dialog->property("visible").toBool()
                        || field->property("text").toString()
                            != QStringLiteral("existing title")
                        || !field->property("activeFocus").toBool()
                        || field->property("cursorPosition").toInt()
                            != field->property("length").toInt()) {
                        qCritical()
                            << "Title-prompt test hook observed invalid QML state";
                        QCoreApplication::exit(1);
                        return;
                    }
                    field->setProperty("text", replacement);
                    *acceptanceInvoked = true;
                    if (!QMetaObject::invokeMethod(dialog, "accept")) {
                        *acceptanceInvoked = false;
                        qCritical()
                            << "Title-prompt test hook could not accept the dialog";
                        QCoreApplication::exit(1);
                    }
                });
        });
    QObject::connect(
        workspace, &TerminalWorkspace::titlePromptResolved,
        dialog,
        [workspace, dialog, replacement, target, activePromptId,
         acceptanceInvoked, focusedPane](quint64 promptId) {
            if (!*acceptanceInvoked || promptId != *activePromptId) {
                qCritical() << "Title-prompt test hook resolved the wrong request";
                QCoreApplication::exit(1);
                return;
            }
            *activePromptId = 0;
            QTimer::singleShot(0, dialog, [workspace, dialog, replacement,
                                           target, focusedPane] {
                const TabListEntry *entry = workspace->tabModel()->entryAt(
                    workspace->currentIndex());
                TerminalPane *const pane = focusedPane->data();
                bool committed = entry != nullptr && pane != nullptr
                    && pane->hasActiveFocus()
                    && workspace->currentTitle() == replacement;
                if (target == TitlePromptTestTarget::Surface) {
                    committed = committed
                        && pane->surfaceTitleOverride()
                            == std::optional<QString>{replacement}
                        && workspace->executeSurfaceActionOnAllPanes(
                            QStringLiteral("set_surface_title:changed base"))
                        && workspace->currentTitle() == replacement;
                } else {
                    committed = committed
                        && entry->titleOverride == replacement;
                }
                if (dialog->property("visible").toBool() || !committed) {
                    qCritical()
                        << "Title-prompt test hook did not commit through QML";
                    QCoreApplication::exit(1);
                    return;
                }
                QCoreApplication::quit();
            });
        });

    const auto exercise = [workspace, seed, setAction, promptAction,
                           focusedPane] {
        TerminalPane *const pane = workspace->findChild<TerminalPane *>();
        if (pane == nullptr) {
            qCritical() << "Title-prompt test hook could not find a pane";
            QCoreApplication::exit(1);
            return;
        }
        pane->forceActiveFocus();
        *focusedPane = pane;
        QTimer::singleShot(0, workspace,
                           [workspace, seed, setAction, promptAction,
                            focusedPane] {
            if (focusedPane->isNull()
                || !focusedPane->data()->hasActiveFocus()
                || !workspace->executeSurfaceActionOnAllPanes(setAction)
                || workspace->currentTitle() != seed
                || !workspace->executeSurfaceActionOnAllPanes(promptAction)) {
                qCritical()
                    << "Title-prompt test hook could not start the prompt";
                QCoreApplication::exit(1);
            }
        });
    };
    if (workspace->tabCount() > 0) {
        QTimer::singleShot(0, workspace, exercise);
    } else {
        QObject::connect(
            workspace, &TerminalWorkspace::tabTitlesChanged,
            workspace,
            [workspace, exercise] {
                QTimer::singleShot(0, workspace, exercise);
            },
            Qt::SingleShotConnection);
    }
    return true;
}

bool installFullscreenActionTestHook(QQmlApplicationEngine *engine,
                                     TerminalWorkspace *workspace)
{
    auto *const window = qobject_cast<QQuickWindow *>(
        engine->rootObjects().constFirst());
    if (window == nullptr) {
        qCritical() << "Fullscreen test hook could not find the QML window";
        return false;
    }

    const auto exercise = [workspace, window] {
        workspace->splitRight();
        if (workspace->findChildren<TerminalPane *>().size() != 2) {
            qCritical() << "Fullscreen test hook could not create two panes";
            QCoreApplication::exit(1);
            return;
        }
        const QWindow::Visibility originalVisibility = window->visibility();
        if (!workspace->executeSurfaceActionOnAllPanes(
                QStringLiteral("toggle_fullscreen"))) {
            qCritical() << "Fullscreen test hook could not enter fullscreen";
            QCoreApplication::exit(1);
            return;
        }
        QTimer::singleShot(0, workspace,
                           [workspace, window, originalVisibility] {
            if (window->visibility() != QWindow::FullScreen) {
                qCritical() << "QML window did not enter fullscreen";
                QCoreApplication::exit(1);
                return;
            }
            if (!workspace->executeSurfaceActionOnAllPanes(
                    QStringLiteral("toggle_fullscreen"))) {
                qCritical() << "Fullscreen test hook could not leave fullscreen";
                QCoreApplication::exit(1);
                return;
            }
            QTimer::singleShot(0, workspace,
                               [window, originalVisibility] {
                if (window->visibility() != originalVisibility) {
                    qCritical() << "QML window did not restore its visibility";
                    QCoreApplication::exit(1);
                    return;
                }
                QCoreApplication::quit();
            });
        });
    };

    if (workspace->tabCount() > 0) {
        QTimer::singleShot(0, workspace, exercise);
    } else {
        QObject::connect(workspace, &TerminalWorkspace::tabTitlesChanged,
                         workspace,
                         [workspace, exercise] {
            // The model notification precedes activateTab(); defer until the
            // new tab's active pane and geometry have been installed.
            QTimer::singleShot(0, workspace, exercise);
        }, Qt::SingleShotConnection);
    }
    return true;
}

bool verifyTabBarTestState(TerminalWorkspace *workspace,
                           QObject *tabBar,
                           int expectedCount,
                           bool expectedVisible,
                           const char *stage)
{
    const bool qmlVisible = tabBar->property("visible").toBool();
    if (workspace->tabCount() == expectedCount
        && workspace->tabBarVisible() == expectedVisible
        && qmlVisible == expectedVisible) {
        return true;
    }

    qCritical().nospace()
        << "Tab-bar test hook mismatch at " << stage
        << ": count=" << workspace->tabCount()
        << ", workspace-visible=" << workspace->tabBarVisible()
        << ", qml-visible=" << qmlVisible;
    QCoreApplication::exit(1);
    return false;
}

bool installTabBarVisibilityTestHook(QQmlApplicationEngine *engine,
                                     TerminalWorkspace *workspace)
{
    QObject *const rootObject = engine->rootObjects().constFirst();
    QObject *const tabBar =
        rootObject->findChild<QObject *>(QStringLiteral("windowTabBar"));
    QObject *const windowToolbar =
        rootObject->findChild<QObject *>(QStringLiteral("windowToolbar"));
    if (tabBar == nullptr || windowToolbar == nullptr) {
        qCritical() << "Tab-bar test hook could not find the QML controls";
        return false;
    }

    const auto applyMode = [workspace](const QString &mode) {
        GhosttyConfigSnapshot snapshot;
        snapshot.availability = GhosttyConfigAvailability::Available;
        snapshot.values.insert(QStringLiteral("confirm-close-surface"),
                               QStringLiteral("false"));
        snapshot.values.insert(QStringLiteral("window-show-tab-bar"), mode);
        workspace->applyConfigSnapshot(snapshot);
    };
    const auto exercise = [workspace, tabBar, windowToolbar, applyMode] {
        auto *const timer = new QTimer(workspace);
        timer->setSingleShot(true);
        const auto stage = std::make_shared<int>(0);
        const auto quitObserved = std::make_shared<bool>(false);

        QObject::connect(
            timer, &QTimer::timeout, workspace,
            [workspace, tabBar, windowToolbar, applyMode, timer, stage,
             quitObserved] {
                switch (*stage) {
                case 0:
                    if (!verifyTabBarTestState(
                            workspace, tabBar, 1, false, "auto with one tab")) {
                        return;
                    }
                    applyMode(QStringLiteral("auto"));
                    workspace->newTab();
                    break;
                case 1:
                    if (!verifyTabBarTestState(
                            workspace, tabBar, 2, true, "auto with two tabs")) {
                        return;
                    }
                    workspace->closeCurrentTab();
                    break;
                case 2:
                    if (!verifyTabBarTestState(
                            workspace, tabBar, 1, false,
                            "auto after returning to one tab")) {
                        return;
                    }
                    applyMode(QStringLiteral("always"));
                    break;
                case 3:
                    if (!verifyTabBarTestState(
                            workspace, tabBar, 1, true, "always with one tab")) {
                        return;
                    }
                    applyMode(QStringLiteral("never"));
                    break;
                case 4:
                    if (!verifyTabBarTestState(
                            workspace, tabBar, 1, false, "never with one tab")) {
                        return;
                    }
                    if (!windowToolbar->property("visible").toBool()) {
                        qCritical()
                            << "Tab-bar test hook hid the surrounding toolbar";
                        QCoreApplication::exit(1);
                        return;
                    }
                    applyMode(QStringLiteral("auto"));
                    break;
                case 5:
                    if (!verifyTabBarTestState(
                            workspace, tabBar, 1, false,
                            "auto restored before shutdown")) {
                        return;
                    }
                    QObject::connect(
                        workspace, &TerminalWorkspace::quitApproved,
                        tabBar,
                        [workspace, tabBar, quitObserved] {
                            *quitObserved = true;
                            if (verifyTabBarTestState(
                                    workspace, tabBar, 0, false,
                                    "auto during zero-tab shutdown")) {
                                QCoreApplication::quit();
                            }
                        },
                        Qt::SingleShotConnection);
                    workspace->closeCurrentTab();
                    break;
                default:
                    if (!*quitObserved) {
                        qCritical() << "Tab-bar test hook did not observe shutdown";
                        QCoreApplication::exit(1);
                    }
                    return;
                }

                ++*stage;
                timer->start(*stage == 6 ? 1000 : 0);
            });
        timer->start(0);
    };

    if (workspace->tabCount() > 0) {
        QTimer::singleShot(0, workspace, exercise);
    } else {
        QObject::connect(workspace, &TerminalWorkspace::tabTitlesChanged,
                         workspace,
                         [workspace, exercise] {
            QTimer::singleShot(0, workspace, exercise);
        }, Qt::SingleShotConnection);
    }
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    QStringList arguments;
    arguments.reserve(argc);
    for (int index = 0; index < argc; ++index) {
        arguments.append(QString::fromLocal8Bit(argv[index]));
    }

    auto parsedOptions = parseLaunchOptions(arguments);
    if (!parsedOptions) {
        QTextStream(stderr) << "ghostty-qt: " << parsedOptions.error() << '\n'
                            << "Try 'ghostty-qt --help' for usage.\n";
        return 2;
    }
    LaunchOptions options = std::move(*parsedOptions);
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
        if (!installCloseDialogTestHook(&engine, workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue(
            "GHOSTTY_QT_TEST_TAB_TITLE_PROMPT") == 1) {
        if (!installTitlePromptTestHook(
                &engine, workspace, TitlePromptTestTarget::Tab)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue(
            "GHOSTTY_QT_TEST_SURFACE_TITLE_PROMPT") == 1) {
        if (!installTitlePromptTestHook(
                &engine, workspace, TitlePromptTestTarget::Surface)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue(
            "GHOSTTY_QT_TEST_TOGGLE_FULLSCREEN") == 1) {
        if (!installFullscreenActionTestHook(&engine, workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue(
            "GHOSTTY_QT_TEST_TAB_BAR_VISIBILITY") == 1) {
        if (!installTabBarVisibilityTestHook(&engine, workspace)) {
            return 1;
        }
    }

    return application.exec();
}
