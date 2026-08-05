#include "application_controller.h"

#include "desktop_activation.h"
#include "desktop_notification_service.h"
#include "ghostty_action_catalog.h"
#include "ghostty_application_ipc.h"
#include "ghostty_application_keybindings.h"
#include "initial_session_coordinator.h"
#include "intentional_crash.h"
#include "quick_terminal_surface.h"
#include "terminal_cell_metrics.h"
#include "terminal_geometry.h"
#include "terminal_workspace.h"
#include "window_blur_controller.h"
#include "window_ui_controller.h"

#include <QCursor>
#include <QDebug>
#include <QGuiApplication>
#include <QInputMethod>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QRunnable>
#include <QScopeGuard>
#include <QScreen>
#include <QTimer>
#include <QVariant>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <ranges>
#include <utility>

namespace {

template <typename... Visitor> struct Overloaded : Visitor... {
    using Visitor::operator()...;
};

constexpr QByteArrayView QuickTerminalEnvironmentKey("GHOSTTY_QUICK_TERMINAL");
constexpr std::chrono::milliseconds QuickTerminalAutohideDelay{50};
constexpr std::chrono::milliseconds WaylandFocusDrainTimeout{500};

bool isWaylandPlatform()
{
    return QGuiApplication::platformName().startsWith(QStringLiteral("wayland"),
                                                      Qt::CaseInsensitive);
}

bool belongsToWindowTree(const QWindow *candidate, const QWindow *root) noexcept
{
    for (const QWindow *window = candidate; window != nullptr;
         window = window->transientParent()) {
        if (window == root) return true;
    }
    return false;
}

void hideWindowTree(QWindow *root)
{
    if (root == nullptr) return;

    const QPointer<QWindow> guardedRoot(root);
    QVector<QPointer<QWindow>> transients;
    for (QWindow *const candidate : QGuiApplication::topLevelWindows()) {
        if (candidate != root && belongsToWindowTree(candidate, root)) {
            transients.append(candidate);
        }
    }
    for (const QPointer<QWindow> &transient : std::as_const(transients)) {
        if (transient != nullptr) transient->hide();
    }
    if (guardedRoot != nullptr) guardedRoot->hide();
}

struct PendingWindowRetirement {
    QMetaObject::Connection focusChanged;
    bool finished = false;
};

bool dispatchIntentionalCrash(QQuickWindow *window,
                              TerminalWorkspace *workspace, PaneId paneId,
                              WorkspaceFrontendActions::CrashTarget target)
{
    using enum WorkspaceFrontendActions::CrashTarget;
    switch (target) {
    case Main: intentionalCrash("main");
    case Io: return workspace != nullptr && workspace->requestIoCrash(paneId);
    case Render:
        if (window == nullptr) return false;
        window->scheduleRenderJob(
            QRunnable::create([] { intentionalCrash("render"); }),
            QQuickWindow::BeforeSynchronizingStage);
        window->update();
        return true;
    }
    return false;
}

std::optional<TerminalCommand>
selectedFirstCommand(const LaunchOptions &options)
{
    return options.initialCommand.has_value() ? options.initialCommand
                                              : options.ordinaryCommand;
}

bool isOwnedBy(const QObject *object, const QObject *owner) noexcept
{
    for (const QObject *candidate = object; candidate != nullptr;
         candidate = candidate->parent()) {
        if (candidate == owner) return true;
    }
    return false;
}

bool isValidWindowPair(const QQuickWindow *window,
                       const TerminalWorkspace *workspace) noexcept
{
    return window != nullptr && workspace != nullptr
        && workspace->window() == window && isOwnedBy(workspace, window);
}

constexpr quint32 minimumWindowColumns = 10;
constexpr quint32 minimumWindowRows = 4;

qreal nonNegativeWindowProperty(const QQuickWindow &window,
                                const char *name) noexcept
{
    bool ok = false;
    const qreal value = window.property(name).toDouble(&ok);
    return ok && std::isfinite(value) && value >= 0.0 ? value : 0.0;
}

int pixelExtent(qreal cellExtent, quint32 cells, qreal chromeExtent) noexcept
{
    const long double pixels =
        static_cast<long double>(cellExtent) * static_cast<long double>(cells)
        + static_cast<long double>(chromeExtent);
    constexpr int maximum = std::numeric_limits<int>::max();
    if (!std::isfinite(pixels) || pixels >= static_cast<long double>(maximum)) {
        return maximum;
    }
    return std::max(1, static_cast<int>(std::ceil(pixels)));
}

QSize windowSizeForGrid(const TerminalCellMetrics &metrics, quint32 columns,
                        quint32 rows, qreal chromeWidth, qreal chromeHeight,
                        const QMarginsF &padding = {}) noexcept
{
    return {
        pixelExtent(metrics.cellWidth, columns,
                    chromeWidth + padding.left() + padding.right()),
        pixelExtent(metrics.cellHeight, rows,
                    chromeHeight + padding.top() + padding.bottom()),
    };
}

struct InitialWindowGeometry {
    QSize minimumSize;
    std::optional<QSize> requestedSize;
};

InitialWindowGeometry initialWindowGeometry(const QQuickWindow &window,
                                            const LaunchOptions &options)
{
    const TerminalCellMetrics metrics =
        terminalCellMetrics(options.typography, window.devicePixelRatio());
    const qreal chromeWidth =
        nonNegativeWindowProperty(window, "terminalChromeWidth");
    const qreal chromeHeight =
        nonNegativeWindowProperty(window, "terminalChromeHeight");
    const QMarginsF padding = terminalExplicitPaddingMargins(
        options.padding, window.devicePixelRatio());
    QSize minimum =
        windowSizeForGrid(metrics, minimumWindowColumns, minimumWindowRows,
                          chromeWidth, chromeHeight, padding);

    QSize available;
    if (const QScreen *const screen = window.screen(); screen != nullptr) {
        available = screen->availableGeometry().size();
        if (available.width() > 0) {
            minimum.setWidth(std::min(minimum.width(), available.width()));
        }
        if (available.height() > 0) {
            minimum.setHeight(std::min(minimum.height(), available.height()));
        }
    }

    // Ghostty treats the dimensions as a pair. A one-sided setting keeps the
    // frontend's ordinary default size, while the minimum grid still follows
    // the actual font used by the first pane.
    if (options.windowWidth == 0 || options.windowHeight == 0) {
        return {
            .minimumSize = minimum,
            .requestedSize = std::nullopt,
        };
    }

    QSize requested = windowSizeForGrid(
        metrics, std::max(options.windowWidth, minimumWindowColumns),
        std::max(options.windowHeight, minimumWindowRows), chromeWidth,
        chromeHeight, padding);
    if (available.width() > 0) {
        requested.setWidth(std::min(requested.width(), available.width()));
    }
    if (available.height() > 0) {
        requested.setHeight(std::min(requested.height(), available.height()));
    }
    return {
        .minimumSize = minimum,
        .requestedSize = requested.expandedTo(minimum),
    };
}

void configureQuickTerminalLaunch(LaunchOptions &options)
{
    options.environment.removeIf([](const TerminalEnvironmentEntry &entry) {
        return entry.key == QuickTerminalEnvironmentKey;
    });
    options.environment.append({
        .key = QByteArray(QuickTerminalEnvironmentKey),
        .value = QByteArrayLiteral("1"),
    });
    // Layer shell and the compositor own this window's state and dimensions.
    // Ordinary new-window startup policy must never turn it into a maximized
    // or fullscreen toplevel.
    options.maximize = false;
    options.fullscreen = false;
}

LaunchOptions terminalRelevantOptions(LaunchOptions options)
{
    // These values are consumed by process/window owners. Normalizing them
    // avoids a key-event barrier and O(panes) reload walk when only application
    // shell presentation or startup-only loader identity changed.
    options.applicationShell = {};
    options.backgroundBlur = 0;
    options.quickTerminalLayerShell = {};
    options.applicationClass.reset();
    options.applicationClassExplicit = false;
    options.configDefaultFiles = true;
    options.configDefaultFilesExplicit = false;
    return options;
}

bool sameTerminalRelevantOptions(const LaunchOptions &left,
                                 const LaunchOptions &right)
{
    return terminalRelevantOptions(left) == terminalRelevantOptions(right);
}

QVector<CommandPaletteEntry>
executableCommandPaletteEntries(const LaunchOptions &options)
{
    QVector<CommandPaletteEntry> entries =
        options.applicationShell.commandPalette;
    entries.removeIf([](const CommandPaletteEntry &entry) {
        return !GhosttyActionCatalog::isImplemented(entry.action);
    });
    return entries;
}

} // namespace

ApplicationController::ApplicationController(QQmlEngine &engine,
                                             LaunchOptions effectiveOptions,
                                             bool enableGlobalShortcutsPortal,
                                             QObject *parent)
    : ApplicationController(std::move(effectiveOptions),
                            qmlWindowFactory(engine),
                            enableGlobalShortcutsPortal, parent)
{}

ApplicationController::ApplicationController(LaunchOptions effectiveOptions,
                                             WindowFactory windowFactory,
                                             bool enableGlobalShortcutsPortal,
                                             QObject *parent)
    : ApplicationController(std::move(effectiveOptions),
                            std::move(windowFactory), dispatchIntentionalCrash,
                            enableGlobalShortcutsPortal, parent)
{}

ApplicationController::ApplicationController(
    LaunchOptions effectiveOptions, WindowFactory windowFactory,
    CrashActionDispatcher crashActionDispatcher,
    bool enableGlobalShortcutsPortal, QObject *parent)
    : QObject(parent)
    , windowFactory_(std::move(windowFactory))
    , crashActionDispatcher_(std::move(crashActionDispatcher))
    , effectiveOptions_(std::move(effectiveOptions))
    , initialSessionCoordinator_(std::make_shared<InitialSessionCoordinator>(
          InitialSessionCoordinator::Payload{
              .program = effectiveOptions_.program,
              .command = selectedFirstCommand(effectiveOptions_),
              .hold = effectiveOptions_.hold,
          }))
    , desktopNotifications_(std::make_unique<DesktopNotificationService>())
    , keybindings_(std::make_unique<GhosttyApplicationKeybindings>(
          effectiveOptions_, enableGlobalShortcutsPortal))
{
    TerminalWorkspace::setDefaultLaunchOptions(
        withoutInitialCommand(effectiveOptions_));
    lifetime_.applyLaunchOptions(effectiveOptions_);

    connect(&lifetime_, &ApplicationLifetimeController::quitRequested, this,
            &ApplicationController::quitRequested);
    connect(
        keybindings_.get(),
        &GhosttyApplicationKeybindings::applicationActionRequested, this,
        [this](ApplicationAction action) { dispatchRequestedAction(action); });
    connect(desktopNotifications_.get(),
            &DesktopNotificationService::activationRequested, this,
            [this](SurfaceTarget target) { (void)presentSurface(target); });
    connect(desktopNotifications_.get(),
            &DesktopNotificationService::warningOccurred, this,
            [](const QString &message) { qWarning().noquote() << message; });
}

ApplicationController::~ApplicationController()
{
    destroying_ = true;
    keybindings_.reset();

    // Start all worker grace periods before QObject destruction waits for any
    // one of them. The QML roots are C++-owned factory results.
    const auto workspaces = workspaceSnapshot();
    for (const QPointer<TerminalWorkspace> &workspace : workspaces) {
        if (workspace != nullptr) {
            workspace->forceShutdownForApplicationQuit();
        }
    }
    QVector<QPointer<QQuickWindow>> roots;
    roots.reserve(static_cast<qsizetype>(windows_.size()));
    for (const WindowRecord &record : std::as_const(windows_)) {
        if (record.window != nullptr) roots.append(record.window);
    }
    for (const QPointer<QQuickWindow> &root : std::as_const(roots)) {
        delete root.data();
    }
    windows_.clear();
}

ApplicationController::WindowFactory
ApplicationController::qmlWindowFactory(QQmlEngine &engine)
{
    auto component = std::make_shared<QQmlComponent>(&engine);
    component->loadFromModule(QStringLiteral("GhosttyQt"),
                              QStringLiteral("Main"));

    return [component = std::move(
                component)]() -> std::expected<ApplicationWindow, QString> {
        if (!component->isReady()) {
            return std::unexpected(component->errorString());
        }

        QObject *const root = component->create();
        if (root == nullptr) {
            return std::unexpected(component->errorString());
        }
        auto *const window = qobject_cast<QQuickWindow *>(root);
        TerminalWorkspace *const workspace =
            root->findChild<TerminalWorkspace *>();
        if (window == nullptr || workspace == nullptr) {
            delete root;
            return std::unexpected(QStringLiteral(
                "Main.qml must create a QQuickWindow containing a TerminalWorkspace"));
        }
        return ApplicationWindow{window, workspace};
    };
}

std::expected<ApplicationWindow, QString>
ApplicationController::createInitialWindow(DesktopActivationContext activation)
{
    if (startupWindowHandled_) {
        return std::unexpected(QStringLiteral(
            "The initial application window was already handled"));
    }

    // createWindow marks the startup decision as soon as its workspace is
    // initialized, including presentation failures. Keeping that transition
    // in one place also lets a creation callback destroy this controller.
    return createWindow(effectiveOptions_, activation);
}

bool ApplicationController::startWithoutInitialWindow()
{
    if (startupWindowHandled_) return false;
    startupWindowHandled_ = true;
    return true;
}

bool ApplicationController::activateNoCommand(
    DesktopActivationContext activation)
{
    if (!startupWindowHandled_) {
        Q_EMIT windowCreationFailed(QStringLiteral(
            "Application activation arrived before the startup window "
            "decision"));
        return false;
    }
    TerminalWorkspace *const source = focusedWorkspace();
    QScreen *const preferredScreen =
        source != nullptr && source->window() != nullptr
        ? source->window()->screen()
        : nullptr;
    const QPointer<ApplicationController> guard(this);
    auto created =
        createWindow(activationWindowOptions(), activation, preferredScreen);
    if (guard == nullptr) return false;
    if (created.has_value()) return true;
    Q_EMIT windowCreationFailed(created.error());
    return false;
}

bool ApplicationController::activateNewWindow(
    GhosttyNewWindowTransportOverrides overrides,
    DesktopActivationContext activation)
{
    if (!startupWindowHandled_) {
        Q_EMIT windowCreationFailed(QStringLiteral(
            "Application action arrived before the startup window decision"));
        return false;
    }

    FirstSurfaceOverrides firstSurface{
        .command = std::move(overrides.command),
        .workingDirectory = std::move(overrides.workingDirectory),
        .titleOverride = std::move(overrides.titleOverride),
    };

    const QPointer<ApplicationController> guard(this);
    auto created = createWindow(effectiveOptions_, activation, nullptr,
                                WindowRole::Normal, &firstSurface);
    if (guard == nullptr) return false;
    if (created.has_value()) return true;
    Q_EMIT windowCreationFailed(created.error());
    return false;
}

bool ApplicationController::activateQuickTerminal(
    DesktopActivationContext activation)
{
    if (!startupWindowHandled_) {
        Q_EMIT windowCreationFailed(QStringLiteral(
            "Application action arrived before the startup window decision"));
        return false;
    }
    return toggleQuickTerminal(activation);
}

std::expected<ApplicationWindow, QString> ApplicationController::createWindow(
    LaunchOptions options, const DesktopActivationContext &activation,
    QScreen *preferredScreen, WindowRole role,
    const FirstSurfaceOverrides *firstSurfaceOverrides)
{
    if (windowCreationInProgress_) {
        return std::unexpected(QStringLiteral(
            "Application window creation is already in progress"));
    }
    if (!windowFactory_) {
        return std::unexpected(
            QStringLiteral("No window factory is available"));
    }
    if (lifetime_.hasRequestedQuit() || quitState_ != QuitState::Idle) {
        return std::unexpected(
            QStringLiteral("The application is already quitting"));
    }
    if (role == WindowRole::QuickTerminal) {
        configureQuickTerminalLaunch(options);
        preferredScreen = quickTerminalSizingScreen();
        if (preferredScreen == nullptr) {
            return std::unexpected(QStringLiteral(
                "No screen is available for the quick terminal"));
        }
    }
    // Keep the request's options and immutable matcher paired. A live reload
    // during window construction is caught up after registration, before the
    // window is presented.
    const GhosttyKeybindProgram requestedKeybindProgram =
        keybindings_->keybindProgram();
    const RevisionCounter::Value requestedOptionsRevision =
        launchOptionsRevision_.current();

    const QPointer<ApplicationController> controllerGuard(this);
    windowCreationInProgress_ = true;
    const auto creationGuard = qScopeGuard([controllerGuard] {
        if (controllerGuard != nullptr) {
            controllerGuard->windowCreationInProgress_ = false;
        }
    });

    // Keep the callable alive independently of this object while it runs. A
    // custom factory is allowed to synchronously delete its controller.
    WindowFactory invocationFactory = std::move(windowFactory_);
    const auto factoryGuard =
        qScopeGuard([controllerGuard, &invocationFactory] {
            if (controllerGuard != nullptr) {
                controllerGuard->windowFactory_ = std::move(invocationFactory);
            }
        });

    std::expected<ApplicationWindow, QString> created = invocationFactory();
    QPointer<QQuickWindow> guardedWindow(created.has_value() ? created->window
                                                             : nullptr);
    QPointer<TerminalWorkspace> guardedWorkspace(
        created.has_value() ? created->workspace : nullptr);
    const auto pairIsValid = [&] {
        return controllerGuard != nullptr
            && isValidWindowPair(guardedWindow.data(), guardedWorkspace.data());
    };
    const auto discardCreated = [&] {
        if (guardedWindow != nullptr) delete guardedWindow.data();
        if (guardedWorkspace != nullptr) delete guardedWorkspace.data();
        if (created.has_value()) *created = {};
    };
    if (controllerGuard == nullptr) {
        discardCreated();
        return std::unexpected(QStringLiteral(
            "The application controller was destroyed by its window factory"));
    }
    if (!created.has_value()) return created;
    if (created->window == nullptr || created->workspace == nullptr) {
        discardCreated();
        return std::unexpected(
            QStringLiteral("The window factory returned an incomplete window"));
    }
    if (created->workspace->window() != created->window) {
        discardCreated();
        return std::unexpected(
            QStringLiteral("The window factory returned an unowned workspace"));
    }
    if (!isOwnedBy(created->workspace, created->window)) {
        discardCreated();
        return std::unexpected(QStringLiteral(
            "The window factory returned a workspace outside the "
            "window's QObject ownership tree"));
    }

    guardedWindow->setProperty("quickTerminal",
                               role == WindowRole::QuickTerminal);
    if (!pairIsValid()) {
        discardCreated();
        return std::unexpected(QStringLiteral(
            "The application window became invalid while selecting its role"));
    }

    if (preferredScreen != nullptr
        && guardedWindow->screen() != preferredScreen) {
        guardedWindow->setScreen(preferredScreen);
        if (!pairIsValid()) {
            discardCreated();
            return std::unexpected(QStringLiteral(
                "The application window became invalid while selecting "
                "its initial screen"));
        }
    }

    std::unique_ptr<QuickTerminalSurface> quickTerminalSurface;
    if (role == WindowRole::QuickTerminal) {
        auto attached = QuickTerminalSurface::create(
            *guardedWindow, options.applicationShell.quickTerminal,
            options.quickTerminalLayerShell, *preferredScreen);
        if (!attached.has_value()) {
            const QString error = std::move(attached.error());
            discardCreated();
            return std::unexpected(error);
        }
        quickTerminalSurface = std::move(*attached);
    } else {
        const InitialWindowGeometry geometry =
            initialWindowGeometry(*guardedWindow, options);
        guardedWindow->setMinimumSize(geometry.minimumSize);
        if (!pairIsValid()) {
            discardCreated();
            return std::unexpected(QStringLiteral(
                "The application window became invalid while configuring "
                "its initial geometry"));
        }
        if (geometry.requestedSize.has_value()) {
            guardedWindow->resize(*geometry.requestedSize);
            if (!pairIsValid()) {
                discardCreated();
                return std::unexpected(QStringLiteral(
                    "The application window became invalid while configuring "
                    "its initial geometry"));
            }
        }
    }

    const TerminalSessionStartMode initialSessionStartMode =
        role == WindowRole::QuickTerminal || options.maximize
            || options.fullscreen
        ? TerminalSessionStartMode::Deferred
        : TerminalSessionStartMode::Immediate;
    const bool initialized = guardedWorkspace->initialize(
        withoutInitialCommand(options), initialSessionStartMode,
        initialSessionCoordinator_, requestedKeybindProgram,
        firstSurfaceOverrides != nullptr ? *firstSurfaceOverrides
                                         : FirstSurfaceOverrides{});
    // The initial-window request and first successful session initialization
    // are independent one-shot decisions. Reaching workspace initialization
    // handles startup even if presentation later destroys the GUI pair.
    if (controllerGuard != nullptr && initialized
        && !controllerGuard->startupWindowHandled_) {
        controllerGuard->startupWindowHandled_ = true;
    }
    if (!pairIsValid()) {
        discardCreated();
        return std::unexpected(QStringLiteral(
            "The application window became invalid during workspace "
            "initialization"));
    }
    if (!initialized) {
        discardCreated();
        return std::unexpected(
            QStringLiteral("The window workspace was already initialized"));
    }
    if (!lifetime_.registerWindow(guardedWindow.data())) {
        discardCreated();
        return std::unexpected(QStringLiteral(
            "Could not register the primary application window"));
    }

    registerWindow({guardedWindow.data(), guardedWorkspace.data()}, role,
                   std::move(quickTerminalSurface));
    if (!pairIsValid()) {
        discardCreated();
        return std::unexpected(QStringLiteral(
            "The application window became invalid during registration"));
    }

    if (requestedOptionsRevision != launchOptionsRevision_.current()) {
        const GhosttyKeybindProgram currentProgram =
            keybindings_->keybindProgram();
        LaunchOptions currentOptions = withoutInitialCommand(effectiveOptions_);
        if (role == WindowRole::QuickTerminal) {
            configureQuickTerminalLaunch(currentOptions);
        }
        guardedWorkspace->applyLaunchOptions(currentOptions, currentProgram);
        if (!pairIsValid()) {
            discardCreated();
            return std::unexpected(QStringLiteral(
                "The application window became invalid while applying "
                "updated configuration"));
        }
    }
    if (role == WindowRole::QuickTerminal) {
        WindowRecord *const record = recordForWindow(guardedWindow);
        if (record == nullptr) {
            discardCreated();
            return std::unexpected(QStringLiteral(
                "The quick-terminal window was retired during registration"));
        }
        if (auto synchronized = syncQuickTerminal(*record); !synchronized) {
            const QString error = synchronized.error();
            discardCreated();
            return std::unexpected(error);
        }
        if (!pairIsValid()) {
            discardCreated();
            return std::unexpected(QStringLiteral(
                "The quick-terminal window became invalid while applying "
                "current options"));
        }
    }

    // Fullscreen takes precedence at presentation time; the QML root retains
    // the maximized state separately so leaving an initially fullscreen and
    // maximized window restores it to maximized. A window first mapped in a
    // non-windowed state also needs its hidden normal size retained until Qt
    // has completed the first transition back to Windowed.
    const QSize initialNormalSize = guardedWindow->size();
    const auto setInitialProperty = [&](const char *name,
                                        const QVariant &value) {
        if (!pairIsValid()) return false;
        guardedWindow->setProperty(name, value);
        return pairIsValid();
    };
    if (!setInitialProperty("initialNormalSize", initialNormalSize)
        || !setInitialProperty("initialNormalSizePending",
                               options.maximize || options.fullscreen)
        || !setInitialProperty("visibilityBeforeFullscreen",
                               static_cast<int>(options.maximize
                                                    ? QWindow::Maximized
                                                    : QWindow::Windowed))) {
        discardCreated();
        return std::unexpected(QStringLiteral(
            "The application window became invalid while configuring "
            "its initial state"));
    }

    const WindowPresentationMode presentationMode = options.fullscreen
        ? WindowPresentationMode::Fullscreen
        : (options.maximize ? WindowPresentationMode::Maximized
                            : WindowPresentationMode::Windowed);
    showWindowWithActivation(*guardedWindow, activation, presentationMode);
    if (!pairIsValid()) {
        discardCreated();
        return std::unexpected(QStringLiteral(
            "The application window became invalid while being shown"));
    }
    if (initialSessionStartMode == TerminalSessionStartMode::Deferred
        && !guardedWorkspace->armInitialSessionStart()) {
        discardCreated();
        return std::unexpected(QStringLiteral(
            "Could not arm the initial terminal session after window "
            "presentation"));
    }
    if (!pairIsValid()) {
        discardCreated();
        return std::unexpected(QStringLiteral(
            "The application window became invalid while arming its "
            "initial terminal session"));
    }
    if (activation.isEmpty()
        && (role != WindowRole::QuickTerminal
            || options.applicationShell.quickTerminal.keyboardInteractivity
                != QuickTerminalKeyboardInteractivity::None)) {
        guardedWindow->requestActivate();
        if (!pairIsValid()) {
            discardCreated();
            return std::unexpected(QStringLiteral(
                "The application window became invalid during activation"));
        }
    }
    if (role == WindowRole::QuickTerminal) {
        if (WindowRecord *const record =
                recordForWindow(guardedWindow.data())) {
            updateQuickTerminalAutohide(*record);
        }
        if (!pairIsValid()) {
            discardCreated();
            return std::unexpected(QStringLiteral(
                "The quick-terminal window became invalid during activation "
                "acknowledgement"));
        }
    }
    Q_EMIT windowCreated(guardedWindow.data(), guardedWorkspace.data());
    if (!pairIsValid()) {
        discardCreated();
        return std::unexpected(QStringLiteral(
            "The application window became invalid in its creation "
            "observer"));
    }
    return ApplicationWindow{
        guardedWindow.data(),
        guardedWorkspace.data(),
    };
}

QScreen *ApplicationController::quickTerminalSizingScreen() const
{
    if (effectiveOptions_.applicationShell.quickTerminal.screen
        == QuickTerminalScreen::Mouse) {
        if (QScreen *const mouseScreen =
                QGuiApplication::screenAt(QCursor::pos())) {
            return mouseScreen;
        }
    }
    return QGuiApplication::primaryScreen();
}

bool ApplicationController::toggleQuickTerminal(
    const DesktopActivationContext &activation)
{
    const QPointer<ApplicationController> guard(this);
    if (WindowRecord *record = quickTerminalRecord()) {
        const QPointer<QQuickWindow> window(record->window);
        const QPointer<TerminalWorkspace> workspace(record->workspace);
        if (window->isVisible()) {
            record->quickTerminalActivationAcknowledged = false;
            if (record->quickTerminalAutohideTimer != nullptr) {
                record->quickTerminalAutohideTimer->stop();
            }
            window->hide();
            if (guard != nullptr) {
                if (WindowRecord *const current =
                        recordForWindow(window.data())) {
                    updateQuickTerminalAutohide(*current);
                }
            }
            return true;
        }

        record->quickTerminalActivationAcknowledged = false;
        if (record->quickTerminalAutohideTimer != nullptr) {
            record->quickTerminalAutohideTimer->stop();
        }
        if (auto synchronized = syncQuickTerminal(*record); !synchronized) {
            if (guard == nullptr) return false;
            Q_EMIT windowCreationFailed(synchronized.error());
            return false;
        }
        if (guard == nullptr) return true;
        record = recordForWindow(window.data());
        if (record == nullptr || workspace == nullptr
            || record->workspace != workspace) {
            return false;
        }
        showWindowWithActivation(*window, activation,
                                 WindowPresentationMode::Windowed);
        if (guard == nullptr || window == nullptr || workspace == nullptr) {
            return true;
        }
        if (effectiveOptions_.applicationShell.quickTerminal
                .keyboardInteractivity
            != QuickTerminalKeyboardInteractivity::None) {
            if (activation.isEmpty()) window->requestActivate();
            if (guard == nullptr || window == nullptr || workspace == nullptr) {
                return true;
            }
            (void)workspace->focusActivePane();
        }
        if (guard != nullptr) {
            if (WindowRecord *const current = recordForWindow(window.data())) {
                updateQuickTerminalAutohide(*current);
            }
        }
        return true;
    }

    auto created = createWindow(effectiveOptions_, activation, nullptr,
                                WindowRole::QuickTerminal);
    if (guard == nullptr) return false;
    if (!created.has_value()) {
        Q_EMIT windowCreationFailed(created.error());
        return false;
    }
    return true;
}

std::expected<void, QString>
ApplicationController::syncQuickTerminal(WindowRecord &record)
{
    if (record.role != WindowRole::QuickTerminal || record.window == nullptr
        || record.workspace == nullptr
        || record.quickTerminalSurface == nullptr) {
        return std::unexpected(
            QStringLiteral("The quick-terminal window is incomplete"));
    }
    QScreen *const screen = quickTerminalSizingScreen();
    if (screen == nullptr) {
        return std::unexpected(
            QStringLiteral("No screen is available for the quick terminal"));
    }
    const WindowId windowId = record.id;
    const QPointer<QuickTerminalSurface> surface(
        record.quickTerminalSurface.get());
    const QuickTerminalOptions options =
        effectiveOptions_.applicationShell.quickTerminal;
    const QuickTerminalLayerShellOptions layerShellOptions =
        effectiveOptions_.quickTerminalLayerShell;
    const QPointer<ApplicationController> guard(this);
    auto synchronized =
        surface->syncOptions(options, layerShellOptions, *screen);
    if (guard == nullptr) return synchronized;
    if (!synchronized) return synchronized;
    WindowRecord *const current = recordForWindowId(windowId);
    if (current == nullptr || current->quickTerminalSurface.get() != surface) {
        return std::unexpected(QStringLiteral(
            "The quick-terminal window was retired while applying options"));
    }
    updateQuickTerminalAutohide(*current);
    return {};
}

void ApplicationController::updateQuickTerminalAutohide(WindowRecord &record)
{
    QTimer *const timer = record.quickTerminalAutohideTimer;
    QQuickWindow *const window = record.window;
    if (timer == nullptr || window == nullptr) return;

    if (window->isActive()) {
        record.quickTerminalActivationAcknowledged = true;
    }
    const bool shouldWait =
        effectiveOptions_.applicationShell.quickTerminal.autohide
        && record.quickTerminalActivationAcknowledged && window->isVisible()
        && !window->isActive();
    if (shouldWait) {
        if (!timer->isActive()) timer->start();
    } else {
        timer->stop();
    }
}

bool ApplicationController::dispatch(ApplicationAction action,
                                     TerminalWorkspace *sourceWorkspace,
                                     PaneId sourcePaneId)
{
    switch (action) {
    case ApplicationAction::Ignore: return true;
    case ApplicationAction::DeprecatedCloseAllWindows:
        // Pinned GTK consumes this deprecated Linux action without mutating
        // any window. Keep it distinct from Ignore, whose input semantics are
        // intentionally special.
        return true;
    case ApplicationAction::OpenConfig:
        Q_EMIT configOpenRequested();
        return true;
    case ApplicationAction::ReloadConfig:
        Q_EMIT configReloadRequested();
        return true;
    case ApplicationAction::ToggleQuickTerminal: return toggleQuickTerminal();
    case ApplicationAction::NewWindow: {
        if (quitState_ != QuitState::Idle || lifetime_.hasRequestedQuit()) {
            return false;
        }
        const QPointer<TerminalWorkspace> guardedSource(sourceWorkspace);
        QTimer::singleShot(0, this, [this, guardedSource, sourcePaneId] {
            if (quitState_ != QuitState::Idle || lifetime_.hasRequestedQuit()) {
                return;
            }
            TerminalWorkspace *screenSource = guardedSource;
            if (!containsWorkspace(screenSource)) {
                screenSource = activeWorkspace();
            }
            QScreen *const preferredScreen =
                screenSource != nullptr && screenSource->window() != nullptr
                ? screenSource->window()->screen()
                : nullptr;
            const QPointer<ApplicationController> guard(this);
            auto created =
                createWindow(nextWindowOptions(guardedSource, sourcePaneId), {},
                             preferredScreen);
            if (guard != nullptr && !created.has_value()) {
                Q_EMIT windowCreationFailed(created.error());
            }
        });
        return true;
    }
    case ApplicationAction::Quit: requestApplicationQuit(); return true;
    }
    return false;
}

bool ApplicationController::dispatch(WindowNavigationAction action)
{
    if (windows_.empty()) return false;

    std::optional<std::size_t> activeIndex;
    for (std::size_t index = 0; index < windows_.size(); ++index) {
        const WindowRecord &record = windows_[index];
        if (!record.retiring && record.window != nullptr
            && record.window->isActive()) {
            activeIndex = index;
            break;
        }
    }

    // GTK starts at the active top-level when one exists, otherwise at the
    // first top-level. It tests that starting node before advancing, then
    // wraps once. Registration order is the stable Qt equivalent of that
    // toolkit-owned list.
    std::size_t index = activeIndex.value_or(0);
    for (std::size_t visited = 0; visited < windows_.size(); ++visited) {
        const QPointer<QQuickWindow> window = windows_[index].window;
        const QPointer<TerminalWorkspace> workspace = windows_[index].workspace;
        if (!windows_[index].retiring
            && isValidWindowPair(window.data(), workspace.data())
            && window->isVisible() && !window->isActive()
            && workspace->canHostApplicationQuitConfirmation()
            && workspace->hasActivePane()) {
            const QPointer<ApplicationController> controllerGuard(this);
            const auto destinationRemainsEligible = [&] {
                return controllerGuard != nullptr && window != nullptr
                    && workspace != nullptr
                    && isValidWindowPair(window.data(), workspace.data())
                    && controllerGuard->containsWorkspace(workspace.data())
                    && window->isVisible()
                    && workspace->canHostApplicationQuitConfirmation()
                    && workspace->hasActivePane();
            };

            // gtk_window_present deiconifies before requesting focus. Preserve
            // any simultaneous maximized/fullscreen state while doing the Qt
            // equivalent for a minimized destination.
            if (window->windowStates().testFlag(Qt::WindowMinimized)) {
                window->setWindowStates(window->windowStates()
                                        & ~Qt::WindowMinimized);
                if (!destinationRemainsEligible()) return true;
            }

            window->requestActivate();
            if (!destinationRemainsEligible()) return true;
            (void)workspace->focusActivePane();
            return true;
        }

        if (action == WindowNavigationAction::Next) {
            index = (index + 1) % windows_.size();
        } else {
            index = (index + windows_.size() - 1) % windows_.size();
        }
    }
    return false;
}

void ApplicationController::dispatchRequestedAction(
    ApplicationAction action, TerminalWorkspace *sourceWorkspace,
    PaneId sourcePaneId)
{
    // New-window already posts its actual creation from dispatch(). Posting a
    // wrapper here as well would let a later queued quit overtake it. Ignore
    // is inert, so both actions can enter dispatch immediately.
    switch (action) {
    case ApplicationAction::Ignore:
    case ApplicationAction::DeprecatedCloseAllWindows:
    case ApplicationAction::NewWindow:
        (void)dispatch(action, sourceWorkspace, sourcePaneId);
        return;
    case ApplicationAction::OpenConfig:
    case ApplicationAction::ReloadConfig:
    case ApplicationAction::ToggleQuickTerminal:
    case ApplicationAction::Quit: break;
    }

    // External config callbacks and quit may synchronously destroy their
    // source workspace. The process owner outlives every pane, so publish
    // these application actions only after the complete keybinding chain and
    // originating key event have unwound.
    const QPointer<TerminalWorkspace> guardedSource(sourceWorkspace);
    QTimer::singleShot(0, this, [this, action, guardedSource, sourcePaneId] {
        (void)dispatch(action, guardedSource.data(), sourcePaneId);
    });
}

LaunchOptions
ApplicationController::nextWindowOptions(TerminalWorkspace *sourceWorkspace,
                                         PaneId sourcePaneId) const
{
    TerminalWorkspace *const fallback = activeWorkspace();
    TerminalWorkspace *source = sourceWorkspace;
    if (!containsWorkspace(source)) {
        source = fallback;
        sourcePaneId = {};
    }

    if (source != nullptr) {
        if (const std::optional<LaunchOptions> inherited =
                source->newWindowLaunchOptions(effectiveOptions_,
                                               sourcePaneId)) {
            return *inherited;
        }
        if (source != fallback && fallback != nullptr) {
            if (const std::optional<LaunchOptions> inherited =
                    fallback->newWindowLaunchOptions(effectiveOptions_)) {
                return *inherited;
            }
        }
    }

    return effectiveOptions_;
}

LaunchOptions ApplicationController::activationWindowOptions() const
{
    LaunchOptions result = effectiveOptions_;
    TerminalWorkspace *const source = focusedWorkspace();
    if (source != nullptr && result.windowInheritWorkingDirectory) {
        // A normal Ghostty GApplication activation has no parent surface.
        // Its GTK surface setup still overlays the globally focused
        // surface's cwd, but parent-only font inheritance does not run.
        LaunchOptions directoryProbe = result;
        directoryProbe.windowInheritFontSize = false;
        if (const auto inherited =
                source->newWindowLaunchOptions(directoryProbe)) {
            result.workingDirectory = inherited->workingDirectory;
            result.inheritWorkingDirectory = inherited->inheritWorkingDirectory;
        }
    }
    return result;
}

void ApplicationController::applyLaunchOptions(const LaunchOptions &options)
{
    const bool terminalOptionsChanged =
        !sameTerminalRelevantOptions(effectiveOptions_, options);
    const RevisionCounter::Value revision = launchOptionsRevision_.advance();
    const QPointer<ApplicationController> guard(this);
    const QPointer<GhosttyApplicationKeybindings> keybindingsGuard(
        keybindings_.get());
    if (terminalOptionsChanged) keybindings_->beginConfigurationUpdate();
    const auto keybindingUpdateGuard =
        qScopeGuard([keybindingsGuard, terminalOptionsChanged] {
            if (terminalOptionsChanged && keybindingsGuard != nullptr) {
                keybindingsGuard->endConfigurationUpdate();
            }
        });
    const auto stillCurrentRevision = [&guard, revision] {
        return guard != nullptr
            && guard->launchOptionsRevision_.isCurrent(revision);
    };

    (void)initialSessionCoordinator_->updatePayload({
        .program = options.program,
        .command = selectedFirstCommand(options),
        .hold = options.hold,
    });
    if (!stillCurrentRevision()) return;

    effectiveOptions_ = options;
    syncWindowBlur();
    if (!stillCurrentRevision()) return;
    const LaunchOptions ordinaryOptions =
        withoutInitialCommand(effectiveOptions_);
    lifetime_.applyLaunchOptions(effectiveOptions_);
    if (!stillCurrentRevision()) return;
    syncApplicationShell();
    if (!stillCurrentRevision() || !terminalOptionsChanged) return;

    const GhosttyKeybindProgram keybindProgram =
        keybindings_->applyLaunchOptions(effectiveOptions_);
    if (!stillCurrentRevision()
        || !keybindings_->keybindProgram().isSameGeneration(keybindProgram)) {
        return;
    }

    TerminalWorkspace::setDefaultLaunchOptions(ordinaryOptions);
    const auto stillCurrentUpdate = [guard, revision, keybindProgram] {
        return guard != nullptr
            && guard->launchOptionsRevision_.isCurrent(revision)
            && guard->keybindProgram().isSameGeneration(keybindProgram);
    };
    for (const ApplicationWindow &window : windows()) {
        if (window.workspace != nullptr) {
            LaunchOptions workspaceOptions = ordinaryOptions;
            if (const WindowRecord *const record =
                    recordForWorkspace(window.workspace);
                record != nullptr
                && record->role == WindowRole::QuickTerminal) {
                configureQuickTerminalLaunch(workspaceOptions);
            }
            window.workspace->applyLaunchOptions(workspaceOptions,
                                                 keybindProgram);
            if (!stillCurrentUpdate()) return;
        }
    }
}

void ApplicationController::syncWindowBlur()
{
    for (WindowRecord &record : windows_) {
        if (record.blur != nullptr) {
            record.blur->setBlur(effectiveOptions_.backgroundBlur);
        }
    }
}

void ApplicationController::syncApplicationShell()
{
    const QVector<ApplicationWindow> snapshot = windows();
    const QVector<CommandPaletteEntry> palette =
        executableCommandPaletteEntries(effectiveOptions_);
    const QPointer<ApplicationController> guard(this);
    for (const ApplicationWindow &window : snapshot) {
        WindowRecord *record = recordForWindow(window.window);
        if (record == nullptr) continue;

        if (record->ui != nullptr) {
            if (record->ui->commandPaletteVisible()) {
                const WindowId windowId = record->id;
                refreshCommandPalette(windowId);
            } else {
                record->ui->replaceCommandPaletteEntries(palette);
            }
            if (guard == nullptr) return;
            record = recordForWindow(window.window);
            if (record == nullptr) continue;
        }

        if (record->role == WindowRole::QuickTerminal) {
            auto synchronized = syncQuickTerminal(*record);
            if (guard == nullptr) return;
            if (!synchronized) {
                const QString error = synchronized.error();
                Q_EMIT windowCreationFailed(error);
                if (guard == nullptr) return;
            }
        }
    }
}

void ApplicationController::refreshCommandPalette(WindowId sourceWindowId)
{
    WindowRecord *source = recordForWindowId(sourceWindowId);
    if (source == nullptr || source->ui == nullptr) return;

    QVector<CommandPaletteEntry> configured =
        executableCommandPaletteEntries(effectiveOptions_);
    QVector<CommandPaletteRow> rows;
    rows.reserve(configured.size());
    for (CommandPaletteEntry &entry : configured) {
        rows.append({
            .title = std::move(entry.title),
            .description = std::move(entry.description),
            .actionKey = std::move(entry.actionKey),
            .command = std::move(entry.action),
        });
    }

    for (const WindowRecord &record : std::as_const(windows_)) {
        if (record.window == nullptr || record.workspace == nullptr) continue;
        const QVector<WorkspaceSurfaceSnapshot> surfaces =
            record.workspace->surfaceSnapshot();
        rows.reserve(rows.size() + surfaces.size());
        for (const WorkspaceSurfaceSnapshot &surface : surfaces) {
            const QString effective =
                surface.effectiveTitle.value_or(QStringLiteral("Untitled"));
            rows.append({
                .title = QStringLiteral("Focus: ") + effective,
                .description = !surface.currentDirectory.isEmpty()
                        && !effective.contains(surface.currentDirectory,
                                               Qt::CaseSensitive)
                    ? surface.currentDirectory
                    : QString{},
                .actionKey = {},
                .command =
                    SurfaceTarget{
                        .windowId = record.id,
                        .paneId = surface.paneId,
                    },
            });
        }
    }

    // Sampling titles/cwds is synchronous but may invoke user-observable Qt
    // accessors in future implementations. Re-resolve the source before
    // publishing the new model.
    source = recordForWindowId(sourceWindowId);
    if (source != nullptr && source->ui != nullptr) {
        source->ui->replaceCommandPaletteRows(std::move(rows));
    }
}

bool ApplicationController::executePaletteAction(WindowId sourceWindowId,
                                                 const QString &action)
{
    WindowRecord *const source = recordForWindowId(sourceWindowId);
    return source != nullptr && source->workspace != nullptr
        && source->workspace->executeActiveConfiguredAction(action);
}

bool ApplicationController::presentSurface(SurfaceTarget target)
{
    if (!target.isValid()) return false;
    const QPointer<ApplicationController> guard(this);
    WindowRecord *record = recordForWindowId(target.windowId);
    if (record == nullptr || record->window == nullptr
        || record->workspace == nullptr
        || !record->workspace->containsPane(target.paneId)) {
        return false;
    }

    const QPointer<QQuickWindow> window(record->window);
    const QPointer<TerminalWorkspace> workspace(record->workspace);
    const bool initiallyFocused =
        workspace->focusPaneForFrontend(target.paneId);
    if (guard == nullptr) return initiallyFocused;
    if (!initiallyFocused) return false;

    record = recordForWindowId(target.windowId);
    if (record == nullptr || record->window != window
        || record->workspace != workspace || window == nullptr
        || workspace == nullptr || !workspace->containsPane(target.paneId)) {
        return false;
    }

    if (record->role == WindowRole::QuickTerminal) {
        if (!window->isVisible()) {
            record->quickTerminalActivationAcknowledged = false;
            if (record->quickTerminalAutohideTimer != nullptr) {
                record->quickTerminalAutohideTimer->stop();
            }
            auto synchronized = syncQuickTerminal(*record);
            if (guard == nullptr) return synchronized.has_value();
            if (!synchronized) {
                Q_EMIT windowCreationFailed(synchronized.error());
                return false;
            }
            record = recordForWindowId(target.windowId);
            if (record == nullptr || record->window != window
                || record->workspace != workspace || window == nullptr
                || workspace == nullptr) {
                return false;
            }
            showWindowWithActivation(*window, {},
                                     WindowPresentationMode::Windowed);
            if (guard == nullptr) return true;
        }

        if (window == nullptr || workspace == nullptr) return true;
        if (effectiveOptions_.applicationShell.quickTerminal
                .keyboardInteractivity
            != QuickTerminalKeyboardInteractivity::None) {
            window->requestActivate();
            if (guard == nullptr || window == nullptr || workspace == nullptr) {
                return true;
            }
            const bool refocused =
                workspace->focusPaneForFrontend(target.paneId);
            if (guard == nullptr) return refocused;
        }
        if (WindowRecord *const current = recordForWindowId(target.windowId)) {
            updateQuickTerminalAutohide(*current);
        }
        return true;
    }

    if (window->windowStates().testFlag(Qt::WindowMinimized)) {
        window->setWindowStates(window->windowStates() & ~Qt::WindowMinimized);
        if (guard == nullptr || window == nullptr || workspace == nullptr) {
            return true;
        }
    }
    if (!window->isVisible()) {
        window->show();
        if (guard == nullptr || window == nullptr || workspace == nullptr) {
            return true;
        }
    }
    window->requestActivate();
    if (guard == nullptr || window == nullptr || workspace == nullptr) {
        return true;
    }
    const bool refocused = workspace->focusPaneForFrontend(target.paneId);
    if (guard == nullptr) return refocused;
    return true;
}

void ApplicationController::notifyConfigurationReloaded()
{
    if (!effectiveOptions_.applicationShell.notifications.configReload) return;

    const QVector<ApplicationWindow> snapshot = windows();
    const QPointer<ApplicationController> guard(this);
    for (const ApplicationWindow &window : snapshot) {
        if (WindowRecord *const record = recordForWindow(window.window);
            record != nullptr && record->ui != nullptr) {
            record->ui->notifyConfigurationReloaded();
            if (guard == nullptr) return;
        }
    }
}

void ApplicationController::reportConfigurationFailure(
    ConfigurationSource source, const QString &message)
{
    QString *failure = nullptr;
    switch (source) {
    case ConfigurationSource::Ghostty:
        failure = &ghosttyConfigurationFailure_;
        break;
    case ConfigurationSource::Frontend:
        failure = &frontendConfigurationFailure_;
        break;
    }

    const QString next = message.isEmpty()
        ? QStringLiteral("Configuration reload failed")
        : message;
    if (*failure == next) {
        return;
    }
    *failure = next;
    syncConfigurationDiagnostics();
}

void ApplicationController::clearConfigurationFailure(
    ConfigurationSource source)
{
    QString *failure = nullptr;
    switch (source) {
    case ConfigurationSource::Ghostty:
        failure = &ghosttyConfigurationFailure_;
        break;
    case ConfigurationSource::Frontend:
        failure = &frontendConfigurationFailure_;
        break;
    }

    if (failure->isEmpty()) {
        return;
    }
    failure->clear();
    syncConfigurationDiagnostics();
}

QString ApplicationController::configurationDiagnosticsText() const
{
    QStringList sections;
    if (!ghosttyConfigurationFailure_.isEmpty()) {
        sections.append(QStringLiteral("Ghostty configuration:\n%1")
                            .arg(ghosttyConfigurationFailure_));
    }
    if (!frontendConfigurationFailure_.isEmpty()) {
        sections.append(QStringLiteral("ghostty-qt frontend configuration:\n%1")
                            .arg(frontendConfigurationFailure_));
    }
    return sections.join(QStringLiteral("\n\n"));
}

namespace {

QString diagnosticsForWorkspace(QString configuration,
                                const TerminalWorkspace *workspace)
{
    if (workspace == nullptr) {
        return configuration;
    }
    const QString shaderDiagnostics = workspace->customShaderDiagnostics();
    if (shaderDiagnostics.isEmpty()) {
        return configuration;
    }
    const QString section =
        QStringLiteral("Custom shaders:\n") + shaderDiagnostics;
    return configuration.isEmpty()
        ? section
        : configuration + QStringLiteral("\n\n") + section;
}

} // namespace

void ApplicationController::syncConfigurationDiagnostics()
{
    const QString diagnostics = configurationDiagnosticsText();
    const QVector<ApplicationWindow> snapshot = windows();
    const QPointer<ApplicationController> guard(this);
    for (const ApplicationWindow &window : snapshot) {
        WindowRecord *const record = recordForWindow(window.window);
        if (record == nullptr || record->ui == nullptr) continue;
        record->ui->setConfigurationDiagnostics(
            diagnosticsForWorkspace(diagnostics, record->workspace));
        if (guard == nullptr) return;
    }
}

TerminalWorkspace *ApplicationController::activeWorkspace() const
{
    if (TerminalWorkspace *const focused = focusedWorkspace()) {
        return focused;
    }
    for (auto record = windows_.crbegin(); record != windows_.crend();
         ++record) {
        if (!record->retiring && record->window != nullptr
            && record->workspace != nullptr) {
            return record->workspace;
        }
    }
    return nullptr;
}

TerminalWorkspace *ApplicationController::focusedWorkspace() const
{
    QWindow *const focusWindow = QGuiApplication::focusWindow();
    if (focusWindow != nullptr) {
        for (const WindowRecord &record : windows_) {
            if (!record.retiring && record.window == focusWindow
                && record.workspace != nullptr) {
                return record.workspace;
            }
        }
    }
    if (containsWorkspace(lastActiveWorkspace_)) {
        return lastActiveWorkspace_;
    }
    return nullptr;
}

int ApplicationController::windowCount() const
{
    return static_cast<int>(
        std::ranges::count_if(windows_, [](const WindowRecord &record) {
            return record.window != nullptr && record.workspace != nullptr;
        }));
}

QVector<ApplicationWindow> ApplicationController::windows() const
{
    QVector<ApplicationWindow> result;
    result.reserve(static_cast<qsizetype>(windows_.size()));
    for (const WindowRecord &record : windows_) {
        if (record.window != nullptr && record.workspace != nullptr) {
            result.append({record.window, record.workspace});
        }
    }
    return result;
}

ApplicationController::WindowRecord *
ApplicationController::recordForWindow(QQuickWindow *window)
{
    if (window == nullptr) return nullptr;
    const auto record =
        std::ranges::find_if(windows_, [window](const WindowRecord &candidate) {
            return candidate.window.data() == window;
        });
    return record == windows_.end() ? nullptr : std::addressof(*record);
}

const ApplicationController::WindowRecord *
ApplicationController::recordForWindow(QQuickWindow *window) const
{
    if (window == nullptr) return nullptr;
    const auto record =
        std::ranges::find_if(windows_, [window](const WindowRecord &candidate) {
            return candidate.window.data() == window;
        });
    return record == windows_.end() ? nullptr : std::addressof(*record);
}

ApplicationController::WindowRecord *
ApplicationController::recordForWorkspace(TerminalWorkspace *workspace)
{
    if (workspace == nullptr) return nullptr;
    const auto record = std::ranges::find_if(
        windows_, [workspace](const WindowRecord &candidate) {
            return !candidate.retiring
                && candidate.workspace.data() == workspace;
        });
    return record == windows_.end() ? nullptr : std::addressof(*record);
}

ApplicationController::WindowRecord *
ApplicationController::recordForWindowId(WindowId id)
{
    if (!id.isValid()) return nullptr;
    const auto record =
        std::ranges::find_if(windows_, [id](const WindowRecord &candidate) {
            return !candidate.retiring && candidate.id == id;
        });
    return record == windows_.end() ? nullptr : std::addressof(*record);
}

ApplicationController::WindowRecord *
ApplicationController::quickTerminalRecord()
{
    const auto record =
        std::ranges::find_if(windows_, [](const WindowRecord &candidate) {
            return candidate.role == WindowRole::QuickTerminal
                && !candidate.retiring && candidate.window != nullptr
                && candidate.workspace != nullptr;
        });
    return record == windows_.end() ? nullptr : std::addressof(*record);
}

GhosttyKeybindProgram ApplicationController::keybindProgram() const
{
    return keybindings_->keybindProgram();
}

bool ApplicationController::containsWorkspace(
    const TerminalWorkspace *workspace) const
{
    return workspace != nullptr
        && std::ranges::any_of(
               windows_, [workspace](const WindowRecord &record) {
                   return !record.retiring && record.workspace == workspace
                       && record.window != nullptr;
               });
}

std::vector<QPointer<TerminalWorkspace>>
ApplicationController::workspaceSnapshot() const
{
    std::vector<QPointer<TerminalWorkspace>> result;
    result.reserve(windows_.size());
    for (const WindowRecord &record : windows_) {
        if (record.workspace != nullptr) result.push_back(record.workspace);
    }
    return result;
}

void ApplicationController::registerWindow(
    ApplicationWindow applicationWindow, WindowRole role,
    std::unique_ptr<QuickTerminalSurface> quickTerminalSurface)
{
    QQuickWindow *const window = applicationWindow.window;
    TerminalWorkspace *const workspace = applicationWindow.workspace;
    const WindowId windowId(nextWindowId_++);
    Q_ASSERT(windowId.isValid());
    auto ui = std::make_unique<WindowUiController>();
    ui->replaceCommandPaletteEntries(
        executableCommandPaletteEntries(effectiveOptions_));
    const QPointer<ApplicationController> controller(this);
    ui->setCommandPaletteRefreshCallback([controller, windowId] {
        if (controller != nullptr) {
            controller->refreshCommandPalette(windowId);
        }
    });
    ui->setCommandPaletteActivationCallback(
        [controller, windowId](CommandPaletteCommand command) {
            if (controller == nullptr) return;
            std::visit(Overloaded{
                           [controller, windowId](const QString &action) {
                               if (controller != nullptr) {
                                   (void)controller->executePaletteAction(
                                       windowId, action);
                               }
                           },
                           [controller](SurfaceTarget target) {
                               if (controller != nullptr) {
                                   (void)controller->presentSurface(target);
                               }
                           },
                       },
                       std::move(command));
        });
    ui->setConfigurationRetryCallback([controller] {
        if (controller != nullptr) {
            Q_EMIT controller->configReloadRequested();
        }
    });
    ui->setConfigurationDiagnostics(
        diagnosticsForWorkspace(configurationDiagnosticsText(), workspace));
    QQmlEngine::setObjectOwnership(ui.get(), QQmlEngine::CppOwnership);
    auto blur = std::make_unique<WindowBlurController>(window);
    blur->setBlur(effectiveOptions_.backgroundBlur);

    QTimer *autohideTimer = nullptr;
    if (role == WindowRole::QuickTerminal) {
        autohideTimer = new QTimer(window);
        autohideTimer->setInterval(QuickTerminalAutohideDelay);
        autohideTimer->setSingleShot(true);
    }
    windows_.push_back({
        .id = windowId,
        .role = role,
        .window = window,
        .workspace = workspace,
        .ui = std::move(ui),
        .blur = std::move(blur),
        .quickTerminalSurface = std::move(quickTerminalSurface),
        .quickTerminalAutohideTimer = autohideTimer,
    });
    connect(workspace, &QObject::destroyed, this,
            [this, workspace, guardedWindow = QPointer(window)] {
                workspaceDestroyed(workspace, guardedWindow);
            });
    connect(window, &QObject::destroyed, this,
            [this, window] { retireWindow(window); });
    const QPointer<QQuickWindow> guardedWindow(window);
    const QPointer<TerminalWorkspace> guardedWorkspace(workspace);
    WindowRecord *registered = recordForWindow(window);
    Q_ASSERT(registered != nullptr);
    window->setProperty(
        "uiController",
        QVariant::fromValue(static_cast<QObject *>(registered->ui.get())));
    if (guardedWindow == nullptr || guardedWorkspace == nullptr) return;
    registered = recordForWindow(guardedWindow);
    if (registered == nullptr) return;
    if (registered->quickTerminalAutohideTimer != nullptr) {
        connect(registered->quickTerminalAutohideTimer, &QTimer::timeout, this,
                [this, guardedWindow = QPointer(window)] {
                    WindowRecord *const record =
                        recordForWindow(guardedWindow.data());
                    if (record == nullptr
                        || record->role != WindowRole::QuickTerminal
                        || record->window == nullptr
                        || !effectiveOptions_.applicationShell.quickTerminal
                                .autohide
                        || !record->window->isVisible()
                        || record->window->isActive()) {
                        return;
                    }
                    record->quickTerminalActivationAcknowledged = false;
                    record->window->hide();
                });
    }
    keybindings_->registerWorkspace(workspace);
    connect(workspace, &TerminalWorkspace::windowDecorationChanged, this,
            [this, guardedWindow = QPointer(window),
             guardedWorkspace = QPointer(workspace)] {
                syncWindowDecoration(guardedWindow, guardedWorkspace);
            });
    connect(workspace, &TerminalWorkspace::customShaderDiagnosticsChanged, this,
            [this](const QString &) { syncConfigurationDiagnostics(); });
    // Registration happens after workspace initialization but before the
    // first show. Applying the requested frame here avoids a decorated flash,
    // and the connection above keeps the same host in sync with reloads and
    // its per-window runtime override.
    syncWindowDecoration(window, workspace);

    connect(workspace, &TerminalWorkspace::applicationActionRequested, this,
            [this, guarded = QPointer(workspace)](ApplicationAction action,
                                                  PaneId paneId) {
                if (guarded != nullptr) {
                    dispatchRequestedAction(action, guarded, paneId);
                }
            });
    connect(workspace, &TerminalWorkspace::frontendActionRequested, this,
            [this, guarded = QPointer(workspace)](
                const WorkspaceFrontendActionRequest &request) {
                if (guarded != nullptr) {
                    (void)dispatchFrontendAction(guarded, request);
                }
            });
    connect(workspace, &TerminalWorkspace::standardClipboardCommitted, this,
            [this, guarded = QPointer(workspace)](bool empty) {
                if (!effectiveOptions_.applicationShell.notifications
                         .clipboardCopy) {
                    return;
                }
                WindowRecord *const record = recordForWorkspace(guarded.data());
                if (record != nullptr && record->ui != nullptr) {
                    record->ui->notifyClipboardCopied(empty);
                }
            });
    connect(
        workspace, &TerminalWorkspace::desktopNotificationRequested, this,
        [this, guarded = QPointer(workspace)](
            PaneId paneId, const TerminalDesktopNotification &notification) {
            if (!effectiveOptions_.desktopNotifications) return;
            const WindowRecord *const record =
                recordForWorkspace(guarded.data());
            if (record == nullptr || record->workspace == nullptr
                || !record->workspace->containsPane(paneId)) {
                return;
            }
            (void)desktopNotifications_->show(notification,
                                              {record->id, paneId});
        });
    connect(workspace, &TerminalWorkspace::windowNavigationRequested, this,
            [this](WindowNavigationAction action, PaneId) {
                // Keep activation in the originating event dispatch so Qt's
                // Wayland backend still has the user-input serial.
                (void)dispatch(action);
            });
    connect(workspace, &TerminalWorkspace::workspaceActivated, this,
            [this, guarded = QPointer(workspace)] {
                noteWorkspaceActivated(guarded);
            });
    connect(window, &QWindow::activeChanged, this,
            [this, guardedWindow = QPointer(window),
             guardedWorkspace = QPointer(workspace)] {
                if (guardedWindow != nullptr && guardedWindow->isActive()) {
                    noteWorkspaceActivated(guardedWorkspace);
                }
                if (WindowRecord *const record =
                        recordForWindow(guardedWindow.data());
                    record != nullptr
                    && record->role == WindowRole::QuickTerminal) {
                    if (guardedWindow->isActive()) {
                        record->quickTerminalActivationAcknowledged = true;
                    }
                    updateQuickTerminalAutohide(*record);
                }
            });
    connect(workspace, &TerminalWorkspace::applicationQuitApproved, this,
            [this, guarded = QPointer(workspace)] {
                commitApplicationQuit(guarded);
            });
    connect(workspace, &TerminalWorkspace::applicationQuitCancelled, this,
            [this, guarded = QPointer(workspace)] {
                applicationQuitCancelled(guarded);
            });
    connect(
        workspace, &TerminalWorkspace::windowCloseApproved, this,
        [this, guardedWindow = QPointer(window),
         guardedWorkspace = QPointer(workspace)] {
            workspaceShutdownApproved(guardedWorkspace);
            if (guardedWindow == nullptr) return;
            if (WindowRecord *const record =
                    recordForWindow(guardedWindow.data())) {
                record->retiring = true;
                if (record->quickTerminalAutohideTimer != nullptr) {
                    record->quickTerminalAutohideTimer->stop();
                }
            }
            guardedWindow->setProperty("closeApproved", true);
            QTimer::singleShot(0, this, [this, guardedWindow] {
                if (guardedWindow != nullptr) {
                    unmapAndRetireWindow(guardedWindow);
                }
            });
        },
        Qt::SingleShotConnection);

    if (WindowRecord *const record = recordForWindow(window);
        record != nullptr && record->role == WindowRole::QuickTerminal) {
        updateQuickTerminalAutohide(*record);
    }
}

bool ApplicationController::dispatchFrontendAction(
    TerminalWorkspace *workspace, const WorkspaceFrontendActionRequest &request)
{
    namespace FrontendAction = WorkspaceFrontendActions;

    WindowRecord *const record = recordForWorkspace(workspace);
    if (record == nullptr || record->ui == nullptr
        || !request.context.paneId.isValid()
        || !workspace->containsPane(request.context.paneId)) {
        return false;
    }

    return std::visit(
        Overloaded{
            [record](const FrontendAction::ToggleCommandPalette &) {
                record->ui->toggleCommandPalette();
                return true;
            },
            [record](const FrontendAction::ToggleTabOverview &) {
                record->ui->toggleTabOverview();
                return true;
            },
            [workspace, paneId = request.context.paneId](
                const FrontendAction::ShowOnScreenKeyboard &) {
                const QPointer<TerminalWorkspace> guard(workspace);
                if (!workspace->focusPaneForFrontend(paneId)
                    || guard == nullptr) {
                    return false;
                }
                QInputMethod *const inputMethod =
                    QGuiApplication::inputMethod();
                if (inputMethod == nullptr) return false;
                inputMethod->show();
                return true;
            },
            [workspace, paneId = request.context.paneId](
                const FrontendAction::Inspector &inspector) {
                return workspace->controlInspector(paneId, inspector.mode);
            },
            [this, record, workspace, paneId = request.context.paneId](
                const FrontendAction::Crash &crash) {
                return crashActionDispatcher_ != nullptr
                    && crashActionDispatcher_(record->window, workspace, paneId,
                                              crash.target);
            },
        },
        request.action);
}

void ApplicationController::syncWindowDecoration(QQuickWindow *window,
                                                 TerminalWorkspace *workspace)
{
    if (window == nullptr || workspace == nullptr
        || workspace->window() != window || !containsWorkspace(workspace)) {
        return;
    }
    const WindowRecord *const record = recordForWindow(window);
    const bool frameless =
        (record != nullptr && record->role == WindowRole::QuickTerminal)
        || workspace->windowDecoration() == WindowDecorationMode::None;
    if (window->flags().testFlag(Qt::FramelessWindowHint) == frameless) {
        return;
    }
    // QWindow::setFlag changes only this hint. Reconstructing setFlags would
    // drop unrelated host policy, while hide/show or geometry restoration
    // would disturb compositor-owned fullscreen/maximized state.
    window->setFlag(Qt::FramelessWindowHint, frameless);
}

void ApplicationController::noteWorkspaceActivated(TerminalWorkspace *workspace)
{
    if (containsWorkspace(workspace)) lastActiveWorkspace_ = workspace;
}

void ApplicationController::unmapAndRetireWindow(QQuickWindow *window)
{
    WindowRecord *const record = recordForWindow(window);
    if (window == nullptr || record == nullptr) return;
    record->retiring = true;
    if (record->quickTerminalAutohideTimer != nullptr) {
        record->quickTerminalAutohideTimer->stop();
    }

    const QPointer<QQuickWindow> guardedWindow(window);

    if (!isWaylandPlatform()) {
        hideWindowTree(window);
        if (guardedWindow != nullptr) guardedWindow->deleteLater();
        return;
    }

    // QWindow::close() destroys the wl_surface immediately. A compositor may
    // already have queued text-input leave for that surface, in which case Qt
    // receives a null protocol argument and diagnoses a mismatched focus
    // surface. Hiding only unmaps the surface, so keep the root alive until
    // Qt has published focus outside it and all of its transient windows.
    const auto retirement = std::make_shared<PendingWindowRetirement>();
    const auto finish = [guardedWindow, retirement] {
        if (retirement->finished) return;
        retirement->finished = true;
        QObject::disconnect(retirement->focusChanged);
        if (guardedWindow != nullptr) guardedWindow->deleteLater();
    };

    retirement->focusChanged =
        connect(qGuiApp, &QGuiApplication::focusWindowChanged, window,
                [guardedWindow, retirement, finish](QWindow *focusedWindow) {
                    if (guardedWindow == nullptr || retirement->finished)
                        return;
                    if (!belongsToWindowTree(focusedWindow, guardedWindow)) {
                        finish();
                        return;
                    }

                    // A nested ApplicationWindow (notably the inspector) can
                    // own the protocol focus. Unmap each focused transient
                    // while waiting for the compositor to move focus outside
                    // the complete root tree.
                    if (focusedWindow != guardedWindow) focusedWindow->hide();
                });
    QTimer::singleShot(WaylandFocusDrainTimeout, window, finish);

    hideWindowTree(window);
    if (guardedWindow != nullptr
        && !belongsToWindowTree(QGuiApplication::focusWindow(),
                                guardedWindow)) {
        finish();
    }
}

void ApplicationController::retireWindow(QQuickWindow *window)
{
    const auto previousSize = windows_.size();
    std::erase_if(windows_, [this, window](const WindowRecord &record) {
        const bool remove = record.window == nullptr || record.window == window;
        if (remove && record.workspace == lastActiveWorkspace_) {
            lastActiveWorkspace_.clear();
        }
        return remove;
    });
    if (previousSize == windows_.size()) return;
    if (windows_.empty()) lifetime_.lastWindowClosed();
    const QPointer<ApplicationController> guardedController(this);
    Q_EMIT windowRetired();
    if (guardedController != nullptr) finishApplicationQuitIfReady();
}

void ApplicationController::workspaceDestroyed(TerminalWorkspace *workspace,
                                               QQuickWindow *window)
{
    if (lastActiveWorkspace_.isNull() || lastActiveWorkspace_ == workspace) {
        lastActiveWorkspace_.clear();
    }
    awaitingShutdown_.remove(workspace);

    // A workspace normally disappears as a child of its root window. If an
    // observer destroys only that half, retire the unusable survivor instead
    // of leaving lifetime tracking and the application registry divergent.
    // Defer the check because QObject children are also destroyed while the
    // root's destructor is active, before that root's QPointer is cleared.
    if (!destroying_ && window != nullptr) {
        const QPointer guardedWindow(window);
        QTimer::singleShot(0, this, [this, guardedWindow] {
            if (destroying_ || guardedWindow == nullptr) return;
            const bool isRegisteredOrphan = std::ranges::any_of(
                windows_, [guardedWindow](const WindowRecord &record) {
                    return record.window == guardedWindow
                        && record.workspace.isNull();
                });
            if (isRegisteredOrphan) delete guardedWindow.data();
        });
    }

    if (quitState_ == QuitState::AwaitingConfirmation
        && (quitDialogHost_.isNull() || quitDialogHost_ == workspace)
        && !destroying_) {
        quitDialogHost_.clear();
        rehostApplicationQuit();
    } else if (quitState_ == QuitState::ClosingWindows) {
        finishApplicationQuitIfReady();
    }
}

void ApplicationController::requestApplicationQuit()
{
    if (quitState_ != QuitState::Idle || lifetime_.hasRequestedQuit()) return;

    const auto workspaces = workspaceSnapshot();
    if (workspaces.empty()) {
        beginApplicationShutdown();
        return;
    }

    WorkspaceCloseAssessment assessment;
    TerminalWorkspace *firstConfirmationHost = nullptr;
    for (const QPointer<TerminalWorkspace> &workspace : workspaces) {
        if (workspace != nullptr
            && workspace->canHostApplicationQuitConfirmation()) {
            if (firstConfirmationHost == nullptr) {
                firstConfirmationHost = workspace;
            }
            assessment |= workspace->closeAssessment();
        }
    }
    if (firstConfirmationHost == nullptr) {
        // Every window has already committed its ordinary close, so there is
        // nothing left to confirm. Let one closing workspace carry the sticky
        // application intent; its ordered window/application publication then
        // enters the shared shutdown path.
        TerminalWorkspace *host = activeWorkspace();
        if (host == nullptr) host = workspaces.front();
        quitState_ = QuitState::AwaitingConfirmation;
        quitDialogHost_ = host;
        host->requestApplicationQuitConfirmation({});
        return;
    }

    TerminalWorkspace *host = activeWorkspace();
    if (host == nullptr || !host->canHostApplicationQuitConfirmation()) {
        host = firstConfirmationHost;
    }
    quitState_ = QuitState::AwaitingConfirmation;
    quitDialogHost_ = host;
    host->requestApplicationQuitConfirmation(assessment);
}

void ApplicationController::beginApplicationShutdown()
{
    quitState_ = QuitState::ClosingWindows;
    quitDialogHost_.clear();
    awaitingShutdown_.clear();
    const auto workspaces = workspaceSnapshot();
    for (const QPointer<TerminalWorkspace> &workspace : workspaces) {
        if (workspace != nullptr
            && !workspace->isWindowCloseApprovalPublished()) {
            awaitingShutdown_.insert(workspace);
        }
    }

    startingApplicationShutdown_ = true;
    for (const QPointer<TerminalWorkspace> &workspace : workspaces) {
        if (workspace != nullptr
            && !workspace->isWindowCloseApprovalPublished()) {
            workspace->forceShutdownForApplicationQuit();
        }
    }
    startingApplicationShutdown_ = false;
    finishApplicationQuitIfReady();
}

void ApplicationController::commitApplicationQuit(TerminalWorkspace *host)
{
    if (quitState_ != QuitState::AwaitingConfirmation
        || quitDialogHost_ != host) {
        return;
    }

    beginApplicationShutdown();
}

void ApplicationController::applicationQuitCancelled(TerminalWorkspace *host)
{
    if (quitState_ != QuitState::AwaitingConfirmation
        || quitDialogHost_ != host) {
        return;
    }
    quitDialogHost_.clear();
    quitState_ = QuitState::Idle;
}

void ApplicationController::workspaceShutdownApproved(
    TerminalWorkspace *workspace)
{
    awaitingShutdown_.remove(workspace);
    finishApplicationQuitIfReady();
}

void ApplicationController::finishApplicationQuitIfReady()
{
    if (destroying_ || startingApplicationShutdown_
        || quitState_ != QuitState::ClosingWindows
        || !awaitingShutdown_.isEmpty() || !windows_.empty()
        || lifetime_.hasRequestedQuit()) {
        return;
    }
    Q_EMIT applicationQuitCommitted();
    lifetime_.requestQuitNow();
}

void ApplicationController::rehostApplicationQuit()
{
    if (quitRehostScheduled_) return;
    quitRehostScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        quitRehostScheduled_ = false;
        if (destroying_ || quitState_ != QuitState::AwaitingConfirmation
            || quitDialogHost_ != nullptr) {
            return;
        }
        quitState_ = QuitState::Idle;
        requestApplicationQuit();
    });
}
