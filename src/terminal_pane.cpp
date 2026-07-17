#include "terminal_pane.h"

#include "ghostty_action_catalog.h"
#include "terminal_controller.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFontDatabase>
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
#include <QSGGeometryNode>
#include <QSGSimpleRectNode>
#include <QSGTextNode>
#include <QSGVertexColorMaterial>
#include <QQuickWindow>
#include <QStyleHints>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace {

constexpr qreal kMinimumFontSize = 6.0;
constexpr qreal kMaximumFontSize = 48.0;

struct ColoredRect {
    QRectF rect;
    QColor color;
};

void appendRect(QVector<ColoredRect> &rects, const QRectF &rect, const QColor &color)
{
    if (!rect.isEmpty() && color.isValid() && color.alpha() > 0) {
        rects.append({rect, color});
    }
}

QColor withOpacity(QColor color, double opacity)
{
    if (!color.isValid()) {
        return color;
    }
    color.setAlpha(std::clamp(
        static_cast<int>(std::ceil(std::clamp(opacity, 0.0, 1.0) * 255.0)),
        0, 255));
    return color;
}

QColor resolveRelativeColor(const TerminalColorValue &configured,
                            const QColor &cellForeground,
                            const QColor &cellBackground,
                            const QColor &fallback)
{
    switch (configured.kind) {
    case TerminalColorKind::Color:
        return configured.color.isValid() ? configured.color : fallback;
    case TerminalColorKind::CellForeground:
        return cellForeground;
    case TerminalColorKind::CellBackground:
        return cellBackground;
    case TerminalColorKind::Unset:
        return fallback;
    }
    return fallback;
}

void applyBoldColor(const TerminalCell &cell, const TerminalFrame &frame,
                    const TerminalAppearance &appearance,
                    QColor *foreground, QColor *background)
{
    if (!cell.bold || foreground == nullptr || background == nullptr
        || appearance.boldColor.kind == TerminalBoldColorKind::Unset) {
        return;
    }

    // The source is the logical SGR foreground. Inverse is applied after
    // Ghostty resolves bold-color, so its rendered destination is the cell
    // background when SGR inverse is active.
    QColor *styleForeground = cell.inverse ? background : foreground;
    switch (cell.styleForegroundSource) {
    case TerminalColorSource::Default:
        if (appearance.boldColor.kind == TerminalBoldColorKind::Color
            && appearance.boldColor.color.isValid()) {
            *styleForeground = appearance.boldColor.color;
        }
        break;
    case TerminalColorSource::Palette:
        if (cell.styleForegroundPaletteIndex >= 0
            && cell.styleForegroundPaletteIndex < 8
            && frame.palette.size() > cell.styleForegroundPaletteIndex + 8) {
            const QColor bright = frame.palette.at(
                cell.styleForegroundPaletteIndex + 8);
            if (bright.isValid()) {
                *styleForeground = bright;
            }
        }
        break;
    case TerminalColorSource::Rgb:
        if (appearance.boldColor.kind == TerminalBoldColorKind::Color
            && appearance.boldColor.color.isValid()
            && *styleForeground == frame.foreground) {
            *styleForeground = appearance.boldColor.color;
        }
        break;
    }
}

void appendUnderline(QVector<ColoredRect> &rects, const QRectF &cellRect,
                     qreal baseline, TerminalUnderlineStyle style,
                     const QColor &color)
{
    if (style == TerminalUnderlineStyle::None) {
        return;
    }

    const qreal left = cellRect.left();
    const qreal right = cellRect.right();
    const qreal width = cellRect.width();
    const qreal bottom = cellRect.bottom();
    const qreal lineY = std::clamp(
        cellRect.top() + baseline + 1.0, cellRect.top(), bottom - 1.0);
    const auto appendSegments = [&](qreal segmentWidth, qreal gap) {
        for (qreal x = left; x < right; x += segmentWidth + gap) {
            appendRect(rects,
                       QRectF(x, lineY, std::min(segmentWidth, right - x), 1.0),
                       color);
        }
    };

    switch (style) {
    case TerminalUnderlineStyle::None:
        break;
    case TerminalUnderlineStyle::Single:
        appendRect(rects, QRectF(left, lineY, width, 1.0), color);
        break;
    case TerminalUnderlineStyle::Double: {
        const qreal firstY = std::max(cellRect.top(), lineY - 1.0);
        const qreal secondY = std::min(bottom - 1.0, lineY + 1.0);
        appendRect(rects, QRectF(left, firstY, width, 1.0), color);
        appendRect(rects, QRectF(left, secondY, width, 1.0), color);
        break;
    }
    case TerminalUnderlineStyle::Curly: {
        // A compact two-pixel wave stays legible at small terminal sizes and
        // joins cleanly across adjacent cells.
        constexpr qreal segment = 2.0;
        int phase = 0;
        for (qreal x = left; x < right; x += segment, ++phase) {
            const qreal offset = (phase % 4 == 1 || phase % 4 == 2) ? 1.0 : 0.0;
            appendRect(rects,
                       QRectF(x, std::min(bottom - 1.0, lineY + offset),
                              std::min(segment, right - x), 1.0),
                       color);
        }
        break;
    }
    case TerminalUnderlineStyle::Dotted:
        appendSegments(1.0, 2.0);
        break;
    case TerminalUnderlineStyle::Dashed:
        appendSegments(std::max<qreal>(2.0, std::min<qreal>(4.0, width / 3.0)),
                       2.0);
        break;
    }
}

QSGNode *createRectNode(const QVector<ColoredRect> &rects,
                        bool softwareRenderer)
{
    if (rects.isEmpty()) {
        return nullptr;
    }

    if (softwareRenderer) {
        // Qt's software scene-graph adaptation does not render the public
        // vertex-color material. Keep its deterministic CI/fallback path on
        // the public rectangle node that it supports.
        auto *group = new QSGNode;
        for (const ColoredRect &coloredRect : rects) {
            group->appendChildNode(new QSGSimpleRectNode(
                coloredRect.rect.normalized(), coloredRect.color));
        }
        return group;
    }

    auto *geometry = new QSGGeometry(
        QSGGeometry::defaultAttributes_ColoredPoint2D(),
        static_cast<int>(rects.size()) * 6);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::StaticPattern);
    QSGGeometry::ColoredPoint2D *vertices =
        geometry->vertexDataAsColoredPoint2D();
    int vertex = 0;
    for (const ColoredRect &coloredRect : rects) {
        const QRectF rect = coloredRect.rect.normalized();
        const QColor color = coloredRect.color.toRgb();
        const int alpha = color.alpha();
        const auto premultiply = [alpha](int component) {
            return static_cast<uchar>((component * alpha + 127) / 255);
        };
        const uchar red = premultiply(color.red());
        const uchar green = premultiply(color.green());
        const uchar blue = premultiply(color.blue());
        const uchar opacity = static_cast<uchar>(alpha);
        const float left = static_cast<float>(rect.left());
        const float top = static_cast<float>(rect.top());
        const float right = static_cast<float>(rect.right());
        const float bottom = static_cast<float>(rect.bottom());
        vertices[vertex++].set(left, top, red, green, blue, opacity);
        vertices[vertex++].set(right, top, red, green, blue, opacity);
        vertices[vertex++].set(left, bottom, red, green, blue, opacity);
        vertices[vertex++].set(left, bottom, red, green, blue, opacity);
        vertices[vertex++].set(right, top, red, green, blue, opacity);
        vertices[vertex++].set(right, bottom, red, green, blue, opacity);
    }

    auto *material = new QSGVertexColorMaterial;
    material->setFlag(QSGMaterial::Blending);
    auto *node = new QSGGeometryNode;
    node->setGeometry(geometry);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsGeometry);
    node->setFlag(QSGNode::OwnsMaterial);
    return node;
}

QSGTextNode *createTextNode(QQuickWindow *window, const QRectF &viewport,
                            const QColor &defaultColor)
{
    if (window == nullptr) {
        return nullptr;
    }
    QSGTextNode *node = window->createTextNode();
    if (node == nullptr) {
        return nullptr;
    }
    // QtRendering uses Qt Quick's distance-field glyph cache on hardware RHI
    // backends. Set every node property before adding layouts, as required by
    // QSGTextNode's public contract.
    node->setRenderType(QSGTextNode::QtRendering);
    node->setColor(defaultColor);
    node->setTextStyle(QSGTextNode::Normal);
    node->setFiltering(QSGTexture::Linear);
    node->setViewport(viewport);
    return node;
}

void appendTextLayout(QSGTextNode *node, const QString &text, const QFont &font,
                      const QColor &color, const QPointF &position,
                      qreal baseline, qreal lineWidth)
{
    if (node == nullptr || text.isEmpty()) {
        return;
    }

    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setFlags(QTextOption::IncludeTrailingSpaces);
    layout.setTextOption(option);

    QTextLayout::FormatRange range;
    range.start = 0;
    range.length = static_cast<int>(text.size());
    range.format.setForeground(color);
    layout.setFormats({range});

    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid()) {
        line.setLineWidth(std::max<qreal>(1.0, lineWidth));
        line.setPosition(QPointF(0.0, baseline - line.ascent()));
    }
    layout.endLayout();
    if (line.isValid()) {
        node->addTextLayout(position, &layout);
    }
}

Qt::KeyboardModifiers normalizedModifiers(Qt::KeyboardModifiers modifiers)
{
    return modifiers & ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
}

Qt::KeyboardModifiers modifiersAfterKeyEvent(const QKeyEvent *event,
                                              bool pressed)
{
    Qt::KeyboardModifiers modifiers = normalizedModifiers(event->modifiers());
    const auto apply = [pressed, &modifiers](Qt::KeyboardModifier modifier) {
        if (pressed) modifiers |= modifier;
        else modifiers &= ~modifier;
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
    const quint64 logical = static_cast<quint64>(
        static_cast<quint32>(event->key()));
    // Press/release logical keys can differ as modifiers change (notably
    // Backtab versus Tab). A native location is the stable identity.
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

} // namespace

TerminalPane::TerminalPane(const LaunchOptions &options, QQuickItem *parent)
    : QQuickItem(parent)
    , options_(options)
    , appearance_(options.appearance)
    , defaultFontPointSize_(options.fontSize)
    , manuallyZoomed_(options.fontSizeManuallyAdjusted)
{
    setFlag(QQuickItem::ItemHasContents, true);
    setClip(true);
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
    setFlag(QQuickItem::ItemAcceptsInputMethod, true);
    setFocusPolicy(Qt::StrongFocus);
    urlOpener_ = [](const QUrl &url) {
        return QDesktopServices::openUrl(url);
    };

    const QString family = options.fontFamily.isEmpty()
        ? QFontDatabase::systemFont(QFontDatabase::FixedFont).family()
        : options.fontFamily;
    font_.setFamily(family);
    font_.setPointSizeF(options.fontSize);
    font_.setFixedPitch(true);
    font_.setStyleHint(QFont::Monospace);
    frame_.foreground = options.appearance.foregroundColor;
    frame_.background = options.appearance.backgroundColor;
    frame_.palette = options.appearance.palette;
    frame_.cursorColor = options.appearance.cursorColor.kind
            == TerminalColorKind::Color
        && options.appearance.cursorColor.color.isValid()
        ? options.appearance.cursorColor.color
        : options.appearance.foregroundColor;
    if (options.keybindingsConfigured) {
        if (!options.keybindings.isEmpty()) {
            (void) keybinds_.load(options.keybindings);
        } else {
            (void) keybinds_.load(options.keybindConfig);
        }
    }
    updateMetrics();

    cursorTimer_ = new QTimer(this);
    cursorTimer_->setInterval(600);
    connect(cursorTimer_, &QTimer::timeout, this, [this] {
        cursorBlinkOn_ = !cursorBlinkOn_;
        update();
    });

    controller_ = new TerminalController(options, this);
    connect(controller_, &TerminalController::terminalUpdated, this,
            [this](const TerminalUpdate &terminalUpdate) {
                bool applied = false;
                {
                    QMutexLocker locker(&renderMutex_);
                    if (hasFrame_ || terminalUpdate.fullFrame) {
                        applied = applyTerminalUpdate(&frame_, terminalUpdate);
                        hasFrame_ = hasFrame_ || applied;
                        if (applied && frame_.rows > 0
                            && (!terminalResizePending_
                                || frame_.rows == terminalRows_)) {
                            terminalRows_ = frame_.rows;
                            terminalResizePending_ = false;
                        }
                    }
                }
                if (applied) {
                    if (hyperlinkQueryRejected_ && options_.linkUrl
                        && (terminalUpdate.fullFrame
                            || terminalUpdate.scrollbarChanged
                            || std::any_of(
                                terminalUpdate.dirtyRows.cbegin(),
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
                    if (terminalUpdate.resetCursorBlink && hasActiveFocus()) {
                        resetCursorBlink();
                    } else {
                        update();
                    }
                }
            });
    connect(controller_, &TerminalController::hyperlinkResolved, this,
            &TerminalPane::handleHyperlinkResult);
    connect(controller_, &TerminalController::hyperlinkActivationResolved,
            this, &TerminalPane::handleHyperlinkActivation);
    connect(controller_, &TerminalController::titleChanged,
            this, &TerminalPane::titleChanged);
    connect(controller_, &TerminalController::currentDirectoryChanged,
            this, &TerminalPane::currentDirectoryChanged);
    connect(controller_, &TerminalController::mouseTrackingChanged, this,
            [this] {
                clearHyperlinkHover();
                recomputeHyperlinkHover();
            });
    connect(controller_, &TerminalController::runningChanged,
            this, &TerminalPane::processStateChanged);
    connect(controller_, &TerminalController::activeProcessChanged,
            this, &TerminalPane::processStateChanged);
    connect(controller_, &TerminalController::errorOccurred, this,
            [this](const QString &message) {
                {
                    QMutexLocker locker(&renderMutex_);
                    statusMessage_ = message;
                }
                update();
            });
    connect(controller_, &TerminalController::sessionExited, this,
            [this](int exitCode, int signalNumber, bool hold) {
                clearHyperlinkHover();
                cancelHyperlinkPress();
                cancelPendingHyperlinkActivation();
                bool hasError = false;
                {
                    QMutexLocker locker(&renderMutex_);
                    hasError = !statusMessage_.isEmpty();
                    if (statusMessage_.isEmpty()) {
                        statusMessage_ = signalNumber > 0
                            ? QStringLiteral("Process ended after signal %1").arg(signalNumber)
                            : QStringLiteral("Process exited with status %1").arg(exitCode);
                    }
                }
                update();
                Q_EMIT sessionEnded(this, exitCode, signalNumber);
                if (!hold && !hasError) {
                    QTimer::singleShot(0, this, [this] { Q_EMIT requestClose(); });
                }
            });
}

TerminalPane::~TerminalPane() = default;

QString TerminalPane::title() const
{
    const QString controllerTitle = controller_->title();
    if (!controllerTitle.isEmpty()) {
        return controllerTitle;
    }
    if (!options_.program.isEmpty()) {
        return QFileInfo(options_.program.constFirst()).fileName();
    }
    return QStringLiteral("Terminal");
}

QString TerminalPane::currentDirectory() const
{
    return controller_->currentDirectory();
}

qreal TerminalPane::fontPointSize() const
{
    QMutexLocker locker(&renderMutex_);
    return font_.pointSizeF();
}

QStringList TerminalPane::activeKeyTables() const
{
    return keybinds_.activeTableNames();
}

bool TerminalPane::isRunning() const
{
    return controller_->running();
}

bool TerminalPane::hasActiveProcess() const
{
    return controller_->activeProcess();
}

LaunchOptions TerminalPane::splitLaunchOptions() const
{
    LaunchOptions result = options_;
    const QString directory = currentDirectory();
    if (!directory.isEmpty() && QFileInfo(directory).isDir()) {
        result.workingDirectory = directory;
    }
    {
        QMutexLocker locker(&renderMutex_);
        result.fontFamily = font_.family();
        result.fontSize = font_.pointSizeF();
    }
    result.fontSizeManuallyAdjusted = manuallyZoomed_;
    result.program.clear();
    result.hold = false;
    return result;
}

void TerminalPane::applyRuntimeOptions(const LaunchOptions &options)
{
    LaunchOptions updated = options_;
    const bool linkUrlChanged = updated.linkUrl != options.linkUrl;
    updated.fontFamily = options.fontFamily;
    updated.fontSize = options.fontSize;
    updated.fontFamilyExplicit = options.fontFamilyExplicit;
    updated.fontSizeExplicit = options.fontSizeExplicit;
    updated.appearance = options.appearance;
    updated.scrollbackLimit = options.scrollbackLimit;
    updated.scrollbackLimitExplicit = options.scrollbackLimitExplicit;
    updated.confirmCloseMode = options.confirmCloseMode;
    updated.linkUrl = options.linkUrl;
    updated.keybindConfig = options.keybindConfig;
    updated.keybindings = options.keybindings;
    updated.keybindingsConfigured = options.keybindingsConfigured;

    const QString family = updated.fontFamily.isEmpty()
        ? QFontDatabase::systemFont(QFontDatabase::FixedFont).family()
        : updated.fontFamily;
    bool metricsChanged = false;
    bool pointSizeChanged = false;
    {
        QMutexLocker locker(&renderMutex_);
        if (font_.family() != family) {
            font_.setFamily(family);
            metricsChanged = true;
        }
        defaultFontPointSize_ = updated.fontSize;
        if (!manuallyZoomed_
            && !qFuzzyCompare(font_.pointSizeF(), updated.fontSize)) {
            font_.setPointSizeF(updated.fontSize);
            metricsChanged = true;
            pointSizeChanged = true;
        }
        if (!hasFrame_) {
            frame_.foreground = updated.appearance.foregroundColor;
            frame_.background = updated.appearance.backgroundColor;
            frame_.palette = updated.appearance.palette;
            frame_.cursorColor = updated.appearance.cursorColor.kind
                    == TerminalColorKind::Color
                && updated.appearance.cursorColor.color.isValid()
                ? updated.appearance.cursorColor.color
                : updated.appearance.foregroundColor;
        }
        appearance_ = updated.appearance;
    }
    options_ = updated;
    options_.fontSizeManuallyAdjusted = manuallyZoomed_;
    if (activeSequenceToken_ != 0) {
        controller_->resolveSequence(activeSequenceToken_,
                                     TerminalSequenceResolution::Drop);
        activeSequenceToken_ = 0;
    }
    const QStringList previousKeyTables = keybinds_.activeTableNames();
    if (options_.keybindingsConfigured) {
        GhosttyKeybindSet candidate;
        if (!options_.keybindings.isEmpty()) {
            (void) candidate.load(options_.keybindings);
        } else {
            (void) candidate.load(options_.keybindConfig);
        }
        keybinds_ = std::move(candidate);
    } else {
        keybinds_.clear();
    }
    if (previousKeyTables != keybinds_.activeTableNames()) {
        Q_EMIT activeKeyTablesChanged();
    }
    if (linkUrlChanged && !options.linkUrl) {
        if (hyperlinkPressKind_ == TerminalLinkKind::Regex) {
            cancelHyperlinkPress();
        }
        if (pendingActivationKind_ == TerminalLinkKind::Regex) {
            cancelPendingHyperlinkActivation();
        }
    }
    if (linkUrlChanged) {
        clearHyperlinkHover();
    }
    controller_->applyRuntimeOptions(options_);
    if (linkUrlChanged) {
        recomputeHyperlinkHover();
    }

    if (metricsChanged) {
        updateMetrics();
        updateTerminalSize();
    }
    update();
    if (pointSizeChanged) {
        Q_EMIT fontPointSizeChanged();
    }
}

void TerminalPane::setWorkspaceActionHandler(
    std::function<bool(WorkspaceActionRequest)> handler)
{
    workspaceActionHandler_ = std::move(handler);
}

void TerminalPane::setUrlOpener(std::function<bool(const QUrl &)> opener)
{
    urlOpener_ = std::move(opener);
}

void TerminalPane::beginShutdown()
{
    controller_->beginShutdown();
}

QSGNode *TerminalPane::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    // Keep the root that Qt already parented into the item scene graph. Returning
    // a different root on every content update leaves the previous root in the
    // child container, so stale frames remain visible and nodes accumulate.
    QSGNode *root = oldNode != nullptr ? oldNode : new QSGNode;
    while (QSGNode *child = root->firstChild()) {
        root->removeChildNode(child);
        delete child;
    }

    TerminalFrame frame;
    TerminalAppearance appearance;
    QFont baseFont;
    QString preedit;
    QString status;
    QSet<int> hoveredHyperlinkCells;
    qreal cellWidth = 0.0;
    qreal cellHeight = 0.0;
    qreal baseline = 0.0;
    bool hasFrame = false;
    {
        QMutexLocker locker(&renderMutex_);
        frame = frame_;
        appearance = appearance_;
        baseFont = font_;
        preedit = preedit_;
        status = statusMessage_;
        if (hoveredHyperlinkColumns_ == frame_.columns
            && hoveredHyperlinkRows_ == frame_.rows) {
            hoveredHyperlinkCells = hoveredHyperlinkCellIndexes_;
        }
        cellWidth = cellWidth_;
        cellHeight = cellHeight_;
        baseline = baseline_;
        hasFrame = hasFrame_;
    }

    const QRectF viewport = boundingRect();
    const QSGRendererInterface *rendererInterface = window() != nullptr
        ? window()->rendererInterface() : nullptr;
    const bool softwareRenderer = rendererInterface == nullptr
        || rendererInterface->graphicsApi() == QSGRendererInterface::Software;
    const QColor background = hasFrame ? frame.background : QColor(QStringLiteral("#1e222a"));
    QVector<ColoredRect> baseBackgrounds;
    QVector<ColoredRect> backgrounds;
    QVector<ColoredRect> cursorBackgrounds;
    QVector<ColoredRect> decorationsBeforeText;
    QVector<ColoredRect> decorationsAfterText;
    QVector<ColoredRect> cursorDecorations;
    QVector<ColoredRect> scrollbarDecorations;
    QVector<ColoredRect> overlayBackgrounds;
    QVector<ColoredRect> overlayDecorations;
    // Keep base, cell, and cursor fills in explicit scene-graph layers so
    // their painter order is identical across Qt render backends.
    appendRect(baseBackgrounds, viewport, background);

    QSGTextNode *mainText = createTextNode(window(), viewport,
                                           hasFrame ? frame.foreground
                                                    : QColor(QStringLiteral("#88909d")));
    QSGTextNode *overlayText = createTextNode(window(), viewport,
                                              QColor(QStringLiteral("#eceff4")));
    bool hasMainText = false;
    bool hasOverlayText = false;

    if (!hasFrame || frame.columns <= 0 || frame.rows <= 0) {
        appendTextLayout(mainText, QStringLiteral("Starting terminal…"), baseFont,
                         QColor(QStringLiteral("#88909d")), QPointF(12.0, 12.0),
                         baseline, std::max<qreal>(1.0, viewport.width() - 24.0));
        hasMainText = mainText != nullptr;
    } else {
        QFont normal = baseFont;
        QFont bold = baseFont;
        bold.setBold(true);
        QFont italic = baseFont;
        italic.setItalic(true);
        QFont boldItalic = bold;
        boldItalic.setItalic(true);

        const int visibleRows = std::min(frame.rows,
            static_cast<int>(std::ceil(height() / cellHeight)));
        const int visibleColumns = std::min(frame.columns,
            static_cast<int>(std::ceil(width() / cellWidth)));
        const bool focused = hasActiveFocus();
        const bool cursorActive = frame.cursorVisible
            && (!focused || !frame.cursorBlinking || cursorBlinkOn_)
            && frame.cursorColumn >= 0 && frame.cursorColumn < visibleColumns
            && frame.cursorRow >= 0 && frame.cursorRow < visibleRows;
        const int cursorStyle = focused ? frame.cursorStyle : 3;
        const bool blockCursorActive = cursorActive
            && cursorStyle == 1;
        QColor cursorCellForeground = frame.foreground;
        QColor cursorCellBackground = frame.background;
        const int cursorCellIndex = frame.cursorRow * frame.columns
            + frame.cursorColumn;
        if (cursorActive && cursorCellIndex >= 0
            && cursorCellIndex < frame.cells.size()) {
            const TerminalCell &cursorCell = frame.cells.at(cursorCellIndex);
            cursorCellForeground = cursorCell.foreground;
            cursorCellBackground = cursorCell.background;
            applyBoldColor(cursorCell, frame, appearance,
                           &cursorCellForeground, &cursorCellBackground);
        }
        QColor blockCursorTextColor = resolveRelativeColor(
            appearance.cursorTextColor,
            cursorCellForeground, cursorCellBackground,
            frame.background);
        blockCursorTextColor.setAlpha(255);

        for (int row = 0; row < visibleRows; ++row) {
            const qreal top = static_cast<qreal>(row) * cellHeight;
            for (int column = 0; column < visibleColumns; ++column) {
                const int index = row * frame.columns + column;
                if (index < 0 || index >= frame.cells.size()) {
                    continue;
                }
                const TerminalCell &cell = frame.cells.at(index);
                const qreal left = static_cast<qreal>(column) * cellWidth;
                const qreal drawWidth = cellWidth
                    * static_cast<qreal>(std::max(1, cell.columnSpan));
                QColor styledForeground = cell.foreground;
                QColor styledBackground = cell.background;
                applyBoldColor(cell, frame, appearance,
                               &styledForeground, &styledBackground);

                const QColor cellBackground = cell.selected
                    ? resolveRelativeColor(appearance.selectionBackground,
                                           styledForeground, styledBackground,
                                           frame.foreground)
                    : styledBackground;
                if (cellBackground != background) {
                    // Background is grid-cell state, even when the glyph in
                    // this cell spans multiple columns. Drawing one column at
                    // a time keeps adjacent/spacer backgrounds non-overlapping
                    // so they can be safely color-batched.
                    appendRect(backgrounds, QRectF(left, top, cellWidth, cellHeight),
                               cellBackground);
                }

                QColor foreground = cell.selected
                    ? resolveRelativeColor(appearance.selectionForeground,
                                           styledForeground, styledBackground,
                                           frame.background)
                    : styledForeground;
                const bool insideBlockCursor = blockCursorActive
                    && row == frame.cursorRow
                    && column >= frame.cursorColumn
                    && column < frame.cursorColumn
                        + std::max(1, frame.cursorColumnSpan);
                if (cell.faint && !insideBlockCursor) {
                    foreground = withOpacity(foreground, appearance.faintOpacity);
                }
                // Ghostty applies the block-cursor text uniform last to all
                // foreground primitives in every column covered by a wide
                // cursor. It is intentionally opaque and overrides faint and
                // explicit decoration colors.
                if (insideBlockCursor) {
                    foreground = blockCursorTextColor;
                }

                if (!cell.invisible && !cell.text.isEmpty() && !cell.spacer) {
                    const QFont *drawFont = &normal;
                    if (cell.bold && cell.italic) drawFont = &boldItalic;
                    else if (cell.bold) drawFont = &bold;
                    else if (cell.italic) drawFont = &italic;
                    appendTextLayout(mainText, cell.text, *drawFont, foreground,
                                     QPointF(left, top), baseline, drawWidth);
                    hasMainText = mainText != nullptr;
                }

                if (cell.invisible) {
                    continue;
                }

                QColor underlineColor = insideBlockCursor
                    ? blockCursorTextColor
                    : (cell.underlineUsesForeground
                        ? foreground : cell.underlineColor);
                if (!insideBlockCursor && !cell.underlineUsesForeground
                    && cell.faint) {
                    underlineColor = withOpacity(underlineColor,
                                                  appearance.faintOpacity);
                }
                TerminalUnderlineStyle underlineStyle = cell.underlineStyle;
                if (hoveredHyperlinkCells.contains(index)) {
                    // Ghostty renders a hovered link as single-underlined,
                    // except an existing single underline becomes double so
                    // the hover remains visually distinguishable.
                    underlineStyle = cell.underlineStyle
                            == TerminalUnderlineStyle::Single
                        ? TerminalUnderlineStyle::Double
                        : TerminalUnderlineStyle::Single;
                }
                appendUnderline(decorationsBeforeText,
                                QRectF(left, top, drawWidth, cellHeight),
                                baseline, underlineStyle, underlineColor);
                if (cell.strikeThrough) {
                    appendRect(decorationsAfterText,
                               QRectF(left, top + cellHeight * 0.52, drawWidth, 1.0),
                               foreground);
                }
                if (cell.overline) {
                    appendRect(decorationsBeforeText,
                               QRectF(left, top + 1.0, drawWidth, 1.0),
                               foreground);
                }
            }
        }

        if (cursorActive) {
            const qreal left = static_cast<qreal>(frame.cursorColumn) * cellWidth;
            const qreal top = static_cast<qreal>(frame.cursorRow) * cellHeight;
            const qreal cursorWidth = cellWidth
                * static_cast<qreal>(std::max(1, frame.cursorColumnSpan));
            const QRectF cursorRect(left, top, cursorWidth, cellHeight);
            QColor cursorColor = frame.cursorColor;
            if (!frame.cursorColorExplicit) {
                cursorColor = resolveRelativeColor(
                    appearance.cursorColor,
                    cursorCellForeground, cursorCellBackground,
                    frame.foreground);
            }
            cursorColor = withOpacity(
                cursorColor, focused ? appearance.cursorOpacity : 1.0);
            switch (cursorStyle) {
            case 0:
                appendRect(cursorDecorations,
                           QRectF(left, top, std::max(1.0, cellWidth * 0.14), cellHeight),
                           cursorColor);
                break;
            case 2:
                appendRect(cursorDecorations,
                           QRectF(left, top + cellHeight - 2.0, cursorWidth, 2.0),
                           cursorColor);
                break;
            case 3: {
                constexpr qreal thickness = 1.0;
                appendRect(cursorDecorations,
                           QRectF(left, top, cursorWidth, thickness), cursorColor);
                appendRect(cursorDecorations,
                           QRectF(left, top + cellHeight - thickness,
                                  cursorWidth, thickness), cursorColor);
                appendRect(cursorDecorations,
                           QRectF(left, top + thickness, thickness,
                                  std::max(0.0, cellHeight - 2.0 * thickness)),
                           cursorColor);
                appendRect(cursorDecorations,
                           QRectF(left + cursorWidth - thickness, top + thickness,
                                  thickness,
                                  std::max(0.0, cellHeight - 2.0 * thickness)),
                           cursorColor);
                break;
            }
            default:
                // Appended after cell backgrounds so a wide block cursor is
                // not overwritten by the spacer cell's background.
                appendRect(cursorBackgrounds, cursorRect, cursorColor);
                break;
            }
        }

        if (frame.scrollTotal > frame.scrollLength && frame.scrollTotal > 0) {
            constexpr qreal barWidth = 3.0;
            const qreal trackHeight = height();
            const qreal barHeight = std::max(18.0,
                trackHeight * static_cast<qreal>(frame.scrollLength)
                    / static_cast<qreal>(frame.scrollTotal));
            const quint64 movable = frame.scrollTotal - frame.scrollLength;
            const qreal barTop = movable == 0 ? 0.0
                : (trackHeight - barHeight) * static_cast<qreal>(frame.scrollOffset)
                    / static_cast<qreal>(movable);
            appendRect(scrollbarDecorations,
                       QRectF(width() - barWidth, barTop, barWidth, barHeight),
                       QColor(216, 222, 233, 100));
        }

        if (!preedit.isEmpty()) {
            const qreal left = static_cast<qreal>(frame.cursorColumn) * cellWidth;
            const qreal top = static_cast<qreal>(frame.cursorRow) * cellHeight;
            const qreal preeditWidth = std::max(
                cellWidth, QFontMetricsF(baseFont).horizontalAdvance(preedit));
            appendRect(overlayBackgrounds,
                       QRectF(left, top, preeditWidth, cellHeight),
                       QColor(QStringLiteral("#3b4252")));
            appendTextLayout(overlayText, preedit, baseFont,
                             QColor(QStringLiteral("#eceff4")), QPointF(left, top),
                             baseline, preeditWidth);
            hasOverlayText = overlayText != nullptr;
            appendRect(overlayDecorations,
                       QRectF(left, top + baseline + 1.0, preeditWidth, 1.0),
                       QColor(QStringLiteral("#eceff4")));
        }

        if (!status.isEmpty()) {
            const QFontMetricsF metrics(baseFont);
            const qreal statusHeight = metrics.height() + 12.0;
            const qreal statusTop = height() - statusHeight;
            appendRect(overlayBackgrounds,
                       QRectF(0.0, statusTop, width(), statusHeight),
                       QColor(46, 52, 64, 230));
            appendTextLayout(overlayText, status, baseFont,
                             QColor(QStringLiteral("#e5c07b")),
                             QPointF(8.0, statusTop),
                             statusHeight - 6.0 - metrics.descent(),
                             std::max<qreal>(1.0, width() - 16.0));
            hasOverlayText = overlayText != nullptr;
        }
    }

    if (QSGNode *node = createRectNode(baseBackgrounds, softwareRenderer)) {
        root->appendChildNode(node);
    }
    if (QSGNode *node = createRectNode(backgrounds, softwareRenderer)) {
        root->appendChildNode(node);
    }
    if (QSGNode *node = createRectNode(cursorBackgrounds, softwareRenderer)) {
        root->appendChildNode(node);
    }
    if (QSGNode *node = createRectNode(decorationsBeforeText, softwareRenderer)) {
        root->appendChildNode(node);
    }
    if (hasMainText) {
        root->appendChildNode(mainText);
    } else {
        delete mainText;
    }
    if (QSGNode *node = createRectNode(decorationsAfterText, softwareRenderer)) {
        root->appendChildNode(node);
    }
    if (QSGNode *node = createRectNode(cursorDecorations, softwareRenderer)) {
        root->appendChildNode(node);
    }
    if (QSGNode *node = createRectNode(scrollbarDecorations, softwareRenderer)) {
        root->appendChildNode(node);
    }
    if (QSGNode *node = createRectNode(overlayBackgrounds, softwareRenderer)) {
        root->appendChildNode(node);
    }
    if (hasOverlayText) {
        root->appendChildNode(overlayText);
    } else {
        delete overlayText;
    }
    if (QSGNode *node = createRectNode(overlayDecorations, softwareRenderer)) {
        root->appendChildNode(node);
    }
    return root;
}

void TerminalPane::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        updateTerminalSize();
        update();
    }
}

void TerminalPane::updateMetrics()
{
    QMutexLocker locker(&renderMutex_);
    const QFontMetricsF metrics(font_);
    cellWidth_ = std::max(1.0, std::ceil(metrics.horizontalAdvance(QLatin1Char('M'))));
    cellHeight_ = std::max(1.0, std::ceil(metrics.height()));
    baseline_ = std::ceil(metrics.ascent() + (cellHeight_ - metrics.height()) / 2.0);
}

void TerminalPane::resetCursorBlink()
{
    cursorBlinkOn_ = true;
    cursorTimer_->start();
    update();
}

void TerminalPane::updateTerminalSize()
{
    qreal cellWidth = 0.0;
    qreal cellHeight = 0.0;
    {
        QMutexLocker locker(&renderMutex_);
        cellWidth = cellWidth_;
        cellHeight = cellHeight_;
    }
    if (width() < cellWidth || height() < cellHeight) {
        if (controller_ != nullptr) {
            clearHyperlinkHover();
            cancelHyperlinkPress();
        }
        return;
    }
    const qreal devicePixelRatio = window() != nullptr ? window()->devicePixelRatio() : 1.0;
    const int columns = std::max(1, static_cast<int>(std::floor(width() / cellWidth)));
    const int rows = std::clamp(
        static_cast<int>(std::floor(height() / cellHeight)),
        1, static_cast<int>(std::numeric_limits<quint16>::max()));
    {
        QMutexLocker locker(&renderMutex_);
        terminalRows_ = rows;
        terminalResizePending_ = true;
    }
    controller_->resizeTerminal(
        columns, rows,
        std::max(1, qRound(cellWidth * devicePixelRatio)),
        std::max(1, qRound(cellHeight * devicePixelRatio)),
        std::max(1, qRound(width() * devicePixelRatio)),
        std::max(1, qRound(height() * devicePixelRatio)));
}

void TerminalPane::keyPressEvent(QKeyEvent *event)
{
    const KeyHandling handling = handleShortcut(event);
    // Run configured actions against the previously accepted hover before a
    // chord's non-modifier key refreshes link eligibility. This keeps
    // copy_url_to_clipboard synchronous within Ghostty action chains.
    updateHyperlinkModifiers(modifiersAfterKeyEvent(event, true));
    if (handling != KeyHandling::PassThrough) {
        if (handling == KeyHandling::ConsumePressAndRelease) {
            consumedKeys_.insert(keyEventIdentity(event));
        }
        event->accept();
        return;
    }

    controller_->sendKey(terminalKeyInput(event));
    event->accept();
}

void TerminalPane::keyReleaseEvent(QKeyEvent *event)
{
    updateHyperlinkModifiers(modifiersAfterKeyEvent(event, false));
    if (consumedKeys_.remove(keyEventIdentity(event))) {
        event->accept();
        return;
    }
    controller_->sendKey(terminalKeyInput(event, false));
    event->accept();
}

TerminalPane::KeyHandling TerminalPane::handleShortcut(QKeyEvent *event)
{
    if (options_.keybindingsConfigured) {
        return handleConfiguredShortcut(event);
    }

    const Qt::KeyboardModifiers modifiers = normalizedModifiers(event->modifiers());
    const bool control = modifiers.testFlag(Qt::ControlModifier);
    const bool shift = modifiers.testFlag(Qt::ShiftModifier);
    const int key = event->key();

    if (control && shift && modifiers == (Qt::ControlModifier | Qt::ShiftModifier)) {
        switch (key) {
        case Qt::Key_C: copySelection(); return KeyHandling::ConsumePressAndRelease;
        case Qt::Key_V: {
            const QString text = QGuiApplication::clipboard()->text();
            if (!text.isEmpty()) {
                if (TerminalController::isPasteSafe(text)) pasteText(text);
                else Q_EMIT unsafePasteRequested(text, this);
            }
            return KeyHandling::ConsumePressAndRelease;
        }
        case Qt::Key_T: Q_EMIT requestNewTab(); return KeyHandling::ConsumePressAndRelease;
        case Qt::Key_O: Q_EMIT requestSplit(static_cast<int>(Qt::Horizontal)); return KeyHandling::ConsumePressAndRelease;
        case Qt::Key_E: Q_EMIT requestSplit(static_cast<int>(Qt::Vertical)); return KeyHandling::ConsumePressAndRelease;
        case Qt::Key_W: Q_EMIT requestCloseTab(); return KeyHandling::ConsumePressAndRelease;
        case Qt::Key_Q: Q_EMIT requestQuit(); return KeyHandling::ConsumePressAndRelease;
        default: break;
        }
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
        && (key == Qt::Key_Left || key == Qt::Key_Right
            || key == Qt::Key_Up || key == Qt::Key_Down)) {
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

TerminalPane::KeyHandling TerminalPane::handleConfiguredShortcut(
    QKeyEvent *event)
{
    const GhosttyKeybindEvent bindingEvent{
        .qtKey = event->key(),
        .modifiers = event->modifiers(),
        .text = event->text(),
        .nativeScanCode = event->nativeScanCode(),
        .unshiftedCodepoint = unshiftedCodepoint(event->key()),
    };
    const TerminalKeyInput currentInput = terminalKeyInput(event);
    const QStringList previousKeyTables = keybinds_.activeTableNames();
    const GhosttyKeybindStep step = keybinds_.advance(bindingEvent);
    if (previousKeyTables != keybinds_.activeTableNames()) {
        Q_EMIT activeKeyTablesChanged();
    }

    switch (step.kind) {
    case GhosttyKeybindStepKind::Unmatched:
        return KeyHandling::PassThrough;
    case GhosttyKeybindStepKind::Leader:
        if (activeSequenceToken_ == 0) {
            activeSequenceToken_ = controller_->stageSequenceKey(currentInput);
        } else {
            controller_->stageSequenceKey(activeSequenceToken_, currentInput);
        }
        return KeyHandling::ConsumePress;
    case GhosttyKeybindStepKind::InvalidSequence:
        if (activeSequenceToken_ != 0) {
            controller_->resolveSequence(
                activeSequenceToken_,
                TerminalSequenceResolution::FlushAndSendCurrent,
                currentInput);
            activeSequenceToken_ = 0;
            return KeyHandling::ConsumePress;
        }
        return KeyHandling::PassThrough;
    case GhosttyKeybindStepKind::IgnoredSequence:
        if (activeSequenceToken_ != 0) {
            controller_->resolveSequence(activeSequenceToken_,
                                         TerminalSequenceResolution::Drop);
            activeSequenceToken_ = 0;
        }
        return KeyHandling::ConsumePress;
    case GhosttyKeybindStepKind::Binding:
        break;
    }

    // `global:` implies `all:` at runtime even though Ghostty retains the raw
    // flags independently. Broad bindings ignore unconsumed/performable and
    // always suppress terminal encoding. `ignore` retains Ghostty's special
    // press-only handling, so a release is not marked as a consumed binding.
    if (step.match.all || step.match.global) {
        Q_EMIT broadActionsRequested(step.match.actions);
        if (activeSequenceToken_ != 0) {
            controller_->resolveSequence(activeSequenceToken_,
                                         TerminalSequenceResolution::Drop);
            activeSequenceToken_ = 0;
        }
        const bool ignored = std::any_of(
            step.match.actions.cbegin(), step.match.actions.cend(),
            [](const QString &action) {
                const qsizetype colon = action.indexOf(u':');
                return (colon < 0 ? QStringView(action)
                                  : QStringView(action).first(colon))
                    == QLatin1StringView("ignore");
            });
        return ignored ? KeyHandling::ConsumePress
                       : KeyHandling::ConsumePressAndRelease;
    }

    bool performed = false;
    bool ignored = false;
    bool hasClosingAction = false;
    for (const QString &action : step.match.actions) {
        const qsizetype colon = action.indexOf(u':');
        const QStringView name = colon < 0
            ? QStringView(action)
            : QStringView(action).first(colon);
        hasClosingAction = hasClosingAction
            || name == QLatin1StringView("close_surface")
            || name == QLatin1StringView("close_tab")
            || name == QLatin1StringView("close_window");
        if (canExecuteConfiguredAction(action)) {
            const bool actionPerformed = executeConfiguredAction(action);
            performed = actionPerformed || performed;
            if (actionPerformed && name == QLatin1StringView("ignore")) {
                ignored = true;
            }
        }
    }
    // Ghostty executes the complete chain, then treats surface/tab/window
    // closure as terminal for this event. TerminalWorkspace uses deleteLater,
    // so chained actions remain safe until this callback returns. `quit` is
    // deliberately not a closing surface action in the pinned implementation.
    if (performed && hasClosingAction) {
        if (activeSequenceToken_ != 0) {
            controller_->resolveSequence(activeSequenceToken_,
                                         TerminalSequenceResolution::Drop);
            activeSequenceToken_ = 0;
        }
        return KeyHandling::ConsumePressAndRelease;
    }
    // Ghostty's ignore action suppresses encoding even on an unconsumed
    // binding. A performable binding with no effective action acts as though
    // it did not exist; every other match follows the binding's consume flag,
    // independently of whether an action happened to change state.
    if (ignored) {
        if (activeSequenceToken_ != 0) {
            controller_->resolveSequence(activeSequenceToken_,
                                         TerminalSequenceResolution::Drop);
            activeSequenceToken_ = 0;
        }
        return KeyHandling::ConsumePress;
    }
    if (step.match.performable && !performed) {
        if (activeSequenceToken_ != 0) {
            controller_->resolveSequence(
                activeSequenceToken_,
                TerminalSequenceResolution::FlushAndSendCurrent,
                currentInput);
            activeSequenceToken_ = 0;
            return KeyHandling::ConsumePress;
        }
        return KeyHandling::PassThrough;
    }
    if (step.match.consumed) {
        if (activeSequenceToken_ != 0) {
            controller_->resolveSequence(activeSequenceToken_,
                                         TerminalSequenceResolution::Drop);
            activeSequenceToken_ = 0;
        }
        return KeyHandling::ConsumePressAndRelease;
    }
    if (activeSequenceToken_ != 0) {
        controller_->resolveSequence(
            activeSequenceToken_,
            TerminalSequenceResolution::FlushAndSendCurrent,
            currentInput);
        activeSequenceToken_ = 0;
        return KeyHandling::ConsumePress;
    }
    return KeyHandling::PassThrough;
}

bool TerminalPane::canExecuteConfiguredAction(QStringView action) const
{
    const qsizetype colon = action.indexOf(u':');
    const QStringView name = colon < 0 ? action : action.first(colon);
    const std::optional<QStringView> parameter = colon < 0
        ? std::nullopt
        : std::optional<QStringView>(action.sliced(colon + 1));

    if (const std::optional<GhosttyPaneAction> paneAction =
            GhosttyActionCatalog::parsePaneAction(action);
        paneAction.has_value()) {
        const bool needsSelection =
            paneAction->kind == GhosttyPaneActionKind::AdjustSelection
            || (paneAction->kind == GhosttyPaneActionKind::ScrollViewport
                && paneAction->viewport.kind
                    == TerminalViewportRequest::Kind::Selection);
        return !needsSelection || controller_->selectionExpected();
    }

    if (name == QLatin1StringView("copy_to_clipboard")) {
        const bool validParameter = !parameter.has_value()
            || *parameter == QLatin1StringView("plain")
            || *parameter == QLatin1StringView("mixed");
        if (!validParameter) {
            return false;
        }
        return controller_->selectionExpected();
    }
    if (name == QLatin1StringView("copy_url_to_clipboard")) {
        bool hoveredCellIsLinked = false;
        {
            QMutexLocker locker(&renderMutex_);
            const int index = hoverCell_.y() * frame_.columns
                + hoverCell_.x();
            hoveredCellIsLinked = hoverCell_.x() >= 0
                && hoverCell_.x() < frame_.columns
                && hoverCell_.y() >= 0 && hoverCell_.y() < frame_.rows
                && hoveredHyperlinkColumns_ == frame_.columns
                && hoveredHyperlinkRows_ == frame_.rows
                && hoveredHyperlinkCellIndexes_.contains(index);
        }
        return !parameter.has_value() && !hoveredHyperlinkUri_.isEmpty()
            && hoveredHyperlinkCell_ == hoverCell_
            && hoveredCellIsLinked
            && hyperlinkModifiersMatch(hoverModifiers_);
    }
    if (name == QLatin1StringView("activate_key_table")
        || name == QLatin1StringView("activate_key_table_once")) {
        return parameter.has_value()
            && keybinds_.canActivateTable(*parameter);
    }
    if (name == QLatin1StringView("deactivate_key_table")) {
        return !parameter.has_value() && !keybinds_.activeTableNames().isEmpty();
    }
    if (name == QLatin1StringView("deactivate_all_key_tables")) {
        return !parameter.has_value() && !keybinds_.activeTableNames().isEmpty();
    }
    if (name == QLatin1StringView("paste_from_clipboard")) {
        return !parameter.has_value()
            && !QGuiApplication::clipboard()->text().isEmpty();
    }
    if (name == QLatin1StringView("paste_from_selection")) {
        return !parameter.has_value()
            && !QGuiApplication::clipboard()->text(QClipboard::Selection).isEmpty();
    }
    if (name == QLatin1StringView("reset_font_size")
        || name == QLatin1StringView("reload_config")
        || name == QLatin1StringView("close_window")
        || name == QLatin1StringView("end_key_sequence")
        || name == QLatin1StringView("ignore")) {
        return !parameter.has_value();
    }
    if (name == QLatin1StringView("increase_font_size")
        || name == QLatin1StringView("decrease_font_size")) {
        if (!parameter.has_value()) {
            return true;
        }
        bool valid = false;
        const qreal amount = parameter->toString().toDouble(&valid);
        return valid && std::isfinite(amount) && amount > 0.0;
    }

    return GhosttyActionCatalog::translate(action).accepted();
}

int TerminalPane::viewportPageRows() const
{
    QMutexLocker locker(&renderMutex_);
    return std::max(1, terminalRows_);
}

bool TerminalPane::executeConfiguredAction(QStringView action)
{
    const qsizetype colon = action.indexOf(u':');
    const QStringView name = colon < 0 ? action : action.first(colon);
    const QStringView parameter = colon < 0 ? QStringView{} : action.sliced(colon + 1);

    if (name == QLatin1StringView("activate_key_table")
        || name == QLatin1StringView("activate_key_table_once")) {
        if (colon < 0) return false;
        const bool changed = keybinds_.activateTable(
            parameter,
            name == QLatin1StringView("activate_key_table_once"));
        if (changed) Q_EMIT activeKeyTablesChanged();
        return changed;
    }
    if (name == QLatin1StringView("deactivate_key_table")) {
        if (colon >= 0) return false;
        const bool changed = keybinds_.deactivateTable();
        if (changed) Q_EMIT activeKeyTablesChanged();
        return changed;
    }
    if (name == QLatin1StringView("deactivate_all_key_tables")) {
        if (colon >= 0) return false;
        const bool changed = keybinds_.deactivateAllTables();
        if (changed) Q_EMIT activeKeyTablesChanged();
        return changed;
    }

    if (const std::optional<GhosttyPaneAction> paneAction =
            GhosttyActionCatalog::parsePaneAction(action);
        paneAction.has_value()) {
        switch (paneAction->kind) {
        case GhosttyPaneActionKind::ScrollViewport:
            if (paneAction->viewport.kind
                    == TerminalViewportRequest::Kind::Selection
                && !controller_->selectionExpected()) {
                return false;
            }
            controller_->scrollViewport(paneAction->viewport);
            return true;
        case GhosttyPaneActionKind::ScrollPageUp:
        case GhosttyPaneActionKind::ScrollPageDown: {
            TerminalViewportRequest request;
            request.kind = TerminalViewportRequest::Kind::Delta;
            const qint64 rows = viewportPageRows();
            request.delta = paneAction->kind
                    == GhosttyPaneActionKind::ScrollPageUp
                ? -rows : rows;
            controller_->scrollViewport(request);
            return true;
        }
        case GhosttyPaneActionKind::ScrollPageFractional: {
            const std::optional<qint64> delta = fractionalPageDelta(
                paneAction->pageFraction, viewportPageRows());
            if (!delta.has_value()) return false;
            TerminalViewportRequest request;
            request.kind = TerminalViewportRequest::Kind::Delta;
            request.delta = *delta;
            controller_->scrollViewport(request);
            return true;
        }
        case GhosttyPaneActionKind::SelectAll:
            controller_->selectAll();
            return true;
        case GhosttyPaneActionKind::AdjustSelection:
            if (!controller_->selectionExpected()) return false;
            controller_->adjustSelection(paneAction->selectionAdjustment);
            return true;
        case GhosttyPaneActionKind::Csi:
            controller_->sendCsi(paneAction->payload.toUtf8());
            return true;
        case GhosttyPaneActionKind::Esc:
            controller_->sendEscape(paneAction->payload.toUtf8());
            return true;
        case GhosttyPaneActionKind::Text:
            // Ghostty validates Zig string escapes only when performing the
            // action. Keep the serialized bytes intact until the worker so a
            // malformed binding is still consumed without writing to the PTY.
            controller_->sendRawText(paneAction->payload.toUtf8());
            return true;
        case GhosttyPaneActionKind::Reset:
            controller_->resetTerminal();
            return true;
        }
    }

    if (name == QLatin1StringView("copy_to_clipboard")) {
        copySelection();
        return true;
    }
    if (name == QLatin1StringView("copy_url_to_clipboard")) {
        // Broad (`all:`/`global:`) bindings execute without the normal
        // performability preflight, so validate again at this public boundary.
        if (!canExecuteConfiguredAction(action)
            || QGuiApplication::clipboard() == nullptr) {
            return false;
        }
        auto *mimeData = new QMimeData;
        mimeData->setData(QStringLiteral("text/plain"),
                          hoveredHyperlinkUri_);
        QGuiApplication::clipboard()->setMimeData(mimeData);
        return true;
    }
    if (name == QLatin1StringView("paste_from_clipboard")) {
        const QString text = QGuiApplication::clipboard()->text();
        if (text.isEmpty()) return false;
        if (TerminalController::isPasteSafe(text)) pasteText(text);
        else Q_EMIT unsafePasteRequested(text, this);
        return true;
    }
    if (name == QLatin1StringView("paste_from_selection")) {
        const QString text =
            QGuiApplication::clipboard()->text(QClipboard::Selection);
        if (text.isEmpty()) return false;
        if (TerminalController::isPasteSafe(text)) pasteText(text);
        else Q_EMIT unsafePasteRequested(text, this);
        return true;
    }
    if (name == QLatin1StringView("increase_font_size")
        || name == QLatin1StringView("decrease_font_size")) {
        bool valid = false;
        qreal amount = parameter.isEmpty()
            ? 1.0
            : parameter.toString().toDouble(&valid);
        if (parameter.isEmpty()) valid = true;
        if (!valid || !std::isfinite(amount) || amount <= 0.0) return false;
        adjustZoom(name == QLatin1StringView("increase_font_size")
                       ? amount
                       : -amount);
        return true;
    }
    if (name == QLatin1StringView("reset_font_size")) {
        resetZoom();
        return true;
    }
    if (name == QLatin1StringView("reload_config")) {
        Q_EMIT requestConfigReload();
        return true;
    }
    if (name == QLatin1StringView("end_key_sequence")) {
        if (colon >= 0) return false;
        keybinds_.resetSequence();
        if (activeSequenceToken_ != 0) {
            controller_->resolveSequence(activeSequenceToken_,
                                         TerminalSequenceResolution::Flush);
            activeSequenceToken_ = 0;
        }
        return true;
    }
    if (name == QLatin1StringView("close_window")) {
        // ghostty-qt currently has exactly one window, so closing that window
        // follows the same confirmed shutdown path as application quit.
        Q_EMIT requestQuit();
        return true;
    }
    if (name == QLatin1StringView("ignore")) {
        return true;
    }

    const GhosttyActionTranslation translated =
        GhosttyActionCatalog::translate(action);
    if (!translated.accepted()) {
        return false;
    }
    const WorkspaceActionRequest &request = *translated.request;
    if (workspaceActionHandler_) {
        return workspaceActionHandler_(request);
    }
    switch (request.action) {
    case WorkspaceAction::NewTab:
        Q_EMIT requestNewTab();
        return true;
    case WorkspaceAction::CloseTab:
        Q_EMIT requestCloseTab();
        return true;
    case WorkspaceAction::ClosePane:
        Q_EMIT requestClose();
        return true;
    case WorkspaceAction::SplitRight:
        Q_EMIT requestSplit(static_cast<int>(Qt::Horizontal));
        return true;
    case WorkspaceAction::SplitDown:
        Q_EMIT requestSplit(static_cast<int>(Qt::Vertical));
        return true;
    case WorkspaceAction::NavigatePane:
        Q_EMIT requestNavigate(static_cast<int>(request.context.value));
        return true;
    case WorkspaceAction::ChangeTabRelative:
        Q_EMIT requestTabChange(static_cast<int>(request.context.value));
        return true;
    case WorkspaceAction::RequestQuit:
        Q_EMIT requestQuit();
        return true;
    case WorkspaceAction::ActivateTab:
    case WorkspaceAction::ActivatePane:
    case WorkspaceAction::NavigatePaneRelative:
    case WorkspaceAction::ActivateTabByIndex:
    case WorkspaceAction::ActivateLastTab:
    case WorkspaceAction::MoveTab:
    case WorkspaceAction::ResizeSplit:
    case WorkspaceAction::EqualizeSplits:
    case WorkspaceAction::ToggleSplitZoom:
        return false;
    }
    return false;
}

void TerminalPane::inputMethodEvent(QInputMethodEvent *event)
{
    if (!event->commitString().isEmpty()) {
        controller_->sendText(event->commitString());
    }
    {
        QMutexLocker locker(&renderMutex_);
        preedit_ = event->preeditString();
    }
    update();
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
        QMutexLocker locker(&renderMutex_);
        return QRectF(static_cast<qreal>(frame_.cursorColumn) * cellWidth_,
                      static_cast<qreal>(frame_.cursorRow) * cellHeight_,
                      cellWidth_ * static_cast<qreal>(frame_.cursorColumnSpan), cellHeight_);
    }
    return QQuickItem::inputMethodQuery(query);
}

void TerminalPane::mousePressEvent(QMouseEvent *event)
{
    forceActiveFocus(Qt::MouseFocusReason);
    Q_EMIT activated(this);
    updateHyperlinkHover(event->position(), event->modifiers());
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
            hyperlinkPressRequestId_ =
                controller_->prepareHyperlinkActivation(
                    hyperlinkPressCell_.x(), hyperlinkPressCell_.y(),
                    contentRevision);
        }
    }
    const Qt::KeyboardModifiers modifiers = hoverModifiers_;
    const bool report = controller_->mouseTracking()
        && !modifiers.testFlag(Qt::ShiftModifier);
    if (event->button() != Qt::NoButton) {
        if (report) {
            mouseReportedPresses_.insert(event->button());
        } else {
            mouseReportedPresses_.remove(event->button());
        }
    }
    if (report) {
        sendMouse(event->position(), TerminalMouseInput::Press, event->button(),
                  reportedMouseButtons(event->buttons()), modifiers);
    } else if (event->button() == Qt::LeftButton) {
        // Ghostty starts its normal selection gesture even for a potential
        // link click. A release may activate the link, while a drag naturally
        // continues as selection instead.
        beginLocalSelection(event->position(), 1, modifiers);
    } else if (event->button() == Qt::MiddleButton) {
        const QString text = QGuiApplication::clipboard()->text(QClipboard::Selection);
        if (!text.isEmpty()) {
            if (TerminalController::isPasteSafe(text)) pasteText(text);
            else Q_EMIT unsafePasteRequested(text, this);
        }
    }
    event->accept();
}

void TerminalPane::mouseDoubleClickEvent(QMouseEvent *event)
{
    forceActiveFocus(Qt::MouseFocusReason);
    Q_EMIT activated(this);
    updateHyperlinkHover(event->position(), event->modifiers());
    const Qt::KeyboardModifiers modifiers = hoverModifiers_;
    const bool report = controller_->mouseTracking()
        && !modifiers.testFlag(Qt::ShiftModifier);
    if (report) {
        mouseReportedPresses_.insert(event->button());
    } else {
        mouseReportedPresses_.remove(event->button());
    }
    if (!report) {
        beginLocalSelection(event->position(), 2, modifiers);
    } else {
        sendMouse(event->position(), TerminalMouseInput::Press, event->button(),
                  reportedMouseButtons(event->buttons()), modifiers);
    }
    event->accept();
}

void TerminalPane::beginLocalSelection(const QPointF &position, int clickCount,
                                       Qt::KeyboardModifiers modifiers)
{
    const QPoint cell = cellAt(position);
    selecting_ = true;
    controller_->beginSelection(cell.x(), cell.y(), clickCount,
                                modifiers.testFlag(Qt::AltModifier));
}

void TerminalPane::mouseMoveEvent(QMouseEvent *event)
{
    if (hyperlinkPressArmed_
        && (event->position() - hyperlinkPressPosition_).manhattanLength()
            >= QGuiApplication::styleHints()->startDragDistance()) {
        if (!hyperlinkPressDragged_) {
            hyperlinkPressDragged_ = true;
            controller_->cancelHyperlinkActivation(
                hyperlinkPressRequestId_);
            hyperlinkPressRequestId_ = 0;
        }
    }
    updateHyperlinkHover(event->position(), event->modifiers());
    const Qt::KeyboardModifiers modifiers = hoverModifiers_;
    const Qt::MouseButtons reportedButtons =
        reportedMouseButtons(event->buttons());
    const bool report = event->buttons() == Qt::NoButton
        ? controller_->mouseTracking()
            && !modifiers.testFlag(Qt::ShiftModifier)
        : reportedButtons != Qt::NoButton;
    if (report) {
        sendMouse(event->position(), TerminalMouseInput::Motion, Qt::NoButton,
                  reportedButtons, modifiers);
    } else if (selecting_ && event->buttons().testFlag(Qt::LeftButton)) {
        const QPoint cell = cellAt(event->position());
        controller_->updateSelection(cell.x(), cell.y(),
                                     modifiers.testFlag(Qt::AltModifier));
    }
    event->accept();
}

void TerminalPane::mouseReleaseEvent(QMouseEvent *event)
{
    if (hyperlinkPressArmed_
        && (event->position() - hyperlinkPressPosition_).manhattanLength()
            >= QGuiApplication::styleHints()->startDragDistance()) {
        if (!hyperlinkPressDragged_) {
            hyperlinkPressDragged_ = true;
            controller_->cancelHyperlinkActivation(
                hyperlinkPressRequestId_);
            hyperlinkPressRequestId_ = 0;
        }
    }
    updateHyperlinkHover(event->position(), event->modifiers());
    const Qt::KeyboardModifiers modifiers = hoverModifiers_;
    const bool report = mouseReportedPresses_.remove(event->button()) > 0;
    if (event->button() == Qt::LeftButton && selecting_) {
        const QPoint cell = cellAt(event->position());
        controller_->endSelection(cell.x(), cell.y());
        selecting_ = false;
    }
    const bool activateHyperlink = event->button() == Qt::LeftButton
        && hyperlinkPressArmed_ && !hyperlinkPressDragged_
        && hyperlinkModifiersMatch(hoverModifiers_)
        && hyperlinkPressRequestId_ != 0
        && hoverCell_.x() >= 0 && hoverCell_.y() >= 0;
    if (activateHyperlink) {
        pendingActivationKind_ = hyperlinkPressKind_;
        pendingActivationUri_ = hyperlinkPressUri_;
        pendingActivationRequestId_ = hyperlinkPressRequestId_;
        controller_->commitHyperlinkActivation(
            hyperlinkPressRequestId_, hoverCell_.x(), hoverCell_.y());
    } else if (report) {
        sendMouse(event->position(), TerminalMouseInput::Release, event->button(),
                  reportedMouseButtons(event->buttons()), modifiers);
    }
    if (!activateHyperlink) {
        controller_->cancelHyperlinkActivation(
            hyperlinkPressRequestId_);
    }
    hyperlinkPressArmed_ = false;
    hyperlinkPressDragged_ = false;
    hyperlinkPressCell_ = QPoint(-1, -1);
    hyperlinkPressKind_ = TerminalLinkKind::Osc8;
    hyperlinkPressUri_.clear();
    hyperlinkPressRequestId_ = 0;
    event->accept();
}

void TerminalPane::hoverMoveEvent(QHoverEvent *event)
{
    updateHyperlinkHover(event->position(), event->modifiers());
    const Qt::KeyboardModifiers modifiers = hoverModifiers_;
    if (controller_->mouseTracking()
        && !modifiers.testFlag(Qt::ShiftModifier)) {
        sendMouse(event->position(), TerminalMouseInput::Motion, Qt::NoButton,
                  Qt::NoButton, modifiers);
    }
    event->accept();
}

void TerminalPane::hoverLeaveEvent(QHoverEvent *event)
{
    hoverInside_ = false;
    hoverCell_ = QPoint(-1, -1);
    clearHyperlinkHover();
    cancelHyperlinkPress();
    QQuickItem::hoverLeaveEvent(event);
    event->accept();
}

void TerminalPane::wheelEvent(QWheelEvent *event)
{
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0) {
        event->ignore();
        return;
    }
    const Qt::KeyboardModifiers modifiers =
        effectivePointerModifiers(event->modifiers());
    if (controller_->mouseTracking()
        && !modifiers.testFlag(Qt::ShiftModifier)) {
        TerminalMouseInput input;
        input.action = TerminalMouseInput::Press;
        input.button = steps > 0 ? 4 : 5;
        input.modifiers = static_cast<int>(modifiers);
        const qreal devicePixelRatio = window() != nullptr ? window()->devicePixelRatio() : 1.0;
        input.x = static_cast<float>(event->position().x() * devicePixelRatio);
        input.y = static_cast<float>(event->position().y() * devicePixelRatio);
        for (int index = 0; index < std::abs(steps); ++index) {
            controller_->sendMouse(input);
        }
    } else {
        TerminalViewportRequest request;
        request.kind = TerminalViewportRequest::Kind::Delta;
        request.delta = -static_cast<qint64>(steps) * 3;
        controller_->scrollViewport(request);
    }
    event->accept();
}

void TerminalPane::sendMouse(const QPointF &position, TerminalMouseInput::Action action,
                             Qt::MouseButton button, Qt::MouseButtons buttons,
                             Qt::KeyboardModifiers modifiers)
{
    const qreal devicePixelRatio = window() != nullptr ? window()->devicePixelRatio() : 1.0;
    TerminalMouseInput input;
    input.action = action;
    Qt::MouseButton effectiveButton = button;
    if (action == TerminalMouseInput::Motion && effectiveButton == Qt::NoButton) {
        // Button-event tracking (DECSET 1002) requires the identity of the
        // held button on motion, not only a generic "some button" flag.
        if (buttons.testFlag(Qt::LeftButton)) effectiveButton = Qt::LeftButton;
        else if (buttons.testFlag(Qt::MiddleButton)) effectiveButton = Qt::MiddleButton;
        else if (buttons.testFlag(Qt::RightButton)) effectiveButton = Qt::RightButton;
        else if (buttons.testFlag(Qt::BackButton)) effectiveButton = Qt::BackButton;
        else if (buttons.testFlag(Qt::ForwardButton)) effectiveButton = Qt::ForwardButton;
    }
    input.button = normalizedMouseButton(effectiveButton);
    input.modifiers = static_cast<int>(modifiers);
    input.x = static_cast<float>(position.x() * devicePixelRatio);
    input.y = static_cast<float>(position.y() * devicePixelRatio);
    input.anyButtonPressed = buttons != Qt::NoButton;
    controller_->sendMouse(input);
}

Qt::MouseButtons TerminalPane::reportedMouseButtons(
    Qt::MouseButtons buttons) const
{
    Qt::MouseButtons reported = Qt::NoButton;
    for (const Qt::MouseButton button : mouseReportedPresses_) {
        if (buttons.testFlag(button)) {
            reported |= button;
        }
    }
    return reported;
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

Qt::KeyboardModifiers TerminalPane::effectivePointerModifiers(
    Qt::KeyboardModifiers modifiers) const
{
    return normalizedModifiers(modifiers) | keyboardModifiers_;
}

bool TerminalPane::hyperlinkModifiersMatch(
    Qt::KeyboardModifiers modifiers) const
{
    modifiers = normalizedModifiers(modifiers);
    if (controller_->mouseTracking()) {
        // Shift is Ghostty's Linux escape hatch from application mouse
        // capture. Once it releases capture, it is removed before matching
        // the exact Ctrl-only OSC 8 modifier.
        if (!modifiers.testFlag(Qt::ShiftModifier)) {
            return false;
        }
        modifiers &= ~Qt::ShiftModifier;
    }
    return modifiers == Qt::ControlModifier;
}

std::optional<QPoint> TerminalPane::hoverCellAt(
    const QPointF &position) const
{
    QMutexLocker locker(&renderMutex_);
    if (!hasFrame_ || frame_.columns <= 0 || frame_.rows <= 0
        || position.x() < 0.0 || position.y() < 0.0) {
        return std::nullopt;
    }
    const int column = static_cast<int>(std::floor(position.x() / cellWidth_));
    const int row = static_cast<int>(std::floor(position.y() / cellHeight_));
    if (column < 0 || column >= frame_.columns
        || row < 0 || row >= frame_.rows) {
        return std::nullopt;
    }
    return QPoint(column, row);
}

void TerminalPane::updateHyperlinkHover(
    const QPointF &position, Qt::KeyboardModifiers modifiers)
{
    hoverInside_ = true;
    hoverPosition_ = position;
    hoverModifiers_ = effectivePointerModifiers(modifiers);
    const std::optional<QPoint> cell = hoverCellAt(position);
    const QPoint nextCell = cell.value_or(QPoint(-1, -1));
    if (nextCell != hoverCell_) {
        if (hyperlinkPressArmed_) {
            hyperlinkPressDragged_ = true;
            controller_->cancelHyperlinkActivation(
                hyperlinkPressRequestId_);
            hyperlinkPressRequestId_ = 0;
        }
        bool remainsOnResolvedLink = false;
        {
            QMutexLocker locker(&renderMutex_);
            const int index = nextCell.y() * frame_.columns + nextCell.x();
            remainsOnResolvedLink = nextCell.x() >= 0
                && nextCell.x() < frame_.columns
                && nextCell.y() >= 0 && nextCell.y() < frame_.rows
                && hoveredHyperlinkColumns_ == frame_.columns
                && hoveredHyperlinkRows_ == frame_.rows
                && hoveredHyperlinkCellIndexes_.contains(index)
                && hyperlinkLeaseActive_
                && !hyperlinkPressDragged_
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
    if (!hyperlinkModifiersMatch(hoverModifiers_)) {
        cancelHyperlinkPress();
        clearHyperlinkHover();
        return;
    }
    refreshHyperlinkHover();
}

void TerminalPane::recomputeHyperlinkHover()
{
    if (hoverInside_) {
        updateHyperlinkHover(hoverPosition_, hoverModifiers_);
    }
}

void TerminalPane::updateHyperlinkModifiers(
    Qt::KeyboardModifiers modifiers)
{
    keyboardModifiers_ = normalizedModifiers(modifiers);
    hoverModifiers_ = keyboardModifiers_;
    if (!hyperlinkModifiersMatch(hoverModifiers_)) {
        cancelHyperlinkPress();
        clearHyperlinkHover();
        return;
    }
    refreshHyperlinkHover();
}

void TerminalPane::refreshHyperlinkHover()
{
    if (!hoverInside_
        || hoverCell_.x() < 0 || hoverCell_.y() < 0
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
        const int targetIndex = hoverCell_.y() * frame_.columns
            + hoverCell_.x();
        targetMayHaveLink = options_.linkUrl || (targetIndex >= 0
            && targetIndex < frame_.cells.size()
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

void TerminalPane::clearHyperlinkDecoration()
{
    hoveredLinkKind_ = TerminalLinkKind::Osc8;
    hoveredHyperlinkUri_.clear();
    hoveredHyperlinkCell_ = QPoint(-1, -1);

    bool hadHighlight = false;
    {
        QMutexLocker locker(&renderMutex_);
        hadHighlight = !hoveredHyperlinkCellIndexes_.isEmpty();
        hoveredHyperlinkCellIndexes_.clear();
        hoveredHyperlinkColumns_ = 0;
        hoveredHyperlinkRows_ = 0;
    }
    unsetCursor();
    if (hadHighlight) {
        update();
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
    controller_->cancelHyperlinkActivation(
        pendingActivationRequestId_);
    pendingActivationRequestId_ = 0;
    pendingActivationKind_ = TerminalLinkKind::Osc8;
    pendingActivationUri_.clear();
}

bool TerminalPane::hyperlinkCellCandidate(
    const QPoint &cell, quint64 *contentRevision) const
{
    QMutexLocker locker(&renderMutex_);
    if (!hasFrame_ || cell.x() < 0 || cell.x() >= frame_.columns
        || cell.y() < 0 || cell.y() >= frame_.rows) {
        return false;
    }
    const int index = cell.y() * frame_.columns + cell.x();
    const bool osc8Candidate = index >= 0 && index < frame_.cells.size()
        && frame_.cells.at(index).hasHyperlink;
    const bool resolvedRegexCandidate = options_.linkUrl
        && hoveredLinkKind_ == TerminalLinkKind::Regex
        && hoveredHyperlinkColumns_ == frame_.columns
        && hoveredHyperlinkRows_ == frame_.rows
        && hoveredHyperlinkCellIndexes_.contains(index);
    if (!osc8Candidate && !resolvedRegexCandidate) {
        return false;
    }
    if (contentRevision != nullptr) {
        *contentRevision = frame_.contentRevision;
    }
    return true;
}

void TerminalPane::handleHyperlinkResult(
    quint64 contentRevision, TerminalHyperlinkState state,
    TerminalLinkKind kind, const QByteArray &uri, const QPoint &targetCell,
    const QVector<QPoint> &matchingCells)
{
    if (!hyperlinkQueryPending_ && !hyperlinkLeaseActive_) {
        return;
    }
    const bool hadTrackedLease = hyperlinkLeaseActive_;
    hyperlinkQueryPending_ = false;

    quint64 currentRevision = 0;
    int columns = 0;
    int rows = 0;
    {
        QMutexLocker locker(&renderMutex_);
        currentRevision = frame_.contentRevision;
        columns = frame_.columns;
        rows = frame_.rows;
    }
    if (hyperlinkQueryCell_ != hoverCell_
        || !hoverInside_ || !hyperlinkModifiersMatch(hoverModifiers_)) {
        clearHyperlinkHover();
        return;
    }

    if (state == TerminalHyperlinkState::Hidden) {
        hyperlinkLeaseActive_ = true;
        hyperlinkQueryRejected_ = false;
        clearHyperlinkDecoration();
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
        if (currentRevision >= contentRevision) {
            refreshHyperlinkHover();
        }
        return;
    }
    if (state != TerminalHyperlinkState::Visible || uri.isEmpty()) {
        hyperlinkLeaseActive_ = false;
        hyperlinkQueryRejected_ = !hadTrackedLease;
        clearHyperlinkDecoration();
        if (hadTrackedLease) {
            hyperlinkQueryCell_ = QPoint(-1, -1);
            refreshHyperlinkHover();
        }
        return;
    }

    QSet<int> indexes;
    for (const QPoint &cell : matchingCells) {
        if (cell.x() >= 0 && cell.x() < columns
            && cell.y() >= 0 && cell.y() < rows) {
            indexes.insert(cell.y() * columns + cell.x());
        }
    }
    if (targetCell.x() >= 0 && targetCell.x() < columns
        && targetCell.y() >= 0 && targetCell.y() < rows) {
        indexes.insert(targetCell.y() * columns + targetCell.x());
    }
    const int hoverIndex = hoverCell_.y() * columns + hoverCell_.x();
    if (!indexes.contains(hoverIndex)) {
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
        hoveredHyperlinkCellIndexes_ = std::move(indexes);
        hoveredHyperlinkColumns_ = columns;
        hoveredHyperlinkRows_ = rows;
    }
    setCursor(Qt::PointingHandCursor);
    update();
}

QUrl TerminalPane::hyperlinkUrl(
    const QByteArray &uri, TerminalLinkKind kind) const
{
    if (uri.isEmpty() || uri.contains('\0')) {
        return {};
    }
    if (kind == TerminalLinkKind::Regex) {
        const QString value = QString::fromUtf8(uri);
        if (!QDir::isAbsolutePath(value)) {
            const QString directory = controller_->currentDirectory();
            if (!directory.isEmpty()) {
                const QString resolved = QDir::cleanPath(
                    QDir(directory).absoluteFilePath(value));
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

void TerminalPane::handleHyperlinkActivation(
    quint64 contentRevision, TerminalLinkKind kind, const QByteArray &uri)
{
    static_cast<void>(contentRevision);
    const quint64 requestId =
        std::exchange(pendingActivationRequestId_, 0);
    const TerminalLinkKind expectedKind = std::exchange(
        pendingActivationKind_, TerminalLinkKind::Osc8);
    const QByteArray expectedUri = std::exchange(pendingActivationUri_, {});
    if (requestId == 0 || uri.isEmpty()
        || kind != expectedKind
        || (!expectedUri.isEmpty() && uri != expectedUri) || !urlOpener_) {
        return;
    }
    const QUrl url = hyperlinkUrl(uri, kind);
    if (url.isValid() && !url.isEmpty()) {
        static_cast<void>(urlOpener_(url));
    }
}

QPoint TerminalPane::cellAt(const QPointF &position) const
{
    QMutexLocker locker(&renderMutex_);
    const int columns = hasFrame_ ? frame_.columns : 80;
    const int rows = hasFrame_ ? frame_.rows : 24;
    return QPoint(
        std::clamp(static_cast<int>(std::floor(position.x() / cellWidth_)), 0,
                   std::max(0, columns - 1)),
        std::clamp(static_cast<int>(std::floor(position.y() / cellHeight_)), 0,
                   std::max(0, rows - 1)));
}

void TerminalPane::focusInEvent(QFocusEvent *event)
{
    QQuickItem::focusInEvent(event);
    resetCursorBlink();
    controller_->setFocused(true);
    Q_EMIT activated(this);
}

void TerminalPane::focusOutEvent(QFocusEvent *event)
{
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
    update();
}

void TerminalPane::focusTerminal()
{
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

void TerminalPane::setFontPointSize(qreal points)
{
    points = std::clamp(points, kMinimumFontSize, kMaximumFontSize);
    {
        QMutexLocker locker(&renderMutex_);
        if (qFuzzyCompare(font_.pointSizeF(), points)) {
            return;
        }
        font_.setPointSizeF(points);
    }
    updateMetrics();
    updateTerminalSize();
    update();
    Q_EMIT fontPointSizeChanged();
}

void TerminalPane::zoomIn()
{
    adjustZoom(1.0);
}

void TerminalPane::zoomOut()
{
    adjustZoom(-1.0);
}

void TerminalPane::adjustZoom(qreal delta)
{
    manuallyZoomed_ = true;
    options_.fontSizeManuallyAdjusted = true;
    setFontPointSize(fontPointSize() + delta);
}

void TerminalPane::resetZoom()
{
    manuallyZoomed_ = false;
    options_.fontSizeManuallyAdjusted = false;
    setFontPointSize(defaultFontPointSize_);
}
