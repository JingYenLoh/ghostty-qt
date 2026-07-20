#include "application_controller.h"
#include "launch_options.h"
#include "single_instance_activation.h"
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

bool installCloseDialogTestHook(QObject *rootObject,
                                TerminalWorkspace *workspace)
{
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
        workspace, &TerminalWorkspace::windowCloseApproved,
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

enum class ApplicationLifetimeTestMode {
    None,
    ResidentAfterWindowClose,
    ExternalActivation,
    ExplicitQuit,
};

ApplicationLifetimeTestMode applicationLifetimeTestMode()
{
    const QByteArray mode = qgetenv("GHOSTTY_QT_TEST_APPLICATION_LIFETIME");
    if (mode == "resident") {
        return ApplicationLifetimeTestMode::ResidentAfterWindowClose;
    }
    if (mode == "explicit-quit") {
        return ApplicationLifetimeTestMode::ExplicitQuit;
    }
    if (mode == "external-activation") {
        return ApplicationLifetimeTestMode::ExternalActivation;
    }
    return ApplicationLifetimeTestMode::None;
}

bool installApplicationLifetimeTestHook(
    QWindow *applicationWindow,
    TerminalWorkspace *workspace,
    ApplicationController *controller,
    const LaunchOptions &options,
    ApplicationLifetimeTestMode mode,
    bool *completed)
{
    if (mode == ApplicationLifetimeTestMode::None) return true;
    if (options.quitAfterLastWindowClosed) {
        qCritical()
            << "Application-lifetime test hook requires the resident policy";
        return false;
    }

    ApplicationLifetimeController *const lifetime =
        controller->lifetimeController();
    if (mode == ApplicationLifetimeTestMode::ResidentAfterWindowClose
        || mode == ApplicationLifetimeTestMode::ExternalActivation) {
        struct ResidentTestState {
            int retiredWindows = 0;
            bool replacementObserved = false;
            QPointer<QWindow> expectedRetiredWindow;
            QPointer<TerminalWorkspace> expectedRetiredWorkspace;
        };
        const auto state = std::make_shared<ResidentTestState>();
        state->expectedRetiredWindow = applicationWindow;
        state->expectedRetiredWorkspace = workspace;

        QObject::connect(
            controller, &ApplicationController::windowCreationFailed,
            controller, [](const QString &message) {
                qCritical().noquote()
                    << "Resident lifetime test could not recreate a window:"
                    << message;
                QCoreApplication::exit(1);
            },
            Qt::SingleShotConnection);
        QObject::connect(
            controller, &ApplicationController::windowRetired,
            controller,
            [controller, lifetime, completed, state, mode] {
                QTimer::singleShot(
                    50, controller,
                    [controller, lifetime, completed, state, mode] {
                        if (lifetime->hasOpenWindow()
                            || lifetime->quitPending()
                            || lifetime->hasRequestedQuit()
                            || controller->windowCount() != 0
                            || !state->expectedRetiredWindow.isNull()
                            || !state->expectedRetiredWorkspace.isNull()) {
                            qCritical()
                                << "Resident lifetime policy did not retire the final window cleanly";
                            QCoreApplication::exit(1);
                            return;
                        }

                        if (state->retiredWindows++ == 0) {
                            QObject::connect(
                                controller,
                                &ApplicationController::windowCreated,
                                controller,
                                [controller, lifetime, state, mode](
                                    QQuickWindow *replacement,
                                    TerminalWorkspace *replacementWorkspace) {
                                    if (!lifetime->hasOpenWindow()
                                        || controller->windowCount() != 1) {
                                        qCritical()
                                            << "Resident lifetime policy did not register the replacement window";
                                        QCoreApplication::exit(1);
                                        return;
                                    }
                                    state->expectedRetiredWindow = replacement;
                                    state->expectedRetiredWorkspace =
                                        replacementWorkspace;
                                    state->replacementObserved = true;
                                    if (mode
                                        == ApplicationLifetimeTestMode::ExternalActivation) {
                                        QTextStream(stdout)
                                            << "GHOSTTY_QT_ACTIVATION_ACCEPTED\n"
                                            << Qt::flush;
                                    }
                                    const auto closeReplacement =
                                        [replacementWorkspace] {
                                            replacementWorkspace
                                                ->requestWindowClose();
                                        };
                                    if (replacementWorkspace->tabCount() > 0) {
                                        QTimer::singleShot(
                                            0, replacementWorkspace,
                                            closeReplacement);
                                    } else {
                                        QObject::connect(
                                            replacementWorkspace,
                                            &TerminalWorkspace::tabTitlesChanged,
                                            replacementWorkspace,
                                            closeReplacement,
                                            Qt::SingleShotConnection);
                                    }
                                },
                                Qt::SingleShotConnection);
                            if (mode
                                == ApplicationLifetimeTestMode::ExternalActivation) {
                                QTextStream(stdout)
                                    << "GHOSTTY_QT_ACTIVATION_READY\n"
                                    << Qt::flush;
                                return;
                            }
                            if (!controller->dispatch(
                                    ApplicationAction::NewWindow)) {
                                qCritical()
                                    << "Resident lifetime test could not queue a replacement window";
                                QCoreApplication::exit(1);
                            }
                            return;
                        }

                        if (!state->replacementObserved) {
                            qCritical()
                                << "Resident lifetime test retired without a replacement";
                            QCoreApplication::exit(1);
                            return;
                        }

                        *completed = true;
                        (void) controller->dispatch(
                            ApplicationAction::Quit);
                    });
            });
    } else {
        QObject::connect(
            controller, &ApplicationController::applicationQuitCommitted,
            lifetime, [completed] { *completed = true; },
            Qt::SingleShotConnection);
    }

    const auto request = [controller, workspace, mode] {
        if (mode == ApplicationLifetimeTestMode::ExplicitQuit) {
            if (!controller->dispatch(ApplicationAction::Quit,
                                      workspace)) {
                qCritical() << "Could not execute the explicit quit test action";
                QCoreApplication::exit(1);
            }
            return;
        }
        workspace->requestWindowClose();
    };
    if (workspace->tabCount() > 0) {
        QTimer::singleShot(0, workspace, request);
    } else {
        QObject::connect(
            workspace, &TerminalWorkspace::tabTitlesChanged,
            workspace, request, Qt::SingleShotConnection);
    }
    return true;
}

bool installTitlePromptTestHook(QObject *rootObject,
                                TerminalWorkspace *workspace,
                                TitlePromptTestTarget target)
{
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

bool installFullscreenActionTestHook(QQuickWindow *window,
                                     TerminalWorkspace *workspace)
{
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

bool installTabBarVisibilityTestHook(QObject *rootObject,
                                     TerminalWorkspace *workspace)
{
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
                        workspace, &TerminalWorkspace::windowCloseApproved,
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
    // Ghostty owns last-window process lifetime, including disabled and
    // delayed modes. Qt's implicit auto-quit would bypass that policy.
    application.setQuitOnLastWindowClosed(false);
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

    LaunchOptions effectiveApplicationOptions = options;
#if GHOSTTY_QT_CONFIG_ENABLED
    if (configService.hasSnapshot()) {
        effectiveApplicationOptions = applyGhosttyConfigSnapshot(
            options, configService.snapshot());
    }
#endif

    std::unique_ptr<SingleInstanceActivation> activationEndpoint;
    if (shouldUseSingleInstance(
            effectiveApplicationOptions, arguments.size(),
            QByteArrayView(qgetenv("TERM_PROGRAM")))) {
        auto candidate = std::make_unique<SingleInstanceActivation>();
        const SingleInstanceActivation::StartResult activation =
            candidate->start();
        switch (activation.role) {
        case SingleInstanceActivation::Role::ActivatedExisting:
            return 0;
        case SingleInstanceActivation::Role::Failed:
            qCritical().noquote() << activation.diagnostic;
            return 1;
        case SingleInstanceActivation::Role::Independent:
            if (!activation.diagnostic.isEmpty()) {
                qWarning().noquote() << activation.diagnostic;
            }
            break;
        case SingleInstanceActivation::Role::Primary:
            activationEndpoint = std::move(candidate);
            break;
        }
    }

    // The engine and process controller both outlive every QML root. Their
    // declaration order tears down the controller, portal, windows, and pane
    // workers before the engine itself.
    QQmlApplicationEngine engine;
    ApplicationController applicationController(
        engine, effectiveApplicationOptions);
    QObject::connect(
        &applicationController, &ApplicationController::quitRequested,
        &application, &QCoreApplication::quit);
    QObject::connect(
        &applicationController, &ApplicationController::windowCreationFailed,
        &application, [](const QString &message) {
            qWarning().noquote()
                << "Could not create a new terminal window:" << message;
        });

#if GHOSTTY_QT_CONFIG_ENABLED
    QObject::connect(
        &configService, &GhosttyConfigService::changed,
        &applicationController,
        [&applicationController, options](
            const GhosttyConfigSnapshot &snapshot) {
            applicationController.applyLaunchOptions(
                applyGhosttyConfigSnapshot(options, snapshot));
        });
    QObject::connect(&configService, &GhosttyConfigService::changed,
                     &application, &reportConfigDiagnostics);
    QObject::connect(
        &applicationController, &ApplicationController::configReloadRequested,
        &configService, &GhosttyConfigService::requestReload);
#endif

    const std::expected<ApplicationWindow, QString> initialWindow =
        applicationController.createInitialWindow();
    if (!initialWindow.has_value()) {
        qCritical().noquote() << "Could not create the primary application window:"
                              << initialWindow.error();
        return 1;
    }
    if (activationEndpoint) {
        activationEndpoint->setActivationHandler(
            [controller = QPointer(&applicationController)] {
                return controller != nullptr
                    && controller->activateNoCommand();
            });
    }
    QQuickWindow *const applicationWindow = initialWindow->window;
    TerminalWorkspace *const workspace = initialWindow->workspace;

    const ApplicationLifetimeTestMode lifetimeTestMode =
        applicationLifetimeTestMode();
    bool lifetimeTestCompleted = false;
    if (!installApplicationLifetimeTestHook(
            applicationWindow, workspace, &applicationController,
            effectiveApplicationOptions, lifetimeTestMode,
            &lifetimeTestCompleted)) {
        return 1;
    }

    // Headless regression hook: exercise the real QML confirmation dialog and
    // complete shutdown without synthesizing compositor input. It is inert in
    // every normal launch.
    if (qEnvironmentVariableIntValue(
            "GHOSTTY_QT_TEST_CONFIRM_CLOSE_DIALOG") == 1) {
        if (!installCloseDialogTestHook(applicationWindow, workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue(
            "GHOSTTY_QT_TEST_TAB_TITLE_PROMPT") == 1) {
        if (!installTitlePromptTestHook(
                applicationWindow, workspace, TitlePromptTestTarget::Tab)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue(
            "GHOSTTY_QT_TEST_SURFACE_TITLE_PROMPT") == 1) {
        if (!installTitlePromptTestHook(
                applicationWindow, workspace, TitlePromptTestTarget::Surface)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue(
            "GHOSTTY_QT_TEST_TOGGLE_FULLSCREEN") == 1) {
        if (!installFullscreenActionTestHook(applicationWindow, workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue(
            "GHOSTTY_QT_TEST_TAB_BAR_VISIBILITY") == 1) {
        if (!installTabBarVisibilityTestHook(applicationWindow, workspace)) {
            return 1;
        }
    }

    const int exitCode = application.exec();
    if (lifetimeTestMode != ApplicationLifetimeTestMode::None
        && !lifetimeTestCompleted) {
        qCritical() << "Application-lifetime test hook did not complete";
        return 1;
    }
    return exitCode;
}
