#include "terminal_pane.h"

#include "ghostty_action_catalog.h"
#include "terminal_cell_metrics.h"
#include "terminal_clipboard.h"
#include "terminal_controller.h"
#include "terminal_drop.h"
#include "terminal_geometry.h"
#include "terminal_pane_renderer.h"

#include <QChronoTimer>
#include <QClipboard>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLatin1StringView>
#include <QMimeData>
#include <QMouseEvent>
#include <QMutexLocker>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QStyleHints>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace {

template <typename... Visitor> struct Overloaded : Visitor... {
    using Visitor::operator()...;
};

constexpr qreal kResizeOverlayWidth = 120.0;
constexpr qreal kResizeOverlayHeight = 40.0;
constexpr auto kMinimumResizeOverlayDuration = std::chrono::milliseconds(250);
constexpr auto kResizeOverlayStartupSuppression =
    std::chrono::milliseconds(250);

constexpr float kMinimumActionFontSize = 1.0F;
constexpr float kMaximumActionFontSize = 255.0F;
constexpr qsizetype kMaximumLinkPreviewBytes = 4096;
constexpr double kMinimumMouseScrollMultiplier = 0.01;
constexpr double kMaximumMouseScrollMultiplier = 10'000.0;
constexpr qint64 kMaximumMouseScrollStepsPerAxis = 10'000;
constexpr double kHorizontalTabScrollThreshold = 120.0;
constexpr auto kHorizontalTabScrollResetInterval =
    std::chrono::milliseconds(500);

TerminalCustomShaderCompileBroker *customShaderCompileBroker()
{
    static TerminalCustomShaderCompileBroker *const broker =
        new TerminalCustomShaderCompileBroker(qGuiApp);
    return broker;
}

TerminalCustomShaderVec4 shaderColor(const QColor &color,
                                     bool preserveAlpha = false)
{
    const QColor valid = color.isValid() ? color : QColor(Qt::transparent);
    constexpr float byteScale = 1.0F / 255.0F;
    return {
        .x = static_cast<float>(valid.red()) * byteScale,
        .y = static_cast<float>(valid.green()) * byteScale,
        .z = static_cast<float>(valid.blue()) * byteScale,
        .w = preserveAlpha ? static_cast<float>(valid.alpha()) * byteScale
                           : 1.0F,
    };
}

QColor shaderRelativeColor(const TerminalColorValue &configured,
                           const QColor &cellForeground,
                           const QColor &cellBackground, const QColor &fallback)
{
    switch (configured.kind) {
    case TerminalColorKind::Color:
        return configured.color.isValid() ? configured.color : fallback;
    case TerminalColorKind::CellForeground: return cellForeground;
    case TerminalColorKind::CellBackground: return cellBackground;
    case TerminalColorKind::Unset: return fallback;
    }
    return fallback;
}

std::int32_t shaderCursorStyle(int terminalStyle)
{
    switch (terminalStyle) {
    case 1: return 0; // block
    case 3: return 1; // hollow block
    case 0: return 2; // bar
    case 2: return 3; // underline
    default: return 0;
    }
}

[[nodiscard]] double normalizedMouseScrollMultiplier(double value,
                                                     double fallback) noexcept
{
    if (!std::isfinite(value)) return fallback;
    return std::clamp(value, kMinimumMouseScrollMultiplier,
                      kMaximumMouseScrollMultiplier);
}

qreal scrollbarFraction(quint64 numerator, quint64 denominator)
{
    if (denominator == 0) return 0.0;
    const long double fraction = static_cast<long double>(numerator)
        / static_cast<long double>(denominator);
    return std::clamp(static_cast<qreal>(fraction), qreal{0.0}, qreal{1.0});
}

bool isModifierOnlyKey(int key)
{
    return key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt
        || key == Qt::Key_AltGr || key == Qt::Key_Meta;
}

float clampFontActionValue(float value, float minimum)
{
    // Zig's @min/@max select the numeric operand when the other is NaN.
    // std::fmin/std::fmax provide the same behavior; std::clamp does not.
    return std::fmax(minimum, std::fmin(value, kMaximumActionFontSize));
}

bool terminalGridMetricsChanged(const TerminalCellMetrics &left,
                                const TerminalCellMetrics &right)
{
    return left.cellWidth != right.cellWidth
        || left.cellHeight != right.cellHeight;
}

QString linkPreviewDisplaySource(const QByteArray &uri)
{
    const bool truncated = uri.size() > kMaximumLinkPreviewBytes;
    const qsizetype byteCount = std::min(uri.size(), kMaximumLinkPreviewBytes);
    const QString decoded = QString::fromUtf8(uri.constData(), byteCount);

    // Keep arbitrary OSC 8 bytes and regex matches out of layout control
    // paths. Invalid UTF-8 is already represented visibly by U+FFFD; encode
    // single-line control characters so a destination cannot reshape the
    // pane overlay while the exact QByteArray remains available to copy/open.
    QString display;
    display.reserve(decoded.size() + (truncated ? 1 : 0));
    for (const QChar character : decoded) {
        const ushort value = character.unicode();
        if (value < 0x20 || (value >= 0x7f && value <= 0x9f)) {
            display += QStringLiteral("\\x");
            display +=
                QString::number(value, 16).rightJustified(2, u'0').toUpper();
        } else if (value == 0x061c || value == 0x200e || value == 0x200f
                   || (value >= 0x2028 && value <= 0x202e)
                   || (value >= 0x2066 && value <= 0x2069)) {
            display += QStringLiteral("\\u");
            display +=
                QString::number(value, 16).rightJustified(4, u'0').toUpper();
        } else {
            display += character;
        }
    }
    if (truncated) {
        display += u'\u2026';
    }
    return display;
}

Qt::KeyboardModifiers normalizedModifiers(Qt::KeyboardModifiers modifiers)
{
    return modifiers & ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
}

bool isRectangleSelectionState(Qt::KeyboardModifiers modifiers)
{
    modifiers = normalizedModifiers(modifiers);
    // Pinned Ghostty uses Ctrl+Alt on non-Darwin platforms. This frontend is
    // Linux-only, so keep the platform rule explicit here.
    return modifiers.testFlag(Qt::ControlModifier)
        && modifiers.testFlag(Qt::AltModifier);
}

Qt::KeyboardModifiers modifiersAfterKeyEvent(const QKeyEvent *event,
                                             bool pressed)
{
    Qt::KeyboardModifiers modifiers = normalizedModifiers(event->modifiers());
    const auto apply = [pressed, &modifiers](Qt::KeyboardModifier modifier) {
        if (pressed)
            modifiers |= modifier;
        else
            modifiers &= ~modifier;
    };
    switch (event->key()) {
    case Qt::Key_Control: apply(Qt::ControlModifier); break;
    case Qt::Key_Shift: apply(Qt::ShiftModifier); break;
    case Qt::Key_Alt: apply(Qt::AltModifier); break;
    case Qt::Key_Meta: apply(Qt::MetaModifier); break;
    default: break;
    }
    return modifiers;
}

uint32_t unshiftedCodepoint(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return static_cast<uint32_t>('a' + key - Qt::Key_A);
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return static_cast<uint32_t>('0' + key - Qt::Key_0);
    }
    switch (key) {
    case Qt::Key_Space: return ' ';
    case Qt::Key_QuoteLeft:
    case Qt::Key_AsciiTilde: return '`';
    case Qt::Key_Backslash:
    case Qt::Key_Bar: return '\\';
    case Qt::Key_BracketLeft:
    case Qt::Key_BraceLeft: return '[';
    case Qt::Key_BracketRight:
    case Qt::Key_BraceRight: return ']';
    case Qt::Key_Comma:
    case Qt::Key_Less: return ',';
    case Qt::Key_Equal:
    case Qt::Key_Plus: return '=';
    case Qt::Key_Minus:
    case Qt::Key_Underscore: return '-';
    case Qt::Key_Period:
    case Qt::Key_Greater: return '.';
    case Qt::Key_Apostrophe:
    case Qt::Key_QuoteDbl: return '\'';
    case Qt::Key_Semicolon:
    case Qt::Key_Colon: return ';';
    case Qt::Key_Slash:
    case Qt::Key_Question: return '/';
    default: return 0;
    }
}

quint64 keyEventIdentity(const QKeyEvent *event)
{
    const quint64 physical = static_cast<quint64>(event->nativeScanCode());
    const quint64 logical =
        static_cast<quint64>(static_cast<quint32>(event->key()));
    // Press/release logical keys can differ as modifiers change (notably
    // Backtab versus Tab). A native location is the stable identity.
    return physical != 0 ? (quint64{1} << 63U) | physical : logical;
}

quint64 keyEventIdentity(const KeyEventSnapshot &event)
{
    const quint64 physical = static_cast<quint64>(event.nativeScanCode);
    const quint64 logical =
        static_cast<quint64>(static_cast<quint32>(event.key));
    return physical != 0 ? (quint64{1} << 63U) | physical : logical;
}

std::optional<qint64> fractionalPageDelta(float fraction, int pageRows)
{
    if (!std::isfinite(fraction) || pageRows <= 0) return std::nullopt;

    const float product = fraction * static_cast<float>(pageRows);
    if (!std::isfinite(product)) return std::nullopt;
    const float truncated = std::trunc(product);

    // Ghostty converts this f32 result to isize. Converting intptr max to f32
    // rounds up on supported Linux targets, so step down to the greatest
    // representable f32 whose integer conversion remains in range.
    const float minimum =
        static_cast<float>(std::numeric_limits<qintptr>::min());
    const float maximum = std::nextafter(
        static_cast<float>(std::numeric_limits<qintptr>::max()), 0.0F);
    if (truncated < minimum || truncated > maximum) return std::nullopt;
    return static_cast<qint64>(truncated);
}

TerminalKeyInput terminalKeyInput(const QKeyEvent *event, bool pressed = true)
{
    return {
        .key = event->key(),
        .modifiers = static_cast<int>(event->modifiers()),
        .text = event->text(),
        .nativeScanCode = event->nativeScanCode(),
        .pressed = pressed,
        .autoRepeat = event->isAutoRepeat(),
        .unshiftedCodepoint = unshiftedCodepoint(event->key()),
    };
}

bool acceptTerminalDrop(QDropEvent *event)
{
    if (!event->possibleActions().testFlag(Qt::CopyAction)) {
        event->ignore();
        return false;
    }

    event->setDropAction(Qt::CopyAction);
    event->accept();
    return true;
}

} // namespace

TerminalPane::TerminalPane(
    const LaunchOptions &options, QQuickItem *parent,
    std::optional<TerminalSessionGeometry> initialGeometry,
    TerminalSessionStartMode startMode,
    std::shared_ptr<InitialSessionCoordinator> initialSessionCoordinator,
    std::optional<GhosttyKeybindProgram> keybindProgram,
    std::optional<TerminalCommand> firstSessionCommandOverride)
    : QQuickItem(parent)
    , options_(options)
    , appearance_(options.appearance)
    , backgroundOptions_(options.background)
    , paddingOptions_(options.padding)
    , splitAppearance_(options.splitAppearance)
    , keybinds_(
          keybindProgram.has_value()
              ? std::move(*keybindProgram)
              : GhosttyKeybindProgram::compile(options.keybindSource).program)
    , modifierRemaps_(options.modifierRemaps)
    , defaultFontPointSize_(options.typography.pointSize)
    , resizeOverlayStartupSuppressionEnds_(std::chrono::steady_clock::now()
                                           + kResizeOverlayStartupSuppression)
    , sessionStartMode_(startMode)
{
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
    TerminalPaneRenderer::clearProbe(this);
    TerminalPaneRenderer::publishInitialGeometryProbe(this, initialGeometry);
#endif
    setFlag(QQuickItem::ItemHasContents, true);
    setFlag(QQuickItem::ItemAcceptsDrops, true);
    setClip(true);
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
    setFlag(QQuickItem::ItemAcceptsInputMethod, true);
    setFocusPolicy(Qt::StrongFocus);
    metrics_ = terminalCellMetrics(
        options.typography,
        window() != nullptr ? window()->devicePixelRatio() : 1.0);
    connect(qGuiApp, &QGuiApplication::fontDatabaseChanged, this,
            &TerminalPane::refreshResolvedFonts);
    itemWindowConnection_ = connect(this, &QQuickItem::windowChanged, this,
                                    &TerminalPane::watchWindow);
    connect(this, &QQuickItem::visibleChanged, this, [this] {
        requestRenderUpdate();
        scheduleCustomShaderAnimationFrame();
    });
    watchWindow(window());
    urlOpener_ = [](const QUrl &url) { return QDesktopServices::openUrl(url); };

    frame_.foreground = options.appearance.foregroundColor;
    frame_.background = options.appearance.backgroundColor;
    frame_.palette = options.appearance.palette;
    frame_.cursorColor =
        options.appearance.cursorColor.kind == TerminalColorKind::Color
            && options.appearance.cursorColor.color.isValid()
        ? options.appearance.cursorColor.color
        : options.appearance.foregroundColor;
    cursorTimer_ = new QTimer(this);
    cursorTimer_->setInterval(600);
    connect(cursorTimer_, &QTimer::timeout, this, [this] {
        cursorBlinkOn_ = !cursorBlinkOn_;
        requestRenderUpdate();
    });
    horizontalTabScrollResetTimer_ = new QChronoTimer(this);
    horizontalTabScrollResetTimer_->setSingleShot(true);
    horizontalTabScrollResetTimer_->setInterval(
        kHorizontalTabScrollResetInterval);
    connect(horizontalTabScrollResetTimer_, &QChronoTimer::timeout, this,
            [this] { pendingHorizontalTabScrollPixels_ = 0.0; });
    resizeOverlayTimer_ = new QChronoTimer(this);
    resizeOverlayTimer_->setSingleShot(true);
    connect(resizeOverlayTimer_, &QChronoTimer::timeout, this,
            &TerminalPane::hideResizeOverlay);

    TerminalSessionLaunchOptions sessionOptions =
        toTerminalSessionLaunchOptions(options);
    sessionOptions.initialGeometry = std::move(initialGeometry);
    sessionOptions.firstSessionCommandOverride =
        std::move(firstSessionCommandOverride);
    controller_ = new TerminalController(sessionOptions, this,
                                         std::move(initialSessionCoordinator));
    controller_->setMouseReportingEnabled(options.mouseReporting);
    connect(controller_, &TerminalController::standardClipboardCommitted, this,
            &TerminalPane::standardClipboardCommitted);
    connect(
        controller_, &TerminalController::terminalUpdated, this,
        [this](const TerminalUpdate &terminalUpdate) {
            const QPointer<TerminalPane> guard(this);
            bool applied = false;
            bool backgroundChanged = false;
            {
                QMutexLocker locker(&renderMutex_);
                if (hasFrame_ || terminalUpdate.fullFrame) {
                    applied = applyTerminalUpdate(frame_, terminalUpdate);
                    hasFrame_ = hasFrame_ || applied;
                    if (applied) {
                        markTextRowsChangedLocked(terminalUpdate);
                        markSolidRowsChangedLocked(terminalUpdate);
                        backgroundChanged = terminalUpdate.fullFrame
                            || terminalUpdate.colorsChanged;
                    }
                    if (applied && frame_.rows > 0
                        && (!terminalResizePending_
                            || frame_.rows == terminalRows_)) {
                        terminalRows_ = frame_.rows;
                        terminalResizePending_ = false;
                    }
                    if (applied) {
                        const bool pendingMatchesFrame = pendingSearchUpdate_
                            && pendingSearchUpdate_->contentRevision
                                == frame_.contentRevision
                            && pendingSearchUpdate_->columns == frame_.columns
                            && pendingSearchUpdate_->rows == frame_.rows;
                        if (pendingMatchesFrame) {
                            installSearchDecorationsLocked(
                                *pendingSearchUpdate_);
                            clearPendingSearchUpdateLocked();
                        } else {
                            if (pendingSearchUpdate_
                                && pendingSearchUpdate_->contentRevision
                                    <= frame_.contentRevision) {
                                clearPendingSearchUpdateLocked();
                            }
                            if (searchDecorationRevision_
                                    != frame_.contentRevision
                                || searchDecorationColumns_ != frame_.columns
                                || searchDecorationRows_ != frame_.rows) {
                                clearSearchDecorationsLocked();
                            }
                        }
                    }
                }
            }
            if (applied) {
                if (backgroundChanged) {
                    refreshBackgroundImage();
                    if (guard == nullptr) return;
                }
                if (terminalUpdate.fullFrame
                    || terminalUpdate.scrollbarChanged) {
                    pendingScrollbarRow_.reset();
                }
                updateScrollbarState();
                if (guard == nullptr) return;
                if (hyperlinkQueryRejected_ && options_.linkUrl
                    && (terminalUpdate.fullFrame
                        || terminalUpdate.scrollbarChanged
                        || std::any_of(terminalUpdate.dirtyRows.cbegin(),
                                       terminalUpdate.dirtyRows.cend(),
                                       [this](const TerminalRowUpdate &row) {
                                           return row.row == hoverCell_.y();
                                       }))) {
                    // A rejected regex query has no worker-owned lease to
                    // refresh. Retry only when the stationary pointer's
                    // row (or its viewport mapping) changed.
                    hyperlinkQueryRejected_ = false;
                    hyperlinkQueryCell_ = QPoint(-1, -1);
                }
                // A worker-owned tracked reference decides whether the
                // logical target actually moved or disappeared. Merely
                // advancing the broad terminal revision must not flicker
                // a stable hover or cancel a press gesture.
                recomputeHyperlinkHover();
                syncCursorBlink(terminalUpdate.resetCursorBlink);
            }
        });
    connect(controller_, &TerminalController::searchUpdated, this,
            &TerminalPane::handleSearchUpdate);
    connect(controller_, &TerminalController::unsafePasteConfirmationRequested,
            this, [this](quint64 requestId, const QString &text) {
                Q_EMIT unsafePasteRequested(requestId, text, this);
            });
    connect(controller_, &TerminalController::terminalClipboardWriteRequested,
            this, [this](const TerminalClipboardWriteRequest &request) {
                Q_EMIT terminalClipboardWriteRequested(request, this);
            });
    connect(controller_, &TerminalController::terminalActionReady, this,
            &TerminalPane::handleTerminalActionResult, Qt::QueuedConnection);
    connect(controller_, &TerminalController::hyperlinkResolved, this,
            &TerminalPane::handleHyperlinkResult);
    connect(controller_, &TerminalController::hyperlinkActivationResolved, this,
            &TerminalPane::handleHyperlinkActivation);
    connect(controller_, &TerminalController::rightClickResolved, this,
            &TerminalPane::handleRightClickResult);
    connect(controller_, &TerminalController::titleChanged, this, [this] {
        if (!surfaceTitleOverride_.has_value()) {
            Q_EMIT titleChanged();
        }
    });
    connect(controller_, &TerminalController::launchProgramChanged, this,
            [this] {
                if (!surfaceTitleOverride_.has_value()
                    && !controller_->hasTitle()) {
                    Q_EMIT titleChanged();
                }
            });
    connect(controller_, &TerminalController::bell, this, [this] {
        const std::shared_ptr<TerminalBellPlayer> bellPlayer = bellPlayer_;
        const BellFeatures features = options_.bellFeatures;
        const std::optional<GhosttyConfigPath> audioPath =
            options_.bellAudioPath;
        const double audioVolume = options_.bellAudioVolume;
        const QPointer<TerminalPane> guard(this);
        setBellRinging(true);
        if (guard != nullptr) {
            Q_EMIT guard->bellRang(guard.data());
        }
        bellPlayer->ring(features, audioPath, audioVolume);
    });
    connect(controller_, &TerminalController::currentDirectoryChanged, this,
            &TerminalPane::currentDirectoryChanged);
    connect(controller_, &TerminalController::terminalMouseTrackingChanged,
            this, [this] {
                clearHyperlinkHover();
                recomputeHyperlinkHover();
            });
    connect(controller_, &TerminalController::runningChanged, this,
            &TerminalPane::processStateChanged);
    connect(controller_, &TerminalController::activeProcessChanged, this,
            &TerminalPane::processStateChanged);
    connect(controller_, &TerminalController::readOnlyChanged, this,
            &TerminalPane::readOnlyChanged);
    connect(controller_, &TerminalController::errorOccurred, this,
            [this](const QString &message) {
                {
                    QMutexLocker locker(&renderMutex_);
                    statusMessage_ = message;
                }
                requestRenderUpdate();
            });
    connect(
        controller_, &TerminalController::sessionExited, this,
        [this](int exitCode, int signalNumber, bool hold, bool waitForKey,
               quint64 runtimeMilliseconds, bool abnormal) {
            const QPointer<TerminalPane> guard(this);
            // Advance before any destruction-capable UI notification.
            // Results already queued to this pane, or retained by a
            // process-wide barrier, belong to the pre-exit epoch.
            advanceTerminalActionEpoch();
            clearHyperlinkHover();
            if (guard == nullptr) return;
            cancelHyperlinkPress();
            cancelPendingHyperlinkActivation();
            // TerminalController invalidates worker progress and fails
            // unresolved terminal actions before forwarding sessionExited.
            setSearchUiActive(false);
            if (guard == nullptr) return;
            searchEngineActive_ = false;
            searchMatchLabel_ = QStringLiteral("0/0");
            Q_EMIT searchMatchLabelChanged();
            if (guard == nullptr) return;
            {
                QMutexLocker locker(&renderMutex_);
                clearPendingSearchUpdateLocked();
                clearSearchDecorationsLocked();
            }
            bool hasError = false;
            waitingForExitKey_ = waitForKey;
            const QString abnormalText = abnormal
                ? (signalNumber > 0
                       ? QStringLiteral(
                             "Command failed after %1 ms (signal %2).")
                             .arg(static_cast<qulonglong>(runtimeMilliseconds))
                             .arg(signalNumber)
                       : QStringLiteral(
                             "Command failed after %1 ms (exit status %2).")
                             .arg(static_cast<qulonglong>(runtimeMilliseconds))
                             .arg(exitCode))
                : QString{};
            const bool abnormalStateChanged = abnormalExitVisible_ != abnormal
                || abnormalExitText_ != abnormalText;
            abnormalExitVisible_ = abnormal;
            abnormalExitText_ = abnormalText;
            if (abnormalStateChanged) {
                Q_EMIT abnormalExitChanged();
                if (guard == nullptr) return;
            }
            {
                QMutexLocker locker(&renderMutex_);
                hasError = !statusMessage_.isEmpty();
                if (statusMessage_.isEmpty()) {
                    const QString outcome = signalNumber > 0
                        ? QStringLiteral("Process ended after signal %1")
                              .arg(signalNumber)
                        : QStringLiteral("Process exited with status %1")
                              .arg(exitCode);
                    statusMessage_ = waitForKey
                        ? outcome
                            + QStringLiteral(
                                ". Press any key to close the terminal.")
                        : outcome;
                }
            }
            failStaleTerminalActionCompletions();
            if (guard == nullptr) return;
            requestRenderUpdate();
            Q_EMIT sessionEnded(this, exitCode, signalNumber);
            if (guard == nullptr) return;
            if (!hold && !waitForKey && !abnormal && !hasError) {
                QTimer::singleShot(0, this, [this] { Q_EMIT requestClose(); });
            }
        });
    connect(controller_, &TerminalController::exitKeyDismissed, this, [this] {
        if (!waitingForExitKey_) return;
        waitingForExitKey_ = false;
        Q_EMIT requestClose();
    });

    syncPointerCursor();
    refreshBackgroundImage();
    reloadCustomShaders(options_.customShaders);
    if (sessionStartMode_ == TerminalSessionStartMode::Immediate) {
        (void)controller_->startSession();
    }
}

TerminalPane::~TerminalPane()
{
    QObject::disconnect(itemWindowConnection_);
    if (observedWindow_ != nullptr) {
        observedWindow_->removeEventFilter(this);
        observedWindow_ = nullptr;
    }
    QObject::disconnect(windowActiveConnection_);
    QObject::disconnect(windowScreenConnection_);
    QObject::disconnect(windowSceneGraphInitializedConnection_);
    QObject::disconnect(customShaderFrameSwappedConnection_);
    disconnectDeferredSessionWindowSignals();
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
    TerminalPaneRenderer::clearProbe(this);
#endif
}

void TerminalPane::requestRenderUpdate()
{
    if (!customShaderStageItems_.isEmpty()) {
        if (!customShaderFramePending_) {
            customShaderFramePending_ = true;
            prepareCustomShaderFrame();
        }
        refreshCustomShaderUniformBase();
        (void)updateCustomShaderEffects();
    }
    if (renderItem_ != nullptr) {
        renderItem_->update();
    } else {
        update();
    }
}

TerminalCustomShaderUniformSnapshot
TerminalPane::terminalCustomShaderUniformSnapshot(int) const
{
    QMutexLocker locker(&customShaderUniformMutex_);
    return customShaderUniforms_;
}

void TerminalPane::terminalCustomShaderEffectAttached(
    TerminalCustomShaderEffect *effect, int)
{
    if (effect == nullptr || customShaderEffects_.contains(effect)) {
        return;
    }
    customShaderEffects_.append(effect);
    requestRenderUpdate();
}

void TerminalPane::terminalCustomShaderEffectDetached(
    TerminalCustomShaderEffect *effect, int)
{
    customShaderEffects_.removeIf(
        [effect](const QPointer<TerminalCustomShaderEffect> &candidate) {
            return candidate == nullptr || candidate == effect;
        });
}

bool TerminalPane::shouldAnimateCustomShaders() const
{
    if (customShaderStageItems_.isEmpty() || customShaderEffects_.isEmpty()
        || !isVisible() || width() <= 0.0 || height() <= 0.0
        || window() == nullptr || !window()->isVisible()
        || !window()->isExposed()) {
        return false;
    }
    switch (options_.customShaders.animation) {
    case TerminalCustomShaderAnimation::Never: return false;
    case TerminalCustomShaderAnimation::Focused: return hasActiveFocus();
    case TerminalCustomShaderAnimation::Always: return true;
    }
    return false;
}

void TerminalPane::prepareCustomShaderFrame()
{
    if (customShaderStages_.isEmpty()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!customShaderFirstFrameTime_.has_value()) {
        customShaderFirstFrameTime_ = now;
    }
    const auto previous = customShaderLastFrameTime_.value_or(now);
    customShaderLastFrameTime_ = now;

    QMutexLocker locker(&customShaderUniformMutex_);
    auto next = acquireCustomShaderUniformSnapshotLocked();
    next->time =
        std::chrono::duration<float>(now - *customShaderFirstFrameTime_)
            .count();
    next->timeDelta = std::chrono::duration<float>(now - previous).count();
    next->frame = next->frame == std::numeric_limits<std::int32_t>::max()
        ? 1
        : next->frame + 1;
    customShaderUniforms_ = std::move(next);
}

std::shared_ptr<TerminalCustomShaderUniforms>
TerminalPane::acquireCustomShaderUniformSnapshotLocked()
{
    for (const std::shared_ptr<TerminalCustomShaderUniforms> &candidate :
         customShaderUniformPool_) {
        if (candidate.use_count() == 1) {
            *candidate = customShaderUniforms_ != nullptr
                ? *customShaderUniforms_
                : TerminalCustomShaderUniforms{};
            return candidate;
        }
    }

    auto next = std::make_shared<TerminalCustomShaderUniforms>(
        customShaderUniforms_ != nullptr ? *customShaderUniforms_
                                         : TerminalCustomShaderUniforms{});
    // Two snapshots normally cover a render frame in flight. Keep a third for
    // a content update that also advances the frame; if rendering falls
    // further behind, transient allocations avoid mutating shared state.
    constexpr qsizetype maximumPooledSnapshots = 3;
    if (customShaderUniformPool_.size() < maximumPooledSnapshots) {
        customShaderUniformPool_.append(next);
    }
    return next;
}

void TerminalPane::scheduleCustomShaderAnimationFrame()
{
    if (!shouldAnimateCustomShaders()) {
        return;
    }
    if (!customShaderFramePending_) {
        customShaderFramePending_ = true;
        prepareCustomShaderFrame();
    }
    (void)updateCustomShaderEffects();
}

bool TerminalPane::updateCustomShaderEffects()
{
    bool updated = false;
    customShaderEffects_.removeIf(
        [&updated](const QPointer<TerminalCustomShaderEffect> &effect) {
            if (effect == nullptr) {
                return true;
            }
            if (effect->isActive()) {
                effect->update();
                updated = true;
            }
            return false;
        });
    return updated;
}

void TerminalPane::refreshCustomShaderUniformBase()
{
    if (customShaderStages_.isEmpty()) {
        return;
    }

    QVector<QColor> palette;
    QColor foreground;
    QColor background;
    QColor configuredCursor;
    QColor cursorCellForeground;
    QColor cursorCellBackground;
    TerminalAppearance appearance;
    TerminalPaddingOptions padding;
    TerminalCellMetrics metrics;
    int cursorColumn = 0;
    int cursorRow = 0;
    int cursorColumnSpan = 1;
    int cursorStyle = 1;
    bool cursorVisible = false;
    bool cursorBlinking = false;
    bool cursorColorExplicit = false;
    bool hasFrame = false;
    {
        QMutexLocker locker(&renderMutex_);
        palette =
            frame_.palette.isEmpty() ? appearance_.palette : frame_.palette;
        foreground = frame_.foreground;
        background = frame_.background;
        configuredCursor = frame_.cursorColor;
        appearance = appearance_;
        padding = paddingOptions_;
        metrics = metrics_;
        cursorColumn = frame_.cursorColumn;
        cursorRow = frame_.cursorRow;
        cursorColumnSpan = frame_.cursorColumnSpan;
        cursorStyle = frame_.cursorStyle;
        cursorVisible = frame_.cursorVisible;
        cursorBlinking = frame_.cursorBlinking;
        cursorColorExplicit = frame_.cursorColorExplicit;
        hasFrame = hasFrame_;
        const qsizetype cursorIndex =
            static_cast<qsizetype>(cursorRow) * frame_.columns + cursorColumn;
        if (cursorColumn >= 0 && cursorColumn < frame_.columns && cursorRow >= 0
            && cursorRow < frame_.rows && cursorIndex >= 0
            && cursorIndex < frame_.cells.size()) {
            cursorCellForeground = frame_.cells.at(cursorIndex).foreground;
            cursorCellBackground = frame_.cells.at(cursorIndex).background;
        } else {
            cursorCellForeground = foreground;
            cursorCellBackground = background;
        }
    }

    const qreal devicePixelRatio =
        TerminalPaneRenderer::normalizedDevicePixelRatio(
            window() != nullptr ? window()->devicePixelRatio() : 1.0);
    const qint64 physicalWidth =
        TerminalPaneRenderer::physicalPixels(width(), devicePixelRatio);
    const qint64 physicalHeight =
        TerminalPaneRenderer::physicalPixels(height(), devicePixelRatio);
    const std::optional<TerminalViewportLayout> layout =
        terminalViewportLayout({
            .surfaceSize = size(),
            .cellSize = QSizeF(metrics.cellWidth, metrics.cellHeight),
            .devicePixelRatio = devicePixelRatio,
            .padding = padding,
        });
    const bool focused = hasActiveFocus();
    const int effectiveStyle = focused ? cursorStyle : 3;
    const bool effectiveCursorVisible = hasFrame && cursorVisible
        && (!focused || !cursorBlinking || cursorBlinkOn_);

    QColor renderedCursor = configuredCursor;
    if (!cursorColorExplicit) {
        renderedCursor =
            shaderRelativeColor(appearance.cursorColor, cursorCellForeground,
                                cursorCellBackground, foreground);
    }
    if (renderedCursor.isValid()) {
        renderedCursor.setAlphaF(static_cast<float>(
            focused ? std::clamp(appearance.cursorOpacity, 0.0, 1.0) : 1.0));
    }
    const QColor cursorText =
        shaderRelativeColor(appearance.cursorTextColor, cursorCellForeground,
                            cursorCellBackground, background);

    QMutexLocker locker(&customShaderUniformMutex_);
    auto next = acquireCustomShaderUniformSnapshotLocked();
    next->resolution = {
        static_cast<float>(physicalWidth),
        static_cast<float>(physicalHeight),
        1.0F,
    };
    next->channelResolution[0] = {
        .x = static_cast<float>(physicalWidth),
        .y = static_cast<float>(physicalHeight),
        .z = 1.0F,
    };
    for (std::size_t index = 0; index < next->palette.size(); ++index) {
        next->palette[index] = index < static_cast<std::size_t>(palette.size())
            ? shaderColor(palette.at(static_cast<qsizetype>(index)))
            : TerminalCustomShaderVec4{};
    }
    next->backgroundColor = shaderColor(background);
    next->foregroundColor = shaderColor(foreground);
    next->cursorColor = shaderColor(
        configuredCursor.isValid() ? configuredCursor : renderedCursor);
    next->cursorText = shaderColor(cursorText);
    next->selectionForegroundColor =
        appearance.selectionForeground.kind == TerminalColorKind::Color
        ? shaderColor(appearance.selectionForeground.color)
        : TerminalCustomShaderVec4{};
    next->selectionBackgroundColor =
        appearance.selectionBackground.kind == TerminalColorKind::Color
        ? shaderColor(appearance.selectionBackground.color)
        : TerminalCustomShaderVec4{};
    // These two uniforms describe terminal state. Cursor blink/focus only
    // affects the rendered cursor glyph and iCurrentCursor, as in Ghostty.
    next->cursorVisible = hasFrame && cursorVisible ? 1 : 0;
    const std::int32_t mappedStyle = shaderCursorStyle(cursorStyle);
    if (next->currentCursorStyle != mappedStyle) {
        next->previousCursorStyle = next->currentCursorStyle;
        next->currentCursorStyle = mappedStyle;
    }
    next->focus = focused ? 1 : 0;
    if (focused && customShaderUniforms_ != nullptr
        && customShaderUniforms_->focus == 0) {
        next->timeFocus = next->time;
    }

    if (layout.has_value() && effectiveCursorVisible && cursorColumn >= 0
        && cursorColumn < layout->session.columns && cursorRow >= 0
        && cursorRow < layout->session.rows) {
        const qreal cellWidth =
            layout->gridRect.width() / layout->session.columns;
        const qreal cellHeight =
            layout->gridRect.height() / layout->session.rows;
        qreal left = layout->gridRect.x() + cursorColumn * cellWidth;
        const qreal top = layout->gridRect.y() + cursorRow * cellHeight;
        qreal cursorTop = top + metrics.cursorTop;
        qreal cursorWidth =
            cellWidth * static_cast<qreal>(std::max(1, cursorColumnSpan));
        qreal cursorHeight = metrics.cursorHeight;
        if (effectiveStyle == 0) {
            left += metrics.cursorBarLeft;
            cursorWidth = metrics.cursorThickness;
        } else if (effectiveStyle == 2) {
            cursorTop = top
                + std::min(metrics.underlinePosition,
                           metrics.underlineMaximumPosition);
            cursorHeight = metrics.cursorThickness;
        }
        const TerminalCustomShaderVec4 cursor{
            .x = static_cast<float>(left * devicePixelRatio),
            .y = static_cast<float>(static_cast<qreal>(physicalHeight)
                                    - cursorTop * devicePixelRatio),
            .z = static_cast<float>(cursorWidth * devicePixelRatio),
            .w = static_cast<float>(cursorHeight * devicePixelRatio),
        };
        const TerminalCustomShaderVec4 color =
            shaderColor(renderedCursor, true);
        if (cursor != next->currentCursor
            || color != next->currentCursorColor) {
            next->previousCursor = next->currentCursor;
            next->previousCursorColor = next->currentCursorColor;
            next->currentCursor = cursor;
            next->currentCursorColor = color;
            next->timeCursorChange = next->time;
        }
    }
    customShaderUniforms_ = std::move(next);
}

void TerminalPane::setCustomShaderStageComponent(QQmlComponent *component)
{
    if (customShaderStageComponent_ == component) {
        return;
    }
    customShaderStageComponent_ = component;
    rebuildCustomShaderStages();
}

void TerminalPane::reloadCustomShaders(
    const TerminalCustomShaderOptions &options)
{
    const quint64 generation = ++customShaderCompileGeneration_;
    if (options.sources.isEmpty()) {
        customShaderStages_.clear();
        customShaderCompileDiagnostic_.clear();
        customShaderRenderDiagnostic_.clear();
        customShaderFramePending_ = false;
        publishCustomShaderDiagnostic();
        rebuildCustomShaderStages();
        return;
    }

    const QPointer<TerminalPane> guard(this);
    customShaderCompileBroker()->request(
        options, this,
        [guard, generation](TerminalCustomShaderCompileResult result) mutable {
            if (guard == nullptr
                || guard->customShaderCompileGeneration_ != generation) {
                return;
            }
            bool rebuild = true;
            if (result.succeeded()) {
                rebuild = guard->customShaderStages_ != result.stages
                    || (guard->customShaderStageItems_.isEmpty()
                        && !result.stages.isEmpty()
                        && guard->customShaderStageComponent_ != nullptr
                        && guard->customShaderRenderingSupported());
                guard->customShaderStages_ = std::move(result.stages);
                guard->customShaderCompileDiagnostic_.clear();
                if (rebuild) {
                    guard->customShaderRenderDiagnostic_.clear();
                }
            } else {
                guard->customShaderStages_.clear();
                guard->customShaderCompileDiagnostic_ =
                    std::move(result.diagnostic);
                guard->customShaderRenderDiagnostic_.clear();
                qWarning().noquote() << guard->customShaderCompileDiagnostic_;
            }
            guard->publishCustomShaderDiagnostic();
            if (rebuild) {
                guard->customShaderFramePending_ = false;
                guard->rebuildCustomShaderStages();
            }
        });
}

void TerminalPane::publishCustomShaderDiagnostic()
{
    QString diagnostic = customShaderCompileDiagnostic_;
    if (!customShaderRenderDiagnostic_.isEmpty()) {
        if (!diagnostic.isEmpty()) {
            diagnostic.append(QLatin1Char('\n'));
        }
        diagnostic.append(customShaderRenderDiagnostic_);
    }
    if (customShaderDiagnostic_ == diagnostic) {
        return;
    }
    customShaderDiagnostic_ = std::move(diagnostic);
    Q_EMIT customShaderDiagnosticChanged(customShaderDiagnostic_);
}

bool TerminalPane::customShaderRenderingSupported() const
{
    const QQuickWindow *const quickWindow = window();
    if (quickWindow == nullptr || quickWindow->rendererInterface() == nullptr) {
        return false;
    }
    const QSGRendererInterface::GraphicsApi api =
        quickWindow->rendererInterface()->graphicsApi();
    return api == QSGRendererInterface::OpenGL
        || api == QSGRendererInterface::Vulkan;
}

void TerminalPane::clearCustomShaderStages()
{
    QObject::disconnect(customShaderFrameSwappedConnection_);
    customShaderFrameSwappedConnection_ = {};
    customShaderFramePending_ = false;
    if (renderItem_ == nullptr) {
        customShaderStageItems_.clear();
        return;
    }
    renderItem_->setParentItem(this);
    renderItem_->setParent(this);
    renderItem_->setZ(-1'000.0);

    QQuickItem *const outer = customShaderStageItems_.isEmpty()
        ? nullptr
        : customShaderStageItems_.constLast().data();
    customShaderStageItems_.clear();
    delete outer;
    customShaderEffects_.removeIf(
        [](const QPointer<TerminalCustomShaderEffect> &effect) {
            return effect == nullptr;
        });
}

void TerminalPane::useDirectTerminalRendering()
{
    if (renderItem_ != nullptr) {
        delete renderItem_;
        renderItem_ = nullptr;
    }
    // TerminalPane keeps ItemHasContents in both modes. While delegated,
    // updatePaintNode() tears down its former direct node and returns null;
    // switching back needs one content update to create that node again.
    update();
}

void TerminalPane::rebuildCustomShaderStages()
{
    clearCustomShaderStages();
    if (customShaderStages_.isEmpty()
        || customShaderStageComponent_ == nullptr) {
        useDirectTerminalRendering();
        customShaderRenderDiagnostic_.clear();
        publishCustomShaderDiagnostic();
        requestRenderUpdate();
        return;
    }
    if (!customShaderRenderingSupported()) {
        useDirectTerminalRendering();
        const QQuickWindow *const quickWindow = window();
        const QSGRendererInterface *const renderer =
            quickWindow != nullptr ? quickWindow->rendererInterface() : nullptr;
        if (renderer != nullptr
            && renderer->graphicsApi() != QSGRendererInterface::Unknown) {
            customShaderRenderDiagnostic_ = QStringLiteral(
                "custom-shader: the active Qt Quick graphics backend does not "
                "support OpenGL or Vulkan RHI shader effects; terminal "
                "rendering is unchanged");
        } else {
            customShaderRenderDiagnostic_.clear();
        }
        publishCustomShaderDiagnostic();
        requestRenderUpdate();
        return;
    }
    customShaderRenderDiagnostic_.clear();
    publishCustomShaderDiagnostic();

    if (renderItem_ == nullptr) {
        renderItem_ = TerminalPaneRenderer::createRenderItem(this);
        renderItem_->setSize(size());
        // Schedule removal of any direct TerminalPane paint node. Merely
        // clearing ItemHasContents does not dirty content and can leave the
        // unfiltered node alive over the shader chain.
        update();
    }
    QQuickItem *source = renderItem_;
    const qreal devicePixelRatio =
        TerminalPaneRenderer::normalizedDevicePixelRatio(
            window() != nullptr ? window()->devicePixelRatio() : 1.0);
    for (qsizetype index = 0; index < customShaderStages_.size(); ++index) {
        const TerminalCustomShaderStage &compiled =
            customShaderStages_.at(index);
        QObject *const created =
            customShaderStageComponent_->createWithInitialProperties({
                {QStringLiteral("fragmentShaderFileName"), compiled.qsbPath},
                {QStringLiteral("fragmentShaderData"),
                 compiled.serializedShader},
                {QStringLiteral("uniformProvider"),
                 QVariant::fromValue(static_cast<QObject *>(this))},
                {QStringLiteral("stageIndex"),
                 QVariant::fromValue(static_cast<int>(index))},
                {QStringLiteral("sourceDevicePixelRatio"), devicePixelRatio},
            });
        auto *const stage = qobject_cast<QQuickItem *>(created);
        if (stage == nullptr) {
            if (customShaderStageComponent_ != nullptr) {
                qWarning().noquote()
                    << "Could not create terminal custom-shader stage:"
                    << customShaderStageComponent_->errorString();
            }
            delete created;
            clearCustomShaderStages();
            useDirectTerminalRendering();
            customShaderRenderDiagnostic_ =
                QStringLiteral("custom-shader: unable to create Qt render "
                               "stage; terminal rendering is unchanged");
            publishCustomShaderDiagnostic();
            return;
        }

        stage->setParent(this);
        stage->setSize(size());
        stage->setZ(-1'000.0);
        source->setParent(stage);
        source->setParentItem(stage);
        source = stage;
        customShaderStageItems_.append(stage);
    }
    source->setParent(this);
    source->setParentItem(this);
    source->setZ(-1'000.0);
    if (QQuickWindow *const quickWindow = window()) {
        customShaderFrameSwappedConnection_ =
            connect(quickWindow, &QQuickWindow::frameSwapped, this, [this] {
                customShaderFramePending_ = false;
                scheduleCustomShaderAnimationFrame();
            });
    }
    requestRenderUpdate();
}

void TerminalPane::syncCustomShaderStageGeometry()
{
    const qreal devicePixelRatio =
        TerminalPaneRenderer::normalizedDevicePixelRatio(
            window() != nullptr ? window()->devicePixelRatio() : 1.0);
    for (const QPointer<QQuickItem> &stage : customShaderStageItems_) {
        if (stage != nullptr) {
            stage->setSize(size());
            stage->setProperty("sourceDevicePixelRatio", devicePixelRatio);
        }
    }
}

bool TerminalPane::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == observedWindow_.data() && event != nullptr) {
        switch (event->type()) {
        case QEvent::DevicePixelRatioChange:
            deferredSessionStartCandidate_.reset();
            (void)updateMetrics();
            updateTerminalSize();
            scheduleDeferredSessionStart();
            syncCustomShaderStageGeometry();
            requestRenderUpdate();
            break;
        case QEvent::Expose:
        case QEvent::Show:
            customShaderFramePending_ = false;
            requestRenderUpdate();
            scheduleCustomShaderAnimationFrame();
            [[fallthrough]];
        case QEvent::Resize:
        case QEvent::WindowStateChange:
            deferredSessionStartCandidate_.reset();
            scheduleDeferredSessionStart();
            break;
        default: break;
        }
    }
    return QQuickItem::eventFilter(watched, event);
}

void TerminalPane::watchWindow(QQuickWindow *quickWindow)
{
    if (observedWindow_ != nullptr) {
        observedWindow_->removeEventFilter(this);
    }
    QObject::disconnect(windowActiveConnection_);
    QObject::disconnect(windowScreenConnection_);
    QObject::disconnect(windowSceneGraphInitializedConnection_);
    QObject::disconnect(customShaderFrameSwappedConnection_);
    disconnectDeferredSessionWindowSignals();
    windowActiveConnection_ = {};
    windowScreenConnection_ = {};
    windowSceneGraphInitializedConnection_ = {};
    customShaderFrameSwappedConnection_ = {};
    observedWindow_ = quickWindow;
    deferredSessionStartCandidate_.reset();
    deferredSessionPresentedFrame_ = 0;
    deferredSessionCandidateFrame_ = 0;
    if (quickWindow != nullptr) {
        quickWindow->installEventFilter(this);
        windowActiveConnection_ =
            connect(quickWindow, &QWindow::activeChanged, this,
                    [this] { requestRenderUpdate(); });
        windowSceneGraphInitializedConnection_ =
            connect(quickWindow, &QQuickWindow::sceneGraphInitialized, this,
                    [this] { rebuildCustomShaderStages(); });
        windowScreenConnection_ = connect(
            quickWindow, &QWindow::screenChanged, this, [this](QScreen *) {
                (void)updateMetrics();
                updateTerminalSize();
                deferredSessionStartCandidate_.reset();
                scheduleDeferredSessionStart();
                syncCustomShaderStageGeometry();
                requestRenderUpdate();
            });
        if (sessionStartMode_ == TerminalSessionStartMode::Deferred
            && (controller_ == nullptr || !controller_->sessionStarted())) {
            windowVisibilityConnection_ =
                connect(quickWindow, &QWindow::visibilityChanged, this,
                        [this](QWindow::Visibility) {
                            deferredSessionStartCandidate_.reset();
                            scheduleDeferredSessionStart();
                        });
            windowStateConnection_ =
                connect(quickWindow, &QWindow::windowStateChanged, this,
                        [this](Qt::WindowState) {
                            deferredSessionStartCandidate_.reset();
                            scheduleDeferredSessionStart();
                        });
            windowFrameSwappedConnection_ =
                connect(quickWindow, &QQuickWindow::frameSwapped, this, [this] {
                    if (!deferredSessionStartArmed_) return;
                    ++deferredSessionPresentedFrame_;
                    scheduleDeferredSessionStart();
                });
        }
        // Geometry may have settled before a previously detached pane entered
        // this scene, so refresh even when its logical size did not change.
        (void)updateMetrics();
        updateTerminalSize();
    } else {
        // Detaching restores the logical one-to-one fallback used for an
        // off-scene pane. The next attachment will rebuild at that screen's
        // physical scale before publishing PTY geometry.
        (void)updateMetrics();
    }
    scheduleDeferredSessionStart();
    rebuildCustomShaderStages();
    requestRenderUpdate();
}

void TerminalPane::disconnectDeferredSessionWindowSignals()
{
    QObject::disconnect(windowVisibilityConnection_);
    QObject::disconnect(windowStateConnection_);
    QObject::disconnect(windowFrameSwappedConnection_);
    windowVisibilityConnection_ = {};
    windowStateConnection_ = {};
    windowFrameSwappedConnection_ = {};
}

bool TerminalPane::armDeferredSessionStart(
    std::function<QSizeF()> viewportSizeProvider)
{
    if (sessionStartMode_ != TerminalSessionStartMode::Deferred
        || controller_ == nullptr) {
        return false;
    }
    if (controller_->sessionStarted()) {
        return true;
    }
    deferredSessionStartArmed_ = true;
    deferredSessionViewportSizeProvider_ = std::move(viewportSizeProvider);
    deferredSessionStartCandidate_.reset();
    deferredSessionCandidateFrame_ = deferredSessionPresentedFrame_;
    scheduleDeferredSessionStart();
    return true;
}

void TerminalPane::scheduleDeferredSessionStart()
{
    if (!deferredSessionStartArmed_ || deferredSessionStartCheckQueued_
        || controller_ == nullptr || controller_->sessionStarted()) {
        return;
    }
    deferredSessionStartCheckQueued_ = true;
    QTimer::singleShot(0, this, [this] {
        deferredSessionStartCheckQueued_ = false;
        tryDeferredSessionStart();
    });
}

void TerminalPane::tryDeferredSessionStart()
{
    if (!deferredSessionStartArmed_ || controller_ == nullptr
        || controller_->sessionStarted()) {
        return;
    }
    QQuickWindow *const quickWindow = window();
    if (quickWindow == nullptr || !quickWindow->isVisible()
        || !quickWindow->isExposed()) {
        deferredSessionStartCandidate_.reset();
        return;
    }
    std::optional<QSizeF> viewportSize;
    if (deferredSessionViewportSizeProvider_) {
        const std::function<QSizeF()> viewportSizeProvider =
            deferredSessionViewportSizeProvider_;
        const QPointer<TerminalPane> guard(this);
        viewportSize = viewportSizeProvider();
        if (guard.isNull()) return;
    }
    const std::optional<TerminalSessionGeometry> geometry =
        currentSessionGeometry(viewportSize);
    if (!geometry.has_value()) {
        deferredSessionStartCandidate_.reset();
        return;
    }
    // One queued stability turn prevents an expose/configure callback from
    // observing the old QML anchor layout before its resize is polished. Two
    // presented frames then acknowledge that the same geometry survived a
    // post-map scene-graph boundary rather than only two zero-delay timers.
    if (deferredSessionStartCandidate_ != geometry) {
        deferredSessionStartCandidate_ = geometry;
        deferredSessionCandidateFrame_ = deferredSessionPresentedFrame_;
        quickWindow->update();
        return;
    }
    if (deferredSessionPresentedFrame_ - deferredSessionCandidateFrame_ < 2) {
        quickWindow->update();
        return;
    }
    // Disarm before startSession emits process-state changes: a synchronous
    // observer may delete this pane or its window from that notification.
    deferredSessionStartArmed_ = false;
    deferredSessionStartCandidate_.reset();
    deferredSessionViewportSizeProvider_ = {};
    disconnectDeferredSessionWindowSignals();
    // The hidden normal geometry may differ from the compositor-assigned
    // viewport. Record the committed grid without presenting it as a resize.
    noteTerminalGridSize(*geometry);
    (void)controller_->startSession(geometry);
}

QString TerminalPane::title() const
{
    if (const std::optional<QString> effective = effectiveSurfaceTitle();
        effective.has_value()) {
        return *effective;
    }
    return controller_->launchTitle();
}

QRectF TerminalPane::resizeOverlayRect() const
{
    const qreal centerX = std::max(0.0, (width() - kResizeOverlayWidth) / 2.0);
    const qreal right = std::max(0.0, width() - kResizeOverlayWidth);
    const qreal centerY =
        std::max(0.0, (height() - kResizeOverlayHeight) / 2.0);
    const qreal bottom = std::max(0.0, height() - kResizeOverlayHeight);

    QPointF position;
    switch (options_.resizeOverlay.position) {
    case ResizeOverlayPosition::Center: position = {centerX, centerY}; break;
    case ResizeOverlayPosition::TopLeft: break;
    case ResizeOverlayPosition::TopCenter: position.setX(centerX); break;
    case ResizeOverlayPosition::TopRight: position.setX(right); break;
    case ResizeOverlayPosition::BottomLeft: position.setY(bottom); break;
    case ResizeOverlayPosition::BottomCenter:
        position = {centerX, bottom};
        break;
    case ResizeOverlayPosition::BottomRight: position = {right, bottom}; break;
    }
    return {position, QSizeF(kResizeOverlayWidth, kResizeOverlayHeight)};
}

void TerminalPane::updateScrollbarState()
{
    quint64 total = 0;
    quint64 offset = 0;
    quint64 length = 0;
    bool hasFrame = false;
    {
        QMutexLocker locker(&renderMutex_);
        total = frame_.scrollTotal;
        offset = frame_.scrollOffset;
        length = frame_.scrollLength;
        hasFrame = hasFrame_;
    }

    const bool visible = options_.scrollbar == ScrollbarPolicy::System
        && hasFrame && length > 0 && length < total;
    qreal position = 0.0;
    qreal size = 1.0;
    if (visible) {
        const quint64 maximumOffset = total - length;
        position = scrollbarFraction(std::min(offset, maximumOffset), total);
        size = scrollbarFraction(length, total);
    } else {
        pendingScrollbarRow_.reset();
    }

    if (scrollbarVisible_ == visible && scrollbarPosition_ == position
        && scrollbarSize_ == size) {
        return;
    }
    scrollbarVisible_ = visible;
    scrollbarPosition_ = position;
    scrollbarSize_ = size;
    Q_EMIT scrollbarChanged();
}

void TerminalPane::scrollbarMoveTo(qreal position)
{
    if (!scrollbarVisible_ || std::isnan(position)) return;

    quint64 total = 0;
    quint64 offset = 0;
    quint64 length = 0;
    bool hasFrame = false;
    {
        QMutexLocker locker(&renderMutex_);
        total = frame_.scrollTotal;
        offset = frame_.scrollOffset;
        length = frame_.scrollLength;
        hasFrame = hasFrame_;
    }
    if (options_.scrollbar != ScrollbarPolicy::System || !hasFrame
        || length == 0 || length >= total) {
        return;
    }

    const quint64 maximumOffset = total - length;
    const qreal maximumPosition = scrollbarFraction(maximumOffset, total);
    qreal boundedPosition = 0.0;
    if (position >= maximumPosition) {
        boundedPosition = maximumPosition;
    } else if (position > 0.0) {
        boundedPosition = position;
    }

    const long double scaled = static_cast<long double>(boundedPosition)
        * static_cast<long double>(total);
    quint64 target = 0;
    if (scaled >= static_cast<long double>(maximumOffset)) {
        target = maximumOffset;
    } else if (scaled > 0.0L) {
        target = static_cast<quint64>(std::floor(scaled + 0.5L));
        target = std::min(target, maximumOffset);
    }

    const quint64 current = std::min(offset, maximumOffset);
    // A bound control writes the authoritative value back while synchronizing.
    // Compare that normalized value before relying on a floating-point round
    // trip that cannot exactly encode every row. A different pending request
    // is the exception: dragging back to the authoritative row must supersede
    // the request that has not been acknowledged yet.
    if (boundedPosition == scrollbarPosition_
        && pendingScrollbarRow_.value_or(current) == current) {
        return;
    }

    if (target == pendingScrollbarRow_.value_or(current)) return;
    pendingScrollbarRow_ = target;
    controller_->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Row,
        .row = target,
    });
}

std::optional<QString> TerminalPane::effectiveSurfaceTitle() const
{
    if (surfaceTitleOverride_.has_value()) {
        return surfaceTitleOverride_;
    }
    if (controller_->hasTitle()) {
        return controller_->title();
    }
    return std::nullopt;
}

void TerminalPane::setSurfaceTitle(QString title)
{
    controller_->setSurfaceTitle(std::move(title));
}

void TerminalPane::setSurfaceTitleOverride(std::optional<QString> title)
{
    if (surfaceTitleOverride_ == title) {
        return;
    }
    surfaceTitleOverride_ = std::move(title);
    Q_EMIT titleChanged();
}

void TerminalPane::setBellRinging(bool ringing)
{
    if (bellRinging_ == ringing) return;
    bellRinging_ = ringing;
    Q_EMIT bellChanged();
}

QString TerminalPane::currentDirectory() const
{
    return controller_->currentDirectory();
}

qreal TerminalPane::fontPointSize() const
{
    QMutexLocker locker(&renderMutex_);
    return metrics_.font(TerminalFontRole::Regular).pointSizeF();
}

QStringList TerminalPane::activeKeyTables() const
{
    return keybinds_.activeTableNames();
}

QStringList TerminalPane::pendingKeySequence() const
{
    return keybinds_.activeSequenceLabels();
}

QString TerminalPane::linkPreviewText() const
{
    QMutexLocker locker(&renderMutex_);
    return linkPreviewText_;
}

QRectF TerminalPane::linkPreviewRect() const
{
    QMutexLocker locker(&renderMutex_);
    return linkPreviewRect_;
}

bool TerminalPane::isRunning() const
{
    return controller_->running();
}

bool TerminalPane::hasActiveProcess() const
{
    return controller_->activeProcess();
}

bool TerminalPane::isReadOnly() const
{
    return controller_->readOnly();
}

void TerminalPane::dismissAbnormalExit()
{
    if (!abnormalExitVisible_) return;
    Q_EMIT requestClose();
}

LaunchOptions TerminalPane::splitLaunchOptions(const LaunchOptions &base) const
{
    LaunchOptions result = base;
    const QString directory = currentDirectory();
    if (base.splitInheritWorkingDirectory && !directory.isEmpty()) {
        result.workingDirectory = directory;
        result.inheritWorkingDirectory = false;
    }
    {
        QMutexLocker locker(&renderMutex_);
        result.typography = options_.typography;
        result.typography.pointSize =
            metrics_.font(TerminalFontRole::Regular).pointSizeF();
    }
    return withoutInitialCommand(std::move(result));
}

LaunchOptions TerminalPane::tabLaunchOptions(const LaunchOptions &base) const
{
    LaunchOptions result = base;
    const QString directory = currentDirectory();
    if (base.tabInheritWorkingDirectory && !directory.isEmpty()) {
        result.workingDirectory = directory;
        result.inheritWorkingDirectory = false;
    }
    if (base.windowInheritFontSize) {
        QMutexLocker locker(&renderMutex_);
        result.typography.pointSize =
            metrics_.font(TerminalFontRole::Regular).pointSizeF();
    }
    return withoutInitialCommand(std::move(result));
}

LaunchOptions TerminalPane::windowLaunchOptions(const LaunchOptions &base) const
{
    LaunchOptions result = base;
    const QString directory = currentDirectory();
    if (base.windowInheritWorkingDirectory && !directory.isEmpty()) {
        result.workingDirectory = directory;
        result.inheritWorkingDirectory = false;
    }
    if (base.windowInheritFontSize) {
        QMutexLocker locker(&renderMutex_);
        result.typography.pointSize =
            metrics_.font(TerminalFontRole::Regular).pointSizeF();
    }
    return withoutInitialCommand(std::move(result));
}

void TerminalPane::applyRuntimeOptions(const LaunchOptions &options)
{
    applyRuntimeOptions(
        options, GhosttyKeybindProgram::compile(options.keybindSource).program);
}

void TerminalPane::applyRuntimeOptions(const LaunchOptions &options,
                                       GhosttyKeybindProgram keybindProgram)
{
    const RevisionCounter::Value revision = runtimeOptionsRevision_.advance();
    const QPointer<TerminalPane> guard(this);
    beginKeyEventDeferral();
    const auto keyEventDeferralGuard = qScopeGuard([guard] {
        if (guard != nullptr) guard->endKeyEventDeferral();
    });
    const auto stillCurrentUpdate = [guard, revision, keybindProgram] {
        return guard != nullptr
            && guard->runtimeOptionsRevision_.isCurrent(revision)
            && guard->keybindProgram().isSameGeneration(keybindProgram);
    };

    LaunchOptions updated = options_;
    updated.typography = options.typography;
    updated.fontFamilyExplicit = options.fontFamilyExplicit;
    updated.fontSizeExplicit = options.fontSizeExplicit;
    updated.appearance = options.appearance;
    updated.colorScheme = options.colorScheme;
    updated.configuredTitle = options.configuredTitle;
    updated.background = options.background;
    // Ghostty documents x/y as new-terminal-only. Balance and color remain
    // live while this pane retains its construction-time dimensions.
    updated.padding.balance = options.padding.balance;
    updated.padding.color = options.padding.color;
    updated.scrollbar = options.scrollbar;
    updated.bellFeatures = options.bellFeatures;
    updated.bellAudioPath = options.bellAudioPath;
    updated.bellAudioVolume = options.bellAudioVolume;
    updated.selectionClipboard = options.selectionClipboard;
    updated.selectionWordChars = options.selectionWordChars;
    updated.clickRepeatIntervalMilliseconds =
        options.clickRepeatIntervalMilliseconds;
    updated.clipboardPaste = options.clipboardPaste;
    updated.clipboardWrite = options.clipboardWrite;
    updated.enquiryResponse = options.enquiryResponse;
    updated.splitAppearance = options.splitAppearance;
    updated.rightClickAction = options.rightClickAction;
    updated.middleClickAction = options.middleClickAction;
    updated.mouseHideWhileTyping = options.mouseHideWhileTyping;
    updated.focusFollowsMouse = options.focusFollowsMouse;
    updated.mouseReporting = options.mouseReporting;
    updated.mouseShiftCapture = options.mouseShiftCapture;
    updated.mouseScrollMultiplier = options.mouseScrollMultiplier;
    updated.horizontalTabScroll = options.horizontalTabScroll;
    updated.vtKamAllowed = options.vtKamAllowed;
    updated.linkUrl = options.linkUrl;
    updated.linkPreviews = options.linkPreviews;
    updated.scrollbackCompression = options.scrollbackCompression;
    updated.kittyImageStorageLimitBytes = options.kittyImageStorageLimitBytes;
    updated.scrollToBottom = options.scrollToBottom;
    updated.abnormalCommandExitRuntimeMilliseconds =
        options.abnormalCommandExitRuntimeMilliseconds;
    updated.waitAfterCommand = options.waitAfterCommand;
    updated.resizeOverlay = options.resizeOverlay;
    updated.customShaders = options.customShaders;
    updated.keybindSource = options.keybindSource;
    updated.modifierRemaps = options.modifierRemaps;

    controller_->applyConfiguredTitle(updated.configuredTitle);
    // titleChanged observers may synchronously apply a newer snapshot. The
    // keybind generation cannot participate in this guard because the matcher
    // is deliberately replaced only after the pane snapshot becomes current.
    if (guard == nullptr
        || !guard->runtimeOptionsRevision_.isCurrent(revision)) {
        return;
    }
    // Re-read same-path shader edits on every successful configuration
    // application. Content-addressed compilation makes unchanged sources a
    // cheap cache hit while fixing Ghostty's pinned path-equality blind spot.
    reloadCustomShaders(updated.customShaders);
    if (guard == nullptr
        || !guard->runtimeOptionsRevision_.isCurrent(revision)) {
        return;
    }

    const bool keybindGenerationChanged =
        !keybinds_.program().isSameGeneration(keybindProgram);
    const bool modifierRemapsChanged =
        options_.modifierRemaps != updated.modifierRemaps;
    LaunchOptions updatedWithoutTitle = updated;
    updatedWithoutTitle.configuredTitle = options_.configuredTitle;
    if (updatedWithoutTitle == options_ && !keybindGenerationChanged) {
        // Keep GUI-only title changes on the fast path. A configuration reload
        // must not recompute fonts, images, layout, or renderer state merely
        // because its base-title policy changed.
        options_.configuredTitle = updated.configuredTitle;
        if (!updated.mouseHideWhileTyping && mouseHiddenWhileTyping_) {
            setMouseHiddenWhileTyping(false);
            if (!stillCurrentUpdate()) return;
        }
        // options_ is the configured snapshot; the controller owns this
        // mutable pane-local policy. Reapplying an unchanged snapshot must
        // still replace an action-originated toggle.
        controller_->setMouseReportingEnabled(updated.mouseReporting);
        return;
    }

    const bool previewWasPointerCaptured = linkPreviewPointerCaptured_;
    const bool linkUrlChanged = options_.linkUrl != updated.linkUrl;
    const bool linkPreviewModeChanged =
        options_.linkPreviews != updated.linkPreviews;
    const bool mouseShiftCaptureChanged =
        options_.mouseShiftCapture != updated.mouseShiftCapture;
    const bool mouseHidePolicyChanged =
        options_.mouseHideWhileTyping != updated.mouseHideWhileTyping;
    const bool horizontalTabScrollChanged =
        options_.horizontalTabScroll != updated.horizontalTabScroll;
    const BellFeatures previousBellFeatures = options_.bellFeatures;
    const ResizeOverlayOptions previousResizeOverlay = options_.resizeOverlay;
    const bool paddingLayoutChanged =
        options_.padding.balance != updated.padding.balance;
    const TerminalSessionRuntimeOptions previousRuntime =
        toTerminalSessionRuntimeOptions(options_);
    const TerminalSessionRuntimeOptions updatedRuntime =
        toTerminalSessionRuntimeOptions(updated);

    TerminalCellMetrics previousMetrics;
    {
        QMutexLocker locker(&renderMutex_);
        previousMetrics = metrics_;
    }
    const qreal previousPointSize =
        previousMetrics.font(TerminalFontRole::Regular).pointSizeF();
    defaultFontPointSize_ = updated.typography.pointSize;
    const qreal reloadedFontSize = manuallyZoomed_
        ? previousPointSize
        : static_cast<qreal>(clampFontActionValue(
              static_cast<float>(updated.typography.pointSize),
              kMinimumActionFontSize));
    TerminalTypography previousEffectiveTypography = options_.typography;
    previousEffectiveTypography.pointSize = previousPointSize;
    TerminalTypography reloadedEffectiveTypography = updated.typography;
    reloadedEffectiveTypography.pointSize = reloadedFontSize;
    // Font database discovery and mapped-face construction are relatively
    // expensive GUI-thread work. Unrelated live reloads retain the resolved
    // generation verbatim.
    const bool metricsChanged =
        previousEffectiveTypography != reloadedEffectiveTypography
        && updateMetrics(updated.typography, reloadedFontSize);
    bool terminalGeometryChanged = false;
    bool pointSizeChanged = false;
    {
        QMutexLocker locker(&renderMutex_);
        terminalGeometryChanged =
            terminalGridMetricsChanged(previousMetrics, metrics_);
        pointSizeChanged = !qFuzzyCompare(
            previousPointSize,
            metrics_.font(TerminalFontRole::Regular).pointSizeF());
        if (!hasFrame_) {
            frame_.foreground = updated.appearance.foregroundColor;
            frame_.background = updated.appearance.backgroundColor;
            frame_.palette = updated.appearance.palette;
            frame_.cursorColor =
                updated.appearance.cursorColor.kind == TerminalColorKind::Color
                    && updated.appearance.cursorColor.color.isValid()
                ? updated.appearance.cursorColor.color
                : updated.appearance.foregroundColor;
        }
        appearance_ = updated.appearance;
        backgroundOptions_ = updated.background;
        paddingOptions_ = updated.padding;
        splitAppearance_ = updated.splitAppearance;
    }
    options_ = updated;
    if (horizontalTabScrollChanged) {
        pendingHorizontalTabScrollPixels_ = 0.0;
        horizontalTabScrollResetTimer_->stop();
    }
    // Order every policy transition ahead of pending performable fallbacks.
    // Enabling must not retroactively make an older key eligible, while
    // disabling must invalidate an older hide even if a reentrant same-policy
    // update supersedes this revision before cursor presentation.
    if (mouseHidePolicyChanged) {
        ++pointerActivityEpoch_;
    }

    quint64 previousSequenceToken = 0;
    bool keyTablesChanged = false;
    bool keySequenceChanged = false;
    if (keybindGenerationChanged || modifierRemapsChanged) {
        previousSequenceToken = std::exchange(activeSequenceToken_, 0);
        keyTablesChanged = keybinds_.hasActiveTables();
        keySequenceChanged = keybinds_.sequenceActive();
        if (keybindGenerationChanged) {
            (void)keybinds_.replaceProgram(keybindProgram);
        } else {
            (void)keybinds_.deactivateAllTables();
            keybinds_.resetSequence();
        }
        if (modifierRemapsChanged) {
            modifierRemaps_.replaceMappings(options_.modifierRemaps);
        }
    }

    // The pane snapshot and matcher are now authoritative. Queue a changed
    // worker policy before publishing any callback that may execute a new
    // binding; GUI-only updates such as configured-title never wake it.
    if (previousRuntime != updatedRuntime) {
        controller_->applyRuntimeOptions(updatedRuntime);
        if (guard == nullptr) return;
    }
    if (keyTablesChanged) {
        Q_EMIT activeKeyTablesChanged();
        if (guard == nullptr) return;
    }
    if (keySequenceChanged) {
        Q_EMIT pendingKeySequenceChanged();
        if (guard == nullptr) return;
    }
    // Always release the old worker staging, even when an earlier observer
    // installed a newer generation. A newly staged token safely supersedes
    // this one and makes the old resolution a no-op.
    if (previousSequenceToken != 0) {
        guard->controller_->resolveSequence(previousSequenceToken,
                                            TerminalSequenceResolution::Drop);
        if (guard == nullptr) return;
    }
    if (!stillCurrentUpdate()) return;

    if (!options_.mouseHideWhileTyping && mouseHiddenWhileTyping_) {
        setMouseHiddenWhileTyping(false);
        if (!stillCurrentUpdate()) return;
    }

    controller_->setMouseReportingEnabled(options_.mouseReporting);
    if (!stillCurrentUpdate()) return;

    updateScrollbarState();
    if (!stillCurrentUpdate()) return;

    if (bellRinging_ && previousBellFeatures != options_.bellFeatures) {
        Q_EMIT bellChanged();
        if (!stillCurrentUpdate()) return;
    }

    if (previousResizeOverlay.position != options_.resizeOverlay.position) {
        Q_EMIT resizeOverlayRectChanged();
        if (!stillCurrentUpdate()) return;
    }
    if (options_.resizeOverlay.mode == ResizeOverlayMode::Never) {
        cancelPendingResizeOverlay();
        hideResizeOverlay();
    } else if (resizeOverlayVisible_
               && previousResizeOverlay.duration
                   != options_.resizeOverlay.duration) {
        restartResizeOverlayTimer();
    }
    if (!stillCurrentUpdate()) return;
    if (linkUrlChanged && !options.linkUrl) {
        if (hyperlinkPressKind_ == TerminalLinkKind::Regex) {
            cancelHyperlinkPress();
            if (!stillCurrentUpdate()) return;
        }
        if (pendingActivationKind_ == TerminalLinkKind::Regex) {
            cancelPendingHyperlinkActivation();
            if (!stillCurrentUpdate()) return;
        }
    }
    if (linkUrlChanged || mouseShiftCaptureChanged) {
        clearHyperlinkHover();
        if (!stillCurrentUpdate()) return;
    }
    if (linkUrlChanged || mouseShiftCaptureChanged) {
        recomputeHyperlinkHover();
        if (!stillCurrentUpdate()) return;
    }

    if (terminalGeometryChanged || paddingLayoutChanged) {
        updateTerminalSize();
        if (!stillCurrentUpdate()) return;
    }
    if (linkPreviewModeChanged || metricsChanged) {
        refreshLinkPreview();
        if (!stillCurrentUpdate()) return;
        reconcileReleasedLinkPreview(previewWasPointerCaptured);
        if (!stillCurrentUpdate()) return;
    }
    refreshBackgroundImage();
    if (!stillCurrentUpdate()) return;
    requestRenderUpdate();
    if (pointSizeChanged) {
        Q_EMIT fontPointSizeChanged();
        if (!stillCurrentUpdate()) return;
    }
}

void TerminalPane::setSplit(bool split)
{
    if (split_ == split) {
        return;
    }
    split_ = split;
    requestRenderUpdate();
}

void TerminalPane::setWorkspaceActionHandler(
    std::function<bool(WorkspaceActionRequest)> handler)
{
    workspaceActionHandler_ = handler
        ? std::make_shared<std::function<bool(WorkspaceActionRequest)>>(
              std::move(handler))
        : nullptr;
}

void TerminalPane::setUrlOpener(std::function<bool(const QUrl &)> opener)
{
    urlOpener_ = std::move(opener);
}

void TerminalPane::setBellPlaybackDevice(
    std::unique_ptr<TerminalBellDevice> device)
{
    bellPlayer_->setDevice(std::move(device));
}

void TerminalPane::beginShutdown()
{
    const QPointer<TerminalPane> guard(this);
    if (terminalActionsAccepted_) {
        terminalActionsAccepted_ = false;
        advanceTerminalActionEpoch();
    }
    pendingRightClickWindowPositions_.clear();
    newestRightClickRequestId_ = 0;
    // Queue worker teardown before resolving a suspended action chain. Its
    // completion may release pane- or process-level input deferral and replay
    // key/IME work; worker queue ordering then makes that replay inert instead
    // of delivering it to a dying PTY.
    controller_->beginShutdown();
    if (guard == nullptr) return;
    failStaleTerminalActionCompletions();
    if (guard == nullptr) return;
    deferredSessionStartArmed_ = false;
    deferredSessionStartCandidate_.reset();
    deferredSessionViewportSizeProvider_ = {};
    disconnectDeferredSessionWindowSignals();
    resizeOverlayShuttingDown_ = true;
    cancelPendingResizeOverlay();
    hideResizeOverlay();
}

void TerminalPane::startSearchUi()
{
    const QPointer<TerminalPane> guard(this);
    if (!searchUiActive_) {
        setSearchUiActive(true);
        if (guard == nullptr || !searchUiActive_) return;
        // Ghostty retains the last entry text. Reopening the UI starts a fresh
        // generation so results cannot outlive terminal mutations that
        // happened while the overlay was hidden.
        controller_->search(searchUiText_);
        if (guard == nullptr || !searchUiActive_) return;
    }
    Q_EMIT searchUiFocusRequested();
}

void TerminalPane::startSearchUiWithSelection(const QString &text)
{
    const QPointer<TerminalPane> guard(this);
    const bool wasActive = searchUiActive_;
    setSearchUiActive(true);
    if (guard == nullptr || !searchUiActive_) return;
    if (!wasActive) {
        controller_->search(searchUiText_);
        if (guard == nullptr || !searchUiActive_) return;
    }
    if (!text.isEmpty()) {
        const bool textChanged = searchUiText_ != text;
        searchUiText_ = text;
        if (textChanged) {
            Q_EMIT searchUiTextChanged();
            if (guard == nullptr || !searchUiActive_ || searchUiText_ != text) {
                return;
            }
        }
        controller_->search(text);
        if (guard == nullptr || !searchUiActive_) return;
    }
    Q_EMIT searchUiFocusRequested();
}

void TerminalPane::setSearchUiActive(bool active)
{
    if (searchUiActive_ == active) {
        return;
    }
    searchUiActive_ = active;
    requestRenderUpdate();
    Q_EMIT searchUiActiveChanged();
}

void TerminalPane::setSearchUiText(const QString &text)
{
    if (searchUiText_ == text) {
        return;
    }
    searchUiText_ = text;
    Q_EMIT searchUiTextChanged();
    controller_->search(searchUiText_);
}

void TerminalPane::endSearchUi()
{
    setSearchUiActive(false);
    searchEngineActive_ = false;
    if (searchMatchLabel_ != QLatin1StringView("0/0")) {
        searchMatchLabel_ = QStringLiteral("0/0");
        Q_EMIT searchMatchLabelChanged();
    }
    {
        QMutexLocker locker(&renderMutex_);
        clearPendingSearchUpdateLocked();
        clearSearchDecorationsLocked();
    }
    controller_->cancelSearch();
    requestRenderUpdate();
    forceActiveFocus(Qt::ShortcutFocusReason);
}

void TerminalPane::navigateSearch(int direction)
{
    controller_->navigateSearch(direction < 0
                                    ? TerminalSearchDirection::Previous
                                    : TerminalSearchDirection::Next);
}

void TerminalPane::clearSearchDecorationsLocked()
{
    markSolidMaskRowsChangedLocked(
        searchCandidateCellMask_, searchDecorationColumns_,
        searchDecorationRows_, {}, searchDecorationColumns_,
        searchDecorationRows_);
    markSolidMaskRowsChangedLocked(
        searchSelectedCellMask_, searchDecorationColumns_,
        searchDecorationRows_, {}, searchDecorationColumns_,
        searchDecorationRows_);
    searchCandidateCellMask_.clear();
    searchSelectedCellMask_.clear();
    searchDecorationRevision_ = 0;
    searchDecorationColumns_ = 0;
    searchDecorationRows_ = 0;
}

void TerminalPane::clearPendingSearchUpdateLocked()
{
    pendingSearchUpdate_.reset();
}

void TerminalPane::installSearchDecorationsLocked(
    const TerminalSearchUpdate &searchUpdate)
{
    QBitArray candidateCellMask;
    QBitArray selectedCellMask;
    if (!searchUpdate.active || searchUpdate.columns <= 0
        || searchUpdate.rows <= 0) {
        // Keep both masks empty.
    } else {
        const qsizetype columnCount = searchUpdate.columns;
        const qsizetype maskSize = searchUpdate.visibleCellMask.size();
        const bool masksEmpty =
            maskSize == 0 && searchUpdate.selectedCellMask.isEmpty();
        const bool masksMatchGrid = maskSize > 0
            && maskSize == searchUpdate.selectedCellMask.size()
            && maskSize % columnCount == 0
            && maskSize / columnCount == searchUpdate.rows;
        if (masksEmpty || masksMatchGrid) {
            candidateCellMask = searchUpdate.visibleCellMask;
            selectedCellMask = searchUpdate.selectedCellMask;
        }
    }
    markSolidMaskRowsChangedLocked(searchCandidateCellMask_,
                                   searchDecorationColumns_,
                                   searchDecorationRows_, candidateCellMask,
                                   searchUpdate.columns, searchUpdate.rows);
    markSolidMaskRowsChangedLocked(searchSelectedCellMask_,
                                   searchDecorationColumns_,
                                   searchDecorationRows_, selectedCellMask,
                                   searchUpdate.columns, searchUpdate.rows);
    searchCandidateCellMask_ = std::move(candidateCellMask);
    searchSelectedCellMask_ = std::move(selectedCellMask);
    searchDecorationRevision_ = searchUpdate.contentRevision;
    searchDecorationColumns_ = searchUpdate.columns;
    searchDecorationRows_ = searchUpdate.rows;
}

void TerminalPane::handleSearchUpdate(const TerminalSearchUpdate &searchUpdate)
{
    searchEngineActive_ = searchUpdate.active;
    const QString nextLabel = searchUpdate.active
        ? QStringLiteral("%1/%2")
              .arg(searchUpdate.selectedMatch >= 0
                       ? searchUpdate.selectedMatch + 1
                       : 0)
              .arg(searchUpdate.totalMatches)
        : QStringLiteral("0/0");
    if (searchMatchLabel_ != nextLabel) {
        searchMatchLabel_ = nextLabel;
        Q_EMIT searchMatchLabelChanged();
    }

    {
        QMutexLocker locker(&renderMutex_);
        if (!searchUpdate.active) {
            clearPendingSearchUpdateLocked();
            clearSearchDecorationsLocked();
        } else if (hasFrame_
                   && searchUpdate.contentRevision == frame_.contentRevision
                   && searchUpdate.columns == frame_.columns
                   && searchUpdate.rows == frame_.rows) {
            installSearchDecorationsLocked(searchUpdate);
            clearPendingSearchUpdateLocked();
        } else if (!hasFrame_
                   || searchUpdate.contentRevision > frame_.contentRevision) {
            pendingSearchUpdate_ = searchUpdate;
            clearSearchDecorationsLocked();
        }
    }
    requestRenderUpdate();
}

void TerminalPane::geometryChange(const QRectF &newGeometry,
                                  const QRectF &oldGeometry)
{
    const bool previewWasPointerCaptured = linkPreviewPointerCaptured_;
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        if (renderItem_ != nullptr) {
            renderItem_->setSize(newGeometry.size());
        }
        syncCustomShaderStageGeometry();
        Q_EMIT resizeOverlayRectChanged();
        deferredSessionStartCandidate_.reset();
        updateTerminalSize();
        scheduleDeferredSessionStart();
        refreshLinkPreview();
        reconcileReleasedLinkPreview(previewWasPointerCaptured);
        requestRenderUpdate();
    }
}

bool TerminalPane::updateMetrics()
{
    return updateMetrics(options_.typography, fontPointSize());
}

bool TerminalPane::updateMetrics(const TerminalTypography &typography,
                                 qreal pointSize)
{
    TerminalTypography effective = typography;
    effective.pointSize = pointSize;
    const TerminalCellMetrics next = terminalCellMetrics(
        effective, window() != nullptr ? window()->devicePixelRatio() : 1.0);

    QMutexLocker locker(&renderMutex_);
    if (metrics_ == next) {
        return false;
    }
    metrics_ = next;
    return true;
}

void TerminalPane::refreshResolvedFonts()
{
    const bool previewWasPointerCaptured = linkPreviewPointerCaptured_;
    TerminalCellMetrics previous;
    {
        QMutexLocker locker(&renderMutex_);
        previous = metrics_;
    }
    if (!updateMetrics()) {
        return;
    }

    TerminalCellMetrics current;
    {
        QMutexLocker locker(&renderMutex_);
        current = metrics_;
    }
    if (terminalGridMetricsChanged(previous, current)) {
        updateTerminalSize();
    }
    refreshLinkPreview();
    reconcileReleasedLinkPreview(previewWasPointerCaptured);
    requestRenderUpdate();
    if (!qFuzzyCompare(previous.font(TerminalFontRole::Regular).pointSizeF(),
                       current.font(TerminalFontRole::Regular).pointSizeF())) {
        Q_EMIT fontPointSizeChanged();
    }
}

void TerminalPane::markTextRowsChangedLocked(const TerminalUpdate &update)
{
    if (!update.fullFrame && update.dirtyRows.isEmpty()) {
        return;
    }

    ++textRowEpoch_;

    if (update.fullFrame || textRowEpochs_.size() != frame_.rows) {
        textRowEpochs_.fill(textRowEpoch_, frame_.rows);
        return;
    }
    for (const TerminalRowUpdate &row : update.dirtyRows) {
        textRowEpochs_[row.row] = textRowEpoch_;
    }
}

void TerminalPane::markSolidRowsChangedLocked(const TerminalUpdate &update)
{
    if (!update.fullFrame && update.dirtyRows.isEmpty()) {
        return;
    }

    ++solidRowEpoch_;

    if (update.fullFrame || solidRowEpochs_.size() != frame_.rows) {
        solidRowEpochs_.fill(solidRowEpoch_, frame_.rows);
        return;
    }
    for (const TerminalRowUpdate &row : update.dirtyRows) {
        solidRowEpochs_[row.row] = solidRowEpoch_;
    }
}

void TerminalPane::markSolidMaskRowsChangedLocked(const QBitArray &oldMask,
                                                  int oldColumns, int oldRows,
                                                  const QBitArray &newMask,
                                                  int newColumns, int newRows)
{
    if (oldMask.isEmpty() && newMask.isEmpty()) {
        return;
    }
    if (oldMask == newMask && oldColumns == newColumns && oldRows == newRows) {
        return;
    }

    ++solidRowEpoch_;
    const bool hasOldMask = !oldMask.isEmpty();
    const bool hasNewMask = !newMask.isEmpty();
    const bool oldMaskMatchesShape = !hasOldMask
        || (oldColumns > 0 && oldRows > 0
            && oldMask.size() == static_cast<qsizetype>(oldColumns) * oldRows);
    const bool newMaskMatchesShape = !hasNewMask
        || (newColumns > 0 && newRows > 0
            && newMask.size() == static_cast<qsizetype>(newColumns) * newRows);
    const int columns = hasNewMask ? newColumns : oldColumns;
    const int rows = hasNewMask ? newRows : oldRows;
    if (solidRowEpochs_.size() != frame_.rows || !oldMaskMatchesShape
        || !newMaskMatchesShape
        || (hasOldMask && hasNewMask
            && (oldColumns != newColumns || oldRows != newRows))
        || columns != frame_.columns || rows != frame_.rows) {
        solidRowEpochs_.fill(solidRowEpoch_, frame_.rows);
        return;
    }

    for (int row = 0; row < rows; ++row) {
        const qsizetype rowStart = static_cast<qsizetype>(row) * columns;
        bool rowChanged = false;
        for (int column = 0; column < columns; ++column) {
            const qsizetype index = rowStart + column;
            const bool oldValue =
                index < oldMask.size() && oldMask.testBit(index);
            const bool newValue =
                index < newMask.size() && newMask.testBit(index);
            if (oldValue != newValue) {
                rowChanged = true;
                break;
            }
        }
        if (rowChanged && row < solidRowEpochs_.size()) {
            solidRowEpochs_[row] = solidRowEpoch_;
        }
    }
}

void TerminalPane::refreshBackgroundImage()
{
    TerminalBackgroundImageRequest request;
    {
        QMutexLocker locker(&renderMutex_);
        const auto &source = backgroundOptions_.image.path;
        if (!source.has_value()) {
            backgroundImageRequest_.cancel();
            ++backgroundImageRequestGeneration_;
            backgroundImageSourceRequest_.reset();
            failedBackgroundImageSource_.reset();
            if (backgroundImageAsset_) {
                backgroundImageAsset_.reset();
                locker.unlock();
                requestRenderUpdate();
            }
            return;
        }
        if (failedBackgroundImageSource_.has_value()
            && *failedBackgroundImageSource_ != *source) {
            failedBackgroundImageSource_.reset();
        } else if (failedBackgroundImageSource_ == *source) {
            return;
        }
        request = {
            .source = *source,
        };
        if (backgroundImageSourceRequest_ == request) {
            return;
        }
        backgroundImageSourceRequest_ = request;
    }

    const quint64 generation = ++backgroundImageRequestGeneration_;
    backgroundImageRequest_ = requestTerminalBackgroundImage(
        request, this,
        [this, generation,
         source = request.source](TerminalBackgroundImageResult result) {
            if (generation != backgroundImageRequestGeneration_) return;
            if (!result) {
                // Pinned Ghostty reports background-image failures for both
                // required and optional path provenance.
                qWarning().noquote() << result.error();
                QMutexLocker locker(&renderMutex_);
                failedBackgroundImageSource_ = source;
                return;
            }
            {
                QMutexLocker locker(&renderMutex_);
                failedBackgroundImageSource_.reset();
                backgroundImageAsset_ = std::move(*result);
            }
            requestRenderUpdate();
        });
}

void TerminalPane::syncCursorBlink(bool resetPhase)
{
    bool shouldBlink = false;
    {
        QMutexLocker locker(&renderMutex_);
        shouldBlink =
            hasFrame_ && frame_.cursorVisible && frame_.cursorBlinking;
    }
    shouldBlink = shouldBlink && hasActiveFocus();

    if (!shouldBlink) {
        cursorTimer_->stop();
        cursorBlinkOn_ = true;
    } else if (resetPhase || !cursorTimer_->isActive()) {
        cursorBlinkOn_ = true;
        cursorTimer_->start();
    }
    requestRenderUpdate();
}

void TerminalPane::updateTerminalSize()
{
    if (controller_ == nullptr) return;

    const std::optional<TerminalSessionGeometry> geometry =
        currentSessionGeometry();
    if (!geometry.has_value()) {
        clearHyperlinkHover();
        cancelHyperlinkPress();
        return;
    }
    qreal cellWidth = 0.0;
    qreal cellHeight = 0.0;
    {
        QMutexLocker locker(&renderMutex_);
        cellWidth = metrics_.cellWidth;
        cellHeight = metrics_.cellHeight;
    }
    if (width() < cellWidth || height() < cellHeight) {
        clearHyperlinkHover();
        cancelHyperlinkPress();
    }
    {
        QMutexLocker locker(&renderMutex_);
        terminalRows_ = geometry->rows;
        terminalResizePending_ = true;
    }
    noteTerminalGridSize(*geometry);
    controller_->resizeTerminal(*geometry);
}

void TerminalPane::noteTerminalGridSize(const TerminalSessionGeometry &geometry)
{
    const QSize grid(geometry.columns, geometry.rows);
    const bool firstGrid = !resizeOverlayGrid_.has_value();
    if (!firstGrid && *resizeOverlayGrid_ == grid) {
        return;
    }
    resizeOverlayGrid_ = grid;

    // Deferred windows may traverse hidden normal and compositor geometries
    // before their worker exists. Those are launch negotiation, not resizes.
    if (controller_ == nullptr || !controller_->sessionStarted()) {
        return;
    }
    if (options_.resizeOverlay.mode == ResizeOverlayMode::AfterFirst
        && (firstGrid
            || std::chrono::steady_clock::now()
                < resizeOverlayStartupSuppressionEnds_)) {
        return;
    }
    scheduleResizeOverlay();
}

void TerminalPane::scheduleResizeOverlay()
{
    if (options_.resizeOverlay.mode == ResizeOverlayMode::Never
        || resizeOverlayShuttingDown_ || resizeOverlayUpdateScheduled_
        || !resizeOverlayGrid_.has_value()) {
        return;
    }
    resizeOverlayUpdateScheduled_ = true;
    const quint64 generation = ++resizeOverlayUpdateGeneration_;
    QTimer::singleShot(0, this, [this, generation] {
        if (generation != resizeOverlayUpdateGeneration_) {
            return;
        }
        resizeOverlayUpdateScheduled_ = false;
        showPendingResizeOverlay();
    });
}

void TerminalPane::cancelPendingResizeOverlay()
{
    if (!resizeOverlayUpdateScheduled_) {
        return;
    }
    resizeOverlayUpdateScheduled_ = false;
    ++resizeOverlayUpdateGeneration_;
}

void TerminalPane::showPendingResizeOverlay()
{
    if (options_.resizeOverlay.mode == ResizeOverlayMode::Never
        || resizeOverlayShuttingDown_ || controller_ == nullptr
        || !controller_->sessionStarted() || !resizeOverlayGrid_.has_value()) {
        return;
    }

    const QString text = QStringLiteral("%1 x %2")
                             .arg(resizeOverlayGrid_->width())
                             .arg(resizeOverlayGrid_->height());
    if (resizeOverlayText_ != text) {
        resizeOverlayText_ = text;
        Q_EMIT resizeOverlayTextChanged();
    }
    if (!resizeOverlayVisible_) {
        resizeOverlayVisible_ = true;
        Q_EMIT resizeOverlayVisibleChanged();
    }
    restartResizeOverlayTimer();
}

void TerminalPane::hideResizeOverlay()
{
    if (resizeOverlayTimer_ != nullptr) {
        resizeOverlayTimer_->stop();
    }
    if (!resizeOverlayVisible_) {
        return;
    }
    resizeOverlayVisible_ = false;
    Q_EMIT resizeOverlayVisibleChanged();
}

void TerminalPane::restartResizeOverlayTimer()
{
    if (resizeOverlayTimer_ == nullptr) {
        return;
    }
    resizeOverlayTimer_->setInterval(std::max(options_.resizeOverlay.duration,
                                              kMinimumResizeOverlayDuration));
    resizeOverlayTimer_->start();
}

std::optional<TerminalSessionGeometry>
TerminalPane::currentSessionGeometry(std::optional<QSizeF> viewportSize) const
{
    return currentViewportLayout(viewportSize)
        .transform([](const TerminalViewportLayout &layout) {
            return layout.session;
        });
}

std::optional<TerminalViewportLayout>
TerminalPane::currentViewportLayout(std::optional<QSizeF> viewportSize) const
{
    qreal cellWidth = 0.0;
    qreal cellHeight = 0.0;
    TerminalPaddingOptions padding;
    {
        QMutexLocker locker(&renderMutex_);
        cellWidth = metrics_.cellWidth;
        cellHeight = metrics_.cellHeight;
        padding = paddingOptions_;
    }
    const QSizeF viewport = viewportSize.value_or(size());
    const qreal devicePixelRatio =
        window() != nullptr ? window()->devicePixelRatio() : 1.0;
    const TerminalViewportSpec spec{
        .surfaceSize = viewport,
        .cellSize = QSizeF(cellWidth, cellHeight),
        .devicePixelRatio = devicePixelRatio,
        .padding = padding,
    };
    if (viewportLayoutCacheSpec_ != spec) {
        viewportLayoutCacheSpec_ = spec;
        viewportLayoutCache_ = terminalViewportLayout(spec);
    }
    return viewportLayoutCache_;
}

void TerminalPane::beginKeyEventDeferral() noexcept
{
    ++keyEventDeferralDepth_;
}

void TerminalPane::endKeyEventDeferral()
{
    Q_ASSERT(keyEventDeferralDepth_ > 0);
    --keyEventDeferralDepth_;
    if (keyEventDeferralDepth_ == 0 && keyEventDispatchDepth_ == 0) {
        drainDeferredKeyEvents();
    }
}

void TerminalPane::beginKeyEventDispatch() noexcept
{
    ++keyEventDispatchDepth_;
}

void TerminalPane::endKeyEventDispatch()
{
    Q_ASSERT(keyEventDispatchDepth_ > 0);
    --keyEventDispatchDepth_;
    if (keyEventDispatchDepth_ == 0 && keyEventDeferralDepth_ == 0) {
        drainDeferredKeyEvents();
    }
}

bool TerminalPane::deferKeyEventIfNeeded(const QKeyEvent &event)
{
    const bool isCurrentReplay = replayingDeferredKeyEvent_ == &event;
    if (keyEventDeferralDepth_ == 0
        && (!drainingDeferredKeyEvents_ || isCurrentReplay)) {
        return false;
    }
    deferKeyEvent(event);
    return true;
}

void TerminalPane::deferKeyEvent(const QKeyEvent &event)
{
    deferredInputs_.emplace_back(DeferredKeyInput{
        .event = KeyEventSnapshot::capture(event),
        .focusEpoch = keyFocusEpoch_,
        .pointerActivityEpoch = pointerActivityEpoch_,
    });
}

void TerminalPane::drainDeferredKeyEvents()
{
    if (keyEventDeferralDepth_ != 0 || keyEventDispatchDepth_ != 0
        || drainingDeferredKeyEvents_) {
        return;
    }

    const QPointer<TerminalPane> guard(this);
    drainingDeferredKeyEvents_ = true;
    while (guard != nullptr && guard->keyEventDeferralDepth_ == 0
           && !guard->deferredInputs_.empty()) {
        DeferredPaneInput input = std::move(guard->deferredInputs_.front());
        guard->deferredInputs_.pop_front();
        if (const auto *key = std::get_if<DeferredKeyInput>(&input)) {
            QKeyEvent replay = key->event.replay();
            guard->replayingDeferredKeyEvent_ = &replay;
            guard->replayingDeferredPointerActivityEpoch_ =
                key->pointerActivityEpoch;
            QCoreApplication::sendEvent(guard, &replay);
            if (guard != nullptr) {
                guard->replayingDeferredKeyEvent_ = nullptr;
                guard->replayingDeferredPointerActivityEpoch_.reset();
            }
        } else {
            guard->controller_->sendInputMethod(
                std::get<TerminalInputMethodInput>(input));
        }
    }
    if (guard != nullptr) guard->drainingDeferredKeyEvents_ = false;
}

void TerminalPane::keyPressEvent(QKeyEvent *event)
{
    const quint64 pointerActivityEpoch =
        replayingDeferredPointerActivityEpoch_.value_or(pointerActivityEpoch_);
    // The original interaction clears the alert before it may be deferred.
    // Replaying that old press must not clear a newer BEL received meanwhile.
    if (replayingDeferredKeyEvent_ != event
        && !isModifierOnlyKey(event->key())) {
        const QPointer<TerminalPane> guard(this);
        setBellRinging(false);
        if (guard == nullptr) {
            event->accept();
            return;
        }
    }
    if (deferKeyEventIfNeeded(*event)) {
        event->accept();
        return;
    }
    const QPointer<TerminalPane> guard(this);
    beginKeyEventDispatch();
    const auto dispatchGuard = qScopeGuard([guard] {
        if (guard != nullptr) guard->endKeyEventDispatch();
    });
    const KeyEventSnapshot remappedSnapshot =
        modifierRemaps_.remapEvent(KeyEventSnapshot::capture(*event));
    QKeyEvent remappedEvent = remappedSnapshot.replay();
    const KeyHandling handling =
        handleShortcut(&remappedEvent, guard, pointerActivityEpoch);
    // Lifecycle actions are owner-deferred, but an embedding application may
    // still attach a destructive direct observer to another pane signal.
    // Never resume ordinary key handling through a deleted QObject.
    if (guard == nullptr) {
        event->accept();
        return;
    }
    // Run configured actions against the previously accepted hover before a
    // chord's non-modifier key refreshes link eligibility. This keeps
    // copy_url_to_clipboard synchronous within Ghostty action chains.
    if (handling != KeyHandling::PassThrough
        || !controller_->keyboardInputSuppressed()) {
        updateHyperlinkModifiers(modifiersAfterKeyEvent(event, true));
    }
    if (guard == nullptr) {
        event->accept();
        return;
    }
    if (handling != KeyHandling::PassThrough) {
        if (handling == KeyHandling::ConsumePressAndRelease) {
            consumedKeys_.insert(keyEventIdentity(event));
        }
        event->accept();
        return;
    }

    const TerminalKeyInput input = terminalKeyInput(&remappedEvent);
    hideMouseForTerminalKey(input, pointerActivityEpoch);
    if (guard == nullptr) {
        event->accept();
        return;
    }
    controller_->sendKey(input);
    event->accept();
}

void TerminalPane::keyReleaseEvent(QKeyEvent *event)
{
    if (deferKeyEventIfNeeded(*event)) {
        event->accept();
        return;
    }
    const QPointer<TerminalPane> guard(this);
    beginKeyEventDispatch();
    const auto dispatchGuard = qScopeGuard([guard] {
        if (guard != nullptr) guard->endKeyEventDispatch();
    });
    const KeyEventSnapshot remappedSnapshot =
        modifierRemaps_.remapEvent(KeyEventSnapshot::capture(*event));
    QKeyEvent remappedEvent = remappedSnapshot.replay();
    if (!controller_->keyboardInputSuppressed()) {
        updateHyperlinkModifiers(modifiersAfterKeyEvent(event, false));
    }
    if (guard == nullptr) {
        event->accept();
        return;
    }
    if (consumedKeys_.remove(keyEventIdentity(event))) {
        event->accept();
        return;
    }
    controller_->sendKey(terminalKeyInput(&remappedEvent, false));
    event->accept();
}

TerminalPane::KeyHandling
TerminalPane::handleShortcut(QKeyEvent *event,
                             const QPointer<TerminalPane> &guard,
                             quint64 pointerActivityEpoch)
{
    if (keybinds_.program().isAvailable()) {
        return handleConfiguredShortcut(event, guard, pointerActivityEpoch);
    }

    const Qt::KeyboardModifiers modifiers =
        normalizedModifiers(event->modifiers());
    const bool control = modifiers.testFlag(Qt::ControlModifier);
    const bool shift = modifiers.testFlag(Qt::ShiftModifier);
    const int key = event->key();

    if (control && shift
        && modifiers == (Qt::ControlModifier | Qt::ShiftModifier)) {
        switch (key) {
        case Qt::Key_C:
            copySelection();
            return KeyHandling::ConsumePressAndRelease;
        case Qt::Key_V: {
            const QString text = QGuiApplication::clipboard()->text();
            if (!text.isEmpty()) {
                pasteText(text);
            }
            return KeyHandling::ConsumePressAndRelease;
        }
        case Qt::Key_N:
            Q_EMIT applicationActionRequested(ApplicationAction::NewWindow);
            return KeyHandling::ConsumePressAndRelease;
        case Qt::Key_T:
            Q_EMIT requestNewTab();
            return KeyHandling::ConsumePressAndRelease;
        case Qt::Key_O:
            Q_EMIT requestSplit(WorkspaceAction::SplitRight);
            return KeyHandling::ConsumePressAndRelease;
        case Qt::Key_E:
            Q_EMIT requestSplit(WorkspaceAction::SplitDown);
            return KeyHandling::ConsumePressAndRelease;
        case Qt::Key_W:
            Q_EMIT requestCloseTab(CloseTabMode::This);
            return KeyHandling::ConsumePressAndRelease;
        case Qt::Key_Q:
            Q_EMIT applicationActionRequested(ApplicationAction::Quit);
            return KeyHandling::ConsumePressAndRelease;
        case Qt::Key_F:
            startSearchUi();
            return KeyHandling::ConsumePressAndRelease;
        default: break;
        }
    }

    if (key == Qt::Key_Escape && modifiers == Qt::NoModifier
        && (searchUiActive_ || searchEngineActive_)) {
        endSearchUi();
        return KeyHandling::ConsumePressAndRelease;
    }

    if ((modifiers == Qt::ControlModifier && key == Qt::Key_Equal)
        || ((modifiers == Qt::ControlModifier
             || modifiers == (Qt::ControlModifier | Qt::ShiftModifier))
            && key == Qt::Key_Plus)) {
        zoomIn();
        return KeyHandling::ConsumePressAndRelease;
    }
    if (modifiers == Qt::ControlModifier && key == Qt::Key_Minus) {
        zoomOut();
        return KeyHandling::ConsumePressAndRelease;
    }
    if (modifiers == Qt::ControlModifier && key == Qt::Key_0) {
        resetZoom();
        return KeyHandling::ConsumePressAndRelease;
    }
    if (modifiers == Qt::ControlModifier && key == Qt::Key_PageUp) {
        Q_EMIT requestTabChange(-1);
        return KeyHandling::ConsumePressAndRelease;
    }
    if (modifiers == Qt::ControlModifier && key == Qt::Key_PageDown) {
        Q_EMIT requestTabChange(1);
        return KeyHandling::ConsumePressAndRelease;
    }
    if ((key == Qt::Key_Backtab || key == Qt::Key_Tab)
        && modifiers == (Qt::ControlModifier | Qt::ShiftModifier)) {
        Q_EMIT requestTabChange(-1);
        return KeyHandling::ConsumePressAndRelease;
    }
    if (key == Qt::Key_Tab && modifiers == Qt::ControlModifier) {
        Q_EMIT requestTabChange(1);
        return KeyHandling::ConsumePressAndRelease;
    }
    if (modifiers == (Qt::ControlModifier | Qt::AltModifier)
        && (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up
            || key == Qt::Key_Down)) {
        Q_EMIT requestNavigate(key);
        return KeyHandling::ConsumePressAndRelease;
    }
    if (modifiers == Qt::ShiftModifier
        && (key == Qt::Key_PageUp || key == Qt::Key_PageDown)) {
        TerminalViewportRequest request;
        request.kind = TerminalViewportRequest::Kind::Delta;
        request.delta = key == Qt::Key_PageUp
            ? -static_cast<qint64>(viewportPageRows())
            : static_cast<qint64>(viewportPageRows());
        controller_->scrollViewport(request);
        return KeyHandling::ConsumePressAndRelease;
    }
    return KeyHandling::PassThrough;
}

bool TerminalPane::resolveActiveSequence(
    TerminalSequenceResolution resolution,
    std::optional<TerminalKeyInput> current)
{
    return resolveSequenceToken(std::exchange(activeSequenceToken_, 0),
                                resolution, std::move(current));
}

bool TerminalPane::resolveSequenceToken(quint64 token,
                                        TerminalSequenceResolution resolution,
                                        std::optional<TerminalKeyInput> current)
{
    if (token == 0) return false;

    controller_->resolveSequence(token, resolution, current);
    return true;
}

bool TerminalPane::resolveExecutingSequence(
    TerminalSequenceResolution resolution,
    std::optional<TerminalKeyInput> current)
{
    if (executingSequenceTokens_.isEmpty()) {
        return resolveActiveSequence(resolution, std::move(current));
    }
    return resolveSequenceToken(
        std::exchange(executingSequenceTokens_.last(), 0), resolution,
        std::move(current));
}

TerminalPane::KeyHandling
TerminalPane::handleConfiguredShortcut(QKeyEvent *event,
                                       const QPointer<TerminalPane> &guard,
                                       quint64 pointerActivityEpoch)
{
    const GhosttyKeybindEvent bindingEvent{
        .qtKey = event->key(),
        .modifiers = event->modifiers(),
        .text = event->text(),
        .nativeScanCode = event->nativeScanCode(),
        .unshiftedCodepoint = unshiftedCodepoint(event->key()),
    };
    const TerminalKeyInput currentInput = terminalKeyInput(event);
    const bool sequenceWasActive = keybinds_.sequenceActive();
    const GhosttyKeybindStep step = keybinds_.advance(bindingEvent);

    if (step.kind == GhosttyKeybindStepKind::Leader) {
        const GhosttyKeybindProgram matchedProgram = keybinds_.program();
        if (activeSequenceToken_ == 0) {
            activeSequenceToken_ = controller_->beginSequence();
        }
        const quint64 token = activeSequenceToken_;
        const bool tokenStillOwned =
            controller_->stageSequenceKey(token, currentInput);
        if (guard == nullptr) return KeyHandling::ConsumePress;

        // A one-shot table is removed by advance(). Publish that transition
        // only after the leader is staged, so a synchronous continuation can
        // never resolve worker bytes that have not been delivered yet.
        if (step.activeTablesChanged) {
            Q_EMIT activeKeyTablesChanged();
            if (guard == nullptr) return KeyHandling::ConsumePress;
        }

        const bool sameProgram =
            guard->keybindProgram().isSameGeneration(matchedProgram);
        if (tokenStillOwned && sameProgram
            && guard->activeSequenceToken_ == token) {
            Q_EMIT pendingKeySequenceChanged();
            if (guard == nullptr) return KeyHandling::ConsumePress;
        } else if (!tokenStillOwned && sameProgram
                   && guard->activeSequenceToken_ == token) {
            guard->activeSequenceToken_ = 0;
            guard->keybinds_.resetSequence();
            Q_EMIT pendingKeySequenceChanged();
        }
        return KeyHandling::ConsumePress;
    }

    // A terminal step no longer owns the mutable traversal. Detach its token
    // before callbacks so a reentrant leader receives a fresh token and the
    // outer step can resolve only the staging that it actually matched.
    const bool completesSequence =
        step.kind == GhosttyKeybindStepKind::InvalidSequence
        || step.kind == GhosttyKeybindStepKind::IgnoredSequence
        || step.kind == GhosttyKeybindStepKind::Binding;
    if (completesSequence) beginKeyEventDeferral();
    const auto keyEventDeferralGuard = qScopeGuard([guard, completesSequence] {
        if (completesSequence && guard != nullptr) {
            guard->endKeyEventDeferral();
        }
    });
    const quint64 matchedSequenceToken =
        completesSequence ? std::exchange(activeSequenceToken_, 0) : 0;

    if (completesSequence && sequenceWasActive) {
        Q_EMIT pendingKeySequenceChanged();
        if (guard == nullptr) {
            return KeyHandling::ConsumePressAndRelease;
        }
    }

    if (step.activeTablesChanged) {
        Q_EMIT activeKeyTablesChanged();
        if (guard == nullptr) {
            return KeyHandling::ConsumePressAndRelease;
        }
    }

    switch (step.kind) {
    case GhosttyKeybindStepKind::Unmatched: return KeyHandling::PassThrough;
    case GhosttyKeybindStepKind::Leader:
        Q_UNREACHABLE_RETURN(KeyHandling::PassThrough);
    case GhosttyKeybindStepKind::InvalidSequence:
        if (matchedSequenceToken != 0) {
            hideMouseForTerminalKey(currentInput, pointerActivityEpoch);
            if (guard == nullptr) return KeyHandling::ConsumePress;
        }
        if (resolveSequenceToken(
                matchedSequenceToken,
                TerminalSequenceResolution::FlushAndSendCurrent,
                currentInput)) {
            return KeyHandling::ConsumePress;
        }
        return KeyHandling::PassThrough;
    case GhosttyKeybindStepKind::IgnoredSequence:
        (void)resolveSequenceToken(matchedSequenceToken,
                                   TerminalSequenceResolution::Drop);
        return KeyHandling::ConsumePress;
    case GhosttyKeybindStepKind::Binding: break;
    }

    // `global:` implies `all:` at runtime even though Ghostty retains the raw
    // flags independently. Broad bindings ignore unconsumed/performable and
    // are considered performed even when no target changes state.
    if (step.match.all || step.match.global) {
        const GhosttyActionInputEffect effect =
            step.match.actionChain.inputEffect;
        // Closing takes priority over ignore so the corresponding release can
        // never reach a surface that the complete broad chain may destroy.
        const KeyHandling handling = effect == GhosttyActionInputEffect::Ignore
            ? KeyHandling::ConsumePress
            : KeyHandling::ConsumePressAndRelease;
        (void)resolveSequenceToken(matchedSequenceToken,
                                   TerminalSequenceResolution::Drop);
        if (guard == nullptr) return handling;
        // Emit last: a close action may synchronously destroy the originating
        // workspace, and therefore this pane, through an approval observer.
        // All pane state needed for the event has already been finalized.
        Q_EMIT broadActionsRequested(step.match.actionChain);
        return handling;
    }

    auto chain =
        std::make_shared<PendingLocalActionChain>(PendingLocalActionChain{
            .chain = step.match.actionChain,
            .sequenceToken = matchedSequenceToken,
            .currentInput = currentInput,
            .keyIdentity = keyEventIdentity(event),
            .keyFocusEpoch = keyFocusEpoch_,
            .pointerActivityEpoch = pointerActivityEpoch,
            .consumed = step.match.consumed,
            .performable = step.match.performable,
            .ownsKeyDeferral = false,
            .startingAction = false,
            .earlyResult = std::nullopt,
        });
    const std::optional<KeyHandling> handling = continueLocalActionChain(chain);
    // A pending worker action retains an additional key-event deferral
    // beyond this stack frame. Accept only the press for now; final release
    // handling is decided from the completed aggregate performed state.
    return handling.value_or(KeyHandling::ConsumePress);
}

std::optional<QByteArray> TerminalPane::hoveredUrlForCopy() const
{
    if (!hyperlinkModifiersMatch(hoverModifiers_)) return std::nullopt;

    QMutexLocker locker(&renderMutex_);
    const int index = hoverCell_.y() * frame_.columns + hoverCell_.x();
    const bool hoveredCellIsLinked = hoverCell_.x() >= 0
        && hoverCell_.x() < frame_.columns && hoverCell_.y() >= 0
        && hoverCell_.y() < frame_.rows
        && hoveredHyperlinkColumns_ == frame_.columns
        && hoveredHyperlinkRows_ == frame_.rows
        && index < hoveredHyperlinkCellMask_.size()
        && hoveredHyperlinkCellMask_.testBit(index);
    if (hoveredHyperlinkUri_.isEmpty() || hoveredHyperlinkCell_ != hoverCell_
        || !hoveredCellIsLinked) {
        return std::nullopt;
    }
    return hoveredHyperlinkUri_;
}

int TerminalPane::viewportPageRows() const
{
    QMutexLocker locker(&renderMutex_);
    return std::max(1, terminalRows_);
}

std::optional<TerminalPane::KeyHandling> TerminalPane::continueLocalActionChain(
    const std::shared_ptr<PendingLocalActionChain> &chain)
{
    const QPointer<TerminalPane> guard(this);
    executingSequenceTokens_.append(chain->sequenceToken);
    bool sequenceAttached = true;
    const auto sequenceGuard = qScopeGuard([guard, chain, &sequenceAttached] {
        if (!sequenceAttached) return;
        if (guard == nullptr || guard->executingSequenceTokens_.isEmpty()) {
            return;
        }
        chain->sequenceToken = guard->executingSequenceTokens_.takeLast();
    });

    while (chain->nextEntry < chain->chain.entries.size()) {
        const GhosttyCompiledAction &entry =
            chain->chain.entries.at(chain->nextEntry++);
        if (!entry.action.has_value()) continue;

        chain->startingAction = true;
        TerminalActionExecutionStart start = startConfiguredAction(
            *entry.action,
            [guard, chain](TerminalActionExecutionResult result) {
                if (chain->startingAction) {
                    chain->earlyResult = std::move(result);
                    return;
                }
                if (guard == nullptr) return;
                const bool performed =
                    guard->commitConfiguredActionResult(result);
                if (guard == nullptr) return;
                chain->performed = performed || chain->performed;
                (void)guard->continueLocalActionChain(chain);
            });
        chain->startingAction = false;
        if (guard == nullptr) {
            // Known lifecycle effects are owner-deferred. An unexpected
            // destructive embedding callback cancels this local continuation.
            return KeyHandling::ConsumePressAndRelease;
        }
        if (chain->earlyResult.has_value()) {
            TerminalActionExecutionResult result =
                std::move(*chain->earlyResult);
            chain->earlyResult.reset();
            chain->performed =
                commitConfiguredActionResult(result) || chain->performed;
            if (guard == nullptr) {
                return KeyHandling::ConsumePressAndRelease;
            }
            continue;
        }
        if (start.pending) {
            if (!chain->ownsKeyDeferral) {
                beginKeyEventDeferral();
                chain->ownsKeyDeferral = true;
            }
            return std::nullopt;
        }
        chain->performed =
            commitConfiguredActionResult(start.result) || chain->performed;
        if (guard == nullptr) {
            return KeyHandling::ConsumePressAndRelease;
        }
    }

    chain->sequenceToken = executingSequenceTokens_.takeLast();
    sequenceAttached = false;
    const KeyHandling handling =
        finishLocalActionChain(*chain, chain->ownsKeyDeferral);
    if (guard == nullptr) {
        return KeyHandling::ConsumePressAndRelease;
    }
    if (!chain->ownsKeyDeferral) {
        return handling;
    }

    const bool matchingReleaseDeferred = std::ranges::any_of(
        deferredInputs_, [chain](const DeferredPaneInput &input) {
            const auto *event = std::get_if<DeferredKeyInput>(&input);
            return event != nullptr && event->focusEpoch == chain->keyFocusEpoch
                && !event->event.pressed
                && keyEventIdentity(event->event) == chain->keyIdentity;
        });
    if (handling == KeyHandling::ConsumePressAndRelease
        && (chain->keyFocusEpoch == keyFocusEpoch_
            || matchingReleaseDeferred)) {
        consumedKeys_.insert(chain->keyIdentity);
    }
    chain->ownsKeyDeferral = false;
    endKeyEventDeferral();
    return std::nullopt;
}

TerminalPane::KeyHandling
TerminalPane::finishLocalActionChain(PendingLocalActionChain &chain,
                                     bool delayed)
{
    const auto resolve = [this, &chain](TerminalSequenceResolution resolution,
                                        bool withCurrent = false) {
        if (withCurrent && chain.sequenceToken != 0) {
            hideMouseForTerminalKey(chain.currentInput,
                                    chain.pointerActivityEpoch);
        }
        return resolveSequenceToken(
            std::exchange(chain.sequenceToken, 0), resolution,
            withCurrent ? std::optional<TerminalKeyInput>(chain.currentInput)
                        : std::nullopt);
    };

    // Ghostty executes the complete chain, then applies this precedence to
    // the aggregate performed state.
    if (chain.performed
        && chain.chain.inputEffect == GhosttyActionInputEffect::ClosingAction) {
        (void)resolve(TerminalSequenceResolution::Drop);
        return KeyHandling::ConsumePressAndRelease;
    }
    if (chain.performed
        && chain.chain.inputEffect == GhosttyActionInputEffect::Ignore) {
        (void)resolve(TerminalSequenceResolution::Drop);
        return KeyHandling::ConsumePress;
    }
    if (chain.performable && !chain.performed) {
        if (resolve(TerminalSequenceResolution::FlushAndSendCurrent, true)) {
            return KeyHandling::ConsumePress;
        }
        if (delayed) {
            hideMouseForTerminalKey(chain.currentInput,
                                    chain.pointerActivityEpoch);
            controller_->sendKey(chain.currentInput);
        }
        return KeyHandling::PassThrough;
    }
    if (chain.consumed) {
        (void)resolve(TerminalSequenceResolution::Drop);
        return KeyHandling::ConsumePressAndRelease;
    }
    if (resolve(TerminalSequenceResolution::FlushAndSendCurrent, true)) {
        return KeyHandling::ConsumePress;
    }
    if (delayed) {
        hideMouseForTerminalKey(chain.currentInput, chain.pointerActivityEpoch);
        controller_->sendKey(chain.currentInput);
    }
    return KeyHandling::PassThrough;
}

bool TerminalPane::executeConfiguredAction(QStringView action)
{
    const std::optional<GhosttyConfiguredAction> parsed =
        GhosttyActionCatalog::parseConfiguredAction(action);
    return parsed.has_value() && executeConfiguredAction(*parsed);
}

bool TerminalPane::executeConfiguredAction(
    const GhosttyConfiguredAction &action)
{
    const QPointer<TerminalPane> guard(this);
    TerminalActionExecutionStart start = startConfiguredAction(
        action, [guard](TerminalActionExecutionResult result) {
            if (guard != nullptr) {
                (void)guard->commitConfiguredActionResult(result);
            }
        });
    if (guard == nullptr) {
        return start.result.performed;
    }
    return start.pending ? start.result.performed
                         : commitConfiguredActionResult(start.result);
}

TerminalActionExecutionStart
TerminalPane::startConfiguredAction(const GhosttyConfiguredAction &action,
                                    ConfiguredActionCompletion completion)
{
    const auto startTerminalAction =
        [this, &completion](auto &&request) -> TerminalActionExecutionStart {
        if (!terminalActionsAccepted_) {
            return {};
        }
        const quint64 requestId = nextTerminalActionRequestId();
        pendingTerminalActionCompletions_.insert(
            requestId,
            {
                .completion = std::move(completion),
                .epoch = terminalActionEpoch_,
            });
        const QPointer<TerminalPane> guard(this);
        const bool accepted =
            std::invoke(std::forward<decltype(request)>(request), requestId);
        if (guard == nullptr) {
            return {};
        }
        if (!accepted) {
            pendingTerminalActionCompletions_.remove(requestId);
            return {};
        }
        return {
            .pending = true,
            // Preserve the immediate API contract for direct programmatic
            // dispatch. Correlated local and broad chains consume the
            // worker's authoritative performed value.
            .result =
                {
                    .performed = true,
                    .terminalAction = std::nullopt,
                },
        };
    };

    const auto *paneAction = std::get_if<GhosttyPaneAction>(&action);
    if (paneAction != nullptr) {
        namespace PaneAction = GhosttyPaneActions;
        if (std::holds_alternative<PaneAction::SelectAll>(*paneAction)) {
            return startTerminalAction([this](quint64 requestId) {
                return controller_->selectAllAction(requestId);
            });
        }
        if (std::holds_alternative<PaneAction::CopyToClipboard>(*paneAction)) {
            return startTerminalAction([this](quint64 requestId) {
                return controller_->copySelectionAction(requestId);
            });
        }
        if (const auto *adjustment =
                std::get_if<PaneAction::AdjustSelection>(paneAction);
            adjustment != nullptr) {
            return startTerminalAction(
                [this, value = adjustment->adjustment](quint64 requestId) {
                    return controller_->adjustSelectionAction(requestId, value);
                });
        }
        if (std::holds_alternative<PaneAction::ScrollToSelection>(
                *paneAction)) {
            return startTerminalAction([this](quint64 requestId) {
                return controller_->scrollToSelectionAction(requestId);
            });
        }
        if (std::holds_alternative<PaneAction::SearchSelection>(*paneAction)) {
            return startTerminalAction([this](quint64 requestId) {
                return controller_->searchSelectionAction(requestId);
            });
        }
        if (const auto *fileAction =
                std::get_if<TerminalWriteFileAction>(paneAction);
            fileAction != nullptr) {
            return startTerminalAction(
                [this, value = *fileAction](quint64 requestId) {
                    return controller_->writeTerminalFile(requestId, value);
                });
        }
    }

    return {
        .pending = false,
        .result =
            {
                .performed = performConfiguredAction(action),
                .terminalAction = std::nullopt,
            },
    };
}

bool TerminalPane::commitConfiguredActionResult(
    const TerminalActionExecutionResult &result)
{
    if (!result.terminalAction.has_value()) {
        return result.performed;
    }
    if (result.terminalActionEpoch != terminalActionEpoch_) {
        return false;
    }
    const TerminalActionResult &terminal = *result.terminalAction;
    if (!terminal.performed) {
        return false;
    }

    switch (terminal.effect) {
    case TerminalActionEffect::None: return true;
    case TerminalActionEffect::Clipboard: {
        QClipboard *const clipboard = QGuiApplication::clipboard();
        if (clipboard != nullptr) {
            const TerminalClipboardWriteTargets targets =
                writeTerminalClipboard(clipboard, terminal.payload,
                                       terminal.clipboardDestination);
            if (targets.standard) {
                Q_EMIT standardClipboardCommitted(terminal.payload.isEmpty());
            }
        }
        return true;
    }
    case TerminalActionEffect::OpenFile: {
        if (!terminal.payload.isEmpty()) {
            const std::function<bool(const QUrl &)> opener = urlOpener_;
            if (opener) {
                static_cast<void>(
                    opener(QUrl::fromLocalFile(terminal.payload)));
            }
        }
        return true;
    }
    case TerminalActionEffect::StartSearch:
        startSearchUiWithSelection(terminal.payload);
        return true;
    }
    return false;
}

quint64 TerminalPane::nextTerminalActionRequestId()
{
    do {
        ++nextTerminalActionRequestId_;
    } while (nextTerminalActionRequestId_ == 0
             || pendingTerminalActionCompletions_.contains(
                 nextTerminalActionRequestId_));
    return nextTerminalActionRequestId_;
}

void TerminalPane::advanceTerminalActionEpoch()
{
    do {
        ++terminalActionEpoch_;
    } while (terminalActionEpoch_ == 0);
}

void TerminalPane::failStaleTerminalActionCompletions()
{
    QList<quint64> requestIds;
    requestIds.reserve(pendingTerminalActionCompletions_.size());
    for (auto iterator = pendingTerminalActionCompletions_.cbegin();
         iterator != pendingTerminalActionCompletions_.cend(); ++iterator) {
        if (iterator->epoch != terminalActionEpoch_) {
            requestIds.append(iterator.key());
        }
    }
    std::ranges::sort(requestIds);

    const QPointer<TerminalPane> guard(this);
    for (quint64 requestId : requestIds) {
        auto iterator = pendingTerminalActionCompletions_.find(requestId);
        if (iterator == pendingTerminalActionCompletions_.end()) {
            continue;
        }
        PendingTerminalActionCompletion pending = std::move(iterator.value());
        pendingTerminalActionCompletions_.erase(iterator);
        if (pending.completion) {
            TerminalActionResult failed;
            failed.requestId = requestId;
            pending.completion({
                .performed = false,
                .terminalAction = std::move(failed),
                .terminalActionEpoch = pending.epoch,
            });
        }
        if (guard == nullptr) return;
    }
}

void TerminalPane::handleTerminalActionResult(
    const TerminalActionResult &result)
{
    auto iterator = pendingTerminalActionCompletions_.find(result.requestId);
    if (iterator == pendingTerminalActionCompletions_.end()) {
        return;
    }
    PendingTerminalActionCompletion pending = std::move(iterator.value());
    pendingTerminalActionCompletions_.erase(iterator);
    if (pending.completion) {
        const bool current = pending.epoch == terminalActionEpoch_;
        TerminalActionResult resolved = result;
        if (!current) {
            resolved = {};
            resolved.requestId = result.requestId;
        }
        pending.completion({
            .performed = current && result.performed,
            .terminalAction = std::move(resolved),
            .terminalActionEpoch = pending.epoch,
        });
    }
}

bool TerminalPane::performConfiguredAction(
    const GhosttyConfiguredAction &action)
{
    return std::visit(
        Overloaded{
            [this](ApplicationAction applicationAction) {
                Q_EMIT applicationActionRequested(applicationAction);
                return true;
            },
            [this](WindowNavigationAction navigationAction) {
                Q_EMIT windowNavigationRequested(navigationAction);
                return true;
            },
            [this](const GhosttyPaneAction &paneAction) {
                return performPaneAction(paneAction);
            },
            [this](const WorkspaceActionRequest &request) {
                return performWorkspaceAction(request);
            },
            [this](const WorkspaceFrontendActionRequest &request) {
                Q_EMIT frontendActionRequested(request);
                return true;
            },
        },
        action);
}

bool TerminalPane::performPaneAction(const GhosttyPaneAction &action)
{
    namespace PaneAction = GhosttyPaneActions;

    const auto scroll = [this](TerminalViewportRequest::Kind kind,
                               qint64 delta = 0, quint64 row = 0) {
        controller_->scrollViewport({.kind = kind, .delta = delta, .row = row});
        return true;
    };
    const auto keyTableChanged = [this](bool changed) {
        if (changed) Q_EMIT activeKeyTablesChanged();
        return changed;
    };

    return std::visit(
        Overloaded{
            [&scroll](const PaneAction::ScrollToTop &) {
                return scroll(TerminalViewportRequest::Kind::Top);
            },
            [&scroll](const PaneAction::ScrollToBottom &) {
                return scroll(TerminalViewportRequest::Kind::Bottom);
            },
            [this](const PaneAction::ScrollToSelection &value) {
                return executeConfiguredAction(
                    GhosttyConfiguredAction{GhosttyPaneAction{value}});
            },
            [&scroll](const PaneAction::ScrollToRow &value) {
                return scroll(TerminalViewportRequest::Kind::Row, 0, value.row);
            },
            [this, &scroll](const PaneAction::ScrollPageUp &) {
                return scroll(TerminalViewportRequest::Kind::Delta,
                              -static_cast<qint64>(viewportPageRows()));
            },
            [this, &scroll](const PaneAction::ScrollPageDown &) {
                return scroll(TerminalViewportRequest::Kind::Delta,
                              viewportPageRows());
            },
            [this, &scroll](const PaneAction::ScrollPageFractional &value) {
                const std::optional<qint64> delta =
                    fractionalPageDelta(value.fraction, viewportPageRows());
                return delta.has_value()
                    && scroll(TerminalViewportRequest::Kind::Delta, *delta);
            },
            [&scroll](const PaneAction::ScrollPageLines &value) {
                return scroll(TerminalViewportRequest::Kind::Delta,
                              value.lines);
            },
            [this](const PaneAction::IncreaseFontSize &value) {
                const float current = static_cast<float>(fontPointSize());
                const float delta = clampFontActionValue(value.points, 0.0F);
                manuallyZoomed_ = true;
                setFontPointSize(
                    std::fmin(current + delta, kMaximumActionFontSize));
                return true;
            },
            [this](const PaneAction::DecreaseFontSize &value) {
                const float current = static_cast<float>(fontPointSize());
                const float delta = clampFontActionValue(value.points, 0.0F);
                manuallyZoomed_ = true;
                setFontPointSize(
                    std::fmax(kMinimumActionFontSize, current - delta));
                return true;
            },
            [this](const PaneAction::SetFontSize &value) {
                manuallyZoomed_ = true;
                setFontPointSize(
                    clampFontActionValue(value.points, kMinimumActionFontSize));
                return true;
            },
            [this](const PaneAction::ResetFontSize &) {
                manuallyZoomed_ = false;
                setFontPointSize(defaultFontPointSize_);
                return true;
            },
            [this,
             &keyTableChanged](const PaneAction::ActivateKeyTable &value) {
                return keyTableChanged(keybinds_.activateTable(value.name));
            },
            [this,
             &keyTableChanged](const PaneAction::ActivateKeyTableOnce &value) {
                return keyTableChanged(
                    keybinds_.activateTable(value.name, true));
            },
            [this, &keyTableChanged](const PaneAction::DeactivateKeyTable &) {
                return keyTableChanged(keybinds_.deactivateTable());
            },
            [this,
             &keyTableChanged](const PaneAction::DeactivateAllKeyTables &) {
                return keyTableChanged(keybinds_.deactivateAllTables());
            },
            [this](const PaneAction::SelectAll &) {
                controller_->selectAll();
                return true;
            },
            [this](const PaneAction::AdjustSelection &value) {
                return executeConfiguredAction(
                    GhosttyConfiguredAction{GhosttyPaneAction{value}});
            },
            [this](const PaneAction::StartSearch &) {
                startSearchUi();
                return true;
            },
            [this](const PaneAction::EndSearch &) {
                const bool performed =
                    searchEngineActive_ || controller_->searchExpected();
                // Always clean up stale UI even when the backend had no
                // active search and the action reports not performed.
                endSearchUi();
                return performed;
            },
            [this](const PaneAction::SearchSelection &value) {
                return executeConfiguredAction(
                    GhosttyConfiguredAction{GhosttyPaneAction{value}});
            },
            [this](const PaneAction::Search &value) {
                const bool hadSearch =
                    searchEngineActive_ || controller_->searchExpected();
                controller_->searchSerialized(value.serializedNeedle);
                return !value.serializedNeedle.isEmpty() || hadSearch;
            },
            [this](const PaneAction::NavigateSearch &value) {
                if (!controller_->searchExpected()) return false;
                controller_->navigateSearch(value.direction);
                return true;
            },
            [this](const PaneAction::SendCsi &value) {
                controller_->sendCsi(value.serializedBytes);
                return true;
            },
            [this](const PaneAction::SendEscape &value) {
                controller_->sendEscape(value.serializedBytes);
                return true;
            },
            [this](const PaneAction::SendText &value) {
                // Ghostty validates Zig escapes only while performing this
                // action, so malformed text remains consumed without PTY IO.
                controller_->sendRawText(value.serializedBytes);
                return true;
            },
            [this](const PaneAction::ResetTerminal &) {
                controller_->resetTerminal();
                return true;
            },
            [this](const PaneAction::ToggleReadOnly &) {
                controller_->setReadOnly(!controller_->readOnly());
                return true;
            },
            [this](const PaneAction::ToggleMouseReporting &) {
                controller_->setMouseReportingEnabled(
                    !controller_->mouseReportingEnabled());
                return true;
            },
            [this](const PaneAction::CopyToClipboard &value) {
                return executeConfiguredAction(
                    GhosttyConfiguredAction{GhosttyPaneAction{value}});
            },
            [this](const TerminalWriteFileAction &value) {
                return executeConfiguredAction(
                    GhosttyConfiguredAction{GhosttyPaneAction{value}});
            },
            [this](const PaneAction::Paste &value) {
                const TerminalClipboardSource source =
                    value.source == PaneAction::PasteSource::Clipboard
                    ? TerminalClipboardSource::Standard
                    : TerminalClipboardSource::Primary;
                const std::optional<QString> text =
                    readTerminalClipboard(QGuiApplication::clipboard(), source);
                if (!text.has_value()) return false;
                if (!text->isEmpty()) pasteText(*text);
                return true;
            },
            [this](const PaneAction::CopyUrlToClipboard &) {
                QClipboard *const clipboard = QGuiApplication::clipboard();
                const std::optional<QByteArray> uri = hoveredUrlForCopy();
                if (clipboard == nullptr || !uri.has_value()) return false;
                auto *mimeData = new QMimeData;
                mimeData->setData(QStringLiteral("text/plain"), *uri);
                clipboard->setMimeData(mimeData);
                Q_EMIT standardClipboardCommitted(uri->isEmpty());
                return true;
            },
            [this](const PaneAction::CopyTitleToClipboard &) {
                const std::optional<QString> title = effectiveSurfaceTitle();
                QClipboard *const clipboard = QGuiApplication::clipboard();
                if (!title || title->isEmpty() || clipboard == nullptr) {
                    return false;
                }
                const TerminalClipboardWriteTargets targets =
                    writeTerminalClipboard(
                        clipboard, *title,
                        TerminalClipboardDestination::Standard);
                if (targets.standard) {
                    Q_EMIT standardClipboardCommitted(title->isEmpty());
                }
                return true;
            },
            [this](const PaneAction::EndKeySequence &) {
                if (executingSequenceTokens_.isEmpty()) {
                    const bool sequenceWasActive = keybinds_.sequenceActive();
                    keybinds_.resetSequence();
                    if (sequenceWasActive) {
                        const QPointer<TerminalPane> guard(this);
                        Q_EMIT pendingKeySequenceChanged();
                        if (guard == nullptr) return true;
                    }
                }
                (void)resolveExecutingSequence(
                    TerminalSequenceResolution::Flush);
                return true;
            },
            [this](const PaneAction::CloseWindow &) {
                Q_EMIT requestCloseWindow();
                return true;
            },
        },
        action);
}

bool TerminalPane::performWorkspaceAction(WorkspaceActionRequest request)
{
    if (request.action == WorkspaceAction::SplitAuto) {
        const qreal devicePixelRatio =
            window() != nullptr ? window()->devicePixelRatio() : 1.0;
        const int surfaceWidth =
            std::max(1, qRound(width() * devicePixelRatio));
        const int surfaceHeight =
            std::max(1, qRound(height() * devicePixelRatio));
        request.action = surfaceWidth > surfaceHeight
            ? WorkspaceAction::SplitRight
            : WorkspaceAction::SplitDown;
    }
    const auto workspaceActionHandler = workspaceActionHandler_;
    if (workspaceActionHandler != nullptr) {
        return (*workspaceActionHandler)(request);
    }
    switch (request.action) {
    case WorkspaceAction::NewTab: Q_EMIT requestNewTab(); return true;
    case WorkspaceAction::CloseTab:
        Q_EMIT requestCloseTab(request.context.closeTabMode);
        return true;
    case WorkspaceAction::ClosePane: Q_EMIT requestClose(); return true;
    case WorkspaceAction::SplitLeft:
    case WorkspaceAction::SplitRight:
    case WorkspaceAction::SplitUp:
    case WorkspaceAction::SplitDown:
    case WorkspaceAction::SplitAuto:
        Q_EMIT requestSplit(request.action);
        return true;
    case WorkspaceAction::NavigatePane:
        Q_EMIT requestNavigate(static_cast<int>(request.context.value));
        return true;
    case WorkspaceAction::ChangeTabRelative:
        Q_EMIT requestTabChange(static_cast<int>(request.context.value));
        return true;
    case WorkspaceAction::SetSurfaceTitle:
        setSurfaceTitle(std::move(request.payload));
        return true;
    case WorkspaceAction::ActivateTab:
    case WorkspaceAction::ActivatePane:
    case WorkspaceAction::NavigatePaneRelative:
    case WorkspaceAction::ActivateTabByIndex:
    case WorkspaceAction::ActivateLastTab:
    case WorkspaceAction::MoveTab:
    case WorkspaceAction::PromptSurfaceTitle:
    case WorkspaceAction::PromptTabTitle:
    case WorkspaceAction::SetTabTitle:
    case WorkspaceAction::ResizeSplit:
    case WorkspaceAction::EqualizeSplits:
    case WorkspaceAction::ToggleSplitZoom:
    case WorkspaceAction::ToggleFullscreen:
    case WorkspaceAction::ToggleMaximize:
    case WorkspaceAction::ToggleWindowDecorations: return false;
    }
    return false;
}

void TerminalPane::inputMethodEvent(QInputMethodEvent *event)
{
    const quint64 pointerActivityEpoch = pointerActivityEpoch_;
    const QString nextPreedit = event->preeditString();
    bool hadPreedit = false;
    {
        QMutexLocker locker(&renderMutex_);
        hadPreedit = !preedit_.isEmpty();
        preedit_ = nextPreedit.isEmpty() ? QString{} : nextPreedit;
    }
    const TerminalInputMethodInput input{
        .commitText = event->commitString(),
        .preeditTransition = hadPreedit || !nextPreedit.isNull(),
    };
    if (!input.commitText.isEmpty() || input.preeditTransition) {
        const QPointer<TerminalPane> bellGuard(this);
        setBellRinging(false);
        if (bellGuard == nullptr) {
            event->accept();
            return;
        }
        if (!input.commitText.isEmpty() && options_.mouseHideWhileTyping
            && !controller_->keyboardInputSuppressed()
            && pointerActivityEpoch == pointerActivityEpoch_) {
            setMouseHiddenWhileTyping(true);
            if (bellGuard == nullptr) {
                event->accept();
                return;
            }
        }
        if (keyEventDeferralDepth_ != 0 || drainingDeferredKeyEvents_) {
            deferredInputs_.emplace_back(input);
        } else {
            const QPointer<TerminalPane> guard(this);
            controller_->sendInputMethod(input);
            if (guard == nullptr) return;
        }
    }
    requestRenderUpdate();
    event->accept();
}

QVariant TerminalPane::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImEnabled) {
        return true;
    }
    if (query == Qt::ImHints) {
        return QVariant::fromValue(Qt::ImhNoPredictiveText);
    }
    if (query == Qt::ImCursorRectangle) {
        const std::optional<TerminalViewportLayout> layout =
            currentViewportLayout();
        QMutexLocker locker(&renderMutex_);
        if (layout) {
            const qreal cellWidth =
                layout->gridRect.width() / layout->session.columns;
            const qreal cellHeight =
                layout->gridRect.height() / layout->session.rows;
            return QRectF(
                layout->gridRect.left()
                    + static_cast<qreal>(frame_.cursorColumn) * cellWidth,
                layout->gridRect.top()
                    + static_cast<qreal>(frame_.cursorRow) * cellHeight,
                cellWidth
                    * static_cast<qreal>(std::max(1, frame_.cursorColumnSpan)),
                cellHeight);
        }
        return QRectF(
            static_cast<qreal>(frame_.cursorColumn) * metrics_.cellWidth,
            static_cast<qreal>(frame_.cursorRow) * metrics_.cellHeight,
            metrics_.cellWidth
                * static_cast<qreal>(std::max(1, frame_.cursorColumnSpan)),
            metrics_.cellHeight);
    }
    return QQuickItem::inputMethodQuery(query);
}

void TerminalPane::mousePressEvent(QMouseEvent *event)
{
    revealMouseAfterActivity();
    const QPointer<TerminalPane> bellGuard(this);
    setBellRinging(false);
    if (bellGuard == nullptr) {
        event->accept();
        return;
    }
    forceActiveFocus(Qt::MouseFocusReason);
    Q_EMIT activated(this);
    updateHyperlinkHover(event->position(), event->modifiers());
    if (linkPreviewPointerCaptured_) {
        cancelPendingHyperlinkActivation();
        cancelHyperlinkPress();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        cancelPendingHyperlinkActivation();
        cancelHyperlinkPress();
        hyperlinkPressDragged_ = false;
        hyperlinkPressPosition_ = event->position();
        quint64 contentRevision = 0;
        hyperlinkPressArmed_ = hyperlinkModifiersMatch(hoverModifiers_)
            && hyperlinkCellCandidate(hoverCell_, &contentRevision);
        if (hyperlinkPressArmed_) {
            hyperlinkPressCell_ = hoverCell_;
            if (hoveredHyperlinkCell_ == hoverCell_) {
                hyperlinkPressKind_ = hoveredLinkKind_;
                hyperlinkPressUri_ = hoveredHyperlinkUri_;
            } else {
                hyperlinkPressKind_ = TerminalLinkKind::Osc8;
                hyperlinkPressUri_.clear();
            }
            hyperlinkPressRequestId_ = controller_->prepareHyperlinkActivation(
                hyperlinkPressCell_.x(), hyperlinkPressCell_.y(),
                contentRevision);
        }
    }
    const Qt::KeyboardModifiers modifiers = hoverModifiers_;
    const bool terminalMouseCaptured = controller_->terminalMouseTracking();
    const bool shiftBypassesCapture = shiftBypassesMouseCapture(modifiers);
    const bool report = controller_->mouseTracking() && !shiftBypassesCapture;
    if (report) {
        // Ghostty resets a local selection gesture whenever a reported
        // button event takes over. In particular, disabling reporting before
        // release must not turn a remotely handled press into a selection.
        selecting_ = false;
        sendMouse(event->position(), TerminalMouseInput::Press, event->button(),
                  event->buttons(), modifiers);
    } else if (event->button() == Qt::LeftButton) {
        // Ghostty starts its normal selection gesture even for a potential
        // link click. A release may activate the link, while a drag naturally
        // continues as selection instead.
        beginLocalSelection(*event, modifiers);
    } else if (event->button() == Qt::MiddleButton) {
        QClipboard *const clipboard = QGuiApplication::clipboard();
        if (options_.middleClickAction == MiddleClickAction::PrimaryPaste
            && clipboard != nullptr) {
            const QString text = readMiddleClickClipboard(
                clipboard, options_.selectionClipboard.copyOnSelect);
            if (!text.isEmpty()) {
                pasteText(text);
            }
        }
    } else if (event->button() == Qt::RightButton) {
        const QPoint cell = cellAt(event->position());
        quint64 contentRevision = 0;
        {
            QMutexLocker locker(&renderMutex_);
            if (hasFrame_) {
                contentRevision = frame_.contentRevision;
            }
        }
        QPointF windowPosition = event->position();
        if (QQuickWindow *const quickWindow = window();
            quickWindow != nullptr && quickWindow->contentItem() != nullptr) {
            windowPosition =
                mapToItem(quickWindow->contentItem(), event->position());
        }

        const QPointer<TerminalPane> guard(this);
        const quint64 requestId = controller_->requestRightClick(
            contentRevision, cell.x(), cell.y(), static_cast<int>(modifiers),
            terminalMouseCaptured && shiftBypassesCapture);
        if (guard == nullptr) {
            event->accept();
            return;
        }
        pendingRightClickWindowPositions_.insert(requestId, windowPosition);
        newestRightClickRequestId_ = requestId;
    }
    event->accept();
}

void TerminalPane::mouseDoubleClickEvent(QMouseEvent *event)
{
    // Qt delivers MouseButtonPress for the physical second click before this
    // additional notification. The ordinary press path has already handled
    // focus, hover, local selection or DEC reporting; forwarding this event
    // would turn every double click into a libghostty triple click (and emit a
    // duplicate application mouse report).
    event->accept();
}

void TerminalPane::beginLocalSelection(const QMouseEvent &event,
                                       Qt::KeyboardModifiers modifiers)
{
    const QPointF position = event.position();
    const QPoint cell = cellAt(position);
    const qreal devicePixelRatio =
        TerminalPaneRenderer::normalizedDevicePixelRatio(
            window() != nullptr ? window()->devicePixelRatio() : 1.0);
    constexpr quint64 nanosecondsPerMillisecond = 1'000'000;
    const quint64 timestampMilliseconds = event.timestamp();
    const bool timestampValid = timestampMilliseconds != 0
        && timestampMilliseconds
            <= std::numeric_limits<quint64>::max() / nanosecondsPerMillisecond;
    const TerminalSelectionPressInput input{
        .column = cell.x(),
        .row = cell.y(),
        .surfaceX = position.x() * devicePixelRatio,
        .surfaceY = position.y() * devicePixelRatio,
        .timestampNanoseconds = timestampValid
            ? timestampMilliseconds * nanosecondsPerMillisecond
            : 0,
        .timestampValid = timestampValid,
        .controlModifier = modifiers.testFlag(Qt::ControlModifier),
        .extendExistingSelection = modifiers.testFlag(Qt::ShiftModifier)
            && shiftBypassesMouseCapture(modifiers),
        .rectangular = isRectangleSelectionState(modifiers),
    };
    selecting_ = true;
    controller_->beginSelection(input);
}

void TerminalPane::hideMouseForTerminalKey(const TerminalKeyInput &input,
                                           quint64 pointerActivityEpoch)
{
    if (!controller_->keyboardInputSuppressed() && options_.mouseHideWhileTyping
        && pointerActivityEpoch == pointerActivityEpoch_ && input.pressed
        && !input.autoRepeat && !input.text.isEmpty()) {
        setMouseHiddenWhileTyping(true);
    }
}

void TerminalPane::revealMouseAfterActivity()
{
    ++pointerActivityEpoch_;
    setMouseHiddenWhileTyping(false);
}

void TerminalPane::setMouseHiddenWhileTyping(bool hidden)
{
    if (mouseHiddenWhileTyping_ == hidden) return;
    mouseHiddenWhileTyping_ = hidden;
    syncPointerCursor();
}

void TerminalPane::syncPointerCursor()
{
    if (mouseHiddenWhileTyping_) {
        setCursor(Qt::BlankCursor);
    } else if (hyperlinkLeaseActive_ && !hoveredHyperlinkUri_.isEmpty()) {
        setCursor(Qt::PointingHandCursor);
    } else if (isRectangleSelectionState(hoverModifiers_)
               && (!controller_->terminalMouseTracking()
                   || hoverModifiers_.testFlag(Qt::ShiftModifier))) {
        // Pinned Ghostty uses raw DEC state for pointer shape. Under capture,
        // Shift is required to show the rectangle-select crosshair even when
        // the configured policy would retain Shift in application input.
        setCursor(Qt::CrossCursor);
    } else if (controller_->terminalMouseTracking()
               && !hoverModifiers_.testFlag(Qt::ShiftModifier)) {
        // DEC mouse tracking uses the ordinary pointer. Shift temporarily
        // restores Ghostty's text pointer while the user takes local control.
        setCursor(Qt::ArrowCursor);
    } else {
        setCursor(Qt::IBeamCursor);
    }
}

bool TerminalPane::revealMouseForPointerPosition(const QPointF &position)
{
    if (!lastPointerActivityPosition_.has_value()) {
        lastPointerActivityPosition_ = position;
        revealMouseAfterActivity();
        return true;
    }
    const qreal devicePixelRatio =
        TerminalPaneRenderer::normalizedDevicePixelRatio(
            window() != nullptr ? window()->devicePixelRatio() : 1.0);
    const QPointF physicalDelta =
        (position - *lastPointerActivityPosition_) * devicePixelRatio;
    if (std::abs(physicalDelta.x()) >= 1.0
        || std::abs(physicalDelta.y()) >= 1.0) {
        lastPointerActivityPosition_ = position;
        revealMouseAfterActivity();
        return true;
    }
    return false;
}

bool TerminalPane::handlePointerMotion(const QPointF &position)
{
    if (!revealMouseForPointerPosition(position) || !options_.focusFollowsMouse
        || hasActiveFocus()) {
        return true;
    }

    QQuickWindow *const host = window();
    if (host == nullptr || !host->isActive()) {
        return true;
    }

    // Focus-in observers synchronously publish the active pane and may destroy
    // either the pane or its workspace. Defer our activation publication
    // until forceActiveFocus returns so deletion cannot occur while Qt Quick
    // is still traversing its internal focus tree.
    const QPointer<TerminalPane> guard(this);
    pointerFocusActivationDeferred_ = true;
    forceActiveFocus(Qt::MouseFocusReason);
    if (guard == nullptr) return false;
    pointerFocusActivationDeferred_ = false;
    if (!hasActiveFocus()) return true;
    Q_EMIT activated(this);
    return guard != nullptr;
}

void TerminalPane::mouseMoveEvent(QMouseEvent *event)
{
    if (!handlePointerMotion(event->position())) {
        event->accept();
        return;
    }
    if (hyperlinkPressArmed_
        && (event->position() - hyperlinkPressPosition_).manhattanLength()
            >= QGuiApplication::styleHints()->startDragDistance()) {
        if (!hyperlinkPressDragged_) {
            hyperlinkPressDragged_ = true;
            controller_->cancelHyperlinkActivation(hyperlinkPressRequestId_);
            hyperlinkPressRequestId_ = 0;
        }
    }
    updateHyperlinkHover(event->position(), event->modifiers());
    if (linkPreviewPointerCaptured_ && event->buttons() == Qt::NoButton) {
        event->accept();
        return;
    }
    const Qt::KeyboardModifiers modifiers = hoverModifiers_;
    // Upstream reevaluates the policy for every event. Shift bypasses
    // application capture only while a physical button is held; ordinary
    // hover motion remains reportable.
    const bool shiftBypassesCapture = event->buttons() != Qt::NoButton
        && shiftBypassesMouseCapture(modifiers);
    const bool report = controller_->mouseTracking() && !shiftBypassesCapture;
    if (report) {
        sendMouse(event->position(), TerminalMouseInput::Motion, Qt::NoButton,
                  event->buttons(), modifiers);
    } else if (selecting_ && event->buttons().testFlag(Qt::LeftButton)) {
        const QPoint cell = cellAt(event->position());
        const qreal devicePixelRatio =
            TerminalPaneRenderer::normalizedDevicePixelRatio(
                window() != nullptr ? window()->devicePixelRatio() : 1.0);
        controller_->updateSelection({
            .column = cell.x(),
            .row = cell.y(),
            .surfaceX = event->position().x() * devicePixelRatio,
            .surfaceY = event->position().y() * devicePixelRatio,
            .rectangular = isRectangleSelectionState(modifiers),
        });
    }
    event->accept();
}

void TerminalPane::mouseReleaseEvent(QMouseEvent *event)
{
    revealMouseAfterActivity();
    if (hyperlinkPressArmed_
        && (event->position() - hyperlinkPressPosition_).manhattanLength()
            >= QGuiApplication::styleHints()->startDragDistance()) {
        if (!hyperlinkPressDragged_) {
            hyperlinkPressDragged_ = true;
            controller_->cancelHyperlinkActivation(hyperlinkPressRequestId_);
            hyperlinkPressRequestId_ = 0;
        }
    }
    updateHyperlinkHover(event->position(), event->modifiers());
    const Qt::KeyboardModifiers modifiers = hoverModifiers_;
    const bool report =
        controller_->mouseTracking() && !shiftBypassesMouseCapture(modifiers);
    if (event->button() == Qt::LeftButton && selecting_) {
        const QPoint cell = cellAt(event->position());
        controller_->endSelection(cell.x(), cell.y());
        selecting_ = false;
    }
    const bool activateHyperlink = event->button() == Qt::LeftButton
        && hyperlinkPressArmed_ && !hyperlinkPressDragged_
        && hyperlinkModifiersMatch(hoverModifiers_)
        && hyperlinkPressRequestId_ != 0 && hoverCell_.x() >= 0
        && hoverCell_.y() >= 0;
    if (activateHyperlink) {
        pendingActivationKind_ = hyperlinkPressKind_;
        pendingActivationUri_ = hyperlinkPressUri_;
        pendingActivationRequestId_ = hyperlinkPressRequestId_;
        controller_->commitHyperlinkActivation(hyperlinkPressRequestId_,
                                               hoverCell_.x(), hoverCell_.y());
    } else if (report) {
        sendMouse(event->position(), TerminalMouseInput::Release,
                  event->button(), event->buttons(), modifiers);
    }
    if (!activateHyperlink) {
        controller_->cancelHyperlinkActivation(hyperlinkPressRequestId_);
    }
    hyperlinkPressArmed_ = false;
    hyperlinkPressDragged_ = false;
    hyperlinkPressCell_ = QPoint(-1, -1);
    hyperlinkPressKind_ = TerminalLinkKind::Osc8;
    hyperlinkPressUri_.clear();
    hyperlinkPressRequestId_ = 0;
    event->accept();
}

void TerminalPane::mouseUngrabEvent()
{
    QQuickItem::mouseUngrabEvent();

    const QPointer<TerminalPane> guard(this);
    if (std::exchange(selecting_, false)) {
        // Qt revoked the implicit grab without delivering a release. Reset
        // only the gesture: the selection reached so far remains installed,
        // and capture loss must not trigger copy-on-select.
        controller_->cancelSelectionGesture();
        if (guard == nullptr) return;
    }
    cancelHyperlinkPress();
}

void TerminalPane::hoverMoveEvent(QHoverEvent *event)
{
    if (!handlePointerMotion(event->position())) {
        event->accept();
        return;
    }
    updateHyperlinkHover(event->position(), event->modifiers());
    const Qt::KeyboardModifiers modifiers = hoverModifiers_;
    if (!linkPreviewPointerCaptured_ && controller_->mouseTracking()) {
        sendMouse(event->position(), TerminalMouseInput::Motion, Qt::NoButton,
                  Qt::NoButton, modifiers);
    }
    event->accept();
}

void TerminalPane::hoverEnterEvent(QHoverEvent *event)
{
    // QQuickItem delivers HoverEnter instead of HoverMove for the first
    // position after crossing into the item. Route both through the same
    // activity and terminal-hover path so entry does not require a second
    // physical motion to reveal a typing-hidden cursor.
    QQuickItem::hoverEnterEvent(event);
    hoverMoveEvent(event);
}

void TerminalPane::hoverLeaveEvent(QHoverEvent *event)
{
    revealMouseAfterActivity();
    hoverInside_ = false;
    hoverCell_ = QPoint(-1, -1);
    clearHyperlinkHover();
    cancelHyperlinkPress();
    QQuickItem::hoverLeaveEvent(event);
    event->accept();
}

void TerminalPane::wheelEvent(QWheelEvent *event)
{
    revealMouseAfterActivity();
    QPoint pixelDelta = event->pixelDelta();
    const QPoint angleDelta = event->angleDelta();
    const bool precision = !pixelDelta.isNull();
    if (precision && options_.horizontalTabScroll && pixelDelta.x() != 0) {
        pendingHorizontalTabScrollPixels_ +=
            static_cast<double>(pixelDelta.x());
        pixelDelta.setX(0);

        if (std::abs(pendingHorizontalTabScrollPixels_)
            >= kHorizontalTabScrollThreshold) {
            const int direction =
                pendingHorizontalTabScrollPixels_ < 0.0 ? 1 : -1;
            pendingHorizontalTabScrollPixels_ = 0.0;
            horizontalTabScrollResetTimer_->stop();
            const QPointer<TerminalPane> guard(this);
            Q_EMIT requestTabChange(direction);
            if (guard == nullptr) return;
        } else {
            horizontalTabScrollResetTimer_->start();
        }

        // The frontend owns the precision X component, including movement
        // below the tab-switch threshold. An accompanying Y component remains
        // terminal scroll input.
        if (pixelDelta.isNull()) {
            event->accept();
            return;
        }
    }

    qreal logicalCellWidth = 0.0;
    qreal logicalCellHeight = 0.0;
    {
        QMutexLocker locker(&renderMutex_);
        logicalCellWidth = metrics_.cellWidth;
        logicalCellHeight = metrics_.cellHeight;
    }
    const qreal devicePixelRatio =
        TerminalPaneRenderer::normalizedDevicePixelRatio(
            window() != nullptr ? window()->devicePixelRatio() : 1.0);
    const double physicalCellWidth =
        static_cast<double>(TerminalPaneRenderer::physicalPixels(
            logicalCellWidth, devicePixelRatio));
    const double physicalCellHeight =
        static_cast<double>(TerminalPaneRenderer::physicalPixels(
            logicalCellHeight, devicePixelRatio));

    const QPoint effectiveDelta = precision ? pixelDelta : angleDelta;
    if (effectiveDelta.isNull()) {
        event->ignore();
        return;
    }

    const auto boundedWholeSteps = [](long double amount) {
        const long double maximum =
            static_cast<long double>(kMaximumMouseScrollStepsPerAxis);
        if (amount >= maximum) {
            return kMaximumMouseScrollStepsPerAxis;
        }
        if (amount <= -maximum) {
            return -kMaximumMouseScrollStepsPerAxis;
        }
        return static_cast<qint64>(amount);
    };
    const auto accumulatePrecisionAxis =
        [&boundedWholeSteps](double offset, double cellSize, double &pending) {
            if (!std::isfinite(offset) || !std::isfinite(cellSize)
                || cellSize <= 0.0) {
                pending = 0.0;
                return qint64{0};
            }
            const double accumulated = pending + offset;
            if (!std::isfinite(accumulated)) {
                pending = 0.0;
                return qint64{0};
            }

            const long double amount = static_cast<long double>(accumulated)
                / static_cast<long double>(cellSize);
            if (std::abs(amount) < 1.0L) {
                pending = accumulated;
                return qint64{0};
            }

            const qint64 steps = boundedWholeSteps(std::trunc(amount));
            // Bound one GUI dispatch without dropping coalesced or synthesized
            // precision movement. An extreme event carries its undispatched
            // whole-cell debt into the next event.
            pending = accumulated - static_cast<double>(steps) * cellSize;
            if (!std::isfinite(pending)) {
                pending = 0.0;
            }
            return steps;
        };

    qint64 rows = 0;
    qint64 columns = 0;
    if (precision) {
        if (pixelDelta.y() != 0) {
            const double multiplier = normalizedMouseScrollMultiplier(
                options_.mouseScrollMultiplier.precision, 1.0);
            rows = accumulatePrecisionAxis(static_cast<double>(pixelDelta.y())
                                               * devicePixelRatio * multiplier,
                                           physicalCellHeight,
                                           pendingWheelVerticalPixels_);
        }
        if (pixelDelta.x() != 0) {
            // Horizontal precision scrolling follows Ghostty's physical
            // cell-width threshold and intentionally ignores the vertical
            // mouse-scroll multiplier.
            columns = accumulatePrecisionAxis(
                static_cast<double>(pixelDelta.x()) * devicePixelRatio,
                physicalCellWidth, pendingWheelHorizontalPixels_);
        }
    } else {
        if (angleDelta.y() != 0 && std::isfinite(physicalCellHeight)
            && physicalCellHeight > 0.0) {
            const double ticks = static_cast<double>(angleDelta.y()) / 120.0;
            const double multiplier = normalizedMouseScrollMultiplier(
                options_.mouseScrollMultiplier.discrete, 3.0);
            rows = accumulatePrecisionAxis(
                ticks * physicalCellHeight * multiplier, physicalCellHeight,
                pendingWheelVerticalPixels_);
        }
        if (angleDelta.x() != 0) {
            // Unlike vertical discrete input, Ghostty rounds horizontal ticks
            // independently and does not apply a scroll multiplier.
            const long double ticks =
                static_cast<long double>(angleDelta.x()) / 120.0L;
            columns = boundedWholeSteps(std::round(ticks));
        }
    }

    if (rows == 0 && columns == 0) {
        if (controller_->mouseTracking()) {
            controller_->clearSelectionIfMouseTracking();
        }
        event->accept();
        return;
    }
    const Qt::KeyboardModifiers modifiers =
        effectivePointerModifiers(event->modifiers());
    controller_->sendWheel({
        .rows = rows,
        .columns = columns,
        .modifiers = static_cast<int>(modifiers),
        .x = static_cast<float>(event->position().x() * devicePixelRatio),
        .y = static_cast<float>(event->position().y() * devicePixelRatio),
        .mouseReportingEnabled = controller_->mouseReportingEnabled(),
    });
    event->accept();
}

void TerminalPane::dragEnterEvent(QDragEnterEvent *event)
{
    const QMimeData *const mimeData = event->mimeData();
    if (mimeData == nullptr || (!mimeData->hasUrls() && !mimeData->hasText())) {
        event->ignore();
        return;
    }
    (void)acceptTerminalDrop(event);
}

void TerminalPane::dragMoveEvent(QDragMoveEvent *event)
{
    const QMimeData *const mimeData = event->mimeData();
    if (mimeData == nullptr || (!mimeData->hasUrls() && !mimeData->hasText())) {
        event->ignore();
        return;
    }
    (void)acceptTerminalDrop(event);
}

void TerminalPane::dropEvent(QDropEvent *event)
{
    const QMimeData *const mimeData = event->mimeData();
    if (mimeData == nullptr) {
        event->ignore();
        return;
    }

    TerminalDropContent content = terminalDropContent(*mimeData);
    if (!content.recognized) {
        event->ignore();
        return;
    }

    // Accept before entering the paste path because signal handlers may
    // synchronously remove this pane.
    if (acceptTerminalDrop(event) && !content.text.isEmpty()) {
        pasteText(content.text);
    }
}

void TerminalPane::sendMouse(const QPointF &position,
                             TerminalMouseInput::Action action,
                             Qt::MouseButton button, Qt::MouseButtons buttons,
                             Qt::KeyboardModifiers modifiers)
{
    const qreal devicePixelRatio =
        window() != nullptr ? window()->devicePixelRatio() : 1.0;
    TerminalMouseInput input;
    input.action = action;
    Qt::MouseButton effectiveButton = button;
    if (action == TerminalMouseInput::Motion
        && effectiveButton == Qt::NoButton) {
        // Button-event tracking (DECSET 1002) requires the identity of the
        // held button on motion, not only a generic "some button" flag.
        if (buttons.testFlag(Qt::LeftButton))
            effectiveButton = Qt::LeftButton;
        else if (buttons.testFlag(Qt::MiddleButton))
            effectiveButton = Qt::MiddleButton;
        else if (buttons.testFlag(Qt::RightButton))
            effectiveButton = Qt::RightButton;
        else if (buttons.testFlag(Qt::BackButton))
            effectiveButton = Qt::BackButton;
        else if (buttons.testFlag(Qt::ForwardButton))
            effectiveButton = Qt::ForwardButton;
    }
    input.button = normalizedMouseButton(effectiveButton);
    input.modifiers = static_cast<int>(modifiers);
    input.x = static_cast<float>(position.x() * devicePixelRatio);
    input.y = static_cast<float>(position.y() * devicePixelRatio);
    input.anyButtonPressed = buttons != Qt::NoButton;
    controller_->sendMouse(input);
}

int TerminalPane::normalizedMouseButton(Qt::MouseButton button) const
{
    switch (button) {
    case Qt::LeftButton: return 1;
    case Qt::RightButton: return 2;
    case Qt::MiddleButton: return 3;
    // Values four/five are protocol wheel events. Physical side buttons map
    // to their distinct extended identities.
    case Qt::BackButton: return 8;
    case Qt::ForwardButton: return 9;
    default: return 0;
    }
}

Qt::KeyboardModifiers
TerminalPane::effectivePointerModifiers(Qt::KeyboardModifiers modifiers) const
{
    return normalizedModifiers(modifiers) | keyboardModifiers_;
}

bool TerminalPane::shiftBypassesMouseCapture(
    Qt::KeyboardModifiers modifiers) const noexcept
{
    if (!normalizedModifiers(modifiers).testFlag(Qt::ShiftModifier)) {
        return false;
    }
    switch (options_.mouseShiftCapture) {
    case MouseShiftCapture::True:
    case MouseShiftCapture::Always: return false;
    case MouseShiftCapture::False:
    case MouseShiftCapture::Never: return true;
    }
    return true;
}

bool TerminalPane::hyperlinkModifiersMatch(
    Qt::KeyboardModifiers modifiers) const
{
    modifiers = normalizedModifiers(modifiers);
    if (controller_->terminalMouseTracking()) {
        // Shift is Ghostty's Linux escape hatch from application mouse
        // capture. Once it releases capture, it is removed before matching
        // the exact Ctrl-only OSC 8 modifier.
        if (!shiftBypassesMouseCapture(modifiers)) {
            return false;
        }
        modifiers &= ~Qt::ShiftModifier;
    }
    return modifiers == Qt::ControlModifier;
}

std::optional<QPoint> TerminalPane::hoverCellAt(const QPointF &position) const
{
    const std::optional<TerminalViewportLayout> layout =
        currentViewportLayout();
    if (!layout) return std::nullopt;
    const std::optional<QPoint> cell = layout->strictCellAt(position);
    if (!cell) return std::nullopt;

    QMutexLocker locker(&renderMutex_);
    if (!hasFrame_ || cell->x() >= frame_.columns || cell->y() >= frame_.rows) {
        return std::nullopt;
    }
    return cell;
}

void TerminalPane::updateHyperlinkHover(const QPointF &position,
                                        Qt::KeyboardModifiers modifiers)
{
    hoverInside_ = true;
    hoverPosition_ = position;
    hoverModifiers_ = effectivePointerModifiers(modifiers);
    syncPointerCursor();
    if (!hyperlinkModifiersMatch(hoverModifiers_)) {
        cancelHyperlinkPress();
        clearHyperlinkHover();
        return;
    }

    bool previewCapturesPointer = false;
    {
        QMutexLocker locker(&renderMutex_);
        previewCapturesPointer = hyperlinkLeaseActive_
            && !linkPreviewText_.isEmpty()
            && linkPreviewGuardRect_.contains(position);
    }
    linkPreviewPointerCaptured_ = previewCapturesPointer;
    if (previewCapturesPointer) {
        // GTK keeps the accepted link lease while its bottom-left preview
        // owns the pointer, hides that copy, and exposes a bottom-right copy.
        // Preserve the logical hover cell until the pointer leaves the
        // original guard instead of querying the obscured terminal row.
        refreshLinkPreview();
        return;
    }

    const std::optional<QPoint> cell = hoverCellAt(position);
    const QPoint nextCell = cell.value_or(QPoint(-1, -1));
    if (nextCell != hoverCell_) {
        if (hyperlinkPressArmed_) {
            hyperlinkPressDragged_ = true;
            controller_->cancelHyperlinkActivation(hyperlinkPressRequestId_);
            hyperlinkPressRequestId_ = 0;
        }
        bool remainsOnResolvedLink = false;
        {
            QMutexLocker locker(&renderMutex_);
            const int index = nextCell.y() * frame_.columns + nextCell.x();
            remainsOnResolvedLink = nextCell.x() >= 0
                && nextCell.x() < frame_.columns && nextCell.y() >= 0
                && nextCell.y() < frame_.rows
                && hoveredHyperlinkColumns_ == frame_.columns
                && hoveredHyperlinkRows_ == frame_.rows
                && index < hoveredHyperlinkCellMask_.size()
                && hoveredHyperlinkCellMask_.testBit(index)
                && hyperlinkLeaseActive_ && !hyperlinkPressDragged_
                && hyperlinkModifiersMatch(hoverModifiers_);
        }
        hoverCell_ = nextCell;
        if (remainsOnResolvedLink) {
            hyperlinkQueryCell_ = nextCell;
            hoveredHyperlinkCell_ = nextCell;
        } else {
            clearHyperlinkHover();
        }
    }
    refreshHyperlinkHover();
    refreshLinkPreview();
}

void TerminalPane::recomputeHyperlinkHover()
{
    if (hoverInside_) {
        updateHyperlinkHover(hoverPosition_, hoverModifiers_);
    }
}

void TerminalPane::updateHyperlinkModifiers(Qt::KeyboardModifiers modifiers)
{
    keyboardModifiers_ = normalizedModifiers(modifiers);
    hoverModifiers_ = keyboardModifiers_;
    syncPointerCursor();
    if (!hyperlinkModifiersMatch(hoverModifiers_)) {
        cancelHyperlinkPress();
        clearHyperlinkHover();
        return;
    }
    refreshHyperlinkHover();
}

void TerminalPane::refreshHyperlinkHover()
{
    if (!hoverInside_ || hoverCell_.x() < 0 || hoverCell_.y() < 0
        || hyperlinkPressDragged_
        || !hyperlinkModifiersMatch(hoverModifiers_)) {
        return;
    }
    if (hyperlinkQueryCell_ == hoverCell_) {
        if (hyperlinkQueryPending_ || hyperlinkQueryRejected_) {
            return;
        }
        if (hyperlinkLeaseActive_) {
            return;
        }
    }

    quint64 contentRevision = 0;
    bool targetMayHaveLink = false;
    {
        QMutexLocker locker(&renderMutex_);
        if (!hasFrame_ || hoverCell_.x() >= frame_.columns
            || hoverCell_.y() >= frame_.rows) {
            return;
        }
        contentRevision = frame_.contentRevision;
        const int targetIndex =
            hoverCell_.y() * frame_.columns + hoverCell_.x();
        targetMayHaveLink = options_.linkUrl
            || (targetIndex >= 0 && targetIndex < frame_.cells.size()
                && frame_.cells.at(targetIndex).hasHyperlink);
    }

    if (!targetMayHaveLink) {
        clearHyperlinkHover();
        return;
    }
    clearHyperlinkHover();
    hyperlinkQueryCell_ = hoverCell_;
    hyperlinkQueryPending_ = true;
    hyperlinkQueryRejected_ = false;
    controller_->requestHyperlink(hoverCell_.x(), hoverCell_.y(),
                                  contentRevision);
}

void TerminalPane::refreshLinkPreview()
{
    const bool modeAllowsPreview =
        options_.linkPreviews == LinkPreviewMode::Always
        || (options_.linkPreviews == LinkPreviewMode::Osc8
            && hoveredLinkKind_ == TerminalLinkKind::Osc8);
    const bool visible = modeAllowsPreview && hyperlinkLeaseActive_
        && hoverInside_ && hyperlinkModifiersMatch(hoverModifiers_)
        && !hoveredHyperlinkUri_.isEmpty();

    QString text;
    QRectF guardRect;
    QRectF previewRect;
    bool pointerCaptured = false;
    if (visible && width() > 0.0 && height() > 0.0) {
        QFont font;
        {
            QMutexLocker locker(&renderMutex_);
            font = metrics_.font(TerminalFontRole::Regular);
        }
        const QFontMetricsF metrics(font);
        const qreal maximumTextWidth = std::max<qreal>(
            1.0,
            width() - 2.0 * TerminalPaneRenderer::linkPreviewHorizontalPadding);
        text =
            metrics.elidedText(linkPreviewDisplaySource(hoveredHyperlinkUri_),
                               Qt::ElideMiddle, maximumTextWidth);
        if (!text.isEmpty()) {
            const qreal previewWidth = std::min(
                width(),
                std::ceil(metrics.horizontalAdvance(text))
                    + 2.0 * TerminalPaneRenderer::linkPreviewHorizontalPadding);
            const qreal previewHeight = std::min(
                height(),
                std::ceil(metrics.height())
                    + 2.0 * TerminalPaneRenderer::linkPreviewVerticalPadding);
            guardRect = QRectF(0.0, std::max(0.0, height() - previewHeight),
                               previewWidth, previewHeight);
            pointerCaptured = guardRect.contains(hoverPosition_);
            previewRect = guardRect;
            if (pointerCaptured) {
                previewRect.moveLeft(std::max(0.0, width() - previewWidth));
            }
        }
    }

    bool changed = false;
    {
        QMutexLocker locker(&renderMutex_);
        changed = linkPreviewText_ != text || linkPreviewRect_ != previewRect;
        linkPreviewText_ = std::move(text);
        linkPreviewRect_ = previewRect;
        linkPreviewGuardRect_ = guardRect;
    }
    linkPreviewPointerCaptured_ = pointerCaptured;
    if (changed) {
        requestRenderUpdate();
        Q_EMIT linkPreviewChanged();
    }
}

void TerminalPane::reconcileReleasedLinkPreview(bool wasPointerCaptured,
                                                bool forceRequery)
{
    if (!wasPointerCaptured || linkPreviewPointerCaptured_ || !hoverInside_) {
        return;
    }
    const QPoint physicalCell =
        hoverCellAt(hoverPosition_).value_or(QPoint(-1, -1));
    if (!forceRequery && physicalCell == hoverCell_) {
        return;
    }

    // The left preview guard deliberately preserves the logical source cell
    // while it owns the pointer. Once the guard disappears, resume hit testing
    // at the physical position instead of leaving copy/underline state bound
    // to a link elsewhere in the pane.
    clearHyperlinkHover();
    hoverCell_ = QPoint(-1, -1);
    updateHyperlinkHover(hoverPosition_, hoverModifiers_);
}

void TerminalPane::clearHyperlinkDecoration()
{
    hoveredLinkKind_ = TerminalLinkKind::Osc8;
    hoveredHyperlinkUri_.clear();
    hoveredHyperlinkCell_ = QPoint(-1, -1);

    bool hadHighlight = false;
    bool previewChanged = false;
    {
        QMutexLocker locker(&renderMutex_);
        hadHighlight = !hoveredHyperlinkCellMask_.isEmpty();
        markSolidMaskRowsChangedLocked(
            hoveredHyperlinkCellMask_, hoveredHyperlinkColumns_,
            hoveredHyperlinkRows_, {}, hoveredHyperlinkColumns_,
            hoveredHyperlinkRows_);
        hoveredHyperlinkCellMask_.clear();
        hoveredHyperlinkColumns_ = 0;
        hoveredHyperlinkRows_ = 0;
        previewChanged =
            !linkPreviewText_.isEmpty() || !linkPreviewRect_.isEmpty();
        linkPreviewText_.clear();
        linkPreviewRect_ = {};
        linkPreviewGuardRect_ = {};
    }
    linkPreviewPointerCaptured_ = false;
    syncPointerCursor();
    if (hadHighlight || previewChanged) {
        requestRenderUpdate();
    }
    if (previewChanged) {
        Q_EMIT linkPreviewChanged();
    }
}

void TerminalPane::clearHyperlinkHover()
{
    controller_->cancelHyperlinkRequest();
    hyperlinkQueryCell_ = QPoint(-1, -1);
    hyperlinkQueryPending_ = false;
    hyperlinkLeaseActive_ = false;
    hyperlinkQueryRejected_ = false;
    clearHyperlinkDecoration();
}

void TerminalPane::cancelHyperlinkPress()
{
    controller_->cancelHyperlinkActivation(hyperlinkPressRequestId_);
    hyperlinkPressArmed_ = false;
    hyperlinkPressDragged_ = false;
    hyperlinkPressCell_ = QPoint(-1, -1);
    hyperlinkPressKind_ = TerminalLinkKind::Osc8;
    hyperlinkPressUri_.clear();
    hyperlinkPressRequestId_ = 0;
}

void TerminalPane::cancelPendingHyperlinkActivation()
{
    controller_->cancelHyperlinkActivation(pendingActivationRequestId_);
    pendingActivationRequestId_ = 0;
    pendingActivationKind_ = TerminalLinkKind::Osc8;
    pendingActivationUri_.clear();
}

bool TerminalPane::hyperlinkCellCandidate(const QPoint &cell,
                                          quint64 *contentRevision) const
{
    QMutexLocker locker(&renderMutex_);
    if (!hasFrame_ || cell.x() < 0 || cell.x() >= frame_.columns || cell.y() < 0
        || cell.y() >= frame_.rows) {
        return false;
    }
    const int index = cell.y() * frame_.columns + cell.x();
    const bool osc8Candidate = index >= 0 && index < frame_.cells.size()
        && frame_.cells.at(index).hasHyperlink;
    const bool resolvedRegexCandidate = options_.linkUrl
        && hoveredLinkKind_ == TerminalLinkKind::Regex
        && hoveredHyperlinkColumns_ == frame_.columns
        && hoveredHyperlinkRows_ == frame_.rows
        && index < hoveredHyperlinkCellMask_.size()
        && hoveredHyperlinkCellMask_.testBit(index);
    if (!osc8Candidate && !resolvedRegexCandidate) {
        return false;
    }
    if (contentRevision != nullptr) {
        *contentRevision = frame_.contentRevision;
    }
    return true;
}

void TerminalPane::handleHyperlinkResult(quint64 contentRevision,
                                         TerminalHyperlinkState state,
                                         TerminalLinkKind kind,
                                         const QByteArray &uri,
                                         const QPoint &targetCell,
                                         const QVector<QPoint> &matchingCells)
{
    if (!hyperlinkQueryPending_ && !hyperlinkLeaseActive_) {
        return;
    }
    const bool hadTrackedLease = hyperlinkLeaseActive_;
    const bool previewWasPointerCaptured = linkPreviewPointerCaptured_;
    hyperlinkQueryPending_ = false;

    quint64 currentRevision = 0;
    int columns = 0;
    int rows = 0;
    qsizetype cellCount = 0;
    {
        QMutexLocker locker(&renderMutex_);
        currentRevision = frame_.contentRevision;
        columns = frame_.columns;
        rows = frame_.rows;
        cellCount = frame_.cells.size();
    }
    if (hyperlinkQueryCell_ != hoverCell_ || !hoverInside_
        || !hyperlinkModifiersMatch(hoverModifiers_)) {
        clearHyperlinkHover();
        return;
    }

    if (state == TerminalHyperlinkState::Hidden) {
        hyperlinkLeaseActive_ = true;
        hyperlinkQueryRejected_ = false;
        clearHyperlinkDecoration();
        if (previewWasPointerCaptured) {
            reconcileReleasedLinkPreview(true, true);
            return;
        }
        if (hyperlinkCellCandidate(hoverCell_)) {
            clearHyperlinkHover();
            refreshHyperlinkHover();
        }
        return;
    }
    if (state == TerminalHyperlinkState::Stale) {
        hyperlinkLeaseActive_ = false;
        hyperlinkQueryRejected_ = false;
        hyperlinkQueryCell_ = QPoint(-1, -1);
        clearHyperlinkDecoration();
        if (previewWasPointerCaptured) {
            reconcileReleasedLinkPreview(true, true);
            return;
        }
        if (currentRevision >= contentRevision) {
            refreshHyperlinkHover();
        }
        return;
    }
    if (state != TerminalHyperlinkState::Visible || uri.isEmpty()) {
        hyperlinkLeaseActive_ = false;
        hyperlinkQueryRejected_ = !hadTrackedLease;
        clearHyperlinkDecoration();
        if (previewWasPointerCaptured) {
            reconcileReleasedLinkPreview(true, true);
            return;
        }
        if (hadTrackedLease) {
            hyperlinkQueryCell_ = QPoint(-1, -1);
            refreshHyperlinkHover();
        }
        return;
    }

    QBitArray indexes(cellCount);
    for (const QPoint &cell : matchingCells) {
        if (cell.x() >= 0 && cell.x() < columns && cell.y() >= 0
            && cell.y() < rows) {
            const qsizetype index =
                static_cast<qsizetype>(cell.y()) * columns + cell.x();
            if (index < indexes.size()) indexes.setBit(index);
        }
    }
    if (targetCell.x() >= 0 && targetCell.x() < columns && targetCell.y() >= 0
        && targetCell.y() < rows) {
        const qsizetype index =
            static_cast<qsizetype>(targetCell.y()) * columns + targetCell.x();
        if (index < indexes.size()) indexes.setBit(index);
    }
    const qsizetype hoverIndex =
        static_cast<qsizetype>(hoverCell_.y()) * columns + hoverCell_.x();
    if (hoverIndex < 0 || hoverIndex >= indexes.size()
        || !indexes.testBit(hoverIndex)) {
        clearHyperlinkHover();
        refreshHyperlinkHover();
        return;
    }

    hyperlinkLeaseActive_ = true;
    hyperlinkQueryRejected_ = false;
    hoveredLinkKind_ = kind;
    hoveredHyperlinkUri_ = uri;
    hoveredHyperlinkCell_ = hoverCell_;
    {
        QMutexLocker locker(&renderMutex_);
        markSolidMaskRowsChangedLocked(
            hoveredHyperlinkCellMask_, hoveredHyperlinkColumns_,
            hoveredHyperlinkRows_, indexes, columns, rows);
        hoveredHyperlinkCellMask_ = std::move(indexes);
        hoveredHyperlinkColumns_ = columns;
        hoveredHyperlinkRows_ = rows;
    }
    refreshLinkPreview();
    reconcileReleasedLinkPreview(previewWasPointerCaptured);
    if (!hyperlinkLeaseActive_) {
        return;
    }
    syncPointerCursor();
    requestRenderUpdate();
}

QUrl TerminalPane::hyperlinkUrl(const QByteArray &uri,
                                TerminalLinkKind kind) const
{
    if (uri.isEmpty() || uri.contains('\0')) {
        return {};
    }
    if (kind == TerminalLinkKind::Regex) {
        const QString value = QString::fromUtf8(uri);
        if (!QDir::isAbsolutePath(value)) {
            const QString directory = controller_->currentDirectory();
            if (!directory.isEmpty()) {
                const QString resolved =
                    QDir::cleanPath(QDir(directory).absoluteFilePath(value));
                if (QFileInfo::exists(resolved)) {
                    return QUrl::fromLocalFile(resolved);
                }
            }
        }
    }
    const QUrl url = uri.startsWith('/')
        ? QUrl::fromLocalFile(QString::fromUtf8(uri))
        : QUrl::fromEncoded(uri, QUrl::StrictMode);
    return url.isValid() && !url.isEmpty() ? url : QUrl{};
}

void TerminalPane::handleHyperlinkActivation(quint64 contentRevision,
                                             TerminalLinkKind kind,
                                             const QByteArray &uri)
{
    static_cast<void>(contentRevision);
    const quint64 requestId = std::exchange(pendingActivationRequestId_, 0);
    const TerminalLinkKind expectedKind =
        std::exchange(pendingActivationKind_, TerminalLinkKind::Osc8);
    const QByteArray expectedUri = std::exchange(pendingActivationUri_, {});
    if (requestId == 0 || uri.isEmpty() || kind != expectedKind
        || (!expectedUri.isEmpty() && uri != expectedUri) || !urlOpener_) {
        return;
    }
    const QUrl url = hyperlinkUrl(uri, kind);
    if (url.isValid() && !url.isEmpty()) {
        static_cast<void>(urlOpener_(url));
    }
}

void TerminalPane::handleRightClickResult(
    const TerminalRightClickResult &result)
{
    if (result.requestId == 0
        || !pendingRightClickWindowPositions_.contains(result.requestId)) {
        return;
    }
    const QPointF windowPosition =
        pendingRightClickWindowPositions_.take(result.requestId);
    const bool newest = result.requestId == newestRightClickRequestId_;
    if (newest) {
        newestRightClickRequestId_ = 0;
    }

    switch (result.effect) {
    case TerminalRightClickEffect::None: return;
    case TerminalRightClickEffect::Paste: {
        const std::optional<QString> text = readTerminalClipboard(
            QGuiApplication::clipboard(), TerminalClipboardSource::Standard);
        if (text.has_value()) {
            pasteText(*text);
        }
        return;
    }
    case TerminalRightClickEffect::ContextMenu:
        if (newest) {
            Q_EMIT contextMenuRequested(windowPosition,
                                        result.selectionAvailable);
        }
        return;
    }
}

QPoint TerminalPane::cellAt(const QPointF &position) const
{
    const std::optional<TerminalViewportLayout> layout =
        currentViewportLayout();
    return layout ? layout->clampedCellAt(position) : QPoint{};
}

void TerminalPane::focusInEvent(QFocusEvent *event)
{
    const QPointer<TerminalPane> guard(this);
    QQuickItem::focusInEvent(event);
    if (guard == nullptr) return;
    revealMouseAfterActivity();
    if (guard == nullptr) return;
    setBellRinging(false);
    if (guard == nullptr) return;
    syncCursorBlink(true);
    controller_->setFocused(true);
    if (guard == nullptr) return;
    if (!pointerFocusActivationDeferred_) {
        Q_EMIT activated(this);
    }
}

void TerminalPane::focusOutEvent(QFocusEvent *event)
{
    revealMouseAfterActivity();
    ++keyFocusEpoch_;
    consumedKeys_.clear();
    cursorTimer_->stop();
    cursorBlinkOn_ = true;
    hoverInside_ = false;
    hoverCell_ = QPoint(-1, -1);
    keyboardModifiers_ = Qt::NoModifier;
    hoverModifiers_ = Qt::NoModifier;
    clearHyperlinkHover();
    cancelHyperlinkPress();
    cancelPendingHyperlinkActivation();
    controller_->setFocused(false);
    QQuickItem::focusOutEvent(event);
    requestRenderUpdate();
}

void TerminalPane::focusTerminal()
{
    if (searchUiActive_) {
        Q_EMIT searchUiFocusRequested();
        return;
    }
    forceActiveFocus(Qt::OtherFocusReason);
}

void TerminalPane::copySelection()
{
    controller_->copySelection();
}

void TerminalPane::pasteText(const QString &text)
{
    controller_->paste(text);
}

void TerminalPane::confirmPaste(quint64 requestId)
{
    controller_->confirmPaste(requestId);
}

void TerminalPane::cancelPaste(quint64 requestId)
{
    controller_->cancelPaste(requestId);
}

void TerminalPane::rememberTerminalClipboardAccess(
    TerminalClipboardAccess access)
{
    if (access == TerminalClipboardAccess::Ask
        || options_.clipboardWrite == access) {
        return;
    }
    options_.clipboardWrite = access;
    controller_->applyRuntimeOptions(toTerminalSessionRuntimeOptions(options_));
}

void TerminalPane::setFontPointSize(qreal points)
{
    const bool previewWasPointerCaptured = linkPreviewPointerCaptured_;
    TerminalCellMetrics previous;
    {
        QMutexLocker locker(&renderMutex_);
        previous = metrics_;
    }
    if (qFuzzyCompare(previous.font(TerminalFontRole::Regular).pointSizeF(),
                      points)
        || !updateMetrics(options_.typography, points)) {
        return;
    }
    TerminalCellMetrics current;
    {
        QMutexLocker locker(&renderMutex_);
        current = metrics_;
    }
    if (terminalGridMetricsChanged(previous, current)) {
        updateTerminalSize();
    }
    refreshLinkPreview();
    reconcileReleasedLinkPreview(previewWasPointerCaptured);
    requestRenderUpdate();
    Q_EMIT fontPointSizeChanged();
}

void TerminalPane::zoomIn()
{
    (void)performPaneAction(GhosttyPaneAction{
        GhosttyPaneActions::IncreaseFontSize{.points = 1.0F}});
}

void TerminalPane::zoomOut()
{
    (void)performPaneAction(GhosttyPaneAction{
        GhosttyPaneActions::DecreaseFontSize{.points = 1.0F}});
}

void TerminalPane::resetZoom()
{
    (void)performPaneAction(
        GhosttyPaneAction{GhosttyPaneActions::ResetFontSize{}});
}
