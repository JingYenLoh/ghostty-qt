#include "application_controller.h"
#include "desktop_activation.h"
#include "ghostty_cli_delegation.h"
#include "ghostty_config_edit.h"
#include "ghostty_config_service.h"
#include "launch_options.h"
#include "single_instance_activation.h"
#include "terminal_cell_metrics.h"
#include "terminal_pane.h"
#include "terminal_workspace.h"

#if GHOSTTY_QT_CONFIG_ENABLED
#include "ghostty_config_process_loader.h"
#endif

#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QTextStream>
#include <QTimer>

#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
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
              "      --scrollback-lines LINES  Estimate capacity for LINES "
              "(default: 10000).\n"
              "      --hold                    Keep the pane after the command "
              "exits.\n"
              "      --gtk-single-instance MODE  Use false, true, or detect "
              "uniqueness.\n"
              "      --initial-window BOOLEAN    Request an initial window.\n";
#if GHOSTTY_QT_CONFIG_ENABLED
    output << "\nPinned Ghostty CLI actions:\n";
    for (const GhosttyCliActionCatalogEntry &entry
         : GhosttyPinnedCliActions) {
        if (!entry.isDelegated()) continue;
        const std::string_view action = entry.argument;
        output << "  "
               << QString::fromLatin1(
                      action.data(), static_cast<qsizetype>(action.size()))
               << '\n';
    }
#endif
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

bool installSuppressedStartupTestHook(
    ApplicationController *controller,
    const LaunchOptions &options,
    bool *completed)
{
    ApplicationLifetimeController *const lifetime =
        controller->lifetimeController();
    if (options.initialWindow || controller->windowCount() != 0
        || lifetime->registeredWindowCount() != 0
        || lifetime->hasOpenWindow() || lifetime->quitPending()
        || lifetime->hasRequestedQuit()) {
        qCritical()
            << "Suppressed-startup test hook did not begin with an idle zero-window application";
        return false;
    }

    QObject::connect(
        controller, &ApplicationController::windowCreationFailed,
        controller, [](const QString &message) {
            qCritical().noquote()
                << "Suppressed-startup test could not create its first window:"
                << message;
            QCoreApplication::exit(1);
        },
        Qt::SingleShotConnection);
    QObject::connect(
        controller, &ApplicationController::windowCreated,
        controller,
        [controller, lifetime, options, completed](
            QQuickWindow *, TerminalWorkspace *workspace) {
            const LaunchOptions &actual =
                workspace->effectiveLaunchOptions();
            if (controller->windowCount() != 1
                || !lifetime->hasOpenWindow()
                || actual.program != options.program
                || actual.hold != options.hold) {
                qCritical()
                    << "Suppressed-startup test did not preserve first-surface options";
                QCoreApplication::exit(1);
                return;
            }

            QTextStream(stdout)
                << "GHOSTTY_QT_INITIAL_WINDOW_CREATED\n" << Qt::flush;
            QObject::connect(
                controller, &ApplicationController::windowRetired,
                controller, [controller, lifetime, completed] {
                    if (controller->windowCount() != 0
                        || lifetime->hasOpenWindow()) {
                        qCritical()
                            << "Suppressed-startup test did not retire its first window";
                        QCoreApplication::exit(1);
                        return;
                    }
                    *completed = true;
                },
                Qt::SingleShotConnection);

            const auto closeFirstWindow = [workspace] {
                workspace->requestWindowClose();
            };
            if (workspace->tabCount() > 0) {
                QTimer::singleShot(0, workspace, closeFirstWindow);
            } else {
                QObject::connect(
                    workspace, &TerminalWorkspace::tabTitlesChanged,
                    workspace, closeFirstWindow,
                    Qt::SingleShotConnection);
            }
        },
        Qt::SingleShotConnection);

    QTextStream(stdout)
        << "GHOSTTY_QT_INITIAL_WINDOW_READY\n" << Qt::flush;
    return true;
}

bool installDesktopActivationTestHook(
    ApplicationController *controller, bool *completed)
{
    ApplicationLifetimeController *const lifetime
        = controller->lifetimeController();
    if (controller->windowCount() != 0 || lifetime->registeredWindowCount() != 0
        || lifetime->hasOpenWindow()) {
        qCritical()
            << "Desktop-activation test hook did not begin with zero windows";
        return false;
    }

    QObject::connect(
        controller, &ApplicationController::windowCreationFailed, controller,
        [](const QString &message) {
            qCritical().noquote()
                << "Desktop activation could not create a window:" << message;
            QCoreApplication::exit(1);
        },
        Qt::SingleShotConnection);
    QObject::connect(
        controller, &ApplicationController::windowCreated, controller,
        [controller, lifetime, completed](QQuickWindow *, TerminalWorkspace *) {
            if (controller->windowCount() != 1
                || lifetime->registeredWindowCount() != 1
                || !lifetime->hasOpenWindow()) {
                qCritical()
                    << "Desktop activation did not create exactly one window";
                QCoreApplication::exit(1);
                return;
            }
            *completed = true;
            QTextStream(stdout) << "GHOSTTY_QT_DESKTOP_ACTIVATION_CREATED\n"
                                << Qt::flush;
            QTimer::singleShot(0, QCoreApplication::instance(),
                [] { QCoreApplication::quit(); });
        },
        Qt::SingleShotConnection);

    QTextStream(stdout) << "GHOSTTY_QT_DESKTOP_ACTIVATION_READY\n"
                        << Qt::flush;
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

bool installInitialWindowStateTestHook(QQuickWindow *window,
                                       const LaunchOptions &options)
{
    if (window == nullptr
        || options.windowWidth == 0 || options.windowHeight == 0) {
        qCritical()
            << "Initial-window-state test hook requires configured geometry";
        return false;
    }

    auto *const timer = new QTimer(window);
    timer->setSingleShot(true);
    const auto stage = std::make_shared<int>(0);
    const auto retries = std::make_shared<int>(0);
    QObject::connect(
        timer, &QTimer::timeout, window,
        [window, options, timer, stage, retries] {
            constexpr std::array expectedStates{
                QWindow::FullScreen,
                QWindow::Maximized,
                QWindow::FullScreen,
                QWindow::Maximized,
                QWindow::Windowed,
            };
            const QWindow::Visibility expected = expectedStates.at(
                static_cast<std::size_t>(*stage));
            const TerminalCellMetrics metrics = terminalCellMetrics(
                options.typography, window->devicePixelRatio());
            const QSize configuredSize(
                qCeil(metrics.cellWidth
                      * static_cast<qreal>(options.windowWidth)
                      + window->property("terminalChromeWidth").toDouble()),
                qCeil(metrics.cellHeight
                      * static_cast<qreal>(options.windowHeight)
                      + window->property("terminalChromeHeight").toDouble()));
            if (window->visibility() != expected
                || (*stage == 4 && window->size() != configuredSize)) {
                if (++*retries <= 100) {
                    timer->start(10);
                    return;
                }
                qCritical()
                    << "Initial-window-state test hook expected visibility"
                    << expected << "at stage" << *stage << "but observed"
                    << window->visibility() << "with size" << window->size()
                    << "instead of" << configuredSize;
                QCoreApplication::exit(1);
                return;
            }
            if (window->property("visibilityBeforeFullscreen").toInt()
                != static_cast<int>(QWindow::Maximized)) {
                qCritical()
                    << "Initial fullscreen window did not retain maximized restore state";
                QCoreApplication::exit(1);
                return;
            }

            *retries = 0;
            if (*stage == 4) {
                QCoreApplication::quit();
                return;
            }
            if (*stage == 0) {
                // Simulate a compositor or window-manager fullscreen exit,
                // which does not call the QML action helper.
                window->setVisibility(QWindow::Windowed);
            } else if (*stage == 1 || *stage == 2) {
                if (!QMetaObject::invokeMethod(
                        window, "toggleFullscreen", Qt::DirectConnection)) {
                    qCritical()
                        << "Initial-window-state test hook could not invoke the QML fullscreen toggle";
                    QCoreApplication::exit(1);
                    return;
                }
            } else {
                if (!QMetaObject::invokeMethod(
                        window, "toggleMaximize", Qt::DirectConnection)) {
                    qCritical()
                        << "Initial-window-state test hook could not invoke the QML maximize toggle";
                    QCoreApplication::exit(1);
                    return;
                }
            }
            ++*stage;
            timer->start(0);
        });
    timer->start(0);
    return true;
}

bool installInitialWindowSizeTestHook(QQuickWindow *window,
                                      TerminalWorkspace *workspace,
                                      const LaunchOptions &options)
{
    if (window == nullptr || workspace == nullptr
        || options.windowWidth == 0 || options.windowHeight == 0) {
        qCritical()
            << "Initial-window-size test hook requires a configured QML window:"
            << window << workspace << options.windowWidth
            << options.windowHeight;
        return false;
    }

    auto *const timer = new QTimer(window);
    timer->setSingleShot(true);
    const auto stage = std::make_shared<int>(0);
    const auto retries = std::make_shared<int>(0);
    QObject::connect(
        timer, &QTimer::timeout, window,
        [window, workspace, options, timer, stage, retries] {
            TerminalPane *const pane = [&]() -> TerminalPane * {
                const QList<TerminalPane *> panes =
                    workspace->findChildren<TerminalPane *>();
                return panes.size() == 1 ? panes.constFirst() : nullptr;
            }();
            const TerminalCellMetrics metrics = terminalCellMetrics(
                options.typography, window->devicePixelRatio());
            const qreal chromeWidth =
                window->property("terminalChromeWidth").toDouble();
            const qreal chromeHeight =
                window->property("terminalChromeHeight").toDouble();
            const QSize configuredSize(
                qCeil(metrics.cellWidth
                      * static_cast<qreal>(options.windowWidth)
                      + chromeWidth),
                qCeil(metrics.cellHeight
                      * static_cast<qreal>(options.windowHeight)
                      + chromeHeight));
            const QSize minimumSize(
                qCeil(metrics.cellWidth * 10.0 + chromeWidth),
                qCeil(metrics.cellHeight * 4.0 + chromeHeight));
            const bool shouldBeWindowed = *stage == 0
                || *stage == 2 || *stage == 4;
            const bool windowReady = pane != nullptr
                && (!shouldBeWindowed
                    || (window->visibility() == QWindow::Windowed
                        && window->size() == configuredSize
                        && window->minimumSize() == minimumSize
                        && static_cast<quint32>(std::floor(
                               pane->width() / metrics.cellWidth))
                            == options.windowWidth
                        && static_cast<quint32>(std::floor(
                               pane->height() / metrics.cellHeight))
                            == options.windowHeight));
            const QWindow::Visibility expectedState = *stage == 1
                ? QWindow::Maximized
                : (*stage == 3 ? QWindow::FullScreen : QWindow::Windowed);
            if (!windowReady || window->visibility() != expectedState) {
                if (++*retries <= 100) {
                    timer->start(10);
                    return;
                }
                qCritical()
                    << "Initial-window-size test hook failed at stage" << *stage
                    << "visibility" << window->visibility()
                    << "window" << window->size() << "minimum"
                    << window->minimumSize() << "pane"
                    << (pane != nullptr ? pane->size() : QSizeF())
                    << "expected window" << configuredSize
                    << "expected minimum" << minimumSize;
                QCoreApplication::exit(1);
                return;
            }

            *retries = 0;
            switch ((*stage)++) {
            case 0:
            case 1:
                if (!QMetaObject::invokeMethod(
                        window, "toggleMaximize", Qt::DirectConnection)) {
                    qCritical()
                        << "Initial-window-size test hook could not toggle maximize";
                    QCoreApplication::exit(1);
                    return;
                }
                break;
            case 2:
            case 3:
                if (!QMetaObject::invokeMethod(
                        window, "toggleFullscreen", Qt::DirectConnection)) {
                    qCritical()
                        << "Initial-window-size test hook could not toggle fullscreen";
                    QCoreApplication::exit(1);
                    return;
                }
                break;
            case 4:
                QCoreApplication::quit();
                return;
            default:
                Q_UNREACHABLE();
            }
            timer->start(0);
        });
    timer->start(0);
    return true;
}

bool installMaximizeActionTestHook(QQuickWindow *window,
                                   TerminalWorkspace *workspace)
{
    if (window == nullptr) {
        qCritical() << "Maximize test hook could not find the QML window";
        return false;
    }

    const auto exercise = [workspace, window] {
        workspace->splitRight();
        if (workspace->findChildren<TerminalPane *>().size() != 2) {
            qCritical() << "Maximize test hook could not create two panes";
            QCoreApplication::exit(1);
            return;
        }

        struct Step final {
            QWindow::Visibility expected;
            enum class Command {
                ToggleMaximize,
                ToggleFullscreen,
                Minimize,
                RestoreWindowed,
                Finish,
            } command;
        };
        using Command = Step::Command;
        constexpr std::array steps{
            Step{QWindow::Windowed, Command::ToggleMaximize},
            Step{QWindow::Maximized, Command::ToggleFullscreen},
            Step{QWindow::FullScreen, Command::ToggleFullscreen},
            Step{QWindow::Maximized, Command::ToggleMaximize},
            Step{QWindow::Windowed, Command::ToggleFullscreen},
            Step{QWindow::FullScreen, Command::ToggleMaximize},
            Step{QWindow::FullScreen, Command::ToggleFullscreen},
            Step{QWindow::Maximized, Command::ToggleFullscreen},
            Step{QWindow::FullScreen, Command::ToggleMaximize},
            Step{QWindow::FullScreen, Command::ToggleFullscreen},
            Step{QWindow::Windowed, Command::ToggleMaximize},
            Step{QWindow::Maximized, Command::ToggleMaximize},
            Step{QWindow::Windowed, Command::Minimize},
            Step{QWindow::Minimized, Command::ToggleFullscreen},
            Step{QWindow::FullScreen, Command::ToggleFullscreen},
            Step{QWindow::Minimized, Command::RestoreWindowed},
            Step{QWindow::Windowed, Command::Finish},
        };

        auto *const timer = new QTimer(workspace);
        timer->setSingleShot(true);
        const auto stage = std::make_shared<std::size_t>(0);
        const auto retries = std::make_shared<int>(0);
        QObject::connect(
            timer, &QTimer::timeout, workspace,
            [workspace, window, timer, stage, retries, steps] {
                const Step &step = steps.at(*stage);
                if (window->visibility() != step.expected) {
                    if (++*retries <= 100) {
                        timer->start(10);
                        return;
                    }
                    qCritical()
                        << "Window-state action did not reach visibility"
                        << step.expected << "at stage" << *stage
                        << "actual" << window->visibility();
                    QCoreApplication::exit(1);
                    return;
                }

                *retries = 0;
                if (step.command == Command::Finish) {
                    QCoreApplication::quit();
                    return;
                }

                if (step.command == Command::Minimize) {
                    window->setVisibility(QWindow::Minimized);
                } else if (step.command == Command::RestoreWindowed) {
                    window->setVisibility(QWindow::Windowed);
                } else {
                    const QString action =
                        step.command == Command::ToggleMaximize
                        ? QStringLiteral("toggle_maximize")
                        : QStringLiteral("toggle_fullscreen");
                    if (!workspace->executeSurfaceActionOnAllPanes(action)) {
                        qCritical()
                            << "Window-state test hook could not execute"
                            << action << "at stage" << *stage;
                        QCoreApplication::exit(1);
                        return;
                    }
                }
                if (*stage + 1 >= steps.size()) {
                    qCritical()
                        << "Window-state test hook exhausted its steps";
                    QCoreApplication::exit(1);
                    return;
                }
                ++*stage;
                timer->start(0);
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

    const auto applyMode = [workspace](WindowShowTabBar mode) {
        LaunchOptions options = workspace->effectiveLaunchOptions();
        options.confirmCloseMode = ConfirmCloseMode::Never;
        options.windowShowTabBar = mode;
        workspace->applyLaunchOptions(options);
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
                    applyMode(WindowShowTabBar::Auto);
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
                    applyMode(WindowShowTabBar::Always);
                    break;
                case 3:
                    if (!verifyTabBarTestState(
                            workspace, tabBar, 1, true, "always with one tab")) {
                        return;
                    }
                    applyMode(WindowShowTabBar::Never);
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
                    applyMode(WindowShowTabBar::Auto);
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
    const std::span<char *const> rawArguments(
        argv, static_cast<std::size_t>(argc));
    const GhosttyCliActionSelection cliAction =
        selectGhosttyCliAction(rawArguments);
    switch (cliAction.disposition) {
    case GhosttyCliActionDisposition::None:
        break;
    case GhosttyCliActionDisposition::Unsupported:
        std::fprintf(
            stderr,
            "ghostty-qt: unsupported Ghostty CLI action '%.*s'\n",
            static_cast<int>(cliAction.argument.size()),
            cliAction.argument.data());
        return 2;
    case GhosttyCliActionDisposition::Multiple:
        std::fprintf(
            stderr,
            "ghostty-qt: multiple Ghostty CLI actions are not allowed "
            "(second action: '%.*s')\n",
            static_cast<int>(cliAction.argument.size()),
            cliAction.argument.data());
        return 2;
    case GhosttyCliActionDisposition::Delegate:
#if GHOSTTY_QT_CONFIG_ENABLED
        {
            const GhosttyCliExecError failure = execGhosttyCliHelper(
                rawArguments,
                GHOSTTY_QT_CONFIG_HELPER_NAME);
            const std::string target = failure.target.native();
            const std::string cause = failure.cause.message();
            std::fprintf(stderr,
                         "ghostty-qt: could not execute CLI helper '%s': %s\n",
                         target.c_str(), cause.c_str());
            return failure.exitCode();
        }
#else
        std::fprintf(
            stderr,
            "ghostty-qt: Ghostty CLI action '%.*s' is unavailable because "
            "this build disabled GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG\n",
            static_cast<int>(cliAction.argument.size()),
            cliAction.argument.data());
        return 1;
#endif
    }

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

    // Launcher presentation data is a one-shot capability. Capture and clear
    // it before Qt, config helpers, or terminal workers can start threads or
    // child processes; the typed value is projected only while its target
    // window is shown.
    DesktopActivationContext startupActivation =
        DesktopActivationContext::takeFromEnvironment();

    QGuiApplication application(argc, argv);
    // Ghostty owns last-window process lifetime, including disabled and
    // delayed modes. Qt's implicit auto-quit would bypass that policy.
    application.setQuitOnLastWindowClosed(false);
    QCoreApplication::setApplicationName(QStringLiteral("ghostty-qt"));
    QCoreApplication::setApplicationVersion(QStringLiteral(GHOSTTY_QT_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("ghostty-qt"));
    QGuiApplication::setDesktopFileName(
        QStringLiteral(GHOSTTY_QT_APPLICATION_ID));

    const bool allowNonWayland = qEnvironmentVariableIntValue(
        "GHOSTTY_QT_ALLOW_NON_WAYLAND") == 1;
    if (QGuiApplication::platformName() != QStringLiteral("wayland") && !allowNonWayland) {
        QTextStream(stderr)
            << "ghostty-qt supports the Qt Wayland platform only (active platform: "
            << QGuiApplication::platformName() << ").\n";
        return 2;
    }

#if GHOSTTY_QT_CONFIG_ENABLED
    const QString configHelperPath = QDir(QCoreApplication::applicationDirPath())
                                         .filePath(QStringLiteral(
                                             GHOSTTY_QT_CONFIG_HELPER_NAME));
    GhosttyConfigService configService(makeGhosttyConfigProcessLoader({
        .helperPath = configHelperPath,
        .configurationArguments =
            ghosttyConfigCliFontArguments(options),
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
            effectiveApplicationOptions,
            QByteArrayView(qgetenv("TERM_PROGRAM")))) {
        auto candidate = std::make_unique<SingleInstanceActivation>();
        const SingleInstanceActivation::StartResult activation =
            candidate->start({
                .existingInstanceAction = effectiveApplicationOptions.initialWindow
                    ? SingleInstanceActivation::ExistingInstanceAction::Activate
                    : SingleInstanceActivation::ExistingInstanceAction::DoNotActivate,
                .activation = startupActivation,
            });
        switch (activation.role) {
        case SingleInstanceActivation::Role::ActivatedExisting:
        case SingleInstanceActivation::Role::ExistingInstance:
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
    QObject::connect(
        &applicationController, &ApplicationController::configOpenRequested,
        &application, [] {
            const auto opened = openGhosttyConfigForEditing(
                GhosttyConfigService::standardConfigEditPaths());
            if (!opened.has_value()) {
                qWarning().noquote()
                    << "Could not open the Ghostty configuration:"
                    << opened.error();
            }
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

    std::optional<ApplicationWindow> initialWindow;
    if (effectiveApplicationOptions.initialWindow) {
        const std::expected<ApplicationWindow, QString> created =
            applicationController.createInitialWindow(
                std::move(startupActivation));
        if (!created.has_value()) {
            qCritical().noquote()
                << "Could not create the primary application window:"
                << created.error();
            return 1;
        }
        initialWindow = *created;
    } else if (!applicationController.startWithoutInitialWindow()) {
        qCritical() << "Could not start without an initial application window";
        return 1;
    }
    QQuickWindow *const applicationWindow = initialWindow
        ? initialWindow->window
        : nullptr;
    TerminalWorkspace *const workspace = initialWindow
        ? initialWindow->workspace
        : nullptr;

    const ApplicationLifetimeTestMode lifetimeTestMode =
        applicationLifetimeTestMode();
    bool lifetimeTestCompleted = false;
    if ((lifetimeTestMode != ApplicationLifetimeTestMode::None
         && !initialWindow)
        || !installApplicationLifetimeTestHook(
            applicationWindow, workspace, &applicationController,
            effectiveApplicationOptions, lifetimeTestMode,
            &lifetimeTestCompleted)) {
        return 1;
    }

    const bool suppressedStartupTest = qEnvironmentVariableIntValue(
        "GHOSTTY_QT_TEST_INITIAL_WINDOW") == 1;
    bool suppressedStartupTestCompleted = false;
    if (suppressedStartupTest
        && (initialWindow
            || !installSuppressedStartupTestHook(
                &applicationController, effectiveApplicationOptions,
                &suppressedStartupTestCompleted))) {
        return 1;
    }

    const bool desktopActivationTest
        = qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_DESKTOP_ACTIVATION")
        == 1;
    bool desktopActivationTestCompleted = false;
    if (desktopActivationTest
        && (initialWindow
            || !installDesktopActivationTestHook(
                &applicationController, &desktopActivationTestCompleted))) {
        return 1;
    }

    // Headless regression hook: exercise the real QML confirmation dialog and
    // complete shutdown without synthesizing compositor input. It is inert in
    // every normal launch.
    if (qEnvironmentVariableIntValue(
            "GHOSTTY_QT_TEST_CONFIRM_CLOSE_DIALOG") == 1) {
        if (!initialWindow
            || !installCloseDialogTestHook(applicationWindow, workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue(
            "GHOSTTY_QT_TEST_TAB_TITLE_PROMPT") == 1) {
        if (!initialWindow || !installTitlePromptTestHook(
                applicationWindow, workspace, TitlePromptTestTarget::Tab)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue(
            "GHOSTTY_QT_TEST_SURFACE_TITLE_PROMPT") == 1) {
        if (!initialWindow || !installTitlePromptTestHook(
                applicationWindow, workspace, TitlePromptTestTarget::Surface)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue(
            "GHOSTTY_QT_TEST_TOGGLE_FULLSCREEN") == 1) {
        if (!initialWindow
            || !installFullscreenActionTestHook(applicationWindow, workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue(
            "GHOSTTY_QT_TEST_INITIAL_WINDOW_STATE") == 1) {
        if (!initialWindow
            || !installInitialWindowStateTestHook(
                applicationWindow, effectiveApplicationOptions)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue(
            "GHOSTTY_QT_TEST_INITIAL_WINDOW_SIZE") == 1) {
        if (!initialWindow
            || !installInitialWindowSizeTestHook(
                applicationWindow, workspace, effectiveApplicationOptions)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue(
            "GHOSTTY_QT_TEST_TOGGLE_MAXIMIZE") == 1) {
        if (!initialWindow
            || !installMaximizeActionTestHook(applicationWindow, workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue(
            "GHOSTTY_QT_TEST_TAB_BAR_VISIBILITY") == 1) {
        if (!initialWindow
            || !installTabBarVisibilityTestHook(applicationWindow, workspace)) {
            return 1;
        }
    }

    // Install activation last. A cold D-Bus call can arrive as soon as the
    // well-known name is claimed; keeping its delayed reply queued until here
    // guarantees that every controller, reload, and test hook is ready before
    // the corresponding window is registered.
    if (activationEndpoint) {
        activationEndpoint->setActivationHandler(
            [controller = QPointer(&applicationController)](
                DesktopActivationContext activation) {
                return controller != nullptr
                    && controller->activateNoCommand(std::move(activation));
            });
    }
    const auto activationHandlerGuard = qScopeGuard([&activationEndpoint] {
        if (activationEndpoint) {
            activationEndpoint->setActivationHandler({});
        }
    });

    const int exitCode = application.exec();
    if (lifetimeTestMode != ApplicationLifetimeTestMode::None
        && !lifetimeTestCompleted) {
        qCritical() << "Application-lifetime test hook did not complete";
        return 1;
    }
    if (suppressedStartupTest && !suppressedStartupTestCompleted) {
        qCritical() << "Suppressed-startup test hook did not complete";
        return 1;
    }
    if (desktopActivationTest && !desktopActivationTestCompleted) {
        qCritical() << "Desktop-activation test hook did not complete";
        return 1;
    }
    return exitCode;
}
