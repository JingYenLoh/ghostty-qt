#include "terminal_pane.h"

#include "ghostty_action_catalog.h"
#include "terminal_clipboard.h"
#include "terminal_controller.h"
#include "terminal_pane_render_probe_p.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
#include <QHash>
#endif
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
#include <array>
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
#include <atomic>
#endif
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace {

constexpr float kMinimumActionFontSize = 1.0F;
constexpr float kMaximumActionFontSize = 255.0F;
constexpr qsizetype kMaximumLinkPreviewBytes = 4096;
constexpr qreal kLinkPreviewHorizontalPadding = 8.0;
constexpr qreal kLinkPreviewVerticalPadding = 4.0;

float unfocusedSplitOverlayOpacity(double paneOpacity)
{
    if (!std::isfinite(paneOpacity)) {
        paneOpacity = SplitAppearance{}.unfocusedOpacity;
    }
    paneOpacity = std::clamp(paneOpacity, 0.15, 1.0);
    // Pinned GTK writes the complementary opacity into runtime CSS with two
    // decimals. Preserve that observable composition rather than treating
    // this setting as the rectangle's opacity directly.
    return static_cast<float>(
        std::round((1.0 - paneOpacity) * 100.0) / 100.0);
}

float clampFontActionValue(float value, float minimum)
{
    // Zig's @min/@max select the numeric operand when the other is NaN.
    // std::fmin/std::fmax provide the same behavior; std::clamp does not.
    return std::fmax(minimum, std::fmin(value, kMaximumActionFontSize));
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
            display += QString::number(value, 16).rightJustified(2, u'0').toUpper();
        } else if (value == 0x061c || value == 0x200e || value == 0x200f
                   || (value >= 0x2028 && value <= 0x202e)
                   || (value >= 0x2066 && value <= 0x2069)) {
            display += QStringLiteral("\\u");
            display += QString::number(value, 16).rightJustified(4, u'0').toUpper();
        } else {
            display += character;
        }
    }
    if (truncated) {
        display += u'\u2026';
    }
    return display;
}

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

void clearNodeChildren(QSGNode *parent)
{
    while (QSGNode *child = parent->firstChild()) {
        parent->removeChildNode(child);
        delete child;
    }
}

struct TerminalTextRenderState {
    QQuickWindow *window = nullptr;
    QRectF viewport;
    QFont font;
    TerminalAppearance appearance;
    QColor foreground;
    QColor background;
    QVector<QColor> palette;
    QBitArray searchCandidateCells;
    QBitArray searchSelectedCells;
    qreal cellWidth = 0.0;
    qreal cellHeight = 0.0;
    qreal baseline = 0.0;
    qreal devicePixelRatio = 1.0;
    int graphicsApi = -1;
    int frameColumns = 0;
    int frameRows = 0;
    int visibleColumns = 0;
    int visibleRows = 0;

    bool operator==(const TerminalTextRenderState &) const = default;
};

struct BlockCursorTextState {
    bool active = false;
    int row = -1;
    int column = 0;
    int span = 0;
    QColor color;

    bool operator==(const BlockCursorTextState &) const = default;
};

#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
std::atomic<quint64> nextRenderNodeSerial{1};
QMutex renderProbeMutex;
QHash<const TerminalPane *, TerminalPaneRenderProbeSnapshot> renderProbes;
#endif

class TerminalSceneNode final : public QSGNode {
public:
    TerminalSceneNode()
        : beforeMain(new QSGNode)
        , mainTextRows(new QSGNode)
        , afterMain(new QSGNode)
        , unfocusedSplitOverlay(new QSGSimpleRectNode)
    {
        appendChildNode(beforeMain);
        appendChildNode(mainTextRows);
        appendChildNode(afterMain);
        appendChildNode(unfocusedSplitOverlay);
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
        rootSerial = nextRenderNodeSerial.fetch_add(
            1, std::memory_order_relaxed);
        unfocusedSplitOverlaySerial = nextRenderNodeSerial.fetch_add(
            1, std::memory_order_relaxed);
#endif
    }

    void setUnfocusedSplitOverlay(const QRectF &rect, QColor color) const
    {
        QRectF effectiveRect = rect;
        if (effectiveRect.isEmpty() || !color.isValid()
            || color.alpha() == 0) {
            effectiveRect = {};
            color = Qt::transparent;
        }
        if (unfocusedSplitOverlay->rect() != effectiveRect) {
            unfocusedSplitOverlay->setRect(effectiveRect);
        }
        if (unfocusedSplitOverlay->color() != color) {
            unfocusedSplitOverlay->setColor(color);
        }
    }

    void clearTransientNodes() const
    {
        clearNodeChildren(beforeMain);
        clearNodeChildren(afterMain);
    }

    void clearMainText()
    {
        clearNodeChildren(mainTextRows);
        rowContainers.clear();
        rowTextNodes.clear();
        builtRowEpochs.clear();
        textState.reset();
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
        rowNodeSerials.clear();
        rowBuildCounts.clear();
#endif
    }

    void resetTextRows(int rowCount, const QFont &baseFont)
    {
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
        QVector<quint64> cumulativeBuildCounts = rowBuildCounts;
#endif
        clearMainText();
        rowContainers.reserve(rowCount);
        rowTextNodes.fill(nullptr, rowCount);
        builtRowEpochs.fill(0, rowCount);
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
        rowNodeSerials.fill(0, rowCount);
        cumulativeBuildCounts.resize(rowCount);
        rowBuildCounts = std::move(cumulativeBuildCounts);
#endif
        for (int row = 0; row < rowCount; ++row) {
            auto *container = new QSGNode;
            mainTextRows->appendChildNode(container);
            rowContainers.append(container);
        }

        fonts.fill(baseFont);
        fonts[1].setBold(true);
        fonts[2].setItalic(true);
        fonts[3].setBold(true);
        fonts[3].setItalic(true);
    }

    QSGTextNode *prepareTextRow(int row, QQuickWindow *window,
                                const QRectF &viewport,
                                const QColor &defaultColor)
    {
        QSGTextNode *&textNode = rowTextNodes[row];
        if (textNode == nullptr) {
            textNode = createTextNode(window, viewport, defaultColor);
            if (textNode != nullptr) {
                rowContainers.at(row)->appendChildNode(textNode);
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
                rowNodeSerials[row] = nextRenderNodeSerial.fetch_add(
                    1, std::memory_order_relaxed);
#endif
            }
        } else {
            textNode->clear();
        }
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
        if (textNode != nullptr) {
            ++rowBuildCounts[row];
        }
#endif
        return textNode;
    }

    QSGNode *beforeMain = nullptr;
    QSGNode *mainTextRows = nullptr;
    QSGNode *afterMain = nullptr;
    QSGSimpleRectNode *unfocusedSplitOverlay = nullptr;
    QVector<QSGNode *> rowContainers;
    QVector<QSGTextNode *> rowTextNodes;
    QVector<quint64> builtRowEpochs;
    std::array<QFont, 4> fonts;
    std::optional<TerminalTextRenderState> textState;
    BlockCursorTextState blockCursorTextState;
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
    quint64 rootSerial = 0;
    quint64 unfocusedSplitOverlaySerial = 0;
    QVector<quint64> rowNodeSerials;
    QVector<quint64> rowBuildCounts;
#endif
};

#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
void clearRenderProbe(const TerminalPane *pane)
{
    QMutexLocker locker(&renderProbeMutex);
    renderProbes.remove(pane);
}

void publishRenderProbe(const TerminalPane *pane,
                        const TerminalSceneNode &root)
{
    QMutexLocker locker(&renderProbeMutex);
    TerminalPaneRenderProbeSnapshot &snapshot = renderProbes[pane];
    ++snapshot.paintSerial;
    snapshot.rootSerial = root.rootSerial;
    snapshot.unfocusedSplitOverlaySerial =
        root.unfocusedSplitOverlaySerial;
    snapshot.unfocusedSplitOverlayRect =
        root.unfocusedSplitOverlay->rect();
    snapshot.unfocusedSplitOverlayColor =
        root.unfocusedSplitOverlay->color();
    snapshot.rowNodeSerials = root.rowNodeSerials;
    snapshot.rowBuildCounts = root.rowBuildCounts;
}
#endif

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

#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
TerminalPaneRenderProbeSnapshot terminalPaneRenderProbe(
    const TerminalPane *pane)
{
    QMutexLocker locker(&renderProbeMutex);
    return renderProbes.value(pane);
}
#endif

TerminalPane::TerminalPane(const LaunchOptions &options, QQuickItem *parent)
    : QQuickItem(parent)
    , options_(options)
    , appearance_(options.appearance)
    , splitAppearance_(options.splitAppearance)
    , defaultFontPointSize_(options.fontSize)
{
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
    clearRenderProbe(this);
#endif
    setFlag(QQuickItem::ItemHasContents, true);
    setClip(true);
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
    setFlag(QQuickItem::ItemAcceptsInputMethod, true);
    setFocusPolicy(Qt::StrongFocus);
    const auto watchWindow = [this](QQuickWindow *quickWindow) {
        QObject::disconnect(windowActiveConnection_);
        windowActiveConnection_ = {};
        if (quickWindow != nullptr) {
            windowActiveConnection_ = connect(
                quickWindow, &QWindow::activeChanged,
                this, [this] { update(); });
        }
        update();
    };
    connect(this, &QQuickItem::windowChanged, this, watchWindow);
    watchWindow(window());
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

    controller_ = new TerminalController(
        toTerminalSessionLaunchOptions(options), this);
    connect(controller_, &TerminalController::terminalUpdated, this,
            [this](const TerminalUpdate &terminalUpdate) {
                bool applied = false;
                {
                    QMutexLocker locker(&renderMutex_);
                    if (hasFrame_ || terminalUpdate.fullFrame) {
                        applied = applyTerminalUpdate(frame_, terminalUpdate);
                        hasFrame_ = hasFrame_ || applied;
                        if (applied) {
                            markTextRowsChangedLocked(terminalUpdate);
                        }
                        if (applied && frame_.rows > 0
                            && (!terminalResizePending_
                                || frame_.rows == terminalRows_)) {
                            terminalRows_ = frame_.rows;
                            terminalResizePending_ = false;
                        }
                        if (applied) {
                            const bool pendingMatchesFrame =
                                pendingSearchUpdate_
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
                    syncCursorBlink(terminalUpdate.resetCursorBlink);
                }
            });
    connect(controller_, &TerminalController::searchUpdated, this,
            &TerminalPane::handleSearchUpdate);
    connect(controller_, &TerminalController::searchSelectionReady, this,
            [this](bool available, const QString &text) {
                if (!available) {
                    return;
                }
                const bool textChanged = searchUiText_ != text;
                searchUiText_ = text;
                setSearchUiActive(true);
                if (textChanged) {
                    Q_EMIT searchUiTextChanged();
                }
                controller_->search(text);
                Q_EMIT searchUiFocusRequested();
            });
    connect(controller_,
            &TerminalController::unsafePasteConfirmationRequested,
            this, [this](quint64 requestId, const QString &text) {
                Q_EMIT unsafePasteRequested(requestId, text, this);
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
    connect(controller_, &TerminalController::readOnlyChanged,
            this, &TerminalPane::readOnlyChanged);
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
                // TerminalController invalidates worker progress and pending
                // search_selection replies before forwarding sessionExited.
                setSearchUiActive(false);
                searchEngineActive_ = false;
                searchMatchLabel_ = QStringLiteral("0/0");
                Q_EMIT searchMatchLabelChanged();
                {
                    QMutexLocker locker(&renderMutex_);
                    clearPendingSearchUpdateLocked();
                    clearSearchDecorationsLocked();
                }
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

TerminalPane::~TerminalPane()
{
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
    clearRenderProbe(this);
#endif
}

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
        result.fontFamily = font_.family();
        result.fontSize = font_.pointSizeF();
    }
    result.program.clear();
    result.hold = false;
    return result;
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
        result.fontSize = font_.pointSizeF();
    }
    result.program.clear();
    result.hold = false;
    return result;
}

void TerminalPane::applyRuntimeOptions(const LaunchOptions &options)
{
    LaunchOptions updated = options_;
    updated.fontFamily = options.fontFamily;
    updated.fontSize = options.fontSize;
    updated.fontFamilyExplicit = options.fontFamilyExplicit;
    updated.fontSizeExplicit = options.fontSizeExplicit;
    updated.appearance = options.appearance;
    updated.selectionClipboard = options.selectionClipboard;
    updated.clipboardPaste = options.clipboardPaste;
    updated.splitAppearance = options.splitAppearance;
    updated.middleClickAction = options.middleClickAction;
    updated.linkUrl = options.linkUrl;
    updated.linkPreviews = options.linkPreviews;
    updated.keybindConfig = options.keybindConfig;
    updated.keybindings = options.keybindings;
    updated.keybindingsConfigured = options.keybindingsConfigured;
    if (updated == options_) {
        return;
    }

    const bool previewWasPointerCaptured = linkPreviewPointerCaptured_;
    const bool linkUrlChanged = options_.linkUrl != updated.linkUrl;
    const bool linkPreviewModeChanged =
        options_.linkPreviews != updated.linkPreviews;
    const TerminalSessionRuntimeOptions previousRuntime =
        toTerminalSessionRuntimeOptions(options_);
    const TerminalSessionRuntimeOptions updatedRuntime =
        toTerminalSessionRuntimeOptions(updated);

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
        const qreal reloadedFontSize = static_cast<qreal>(
            clampFontActionValue(static_cast<float>(updated.fontSize),
                                 kMinimumActionFontSize));
        if (!manuallyZoomed_
            && !qFuzzyCompare(font_.pointSizeF(), reloadedFontSize)) {
            font_.setPointSizeF(reloadedFontSize);
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
        splitAppearance_ = updated.splitAppearance;
    }
    options_ = updated;
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
    if (previousRuntime != updatedRuntime) {
        controller_->applyRuntimeOptions(updatedRuntime);
    }
    if (linkUrlChanged) {
        recomputeHyperlinkHover();
    }

    if (metricsChanged) {
        updateMetrics();
        updateTerminalSize();
    }
    if (linkPreviewModeChanged || metricsChanged) {
        refreshLinkPreview();
        reconcileReleasedLinkPreview(previewWasPointerCaptured);
    }
    update();
    if (pointSizeChanged) {
        Q_EMIT fontPointSizeChanged();
    }
}

void TerminalPane::setSplit(bool split)
{
    if (split_ == split) {
        return;
    }
    split_ = split;
    update();
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

void TerminalPane::startSearchUi()
{
    if (!searchUiActive_) {
        setSearchUiActive(true);
        // Ghostty retains the last entry text. Reopening the UI starts a fresh
        // generation so results cannot outlive terminal mutations that
        // happened while the overlay was hidden.
        controller_->search(searchUiText_);
    }
    Q_EMIT searchUiFocusRequested();
}

void TerminalPane::setSearchUiActive(bool active)
{
    if (searchUiActive_ == active) {
        return;
    }
    searchUiActive_ = active;
    update();
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
    update();
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
    searchCandidateCellMask_.clear();
    searchSelectedCellMask_.clear();
    searchDecorationRevision_ = searchUpdate.contentRevision;
    searchDecorationColumns_ = searchUpdate.columns;
    searchDecorationRows_ = searchUpdate.rows;
    if (!searchUpdate.active || searchUpdate.columns <= 0
        || searchUpdate.rows <= 0) {
        return;
    }

    const qsizetype columnCount = searchUpdate.columns;
    const qsizetype maskSize = searchUpdate.visibleCellMask.size();
    const bool masksEmpty = maskSize == 0
        && searchUpdate.selectedCellMask.isEmpty();
    const bool masksMatchGrid = maskSize > 0
        && maskSize == searchUpdate.selectedCellMask.size()
        && maskSize % columnCount == 0
        && maskSize / columnCount == searchUpdate.rows;
    if (!masksEmpty && !masksMatchGrid) {
        return;
    }
    searchCandidateCellMask_ = searchUpdate.visibleCellMask;
    searchSelectedCellMask_ = searchUpdate.selectedCellMask;
}

void TerminalPane::handleSearchUpdate(
    const TerminalSearchUpdate &searchUpdate)
{
    searchEngineActive_ = searchUpdate.active;
    const QString nextLabel = searchUpdate.active
        ? QStringLiteral("%1/%2")
              .arg(searchUpdate.selectedMatch >= 0
                       ? searchUpdate.selectedMatch + 1 : 0)
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
    update();
}

QSGNode *TerminalPane::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    auto *root = oldNode != nullptr
        ? static_cast<TerminalSceneNode *>(oldNode)
        : new TerminalSceneNode;
    root->clearTransientNodes();

    TerminalFrame frame;
    QVector<quint64> textRowEpochs;
    TerminalAppearance appearance;
    SplitAppearance splitAppearance;
    QFont baseFont;
    QString preedit;
    QString status;
    QString linkPreview;
    QRectF linkPreviewRect;
    QSet<int> hoveredHyperlinkCells;
    QBitArray searchCandidateCellMask;
    QBitArray searchSelectedCellMask;
    qreal cellWidth = 0.0;
    qreal cellHeight = 0.0;
    qreal baseline = 0.0;
    bool hasFrame = false;
    {
        QMutexLocker locker(&renderMutex_);
        frame = frame_;
        textRowEpochs = textRowEpochs_;
        appearance = appearance_;
        splitAppearance = splitAppearance_;
        baseFont = font_;
        preedit = preedit_;
        status = statusMessage_;
        linkPreview = linkPreviewText_;
        linkPreviewRect = linkPreviewRect_;
        if (hoveredHyperlinkColumns_ == frame_.columns
            && hoveredHyperlinkRows_ == frame_.rows) {
            hoveredHyperlinkCells = hoveredHyperlinkCellIndexes_;
        }
        if (searchDecorationRevision_ == frame_.contentRevision
            && searchDecorationColumns_ == frame_.columns
            && searchDecorationRows_ == frame_.rows) {
            searchCandidateCellMask = searchCandidateCellMask_;
            searchSelectedCellMask = searchSelectedCellMask_;
        }
        cellWidth = cellWidth_;
        cellHeight = cellHeight_;
        baseline = baseline_;
        hasFrame = hasFrame_;
    }
    const bool candidateMaskMatchesFrame =
        searchCandidateCellMask.size() == frame.cells.size();
    const bool selectedMaskMatchesFrame =
        searchSelectedCellMask.size() == frame.cells.size();

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

    QSGTextNode *overlayText = nullptr;
    const auto ensureOverlayText = [&] {
        if (overlayText == nullptr) {
            overlayText = createTextNode(
                window(), viewport, QColor(QStringLiteral("#eceff4")));
        }
        return overlayText;
    };

    if (!hasFrame || frame.columns <= 0 || frame.rows <= 0) {
        root->clearMainText();
        QSGTextNode *startingText = createTextNode(
            window(), viewport, QColor(QStringLiteral("#88909d")));
        appendTextLayout(startingText, QStringLiteral("Starting terminal…"), baseFont,
                         QColor(QStringLiteral("#88909d")), QPointF(12.0, 12.0),
                         baseline, std::max<qreal>(1.0, viewport.width() - 24.0));
        if (startingText != nullptr) {
            root->mainTextRows->appendChildNode(startingText);
        }
    } else {
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
        const qsizetype cursorCellIndex = cursorActive
            ? static_cast<qsizetype>(frame.cursorRow) * frame.columns
                + frame.cursorColumn
            : -1;
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

        TerminalTextRenderState textState;
        textState.window = window();
        textState.viewport = viewport;
        textState.font = baseFont;
        textState.appearance = appearance;
        textState.foreground = frame.foreground;
        textState.background = frame.background;
        textState.palette = frame.palette;
        textState.searchCandidateCells = searchCandidateCellMask;
        textState.searchSelectedCells = searchSelectedCellMask;
        textState.cellWidth = cellWidth;
        textState.cellHeight = cellHeight;
        textState.baseline = baseline;
        textState.devicePixelRatio = window() != nullptr
            ? window()->devicePixelRatio() : 1.0;
        textState.graphicsApi = rendererInterface != nullptr
            ? static_cast<int>(rendererInterface->graphicsApi()) : -1;
        textState.frameColumns = frame.columns;
        textState.frameRows = frame.rows;
        textState.visibleColumns = visibleColumns;
        textState.visibleRows = visibleRows;

        const bool rebuildAllText = !root->textState.has_value()
            || *root->textState != textState
            || root->rowTextNodes.size() != visibleRows;
        if (rebuildAllText) {
            root->resetTextRows(visibleRows, baseFont);
            root->textState = textState;
        }

        BlockCursorTextState blockCursorTextState;
        if (blockCursorActive) {
            blockCursorTextState = {
                .active = true,
                .row = frame.cursorRow,
                .column = frame.cursorColumn,
                .span = std::max(1, frame.cursorColumnSpan),
                .color = blockCursorTextColor,
            };
        }
        const BlockCursorTextState previousBlockCursorTextState =
            root->blockCursorTextState;
        const bool blockCursorTextChanged = !rebuildAllText
            && previousBlockCursorTextState != blockCursorTextState;

        for (int row = 0; row < visibleRows; ++row) {
            const qreal top = static_cast<qreal>(row) * cellHeight;
            const quint64 rowEpoch = row < textRowEpochs.size()
                ? textRowEpochs.at(row) : 0;
            const bool cursorChangedThisRow = blockCursorTextChanged
                && ((previousBlockCursorTextState.active
                     && previousBlockCursorTextState.row == row)
                    || (blockCursorTextState.active
                        && blockCursorTextState.row == row));
            const bool rebuildRowText = rebuildAllText
                || root->rowTextNodes.at(row) == nullptr
                || root->builtRowEpochs.at(row) != rowEpoch
                || cursorChangedThisRow;
            QSGTextNode *rowText = rebuildRowText
                ? root->prepareTextRow(row, window(), viewport,
                                       frame.foreground)
                : nullptr;
            for (int column = 0; column < visibleColumns; ++column) {
                const qsizetype index = static_cast<qsizetype>(row)
                    * frame.columns + column;
                if (index >= frame.cells.size()) {
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

                bool candidateSearchMatch = candidateMaskMatchesFrame
                    && searchCandidateCellMask.testBit(index);
                bool selectedSearchMatch = selectedMaskMatchesFrame
                    && searchSelectedCellMask.testBit(index);
                if (cell.spacer && column > 0) {
                    // libghostty maps every UTF-8 byte to the owning wide
                    // head. Carry that decoration into its spacer tail so the
                    // highlighted grapheme remains one visual unit.
                    candidateSearchMatch = candidateSearchMatch
                        || (candidateMaskMatchesFrame
                            && searchCandidateCellMask.testBit(index - 1));
                    selectedSearchMatch = selectedSearchMatch
                        || (selectedMaskMatchesFrame
                            && searchSelectedCellMask.testBit(index - 1));
                }

                QColor cellBackground = styledBackground;
                if (cell.selected) {
                    cellBackground = resolveRelativeColor(
                        appearance.selectionBackground,
                        styledForeground, styledBackground,
                        frame.foreground);
                } else if (selectedSearchMatch) {
                    cellBackground = resolveRelativeColor(
                        appearance.searchSelectedBackground,
                        styledForeground, styledBackground,
                        frame.foreground);
                } else if (candidateSearchMatch) {
                    cellBackground = resolveRelativeColor(
                        appearance.searchBackground,
                        styledForeground, styledBackground,
                        frame.foreground);
                }
                if (cellBackground != background) {
                    // Background is grid-cell state, even when the glyph in
                    // this cell spans multiple columns. Drawing one column at
                    // a time keeps adjacent/spacer backgrounds non-overlapping
                    // so they can be safely color-batched.
                    appendRect(backgrounds, QRectF(left, top, cellWidth, cellHeight),
                               cellBackground);
                }

                QColor foreground = styledForeground;
                if (cell.selected) {
                    foreground = resolveRelativeColor(
                        appearance.selectionForeground,
                        styledForeground, styledBackground,
                        frame.background);
                } else if (selectedSearchMatch) {
                    foreground = resolveRelativeColor(
                        appearance.searchSelectedForeground,
                        styledForeground, styledBackground,
                        frame.background);
                } else if (candidateSearchMatch) {
                    foreground = resolveRelativeColor(
                        appearance.searchForeground,
                        styledForeground, styledBackground,
                        frame.background);
                }
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

                if (rowText != nullptr && !cell.invisible
                    && !cell.text.isEmpty() && !cell.spacer) {
                    const size_t fontIndex = (cell.bold ? 1U : 0U)
                        | (cell.italic ? 2U : 0U);
                    appendTextLayout(rowText, cell.text,
                                     root->fonts[fontIndex], foreground,
                                     QPointF(left, top), baseline, drawWidth);
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
                if (index <= std::numeric_limits<int>::max()
                    && hoveredHyperlinkCells.contains(
                        static_cast<int>(index))) {
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
            if (rebuildRowText) {
                root->builtRowEpochs[row] = rowText != nullptr ? rowEpoch : 0;
            }
        }
        root->blockCursorTextState = blockCursorTextState;

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
            appendTextLayout(ensureOverlayText(), preedit, baseFont,
                             QColor(QStringLiteral("#eceff4")), QPointF(left, top),
                             baseline, preeditWidth);
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
            appendTextLayout(ensureOverlayText(), status, baseFont,
                             QColor(QStringLiteral("#e5c07b")),
                             QPointF(8.0, statusTop),
                             statusHeight - 6.0 - metrics.descent(),
                             std::max<qreal>(1.0, width() - 16.0));
        }

        if (!linkPreview.isEmpty() && !linkPreviewRect.isEmpty()) {
            QColor previewBackground = frame.background;
            previewBackground.setAlpha(242);
            QColor previewForeground = frame.foreground;
            previewForeground.setAlpha(255);
            QColor previewOutline = previewForeground;
            previewOutline.setAlpha(100);
            appendRect(overlayBackgrounds, linkPreviewRect,
                       previewBackground);

            constexpr qreal outlineWidth = 1.0;
            appendRect(overlayDecorations,
                       QRectF(linkPreviewRect.left(), linkPreviewRect.top(),
                              linkPreviewRect.width(), outlineWidth),
                       previewOutline);
            appendRect(overlayDecorations,
                       QRectF(linkPreviewRect.left(),
                              linkPreviewRect.bottom() - outlineWidth,
                              linkPreviewRect.width(), outlineWidth),
                       previewOutline);
            appendRect(overlayDecorations,
                       QRectF(linkPreviewRect.left(), linkPreviewRect.top(),
                              outlineWidth, linkPreviewRect.height()),
                       previewOutline);
            appendRect(overlayDecorations,
                       QRectF(linkPreviewRect.right() - outlineWidth,
                              linkPreviewRect.top(), outlineWidth,
                              linkPreviewRect.height()),
                       previewOutline);

            const QFontMetricsF metrics(baseFont);
            appendTextLayout(
                ensureOverlayText(), linkPreview, baseFont, previewForeground,
                QPointF(linkPreviewRect.left()
                            + kLinkPreviewHorizontalPadding,
                        linkPreviewRect.top()
                            + kLinkPreviewVerticalPadding),
                std::ceil(metrics.ascent()),
                std::max<qreal>(
                    1.0, linkPreviewRect.width()
                        - 2.0 * kLinkPreviewHorizontalPadding));
        }
    }

    if (QSGNode *node = createRectNode(baseBackgrounds, softwareRenderer)) {
        root->beforeMain->appendChildNode(node);
    }
    if (QSGNode *node = createRectNode(backgrounds, softwareRenderer)) {
        root->beforeMain->appendChildNode(node);
    }
    if (QSGNode *node = createRectNode(cursorBackgrounds, softwareRenderer)) {
        root->beforeMain->appendChildNode(node);
    }
    if (QSGNode *node = createRectNode(decorationsBeforeText, softwareRenderer)) {
        root->beforeMain->appendChildNode(node);
    }
    if (QSGNode *node = createRectNode(decorationsAfterText, softwareRenderer)) {
        root->afterMain->appendChildNode(node);
    }
    if (QSGNode *node = createRectNode(cursorDecorations, softwareRenderer)) {
        root->afterMain->appendChildNode(node);
    }
    if (QSGNode *node = createRectNode(scrollbarDecorations, softwareRenderer)) {
        root->afterMain->appendChildNode(node);
    }
    if (QSGNode *node = createRectNode(overlayBackgrounds, softwareRenderer)) {
        root->afterMain->appendChildNode(node);
    }
    if (overlayText != nullptr) {
        root->afterMain->appendChildNode(overlayText);
    }
    if (QSGNode *node = createRectNode(overlayDecorations, softwareRenderer)) {
        root->afterMain->appendChildNode(node);
    }

    QColor unfocusedSplitColor;
    const bool surfaceFocused = hasActiveFocus()
        && window() != nullptr && window()->isActive();
    if (split_ && !surfaceFocused && !searchUiActive_) {
        unfocusedSplitColor = splitAppearance.unfocusedFill.has_value()
                && splitAppearance.unfocusedFill->isValid()
            ? *splitAppearance.unfocusedFill
            : appearance.backgroundColor;
        if (unfocusedSplitColor.isValid()) {
            unfocusedSplitColor.setAlphaF(
                unfocusedSplitOverlayOpacity(
                    splitAppearance.unfocusedOpacity));
        }
    }
    root->setUnfocusedSplitOverlay(viewport, unfocusedSplitColor);
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
    publishRenderProbe(this, *root);
#endif
    return root;
}

void TerminalPane::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    const bool previewWasPointerCaptured = linkPreviewPointerCaptured_;
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        updateTerminalSize();
        refreshLinkPreview();
        reconcileReleasedLinkPreview(previewWasPointerCaptured);
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

void TerminalPane::syncCursorBlink(bool resetPhase)
{
    bool shouldBlink = false;
    {
        QMutexLocker locker(&renderMutex_);
        shouldBlink = hasFrame_ && frame_.cursorVisible
            && frame_.cursorBlinking;
    }
    shouldBlink = shouldBlink && hasActiveFocus();

    if (!shouldBlink) {
        cursorTimer_->stop();
        cursorBlinkOn_ = true;
    } else if (resetPhase || !cursorTimer_->isActive()) {
        cursorBlinkOn_ = true;
        cursorTimer_->start();
    }
    update();
}

void TerminalPane::updateTerminalSize()
{
    if (controller_ == nullptr) return;

    qreal cellWidth = 0.0;
    qreal cellHeight = 0.0;
    {
        QMutexLocker locker(&renderMutex_);
        cellWidth = cellWidth_;
        cellHeight = cellHeight_;
    }
    if (width() <= 0.0 || height() <= 0.0
        || cellWidth <= 0.0 || cellHeight <= 0.0
        || !std::isfinite(width()) || !std::isfinite(height())
        || !std::isfinite(cellWidth) || !std::isfinite(cellHeight)) {
        clearHyperlinkHover();
        cancelHyperlinkPress();
        return;
    }
    if (width() < cellWidth || height() < cellHeight) {
        clearHyperlinkHover();
        cancelHyperlinkPress();
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
                pasteText(text);
            }
            return KeyHandling::ConsumePressAndRelease;
        }
        case Qt::Key_T: Q_EMIT requestNewTab(); return KeyHandling::ConsumePressAndRelease;
        case Qt::Key_O: Q_EMIT requestSplit(WorkspaceAction::SplitRight); return KeyHandling::ConsumePressAndRelease;
        case Qt::Key_E: Q_EMIT requestSplit(WorkspaceAction::SplitDown); return KeyHandling::ConsumePressAndRelease;
        case Qt::Key_W: Q_EMIT requestCloseTab(); return KeyHandling::ConsumePressAndRelease;
        case Qt::Key_Q: Q_EMIT requestQuit(); return KeyHandling::ConsumePressAndRelease;
        case Qt::Key_F: startSearchUi(); return KeyHandling::ConsumePressAndRelease;
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
                return action == QLatin1StringView("ignore");
            });
        return ignored ? KeyHandling::ConsumePress
                       : KeyHandling::ConsumePressAndRelease;
    }

    bool performed = false;
    bool ignored = false;
    bool hasClosingAction = false;
    for (const QString &action : step.match.actions) {
        if (canExecuteConfiguredAction(action)) {
            const bool actionPerformed = executeConfiguredAction(action);
            performed = actionPerformed || performed;
            if (actionPerformed) {
                ignored = ignored
                    || action == QLatin1StringView("ignore");
                hasClosingAction = hasClosingAction
                    || action == QLatin1StringView("close_surface")
                    || action == QLatin1StringView("close_tab")
                    || action == QLatin1StringView("close_tab:this")
                    || action == QLatin1StringView("close_window");
            }
        }
    }
    // Ghostty executes the complete chain, then treats surface/tab/window
    // closure as terminal for this event. TerminalWorkspace uses deleteLater,
    // so chained actions remain safe until this callback returns. `quit` is
    // deliberately not a closing surface action in the pinned implementation.
    if (hasClosingAction) {
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
    const std::optional<GhosttyPaneAction> paneAction =
        GhosttyActionCatalog::parsePaneAction(action);
    if (paneAction.has_value()) {
        if (paneAction->kind == GhosttyPaneActionKind::KeyTable) {
            return canApplyKeyTableRequest(paneAction->keyTable);
        }
        const bool needsSelection =
            paneAction->kind == GhosttyPaneActionKind::AdjustSelection
            || paneAction->kind == GhosttyPaneActionKind::SearchSelection
            || (paneAction->kind == GhosttyPaneActionKind::ScrollViewport
                && paneAction->viewport.kind
                    == TerminalViewportRequest::Kind::Selection);
        if (needsSelection && !controller_->selectionExpected()) {
            return false;
        }
        if (paneAction->kind == GhosttyPaneActionKind::EndSearch) {
            // Ghostty always dispatches end_search so the GUI can clean up
            // stale UI, even though execution reports false without an
            // active engine.
            return true;
        }
        if (paneAction->kind == GhosttyPaneActionKind::NavigateSearch) {
            return searchEngineActive_ || controller_->searchExpected();
        }
        return true;
    }

    if (!GhosttyActionCatalog::isImplemented(action)) {
        return false;
    }
    const GhosttySerializedActionView parsed =
        GhosttyActionCatalog::parseSerializedAction(action);
    const QStringView name = parsed.name;

    if (name == QLatin1StringView("copy_to_clipboard")) {
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
        return !hoveredHyperlinkUri_.isEmpty()
            && hoveredHyperlinkCell_ == hoverCell_
            && hoveredCellIsLinked
            && hyperlinkModifiersMatch(hoverModifiers_);
    }
    if (name == QLatin1StringView("paste_from_clipboard")) {
        return !QGuiApplication::clipboard()->text().isEmpty();
    }
    if (name == QLatin1StringView("paste_from_selection")) {
        return !QGuiApplication::clipboard()->text(
            QClipboard::Selection).isEmpty();
    }
    if (name == QLatin1StringView("reload_config")
        || name == QLatin1StringView("close_window")
        || name == QLatin1StringView("end_key_sequence")
        || name == QLatin1StringView("ignore")) {
        return true;
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
    // Broad action fanout calls this public boundary without the normal
    // performability preflight. Apply the catalog grammar before any action
    // can mutate pane or application state.
    const std::optional<GhosttyPaneAction> paneAction =
        GhosttyActionCatalog::parsePaneAction(action);
    if (paneAction.has_value()) {
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
        case GhosttyPaneActionKind::FontSize:
            applyFontSizeRequest(paneAction->fontSize);
            return true;
        case GhosttyPaneActionKind::KeyTable:
            return applyKeyTableRequest(paneAction->keyTable);
        case GhosttyPaneActionKind::SelectAll:
            controller_->selectAll();
            return true;
        case GhosttyPaneActionKind::AdjustSelection:
            if (!controller_->selectionExpected()) return false;
            controller_->adjustSelection(paneAction->selectionAdjustment);
            return true;
        case GhosttyPaneActionKind::StartSearch:
            startSearchUi();
            return true;
        case GhosttyPaneActionKind::EndSearch: {
            const bool performed = searchEngineActive_
                || controller_->searchExpected();
            endSearchUi();
            return performed;
        }
        case GhosttyPaneActionKind::SearchSelection:
            if (!controller_->selectionExpected()) return false;
            controller_->requestSearchSelection();
            return true;
        case GhosttyPaneActionKind::Search: {
            // Binding.Action.format serializes arbitrary bytes with Zig
            // escapes. Preserve the payload until the worker decodes it.
            const bool hadSearch = searchEngineActive_
                || controller_->searchExpected();
            controller_->searchSerialized(paneAction->payload.toUtf8());
            return !paneAction->payload.isEmpty() || hadSearch;
        }
        case GhosttyPaneActionKind::NavigateSearch:
            if (!controller_->searchExpected()) return false;
            controller_->navigateSearch(paneAction->searchDirection);
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
        case GhosttyPaneActionKind::ToggleReadOnly:
            controller_->setReadOnly(!controller_->readOnly());
            return true;
        }
    }

    if (!GhosttyActionCatalog::isImplemented(action)) {
        return false;
    }
    const GhosttySerializedActionView parsed =
        GhosttyActionCatalog::parseSerializedAction(action);
    const QStringView name = parsed.name;

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
        pasteText(text);
        return true;
    }
    if (name == QLatin1StringView("paste_from_selection")) {
        const QString text =
            QGuiApplication::clipboard()->text(QClipboard::Selection);
        if (text.isEmpty()) return false;
        pasteText(text);
        return true;
    }
    if (name == QLatin1StringView("reload_config")) {
        Q_EMIT requestConfigReload();
        return true;
    }
    if (name == QLatin1StringView("end_key_sequence")) {
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
    WorkspaceActionRequest request = *translated.request;
    if (request.action == WorkspaceAction::SplitAuto) {
        const qreal devicePixelRatio = window() != nullptr
            ? window()->devicePixelRatio()
            : 1.0;
        const int surfaceWidth = std::max(
            1, qRound(width() * devicePixelRatio));
        const int surfaceHeight = std::max(
            1, qRound(height() * devicePixelRatio));
        request.action = surfaceWidth > surfaceHeight
            ? WorkspaceAction::SplitRight
            : WorkspaceAction::SplitDown;
    }
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
    case WorkspaceAction::RequestQuit:
        Q_EMIT requestQuit();
        return true;
    case WorkspaceAction::ActivateTab:
    case WorkspaceAction::ActivatePane:
    case WorkspaceAction::NavigatePaneRelative:
    case WorkspaceAction::ActivateTabByIndex:
    case WorkspaceAction::ActivateLastTab:
    case WorkspaceAction::MoveTab:
    case WorkspaceAction::PromptTabTitle:
    case WorkspaceAction::SetTabTitle:
    case WorkspaceAction::ResizeSplit:
    case WorkspaceAction::EqualizeSplits:
    case WorkspaceAction::ToggleSplitZoom:
    case WorkspaceAction::ToggleFullscreen:
        return false;
    }
    return false;
}

void TerminalPane::inputMethodEvent(QInputMethodEvent *event)
{
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
        controller_->sendInputMethod(input);
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
        QClipboard *const clipboard = QGuiApplication::clipboard();
        if (options_.middleClickAction == MiddleClickAction::PrimaryPaste
            && clipboard != nullptr) {
            const QString text = readMiddleClickClipboard(
                clipboard, options_.selectionClipboard.copyOnSelect);
            if (!text.isEmpty()) {
                pasteText(text);
            }
        }
    }
    event->accept();
}

void TerminalPane::mouseDoubleClickEvent(QMouseEvent *event)
{
    forceActiveFocus(Qt::MouseFocusReason);
    Q_EMIT activated(this);
    updateHyperlinkHover(event->position(), event->modifiers());
    if (linkPreviewPointerCaptured_) {
        event->accept();
        return;
    }
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
    if (linkPreviewPointerCaptured_ && event->buttons() == Qt::NoButton) {
        event->accept();
        return;
    }
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
    if (!linkPreviewPointerCaptured_ && controller_->mouseTracking()
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
    refreshHyperlinkHover();
    refreshLinkPreview();
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

void TerminalPane::refreshLinkPreview()
{
    const bool modeAllowsPreview = options_.linkPreviews
            == LinkPreviewMode::Always
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
            font = font_;
        }
        const QFontMetricsF metrics(font);
        const qreal maximumTextWidth = std::max<qreal>(
            1.0, width() - 2.0 * kLinkPreviewHorizontalPadding);
        text = metrics.elidedText(
            linkPreviewDisplaySource(hoveredHyperlinkUri_),
            Qt::ElideMiddle, maximumTextWidth);
        if (!text.isEmpty()) {
            const qreal previewWidth = std::min(
                width(), std::ceil(metrics.horizontalAdvance(text))
                    + 2.0 * kLinkPreviewHorizontalPadding);
            const qreal previewHeight = std::min(
                height(), std::ceil(metrics.height())
                    + 2.0 * kLinkPreviewVerticalPadding);
            guardRect = QRectF(
                0.0, std::max(0.0, height() - previewHeight),
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
        changed = linkPreviewText_ != text
            || linkPreviewRect_ != previewRect;
        linkPreviewText_ = std::move(text);
        linkPreviewRect_ = previewRect;
        linkPreviewGuardRect_ = guardRect;
    }
    linkPreviewPointerCaptured_ = pointerCaptured;
    if (changed) {
        Q_EMIT linkPreviewChanged();
        update();
    }
}

void TerminalPane::reconcileReleasedLinkPreview(
    bool wasPointerCaptured, bool forceRequery)
{
    if (!wasPointerCaptured || linkPreviewPointerCaptured_ || !hoverInside_) {
        return;
    }
    const QPoint physicalCell = hoverCellAt(hoverPosition_)
        .value_or(QPoint(-1, -1));
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
        hadHighlight = !hoveredHyperlinkCellIndexes_.isEmpty();
        hoveredHyperlinkCellIndexes_.clear();
        hoveredHyperlinkColumns_ = 0;
        hoveredHyperlinkRows_ = 0;
        previewChanged = !linkPreviewText_.isEmpty()
            || !linkPreviewRect_.isEmpty();
        linkPreviewText_.clear();
        linkPreviewRect_ = {};
        linkPreviewGuardRect_ = {};
    }
    linkPreviewPointerCaptured_ = false;
    unsetCursor();
    if (previewChanged) {
        Q_EMIT linkPreviewChanged();
    }
    if (hadHighlight || previewChanged) {
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
    const bool previewWasPointerCaptured = linkPreviewPointerCaptured_;
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
    refreshLinkPreview();
    reconcileReleasedLinkPreview(previewWasPointerCaptured);
    if (!hyperlinkLeaseActive_) {
        return;
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
    syncCursorBlink(true);
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

void TerminalPane::setFontPointSize(qreal points)
{
    const bool previewWasPointerCaptured = linkPreviewPointerCaptured_;
    {
        QMutexLocker locker(&renderMutex_);
        if (qFuzzyCompare(font_.pointSizeF(), points)) {
            return;
        }
        font_.setPointSizeF(points);
    }
    updateMetrics();
    updateTerminalSize();
    refreshLinkPreview();
    reconcileReleasedLinkPreview(previewWasPointerCaptured);
    update();
    Q_EMIT fontPointSizeChanged();
}

void TerminalPane::zoomIn()
{
    applyFontSizeRequest({TerminalFontSizeRequest::Kind::Increase, 1.0F});
}

void TerminalPane::zoomOut()
{
    applyFontSizeRequest({TerminalFontSizeRequest::Kind::Decrease, 1.0F});
}

void TerminalPane::resetZoom()
{
    applyFontSizeRequest({TerminalFontSizeRequest::Kind::Reset, 0.0F});
}

void TerminalPane::applyFontSizeRequest(
    const TerminalFontSizeRequest &request)
{
    if (request.kind == TerminalFontSizeRequest::Kind::Reset) {
        manuallyZoomed_ = false;
        setFontPointSize(defaultFontPointSize_);
        return;
    }

    const float current = static_cast<float>(fontPointSize());
    float points = current;
    switch (request.kind) {
    case TerminalFontSizeRequest::Kind::Increase: {
        const float delta = clampFontActionValue(request.points, 0.0F);
        points = std::fmin(current + delta, kMaximumActionFontSize);
        break;
    }
    case TerminalFontSizeRequest::Kind::Decrease: {
        const float delta = clampFontActionValue(request.points, 0.0F);
        points = std::fmax(kMinimumActionFontSize, current - delta);
        break;
    }
    case TerminalFontSizeRequest::Kind::Set:
        points = clampFontActionValue(request.points,
                                      kMinimumActionFontSize);
        break;
    case TerminalFontSizeRequest::Kind::Reset:
        Q_UNREACHABLE();
    }

    manuallyZoomed_ = true;
    setFontPointSize(points);
}

bool TerminalPane::canApplyKeyTableRequest(
    const TerminalKeyTableRequest &request) const
{
    switch (request.kind) {
    case TerminalKeyTableRequest::Kind::Activate:
    case TerminalKeyTableRequest::Kind::ActivateOnce:
        return keybinds_.canActivateTable(request.name);
    case TerminalKeyTableRequest::Kind::Deactivate:
    case TerminalKeyTableRequest::Kind::DeactivateAll:
        return keybinds_.hasActiveTables();
    }
    return false;
}

bool TerminalPane::applyKeyTableRequest(
    const TerminalKeyTableRequest &request)
{
    bool changed = false;
    switch (request.kind) {
    case TerminalKeyTableRequest::Kind::Activate:
        changed = keybinds_.activateTable(request.name);
        break;
    case TerminalKeyTableRequest::Kind::ActivateOnce:
        changed = keybinds_.activateTable(request.name, true);
        break;
    case TerminalKeyTableRequest::Kind::Deactivate:
        changed = keybinds_.deactivateTable();
        break;
    case TerminalKeyTableRequest::Kind::DeactivateAll:
        changed = keybinds_.deactivateAllTables();
        break;
    }

    if (changed) {
        Q_EMIT activeKeyTablesChanged();
    }
    return changed;
}
