#include "terminal_pane.h"

#include "terminal_controller.h"

#include <QClipboard>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMutexLocker>
#include <QSGGeometryNode>
#include <QSGTextNode>
#include <QSGVertexColorMaterial>
#include <QQuickWindow>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

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

QSGGeometryNode *createRectNode(const QVector<ColoredRect> &rects)
{
    if (rects.isEmpty()) {
        return nullptr;
    }

    auto *geometry = new QSGGeometry(
        QSGGeometry::defaultAttributes_ColoredPoint2D(),
        static_cast<int>(rects.size()) * 6);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::StaticPattern);
    auto *vertices = geometry->vertexDataAsColoredPoint2D();

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

} // namespace

TerminalPane::TerminalPane(const LaunchOptions &options, QQuickItem *parent)
    : QQuickItem(parent)
    , options_(options)
    , defaultFontPointSize_(options.fontSize)
{
    setFlag(QQuickItem::ItemHasContents, true);
    setClip(true);
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
    setFlag(QQuickItem::ItemAcceptsInputMethod, true);
    setFocusPolicy(Qt::StrongFocus);

    const QString family = options.fontFamily.isEmpty()
        ? QFontDatabase::systemFont(QFontDatabase::FixedFont).family()
        : options.fontFamily;
    font_.setFamily(family);
    font_.setPointSizeF(options.fontSize);
    font_.setFixedPitch(true);
    font_.setStyleHint(QFont::Monospace);
    updateMetrics();

    cursorTimer_ = new QTimer(this);
    cursorTimer_->setInterval(500);
    connect(cursorTimer_, &QTimer::timeout, this, [this] {
        cursorBlinkOn_ = !cursorBlinkOn_;
        update();
    });
    cursorTimer_->start();

    controller_ = new TerminalController(options, this);
    connect(controller_, &TerminalController::frameReady, this,
            [this](const TerminalFrame &frame) {
                {
                    QMutexLocker locker(&renderMutex_);
                    frame_ = frame;
                    hasFrame_ = true;
                }
                update();
            });
    connect(controller_, &TerminalController::titleChanged,
            this, &TerminalPane::titleChanged);
    connect(controller_, &TerminalController::currentDirectoryChanged,
            this, &TerminalPane::currentDirectoryChanged);
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

bool TerminalPane::isRunning() const
{
    return controller_->running();
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
    result.program.clear();
    result.hold = false;
    return result;
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
    QFont baseFont;
    QString preedit;
    QString status;
    qreal cellWidth = 0.0;
    qreal cellHeight = 0.0;
    qreal baseline = 0.0;
    bool hasFrame = false;
    {
        QMutexLocker locker(&renderMutex_);
        frame = frame_;
        baseFont = font_;
        preedit = preedit_;
        status = statusMessage_;
        cellWidth = cellWidth_;
        cellHeight = cellHeight_;
        baseline = baseline_;
        hasFrame = hasFrame_;
    }

    const QRectF viewport = boundingRect();
    const QColor background = hasFrame ? frame.background : QColor(QStringLiteral("#1e222a"));
    QVector<ColoredRect> backgrounds;
    QVector<ColoredRect> decorations;
    QVector<ColoredRect> overlayBackgrounds;
    QVector<ColoredRect> overlayDecorations;
    appendRect(backgrounds, viewport, background);

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
        const QColor selectionBackground(QStringLiteral("#4c566a"));
        const QColor selectionForeground(QStringLiteral("#eceff4"));

        const int visibleRows = std::min(frame.rows,
            static_cast<int>(std::ceil(height() / cellHeight)));
        const int visibleColumns = std::min(frame.columns,
            static_cast<int>(std::ceil(width() / cellWidth)));
        const bool cursorActive = frame.cursorVisible
            && (!frame.cursorBlinking || cursorBlinkOn_)
            && frame.cursorColumn >= 0 && frame.cursorColumn < visibleColumns
            && frame.cursorRow >= 0 && frame.cursorRow < visibleRows;
        const bool blockCursorActive = cursorActive
            && frame.cursorStyle != 0 && frame.cursorStyle != 2 && frame.cursorStyle != 3;

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
                const QColor cellBackground = cell.selected
                    ? selectionBackground : cell.background;
                if (cellBackground != background) {
                    appendRect(backgrounds, QRectF(left, top, drawWidth, cellHeight),
                               cellBackground);
                }

                QColor foreground = cell.selected
                    ? selectionForeground : cell.foreground;
                if (cell.faint) {
                    foreground.setAlpha(150);
                }
                if (blockCursorActive && row == frame.cursorRow
                    && column == frame.cursorColumn) {
                    foreground = frame.background;
                }

                if (!cell.text.isEmpty() && !cell.spacer) {
                    const QFont *drawFont = &normal;
                    if (cell.bold && cell.italic) drawFont = &boldItalic;
                    else if (cell.bold) drawFont = &bold;
                    else if (cell.italic) drawFont = &italic;
                    appendTextLayout(mainText, cell.text, *drawFont, foreground,
                                     QPointF(left, top), baseline, drawWidth);
                    hasMainText = mainText != nullptr;
                }

                if (cell.underline) {
                    appendRect(decorations,
                               QRectF(left, top + baseline + 1.0, drawWidth, 1.0),
                               cell.selected ? selectionForeground
                                             : cell.underlineColor);
                }
                if (cell.strikeThrough) {
                    appendRect(decorations,
                               QRectF(left, top + cellHeight * 0.52, drawWidth, 1.0),
                               foreground);
                }
                if (cell.overline) {
                    appendRect(decorations, QRectF(left, top + 1.0, drawWidth, 1.0),
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
            switch (frame.cursorStyle) {
            case 0:
                appendRect(decorations,
                           QRectF(left, top, std::max(1.0, cellWidth * 0.14), cellHeight),
                           frame.cursorColor);
                break;
            case 2:
                appendRect(decorations,
                           QRectF(left, top + cellHeight - 2.0, cursorWidth, 2.0),
                           frame.cursorColor);
                break;
            case 3: {
                constexpr qreal thickness = 1.0;
                appendRect(decorations,
                           QRectF(left, top, cursorWidth, thickness), frame.cursorColor);
                appendRect(decorations,
                           QRectF(left, top + cellHeight - thickness,
                                  cursorWidth, thickness), frame.cursorColor);
                appendRect(decorations,
                           QRectF(left, top + thickness, thickness,
                                  std::max(0.0, cellHeight - 2.0 * thickness)),
                           frame.cursorColor);
                appendRect(decorations,
                           QRectF(left + cursorWidth - thickness, top + thickness,
                                  thickness,
                                  std::max(0.0, cellHeight - 2.0 * thickness)),
                           frame.cursorColor);
                break;
            }
            default:
                // Appended after cell backgrounds so a wide block cursor is
                // not overwritten by the spacer cell's background.
                appendRect(backgrounds, cursorRect, frame.cursorColor);
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
            appendRect(decorations,
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

    if (QSGGeometryNode *node = createRectNode(backgrounds)) {
        root->appendChildNode(node);
    }
    if (hasMainText) {
        root->appendChildNode(mainText);
    } else {
        delete mainText;
    }
    if (QSGGeometryNode *node = createRectNode(decorations)) {
        root->appendChildNode(node);
    }
    if (QSGGeometryNode *node = createRectNode(overlayBackgrounds)) {
        root->appendChildNode(node);
    }
    if (hasOverlayText) {
        root->appendChildNode(overlayText);
    } else {
        delete overlayText;
    }
    if (QSGGeometryNode *node = createRectNode(overlayDecorations)) {
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
        return;
    }
    const qreal devicePixelRatio = window() != nullptr ? window()->devicePixelRatio() : 1.0;
    const int columns = std::max(1, static_cast<int>(std::floor(width() / cellWidth)));
    const int rows = std::max(1, static_cast<int>(std::floor(height() / cellHeight)));
    controller_->resizeTerminal(
        columns, rows,
        std::max(1, qRound(cellWidth * devicePixelRatio)),
        std::max(1, qRound(cellHeight * devicePixelRatio)),
        std::max(1, qRound(width() * devicePixelRatio)),
        std::max(1, qRound(height() * devicePixelRatio)));
}

void TerminalPane::keyPressEvent(QKeyEvent *event)
{
    if (handleShortcut(event)) {
        consumedKeys_.insert(event->key());
        event->accept();
        return;
    }

    TerminalKeyInput input;
    input.key = event->key();
    input.modifiers = static_cast<int>(event->modifiers());
    input.text = event->text();
    input.pressed = true;
    input.autoRepeat = event->isAutoRepeat();
    input.unshiftedCodepoint = unshiftedCodepoint(event->key());
    controller_->sendKey(input);
    event->accept();
}

void TerminalPane::keyReleaseEvent(QKeyEvent *event)
{
    if (consumedKeys_.remove(event->key())) {
        event->accept();
        return;
    }
    TerminalKeyInput input;
    input.key = event->key();
    input.modifiers = static_cast<int>(event->modifiers());
    input.text = event->text();
    input.pressed = false;
    input.autoRepeat = event->isAutoRepeat();
    input.unshiftedCodepoint = unshiftedCodepoint(event->key());
    controller_->sendKey(input);
    event->accept();
}

bool TerminalPane::handleShortcut(QKeyEvent *event)
{
    const Qt::KeyboardModifiers modifiers = normalizedModifiers(event->modifiers());
    const bool control = modifiers.testFlag(Qt::ControlModifier);
    const bool shift = modifiers.testFlag(Qt::ShiftModifier);
    const bool alt = modifiers.testFlag(Qt::AltModifier);
    const int key = event->key();

    if (control && shift && modifiers == (Qt::ControlModifier | Qt::ShiftModifier)) {
        switch (key) {
        case Qt::Key_C: copySelection(); return true;
        case Qt::Key_V: {
            const QString text = QGuiApplication::clipboard()->text();
            if (!text.isEmpty()) {
                if (TerminalController::isPasteSafe(text)) pasteText(text);
                else Q_EMIT unsafePasteRequested(text, this);
            }
            return true;
        }
        case Qt::Key_T: Q_EMIT requestNewTab(); return true;
        case Qt::Key_E: Q_EMIT requestSplit(static_cast<int>(Qt::Horizontal)); return true;
        case Qt::Key_O: Q_EMIT requestSplit(static_cast<int>(Qt::Vertical)); return true;
        case Qt::Key_W: Q_EMIT requestClose(); return true;
        case Qt::Key_Q: Q_EMIT requestQuit(); return true;
        default: break;
        }
    }

    if (control && !alt && (key == Qt::Key_Plus || key == Qt::Key_Equal)) {
        zoomIn();
        return true;
    }
    if (control && !alt && key == Qt::Key_Minus) {
        zoomOut();
        return true;
    }
    if (control && !alt && key == Qt::Key_0) {
        resetZoom();
        return true;
    }
    if (control && !shift && !alt && key == Qt::Key_PageUp) {
        Q_EMIT requestTabChange(-1);
        return true;
    }
    if (control && !shift && !alt && key == Qt::Key_PageDown) {
        Q_EMIT requestTabChange(1);
        return true;
    }
    if (alt && !control && !shift
        && (key == Qt::Key_Left || key == Qt::Key_Right
            || key == Qt::Key_Up || key == Qt::Key_Down)) {
        Q_EMIT requestNavigate(key);
        return true;
    }
    if (shift && !control && !alt && (key == Qt::Key_PageUp || key == Qt::Key_PageDown)) {
        int pageRows = 20;
        {
            QMutexLocker locker(&renderMutex_);
            if (hasFrame_) pageRows = std::max(1, frame_.rows - 1);
        }
        controller_->scrollViewport(key == Qt::Key_PageUp ? -pageRows : pageRows);
        return true;
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
    const bool report = controller_->mouseTracking()
        && !event->modifiers().testFlag(Qt::ShiftModifier);
    if (report) {
        sendMouse(event->position(), TerminalMouseInput::Press, event->button(),
                  event->buttons(), event->modifiers());
    } else if (event->button() == Qt::LeftButton) {
        beginLocalSelection(event->position(), 1, event->modifiers());
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
    if (!controller_->mouseTracking() || event->modifiers().testFlag(Qt::ShiftModifier)) {
        beginLocalSelection(event->position(), 2, event->modifiers());
    } else {
        sendMouse(event->position(), TerminalMouseInput::Press, event->button(),
                  event->buttons(), event->modifiers());
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
    const bool report = controller_->mouseTracking()
        && !event->modifiers().testFlag(Qt::ShiftModifier);
    if (report) {
        sendMouse(event->position(), TerminalMouseInput::Motion, Qt::NoButton,
                  event->buttons(), event->modifiers());
    } else if (selecting_ && event->buttons().testFlag(Qt::LeftButton)) {
        const QPoint cell = cellAt(event->position());
        controller_->updateSelection(cell.x(), cell.y(),
                                     event->modifiers().testFlag(Qt::AltModifier));
    }
    event->accept();
}

void TerminalPane::mouseReleaseEvent(QMouseEvent *event)
{
    const bool report = controller_->mouseTracking()
        && !event->modifiers().testFlag(Qt::ShiftModifier);
    if (report) {
        sendMouse(event->position(), TerminalMouseInput::Release, event->button(),
                  event->buttons(), event->modifiers());
    }
    if (event->button() == Qt::LeftButton && selecting_) {
        const QPoint cell = cellAt(event->position());
        controller_->endSelection(cell.x(), cell.y());
        selecting_ = false;
    }
    event->accept();
}

void TerminalPane::hoverMoveEvent(QHoverEvent *event)
{
    if (controller_->mouseTracking()) {
        sendMouse(event->position(), TerminalMouseInput::Motion, Qt::NoButton,
                  Qt::NoButton, event->modifiers());
    }
    event->accept();
}

void TerminalPane::wheelEvent(QWheelEvent *event)
{
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0) {
        event->ignore();
        return;
    }
    if (controller_->mouseTracking()
        && !event->modifiers().testFlag(Qt::ShiftModifier)) {
        TerminalMouseInput input;
        input.action = TerminalMouseInput::Press;
        input.button = steps > 0 ? 4 : 5;
        input.modifiers = static_cast<int>(event->modifiers());
        const qreal devicePixelRatio = window() != nullptr ? window()->devicePixelRatio() : 1.0;
        input.x = static_cast<float>(event->position().x() * devicePixelRatio);
        input.y = static_cast<float>(event->position().y() * devicePixelRatio);
        for (int index = 0; index < std::abs(steps); ++index) {
            controller_->sendMouse(input);
        }
    } else {
        controller_->scrollViewport(-steps * 3);
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
    controller_->setFocused(true);
    Q_EMIT activated(this);
}

void TerminalPane::focusOutEvent(QFocusEvent *event)
{
    controller_->setFocused(false);
    QQuickItem::focusOutEvent(event);
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
    setFontPointSize(fontPointSize() + 1.0);
}

void TerminalPane::zoomOut()
{
    setFontPointSize(fontPointSize() - 1.0);
}

void TerminalPane::resetZoom()
{
    setFontPointSize(defaultFontPointSize_);
}
