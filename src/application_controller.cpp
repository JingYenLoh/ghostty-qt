#include "application_controller.h"

#include "desktop_activation.h"
#include "ghostty_application_keybindings.h"
#include "terminal_cell_metrics.h"
#include "terminal_workspace.h"

#include <QGuiApplication>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QScreen>
#include <QScopeGuard>
#include <QTimer>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <ranges>
#include <utility>

namespace {

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
    return window != nullptr
        && workspace != nullptr
        && workspace->window() == window
        && isOwnedBy(workspace, window);
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
        static_cast<long double>(cellExtent)
            * static_cast<long double>(cells)
        + static_cast<long double>(chromeExtent);
    constexpr int maximum = std::numeric_limits<int>::max();
    if (!std::isfinite(pixels)
        || pixels >= static_cast<long double>(maximum)) {
        return maximum;
    }
    return std::max(1, static_cast<int>(std::ceil(pixels)));
}

QSize windowSizeForGrid(const TerminalCellMetrics &metrics,
                        quint32 columns, quint32 rows,
                        qreal chromeWidth, qreal chromeHeight) noexcept
{
    return {
        pixelExtent(metrics.cellWidth, columns, chromeWidth),
        pixelExtent(metrics.cellHeight, rows, chromeHeight),
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
        terminalCellMetrics(options.fontFamily, options.fontSize);
    const qreal chromeWidth =
        nonNegativeWindowProperty(window, "terminalChromeWidth");
    const qreal chromeHeight =
        nonNegativeWindowProperty(window, "terminalChromeHeight");
    QSize minimum = windowSizeForGrid(
        metrics, minimumWindowColumns, minimumWindowRows,
        chromeWidth, chromeHeight);

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
        metrics,
        std::max(options.windowWidth, minimumWindowColumns),
        std::max(options.windowHeight, minimumWindowRows),
        chromeWidth, chromeHeight);
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

} // namespace

ApplicationController::ApplicationController(
    QQmlEngine &engine,
    LaunchOptions effectiveOptions,
    bool enableGlobalShortcutsPortal,
    QObject *parent)
    : ApplicationController(std::move(effectiveOptions),
                            qmlWindowFactory(engine),
                            enableGlobalShortcutsPortal,
                            parent)
{
}

ApplicationController::ApplicationController(
    LaunchOptions effectiveOptions,
    WindowFactory windowFactory,
    bool enableGlobalShortcutsPortal,
    QObject *parent)
    : QObject(parent)
    , windowFactory_(std::move(windowFactory))
    , effectiveOptions_(std::move(effectiveOptions))
    , keybindings_(std::make_unique<GhosttyApplicationKeybindings>(
          effectiveOptions_, enableGlobalShortcutsPortal))
{
    TerminalWorkspace::setDefaultLaunchOptions(effectiveOptions_);
    lifetime_.applyLaunchOptions(effectiveOptions_);

    connect(&lifetime_, &ApplicationLifetimeController::quitRequested,
            this, &ApplicationController::quitRequested);
    connect(keybindings_.get(),
            &GhosttyApplicationKeybindings::applicationActionRequested,
            this, [this](ApplicationAction action) {
                (void) dispatch(action);
            });
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
    component->loadFromModule(
        QStringLiteral("GhosttyQt"), QStringLiteral("Main"));

    return [component = std::move(component)]()
        -> std::expected<ApplicationWindow, QString> {
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
            return std::unexpected(
                QStringLiteral("Main.qml must create a QQuickWindow containing a TerminalWorkspace"));
        }
        return ApplicationWindow{window, workspace};
    };
}

std::expected<ApplicationWindow, QString>
ApplicationController::createInitialWindow(
    DesktopActivationContext activation)
{
    if (startupWindowHandled_) {
        return std::unexpected(
            QStringLiteral("The initial application window was already handled"));
    }

    auto created = createWindow(effectiveOptions_, activation);
    if (created.has_value() || hasCreatedSurface_) {
        startupWindowHandled_ = true;
    }
    return created;
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
    QScreen *const preferredScreen = source != nullptr
        && source->window() != nullptr
        ? source->window()->screen()
        : nullptr;
    auto created = createWindow(
        activationWindowOptions(), activation, preferredScreen);
    if (created.has_value()) return true;
    Q_EMIT windowCreationFailed(created.error());
    return false;
}

std::expected<ApplicationWindow, QString> ApplicationController::createWindow(
    const LaunchOptions &options,
    const DesktopActivationContext &activation,
    QScreen *preferredScreen)
{
    if (windowCreationInProgress_) {
        return std::unexpected(
            QStringLiteral("Application window creation is already in progress"));
    }
    if (!windowFactory_) {
        return std::unexpected(QStringLiteral("No window factory is available"));
    }
    if (lifetime_.hasRequestedQuit() || quitState_ != QuitState::Idle) {
        return std::unexpected(
            QStringLiteral("The application is already quitting"));
    }
    windowCreationInProgress_ = true;
    const auto creationGuard = qScopeGuard(
        [this] { windowCreationInProgress_ = false; });

    std::expected<ApplicationWindow, QString> created = windowFactory_();
    if (!created.has_value()) return created;
    QPointer<QQuickWindow> guardedWindow(created->window);
    QPointer<TerminalWorkspace> guardedWorkspace(created->workspace);
    const auto pairIsValid = [&] {
        return isValidWindowPair(
            guardedWindow.data(), guardedWorkspace.data());
    };
    const auto discardCreated = [&] {
        if (guardedWindow != nullptr) delete guardedWindow.data();
        if (guardedWorkspace != nullptr) delete guardedWorkspace.data();
        *created = {};
    };
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
        return std::unexpected(
            QStringLiteral(
                "The window factory returned a workspace outside the "
                "window's QObject ownership tree"));
    }

    if (preferredScreen != nullptr
        && guardedWindow->screen() != preferredScreen) {
        guardedWindow->setScreen(preferredScreen);
        if (!pairIsValid()) {
            discardCreated();
            return std::unexpected(
                QStringLiteral(
                    "The application window became invalid while selecting "
                    "its initial screen"));
        }
    }

    const InitialWindowGeometry geometry =
        initialWindowGeometry(*guardedWindow, options);
    guardedWindow->setMinimumSize(geometry.minimumSize);
    if (!pairIsValid()) {
        discardCreated();
        return std::unexpected(
            QStringLiteral(
                "The application window became invalid while configuring "
                "its initial geometry"));
    }
    if (geometry.requestedSize.has_value()) {
        guardedWindow->resize(*geometry.requestedSize);
        if (!pairIsValid()) {
            discardCreated();
            return std::unexpected(
                QStringLiteral(
                    "The application window became invalid while configuring "
                    "its initial geometry"));
        }
    }

    const TerminalSessionStartMode initialSessionStartMode =
        options.maximize || options.fullscreen
        ? TerminalSessionStartMode::Deferred
        : TerminalSessionStartMode::Immediate;
    const bool initialized = guardedWorkspace->initialize(
        options, initialSessionStartMode);
    // Match Ghostty's App.first lifecycle. A successfully initialized surface
    // consumes the one-shot initial command even if a synchronous observer
    // destroys the GUI pair before a later registration checkpoint.
    if (initialized) hasCreatedSurface_ = true;
    if (!pairIsValid()) {
        discardCreated();
        return std::unexpected(
            QStringLiteral(
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
        return std::unexpected(
            QStringLiteral("Could not register the primary application window"));
    }

    registerWindow({guardedWindow.data(), guardedWorkspace.data()});
    if (!pairIsValid()) {
        discardCreated();
        return std::unexpected(
            QStringLiteral(
                "The application window became invalid during registration"));
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
        || !setInitialProperty(
            "initialNormalSizePending",
            options.maximize || options.fullscreen)
        || !setInitialProperty(
            "visibilityBeforeFullscreen",
            static_cast<int>(options.maximize
                                 ? QWindow::Maximized
                                 : QWindow::Windowed))) {
        discardCreated();
        return std::unexpected(
            QStringLiteral(
                "The application window became invalid while configuring "
                "its initial state"));
    }

    const WindowPresentationMode presentationMode = options.fullscreen
        ? WindowPresentationMode::Fullscreen
        : (options.maximize
               ? WindowPresentationMode::Maximized
               : WindowPresentationMode::Windowed);
    showWindowWithActivation(
        *guardedWindow, activation, presentationMode);
    if (!pairIsValid()) {
        discardCreated();
        return std::unexpected(
            QStringLiteral(
                "The application window became invalid while being shown"));
    }
    if (initialSessionStartMode == TerminalSessionStartMode::Deferred
        && !guardedWorkspace->armInitialSessionStart()) {
        discardCreated();
        return std::unexpected(
            QStringLiteral(
                "Could not arm the initial terminal session after window "
                "presentation"));
    }
    if (!pairIsValid()) {
        discardCreated();
        return std::unexpected(
            QStringLiteral(
                "The application window became invalid while arming its "
                "initial terminal session"));
    }
    if (activation.isEmpty()) {
        guardedWindow->requestActivate();
        if (!pairIsValid()) {
            discardCreated();
            return std::unexpected(
                QStringLiteral(
                    "The application window became invalid during activation"));
        }
    }
    Q_EMIT windowCreated(guardedWindow.data(), guardedWorkspace.data());
    if (!pairIsValid()) {
        discardCreated();
        return std::unexpected(
            QStringLiteral(
                "The application window became invalid in its creation "
                "observer"));
    }
    return ApplicationWindow{
        guardedWindow.data(),
        guardedWorkspace.data(),
    };
}

bool ApplicationController::dispatch(ApplicationAction action,
                                     TerminalWorkspace *sourceWorkspace,
                                     PaneId sourcePaneId)
{
    switch (action) {
    case ApplicationAction::Ignore:
        return true;
    case ApplicationAction::OpenConfig:
        Q_EMIT configOpenRequested();
        return true;
    case ApplicationAction::ReloadConfig:
        Q_EMIT configReloadRequested();
        return true;
    case ApplicationAction::NewWindow: {
        if (quitState_ != QuitState::Idle || lifetime_.hasRequestedQuit()) {
            return false;
        }
        const QPointer<TerminalWorkspace> guardedSource(sourceWorkspace);
        QTimer::singleShot(
            0, this, [this, guardedSource, sourcePaneId] {
                TerminalWorkspace *screenSource = guardedSource;
                if (!containsWorkspace(screenSource)) {
                    screenSource = activeWorkspace();
                }
                QScreen *const preferredScreen = screenSource != nullptr
                    && screenSource->window() != nullptr
                    ? screenSource->window()->screen()
                    : nullptr;
                auto created = createWindow(
                    nextWindowOptions(guardedSource, sourcePaneId), {},
                    preferredScreen);
                if (!created.has_value()) {
                    Q_EMIT windowCreationFailed(created.error());
                }
            });
        return true;
    }
    case ApplicationAction::Quit:
        requestApplicationQuit();
        return true;
    }
    return false;
}

LaunchOptions ApplicationController::nextWindowOptions(
    TerminalWorkspace *sourceWorkspace,
    PaneId sourcePaneId) const
{
    if (!hasCreatedSurface_) return effectiveOptions_;

    TerminalWorkspace *const fallback = activeWorkspace();
    TerminalWorkspace *source = sourceWorkspace;
    if (!containsWorkspace(source)) {
        source = fallback;
        sourcePaneId = {};
    }

    if (source != nullptr) {
        if (const std::optional<LaunchOptions> inherited =
                source->newWindowLaunchOptions(
                    effectiveOptions_, sourcePaneId)) {
            return *inherited;
        }
        if (source != fallback && fallback != nullptr) {
            if (const std::optional<LaunchOptions> inherited =
                    fallback->newWindowLaunchOptions(effectiveOptions_)) {
                return *inherited;
            }
        }
    }

    LaunchOptions result = effectiveOptions_;
    result.program.clear();
    result.hold = false;
    return result;
}

LaunchOptions ApplicationController::activationWindowOptions() const
{
    if (!hasCreatedSurface_) return effectiveOptions_;

    LaunchOptions result = effectiveOptions_;
    if (TerminalWorkspace *const source = focusedWorkspace();
        source != nullptr && result.windowInheritWorkingDirectory) {
        // A normal Ghostty GApplication activation has no parent surface.
        // Its GTK surface setup still overlays the globally focused surface's
        // cwd, but parent-only font inheritance does not run.
        LaunchOptions directoryProbe = result;
        directoryProbe.windowInheritFontSize = false;
        if (const auto inherited =
                source->newWindowLaunchOptions(directoryProbe)) {
            result.workingDirectory = inherited->workingDirectory;
            result.inheritWorkingDirectory =
                inherited->inheritWorkingDirectory;
        }
    }
    result.program.clear();
    result.hold = false;
    return result;
}

void ApplicationController::applyLaunchOptions(const LaunchOptions &options)
{
    effectiveOptions_ = options;
    TerminalWorkspace::setDefaultLaunchOptions(effectiveOptions_);
    lifetime_.applyLaunchOptions(effectiveOptions_);
    keybindings_->applyLaunchOptions(effectiveOptions_);
    for (const QPointer<TerminalWorkspace> &workspace : workspaceSnapshot()) {
        if (workspace != nullptr) {
            workspace->applyLaunchOptions(effectiveOptions_);
        }
    }
}

TerminalWorkspace *ApplicationController::activeWorkspace() const
{
    if (TerminalWorkspace *const focused = focusedWorkspace()) {
        return focused;
    }
    for (auto record = windows_.crbegin(); record != windows_.crend();
         ++record) {
        if (record->window != nullptr && record->workspace != nullptr) {
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
            if (record.window == focusWindow && record.workspace != nullptr) {
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
    return static_cast<int>(std::ranges::count_if(
        windows_, [](const WindowRecord &record) {
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

bool ApplicationController::containsWorkspace(
    const TerminalWorkspace *workspace) const
{
    return workspace != nullptr
        && std::ranges::any_of(windows_, [workspace](const WindowRecord &record) {
               return record.workspace == workspace && record.window != nullptr;
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

void ApplicationController::registerWindow(ApplicationWindow applicationWindow)
{
    QQuickWindow *const window = applicationWindow.window;
    TerminalWorkspace *const workspace = applicationWindow.workspace;
    windows_.push_back({window, workspace});
    connect(workspace, &QObject::destroyed, this,
            [this, workspace, guardedWindow = QPointer(window)] {
                workspaceDestroyed(workspace, guardedWindow);
            });
    connect(window, &QObject::destroyed, this,
            [this, window] { retireWindow(window); });
    keybindings_->registerWorkspace(workspace);

    connect(workspace, &TerminalWorkspace::applicationActionRequested,
            this, [this, guarded = QPointer(workspace)](
                      ApplicationAction action, PaneId paneId) {
                if (guarded != nullptr) {
                    (void) dispatch(action, guarded, paneId);
                }
            });
    connect(workspace, &TerminalWorkspace::workspaceActivated,
            this, [this, guarded = QPointer(workspace)] {
                noteWorkspaceActivated(guarded);
            });
    connect(window, &QWindow::activeChanged, this,
            [this, guardedWindow = QPointer(window),
             guardedWorkspace = QPointer(workspace)] {
                if (guardedWindow != nullptr && guardedWindow->isActive()) {
                    noteWorkspaceActivated(guardedWorkspace);
                }
            });
    connect(workspace, &TerminalWorkspace::applicationQuitApproved,
            this, [this, guarded = QPointer(workspace)] {
                commitApplicationQuit(guarded);
            });
    connect(workspace, &TerminalWorkspace::applicationQuitCancelled,
            this, [this, guarded = QPointer(workspace)] {
                applicationQuitCancelled(guarded);
            });
    connect(workspace, &TerminalWorkspace::windowCloseApproved,
            this, [this, guardedWindow = QPointer(window),
                   guardedWorkspace = QPointer(workspace)] {
                workspaceShutdownApproved(guardedWorkspace);
                if (guardedWindow == nullptr) return;
                guardedWindow->setProperty("closeApproved", true);
                connect(guardedWindow, &QWindow::visibleChanged,
                        guardedWindow,
                        [guardedWindow](bool visible) {
                            if (!visible && guardedWindow != nullptr) {
                                guardedWindow->deleteLater();
                            }
                        },
                        Qt::SingleShotConnection);
                QTimer::singleShot(0, guardedWindow, [guardedWindow] {
                    if (guardedWindow == nullptr) return;
                    guardedWindow->close();
                    if (!guardedWindow->isVisible()) {
                        guardedWindow->deleteLater();
                    }
                });
            },
            Qt::SingleShotConnection);
}

void ApplicationController::noteWorkspaceActivated(
    TerminalWorkspace *workspace)
{
    if (containsWorkspace(workspace)) lastActiveWorkspace_ = workspace;
}

void ApplicationController::retireWindow(QQuickWindow *window)
{
    const auto previousSize = windows_.size();
    std::erase_if(windows_, [this, window](const WindowRecord &record) {
        const bool remove = record.window == nullptr
            || record.window == window;
        if (remove && record.workspace == lastActiveWorkspace_) {
            lastActiveWorkspace_.clear();
        }
        return remove;
    });
    if (previousSize == windows_.size()) return;
    if (windows_.empty()) lifetime_.lastWindowClosed();
    Q_EMIT windowRetired();
}

void ApplicationController::workspaceDestroyed(
    TerminalWorkspace *workspace, QQuickWindow *window)
{
    if (lastActiveWorkspace_.isNull()
        || lastActiveWorkspace_ == workspace) {
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
        quitState_ = QuitState::ClosingWindows;
        Q_EMIT applicationQuitCommitted();
        lifetime_.requestQuitNow();
        return;
    }

    WorkspaceCloseAssessment assessment;
    for (const QPointer<TerminalWorkspace> &workspace : workspaces) {
        if (workspace != nullptr) assessment |= workspace->closeAssessment();
    }

    TerminalWorkspace *host = activeWorkspace();
    if (host == nullptr) {
        host = workspaces.front();
    }
    quitState_ = QuitState::AwaitingConfirmation;
    quitDialogHost_ = host;
    host->requestApplicationQuitConfirmation(assessment);
}

void ApplicationController::commitApplicationQuit(TerminalWorkspace *host)
{
    if (quitState_ != QuitState::AwaitingConfirmation
        || quitDialogHost_ != host) {
        return;
    }

    quitState_ = QuitState::ClosingWindows;
    quitDialogHost_.clear();
    awaitingShutdown_.clear();
    const auto workspaces = workspaceSnapshot();
    for (const QPointer<TerminalWorkspace> &workspace : workspaces) {
        if (workspace != nullptr && !workspace->isWindowCloseApproved()) {
            awaitingShutdown_.insert(workspace);
        }
    }

    startingApplicationShutdown_ = true;
    for (const QPointer<TerminalWorkspace> &workspace : workspaces) {
        if (workspace != nullptr && !workspace->isWindowCloseApproved()) {
            workspace->forceShutdownForApplicationQuit();
        }
    }
    startingApplicationShutdown_ = false;
    finishApplicationQuitIfReady();
}

void ApplicationController::applicationQuitCancelled(
    TerminalWorkspace *host)
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
        || !awaitingShutdown_.isEmpty()
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
