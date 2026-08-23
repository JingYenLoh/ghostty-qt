#include "app/application_appearance.h"
#include "app/application_controller.h"
#include "app/application_identity.h"
#include "app/ghostty_application_ipc.h"
#include "app/ghostty_cli_delegation.h"
#include "app/launch_options.h"
#include "config/frontend_config_service.h"
#include "config/ghostty_config_edit.h"
#include "config/ghostty_config_service.h"
#include "desktop/desktop_activation.h"
#include "desktop/desktop_quick_controls_style.h"
#include "desktop/single_instance_activation.h"
#include "desktop/systemd_notify.h"
#include "input/keyboard_layout.h"
#include "terminal/core/terminal_cell_metrics.h"
#include "terminal/core/terminal_geometry.h"
#include "terminal/rendering/renderer_graphics_library_manifest.h"
#include "testing/application_test_hooks.h"
#include "workspace/terminal_pane.h"
#include "workspace/terminal_workspace.h"

#if GHOSTTY_QT_CONFIG_ENABLED
#include "config/ghostty_config_process_loader.h"
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

} // namespace

namespace TestHooks = ApplicationTestHooks;

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

    const TestHooks::ApplicationLifetimeTestMode lifetimeTestMode =
        TestHooks::applicationLifetimeTestMode();
    bool lifetimeTestCompleted = false;
    if ((lifetimeTestMode != TestHooks::ApplicationLifetimeTestMode::None
         && !initialWindow)
        || !TestHooks::installApplicationLifetimeTestHook(
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
            || !TestHooks::installSuppressedStartupTestHook(
                &applicationController, effectiveApplicationOptions,
                &suppressedStartupTestCompleted))) {
        return 1;
    }

    const bool desktopActivationTest =
        qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_DESKTOP_ACTIVATION") == 1;
    bool desktopActivationTestCompleted = false;
    if (desktopActivationTest
        && (initialWindow
            || !TestHooks::installDesktopActivationTestHook(
                &applicationController, &desktopActivationTestCompleted))) {
        return 1;
    }

    // Headless regression hook: exercise the real QML confirmation dialog and
    // complete shutdown without synthesizing compositor input. It is inert in
    // every normal launch.
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_CONFIRM_CLOSE_DIALOG")
        == 1) {
        if (!initialWindow
            || !TestHooks::installCloseDialogTestHook(applicationWindow,
                                                      workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_TAB_TITLE_PROMPT") == 1) {
        if (!initialWindow
            || !TestHooks::installTitlePromptTestHook(
                applicationWindow, workspace,
                TestHooks::TitlePromptTestTarget::Tab)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_SURFACE_TITLE_PROMPT")
        == 1) {
        if (!initialWindow
            || !TestHooks::installTitlePromptTestHook(
                applicationWindow, workspace,
                TestHooks::TitlePromptTestTarget::Surface)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_CONTEXT_MENU_POSITION")
        == 1) {
        if (!initialWindow
            || !TestHooks::installContextMenuPositionTestHook(applicationWindow,
                                                              workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_CONTEXT_MENU_ACTION")
        == 1) {
        if (!initialWindow
            || !TestHooks::installContextMenuActionTestHook(
                applicationWindow, workspace, &applicationController)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_TOGGLE_FULLSCREEN")
        == 1) {
        if (!initialWindow
            || !TestHooks::installFullscreenActionTestHook(applicationWindow,
                                                           workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_INITIAL_WINDOW_STATE")
        == 1) {
        if (!initialWindow
            || !TestHooks::installInitialWindowStateTestHook(
                applicationWindow, effectiveApplicationOptions)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_INITIAL_WINDOW_SIZE")
        == 1) {
        if (!initialWindow
            || !TestHooks::installInitialWindowSizeTestHook(
                applicationWindow, workspace, effectiveApplicationOptions)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_TOGGLE_MAXIMIZE") == 1) {
        if (!initialWindow
            || !TestHooks::installMaximizeActionTestHook(applicationWindow,
                                                         workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue(
            "GHOSTTY_QT_TEST_TOGGLE_WINDOW_DECORATIONS")
        == 1) {
        if (!initialWindow
            || !TestHooks::installWindowDecorationActionTestHook(
                applicationWindow, workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_TAB_BAR_VISIBILITY")
        == 1) {
        if (!initialWindow
            || !TestHooks::installTabBarVisibilityTestHook(applicationWindow,
                                                           workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_TABS_LOCATION") == 1) {
        if (!initialWindow
            || !TestHooks::installTabsLocationTestHook(applicationWindow,
                                                       workspace)) {
            return 1;
        }
    }
    if (qEnvironmentVariableIntValue("GHOSTTY_QT_TEST_RENDERER_QUALIFICATION")
        == 1) {
        if (!initialWindow
            || !TestHooks::installRendererQualificationTestHook(
                applicationWindow, workspace)) {
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
    if (lifetimeTestMode != TestHooks::ApplicationLifetimeTestMode::None
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
