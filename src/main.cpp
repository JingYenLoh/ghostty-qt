#include "application_appearance.h"
#include "application_controller.h"
#include "application_identity.h"
#include "desktop_activation.h"
#include "desktop_quick_controls_style.h"
#include "frontend_config_service.h"
#include "ghostty_application_ipc.h"
#include "ghostty_cli_delegation.h"
#include "ghostty_config_edit.h"
#include "ghostty_config_service.h"
#include "keyboard_layout.h"
#include "launch_options.h"
#include "renderer_graphics_library_manifest.h"
#include "single_instance_activation.h"
#include "systemd_notify.h"
#include "terminal_cell_metrics.h"
#include "terminal_geometry.h"
#include "terminal_pane.h"
#include "terminal_workspace.h"

#if GHOSTTY_QT_CONFIG_ENABLED
#include "ghostty_config_process_loader.h"
#endif

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMetaEnum>
#include <QMetaProperty>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScopeGuard>
#include <QScreen>
#include <QStandardPaths>
#include <QStyleHints>
#include <QSurfaceFormat>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QVector>
#include <rhi/qrhi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace {

QByteArray shellQuote(QByteArrayView value)
{
    QByteArray result("'");
    result.reserve(value.size() + 2);
    for (const char character : value) {
        if (character == '\'') {
            result.append(QByteArrayLiteral("'\\''"));
        } else {
            result.append(character);
        }
    }
    result.append('\'');
    return result;
}

#if GHOSTTY_QT_CONFIG_ENABLED
std::expected<QString, QString> siblingExecutablePath(QStringView filename)
{
    const QString executable =
        QFile::symLinkTarget(QStringLiteral("/proc/self/exe"));
    if (executable.isEmpty()) {
        return std::unexpected(QStringLiteral(
            "Could not resolve the executable through /proc/self/exe"));
    }
    const QFileInfo executableInfo(executable);
    if (!executableInfo.isAbsolute()) {
        return std::unexpected(QStringLiteral(
            "/proc/self/exe did not resolve to an absolute executable path"));
    }
    return executableInfo.dir().filePath(filename.toString());
}
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 11, 0)
bool hasDiscoverableDesktopEntry(QStringView applicationId)
{
    return !QStandardPaths::locate(QStandardPaths::ApplicationsLocation,
                                   applicationId.toString()
                                       + QStringLiteral(".desktop"),
                                   QStandardPaths::LocateFile)
                .isEmpty();
}
#endif

void printHelp()
{
    QTextStream output(stdout);
    output
        << "Usage: ghostty-qt [options] [-e program [arguments...]]\n"
           "       ghostty-qt [options] [-- program [arguments...]]\n\n"
           "Linux Wayland terminal emulator powered by libghostty-vt.\n\n"
           "Options:\n"
           "  -h, --help                    Show this help.\n"
           "  -v, --version                 Show version information.\n"
           "      --working-directory DIR   Start the command in DIR.\n"
           "      --title=TITLE             Set the initial terminal title.\n"
           "      --font-family FAMILY      Use FAMILY for terminal text.\n"
           "      --font-size POINTS        Set font size (default: 12).\n"
           "      --class ID                Set the application identity.\n"
           "      --config-default-files BOOLEAN\n"
           "                                  Load standard Ghostty config "
           "files.\n"
           "      --scrollback-lines LINES  Estimate capacity for LINES "
           "(default: 10000).\n"
           "      --hold                    Keep the pane after the command "
           "exits.\n"
           "      --wait-after-command      Wait for a key press after the "
           "command exits.\n"
           "  -e PROGRAM [ARGUMENTS...]     Run a command; all remaining "
           "arguments belong to it.\n"
           "  -- PROGRAM [ARGUMENTS...]     Run a command using the "
           "positional boundary.\n"
           "      --single-instance MODE      Use false, true, or detect "
           "uniqueness.\n"
           "      --initial-window BOOLEAN    Request an initial window.\n";
    output << "\nPinned Ghostty CLI actions:\n";
    for (const GhosttyCliActionCatalogEntry &entry : GhosttyPinnedCliActions) {
        const bool available = entry.isApplicationIpc()
#if GHOSTTY_QT_CONFIG_ENABLED
            || entry.isDelegated()
#endif
            ;
        if (!available) continue;
        const std::string_view action = entry.argument;
        output << "  "
               << QString::fromLatin1(action.data(),
                                      static_cast<qsizetype>(action.size()))
               << '\n';
    }
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
        workspace, &TerminalWorkspace::closeConfirmationRequested, closeDialog,
        [closeDialog, acceptanceInvoked](quint64 confirmationId,
                                         const QString &) {
            QTimer::singleShot(
                0, closeDialog,
                [closeDialog, acceptanceInvoked, confirmationId] {
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
        workspace, &TerminalWorkspace::windowCloseApproved, closeDialog,
        [acceptanceInvoked] {
            if (!*acceptanceInvoked) {
                qCritical()
                    << "Close-dialog test hook quit without QML acceptance";
                QCoreApplication::exit(1);
            }
        });

    const auto requestSurfaceClose = [workspace] {
        TerminalPane *const pane = workspace->findChild<TerminalPane *>();
        if (pane == nullptr
            || !pane->executeConfiguredAction(QStringLiteral("toggle_readonly"))
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
        QObject::connect(workspace, &TerminalWorkspace::tabTitlesChanged,
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

bool installApplicationLifetimeTestHook(QWindow *applicationWindow,
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
            controller,
            [](const QString &message) {
                qCritical().noquote()
                    << "Resident lifetime test could not recreate a window:"
                    << message;
                QCoreApplication::exit(1);
            },
            Qt::SingleShotConnection);
        QObject::connect(
            controller, &ApplicationController::windowRetired, controller,
            [controller, lifetime, completed, state, mode] {
                QTimer::singleShot(
                    50, controller,
                    [controller, lifetime, completed, state, mode] {
                        if (lifetime->hasOpenWindow() || lifetime->quitPending()
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
                                        == ApplicationLifetimeTestMode::
                                            ExternalActivation) {
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
                                        QTimer::singleShot(0,
                                                           replacementWorkspace,
                                                           closeReplacement);
                                    } else {
                                        QObject::connect(
                                            replacementWorkspace,
                                            &TerminalWorkspace::
                                                tabTitlesChanged,
                                            replacementWorkspace,
                                            closeReplacement,
                                            Qt::SingleShotConnection);
                                    }
                                },
                                Qt::SingleShotConnection);
                            if (mode
                                == ApplicationLifetimeTestMode::
                                    ExternalActivation) {
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
                        (void)controller->dispatch(ApplicationAction::Quit);
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
            if (!controller->dispatch(ApplicationAction::Quit, workspace)) {
                qCritical()
                    << "Could not execute the explicit quit test action";
                QCoreApplication::exit(1);
            }
            return;
        }
        workspace->requestWindowClose();
    };
    if (workspace->tabCount() > 0) {
        QTimer::singleShot(0, workspace, request);
    } else {
        QObject::connect(workspace, &TerminalWorkspace::tabTitlesChanged,
                         workspace, request, Qt::SingleShotConnection);
    }
    return true;
}

bool installSuppressedStartupTestHook(ApplicationController *controller,
                                      const LaunchOptions &options,
                                      bool *completed)
{
    ApplicationLifetimeController *const lifetime =
        controller->lifetimeController();
    if (options.initialWindow || controller->windowCount() != 0
        || lifetime->registeredWindowCount() != 0 || lifetime->hasOpenWindow()
        || lifetime->quitPending() || lifetime->hasRequestedQuit()) {
        qCritical()
            << "Suppressed-startup test hook did not begin with an idle zero-window application";
        return false;
    }

    QObject::connect(
        controller, &ApplicationController::windowCreationFailed, controller,
        [](const QString &message) {
            qCritical().noquote()
                << "Suppressed-startup test could not create its first window:"
                << message;
            QCoreApplication::exit(1);
        },
        Qt::SingleShotConnection);
    QObject::connect(
        controller, &ApplicationController::windowCreated, controller,
        [controller, lifetime, options,
         completed](QQuickWindow *, TerminalWorkspace *workspace) {
            const LaunchOptions &actual = workspace->effectiveLaunchOptions();
            if (controller->windowCount() != 1 || !lifetime->hasOpenWindow()
                || actual.program != options.program
                || actual.hold != options.hold) {
                qCritical()
                    << "Suppressed-startup test did not preserve first-surface options";
                QCoreApplication::exit(1);
                return;
            }

            QTextStream(stdout) << "GHOSTTY_QT_INITIAL_WINDOW_CREATED\n"
                                << Qt::flush;
            QObject::connect(
                controller, &ApplicationController::windowRetired, controller,
                [controller, lifetime, completed] {
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
                    workspace, &TerminalWorkspace::tabTitlesChanged, workspace,
                    closeFirstWindow, Qt::SingleShotConnection);
            }
        },
        Qt::SingleShotConnection);

    QTextStream(stdout) << "GHOSTTY_QT_INITIAL_WINDOW_READY\n" << Qt::flush;
    return true;
}

bool installDesktopActivationTestHook(ApplicationController *controller,
                                      bool *completed)
{
    ApplicationLifetimeController *const lifetime =
        controller->lifetimeController();
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

    QTextStream(stdout) << "GHOSTTY_QT_DESKTOP_ACTIVATION_READY\n" << Qt::flush;
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
    const auto focusedPane = std::make_shared<QPointer<TerminalPane>>(nullptr);
    QObject::connect(
        workspace, &TerminalWorkspace::titlePromptRequested, dialog,
        [dialog, field, seed, replacement, heading, activePromptId,
         acceptanceInvoked](quint64 promptId, const QString &actualHeading,
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
                0, dialog, [dialog, field, replacement, acceptanceInvoked] {
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
        workspace, &TerminalWorkspace::titlePromptResolved, dialog,
        [workspace, dialog, replacement, target, activePromptId,
         acceptanceInvoked, focusedPane](quint64 promptId) {
            if (!*acceptanceInvoked || promptId != *activePromptId) {
                qCritical()
                    << "Title-prompt test hook resolved the wrong request";
                QCoreApplication::exit(1);
                return;
            }
            *activePromptId = 0;
            auto *const timer = new QTimer(dialog);
            timer->setSingleShot(true);
            const auto attempts = std::make_shared<int>(0);
            QObject::connect(
                timer, &QTimer::timeout, dialog,
                [workspace, dialog, replacement, target, focusedPane, timer,
                 attempts] {
                    const TabListEntry *entry = workspace->tabModel()->entryAt(
                        workspace->currentIndex());
                    TerminalPane *const pane = focusedPane->data();
                    const bool settled = entry != nullptr && pane != nullptr
                        && pane->hasActiveFocus()
                        && workspace->currentTitle() == replacement
                        && !dialog->property("visible").toBool();
                    if (!settled) {
                        if (++*attempts < 50) {
                            timer->start(10);
                            return;
                        }
                        qCritical()
                            << "Title-prompt test hook did not settle after closing";
                        QCoreApplication::exit(1);
                        return;
                    }

                    bool committed = true;
                    if (target == TitlePromptTestTarget::Surface) {
                        committed = pane->surfaceTitleOverride()
                                == std::optional<QString>{replacement}
                            && workspace->executeSurfaceActionOnAllPanes(
                                QStringLiteral(
                                    "set_surface_title:changed base"))
                            && workspace->currentTitle() == replacement;
                    } else {
                        committed = entry->titleOverride == replacement;
                    }
                    if (!committed) {
                        qCritical()
                            << "Title-prompt test hook did not commit through QML";
                        QCoreApplication::exit(1);
                        return;
                    }
                    QCoreApplication::quit();
                });
            timer->start(0);
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
        QTimer::singleShot(
            0, workspace,
            [workspace, seed, setAction, promptAction, focusedPane] {
                if (focusedPane->isNull()
                    || !focusedPane->data()->hasActiveFocus()
                    || !workspace->executeSurfaceActionOnAllPanes(setAction)
                    || workspace->currentTitle() != seed
                    || !workspace->executeSurfaceActionOnAllPanes(
                        promptAction)) {
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
            workspace, &TerminalWorkspace::tabTitlesChanged, workspace,
            [workspace, exercise] {
                QTimer::singleShot(0, workspace, exercise);
            },
            Qt::SingleShotConnection);
    }
    return true;
}

bool installContextMenuPositionTestHook(QQuickWindow *window,
                                        TerminalWorkspace *workspace)
{
    QObject *const menu =
        window->findChild<QObject *>(QStringLiteral("terminalContextMenu"));
    QQuickItem *const windowContentRoot = window->contentItem();
    auto *const applicationContentItem =
        window->property("contentItem").value<QQuickItem *>();
    if (menu == nullptr || windowContentRoot == nullptr
        || applicationContentItem == nullptr) {
        qCritical()
            << "Context-menu-position test hook could not find the QML controls";
        return false;
    }

    const int popupTypeIndex = menu->metaObject()->indexOfProperty("popupType");
    if (popupTypeIndex < 0) {
        qCritical()
            << "Context-menu-position test hook could not inspect the popup type";
        return false;
    }
    const QMetaProperty popupTypeProperty =
        menu->metaObject()->property(popupTypeIndex);
    const QMetaEnum popupTypes = popupTypeProperty.enumerator();
    const int itemPopupType = popupTypes.keyToValue("Item");
    if (!popupTypes.isValid() || itemPopupType < 0
        || menu->property("popupType").toInt() != itemPopupType) {
        qCritical() << "Context-menu-position test hook requires an item popup";
        return false;
    }

    const QPointF rootPosition(260.0, 240.0);
    const auto exercise = [window, workspace, menu, windowContentRoot,
                           applicationContentItem, rootPosition] {
        TerminalPane *const pane = workspace->findChild<TerminalPane *>();
        if (pane == nullptr) {
            qCritical()
                << "Context-menu-position test hook could not find a terminal pane";
            QCoreApplication::exit(1);
            return;
        }

        auto *const timer = new QTimer(workspace);
        timer->setSingleShot(true);
        const auto attempts = std::make_shared<int>(0);
        QObject::connect(
            timer, &QTimer::timeout, workspace,
            [window, menu, windowContentRoot, applicationContentItem,
             rootPosition, timer, attempts] {
                if (!menu->property("visible").toBool()) {
                    if (++*attempts < 50) {
                        timer->start(10);
                        return;
                    }
                    qCritical()
                        << "Context-menu-position test hook did not observe an open menu";
                    QCoreApplication::exit(1);
                    return;
                }

                auto *const actualParent =
                    menu->property("parent").value<QQuickItem *>();
                const QPointF contentOrigin = applicationContentItem->mapToItem(
                    windowContentRoot, QPointF());
                const QPointF expected = actualParent != nullptr
                    ? actualParent->mapFromItem(windowContentRoot, rootPosition)
                    : QPointF();
                const QPointF actual(menu->property("x").toReal(),
                                     menu->property("y").toReal());
                constexpr qreal tolerance = 0.5;
                const bool valid = actualParent == applicationContentItem
                    && actualParent->window() == window
                    && contentOrigin.y() > tolerance
                    && std::abs(expected.x() - actual.x()) < tolerance
                    && std::abs(expected.y() - actual.y()) < tolerance
                    && std::abs(rootPosition.y() - actual.y()) > tolerance;
                if (!valid) {
                    qCritical().nospace()
                        << "Context-menu-position test hook mismatch: root="
                        << rootPosition << ", content-origin=" << contentOrigin
                        << ", expected=" << expected << ", actual=" << actual
                        << ", content-parent="
                        << (actualParent == applicationContentItem);
                    QCoreApplication::exit(1);
                    return;
                }

                if (!QMetaObject::invokeMethod(menu, "close")) {
                    qCritical()
                        << "Context-menu-position test hook could not close the menu";
                    QCoreApplication::exit(1);
                    return;
                }
                QTimer::singleShot(0, QCoreApplication::instance(),
                                   [] { QCoreApplication::quit(); });
            });

        Q_EMIT pane->contextMenuRequested(rootPosition, false);
        timer->start(0);
    };
    if (workspace->tabCount() > 0) {
        QTimer::singleShot(0, workspace, exercise);
    } else {
        QObject::connect(
            workspace, &TerminalWorkspace::tabTitlesChanged, workspace,
            [workspace, exercise] {
                QTimer::singleShot(0, workspace, exercise);
            },
            Qt::SingleShotConnection);
    }
    return true;
}

bool installContextMenuActionTestHook(QQuickWindow *window,
                                      TerminalWorkspace *workspace,
                                      ApplicationController *controller)
{
    QObject *const menu =
        window->findChild<QObject *>(QStringLiteral("terminalContextMenu"));
    if (menu == nullptr || controller == nullptr) {
        qCritical()
            << "Context-menu-action test hook could not find the QML controls";
        return false;
    }

    struct TestState {
        bool actionTriggered = false;
        bool triggerReturned = false;
        bool reloadDispatched = false;
    };
    const auto state = std::make_shared<TestState>();
    QObject::connect(
        controller, &ApplicationController::configReloadRequested, workspace,
        [menu, state] {
            state->reloadDispatched = true;
            if (!state->actionTriggered || !state->triggerReturned
                || menu->property("visible").toBool()
                || menu->property("requestId").toULongLong() != 0
                || !menu->property("pendingAction").toString().isEmpty()) {
                qCritical()
                    << "Context-menu-action test hook dispatched before the QML menu closed";
                QCoreApplication::exit(1);
                return;
            }
            QTimer::singleShot(0, QCoreApplication::instance(),
                               [] { QCoreApplication::quit(); });
        });

    const QPointF rootPosition(260.0, 240.0);
    const auto exercise = [window, workspace, menu, state, rootPosition] {
        TerminalPane *const pane = workspace->findChild<TerminalPane *>();
        if (pane == nullptr) {
            qCritical()
                << "Context-menu-action test hook could not find a terminal pane";
            QCoreApplication::exit(1);
            return;
        }

        auto *const timer = new QTimer(workspace);
        timer->setSingleShot(true);
        const auto attempts = std::make_shared<int>(0);
        QObject::connect(
            timer, &QTimer::timeout, workspace,
            [window, menu, state, timer, attempts] {
                if (!state->actionTriggered) {
                    if (!menu->property("visible").toBool()) {
                        if (++*attempts < 50) {
                            timer->start(10);
                            return;
                        }
                        qCritical()
                            << "Context-menu-action test hook did not observe an open menu";
                        QCoreApplication::exit(1);
                        return;
                    }

                    QObject *const reloadAction = window->findChild<QObject *>(
                        QStringLiteral("terminalContextMenuReloadConfig"));
                    state->actionTriggered = true;
                    const bool invoked = reloadAction != nullptr
                        && QMetaObject::invokeMethod(reloadAction, "trigger",
                                                     Q_ARG(QObject *, nullptr));
                    if (!invoked || state->reloadDispatched) {
                        qCritical()
                            << "Context-menu-action test hook could not trigger the QML action";
                        QCoreApplication::exit(1);
                        return;
                    }

                    // Popup exit transitions may keep the item visible after
                    // trigger() returns. QML must defer workspace dispatch
                    // until onClosed has cleared the request state.
                    state->triggerReturned = true;
                    return;
                }
            });

        QTimer::singleShot(2000, workspace, [state] {
            if (!state->reloadDispatched) {
                qCritical()
                    << "Context-menu-action test hook did not observe the deferred action";
                QCoreApplication::exit(1);
            }
        });
        Q_EMIT pane->contextMenuRequested(rootPosition, false);
        timer->start(0);
    };
    if (workspace->tabCount() > 0) {
        QTimer::singleShot(0, workspace, exercise);
    } else {
        QObject::connect(
            workspace, &TerminalWorkspace::tabTitlesChanged, workspace,
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
        QTimer::singleShot(
            0, workspace, [workspace, window, originalVisibility] {
                if (window->visibility() != QWindow::FullScreen) {
                    qCritical() << "QML window did not enter fullscreen";
                    QCoreApplication::exit(1);
                    return;
                }
                if (!workspace->executeSurfaceActionOnAllPanes(
                        QStringLiteral("toggle_fullscreen"))) {
                    qCritical()
                        << "Fullscreen test hook could not leave fullscreen";
                    QCoreApplication::exit(1);
                    return;
                }
                QTimer::singleShot(0, workspace, [window, originalVisibility] {
                    if (window->visibility() != originalVisibility) {
                        qCritical()
                            << "QML window did not restore its visibility";
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
        QObject::connect(
            workspace, &TerminalWorkspace::tabTitlesChanged, workspace,
            [workspace, exercise] {
                // The model notification precedes activateTab(); defer until
                // the new tab's active pane and geometry have been installed.
                QTimer::singleShot(0, workspace, exercise);
            },
            Qt::SingleShotConnection);
    }
    return true;
}

bool installWindowDecorationActionTestHook(QQuickWindow *window,
                                           TerminalWorkspace *workspace)
{
    if (window == nullptr || workspace == nullptr) {
        qCritical()
            << "Window-decoration test hook could not find the QML window";
        return false;
    }

    const auto exercise = [workspace, window] {
        workspace->splitRight();
        const QList<TerminalPane *> panes =
            workspace->findChildren<TerminalPane *>();
        if (panes.size() != 2
            || window->flags().testFlag(Qt::FramelessWindowHint)) {
            qCritical()
                << "Window-decoration test hook could not establish its initial state";
            QCoreApplication::exit(1);
            return;
        }

        window->setFlag(Qt::WindowDoesNotAcceptFocus, true);
        const QSize size = window->size();
        const QWindow::Visibility visibility = window->visibility();
        const Qt::WindowStates states = window->windowStates();
        const QVariant fullscreenRestore =
            window->property("visibilityBeforeFullscreen");
        if (!workspace->executeSurfaceActionOnAllPanes(
                QStringLiteral("toggle_window_decorations"))) {
            qCritical()
                << "Window-decoration test hook could not remove decorations";
            QCoreApplication::exit(1);
            return;
        }

        QTimer::singleShot(
            0, workspace,
            [workspace, window, panes, size, visibility, states,
             fullscreenRestore] {
                if (!window->flags().testFlag(Qt::FramelessWindowHint)
                    || !window->flags().testFlag(Qt::WindowDoesNotAcceptFocus)
                    || window->size() != size
                    || window->visibility() != visibility
                    || window->windowStates() != states
                    || window->property("visibilityBeforeFullscreen")
                        != fullscreenRestore
                    || workspace->findChildren<TerminalPane *>() != panes) {
                    qCritical()
                        << "QML window did not preserve its host and terminal state while removing decorations";
                    QCoreApplication::exit(1);
                    return;
                }
                if (!workspace->executeSurfaceActionOnAllPanes(
                        QStringLiteral("toggle_window_decorations"))) {
                    qCritical()
                        << "Window-decoration test hook could not restore decorations";
                    QCoreApplication::exit(1);
                    return;
                }
                QTimer::singleShot(0, workspace, [window] {
                    if (window->flags().testFlag(Qt::FramelessWindowHint)
                        || !window->flags().testFlag(
                            Qt::WindowDoesNotAcceptFocus)) {
                        qCritical()
                            << "QML window did not restore configured decorations";
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
        QObject::connect(
            workspace, &TerminalWorkspace::tabTitlesChanged, workspace,
            [workspace, exercise] {
                QTimer::singleShot(0, workspace, exercise);
            },
            Qt::SingleShotConnection);
    }
    return true;
}

bool installInitialWindowStateTestHook(QQuickWindow *window,
                                       const LaunchOptions &options)
{
    if (window == nullptr || options.windowWidth == 0
        || options.windowHeight == 0) {
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
                QWindow::FullScreen, QWindow::Maximized, QWindow::FullScreen,
                QWindow::Maximized,  QWindow::Windowed,
            };
            const QWindow::Visibility expected =
                expectedStates.at(static_cast<std::size_t>(*stage));
            const TerminalCellMetrics metrics = terminalCellMetrics(
                options.typography, window->devicePixelRatio());
            const QMarginsF padding = terminalExplicitPaddingMargins(
                options.padding, window->devicePixelRatio());
            const QSize configuredSize(
                qCeil(metrics.cellWidth
                          * static_cast<qreal>(options.windowWidth)
                      + padding.left() + padding.right()
                      + window->property("terminalChromeWidth").toDouble()),
                qCeil(metrics.cellHeight
                          * static_cast<qreal>(options.windowHeight)
                      + padding.top() + padding.bottom()
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
                if (!QMetaObject::invokeMethod(window, "toggleFullscreen",
                                               Qt::DirectConnection)) {
                    qCritical()
                        << "Initial-window-state test hook could not invoke the QML fullscreen toggle";
                    QCoreApplication::exit(1);
                    return;
                }
            } else {
                if (!QMetaObject::invokeMethod(window, "toggleMaximize",
                                               Qt::DirectConnection)) {
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
    if (window == nullptr || workspace == nullptr || options.windowWidth == 0
        || options.windowHeight == 0) {
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
            const QMarginsF padding = terminalExplicitPaddingMargins(
                options.padding, window->devicePixelRatio());
            const QSize configuredSize(
                qCeil(metrics.cellWidth
                          * static_cast<qreal>(options.windowWidth)
                      + padding.left() + padding.right() + chromeWidth),
                qCeil(metrics.cellHeight
                          * static_cast<qreal>(options.windowHeight)
                      + padding.top() + padding.bottom() + chromeHeight));
            const QSize minimumSize(
                qCeil(metrics.cellWidth * 10.0 + padding.left()
                      + padding.right() + chromeWidth),
                qCeil(metrics.cellHeight * 4.0 + padding.top()
                      + padding.bottom() + chromeHeight));
            const auto paneLayout = pane != nullptr
                ? terminalViewportLayout({
                      .surfaceSize = pane->size(),
                      .cellSize = QSizeF(metrics.cellWidth, metrics.cellHeight),
                      .devicePixelRatio = window->devicePixelRatio(),
                      .padding = options.padding,
                  })
                : std::nullopt;
            const bool shouldBeWindowed =
                *stage == 0 || *stage == 2 || *stage == 4;
            const bool windowReady = pane != nullptr
                && (!shouldBeWindowed
                    || (window->visibility() == QWindow::Windowed
                        && window->size() == configuredSize
                        && window->minimumSize() == minimumSize && paneLayout
                        && static_cast<quint32>(paneLayout->session.columns)
                            == options.windowWidth
                        && static_cast<quint32>(paneLayout->session.rows)
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
                    << "visibility" << window->visibility() << "window"
                    << window->size() << "minimum" << window->minimumSize()
                    << "pane" << (pane != nullptr ? pane->size() : QSizeF())
                    << "expected window" << configuredSize << "expected minimum"
                    << minimumSize;
                QCoreApplication::exit(1);
                return;
            }

            *retries = 0;
            switch ((*stage)++) {
            case 0:
            case 1:
                if (!QMetaObject::invokeMethod(window, "toggleMaximize",
                                               Qt::DirectConnection)) {
                    qCritical()
                        << "Initial-window-size test hook could not toggle maximize";
                    QCoreApplication::exit(1);
                    return;
                }
                break;
            case 2:
            case 3:
                if (!QMetaObject::invokeMethod(window, "toggleFullscreen",
                                               Qt::DirectConnection)) {
                    qCritical()
                        << "Initial-window-size test hook could not toggle fullscreen";
                    QCoreApplication::exit(1);
                    return;
                }
                break;
            case 4: QCoreApplication::quit(); return;
            default: Q_UNREACHABLE();
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
                        << step.expected << "at stage" << *stage << "actual"
                        << window->visibility();
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
                    qCritical() << "Window-state test hook exhausted its steps";
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
        QObject::connect(
            workspace, &TerminalWorkspace::tabTitlesChanged, workspace,
            [workspace, exercise] {
                QTimer::singleShot(0, workspace, exercise);
            },
            Qt::SingleShotConnection);
    }
    return true;
}

bool verifyTabBarTestState(TerminalWorkspace *workspace, QObject *tabBar,
                           int expectedCount, bool expectedVisible,
                           const char *stage)
{
    const bool qmlVisible = tabBar->property("visible").toBool();
    if (workspace->tabCount() == expectedCount
        && workspace->tabBarVisible() == expectedVisible
        && qmlVisible == expectedVisible) {
        return true;
    }

    qCritical().nospace() << "Tab-bar test hook mismatch at " << stage
                          << ": count=" << workspace->tabCount()
                          << ", workspace-visible="
                          << workspace->tabBarVisible()
                          << ", qml-visible=" << qmlVisible;
    QCoreApplication::exit(1);
    return false;
}

bool verifyTabButtonWidths(QObject *tabBar, bool wide, const char *stage)
{
    QList<QQuickItem *> buttons;
    const auto collectButtons = [&buttons](const auto &collect,
                                           QQuickItem *item) -> void {
        if (item == nullptr) return;
        if (item->objectName() == QLatin1StringView("windowTabButton")) {
            buttons.append(item);
        }
        for (QQuickItem *const child : item->childItems()) {
            collect(collect, child);
        }
    };
    collectButtons(collectButtons, qobject_cast<QQuickItem *>(tabBar));
    if (buttons.size() != 2) {
        std::fprintf(stderr,
                     "Tab-bar test hook found %lld buttons at %s; expected 2\n",
                     static_cast<long long>(buttons.size()), stage);
        QCoreApplication::exit(1);
        return false;
    }

    const qreal firstWidth = buttons.at(0)->width();
    const qreal secondWidth = buttons.at(1)->width();
    bool valid = false;
    qreal expectedWidth = 0.0;
    if (wide) {
        const qreal availableWidth =
            tabBar->property("availableWidth").toReal();
        const qreal spacing = tabBar->property("spacing").toReal();
        expectedWidth =
            std::max(qreal{0.0}, (availableWidth - spacing) / qreal{2.0});
        valid = std::abs(firstWidth - expectedWidth) <= 1.0
            && std::abs(secondWidth - expectedWidth) <= 1.0;
    } else {
        valid = firstWidth >= 130.0 && firstWidth <= 240.0
            && secondWidth >= 130.0 && secondWidth <= 240.0;
    }
    if (valid) return true;

    std::fprintf(stderr,
                 "Tab-bar test hook width mismatch at %s: wide=%d, "
                 "widths=%g,%g, expected-wide=%g\n",
                 stage, wide, firstWidth, secondWidth, expectedWidth);
    QCoreApplication::exit(1);
    return false;
}

bool verifyTabBarContentEdge(QObject *rootObject, QObject *tabBar,
                             bool expectedBottom, const char *stage)
{
    auto *const tabBarItem = qobject_cast<QQuickItem *>(tabBar);
    auto *const toolbar =
        rootObject->findChild<QQuickItem *>(QStringLiteral("windowToolbar"));
    QQuickItem *firstButton = nullptr;
    const auto findFirstButton = [&firstButton](const auto &find,
                                                QQuickItem *item) -> void {
        if (item == nullptr || firstButton != nullptr) return;
        if (item->objectName() == QLatin1StringView("windowTabButton")) {
            firstButton = item;
            return;
        }
        for (QQuickItem *const child : item->childItems()) find(find, child);
    };
    findFirstButton(findFirstButton, tabBarItem);
    if (tabBarItem == nullptr || toolbar == nullptr || firstButton == nullptr) {
        std::fprintf(stderr,
                     "Tab-edge test hook could not find geometry at %s\n",
                     stage);
        QCoreApplication::exit(1);
        return false;
    }

    const QPointF barOrigin = tabBarItem->mapToItem(toolbar, QPointF{});
    const QPointF buttonOrigin = firstButton->mapToItem(toolbar, QPointF{});
    const qreal barGap = expectedBottom
        ? barOrigin.y()
        : toolbar->height() - barOrigin.y() - tabBarItem->height();
    const qreal buttonGap = expectedBottom
        ? buttonOrigin.y()
        : toolbar->height() - buttonOrigin.y() - firstButton->height();
    const int expectedPosition = expectedBottom ? 1 : 0;
    const bool valid = tabBarItem->isVisible()
        && tabBar->property("position").toInt() == expectedPosition
        && qAbs(toolbar->property("topPadding").toReal()) <= 0.01
        && qAbs(toolbar->property("bottomPadding").toReal()) <= 0.01
        && qAbs(tabBarItem->height() - firstButton->implicitHeight()) <= 1.0
        && qAbs(barGap) <= 1.0 && qAbs(buttonGap) <= 1.0;
    if (valid) return true;

    std::fprintf(stderr,
                 "Tab-edge test hook found padding at %s: bottom=%d "
                 "toolbar=%g tab=%g,%g button=%g,%g implicit=%g "
                 "bar-gap=%g button-gap=%g top-padding=%g "
                 "bottom-padding=%g position=%d\n",
                 stage, expectedBottom, toolbar->height(), barOrigin.y(),
                 tabBarItem->height(), buttonOrigin.y(), firstButton->height(),
                 firstButton->implicitHeight(), barGap, buttonGap,
                 toolbar->property("topPadding").toReal(),
                 toolbar->property("bottomPadding").toReal(),
                 tabBar->property("position").toInt());
    QCoreApplication::exit(1);
    return false;
}

bool verifyToolbarActionGeometry(QObject *rootObject, QObject *toolbar,
                                 const char *stage)
{
    auto *const actionCluster = rootObject->findChild<QQuickItem *>(
        QStringLiteral("windowToolbarActions"));
    constexpr auto ActionNames = std::to_array<QLatin1StringView>({
        QLatin1StringView("windowNewTabButton"),
        QLatin1StringView("windowSplitRightButton"),
        QLatin1StringView("windowSplitDownButton"),
        QLatin1StringView("windowClosePaneButton"),
    });
    if (actionCluster == nullptr || !actionCluster->isVisible()) {
        std::fprintf(
            stderr,
            "Toolbar-layout test hook found no visible action cluster at %s\n",
            stage);
        QCoreApplication::exit(1);
        return false;
    }

    qreal expectedClusterWidth = 0.0;
    for (const QLatin1StringView name : ActionNames) {
        auto *const button = rootObject->findChild<QQuickItem *>(name);
        if (button == nullptr || !button->isVisible()) {
            std::fprintf(stderr,
                         "Toolbar-layout test hook could not find %.*s at %s\n",
                         static_cast<int>(name.size()), name.data(), stage);
            QCoreApplication::exit(1);
            return false;
        }
        const qreal naturalWidth =
            std::max(button->implicitWidth(), button->implicitHeight());
        const bool naturallySized = button->width() > 0.0
            && button->height() > 0.0
            && std::abs(button->width() - naturalWidth) <= 1.0
            && std::abs(button->height() - button->implicitHeight()) <= 1.0;
        if (!naturallySized) {
            std::fprintf(stderr,
                         "Toolbar-layout test hook stretched %.*s at %s: "
                         "size=%gx%g implicit=%gx%g natural-width=%g\n",
                         static_cast<int>(name.size()), name.data(), stage,
                         button->width(), button->height(),
                         button->implicitWidth(), button->implicitHeight(),
                         naturalWidth);
            QCoreApplication::exit(1);
            return false;
        }
        expectedClusterWidth += naturalWidth;
    }

    const qreal spacing = actionCluster->property("spacing").toReal();
    expectedClusterWidth +=
        spacing * static_cast<qreal>(ActionNames.size() - 1);

    auto *const toolbarItem = qobject_cast<QQuickItem *>(toolbar);
    auto *const tabBar =
        rootObject->findChild<QQuickItem *>(QStringLiteral("windowTabBar"));
    if (toolbarItem == nullptr) {
        qCritical() << "Toolbar-layout test hook received no toolbar item";
        QCoreApplication::exit(1);
        return false;
    }
    const QPointF clusterOrigin =
        actionCluster->mapToItem(toolbarItem, QPointF{});
    const qreal clusterStart = clusterOrigin.x();
    const qreal clusterEnd = clusterStart + actionCluster->width();
    const qreal toolbarWidth = toolbarItem->width();
    const bool mirrored = toolbar->property("mirrored").toBool();
    const qreal trailingPadding =
        toolbar->property(mirrored ? "leftPadding" : "rightPadding").toReal();
    const qreal trailingGap =
        mirrored ? clusterStart : toolbarWidth - clusterEnd;
    const bool atTrailingEdge =
        trailingGap >= -1.0 && std::abs(trailingGap - trailingPadding) <= 2.0;
    bool avoidsTabOverlap = true;
    if (tabBar != nullptr && tabBar->isVisible()) {
        const QPointF tabOrigin = tabBar->mapToItem(toolbarItem, QPointF{});
        const qreal tabStart = tabOrigin.x();
        const qreal tabEnd = tabStart + tabBar->width();
        avoidsTabOverlap = mirrored ? tabStart >= clusterEnd - 1.0
                                    : tabEnd <= clusterStart + 1.0;
    }
    const bool clusterIsCompact =
        std::abs(actionCluster->width() - expectedClusterWidth) <= 1.0
        && actionCluster->width() < toolbarWidth / 2.0 && atTrailingEdge
        && avoidsTabOverlap;
    if (clusterIsCompact) return true;

    std::fprintf(stderr,
                 "Toolbar-layout test hook stretched the action cluster at %s: "
                 "width=%g expected=%g toolbar=%g start=%g end=%g "
                 "trailing-gap=%g padding=%g mirrored=%d tab-overlap=%d\n",
                 stage, actionCluster->width(), expectedClusterWidth,
                 toolbarWidth, clusterStart, clusterEnd, trailingGap,
                 trailingPadding, mirrored, !avoidsTabOverlap);
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
    const auto applyWideTabs = [workspace](bool wide) {
        LaunchOptions options = workspace->effectiveLaunchOptions();
        options.confirmCloseMode = ConfirmCloseMode::Never;
        options.wideTabs = wide;
        workspace->applyLaunchOptions(options);
    };
    const auto exercise = [rootObject, workspace, tabBar, windowToolbar,
                           applyMode, applyWideTabs] {
        auto *const timer = new QTimer(workspace);
        timer->setSingleShot(true);
        const auto stage = std::make_shared<int>(0);
        const auto quitObserved = std::make_shared<bool>(false);

        QObject::connect(
            timer, &QTimer::timeout, workspace,
            [rootObject, workspace, tabBar, windowToolbar, applyMode,
             applyWideTabs, timer, stage, quitObserved] {
                switch (*stage) {
                case 0:
                    if (!verifyTabBarTestState(workspace, tabBar, 1, false,
                                               "auto with one tab")
                        || !verifyToolbarActionGeometry(
                            rootObject, windowToolbar,
                            "auto with one hidden tab")) {
                        return;
                    }
                    applyMode(WindowShowTabBar::Auto);
                    workspace->newTab();
                    break;
                case 1:
                    if (!verifyTabBarTestState(workspace, tabBar, 2, true,
                                               "auto with two tabs")) {
                        return;
                    }
                    if (!workspace->wideTabs()
                        || !verifyTabBarContentEdge(rootObject, tabBar, false,
                                                    "wide top tabs")
                        || !verifyTabButtonWidths(tabBar, true, "wide tabs")
                        || !verifyToolbarActionGeometry(
                            rootObject, windowToolbar,
                            "wide tabs with visible action cluster")) {
                        return;
                    }
                    applyWideTabs(false);
                    break;
                case 2:
                    if (workspace->wideTabs()
                        || !verifyTabButtonWidths(tabBar, false, "compact tabs")
                        || !verifyToolbarActionGeometry(
                            rootObject, windowToolbar,
                            "compact tabs with visible action cluster")) {
                        return;
                    }
                    applyWideTabs(true);
                    break;
                case 3:
                    if (!workspace->wideTabs()
                        || !verifyTabButtonWidths(tabBar, true,
                                                  "wide tabs restored")) {
                        return;
                    }
                    workspace->closeCurrentTab();
                    break;
                case 4:
                    if (!verifyTabBarTestState(
                            workspace, tabBar, 1, false,
                            "auto after returning to one tab")) {
                        return;
                    }
                    applyMode(WindowShowTabBar::Always);
                    break;
                case 5:
                    if (!verifyTabBarTestState(workspace, tabBar, 1, true,
                                               "always with one tab")) {
                        return;
                    }
                    applyMode(WindowShowTabBar::Never);
                    break;
                case 6:
                    if (!verifyTabBarTestState(workspace, tabBar, 1, false,
                                               "never with one tab")) {
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
                case 7:
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
                        qCritical()
                            << "Tab-bar test hook did not observe shutdown";
                        QCoreApplication::exit(1);
                    }
                    return;
                }

                ++*stage;
                timer->start(*stage == 8 ? 1000 : 0);
            });
        timer->start(0);
    };

    if (workspace->tabCount() > 0) {
        QTimer::singleShot(0, workspace, exercise);
    } else {
        QObject::connect(
            workspace, &TerminalWorkspace::tabTitlesChanged, workspace,
            [workspace, exercise] {
                QTimer::singleShot(0, workspace, exercise);
            },
            Qt::SingleShotConnection);
    }
    return true;
}

bool installTabsLocationTestHook(QQuickWindow *window,
                                 TerminalWorkspace *workspace)
{
    if (window == nullptr || workspace == nullptr) {
        qCritical() << "Tabs-location test hook could not find the QML window";
        return false;
    }
    auto *const toolbar =
        window->findChild<QQuickItem *>(QStringLiteral("windowToolbar"));
    auto *const topSlot =
        window->findChild<QQuickItem *>(QStringLiteral("topToolbarSlot"));
    auto *const bottomSlot =
        window->findChild<QQuickItem *>(QStringLiteral("bottomToolbarSlot"));
    QObject *const tabBar =
        window->findChild<QObject *>(QStringLiteral("windowTabBar"));
    if (toolbar == nullptr || topSlot == nullptr || bottomSlot == nullptr
        || tabBar == nullptr) {
        qCritical()
            << "Tabs-location test hook could not find the QML toolbar slots";
        return false;
    }

    LaunchOptions visibleTabs = workspace->effectiveLaunchOptions();
    visibleTabs.windowShowTabBar = WindowShowTabBar::Always;
    workspace->applyLaunchOptions(visibleTabs);

    const auto exercise = [window, workspace, toolbar, topSlot, bottomSlot,
                           tabBar] {
        if (workspace->findChildren<TerminalPane *>().size() != 1) {
            qCritical()
                << "Tabs-location test hook did not start with one terminal pane";
            QCoreApplication::exit(1);
            return;
        }
        workspace->splitRight();
        const QList<TerminalPane *> panes =
            workspace->findChildren<TerminalPane *>();
        if (panes.size() != 2) {
            qCritical() << "Tabs-location test hook could not create a split";
            QCoreApplication::exit(1);
            return;
        }

        const QSize windowSize = window->size();
        const WId nativeWindowId = window->winId();
        QQuickItem *const contentItem = window->contentItem();
        const qreal chromeHeight =
            window->property("terminalChromeHeight").toReal();
        const auto locationSignals = std::make_shared<int>(0);
        QObject::connect(workspace, &TerminalWorkspace::tabsLocationChanged,
                         workspace, [locationSignals] { ++*locationSignals; });

        const auto verify = [window, workspace, toolbar, topSlot, bottomSlot,
                             tabBar, panes, locationSignals, windowSize,
                             nativeWindowId, contentItem, chromeHeight](
                                bool expectedBottom, int expectedSignals,
                                const char *stage) {
            if (!verifyToolbarActionGeometry(window, toolbar, stage)
                || !verifyTabBarContentEdge(window, tabBar, expectedBottom,
                                            stage)) {
                return false;
            }
            QQuickItem *const expectedParent =
                expectedBottom ? bottomSlot : topSlot;
            const QColor topChromeColor =
                topSlot->property("color").value<QColor>();
            const QColor bottomChromeColor =
                bottomSlot->property("color").value<QColor>();
            const bool valid =
                QQuickWindow::hasDefaultAlphaBuffer()
                && window->requestedFormat().hasAlpha()
                && window->format().hasAlpha() && window->color().alpha() == 0
                && topChromeColor.isValid() && topChromeColor.alpha() == 255
                && bottomChromeColor.isValid()
                && bottomChromeColor.alpha() == 255
                && workspace->tabBarAtBottom() == expectedBottom
                && topSlot->isVisible() != expectedBottom
                && bottomSlot->isVisible() == expectedBottom
                && toolbar->parentItem() == expectedParent
                && workspace->findChildren<TerminalPane *>() == panes
                && workspace->window() == window
                && window->contentItem() == contentItem
                && window->winId() == nativeWindowId
                && *locationSignals == expectedSignals
                && window->size() == windowSize
                && qFuzzyCompare(
                    window->property("terminalChromeHeight").toReal(),
                    chromeHeight);
            if (valid) return true;

            qCritical().nospace()
                << "Tabs-location test hook mismatch at " << stage
                << ": bottom=" << workspace->tabBarAtBottom()
                << ", top-visible=" << topSlot->isVisible()
                << ", bottom-visible=" << bottomSlot->isVisible()
                << ", stable-parent="
                << (toolbar->parentItem() == expectedParent)
                << ", alpha-default=" << QQuickWindow::hasDefaultAlphaBuffer()
                << ", alpha-requested=" << window->requestedFormat().hasAlpha()
                << ", alpha-actual=" << window->format().hasAlpha()
                << ", clear-alpha=" << window->color().alpha()
                << ", signals=" << *locationSignals;
            QCoreApplication::exit(1);
            return false;
        };

        auto *const timer = new QTimer(workspace);
        timer->setSingleShot(true);
        const auto stage = std::make_shared<int>(0);
        QObject::connect(timer, &QTimer::timeout, workspace,
                         [workspace, verify, timer, stage] {
                             LaunchOptions options =
                                 workspace->effectiveLaunchOptions();
                             switch (*stage) {
                             case 0:
                                 if (!verify(true, 0, "configured bottom"))
                                     return;
                                 options.tabsLocation = TabsLocation::Top;
                                 workspace->applyLaunchOptions(options);
                                 break;
                             case 1:
                                 if (!verify(false, 1, "live top")) return;
                                 workspace->applyLaunchOptions(options);
                                 break;
                             case 2:
                                 if (!verify(false, 1, "repeated top")) return;
                                 options.tabsLocation = TabsLocation::Bottom;
                                 workspace->applyLaunchOptions(options);
                                 break;
                             case 3:
                                 if (!verify(true, 2, "restored bottom"))
                                     return;
                                 QCoreApplication::quit();
                                 return;
                             default: Q_UNREACHABLE();
                             }
                             ++*stage;
                             timer->start(0);
                         });
        timer->start(0);
    };

    if (workspace->tabCount() > 0) {
        QTimer::singleShot(0, workspace, exercise);
    } else {
        QObject::connect(
            workspace, &TerminalWorkspace::tabTitlesChanged, workspace,
            [workspace, exercise] {
                QTimer::singleShot(0, workspace, exercise);
            },
            Qt::SingleShotConnection);
    }
    return true;
}

QStringView
rendererQualificationGraphicsApiName(QSGRendererInterface::GraphicsApi api)
{
    switch (api) {
    case QSGRendererInterface::Software: return u"software";
    case QSGRendererInterface::OpenGL: return u"opengl";
    case QSGRendererInterface::Vulkan: return u"vulkan";
    case QSGRendererInterface::OpenVG: return u"openvg";
    case QSGRendererInterface::Direct3D11: return u"direct3d11";
    case QSGRendererInterface::Direct3D12: return u"direct3d12";
    case QSGRendererInterface::Metal: return u"metal";
    case QSGRendererInterface::Null: return u"null";
    case QSGRendererInterface::Unknown: return u"unknown";
    }
    return u"unknown";
}

QStringView rendererQualificationDeviceTypeName(QRhiDriverInfo::DeviceType type)
{
    switch (type) {
    case QRhiDriverInfo::UnknownDevice: return u"unknown";
    case QRhiDriverInfo::IntegratedDevice: return u"integrated";
    case QRhiDriverInfo::DiscreteDevice: return u"discrete";
    case QRhiDriverInfo::ExternalDevice: return u"external";
    case QRhiDriverInfo::VirtualDevice: return u"virtual";
    case QRhiDriverInfo::CpuDevice: return u"cpu";
    }
    return u"unknown";
}

QStringView
rendererQualificationSwapchainFormatName(QRhiSwapChain::Format format)
{
    switch (format) {
    case QRhiSwapChain::SDR: return u"sdr";
    case QRhiSwapChain::HDRExtendedSrgbLinear:
        return u"hdr-extended-srgb-linear";
    case QRhiSwapChain::HDR10: return u"hdr10";
    case QRhiSwapChain::HDRExtendedDisplayP3Linear:
        return u"hdr-extended-display-p3-linear";
    }
    return u"unknown";
}

struct RendererQualificationSwapchainInfo {
    QString format;
    quint32 flags = 0;
    int samples = 0;
    QSize pixelSize;
    bool srgb = false;
    bool premultipliedAlpha = false;
    bool nonPremultipliedAlpha = false;
    bool noVsync = false;
    bool hdr = false;
    QString hdrLimits;
    QString hdrBehavior;
    double hdrMinimum = 0.0;
    double hdrMaximum = 0.0;
    double hdrMaximumPotential = 0.0;
    double hdrSdrWhite = 0.0;
};

QString rendererQualificationOutputToken(QStringView value)
{
    return QString::fromLatin1(
        QUrl::toPercentEncoding(value.toString(), QByteArrayLiteral("._-")));
}

QString rendererQualificationRectToken(const QRect &rect)
{
    return QStringLiteral("%1,%2,%3x%4")
        .arg(rect.x())
        .arg(rect.y())
        .arg(rect.width())
        .arg(rect.height());
}

struct RendererQualificationImageStatistics {
    bool nonUniform = false;
    int minimumAlpha = 255;
    int maximumAlpha = 0;
    quint64 translucentPixels = 0;
    quint64 halfAlphaPixels = 0;
};

RendererQualificationImageStatistics
rendererQualificationImageStatistics(const QImage &image)
{
    RendererQualificationImageStatistics result;
    if (image.isNull()) return result;

    const QRgb firstPixel = image.pixel(0, 0);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            const int alpha = qAlpha(pixel);
            result.nonUniform |= pixel != firstPixel;
            result.minimumAlpha = std::min(result.minimumAlpha, alpha);
            result.maximumAlpha = std::max(result.maximumAlpha, alpha);
            if (alpha > 0 && alpha < 255) ++result.translucentPixels;
            if (alpha >= 120 && alpha <= 136) ++result.halfAlphaPixels;
        }
    }
    return result;
}

bool installRendererQualificationTestHook(QQuickWindow *window,
                                          TerminalWorkspace *workspace)
{
    if (window == nullptr || workspace == nullptr) {
        qCritical()
            << "Renderer-qualification test hook requires an initial window";
        return false;
    }

    bool frameCountOk = false;
    int targetFrameCount = qEnvironmentVariableIntValue(
        "GHOSTTY_QT_TEST_RENDERER_QUALIFICATION_FRAMES", &frameCountOk);
    if (!frameCountOk) targetFrameCount = 30;
    if (targetFrameCount < 3 || targetFrameCount > 240) {
        qCritical()
            << "Renderer-qualification frame count must be between 3 and 240";
        return false;
    }

    struct QualificationState {
        QPointer<QQuickWindow> window;
        QPointer<TerminalWorkspace> workspace;
        QElapsedTimer timer;
        QVector<qint64> frameIntervalsNanoseconds;
        qint64 previousFrameNanoseconds = 0;
        int frameCount = 0;
        int targetFrameCount = 0;
        QMutex driverInfoMutex;
        std::optional<QRhiDriverInfo> driverInfo;
        std::optional<RendererQualificationSwapchainInfo> swapchainInfo;
        QString rhiBackend;
        bool complete = false;
        bool failed = false;
    };
    const auto state = std::make_shared<QualificationState>();
    state->window = window;
    state->workspace = workspace;
    state->targetFrameCount = targetFrameCount;
    state->frameIntervalsNanoseconds.reserve(targetFrameCount - 1);
    state->timer.start();

    const auto fail = [state](const QString &message) {
        if (state->failed) return;
        state->failed = true;
        state->complete = true;
        qCritical().noquote() << "Renderer-qualification test hook:" << message;
        QCoreApplication::exit(1);
    };
    const auto unavailable = [state](const QString &message) {
        if (state->complete) return;
        state->complete = true;
        qWarning().noquote()
            << "Renderer-qualification backend unavailable:" << message;
        QCoreApplication::exit(77);
    };

    QObject::connect(
        window, &QQuickWindow::sceneGraphError, window,
        [fail, unavailable](QQuickWindow::SceneGraphError error,
                            const QString &message) {
            if (error == QQuickWindow::ContextNotAvailable) {
                unavailable(message);
                return;
            }
            fail(QStringLiteral("scene graph error: %1").arg(message));
        });
    QObject::connect(
        window, &QQuickWindow::beforeRendering, window,
        [state] {
            QQuickWindow *const quickWindow = state->window.data();
            if (quickWindow == nullptr) return;
            const QSGRendererInterface *const renderer =
                quickWindow->rendererInterface();
            if (renderer == nullptr) return;
            auto *const rhi = static_cast<QRhi *>(renderer->getResource(
                quickWindow, QSGRendererInterface::RhiResource));
            if (rhi == nullptr) return;
            auto *const swapchain =
                static_cast<QRhiSwapChain *>(renderer->getResource(
                    quickWindow, QSGRendererInterface::RhiSwapchainResource));
            if (swapchain == nullptr) return;
            const QRhiSwapChain::Flags flags = swapchain->flags();
            RendererQualificationSwapchainInfo swapchainInfo;
            swapchainInfo.format =
                rendererQualificationSwapchainFormatName(swapchain->format())
                    .toString();
            swapchainInfo.flags = static_cast<quint32>(flags.toInt());
            swapchainInfo.samples = swapchain->sampleCount();
            swapchainInfo.pixelSize = swapchain->currentPixelSize();
            swapchainInfo.srgb = flags.testFlag(QRhiSwapChain::sRGB);
            swapchainInfo.premultipliedAlpha =
                flags.testFlag(QRhiSwapChain::SurfaceHasPreMulAlpha);
            swapchainInfo.nonPremultipliedAlpha =
                flags.testFlag(QRhiSwapChain::SurfaceHasNonPreMulAlpha);
            swapchainInfo.noVsync = flags.testFlag(QRhiSwapChain::NoVSync);
            swapchainInfo.hdr = swapchain->format() != QRhiSwapChain::SDR;
            if (swapchainInfo.hdr) {
                const QRhiSwapChainHdrInfo hdrInfo = swapchain->hdrInfo();
                swapchainInfo.hdrSdrWhite = hdrInfo.sdrWhiteLevel;
                swapchainInfo.hdrBehavior = hdrInfo.luminanceBehavior
                        == QRhiSwapChainHdrInfo::SceneReferred
                    ? QStringLiteral("scene-referred")
                    : QStringLiteral("display-referred");
                if (hdrInfo.limitsType
                    == QRhiSwapChainHdrInfo::LuminanceInNits) {
                    swapchainInfo.hdrLimits = QStringLiteral("nits");
                    swapchainInfo.hdrMinimum =
                        hdrInfo.limits.luminanceInNits.minLuminance;
                    swapchainInfo.hdrMaximum =
                        hdrInfo.limits.luminanceInNits.maxLuminance;
                } else {
                    swapchainInfo.hdrLimits = QStringLiteral("color-component");
                    swapchainInfo.hdrMaximum =
                        hdrInfo.limits.colorComponentValue
                            .maxColorComponentValue;
                    swapchainInfo.hdrMaximumPotential =
                        hdrInfo.limits.colorComponentValue
                            .maxPotentialColorComponentValue;
                }
            }
            const QMutexLocker lock(&state->driverInfoMutex);
            state->driverInfo = rhi->driverInfo();
            state->swapchainInfo = std::move(swapchainInfo);
            state->rhiBackend = QString::fromLatin1(rhi->backendName());
        },
        Qt::DirectConnection);
    QObject::connect(
        window, &QQuickWindow::frameSwapped, window, [state, fail] {
            if (state->complete || state->window == nullptr
                || state->workspace == nullptr) {
                return;
            }
            const qint64 now = state->timer.nsecsElapsed();
            if (state->previousFrameNanoseconds > 0) {
                state->frameIntervalsNanoseconds.append(
                    now - state->previousFrameNanoseconds);
            }
            state->previousFrameNanoseconds = now;
            ++state->frameCount;
            const QList<TerminalPane *> framePanes =
                state->workspace->findChildren<TerminalPane *>();
            const bool sessionRunning =
                std::ranges::any_of(framePanes, [](const TerminalPane *pane) {
                    return pane != nullptr && pane->isRunning();
                });
            if (state->frameCount < state->targetFrameCount
                || !sessionRunning) {
                state->window->update();
                return;
            }

            state->complete = true;
            QTimer::singleShot(0, state->window, [state, fail] {
                QQuickWindow *const quickWindow = state->window.data();
                TerminalWorkspace *const terminalWorkspace =
                    state->workspace.data();
                if (quickWindow == nullptr || terminalWorkspace == nullptr) {
                    fail(
                        QStringLiteral("window disappeared before validation"));
                    return;
                }
                const QSGRendererInterface *const renderer =
                    quickWindow->rendererInterface();
                const QSGRendererInterface::GraphicsApi api =
                    renderer != nullptr ? renderer->graphicsApi()
                                        : QSGRendererInterface::Unknown;
                const QImage image = quickWindow->grabWindow();
                const RendererQualificationImageStatistics imageStatistics =
                    rendererQualificationImageStatistics(image);
                const quint64 pixelCount = static_cast<quint64>(image.width())
                    * static_cast<quint64>(image.height());
                const quint64 minimumHalfAlphaPixels =
                    std::max<quint64>(1, pixelCount / 20);
                const qreal dpr = quickWindow->devicePixelRatio();
                const QSize expectedPhysicalSize(
                    qMax(1, qRound(quickWindow->width() * dpr)),
                    qMax(1, qRound(quickWindow->height() * dpr)));
                const bool physicalSizeMatches =
                    std::abs(image.width() - expectedPhysicalSize.width()) <= 1
                    && std::abs(image.height() - expectedPhysicalSize.height())
                        <= 1;
                const QList<TerminalPane *> panes =
                    terminalWorkspace->findChildren<TerminalPane *>();
                QScreen *const screen = quickWindow->screen();
                const qsizetype runningPaneCount =
                    std::ranges::count_if(panes, [](const TerminalPane *pane) {
                        return pane != nullptr && pane->isRunning();
                    });
                const bool expectTranslucentPixels =
                    qEnvironmentVariableIntValue(
                        "GHOSTTY_QT_TEST_RENDERER_EXPECT_TRANSLUCENT")
                    == 1;
                std::optional<QRhiDriverInfo> driverInfo;
                std::optional<RendererQualificationSwapchainInfo> swapchainInfo;
                QString rhiBackend;
                {
                    const QMutexLocker lock(&state->driverInfoMutex);
                    driverInfo = state->driverInfo;
                    swapchainInfo = state->swapchainInfo;
                    rhiBackend = state->rhiBackend;
                }
                if (!quickWindow->isVisible() || !quickWindow->isExposed()
                    || api == QSGRendererInterface::Unknown
                    || api == QSGRendererInterface::Null
                    || api == QSGRendererInterface::Software
                    || (api != QSGRendererInterface::OpenGL
                        && api != QSGRendererInterface::Vulkan)
                    || image.isNull() || !physicalSizeMatches
                    || !QQuickWindow::hasDefaultAlphaBuffer()
                    || quickWindow->format().alphaBufferSize() <= 0
                    || quickWindow->color().alpha() != 0 || panes.isEmpty()
                    || runningPaneCount == 0 || !image.hasAlphaChannel()
                    || !imageStatistics.nonUniform || !driverInfo.has_value()
                    || !swapchainInfo.has_value() || rhiBackend.isEmpty()
                    || screen == nullptr
                    || swapchainInfo->pixelSize != image.size()
                    || (expectTranslucentPixels
                        && (imageStatistics.translucentPixels > pixelCount
                            || imageStatistics.halfAlphaPixels
                                > imageStatistics.translucentPixels
                            || imageStatistics.halfAlphaPixels
                                < minimumHalfAlphaPixels))) {
                    fail(
                        QStringLiteral(
                            "invalid client surface: visible=%1 exposed=%2 "
                            "api=%3 image=%4x%5 expected=%6x%7 "
                            "default-alpha=%8 alpha-bits=%9 clear-alpha=%10 "
                            "image-alpha=%11 nonuniform=%12 min-alpha=%13 "
                            "max-alpha=%14 translucent-pixels=%15 panes=%16 "
                            "running-panes=%17 expect-translucent=%18 "
                            "half-alpha-pixels=%19 pixel-count=%20 "
                            "minimum-half-alpha-pixels=%21 driver=%22 "
                            "swapchain=%23 swapchain-size=%24 screen=%25")
                            .arg(quickWindow->isVisible())
                            .arg(quickWindow->isExposed())
                            .arg(rendererQualificationGraphicsApiName(api))
                            .arg(image.width())
                            .arg(image.height())
                            .arg(expectedPhysicalSize.width())
                            .arg(expectedPhysicalSize.height())
                            .arg(QQuickWindow::hasDefaultAlphaBuffer())
                            .arg(quickWindow->format().alphaBufferSize())
                            .arg(quickWindow->color().alpha())
                            .arg(image.hasAlphaChannel())
                            .arg(imageStatistics.nonUniform)
                            .arg(imageStatistics.minimumAlpha)
                            .arg(imageStatistics.maximumAlpha)
                            .arg(imageStatistics.translucentPixels)
                            .arg(panes.size())
                            .arg(runningPaneCount)
                            .arg(expectTranslucentPixels)
                            .arg(imageStatistics.halfAlphaPixels)
                            .arg(pixelCount)
                            .arg(minimumHalfAlphaPixels)
                            .arg(driverInfo.has_value())
                            .arg(swapchainInfo.has_value())
                            .arg(
                                swapchainInfo.has_value()
                                    ? QStringLiteral("%1x%2")
                                          .arg(swapchainInfo->pixelSize.width())
                                          .arg(
                                              swapchainInfo->pixelSize.height())
                                    : QStringLiteral("unavailable"))
                            .arg(screen != nullptr));
                    return;
                }

                std::ranges::sort(state->frameIntervalsNanoseconds);
                const auto intervalMicroseconds = [state](qsizetype index) {
                    return static_cast<double>(
                               state->frameIntervalsNanoseconds.at(index))
                        / 1'000.0;
                };
                const qsizetype intervalCount =
                    state->frameIntervalsNanoseconds.size();
                const double medianMicroseconds = intervalCount % 2 == 0
                    ? (intervalMicroseconds(intervalCount / 2 - 1)
                       + intervalMicroseconds(intervalCount / 2))
                        / 2.0
                    : intervalMicroseconds(intervalCount / 2);
                const qsizetype p90Index = std::min(
                    intervalCount - 1,
                    static_cast<qsizetype>(
                        std::ceil(static_cast<double>(intervalCount) * 0.9)
                        - 1.0));
                const QSizeF physicalSize = screen->physicalSize();
                const bool physicalSizeKnown = physicalSize.width() > 0.0
                    && physicalSize.height() > 0.0
                    && std::isfinite(physicalSize.width())
                    && std::isfinite(physicalSize.height());
                const auto physicalDpiToken = [](qreal value) {
                    return value > 0.0 && std::isfinite(value)
                        ? QString::number(value, 'f', 2)
                        : QStringLiteral("na");
                };
                const RendererGraphicsBackend graphicsBackend =
                    api == QSGRendererInterface::Vulkan
                    ? RendererGraphicsBackend::Vulkan
                    : RendererGraphicsBackend::OpenGL;
                const RendererGraphicsLibraryManifest graphicsLibraries =
                    collectRendererGraphicsLibraryManifest(graphicsBackend);

                QTextStream output(stdout);
                output.setRealNumberNotation(QTextStream::FixedNotation);
                output.setRealNumberPrecision(2);
                output
                    << "renderer_qualification benchmark_contract=1"
                    << " qt_version=" << qVersion()
                    << " platform=" << QGuiApplication::platformName()
                    << " graphics_api="
                    << rendererQualificationGraphicsApiName(api)
                    << " graphics_library_contract=1"
                    << " graphics_library_status="
                    << rendererGraphicsLibraryManifestStatusName(
                           graphicsLibraries.status)
                    << " graphics_library_count="
                    << graphicsLibraries.libraries.size()
                    << " graphics_library_sha256="
                    << (graphicsLibraries.aggregateSha256.isEmpty()
                            ? QByteArrayLiteral("unavailable")
                            : graphicsLibraries.aggregateSha256)
                    << " graphics_library_manifest="
                    << rendererQualificationOutputToken(
                           QString::fromUtf8(graphicsLibraries.compactJson))
                    << " dpr=" << dpr << " logical=" << quickWindow->width()
                    << 'x' << quickWindow->height()
                    << " physical=" << image.width() << 'x' << image.height()
                    << " frame_swaps=" << state->frameCount
                    << " median_frame_interval_us=" << medianMicroseconds
                    << " p90_frame_interval_us="
                    << intervalMicroseconds(p90Index) << " alpha_buffer_bits="
                    << quickWindow->format().alphaBufferSize()
                    << " clear_alpha=" << quickWindow->color().alpha()
                    << " image_alpha=" << image.hasAlphaChannel()
                    << " nonuniform=" << imageStatistics.nonUniform
                    << " min_alpha=" << imageStatistics.minimumAlpha
                    << " max_alpha=" << imageStatistics.maximumAlpha
                    << " translucent_pixels="
                    << imageStatistics.translucentPixels
                    << " half_alpha_pixels=" << imageStatistics.halfAlphaPixels
                    << " pixel_count=" << pixelCount
                    << " minimum_half_alpha_pixels=" << minimumHalfAlphaPixels
                    << " panes=" << panes.size()
                    << " running_panes=" << runningPaneCount
                    << " screen_count=" << QGuiApplication::screens().size()
                    << " screen_name="
                    << rendererQualificationOutputToken(screen->name())
                    << " screen_manufacturer="
                    << rendererQualificationOutputToken(screen->manufacturer())
                    << " screen_model="
                    << rendererQualificationOutputToken(screen->model())
                    << " screen_serial="
                    << rendererQualificationOutputToken(screen->serialNumber())
                    << " screen_geometry="
                    << rendererQualificationRectToken(screen->geometry())
                    << " screen_available_geometry="
                    << rendererQualificationRectToken(
                           screen->availableGeometry())
                    << " screen_physical_mm="
                    << (physicalSizeKnown
                            ? QStringLiteral("%1x%2")
                                  .arg(QString::number(physicalSize.width(),
                                                       'f', 2))
                                  .arg(QString::number(physicalSize.height(),
                                                       'f', 2))
                            : QStringLiteral("na"))
                    << " screen_depth=" << screen->depth()
                    << " screen_dpr=" << screen->devicePixelRatio()
                    << " screen_logical_dpi_x=" << screen->logicalDotsPerInchX()
                    << " screen_logical_dpi_y=" << screen->logicalDotsPerInchY()
                    << " screen_physical_dpi_x="
                    << physicalDpiToken(screen->physicalDotsPerInchX())
                    << " screen_physical_dpi_y="
                    << physicalDpiToken(screen->physicalDotsPerInchY())
                    << " screen_refresh_millihz="
                    << qRound64(screen->refreshRate() * 1'000.0)
                    << " screen_orientation="
                    << static_cast<int>(screen->orientation())
                    << " screen_primary="
                    << (screen == QGuiApplication::primaryScreen())
                    << " swapchain_format=" << swapchainInfo->format
                    << " swapchain_flags=" << swapchainInfo->flags
                    << " swapchain_samples=" << swapchainInfo->samples
                    << " swapchain_size=" << swapchainInfo->pixelSize.width()
                    << 'x' << swapchainInfo->pixelSize.height()
                    << " swapchain_srgb=" << swapchainInfo->srgb
                    << " swapchain_premultiplied_alpha="
                    << swapchainInfo->premultipliedAlpha
                    << " swapchain_nonpremultiplied_alpha="
                    << swapchainInfo->nonPremultipliedAlpha
                    << " swapchain_no_vsync=" << swapchainInfo->noVsync
                    << " swapchain_hdr=" << swapchainInfo->hdr
                    << " swapchain_hdr_limits="
                    << (swapchainInfo->hdr ? swapchainInfo->hdrLimits
                                           : QStringLiteral("na"))
                    << " swapchain_hdr_behavior="
                    << (swapchainInfo->hdr ? swapchainInfo->hdrBehavior
                                           : QStringLiteral("na"))
                    << " swapchain_hdr_minimum="
                    << (swapchainInfo->hdr
                                && swapchainInfo->hdrLimits
                                    == QStringLiteral("nits")
                            ? QString::number(swapchainInfo->hdrMinimum)
                            : QStringLiteral("na"))
                    << " swapchain_hdr_maximum="
                    << (swapchainInfo->hdr
                            ? QString::number(swapchainInfo->hdrMaximum)
                            : QStringLiteral("na"))
                    << " swapchain_hdr_maximum_potential="
                    << (swapchainInfo->hdr
                                && swapchainInfo->hdrLimits
                                    == QStringLiteral("color-component")
                            ? QString::number(
                                  swapchainInfo->hdrMaximumPotential)
                            : QStringLiteral("na"))
                    << " swapchain_hdr_sdr_white="
                    << (swapchainInfo->hdr
                            ? QString::number(swapchainInfo->hdrSdrWhite)
                            : QStringLiteral("na"))
                    << " rhi_backend="
                    << rendererQualificationOutputToken(rhiBackend)
                    << " rhi_device_name="
                    << rendererQualificationOutputToken(
                           QString::fromUtf8(driverInfo->deviceName))
                    << " rhi_device_type="
                    << rendererQualificationDeviceTypeName(
                           driverInfo->deviceType)
                    << " rhi_vendor_id=0x"
                    << QString::number(driverInfo->vendorId, 16)
                    << " rhi_device_id=0x"
                    << QString::number(driverInfo->deviceId, 16) << '\n';
                output.flush();
                QCoreApplication::quit();
            });
        });

    QTimer::singleShot(15'000, window, [state, fail] {
        if (!state->complete) {
            fail(QStringLiteral("timed out waiting for frame swaps"));
        }
    });
    QTimer::singleShot(0, window, [state] {
        if (!state->complete && state->window != nullptr) {
            state->window->update();
        }
    });
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    const std::span<char *const> rawArguments(argv,
                                              static_cast<std::size_t>(argc));
    const bool probableCli = argc > 1 || !qgetenv("TERM_PROGRAM").isEmpty();
    const GhosttyCliActionSelection cliAction =
        selectGhosttyCliAction(rawArguments);
    switch (cliAction.disposition) {
    case GhosttyCliActionDisposition::None: break;
    case GhosttyCliActionDisposition::Unsupported:
        std::fprintf(stderr,
                     "ghostty-qt: unsupported Ghostty CLI action '%.*s'\n",
                     static_cast<int>(cliAction.argument.size()),
                     cliAction.argument.data());
        return 2;
    case GhosttyCliActionDisposition::Multiple:
        std::fprintf(stderr,
                     "ghostty-qt: multiple Ghostty CLI actions are not allowed "
                     "(second action: '%.*s')\n",
                     static_cast<int>(cliAction.argument.size()),
                     cliAction.argument.data());
        return 2;
    case GhosttyCliActionDisposition::ApplicationIpc: {
        const GhosttyApplicationIpcAction action = [&] {
            if (cliAction.argument == std::string_view("+new-tab")) {
                return GhosttyApplicationIpcAction::NewTab;
            }
            if (cliAction.argument == std::string_view("+new-window")) {
                return GhosttyApplicationIpcAction::NewWindow;
            }
            return GhosttyApplicationIpcAction::ToggleQuickTerminal;
        }();
        auto request = parseGhosttyApplicationIpcRequest(
            action, rawArguments,
            GhosttyApplicationIpcParseContext::fromProcess(
                QStringLiteral(GHOSTTY_QT_APPLICATION_ID)));
        if (!request) {
            QTextStream(stderr)
                << "ghostty-qt: " << request.error().diagnostic << '\n';
            return request.error().exitCode();
        }

        // The action client never constructs QGuiApplication or a native
        // surface. QCoreApplication supplies Qt's D-Bus runtime while the
        // blocking call lets service activation finish before this process
        // exits.
        QCoreApplication ipcApplication(argc, argv);
        auto sent = sendGhosttyApplicationIpcRequest(*request);
        if (!sent) {
            QTextStream(stderr)
                << "ghostty-qt: " << sent.error().diagnostic << '\n';
            return sent.error().exitCode();
        }
        return 0;
    }
    case GhosttyCliActionDisposition::Delegate:
#if GHOSTTY_QT_CONFIG_ENABLED
    {
        const GhosttyCliExecError failure =
            execGhosttyCliHelper(rawArguments, GHOSTTY_QT_CONFIG_HELPER_NAME);
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

    auto parsedOptions = parseLaunchOptionsFromRaw(rawArguments);
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

    const QString frontendConfigPath =
        FrontendConfigService::standardConfigPath();

    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("wayland"));
    }

    // Launcher presentation data is a one-shot capability. Capture and clear
    // it before Qt, config helpers, or terminal workers can start threads or
    // child processes; the typed value is projected only while its target
    // window is shown.
    DesktopActivationContext startupActivation =
        DesktopActivationContext::takeFromEnvironment();

#if GHOSTTY_QT_CONFIG_ENABLED
    QString configHelperPath;
    std::optional<QString> identityConfigFailure;
    const auto siblingHelper =
        siblingExecutablePath(QStringLiteral(GHOSTTY_QT_CONFIG_HELPER_NAME));
    if (!siblingHelper.has_value()) {
        QTextStream(stderr) << "ghostty-qt: " << siblingHelper.error() << '\n';
        return 1;
    }
    configHelperPath = *siblingHelper;
#endif
    std::optional<QByteArray> preGuiApplicationClass = options.applicationClass;
#if GHOSTTY_QT_CONFIG_ENABLED
    {
        const GhosttyConfigLoader identityLoader =
            makeGhosttyConfigProcessLoader({
                .helperPath = configHelperPath,
                .probableCli = probableCli,
                .configurationArguments =
                    ghosttyConfigurationArguments(options),
                .frontendConfigPath = frontendConfigPath,
            });
        GhosttyConfigLoadResult loadedIdentityConfig = identityLoader({
            .candidatePaths = GhosttyConfigService::standardConfigPaths(),
            // `class` is startup identity rather than appearance. The normal
            // post-QApplication load below verifies it is identical under the
            // actual Qt color scheme before any surface or D-Bus endpoint is
            // created.
            .colorScheme = TerminalColorScheme::Light,
        });
        if (loadedIdentityConfig.has_value()) {
            preGuiApplicationClass =
                std::move(loadedIdentityConfig->values.applicationClass);
        } else {
            identityConfigFailure = std::move(loadedIdentityConfig.error());
        }
    }
#endif
    const auto preGuiIdentity = resolveApplicationIdentity(
        preGuiApplicationClass, QStringLiteral(GHOSTTY_QT_APPLICATION_ID));
    if (!preGuiIdentity.has_value()) {
        QTextStream(stderr) << "ghostty-qt: " << preGuiIdentity.error() << '\n';
        return 1;
    }

    // Qt's Unix platform services can issue portal calls from QApplication's
    // constructor. The host registry permits identity registration only
    // before the connection's first portal method, so all process and desktop
    // identity must be published before constructing the GUI application.
    QCoreApplication::setApplicationName(QStringLiteral("ghostty-qt"));
    QCoreApplication::setApplicationVersion(QStringLiteral(GHOSTTY_QT_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("ghostty-qt"));
    QGuiApplication::setDesktopFileName(preGuiIdentity->applicationId);

    // Qt 6.11 registers desktopFileName with the host portal from the Wayland
    // platform-services constructor. Host registration intentionally rejects
    // IDs without an installed desktop entry, which is normal for direct
    // build-tree runs and arbitrary `class` overrides. Suppress only that Qt
    // platform-service initialization when metadata is not discoverable; the
    // override is removed immediately so terminal children and ghostty-qt's
    // own portal clients retain the caller's environment.
    constexpr auto NoDesktopPortal = "QT_NO_XDG_DESKTOP_PORTAL";
#if QT_VERSION >= QT_VERSION_CHECK(6, 11, 0)
    const bool suppressUnavailableHostRegistration =
        !qEnvironmentVariableIsSet(NoDesktopPortal)
        && !hasDiscoverableDesktopEntry(preGuiIdentity->applicationId);
#else
    constexpr bool suppressUnavailableHostRegistration = false;
#endif
    if (suppressUnavailableHostRegistration) {
        (void)qputenv(NoDesktopPortal, QByteArrayLiteral("1"));
    }

    // An alpha channel is a native-surface capability and cannot be added by
    // live reload after the first QQuickWindow has been created. Request it
    // unconditionally so an initially opaque terminal can become translucent
    // without recreating its window, scene graph, panes, or PTYs.
    QQuickWindow::setDefaultAlphaBuffer(true);
    QApplication application(argc, argv);
    if (suppressUnavailableHostRegistration) {
        (void)qunsetenv(NoDesktopPortal);
    }

    // Plasma's optional Qt Quick Controls bridge delegates control rendering
    // to the user's active QStyle (normally Breeze, including custom themes).
    // QApplication must exist before discovery so an application-local
    // qt.conf and executable-relative import roots are visible. This remains
    // safely before the first Qt Quick Controls QML module is loaded.
    if (shouldSelectKdeDesktopQuickControlsStyle()) {
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    }

    // Ghostty owns last-window process lifetime, including disabled and
    // delayed modes. Qt's implicit auto-quit would bypass that policy.
    application.setQuitOnLastWindowClosed(false);

    // Mirror the compositor's XKB keymap before any terminal surfaces can
    // receive input. Stack lifetime releases the extra wl_keyboard while the
    // QApplication and its Wayland display are still alive.
    WaylandKeyboardLayout keyboardLayout;

    SystemdApplicationLifecycle systemdLifecycle;
    const auto reloadSignalInstalled = systemdLifecycle.installReloadSignal();
    if (!reloadSignalInstalled) {
        qCritical().noquote()
            << "Could not install the systemd SIGUSR2 reload bridge:"
            << reloadSignalInstalled.error();
        return 1;
    }
    QObject::connect(
        &systemdLifecycle, &SystemdApplicationLifecycle::notificationFailed,
        &application, [](const QString &message) {
            qWarning().noquote() << "Systemd notification failed:" << message;
        });
#if GHOSTTY_QT_CONFIG_ENABLED
    if (identityConfigFailure.has_value()) {
        qWarning().noquote()
            << "Ghostty configuration was unavailable while resolving the pre-GUI application identity; using command-line or build identity:"
            << *identityConfigFailure;
    }
#endif
    QStyleHints *const styleHints = QGuiApplication::styleHints();
    ApplicationAppearance appearance(
        ApplicationAppearance::fromQtColorScheme(styleHints->colorScheme()));

    const bool allowNonWayland =
        qEnvironmentVariableIntValue("GHOSTTY_QT_ALLOW_NON_WAYLAND") == 1;
    if (QGuiApplication::platformName() != QStringLiteral("wayland")
        && !allowNonWayland) {
        QTextStream(stderr)
            << "ghostty-qt supports the Qt Wayland platform only (active platform: "
            << QGuiApplication::platformName() << ").\n";
        return 2;
    }

    FrontendConfigService frontendConfigService;
    if (!frontendConfigService.hasSnapshot()) {
        qWarning().noquote()
            << "ghostty-qt frontend configuration is unavailable; using built-in and command-line defaults:"
            << frontendConfigService.lastError();
    }
    QObject::connect(
        &frontendConfigService, &FrontendConfigService::reloadFailed,
        &application, [](const QString &message) {
            qWarning().noquote()
                << "ghostty-qt frontend configuration reload failed; keeping the last valid configuration:"
                << message;
        });

#if GHOSTTY_QT_CONFIG_ENABLED
    GhosttyConfigService configService(
        makeGhosttyConfigProcessLoader({
            .helperPath = configHelperPath,
            .probableCli = probableCli,
            .configurationArguments = ghosttyConfigurationArguments(options),
            .frontendConfigPath = frontendConfigPath,
        }),
        appearance.colorScheme(), options.configDefaultFiles);
    if (!configService.hasSnapshot()) {
        qWarning().noquote()
            << "Ghostty configuration is unavailable; using built-in and command-line defaults"
            << QStringLiteral("(helper: %1):").arg(configHelperPath)
            << configService.lastError();
    }
    QObject::connect(
        &configService, &GhosttyConfigService::reloadFailed, &application,
        [](const QString &message) {
            qWarning().noquote()
                << "Ghostty configuration reload failed; keeping the last valid configuration:"
                << message;
        });
#endif

    const auto resolveProjectedOptions = [&] {
#if GHOSTTY_QT_CONFIG_ENABLED
        const GhosttyConfigSnapshot *const ghosttySnapshot =
            configService.hasSnapshot() ? &configService.snapshot() : nullptr;
#else
        const GhosttyConfigSnapshot *const ghosttySnapshot = nullptr;
#endif
        const FrontendConfigSnapshot *const frontendSnapshot =
            frontendConfigService.hasSnapshot()
            ? &frontendConfigService.snapshot()
            : nullptr;
        LaunchOptions result =
            resolveLaunchOptions(options, ghosttySnapshot, frontendSnapshot);
        result.colorScheme = appearance.colorScheme();
        return result;
    };
    const auto reconcileAppearance = [&]([[maybe_unused]] bool synchronous) {
        LaunchOptions result;
        // A forced window theme can select the opposite conditional theme.
        // Settle startup before constructing QML controls; runtime changes use
        // the same loop but leave the expensive helper work debounced.
        for (int attempt = 0; attempt < 3; ++attempt) {
            result = resolveProjectedOptions();
            (void)appearance.apply(result.windowAppearance,
                                   result.appearance.backgroundColor);
            result.colorScheme = appearance.colorScheme();
#if GHOSTTY_QT_CONFIG_ENABLED
            if (configService.colorScheme() == appearance.colorScheme()) {
                break;
            }
            configService.setColorScheme(appearance.colorScheme());
            if (!synchronous) break;
            configService.reloadNow();
#else
            break;
#endif
        }
        result = resolveProjectedOptions();
        (void)appearance.apply(result.windowAppearance,
                               result.appearance.backgroundColor);
        result.colorScheme = appearance.colorScheme();
        return result;
    };
    LaunchOptions effectiveApplicationOptions = reconcileAppearance(true);
#if GHOSTTY_QT_CONFIG_ENABLED
    if (configService.hasSnapshot()) {
        reportConfigDiagnostics(configService.snapshot());
    }
#endif

    const auto identity =
        resolveApplicationIdentity(effectiveApplicationOptions.applicationClass,
                                   QStringLiteral(GHOSTTY_QT_APPLICATION_ID));
    if (!identity.has_value()) {
        qCritical().noquote() << identity.error();
        return 1;
    }
    if (identity->diagnostic.has_value()) {
        qWarning().noquote() << *identity->diagnostic;
    }
    if (identity->applicationId != preGuiIdentity->applicationId) {
        qCritical().noquote()
            << QStringLiteral(
                   "Ghostty application identity changed during GUI startup "
                   "(%1 -> %2); restart after the configuration is stable")
                   .arg(preGuiIdentity->applicationId, identity->applicationId);
        return 1;
    }

    std::unique_ptr<SingleInstanceActivation> activationEndpoint;
    if (shouldUseSingleInstance(effectiveApplicationOptions,
                                QByteArrayView(qgetenv("TERM_PROGRAM")))) {
        auto candidate = std::make_unique<SingleInstanceActivation>(
            SingleInstanceActivation::defaultConnection(),
            identity->serviceId());
        const SingleInstanceActivation::StartResult activation =
            candidate->start({
                .existingInstanceAction =
                    effectiveApplicationOptions.initialWindow
                    ? SingleInstanceActivation::ExistingInstanceAction::Activate
                    : SingleInstanceActivation::ExistingInstanceAction::
                          DoNotActivate,
                .activation = startupActivation,
            });
        switch (activation.role) {
        case SingleInstanceActivation::Role::ActivatedExisting:
        case SingleInstanceActivation::Role::ExistingInstance: return 0;
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
    // Cgroup single-instance policy follows the process role that startup
    // arbitration actually established. Keep this invariant fixed across
    // later frontend and shared-config reloads.
    options.processUsesSingleInstance = activationEndpoint != nullptr;
    effectiveApplicationOptions.processUsesSingleInstance =
        options.processUsesSingleInstance;

    // The engine and process controller both outlive every QML root. Their
    // declaration order tears down the controller, portal, windows, and pane
    // workers before the engine itself.
    QQmlApplicationEngine engine;
    ApplicationController applicationController(engine,
                                                effectiveApplicationOptions);
    QObject::connect(
        &frontendConfigService, &FrontendConfigService::reloadFailed,
        &applicationController,
        [&applicationController](const QString &message) {
            applicationController.reportConfigurationFailure(
                ApplicationController::ConfigurationSource::Frontend, message);
        });
    if (!frontendConfigService.lastError().isEmpty()) {
        applicationController.reportConfigurationFailure(
            ApplicationController::ConfigurationSource::Frontend,
            frontendConfigService.lastError());
    }
#if GHOSTTY_QT_CONFIG_ENABLED
    QObject::connect(
        &configService, &GhosttyConfigService::reloadFailed,
        &applicationController,
        [&applicationController](const QString &message) {
            applicationController.reportConfigurationFailure(
                ApplicationController::ConfigurationSource::Ghostty, message);
        });
    if (!configService.lastError().isEmpty()) {
        applicationController.reportConfigurationFailure(
            ApplicationController::ConfigurationSource::Ghostty,
            configService.lastError());
    }
#endif
    QObject::connect(&applicationController,
                     &ApplicationController::quitRequested, &application,
                     &QCoreApplication::quit);
    QObject::connect(&applicationController,
                     &ApplicationController::windowCreationFailed, &application,
                     [](const QString &message) {
                         qWarning().noquote()
                             << "Could not create a new terminal window:"
                             << message;
                     });
    QObject::connect(&applicationController,
                     &ApplicationController::configOpenRequested, &application,
                     [frontendConfigPath] {
                         const auto opened =
                             openGhosttyConfigForEditing({frontendConfigPath});
                         if (!opened.has_value()) {
                             qWarning().noquote()
                                 << "Could not open the Ghostty configuration:"
                                 << opened.error();
                         }
                     });
    QObject::connect(
        &applicationController,
        &ApplicationController::configOpenInNewWindowRequested, &application,
        [&applicationController, frontendConfigPath] {
            const auto path =
                prepareGhosttyConfigForEditing({frontendConfigPath});
            if (!path.has_value()) {
                qWarning().noquote()
                    << "Could not prepare the Ghostty configuration:"
                    << path.error();
                return;
            }
            QByteArray editor = qgetenv("VISUAL");
            if (editor.isEmpty()) editor = qgetenv("EDITOR");
            if (editor.isEmpty()) editor = QByteArrayLiteral("vi");
            const QByteArray encodedPath = QFile::encodeName(*path);
            GhosttyNewWindowTransportOverrides overrides{
                .command = TerminalCommand::direct(
                    {QByteArrayLiteral("/bin/sh"), QByteArrayLiteral("-c"),
                     editor + QByteArrayLiteral(" ")
                         + shellQuote(encodedPath)}),
                .shellIntegration = std::nullopt,
                .workingDirectory = std::nullopt,
                .titleOverride =
                    QStringLiteral("Editing configuration file %1").arg(*path),
            };
            if (!applicationController.activateNewWindow(std::move(overrides),
                                                         {})) {
                qWarning().noquote()
                    << "Could not open the Ghostty configuration in a new window";
            }
        });

    QObject::connect(&systemdLifecycle,
                     &SystemdApplicationLifecycle::reloadRequested,
                     &applicationController, [&applicationController] {
                         (void)applicationController.dispatch(
                             ApplicationAction::ReloadConfig);
                     });

#if GHOSTTY_QT_CONFIG_ENABLED
    const auto applyCurrentOptions = [&] {
        applicationController.applyLaunchOptions(reconcileAppearance(false));
    };
    QObject::connect(
        &configService, &GhosttyConfigService::changed, &applicationController,
        [&applyCurrentOptions,
         &applicationController](const GhosttyConfigSnapshot &) {
            applicationController.clearConfigurationFailure(
                ApplicationController::ConfigurationSource::Ghostty);
            applyCurrentOptions();
            applicationController.notifyConfigurationReloaded();
        });
    QObject::connect(&configService, &GhosttyConfigService::changed,
                     &application, &reportConfigDiagnostics);
    QObject::connect(
        &configService, &GhosttyConfigService::reloadScheduled,
        &systemdLifecycle, [&systemdLifecycle, &configService](quint64 epoch) {
            systemdLifecycle.reloadScheduled(&configService, epoch);
        });
    QObject::connect(&configService, &GhosttyConfigService::reloadSettled,
                     &systemdLifecycle,
                     [&systemdLifecycle, &configService](quint64 epoch) {
                         systemdLifecycle.reloadSettled(&configService, epoch);
                     });
    QObject::connect(&applicationController,
                     &ApplicationController::configReloadRequested,
                     &configService, &GhosttyConfigService::requestReload);
#else
    const auto applyCurrentOptions = [&] {
        applicationController.applyLaunchOptions(reconcileAppearance(false));
    };
#endif
    QObject::connect(
        &frontendConfigService, &FrontendConfigService::changed,
        &applicationController,
        [&applyCurrentOptions,
         &applicationController](const FrontendConfigSnapshot &) {
            applicationController.clearConfigurationFailure(
                ApplicationController::ConfigurationSource::Frontend);
            applyCurrentOptions();
            applicationController.notifyConfigurationReloaded();
        });
    QObject::connect(
        &frontendConfigService, &FrontendConfigService::reloadScheduled,
        &systemdLifecycle,
        [&systemdLifecycle, &frontendConfigService](quint64 epoch) {
            systemdLifecycle.reloadScheduled(&frontendConfigService, epoch);
        });
    QObject::connect(
        &frontendConfigService, &FrontendConfigService::reloadSettled,
        &systemdLifecycle,
        [&systemdLifecycle, &frontendConfigService](quint64 epoch) {
            systemdLifecycle.reloadSettled(&frontendConfigService, epoch);
        });
    QObject::connect(
        &applicationController, &ApplicationController::configReloadRequested,
        &frontendConfigService, &FrontendConfigService::requestReload);
    QObject::connect(
        styleHints, &QStyleHints::colorSchemeChanged, &applicationController,
        [&](Qt::ColorScheme scheme) {
            if (!appearance.setSystemColorScheme(
                    ApplicationAppearance::fromQtColorScheme(scheme))) {
                return;
            }
#if GHOSTTY_QT_CONFIG_ENABLED
            configService.setColorScheme(appearance.colorScheme());
#endif
            applyCurrentOptions();
        });

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
    QQuickWindow *const applicationWindow =
        initialWindow ? initialWindow->window : nullptr;
    TerminalWorkspace *const workspace =
        initialWindow ? initialWindow->workspace : nullptr;

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

    const bool suppressedStartupTest =
        qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_INITIAL_WINDOW") == 1;
    bool suppressedStartupTestCompleted = false;
    if (suppressedStartupTest
        && (initialWindow
            || !installSuppressedStartupTestHook(
                &applicationController, effectiveApplicationOptions,
                &suppressedStartupTestCompleted))) {
        return 1;
    }

    const bool desktopActivationTest =
        qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_DESKTOP_ACTIVATION") == 1;
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
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_CONFIRM_CLOSE_DIALOG")
        == 1) {
        if (!initialWindow
            || !installCloseDialogTestHook(applicationWindow, workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_TAB_TITLE_PROMPT") == 1) {
        if (!initialWindow
            || !installTitlePromptTestHook(applicationWindow, workspace,
                                           TitlePromptTestTarget::Tab)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_SURFACE_TITLE_PROMPT")
        == 1) {
        if (!initialWindow
            || !installTitlePromptTestHook(applicationWindow, workspace,
                                           TitlePromptTestTarget::Surface)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_CONTEXT_MENU_POSITION")
        == 1) {
        if (!initialWindow
            || !installContextMenuPositionTestHook(applicationWindow,
                                                   workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_CONTEXT_MENU_ACTION")
        == 1) {
        if (!initialWindow
            || !installContextMenuActionTestHook(applicationWindow, workspace,
                                                 &applicationController)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_TOGGLE_FULLSCREEN")
        == 1) {
        if (!initialWindow
            || !installFullscreenActionTestHook(applicationWindow, workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_INITIAL_WINDOW_STATE")
        == 1) {
        if (!initialWindow
            || !installInitialWindowStateTestHook(
                applicationWindow, effectiveApplicationOptions)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_INITIAL_WINDOW_SIZE")
        == 1) {
        if (!initialWindow
            || !installInitialWindowSizeTestHook(applicationWindow, workspace,
                                                 effectiveApplicationOptions)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_TOGGLE_MAXIMIZE") == 1) {
        if (!initialWindow
            || !installMaximizeActionTestHook(applicationWindow, workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue(
            "GHOSTTY_QT_TEST_TOGGLE_WINDOW_DECORATIONS")
        == 1) {
        if (!initialWindow
            || !installWindowDecorationActionTestHook(applicationWindow,
                                                      workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_TAB_BAR_VISIBILITY")
        == 1) {
        if (!initialWindow
            || !installTabBarVisibilityTestHook(applicationWindow, workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_TABS_LOCATION") == 1) {
        if (!initialWindow
            || !installTabsLocationTestHook(applicationWindow, workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_RENDERER_QUALIFICATION")
        == 1) {
        if (!initialWindow
            || !installRendererQualificationTestHook(applicationWindow,
                                                     workspace)) {
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
                ApplicationActivationRequest request) {
                if (controller == nullptr) return false;
                using Kind = ApplicationActivationRequest::Kind;
                switch (request.kind) {
                case Kind::Activate:
                    return controller->activateNoCommand(
                        std::move(request.activation));
                case Kind::NewWindow:
                    return controller->activateNewWindow(
                        GhosttyNewWindowTransportOverrides{},
                        std::move(request.activation));
                case Kind::NewTab: {
                    auto tab = decodeGhosttyNewTabParameter({
                        .surfaceId = request.surfaceId,
                        .arguments = std::move(request.arguments),
                    });
                    if (!tab) {
                        qWarning().noquote()
                            << "Rejected Ghostty new-tab action:"
                            << tab.error().diagnostic;
                        return false;
                    }
                    return controller->activateNewTab(
                        std::move(*tab), std::move(request.activation));
                }
                case Kind::NewWindowCommand: {
                    auto overrides =
                        decodeGhosttyNewWindowArguments(request.arguments);
                    if (!overrides) {
                        qWarning().noquote()
                            << "Rejected Ghostty new-window action:"
                            << overrides.error().diagnostic;
                        return false;
                    }
                    return controller->activateNewWindow(
                        std::move(*overrides), std::move(request.activation));
                }
                case Kind::ToggleQuickTerminal:
                    return controller->activateQuickTerminal(
                        std::move(request.activation));
                }
                return false;
            });
    }
    const auto activationHandlerGuard = qScopeGuard([&activationEndpoint] {
        if (activationEndpoint) {
            activationEndpoint->setActivationHandler({});
        }
    });

    // Remote launchers have already returned above. At this point the serving
    // process has committed its initial-window policy, installed every action
    // and reload handler, and can safely accept systemd activation or reload.
    systemdLifecycle.applicationReady();
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
