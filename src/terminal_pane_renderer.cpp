#include "terminal_pane_renderer.h"

#include "terminal_pane.h"

#include "terminal_backdrop_qsg.h"
#include "terminal_kitty_graphics_qsg.h"
#include "terminal_pane_render_probe_p.h"
#include "terminal_rect_batch.h"
#include "terminal_text_runs.h"

#include <QBitArray>
#include <QFontMetricsF>
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
#include <QHash>
#endif
#include <QMatrix4x4>
#include <QMutexLocker>
#include <QQuickWindow>
#include <QSGSimpleRectNode>
#include <QSGTextNode>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextOption>
#include <QVarLengthArray>

#include <algorithm>
#include <array>
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
#include <atomic>
#endif
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <ranges>
#include <span>
#include <utility>

class TerminalPaneRenderItem final : public QQuickItem {
public:
    explicit TerminalPaneRenderItem(TerminalPane *pane)
        : QQuickItem(pane)
        , pane_(pane)
    {
        setFlag(QQuickItem::ItemHasContents, true);
        setAcceptedMouseButtons(Qt::NoButton);
        setZ(-1'000.0);
    }

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode,
                             UpdatePaintNodeData *updateData) override
    {
        return pane_ != nullptr
            ? pane_->updateTerminalPaintNode(oldNode, updateData)
            : oldNode;
    }

private:
    TerminalPane *pane_ = nullptr;
};

qreal TerminalPaneRenderer::normalizedDevicePixelRatio(qreal value) noexcept
{
    return std::isfinite(value) && value > 0.0 ? value : 1.0;
}

qint64 TerminalPaneRenderer::physicalPixels(qreal logicalPixels,
                                            qreal devicePixelRatio) noexcept
{
    if (!std::isfinite(logicalPixels) || logicalPixels <= 0.0) {
        return 0;
    }

    const qreal physical =
        logicalPixels * normalizedDevicePixelRatio(devicePixelRatio);
    if (physical >= static_cast<qreal>(std::numeric_limits<qint64>::max())) {
        return std::numeric_limits<qint64>::max();
    }
    return std::max<qint64>(0, qRound64(physical));
}

QQuickItem *TerminalPaneRenderer::createRenderItem(TerminalPane *pane)
{
    return pane != nullptr ? new TerminalPaneRenderItem(pane) : nullptr;
}

namespace {

using TerminalPaneRenderer::normalizedDevicePixelRatio;
using TerminalPaneRenderer::physicalPixels;

float unfocusedSplitOverlayOpacity(double paneOpacity)
{
    if (!std::isfinite(paneOpacity)) {
        paneOpacity = SplitAppearance{}.unfocusedOpacity;
    }
    paneOpacity = std::clamp(paneOpacity, 0.15, 1.0);
    // Pinned GTK writes the complementary opacity into runtime CSS with two
    // decimals. Preserve that observable composition rather than treating
    // this setting as the rectangle's opacity directly.
    return static_cast<float>(std::round((1.0 - paneOpacity) * 100.0) / 100.0);
}

TerminalFontRole terminalFontRole(bool bold, bool italic) noexcept
{
    if (bold) {
        return italic ? TerminalFontRole::BoldItalic : TerminalFontRole::Bold;
    }
    return italic ? TerminalFontRole::Italic : TerminalFontRole::Regular;
}

using ColoredRect = TerminalColoredRect;

void appendRect(QVector<ColoredRect> &rects, const QRectF &rect,
                const QColor &color)
{
    if (!rect.isEmpty() && color.isValid() && color.alpha() > 0) {
        rects.append({rect, color});
    }
}

void appendHorizontalRect(QVector<ColoredRect> &rects, const QRectF &rect,
                          const QColor &color)
{
    if (rect.isEmpty() || !color.isValid() || color.alpha() == 0) return;
    if (!rects.isEmpty()) {
        ColoredRect &previous = rects.last();
        const bool adjacent =
            qFuzzyCompare(previous.rect.x() + previous.rect.width(), rect.x());
        if (previous.color == color && adjacent
            && qFuzzyCompare(previous.rect.y(), rect.y())
            && qFuzzyCompare(previous.rect.height(), rect.height())) {
            previous.rect.setWidth(previous.rect.width() + rect.width());
            return;
        }
    }
    rects.append({rect, color});
}

[[nodiscard]] qreal logicalPixels(double physicalPixels,
                                  qreal devicePixelRatio) noexcept
{
    return physicalPixels
        / TerminalPaneRenderer::normalizedDevicePixelRatio(devicePixelRatio);
}

[[nodiscard]] QRectF paddedSpriteCanvas(const QRectF &spriteRect,
                                        qreal devicePixelRatio)
{
    const qint64 physicalWidth = TerminalPaneRenderer::physicalPixels(
        spriteRect.width(), devicePixelRatio);
    const qint64 physicalHeight = TerminalPaneRenderer::physicalPixels(
        spriteRect.height(), devicePixelRatio);
    const qreal horizontalPadding =
        logicalPixels(static_cast<double>(physicalWidth / 4), devicePixelRatio);
    const qreal verticalPadding = logicalPixels(
        static_cast<double>(physicalHeight / 4), devicePixelRatio);
    return spriteRect.adjusted(-horizontalPadding, -verticalPadding,
                               horizontalPadding, verticalPadding);
}

[[nodiscard]] QRectF clippedToCanvas(const QRectF &rect, const QRectF &canvas)
{
    return rect.intersected(canvas);
}

void appendClippedRect(QVector<ColoredRect> &rects, const QRectF &rect,
                       const QRectF &canvas, const QColor &color,
                       QVector<QRectF> *geometry = nullptr)
{
    const QRectF clipped = clippedToCanvas(rect, canvas);
    if (clipped.isEmpty()) {
        return;
    }

    appendRect(rects, clipped, color);
    if (geometry != nullptr) {
        geometry->append(clipped);
    }
}

QColor withOpacity(QColor color, double opacity)
{
    if (!color.isValid()) {
        return color;
    }
    color.setAlpha(std::clamp(
        static_cast<int>(std::ceil(std::clamp(opacity, 0.0, 1.0) * 255.0)), 0,
        255));
    return color;
}

double normalizedBackgroundOpacity(double opacity)
{
    return std::isfinite(opacity) ? std::clamp(opacity, 0.0, 1.0) : 1.0;
}

int roundedOpacityByte(double opacity)
{
    return std::clamp(static_cast<int>(std::lround(
                          normalizedBackgroundOpacity(opacity) * 255.0)),
                      0, 255);
}

int truncatedOpacityByte(double opacity)
{
    return std::clamp(
        static_cast<int>(normalizedBackgroundOpacity(opacity) * 255.0), 0, 255);
}

QColor withAlpha(QColor color, int alpha)
{
    if (color.isValid()) {
        color.setAlpha(std::clamp(alpha, 0, 255));
    }
    return color;
}

QColor cellBackgroundLayer(QColor resolved, bool explicitBackground,
                           bool forceOpaque, int explicitBackgroundAlpha)
{
    if (forceOpaque) {
        return withAlpha(resolved, 255);
    }
    if (!explicitBackground) {
        return withAlpha(resolved, 0);
    }
    return withAlpha(resolved, explicitBackgroundAlpha);
}

struct LinearPremultipliedColor {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    float alpha = 0.0F;
};

float linearizeSrgb(float component)
{
    return component <= 0.04045F
        ? component / 12.92F
        : std::pow((component + 0.055F) / 1.055F, 2.4F);
}

LinearPremultipliedColor loadLinearColor(const QColor &color)
{
    const float alpha = static_cast<float>(color.alpha()) / 255.0F;
    return {
        .red = linearizeSrgb(static_cast<float>(color.red()) / 255.0F) * alpha,
        .green =
            linearizeSrgb(static_cast<float>(color.green()) / 255.0F) * alpha,
        .blue =
            linearizeSrgb(static_cast<float>(color.blue()) / 255.0F) * alpha,
        .alpha = alpha,
    };
}

float colorLuminance(const LinearPremultipliedColor &color)
{
    return 0.2126F * color.red + 0.7152F * color.green + 0.0722F * color.blue;
}

float colorContrast(const LinearPremultipliedColor &left,
                    const LinearPremultipliedColor &right)
{
    const float leftLuminance = colorLuminance(left) + 0.05F;
    const float rightLuminance = colorLuminance(right) + 0.05F;
    return std::max(leftLuminance, rightLuminance)
        / std::min(leftLuminance, rightLuminance);
}

QColor minimumContrastColor(const QColor &foreground,
                            const QColor &cellBackground,
                            const QColor &globalBackground,
                            double minimumContrast)
{
    const float threshold = static_cast<float>(minimumContrast);
    if (threshold <= 1.0F || !foreground.isValid() || !cellBackground.isValid()
        || !globalBackground.isValid()) {
        return foreground;
    }

    const LinearPremultipliedColor foregroundLinear =
        loadLinearColor(foreground);
    const LinearPremultipliedColor cellBackgroundLinear =
        loadLinearColor(cellBackground);
    const LinearPremultipliedColor globalBackgroundLinear =
        loadLinearColor(globalBackground);
    const float uncovered = 1.0F - cellBackgroundLinear.alpha;
    const LinearPremultipliedColor effectiveBackground{
        .red =
            cellBackgroundLinear.red + globalBackgroundLinear.red * uncovered,
        .green = cellBackgroundLinear.green
            + globalBackgroundLinear.green * uncovered,
        .blue =
            cellBackgroundLinear.blue + globalBackgroundLinear.blue * uncovered,
        .alpha = cellBackgroundLinear.alpha
            + globalBackgroundLinear.alpha * uncovered,
    };

    if (colorContrast(foregroundLinear, effectiveBackground) >= threshold) {
        return foreground;
    }

    const LinearPremultipliedColor white = loadLinearColor(Qt::white);
    const LinearPremultipliedColor black = loadLinearColor(Qt::black);
    return colorContrast(white, effectiveBackground)
            > colorContrast(black, effectiveBackground)
        ? QColor(Qt::white)
        : QColor(Qt::black);
}

QColor resolveRelativeColor(const TerminalColorValue &configured,
                            const QColor &cellForeground,
                            const QColor &cellBackground,
                            const QColor &fallback)
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

void applyBoldColor(const TerminalCell &cell, const TerminalFrame &frame,
                    const TerminalAppearance &appearance, QColor *foreground,
                    QColor *background)
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
            const QColor bright =
                frame.palette.at(cell.styleForegroundPaletteIndex + 8);
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
                     qreal position, qreal thickness,
                     TerminalUnderlineStyle style, const QColor &color,
                     qreal devicePixelRatio,
                     QVector<QRectF> *geometry = nullptr)
{
    if (style == TerminalUnderlineStyle::None) {
        return;
    }

    const qreal dpr = normalizedDevicePixelRatio(devicePixelRatio);
    const qint64 physicalWidth = physicalPixels(cellRect.width(), dpr);
    const qint64 physicalHeight = physicalPixels(cellRect.height(), dpr);
    const qint64 physicalPosition = physicalPixels(position, dpr);
    const qint64 physicalThickness = physicalPixels(thickness, dpr);
    if (physicalWidth <= 0 || physicalHeight <= 0 || physicalThickness <= 0) {
        return;
    }

    const qint64 physicalPadding = physicalHeight / 4;
    const qint64 physicalCanvasBottom =
        std::min(static_cast<qint64>(std::numeric_limits<quint32>::max()),
                 physicalHeight + physicalPadding);
    const QRectF canvas = paddedSpriteCanvas(cellRect, dpr);
    const auto saturatingSubtract = [](qint64 left, qint64 right) {
        return left > right ? left - right : qint64{0};
    };
    const auto localRect = [&](double x, double y, double width,
                               double height) {
        return QRectF(cellRect.left() + logicalPixels(x, dpr),
                      cellRect.top() + logicalPixels(y, dpr),
                      logicalPixels(width, dpr), logicalPixels(height, dpr));
    };
    const auto appendLine = [&](const QRectF &rect) {
        appendClippedRect(rects, rect, canvas, color, geometry);
    };
    const qint64 singlePosition =
        std::min(physicalPosition,
                 saturatingSubtract(physicalCanvasBottom, physicalThickness));

    switch (style) {
    case TerminalUnderlineStyle::None: break;
    case TerminalUnderlineStyle::Single:
        appendLine(localRect(0.0, static_cast<double>(singlePosition),
                             static_cast<double>(physicalWidth),
                             static_cast<double>(physicalThickness)));
        break;
    case TerminalUnderlineStyle::Double: {
        const qint64 twiceThickness =
            std::min(std::numeric_limits<qint64>::max(),
                     physicalThickness > std::numeric_limits<qint64>::max() / 2
                         ? std::numeric_limits<qint64>::max()
                         : physicalThickness * 2);
        const qint64 middle =
            std::min(physicalPosition,
                     saturatingSubtract(physicalCanvasBottom, twiceThickness));
        const qint64 first = saturatingSubtract(middle, physicalThickness);
        const qint64 second =
            middle > std::numeric_limits<qint64>::max() - physicalThickness
            ? std::numeric_limits<qint64>::max()
            : middle + physicalThickness;
        appendLine(localRect(0.0, static_cast<double>(first),
                             static_cast<double>(physicalWidth),
                             static_cast<double>(physicalThickness)));
        appendLine(localRect(0.0, static_cast<double>(second),
                             static_cast<double>(physicalWidth),
                             static_cast<double>(physicalThickness)));
        break;
    }
    case TerminalUnderlineStyle::Curly: {
        const double width = static_cast<double>(physicalWidth);
        const double lineThickness = static_cast<double>(physicalThickness);
        const double amplitude = width / std::numbers::pi;
        const double top = std::min(static_cast<double>(physicalPosition),
                                    static_cast<double>(physicalCanvasBottom)
                                        - amplitude - lineThickness);
        const double bottom = top + amplitude;
        const double center = width / 2.0;
        constexpr double curvature = 0.4;
        const auto cubic = [](double start, double control1, double control2,
                              double end, double t) {
            const double inverse = 1.0 - t;
            return inverse * inverse * inverse * start
                + 3.0 * inverse * inverse * t * control1
                + 3.0 * inverse * t * t * control2 + t * t * t * end;
        };
        const auto firstHalfY = [&](double x) {
            const double target = x / center;
            double t = target;
            for (int iteration = 0; iteration < 6; ++iteration) {
                const double inverse = 1.0 - t;
                const double candidate =
                    cubic(0.0, curvature, 1.0 - curvature, 1.0, t);
                const double derivative = 3.0 * inverse * inverse * curvature
                    + 6.0 * inverse * t * (1.0 - 2.0 * curvature)
                    + 3.0 * t * t * curvature;
                t = std::clamp(t - (candidate - target) / derivative, 0.0, 1.0);
            }
            return cubic(bottom, bottom, top, top, t);
        };

        // The pinned sprite is one cubic cycle per cell. Sampling at physical
        // pixel centers keeps that wavelength independent of line thickness,
        // and disjoint one-pixel columns apply opacity only once.
        for (qint64 x = 0; x < physicalWidth; ++x) {
            const double centerX = static_cast<double>(x) + 0.5;
            const double firstHalfX = std::min(centerX, width - centerX);
            const double centerY = firstHalfY(firstHalfX);
            appendLine(localRect(static_cast<double>(x),
                                 centerY - lineThickness / 2.0, 1.0,
                                 lineThickness));
        }
        break;
    }
    case TerminalUnderlineStyle::Dotted: {
        const double width = static_cast<double>(physicalWidth);
        const double lineThickness = static_cast<double>(physicalThickness);
        const double radius = std::numbers::sqrt2 / 2.0 * lineThickness;
        const double centerY = std::min(
            static_cast<double>(physicalPosition) + 0.5 * lineThickness,
            static_cast<double>(physicalCanvasBottom) - std::ceil(radius));
        const qint64 dotCount = static_cast<qint64>(
            std::max(std::min({
                         std::ceil(width / (4.0 * radius)),
                         std::floor(width / (3.0 * radius)),
                         std::floor(width / (2.0 * radius + 1.0)),
                     }),
                     1.0));
        const double stride = width / static_cast<double>(dotCount);
        for (qint64 dot = 0; dot < dotCount; ++dot) {
            const double centerX = (static_cast<double>(dot) + 0.5) * stride;
            const qint64 firstY = std::max<qint64>(
                -physicalPadding,
                static_cast<qint64>(std::floor(centerY - radius)));
            const qint64 pastLastY = std::min<qint64>(
                physicalCanvasBottom,
                static_cast<qint64>(std::ceil(centerY + radius)));
            for (qint64 y = firstY; y < pastLastY; ++y) {
                const double distanceY = static_cast<double>(y) + 0.5 - centerY;
                const double remaining =
                    radius * radius - distanceY * distanceY;
                if (remaining < 0.0) {
                    continue;
                }
                const double halfWidth = std::sqrt(remaining);
                const qint64 firstX =
                    static_cast<qint64>(std::ceil(centerX - halfWidth - 0.5));
                const qint64 lastX =
                    static_cast<qint64>(std::floor(centerX + halfWidth - 0.5));
                if (lastX >= firstX) {
                    appendLine(localRect(
                        static_cast<double>(firstX), static_cast<double>(y),
                        static_cast<double>(lastX - firstX + 1), 1.0));
                }
            }
        }
        break;
    }
    case TerminalUnderlineStyle::Dashed: {
        const qint64 dashWidth = physicalWidth / 3 + 1;
        const qint64 dashCount = physicalWidth / dashWidth + 1;
        for (qint64 index = 0; index < dashCount; index += 2) {
            appendLine(localRect(static_cast<double>(index * dashWidth),
                                 static_cast<double>(singlePosition),
                                 static_cast<double>(dashWidth),
                                 static_cast<double>(physicalThickness)));
        }
        break;
    }
    }
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
                      qreal baseline, qreal lineWidth,
                      Qt::LayoutDirection direction = Qt::LayoutDirectionAuto)
{
    if (node == nullptr || text.isEmpty()) {
        return;
    }

    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setFlags(QTextOption::IncludeTrailingSpaces);
    option.setTextDirection(direction);
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

struct TerminalRunLayoutResult {
    quint64 layoutCount = 0;
    quint64 fallbackCellCount = 0;
};

[[nodiscard]] QFont gridAlignedRunFont(const TerminalTextRun &run,
                                       qreal cellWidth)
{
    if (!run.font.fixedPitch() || run.boundaries.isEmpty()
        || !std::isfinite(cellWidth) || cellWidth <= 0.0) {
        return run.font;
    }

    const QFontMetricsF metrics(run.font);
    qreal commonAdvance = -1.0;
    int previousTextPosition = 0;
    int previousColumn = 0;
    for (const TerminalTextBoundary &boundary : run.boundaries) {
        const int textLength = boundary.textPosition - previousTextPosition;
        const int columnSpan = boundary.column - previousColumn;
        // A constant spacing correction is safe only when one UTF-16 code
        // unit represents one grid cell. Graphemes and wide cells retain the
        // configured font unchanged and rely on validation/fallback.
        if (textLength != 1 || columnSpan != 1) {
            return run.font;
        }
        const qreal advance =
            metrics.horizontalAdvance(run.text.at(previousTextPosition));
        if (!std::isfinite(advance) || advance <= 0.0
            || (commonAdvance > 0.0
                && !qFuzzyCompare(commonAdvance, advance))) {
            return run.font;
        }
        commonAdvance = advance;
        previousTextPosition = boundary.textPosition;
        previousColumn = boundary.column;
    }

    QFont aligned = run.font;
    aligned.setLetterSpacing(QFont::AbsoluteSpacing, cellWidth - commonAdvance);
    return aligned;
}

[[nodiscard]] bool terminalRunFitsGrid(const QTextLine &line,
                                       const TerminalTextRun &run,
                                       qreal cellWidth, qreal devicePixelRatio)
{
    const qreal dpr = normalizedDevicePixelRatio(devicePixelRatio);
    return std::ranges::all_of(
        run.boundaries, [&](const TerminalTextBoundary &boundary) {
            const qint64 actual =
                qRound64(line.cursorToX(boundary.textPosition) * dpr);
            const qint64 expected =
                qRound64(static_cast<qreal>(boundary.column) * cellWidth * dpr);
            return actual == expected;
        });
}

TerminalRunLayoutResult
appendTerminalTextRun(QSGTextNode *node, const TerminalTextRun &run, qreal top,
                      qreal baseline, qreal cellWidth, qreal devicePixelRatio)
{
    if (node == nullptr || run.text.isEmpty()) {
        return {};
    }

    QTextLayout layout(run.text, gridAlignedRunFont(run, cellWidth));
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setFlags(QTextOption::IncludeTrailingSpaces);
    // Terminal rows are physical left-to-right grids even when individual
    // codepoints have a right-to-left bidi class.
    option.setTextDirection(Qt::LeftToRight);
    layout.setTextOption(option);

    QTextLayout::FormatRange range;
    range.start = 0;
    range.length = static_cast<int>(run.text.size());
    range.format.setForeground(run.color);
    layout.setFormats({range});

    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid()) {
        line.setLineWidth(std::max<qreal>(1.0, run.columnSpan * cellWidth));
        line.setPosition(QPointF(0.0, baseline - line.ascent()));
    }
    layout.endLayout();
    if (line.isValid()
        && terminalRunFitsGrid(line, run, cellWidth, devicePixelRatio)) {
        node->addTextLayout(
            QPointF(static_cast<qreal>(run.column) * cellWidth, top), &layout);
        return {.layoutCount = 1};
    }

    TerminalRunLayoutResult result;
    result.layoutCount = static_cast<quint64>(run.fallbackCells.size());
    result.fallbackCellCount = result.layoutCount;
    for (const TerminalTextFallbackCell &cell : run.fallbackCells) {
        appendTextLayout(
            node, cell.text, run.font, run.color,
            QPointF(static_cast<qreal>(cell.column) * cellWidth, top), baseline,
            static_cast<qreal>(cell.columnSpan) * cellWidth, Qt::LeftToRight);
    }
    return result;
}

void clearNodeChildren(QSGNode *parent)
{
    while (QSGNode *child = parent->firstChild()) {
        parent->removeChildNode(child);
        delete child;
    }
}

struct TextLayoutSpec {
    QString text;
    QFont font;
    QColor color;
    QPointF position;
    qreal baseline = 0.0;
    qreal lineWidth = 1.0;

    bool operator==(const TextLayoutSpec &) const = default;
};

// A pane currently renders at most one grid overlay (IME preedit) and two
// pane overlays (status and link preview). Fixed-capacity state avoids a
// per-frame container allocation while making unchanged text layouts cheap to
// identify and retain.
struct OverlayTextRenderState {
    OverlayTextRenderState(QQuickWindow *renderWindow,
                           const QRectF &renderViewport, QColor renderColor)
        : window(renderWindow)
        , viewport(renderViewport)
        , defaultColor(std::move(renderColor))
    {}

    QQuickWindow *window = nullptr;
    QRectF viewport;
    QColor defaultColor;
    QVarLengthArray<TextLayoutSpec, 2> layouts;

    void append(TextLayoutSpec layout)
    {
        layouts.append(std::move(layout));
    }

    [[nodiscard]] std::span<const TextLayoutSpec> activeLayouts() const
    {
        return {layouts.constData(), static_cast<std::size_t>(layouts.size())};
    }

    bool operator==(const OverlayTextRenderState &) const = default;
};

struct TerminalGlyphStyle {
    TerminalColorValue selectionForeground;
    TerminalColorValue selectionBackground;
    TerminalColorValue searchForeground;
    TerminalColorValue searchBackground;
    TerminalColorValue searchSelectedForeground;
    TerminalColorValue searchSelectedBackground;
    TerminalBoldColor boldColor;
    double faintOpacity = 0.5;
    double minimumContrast = 1.0;

    static TerminalGlyphStyle
    fromAppearance(const TerminalAppearance &appearance)
    {
        return {
            .selectionForeground = appearance.selectionForeground,
            .selectionBackground = appearance.selectionBackground,
            .searchForeground = appearance.searchForeground,
            .searchBackground = appearance.searchBackground,
            .searchSelectedForeground =
                appearance.searchSelectedForeground,
            .searchSelectedBackground =
                appearance.searchSelectedBackground,
            .boldColor = appearance.boldColor,
            .faintOpacity = appearance.faintOpacity,
            .minimumContrast = appearance.minimumContrast,
        };
    }

    bool operator==(const TerminalGlyphStyle &) const = default;
};

struct TerminalTextRenderState {
    QQuickWindow *window = nullptr;
    std::shared_ptr<const TerminalFontProgram> fontProgram;
    bool shapingBreakCursor = true;
    qreal cellWidth = 1.0;
    qreal cellHeight = 1.0;
    qreal baseline = 1.0;
    TerminalGlyphStyle glyphStyle;
    QColor foreground;
    QColor globalBackground;
    int explicitBackgroundAlpha = 255;
    QVector<QColor> palette;
    QBitArray searchCandidateCells;
    QBitArray searchSelectedCells;
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

struct ShapingCursorState {
    bool active = false;
    int row = -1;
    int column = -1;

    bool operator==(const ShapingCursorState &) const = default;
};

struct TerminalSolidRenderState {
    qreal cellWidth = 1.0;
    qreal cellHeight = 1.0;
    qreal devicePixelRatio = 1.0;
    qreal underlinePosition = 0.0;
    qreal underlineThickness = 1.0;
    qreal strikethroughPosition = 0.0;
    qreal strikethroughThickness = 1.0;
    qreal overlinePosition = 0.0;
    qreal overlineThickness = 1.0;
    TerminalGlyphStyle glyphStyle;
    QColor foreground;
    QColor background;
    QColor globalBackground;
    QVector<QColor> palette;
    int explicitBackgroundAlpha = 255;
    int frameColumns = 0;
    int frameRows = 0;
    int visibleColumns = 0;
    int visibleRows = 0;
    bool softwareRenderer = false;

    bool operator==(const TerminalSolidRenderState &) const = default;
};

struct TerminalSolidRowCache {
    quint64 builtEpoch = 0;
    QVector<ColoredRect> backgrounds;
    QVector<ColoredRect> decorationsBeforeText;
    QVector<ColoredRect> decorationsAfterText;
    QVector<QColor> backgroundLayers;
    QVector<QColor> glyphForegrounds;
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
    std::array<quint64, terminalEnumIndex(TerminalFontRole::Count)>
        fontRoleCellCounts{};
    QVector<QColor> decorationForegrounds;
    QVector<QColor> underlineColors;
    QVector<QRectF> underlineRects;
    QVector<QRectF> strikethroughRects;
    QVector<QRectF> overlineRects;
#endif
};

#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
std::atomic<quint64> nextRenderNodeSerial{1};
QMutex renderProbeMutex;
QHash<const TerminalPane *, TerminalPaneRenderProbeSnapshot> renderProbes;
#endif

enum class RectLayer : quint8 {
    Background,
    CursorBackground,
    DecorationBeforeText,
    DecorationAfterText,
    CursorDecoration,
    OverlayBackground,
    OverlayDecoration,
    PaneOverlayBackground,
    PaneOverlayDecoration,
    Count,
};

enum class SolidRowLayer : quint8 {
    Background,
    DecorationBeforeText,
    DecorationAfterText,
    Count,
};

class TerminalSceneNode final : public QSGNode {
public:
    TerminalSceneNode()
        : backdrop(new TerminalBackdropSceneNode)
        , gridTransform(new QSGTransformNode)
        , kittyBelowBackground(new QSGNode)
        , beforeMain(new QSGNode)
        , kittyBelowText(new QSGNode)
        , mainTextRows(new QSGNode)
        , startingTextContainer(new QSGNode)
        , afterMain(new QSGNode)
        , kittyAboveText(new QSGNode)
        , mainOverlay(new QSGNode)
        , overlayTextContainer(new QSGNode)
        , paneOverlay(new QSGNode)
        , paneOverlayTextContainer(new QSGNode)
        , unfocusedSplitOverlay(new QSGSimpleRectNode)
    {
        const auto appendLayer = [this](RectLayer layer, QSGNode *parent) {
            auto *const batch = new TerminalRectBatch;
            rectLayers[terminalEnumIndex(layer)] = batch;
            parent->appendChildNode(batch);
        };
        const auto appendSolidRowLayer = [this](SolidRowLayer layer,
                                                QSGNode *parent) {
            auto *const container = new QSGNode;
            solidRowLayerContainers[terminalEnumIndex(layer)] = container;
            parent->appendChildNode(container);
        };

        appendChildNode(backdrop);
        gridTransform->appendChildNode(kittyBelowBackground);
        appendSolidRowLayer(SolidRowLayer::Background, beforeMain);
        appendLayer(RectLayer::Background, beforeMain);
        beforeMain->appendChildNode(kittyBelowText);
        appendLayer(RectLayer::CursorBackground, beforeMain);
        appendSolidRowLayer(SolidRowLayer::DecorationBeforeText, beforeMain);
        appendLayer(RectLayer::DecorationBeforeText, beforeMain);
        appendSolidRowLayer(SolidRowLayer::DecorationAfterText, afterMain);
        appendLayer(RectLayer::DecorationAfterText, afterMain);
        appendLayer(RectLayer::CursorDecoration, afterMain);
        appendLayer(RectLayer::OverlayBackground, mainOverlay);
        mainOverlay->appendChildNode(overlayTextContainer);
        appendLayer(RectLayer::OverlayDecoration, mainOverlay);
        gridTransform->appendChildNode(beforeMain);
        gridTransform->appendChildNode(mainTextRows);
        gridTransform->appendChildNode(startingTextContainer);
        gridTransform->appendChildNode(afterMain);
        gridTransform->appendChildNode(kittyAboveText);
        gridTransform->appendChildNode(mainOverlay);
        appendChildNode(gridTransform);
        appendLayer(RectLayer::PaneOverlayBackground, paneOverlay);
        paneOverlay->appendChildNode(paneOverlayTextContainer);
        appendLayer(RectLayer::PaneOverlayDecoration, paneOverlay);
        appendChildNode(paneOverlay);
        appendChildNode(unfocusedSplitOverlay);
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
        rootSerial =
            nextRenderNodeSerial.fetch_add(1, std::memory_order_relaxed);
        unfocusedSplitOverlaySerial =
            nextRenderNodeSerial.fetch_add(1, std::memory_order_relaxed);
        kittyBelowBackgroundNodeSerial =
            nextRenderNodeSerial.fetch_add(1, std::memory_order_relaxed);
        kittyBelowTextNodeSerial =
            nextRenderNodeSerial.fetch_add(1, std::memory_order_relaxed);
        kittyAboveTextNodeSerial =
            nextRenderNodeSerial.fetch_add(1, std::memory_order_relaxed);
#endif
    }

    void setGridOrigin(const QPointF &origin) const
    {
        QMatrix4x4 matrix;
        matrix.translate(static_cast<float>(origin.x()),
                         static_cast<float>(origin.y()));
        if (gridTransform->matrix() != matrix) {
            gridTransform->setMatrix(matrix);
        }
    }

    void setUnfocusedSplitOverlay(const QRectF &rect, QColor color) const
    {
        QRectF effectiveRect = rect;
        if (effectiveRect.isEmpty() || !color.isValid() || color.alpha() == 0) {
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

    [[nodiscard]] QVector<ColoredRect> &rects(RectLayer layer) const
    {
        return rectLayers[terminalEnumIndex(layer)]->beginUpdate();
    }

    void commitRectLayers(bool softwareRenderer) const
    {
        for (TerminalRectBatch *batch : rectLayers) {
            batch->commit(softwareRenderer);
        }
    }

    void clearMainText()
    {
        clearNodeChildren(mainTextRows);
        rowContainers.clear();
        rowTextNodes.clear();
        builtRowEpochs.clear();
        textState.reset();
        shapingCursorState = {};
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
        rowNodeSerials.clear();
        rowBuildCounts.clear();
        rowLayoutCounts.clear();
        rowFallbackCellCounts.clear();
#endif
    }

    void clearSolidRows()
    {
        for (QSGNode *container : solidRowLayerContainers) {
            clearNodeChildren(container);
        }
        for (QVector<TerminalRectBatch *> &batches : solidRowLayers) {
            batches.clear();
        }
        solidRows.clear();
        solidState.reset();
        retainedSolidRows = false;
        blockCursorTextState = {};
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
        solidRowBuildCounts.clear();
#endif
    }

    void prepareSolidRows(int rowCount, bool retainGeometry)
    {
        if (solidRows.size() != rowCount
            || retainedSolidRows != retainGeometry) {
            for (std::size_t layer = 0; layer < solidRowLayers.size();
                 ++layer) {
                QSGNode *const container = solidRowLayerContainers[layer];
                clearNodeChildren(container);
                QVector<TerminalRectBatch *> &batches = solidRowLayers[layer];
                batches.clear();
                if (retainGeometry) {
                    batches.reserve(rowCount);
                    for (int row = 0; row < rowCount; ++row) {
                        auto *const batch = new TerminalRectBatch;
                        batches.append(batch);
                        container->appendChildNode(batch);
                    }
                }
            }
        }
        solidRows.resize(rowCount);
        retainedSolidRows = retainGeometry;
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
        solidRowBuildCounts.resize(rowCount);
#endif
    }

    void updateSolidRow(SolidRowLayer layer, int row,
                        const QVector<ColoredRect> &rects,
                        bool softwareRenderer) const
    {
        TerminalRectBatch *const batch =
            solidRowLayers[terminalEnumIndex(layer)].at(row);
        batch->beginUpdate() = rects;
        batch->commit(softwareRenderer);
    }

#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
    [[nodiscard]] QVector<quint64>
    solidRowCommitGenerations(SolidRowLayer layer) const
    {
        QVector<quint64> result;
        const QVector<TerminalRectBatch *> &batches =
            solidRowLayers[terminalEnumIndex(layer)];
        result.reserve(batches.size());
        for (const TerminalRectBatch *batch : batches) {
            result.append(batch->commitGeneration());
        }
        return result;
    }
#endif

    void resetTextRows(
        int rowCount,
        const std::array<QFont, terminalEnumIndex(TerminalFontRole::Count)>
            &newFonts)
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
        rowLayoutCounts.fill(0, rowCount);
        rowFallbackCellCounts.fill(0, rowCount);
#endif
        for (int row = 0; row < rowCount; ++row) {
            auto *container = new QSGNode;
            mainTextRows->appendChildNode(container);
            rowContainers.append(container);
        }

        fonts = newFonts;
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

    void updateTextRowViewports(const QRectF &viewport) const
    {
        for (QSGTextNode *node : rowTextNodes) {
            if (node != nullptr && node->viewport() != viewport) {
                node->setViewport(viewport);
            }
        }
    }

    void updateOverlayText(const OverlayTextRenderState &state)
    {
        [[maybe_unused]] const TextNodeUpdate result =
            updateTextNode(overlayText, overlayTextContainer, overlayTextState,
                           state);
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
        if (result.created) {
            overlayTextNodeSerial =
                nextRenderNodeSerial.fetch_add(1, std::memory_order_relaxed);
        }
        overlayTextBuildCount += static_cast<quint64>(result.rebuilt);
#endif
    }

    void updateStartingText(const OverlayTextRenderState &state)
    {
        [[maybe_unused]] const TextNodeUpdate result =
            updateTextNode(startingText, startingTextContainer,
                           startingTextState, state);
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
        if (result.created) {
            startingTextNodeSerial =
                nextRenderNodeSerial.fetch_add(1, std::memory_order_relaxed);
        }
        startingTextBuildCount += static_cast<quint64>(result.rebuilt);
#endif
    }

    void updatePaneOverlayText(const OverlayTextRenderState &state)
    {
        [[maybe_unused]] const TextNodeUpdate result = updateTextNode(
            paneOverlayText, paneOverlayTextContainer, paneOverlayTextState,
            state);
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
        if (result.created) {
            paneOverlayTextNodeSerial =
                nextRenderNodeSerial.fetch_add(1, std::memory_order_relaxed);
        }
        paneOverlayTextBuildCount += static_cast<quint64>(result.rebuilt);
#endif
    }

private:
    struct TextNodeUpdate {
        bool created = false;
        bool rebuilt = false;
    };

    static TextNodeUpdate
    updateTextNode(QSGTextNode *&node, QSGNode *container,
                   std::optional<OverlayTextRenderState> &previous,
                   const OverlayTextRenderState &state)
    {
        TextNodeUpdate result;
        if (node == nullptr && !state.layouts.isEmpty()) {
            node = createTextNode(state.window, state.viewport,
                                  state.defaultColor);
            if (node != nullptr) {
                container->appendChildNode(node);
                result.created = true;
            }
        }
        if (node == nullptr) {
            previous.reset();
            return result;
        }
        if (previous.has_value() && *previous == state) {
            return result;
        }

        node->clear();
        node->setViewport(state.viewport);
        node->setColor(state.defaultColor);
        for (const TextLayoutSpec &layout : state.activeLayouts()) {
            appendTextLayout(node, layout.text, layout.font, layout.color,
                             layout.position, layout.baseline,
                             layout.lineWidth);
        }
        previous = state;
        result.rebuilt = true;
        return result;
    }

public:
    TerminalBackdropSceneNode *backdrop = nullptr;
    QSGTransformNode *gridTransform = nullptr;
    QSGNode *kittyBelowBackground = nullptr;
    QSGNode *beforeMain = nullptr;
    QSGNode *kittyBelowText = nullptr;
    QSGNode *mainTextRows = nullptr;
    QSGNode *startingTextContainer = nullptr;
    QSGTextNode *startingText = nullptr;
    QSGNode *afterMain = nullptr;
    QSGNode *kittyAboveText = nullptr;
    QSGNode *mainOverlay = nullptr;
    QSGNode *overlayTextContainer = nullptr;
    QSGTextNode *overlayText = nullptr;
    QSGNode *paneOverlay = nullptr;
    QSGNode *paneOverlayTextContainer = nullptr;
    QSGTextNode *paneOverlayText = nullptr;
    QSGSimpleRectNode *unfocusedSplitOverlay = nullptr;
    std::array<TerminalRectBatch *, terminalEnumIndex(RectLayer::Count)>
        rectLayers{};
    std::array<QSGNode *, terminalEnumIndex(SolidRowLayer::Count)>
        solidRowLayerContainers{};
    std::array<QVector<TerminalRectBatch *>,
               terminalEnumIndex(SolidRowLayer::Count)>
        solidRowLayers;
    bool retainedSolidRows = false;
    QVector<QSGNode *> rowContainers;
    QVector<QSGTextNode *> rowTextNodes;
    QVector<quint64> builtRowEpochs;
    QVector<TerminalSolidRowCache> solidRows;
    std::array<QFont, terminalEnumIndex(TerminalFontRole::Count)> fonts;
    std::optional<TerminalTextRenderState> textState;
    std::optional<TerminalSolidRenderState> solidState;
    std::optional<OverlayTextRenderState> startingTextState;
    std::optional<OverlayTextRenderState> overlayTextState;
    std::optional<OverlayTextRenderState> paneOverlayTextState;
    BlockCursorTextState blockCursorTextState;
    ShapingCursorState shapingCursorState;
    TerminalKittyGraphicsScene kittyGraphics;
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
    quint64 rootSerial = 0;
    quint64 unfocusedSplitOverlaySerial = 0;
    quint64 startingTextNodeSerial = 0;
    quint64 overlayTextNodeSerial = 0;
    quint64 paneOverlayTextNodeSerial = 0;
    quint64 kittyBelowBackgroundNodeSerial = 0;
    quint64 kittyBelowTextNodeSerial = 0;
    quint64 kittyAboveTextNodeSerial = 0;
    quint64 overlayTextBuildCount = 0;
    quint64 paneOverlayTextBuildCount = 0;
    quint64 startingTextBuildCount = 0;
    QVector<quint64> rowNodeSerials;
    QVector<quint64> rowBuildCounts;
    QVector<quint64> solidRowBuildCounts;
    quint64 solidCellVisitCount = 0;
    QVector<quint64> rowLayoutCounts;
    QVector<quint64> rowFallbackCellCounts;
#endif
};

#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
void clearRenderProbe(const TerminalPane *pane)
{
    QMutexLocker locker(&renderProbeMutex);
    renderProbes.remove(pane);
}

void publishInitialGeometryProbe(
    const TerminalPane *pane,
    const std::optional<TerminalSessionGeometry> &geometry)
{
    QMutexLocker locker(&renderProbeMutex);
    renderProbes[pane].initialGeometry = geometry;
}

QVector<TerminalPaneRenderLayer> renderLayerOrder(const TerminalSceneNode &root)
{
    QVector<TerminalPaneRenderLayer> result;
    const auto visit = [&](const auto &self, const QSGNode *node) -> void {
        if (node == root.kittyBelowBackground) {
            result.append(TerminalPaneRenderLayer::KittyBelowBackground);
        } else if (node
                   == root.solidRowLayerContainers[terminalEnumIndex(
                       SolidRowLayer::Background)]) {
            result.append(TerminalPaneRenderLayer::CellBackground);
        } else if (node == root.kittyBelowText) {
            result.append(TerminalPaneRenderLayer::KittyBelowText);
        } else if (node
                   == root.rectLayers[terminalEnumIndex(
                       RectLayer::CursorBackground)]) {
            result.append(TerminalPaneRenderLayer::CursorBackground);
        } else if (node == root.mainTextRows) {
            result.append(TerminalPaneRenderLayer::CellForeground);
        } else if (node == root.kittyAboveText) {
            result.append(TerminalPaneRenderLayer::KittyAboveText);
        } else if (node == root.mainOverlay) {
            result.append(TerminalPaneRenderLayer::TerminalOverlay);
        }
        for (const QSGNode *child = node->firstChild(); child != nullptr;
             child = child->nextSibling()) {
            self(self, child);
        }
    };
    visit(visit, root.gridTransform);
    return result;
}

void publishRenderProbe(
    const TerminalPane *pane, const TerminalSceneNode &root,
    const TerminalCellMetrics &metrics,
    const std::array<quint64, terminalEnumIndex(TerminalFontRole::Count)>
        &fontRoleCellCounts,
    const QColor &baseBackground, const QVector<QColor> &cellBackgrounds,
    const QVector<QColor> &glyphForegrounds,
    const QVector<QColor> &decorationForegrounds,
    const QVector<QColor> &underlineColors, const QColor &cursorColor,
    const QVector<QRectF> &underlineRects,
    const QVector<QRectF> &strikethroughRects,
    const QVector<QRectF> &overlineRects, const QVector<QRectF> &cursorRects)
{
    QMutexLocker locker(&renderProbeMutex);
    TerminalPaneRenderProbeSnapshot &snapshot = renderProbes[pane];
    ++snapshot.paintSerial;
    snapshot.rootSerial = root.rootSerial;
    snapshot.unfocusedSplitOverlaySerial = root.unfocusedSplitOverlaySerial;
    snapshot.startingTextNodeSerial = root.startingTextNodeSerial;
    snapshot.overlayTextNodeSerial = root.overlayTextNodeSerial;
    snapshot.paneOverlayTextNodeSerial = root.paneOverlayTextNodeSerial;
    snapshot.kittyBelowBackgroundNodeSerial =
        root.kittyBelowBackgroundNodeSerial;
    snapshot.kittyBelowTextNodeSerial = root.kittyBelowTextNodeSerial;
    snapshot.kittyAboveTextNodeSerial = root.kittyAboveTextNodeSerial;
    snapshot.renderLayerOrder = renderLayerOrder(root);
    snapshot.overlayTextBuildCount = root.overlayTextBuildCount;
    snapshot.paneOverlayTextBuildCount = root.paneOverlayTextBuildCount;
    snapshot.startingTextBuildCount = root.startingTextBuildCount;
    snapshot.unfocusedSplitOverlayRect = root.unfocusedSplitOverlay->rect();
    snapshot.unfocusedSplitOverlayColor = root.unfocusedSplitOverlay->color();
    snapshot.backgroundImageAssetSerial = root.backdrop->assetSerial();
    snapshot.backgroundImageRect = root.backdrop->imageRect();
    snapshot.backgroundImageSourceRect = root.backdrop->sourceRect();
    snapshot.kittyGraphicsTextureUploadCount =
        root.kittyGraphics.textureUploadCount();
    snapshot.kittyGraphicsNodeCreationCount =
        root.kittyGraphics.nodeCreationCount();
    snapshot.kittyGraphicsNodeDeletionCount =
        root.kittyGraphics.nodeDeletionCount();
    snapshot.kittyGraphicsGeometryWriteCount =
        root.kittyGraphics.geometryWriteCount();
    snapshot.kittyGraphicsMaterialAssignmentCount =
        root.kittyGraphics.materialAssignmentCount();
    snapshot.kittyGraphicsTextureSetEvictionCount =
        root.kittyGraphics.textureSetEvictionCount();
    snapshot.kittyGraphicsTextureCount = root.kittyGraphics.textureCount();
    snapshot.kittyGraphicsTextureBytes = root.kittyGraphics.textureBytes();
    snapshot.kittyGraphicsDestinations.clear();
    snapshot.kittyGraphicsSources.clear();
    snapshot.kittyGraphicsLayers.clear();
    const auto &kittyPlacements = root.kittyGraphics.renderedPlacements();
    snapshot.kittyGraphicsDestinations.reserve(kittyPlacements.size());
    snapshot.kittyGraphicsSources.reserve(kittyPlacements.size());
    snapshot.kittyGraphicsLayers.reserve(kittyPlacements.size());
    for (const TerminalKittyGraphicsRenderPlacement &placement :
         kittyPlacements) {
        snapshot.kittyGraphicsDestinations.append(placement.destination);
        snapshot.kittyGraphicsSources.append(placement.source);
        snapshot.kittyGraphicsLayers.append(placement.layer);
    }
    snapshot.backdropBaseRects.clear();
    const auto baseRects = root.backdrop->baseRects();
    snapshot.backdropBaseRects.reserve(
        static_cast<qsizetype>(baseRects.size()));
    for (const QRectF &rect : baseRects) {
        snapshot.backdropBaseRects.append(rect);
    }
    snapshot.rowNodeSerials = root.rowNodeSerials;
    snapshot.rowBuildCounts = root.rowBuildCounts;
    snapshot.rowSolidBuildCounts = root.solidRowBuildCounts;
    snapshot.retainedSolidRowGeometry = root.retainedSolidRows;
    snapshot.rowBackgroundGeometryCommitCounts =
        root.solidRowCommitGenerations(SolidRowLayer::Background);
    snapshot.rowDecorationBeforeTextGeometryCommitCounts =
        root.solidRowCommitGenerations(SolidRowLayer::DecorationBeforeText);
    snapshot.rowDecorationAfterTextGeometryCommitCounts =
        root.solidRowCommitGenerations(SolidRowLayer::DecorationAfterText);
    snapshot.globalBackgroundGeometryCommitCount =
        root.rectLayers[terminalEnumIndex(RectLayer::Background)]
            ->commitGeneration();
    snapshot.globalDecorationBeforeTextGeometryCommitCount =
        root.rectLayers[terminalEnumIndex(RectLayer::DecorationBeforeText)]
            ->commitGeneration();
    snapshot.globalDecorationAfterTextGeometryCommitCount =
        root.rectLayers[terminalEnumIndex(RectLayer::DecorationAfterText)]
            ->commitGeneration();
    snapshot.solidCellVisitCount = root.solidCellVisitCount;
    snapshot.rowLayoutCounts = root.rowLayoutCounts;
    snapshot.rowFallbackCellCounts = root.rowFallbackCellCounts;
    snapshot.metrics = metrics;
    snapshot.renderFonts = root.fonts;
    snapshot.fontRoleCellCounts = fontRoleCellCounts;
    snapshot.baseBackground = baseBackground;
    snapshot.cellBackgrounds = cellBackgrounds;
    snapshot.glyphForegrounds = glyphForegrounds;
    snapshot.decorationForegrounds = decorationForegrounds;
    snapshot.underlineColors = underlineColors;
    snapshot.cursorColor = cursorColor;
    snapshot.underlineRects = underlineRects;
    snapshot.strikethroughRects = strikethroughRects;
    snapshot.overlineRects = overlineRects;
    snapshot.cursorRects = cursorRects;
}
#endif

} // namespace

#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
namespace TerminalPaneRenderer {

void clearProbe(const TerminalPane *pane)
{
    clearRenderProbe(pane);
}

void publishInitialGeometryProbe(
    const TerminalPane *pane,
    const std::optional<TerminalSessionGeometry> &geometry)
{
    ::publishInitialGeometryProbe(pane, geometry);
}

} // namespace TerminalPaneRenderer
#endif

#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
TerminalPaneRenderProbeSnapshot
terminalPaneRenderProbe(const TerminalPane *pane)
{
    QMutexLocker locker(&renderProbeMutex);
    return renderProbes.value(pane);
}

bool terminalPaneDelegatedPaintNodeTeardownForTest(TerminalPane *pane)
{
    if (pane == nullptr || pane->renderItem_ != nullptr) return false;

    class LifetimeNode final : public QSGNode {
    public:
        explicit LifetimeNode(bool *destroyed)
            : destroyed_(destroyed)
        {}

        ~LifetimeNode() override { *destroyed_ = true; }

    private:
        bool *destroyed_ = nullptr;
    };

    bool destroyed = false;
    pane->renderItem_ = TerminalPaneRenderer::createRenderItem(pane);
    QSGNode *const result =
        pane->updatePaintNode(new LifetimeNode(&destroyed), nullptr);
    pane->useDirectTerminalRendering();
    return result == nullptr && destroyed && pane->renderItem_ == nullptr
        && pane->flags().testFlag(QQuickItem::ItemHasContents);
}

QColor terminalMinimumContrastColorForTest(const QColor &foreground,
                                           const QColor &cellBackground,
                                           const QColor &globalBackground,
                                           double minimumContrast)
{
    return minimumContrastColor(foreground, cellBackground, globalBackground,
                                minimumContrast);
}
#endif

QSGNode *TerminalPane::updatePaintNode(QSGNode *oldNode,
                                       UpdatePaintNodeData *updateData)
{
    if (renderItem_ != nullptr) {
        delete oldNode;
        return nullptr;
    }
    return updateTerminalPaintNode(oldNode, updateData);
}

QSGNode *TerminalPane::updateTerminalPaintNode(QSGNode *oldNode,
                                               UpdatePaintNodeData *)
{
    auto *root = oldNode != nullptr ? static_cast<TerminalSceneNode *>(oldNode)
                                    : new TerminalSceneNode;

    TerminalFrame frame;
    QVector<quint64> textRowEpochs;
    QVector<quint64> solidRowEpochs;
    TerminalAppearance appearance;
    TerminalBackgroundOptions backgroundOptions;
    std::shared_ptr<const TerminalBackgroundImageAsset> backgroundImageAsset;
    TerminalPaddingOptions paddingOptions;
    SplitAppearance splitAppearance;
    TerminalCellMetrics metrics;
    QString preedit;
    QString status;
    QString linkPreview;
    QRectF linkPreviewRect;
    QBitArray hoveredHyperlinkCells;
    QBitArray searchCandidateCellMask;
    QBitArray searchSelectedCellMask;
    bool hasFrame = false;
    {
        QMutexLocker locker(&renderMutex_);
        frame = frame_;
        textRowEpochs = textRowEpochs_;
        solidRowEpochs = solidRowEpochs_;
        appearance = appearance_;
        backgroundOptions = backgroundOptions_;
        backgroundImageAsset = backgroundImageAsset_;
        paddingOptions = paddingOptions_;
        splitAppearance = splitAppearance_;
        metrics = metrics_;
        preedit = preedit_;
        status = statusMessage_;
        linkPreview = linkPreviewText_;
        linkPreviewRect = linkPreviewRect_;
        if (hoveredHyperlinkColumns_ == frame_.columns
            && hoveredHyperlinkRows_ == frame_.rows) {
            hoveredHyperlinkCells = hoveredHyperlinkCellMask_;
        }
        if (searchDecorationRevision_ == frame_.contentRevision
            && searchDecorationColumns_ == frame_.columns
            && searchDecorationRows_ == frame_.rows) {
            searchCandidateCellMask = searchCandidateCellMask_;
            searchSelectedCellMask = searchSelectedCellMask_;
        }
        hasFrame = hasFrame_;
    }
    const QFont &baseFont = metrics.font(TerminalFontRole::Regular);
    const qreal metricCellWidth = metrics.cellWidth;
    const qreal metricCellHeight = metrics.cellHeight;
    const qreal baseline = metrics.baseline;
    const bool candidateMaskMatchesFrame =
        searchCandidateCellMask.size() == frame.cells.size();
    const bool selectedMaskMatchesFrame =
        searchSelectedCellMask.size() == frame.cells.size();

    const QRectF viewport = boundingRect();
    const QSGRendererInterface *rendererInterface =
        window() != nullptr ? window()->rendererInterface() : nullptr;
    const qreal devicePixelRatio =
        window() != nullptr ? window()->devicePixelRatio() : 1.0;
    const std::optional<TerminalViewportLayout> layout =
        terminalViewportLayout({
            .surfaceSize = viewport.size(),
            .cellSize = QSizeF(metricCellWidth, metricCellHeight),
            .devicePixelRatio = devicePixelRatio,
            .padding = paddingOptions,
        });
    const QPointF gridOrigin = layout ? layout->gridRect.topLeft() : QPointF{};
    const qreal cellWidth = layout
        ? layout->gridRect.width() / layout->session.columns
        : metricCellWidth;
    const qreal cellHeight = layout
        ? layout->gridRect.height() / layout->session.rows
        : metricCellHeight;
    const QRectF gridViewport = viewport.translated(-gridOrigin);
    root->setGridOrigin(gridOrigin);
    const qreal underlinePosition =
        std::min(metrics.underlinePosition, metrics.underlineMaximumPosition);
    const qreal overlinePosition =
        std::max(metrics.overlinePosition, metrics.overlineMinimumPosition);
    const bool softwareRenderer = rendererInterface == nullptr
        || rendererInterface->graphicsApi() == QSGRendererInterface::Software;
    const bool customBackdropRenderer = rendererInterface != nullptr
        && QSGRendererInterface::isApiRhiBased(
                                            rendererInterface->graphicsApi());
    const QColor opaqueBackground =
        hasFrame ? frame.background : appearance.backgroundColor;
    const int baseBackgroundAlpha =
        roundedOpacityByte(backgroundOptions.opacity);
    const int explicitBackgroundAlpha = backgroundOptions.opacityCells
        ? truncatedOpacityByte(backgroundOptions.opacity)
        : 255;
    const QColor globalBackground =
        withAlpha(opaqueBackground, baseBackgroundAlpha);
    root->backdrop->update(window(), viewport, globalBackground,
                           backgroundImageAsset, backgroundOptions.image,
                           devicePixelRatio, customBackdropRenderer);
    auto &backgrounds = root->rects(RectLayer::Background);
    auto &cursorBackgrounds = root->rects(RectLayer::CursorBackground);
    auto &decorationsBeforeText = root->rects(RectLayer::DecorationBeforeText);
    auto &decorationsAfterText = root->rects(RectLayer::DecorationAfterText);
    auto &cursorDecorations = root->rects(RectLayer::CursorDecoration);
    auto &overlayBackgrounds = root->rects(RectLayer::OverlayBackground);
    auto &overlayDecorations = root->rects(RectLayer::OverlayDecoration);
    auto &paneOverlayBackgrounds =
        root->rects(RectLayer::PaneOverlayBackground);
    auto &paneOverlayDecorations =
        root->rects(RectLayer::PaneOverlayDecoration);
    QVector<QRectF> *underlineProbe = nullptr;
    QVector<QRectF> *cursorProbe = nullptr;
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
    std::array<quint64, terminalEnumIndex(TerminalFontRole::Count)>
        fontRoleCellCounts{};
    QVector<QRectF> underlineRects;
    QVector<QRectF> strikethroughRects;
    QVector<QRectF> overlineRects;
    QVector<QRectF> cursorRects;
    QVector<QColor> glyphForegrounds(frame.cells.size());
    QVector<QColor> decorationForegrounds(frame.cells.size());
    QVector<QColor> underlineColors(frame.cells.size());
    QVector<QColor> cellBackgroundLayers(frame.cells.size());
    QColor renderedCursorColor;
    underlineProbe = &underlineRects;
    cursorProbe = &cursorRects;
#endif
    OverlayTextRenderState overlayTextState(
        window(), gridViewport, QColor(QStringLiteral("#eceff4")));
    OverlayTextRenderState paneOverlayTextState(
        window(), viewport, QColor(QStringLiteral("#eceff4")));
    OverlayTextRenderState startingTextState(
        window(), gridViewport, QColor(QStringLiteral("#88909d")));

    if (!hasFrame || frame.columns <= 0 || frame.rows <= 0) {
        root->kittyGraphics.update(window(), root->kittyBelowBackground,
                                   root->kittyBelowText, root->kittyAboveText,
                                   {}, QSizeF(cellWidth, cellHeight), {},
                                   customBackdropRenderer);
        root->clearMainText();
        root->clearSolidRows();
        startingTextState.append({
            .text = QStringLiteral("Starting terminal…"),
            .font = baseFont,
            .color = QColor(QStringLiteral("#88909d")),
            .position = QPointF(12.0, 12.0),
            .baseline = baseline,
            .lineWidth = std::max<qreal>(1.0, viewport.width() - 24.0),
        });
    } else {
        const int visibleRows =
            std::min(frame.rows, layout ? layout->session.rows : frame.rows);
        const int visibleColumns = std::min(
            frame.columns, layout ? layout->session.columns : frame.columns);
        const bool kittyGeometryCurrent = layout.has_value()
            && frame.columns == layout->session.columns
            && frame.rows == layout->session.rows
            && frame.kittyGraphics != nullptr
            && frame.kittyGraphics->cellWidthPixels
                == static_cast<quint32>(layout->session.cellWidthPixels)
            && frame.kittyGraphics->cellHeightPixels
                == static_cast<quint32>(layout->session.cellHeightPixels);
        root->kittyGraphics.update(
            window(), root->kittyBelowBackground, root->kittyBelowText,
            root->kittyAboveText,
            kittyGeometryCurrent ? frame.kittyGraphics : nullptr,
            QSizeF(cellWidth, cellHeight),
            QRectF(0.0, 0.0, static_cast<qreal>(visibleColumns) * cellWidth,
                   static_cast<qreal>(visibleRows) * cellHeight),
            customBackdropRenderer);
        const bool focused = hasActiveFocus();
        const bool logicalCursorActive = frame.cursorVisible
            && frame.cursorColumn >= 0 && frame.cursorColumn < visibleColumns
            && frame.cursorRow >= 0 && frame.cursorRow < visibleRows;
        const bool cursorActive = logicalCursorActive
            && (!focused || !frame.cursorBlinking || cursorBlinkOn_);
        const int cursorStyle = focused ? frame.cursorStyle : 3;
        const bool blockCursorActive = cursorActive && cursorStyle == 1;
        QColor cursorCellForeground = frame.foreground;
        QColor cursorCellBackground = frame.background;
        QColor cursorEffectiveBackground = withAlpha(frame.background, 0);
        const qsizetype cursorCellIndex = cursorActive
            ? static_cast<qsizetype>(frame.cursorRow) * frame.columns
                + frame.cursorColumn
            : -1;
        if (cursorActive && cursorCellIndex >= 0
            && cursorCellIndex < frame.cells.size()) {
            const TerminalCell &cursorCell = frame.cells.at(cursorCellIndex);
            cursorCellForeground = cursorCell.foreground;
            cursorCellBackground = cursorCell.background;
            applyBoldColor(cursorCell, frame, appearance, &cursorCellForeground,
                           &cursorCellBackground);
            cursorEffectiveBackground = cursorCellBackground;
        }
        QColor blockCursorTextColor = resolveRelativeColor(
            appearance.cursorTextColor, cursorCellForeground,
            cursorCellBackground, frame.background);
        blockCursorTextColor.setAlpha(255);

        TerminalTextRenderState textState;
        textState.window = window();
        textState.fontProgram = metrics.fontProgram;
        textState.shapingBreakCursor = metrics.shapingBreakCursor;
        textState.cellWidth = cellWidth;
        textState.cellHeight = cellHeight;
        textState.baseline = metrics.baseline;
        textState.glyphStyle =
            TerminalGlyphStyle::fromAppearance(appearance);
        textState.foreground = frame.foreground;
        // Opacity can only alter retained glyph/decor colors through minimum
        // contrast. Keep background-only reloads off the text rebuild path at
        // the default threshold.
        if (appearance.minimumContrast > 1.0) {
            textState.globalBackground = globalBackground;
            textState.explicitBackgroundAlpha = explicitBackgroundAlpha;
        } else {
            textState.globalBackground = frame.background;
        }
        textState.palette = frame.palette;
        textState.searchCandidateCells = searchCandidateCellMask;
        textState.searchSelectedCells = searchSelectedCellMask;
        textState.devicePixelRatio = devicePixelRatio;
        textState.graphicsApi = rendererInterface != nullptr
            ? static_cast<int>(rendererInterface->graphicsApi())
            : -1;
        textState.frameColumns = frame.columns;
        textState.frameRows = frame.rows;
        textState.visibleColumns = visibleColumns;
        textState.visibleRows = visibleRows;

        const bool rebuildAllText = !root->textState.has_value()
            || *root->textState != textState
            || root->rowTextNodes.size() != visibleRows;
        if (rebuildAllText) {
            root->resetTextRows(visibleRows, metrics.fontProgram->fonts);
            root->textState = textState;
        } else {
            // The viewport is clipping state, not shaping state. Interactive
            // pixel resizes that retain the same grid geometry update it in
            // place without rebuilding every row's glyph layouts.
            root->updateTextRowViewports(gridViewport);
        }

        const TerminalSolidRenderState solidState{
            .cellWidth = cellWidth,
            .cellHeight = cellHeight,
            .devicePixelRatio = devicePixelRatio,
            .underlinePosition = metrics.underlinePosition,
            .underlineThickness = metrics.underlineThickness,
            .strikethroughPosition = metrics.strikethroughPosition,
            .strikethroughThickness = metrics.strikethroughThickness,
            .overlinePosition = overlinePosition,
            .overlineThickness = metrics.overlineThickness,
            .glyphStyle = TerminalGlyphStyle::fromAppearance(appearance),
            .foreground = frame.foreground,
            .background = frame.background,
            .globalBackground = appearance.minimumContrast > 1.0
                ? globalBackground
                : frame.background,
            .palette = frame.palette,
            .explicitBackgroundAlpha = explicitBackgroundAlpha,
            .frameColumns = frame.columns,
            .frameRows = frame.rows,
            .visibleColumns = visibleColumns,
            .visibleRows = visibleRows,
            .softwareRenderer = softwareRenderer,
        };
        const bool rebuildAllSolids = !root->solidState.has_value()
            || *root->solidState != solidState
            || root->solidRows.size() != visibleRows;
        root->prepareSolidRows(visibleRows, !softwareRenderer);
        root->solidState = solidState;

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
        const bool blockCursorStateChanged =
            previousBlockCursorTextState != blockCursorTextState;
        const bool blockCursorTextChanged =
            !rebuildAllText && blockCursorStateChanged;
        ShapingCursorState shapingCursorState;
        if (metrics.shapingBreakCursor && logicalCursorActive) {
            shapingCursorState = {
                .active = true,
                .row = frame.cursorRow,
                .column = frame.cursorColumn,
            };
        }
        const ShapingCursorState previousShapingCursorState =
            root->shapingCursorState;
        const bool shapingCursorChanged =
            !rebuildAllText && previousShapingCursorState != shapingCursorState;
        const bool extendPadding =
            layout && paddingOptions.color != TerminalPaddingColor::Background;

        for (int row = 0; row < visibleRows; ++row) {
            const qreal top = static_cast<qreal>(row) * cellHeight;
            const quint64 textRowEpoch =
                row < textRowEpochs.size() ? textRowEpochs.at(row) : 0;
            const quint64 solidRowEpoch =
                row < solidRowEpochs.size() ? solidRowEpochs.at(row) : 0;
            const bool cursorChangedThisRow = blockCursorTextChanged
                && ((previousBlockCursorTextState.active
                     && previousBlockCursorTextState.row == row)
                    || (blockCursorTextState.active
                        && blockCursorTextState.row == row));
            const bool shapingCursorChangedThisRow = shapingCursorChanged
                && ((previousShapingCursorState.active
                     && previousShapingCursorState.row == row)
                    || (shapingCursorState.active
                        && shapingCursorState.row == row));
            const bool rebuildRowText = rebuildAllText
                || root->rowTextNodes.at(row) == nullptr
                || root->builtRowEpochs.at(row) != textRowEpoch
                || cursorChangedThisRow || shapingCursorChangedThisRow;
            const bool blockCursorChangedThisRow = blockCursorStateChanged
                && ((previousBlockCursorTextState.active
                     && previousBlockCursorTextState.row == row)
                    || (blockCursorTextState.active
                        && blockCursorTextState.row == row));
            TerminalSolidRowCache &solidRow = root->solidRows[row];
            const bool rebuildRowSolids = rebuildAllSolids
                || solidRow.backgroundLayers.size() != visibleColumns
                || solidRow.glyphForegrounds.size() != visibleColumns
                || solidRow.builtEpoch != solidRowEpoch
                || blockCursorChangedThisRow;
            QSGTextNode *rowText = rebuildRowText
                ? root->prepareTextRow(row, window(), gridViewport,
                                       frame.foreground)
                : nullptr;
            QVector<TerminalTextCell> rowTextCells;
            if (rowText != nullptr) {
                rowTextCells.reserve(visibleColumns);
            }
            if (rebuildRowSolids) {
                solidRow.backgrounds.clear();
                solidRow.decorationsBeforeText.clear();
                solidRow.decorationsAfterText.clear();
                solidRow.backgroundLayers.fill({}, visibleColumns);
                solidRow.glyphForegrounds.fill({}, visibleColumns);
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
                solidRow.fontRoleCellCounts.fill(0);
                solidRow.decorationForegrounds.fill({}, visibleColumns);
                solidRow.underlineColors.fill({}, visibleColumns);
                solidRow.underlineRects.clear();
                solidRow.strikethroughRects.clear();
                solidRow.overlineRects.clear();
                ++root->solidRowBuildCounts[row];
                root->solidCellVisitCount +=
                    static_cast<quint64>(visibleColumns);
#endif
            }
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
            if (rebuildRowText) {
                root->rowLayoutCounts[row] = 0;
                root->rowFallbackCellCounts[row] = 0;
            }
#endif
            if (rebuildRowSolids || rowText != nullptr) {
                for (int column = 0; column < visibleColumns; ++column) {
                    const qsizetype index =
                        static_cast<qsizetype>(row) * frame.columns + column;
                    if (index >= frame.cells.size()) {
                        continue;
                    }
                    const TerminalCell &cell = frame.cells.at(index);
                    const qreal left = static_cast<qreal>(column) * cellWidth;
                    const qreal drawWidth = cellWidth
                        * static_cast<qreal>(std::max(1, cell.columnSpan));
                    const TerminalFontRole fontRole =
                        terminalFontRole(cell.bold, cell.italic);

                    if (rebuildRowSolids) {
                        QColor styledForeground = cell.foreground;
                        QColor styledBackground = cell.background;
                        applyBoldColor(cell, frame, appearance,
                                       &styledForeground, &styledBackground);

                        bool candidateSearchMatch = candidateMaskMatchesFrame
                            && searchCandidateCellMask.testBit(index);
                        bool selectedSearchMatch = selectedMaskMatchesFrame
                            && searchSelectedCellMask.testBit(index);
                        if (cell.spacer && column > 0) {
                            // libghostty maps every UTF-8 byte to the owning
                            // wide head. Carry its decoration into the spacer.
                            candidateSearchMatch = candidateSearchMatch
                                || (candidateMaskMatchesFrame
                                    && searchCandidateCellMask.testBit(index
                                                                       - 1));
                            selectedSearchMatch = selectedSearchMatch
                                || (selectedMaskMatchesFrame
                                    && searchSelectedCellMask.testBit(index
                                                                      - 1));
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
                                appearance.searchBackground, styledForeground,
                                styledBackground, frame.foreground);
                        }
                        const bool forceOpaqueBackground = cell.selected
                            || selectedSearchMatch || candidateSearchMatch
                            || cell.inverse;
                        const QColor backgroundLayer = cellBackgroundLayer(
                            cellBackground, cell.backgroundExplicit,
                            forceOpaqueBackground, explicitBackgroundAlpha);
                        solidRow.backgroundLayers[column] = backgroundLayer;
                        if (backgroundLayer.alpha() > 0) {
                            // Backgrounds are cell state even for wide glyphs;
                            // row-local batching safely merges adjacent fills.
                            appendHorizontalRect(
                                solidRow.backgrounds,
                                QRectF(left, top, cellWidth, cellHeight),
                                backgroundLayer);
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
                                appearance.searchForeground, styledForeground,
                                styledBackground, frame.background);
                        }
                        const bool insideBlockCursor = blockCursorActive
                            && row == frame.cursorRow
                            && column >= frame.cursorColumn
                            && column < frame.cursorColumn
                                    + std::max(1, frame.cursorColumnSpan);
                        if (cell.faint) {
                            foreground = withOpacity(foreground,
                                                     appearance.faintOpacity);
                        }
                        QColor decorationForeground = minimumContrastColor(
                            foreground, backgroundLayer, globalBackground,
                            appearance.minimumContrast);
                        if (!cell.minimumContrastExemptGlyph) {
                            foreground = decorationForeground;
                        }
                        // Ghostty applies block-cursor text last to every
                        // foreground primitive covered by a wide cursor.
                        if (insideBlockCursor) {
                            foreground = blockCursorTextColor;
                            decorationForeground = blockCursorTextColor;
                        }
                        solidRow.glyphForegrounds[column] = foreground;
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
                        solidRow.decorationForegrounds[column] =
                            decorationForeground;
                        if (!cell.invisible && !cell.text.isEmpty()
                            && !cell.spacer) {
                            ++solidRow.fontRoleCellCounts[terminalEnumIndex(
                                fontRole)];
                        }
#endif

                        if (!cell.invisible) {
                            TerminalUnderlineStyle underlineStyle =
                                cell.underlineStyle;
                            if (index < hoveredHyperlinkCells.size()
                                && hoveredHyperlinkCells.testBit(index)) {
                                underlineStyle = cell.underlineStyle
                                        == TerminalUnderlineStyle::Single
                                    ? TerminalUnderlineStyle::Double
                                    : TerminalUnderlineStyle::Single;
                            }
                            if (underlineStyle
                                != TerminalUnderlineStyle::None) {
                                QColor underlineColor =
                                    cell.underlineUsesForeground
                                    ? decorationForeground
                                    : cell.underlineColor;
                                if (!cell.underlineUsesForeground
                                    && cell.faint) {
                                    underlineColor =
                                        withOpacity(underlineColor,
                                                    appearance.faintOpacity);
                                }
                                if (!cell.underlineUsesForeground) {
                                    underlineColor = minimumContrastColor(
                                        underlineColor, backgroundLayer,
                                        globalBackground,
                                        appearance.minimumContrast);
                                }
                                if (insideBlockCursor) {
                                    underlineColor = blockCursorTextColor;
                                }
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
                                solidRow.underlineColors[column] =
                                    underlineColor;
                                QVector<QRectF> *rowUnderlineProbe =
                                    &solidRow.underlineRects;
#else
                                QVector<QRectF> *rowUnderlineProbe = nullptr;
#endif
                                appendUnderline(
                                    solidRow.decorationsBeforeText,
                                    QRectF(left, top, drawWidth, cellHeight),
                                    metrics.underlinePosition,
                                    metrics.underlineThickness, underlineStyle,
                                    underlineColor, devicePixelRatio,
                                    rowUnderlineProbe);
                            }
                            if (cell.strikeThrough || cell.overline) {
                                const QRectF decorationCanvas =
                                    paddedSpriteCanvas(QRectF(left, top,
                                                              drawWidth,
                                                              cellHeight),
                                                       devicePixelRatio);
                                if (cell.strikeThrough) {
                                    const QRectF rect(
                                        left,
                                        top + metrics.strikethroughPosition,
                                        drawWidth,
                                        metrics.strikethroughThickness);
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
                                    QVector<QRectF> *rowStrikeProbe =
                                        &solidRow.strikethroughRects;
#else
                                    QVector<QRectF> *rowStrikeProbe = nullptr;
#endif
                                    appendClippedRect(
                                        solidRow.decorationsAfterText, rect,
                                        decorationCanvas, decorationForeground,
                                        rowStrikeProbe);
                                }
                                if (cell.overline) {
                                    const QRectF rect(
                                        left, top + overlinePosition, drawWidth,
                                        metrics.overlineThickness);
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
                                    QVector<QRectF> *rowOverlineProbe =
                                        &solidRow.overlineRects;
#else
                                    QVector<QRectF> *rowOverlineProbe = nullptr;
#endif
                                    appendClippedRect(
                                        solidRow.decorationsBeforeText, rect,
                                        decorationCanvas, decorationForeground,
                                        rowOverlineProbe);
                                }
                            }
                        }
                    }

                    if (rowText != nullptr) {
                        rowTextCells.append({
                            .text = cell.text,
                            .font = metrics.fontForText(fontRole, cell.text),
                            .color = solidRow.glyphForegrounds.at(column),
                            .style = terminalShapingStyle(cell),
                            .baseCodepoint = cell.baseCodepoint,
                            .column = column,
                            .columnSpan = std::max(1, cell.columnSpan),
                            .plainCodepoint = cell.plainCodepoint,
                            .extendedGrapheme = cell.extendedGrapheme,
                            .selected = cell.selected,
                            .invisible = cell.invisible,
                            .spacer = cell.spacer,
                            .cursor = shapingCursorState.active
                                && shapingCursorState.row == row
                                && shapingCursorState.column == column,
                        });
                    }
                }
            }
            if (rowText != nullptr) {
                const QVector<TerminalTextRun> runs = planTerminalTextRuns(
                    std::span<const TerminalTextCell>(
                        rowTextCells.constData(),
                        static_cast<std::size_t>(rowTextCells.size())),
                    metrics.shapingBreakCursor);
                for (const TerminalTextRun &run : runs) {
                    const TerminalRunLayoutResult result =
                        appendTerminalTextRun(rowText, run, top, baseline,
                                              cellWidth, devicePixelRatio);
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
                    root->rowLayoutCounts[row] += result.layoutCount;
                    root->rowFallbackCellCounts[row] +=
                        result.fallbackCellCount;
#else
                    Q_UNUSED(result);
#endif
                }
            }
            if (rebuildRowText) {
                root->builtRowEpochs[row] =
                    rowText != nullptr ? textRowEpoch : 0;
            }
            if (rebuildRowSolids) {
                solidRow.builtEpoch = solidRowEpoch;
                if (!softwareRenderer) {
                    root->updateSolidRow(SolidRowLayer::Background, row,
                                         solidRow.backgrounds,
                                         softwareRenderer);
                    root->updateSolidRow(SolidRowLayer::DecorationBeforeText,
                                         row, solidRow.decorationsBeforeText,
                                         softwareRenderer);
                    root->updateSolidRow(SolidRowLayer::DecorationAfterText,
                                         row, solidRow.decorationsAfterText,
                                         softwareRenderer);
                }
            }
            if (softwareRenderer) {
                backgrounds.append(solidRow.backgrounds);
                decorationsBeforeText.append(solidRow.decorationsBeforeText);
                decorationsAfterText.append(solidRow.decorationsAfterText);
            }
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
            const qsizetype probeOffset =
                static_cast<qsizetype>(row) * frame.columns;
            std::copy(solidRow.backgroundLayers.cbegin(),
                      solidRow.backgroundLayers.cend(),
                      cellBackgroundLayers.begin() + probeOffset);
            std::copy(solidRow.glyphForegrounds.cbegin(),
                      solidRow.glyphForegrounds.cend(),
                      glyphForegrounds.begin() + probeOffset);
            std::copy(solidRow.decorationForegrounds.cbegin(),
                      solidRow.decorationForegrounds.cend(),
                      decorationForegrounds.begin() + probeOffset);
            std::copy(solidRow.underlineColors.cbegin(),
                      solidRow.underlineColors.cend(),
                      underlineColors.begin() + probeOffset);
            underlineRects.append(solidRow.underlineRects);
            strikethroughRects.append(solidRow.strikethroughRects);
            overlineRects.append(solidRow.overlineRects);
            for (std::size_t role = 0;
                 role < solidRow.fontRoleCellCounts.size(); ++role) {
                fontRoleCellCounts[role] += solidRow.fontRoleCellCounts[role];
            }
#endif
        }

        if (extendPadding && visibleRows > 0 && visibleColumns > 0) {
            const qreal gridWidth = visibleColumns * cellWidth;
            const qreal gridHeight = visibleRows * cellHeight;
            const qreal leftExtent = std::max<qreal>(0.0, gridOrigin.x());
            const qreal topExtent = std::max<qreal>(0.0, gridOrigin.y());
            const qreal rightExtent = std::max<qreal>(
                0.0, viewport.width() - gridOrigin.x() - gridWidth);
            const qreal bottomExtent = std::max<qreal>(
                0.0, viewport.height() - gridOrigin.y() - gridHeight);
            for (int row = 0; row < visibleRows; ++row) {
                const qreal top = row * cellHeight;
                const QVector<QColor> &edgeBackgrounds =
                    root->solidRows.at(row).backgroundLayers;
                appendRect(backgrounds,
                           QRectF(-leftExtent, top, leftExtent, cellHeight),
                           edgeBackgrounds.first());
                appendRect(backgrounds,
                           QRectF(gridWidth, top, rightExtent, cellHeight),
                           edgeBackgrounds.last());
            }

            const bool always =
                paddingOptions.color == TerminalPaddingColor::ExtendAlways;
            const bool presentationsMatch =
                frame.rowPresentation.size() == frame.rows;
            const bool extendTop = always
                || (presentationsMatch
                    && frame.rowPresentation.first().paddingExtensionSafe);
            const bool extendBottom = always
                || (presentationsMatch
                    && frame.rowPresentation.at(visibleRows - 1)
                           .paddingExtensionSafe);
            if (extendTop) {
                const QVector<QColor> &topEdgeBackgrounds =
                    root->solidRows.first().backgroundLayers;
                for (int column = 0; column < visibleColumns; ++column) {
                    appendHorizontalRect(backgrounds,
                                         QRectF(column * cellWidth, -topExtent,
                                                cellWidth, topExtent),
                                         topEdgeBackgrounds.at(column));
                }
                appendRect(
                    backgrounds,
                    QRectF(-leftExtent, -topExtent, leftExtent, topExtent),
                    topEdgeBackgrounds.first());
                appendRect(
                    backgrounds,
                    QRectF(gridWidth, -topExtent, rightExtent, topExtent),
                    topEdgeBackgrounds.last());
            }
            if (extendBottom) {
                const QVector<QColor> &bottomEdgeBackgrounds =
                    root->solidRows.last().backgroundLayers;
                for (int column = 0; column < visibleColumns; ++column) {
                    appendHorizontalRect(backgrounds,
                                         QRectF(column * cellWidth, gridHeight,
                                                cellWidth, bottomExtent),
                                         bottomEdgeBackgrounds.at(column));
                }
                appendRect(
                    backgrounds,
                    QRectF(-leftExtent, gridHeight, leftExtent, bottomExtent),
                    bottomEdgeBackgrounds.first());
                appendRect(
                    backgrounds,
                    QRectF(gridWidth, gridHeight, rightExtent, bottomExtent),
                    bottomEdgeBackgrounds.last());
            }
        }
        if (cursorActive && frame.cursorRow < root->solidRows.size()) {
            const QVector<QColor> &cursorRowBackgrounds =
                root->solidRows.at(frame.cursorRow).backgroundLayers;
            if (frame.cursorColumn < cursorRowBackgrounds.size()) {
                cursorEffectiveBackground =
                    cursorRowBackgrounds.at(frame.cursorColumn);
            }
        }
        root->blockCursorTextState = blockCursorTextState;
        root->shapingCursorState = shapingCursorState;

        if (cursorActive) {
            const qreal left =
                static_cast<qreal>(frame.cursorColumn) * cellWidth;
            const qreal top = static_cast<qreal>(frame.cursorRow) * cellHeight;
            const qreal cursorWidth = cellWidth
                * static_cast<qreal>(std::max(1, frame.cursorColumnSpan));
            const qreal cursorTop = top + metrics.cursorTop;
            const QRectF cursorRect(left, cursorTop, cursorWidth,
                                    metrics.cursorHeight);
            const QRectF cursorCanvas =
                paddedSpriteCanvas(cursorRect, devicePixelRatio);
            const QRectF underlineCursorCell(left, top, cursorWidth,
                                             cellHeight);
            const QRectF underlineCursorCanvas =
                paddedSpriteCanvas(underlineCursorCell, devicePixelRatio);
            QColor cursorColor = frame.cursorColor;
            if (!frame.cursorColorExplicit) {
                cursorColor = resolveRelativeColor(
                    appearance.cursorColor, cursorCellForeground,
                    cursorCellBackground, frame.foreground);
            }
            cursorColor = withOpacity(cursorColor,
                                      focused ? appearance.cursorOpacity : 1.0);
            cursorColor = minimumContrastColor(
                cursorColor, cursorEffectiveBackground, globalBackground,
                appearance.minimumContrast);
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
            renderedCursorColor = cursorColor;
#endif
            const auto appendCursorRect = [&](QVector<ColoredRect> &rects,
                                              const QRectF &rect,
                                              const QRectF &canvas) {
                appendClippedRect(rects, rect, canvas, cursorColor,
                                  cursorProbe);
            };
            switch (cursorStyle) {
            case 0:
                appendCursorRect(cursorDecorations,
                                 QRectF(left + metrics.cursorBarLeft, cursorTop,
                                        metrics.cursorThickness,
                                        metrics.cursorHeight),
                                 cursorCanvas);
                break;
            case 2:
                appendCursorRect(cursorDecorations,
                                 QRectF(left, top + underlinePosition,
                                        cursorWidth, metrics.cursorThickness),
                                 underlineCursorCanvas);
                break;
            case 3: {
                const qreal thickness = metrics.cursorThickness;
                const qreal innerWidth =
                    std::max(qreal{0.0}, cursorWidth - 2.0 * thickness);
                const qreal innerHeight = std::max(
                    qreal{0.0}, metrics.cursorHeight - 2.0 * thickness);
                if (innerWidth <= 0.0 || innerHeight <= 0.0) {
                    appendCursorRect(cursorDecorations, cursorRect,
                                     cursorCanvas);
                    break;
                }

                const QRectF inner(left + thickness, cursorTop + thickness,
                                   innerWidth, innerHeight);
                // These four rectangles form an exact, non-overlapping
                // partition of outer minus inner. In particular, fractional
                // cursor opacity is applied once at the corners.
                appendCursorRect(cursorDecorations,
                                 QRectF(cursorRect.left(), cursorRect.top(),
                                        cursorRect.width(),
                                        inner.top() - cursorRect.top()),
                                 cursorCanvas);
                appendCursorRect(cursorDecorations,
                                 QRectF(cursorRect.left(), inner.bottom(),
                                        cursorRect.width(),
                                        cursorRect.bottom() - inner.bottom()),
                                 cursorCanvas);
                appendCursorRect(cursorDecorations,
                                 QRectF(cursorRect.left(), inner.top(),
                                        inner.left() - cursorRect.left(),
                                        inner.height()),
                                 cursorCanvas);
                appendCursorRect(cursorDecorations,
                                 QRectF(inner.right(), inner.top(),
                                        cursorRect.right() - inner.right(),
                                        inner.height()),
                                 cursorCanvas);
                break;
            }
            default:
                // Appended after cell backgrounds so a wide block cursor is
                // not overwritten by the spacer cell's background.
                appendCursorRect(cursorBackgrounds, cursorRect, cursorCanvas);
                break;
            }
        }

        if (!preedit.isEmpty()) {
            const qreal left =
                static_cast<qreal>(frame.cursorColumn) * cellWidth;
            const qreal top = static_cast<qreal>(frame.cursorRow) * cellHeight;
            const qreal preeditWidth = std::max(
                cellWidth, QFontMetricsF(baseFont).horizontalAdvance(preedit));
            appendRect(overlayBackgrounds,
                       QRectF(left, top, preeditWidth, cellHeight),
                       QColor(QStringLiteral("#3b4252")));
            overlayTextState.append({
                .text = preedit,
                .font = baseFont,
                .color = QColor(QStringLiteral("#eceff4")),
                .position = QPointF(left, top),
                .baseline = baseline,
                .lineWidth = preeditWidth,
            });
            appendUnderline(
                overlayDecorations, QRectF(left, top, preeditWidth, cellHeight),
                metrics.underlinePosition, metrics.underlineThickness,
                TerminalUnderlineStyle::Single,
                QColor(QStringLiteral("#eceff4")), devicePixelRatio,
                underlineProbe);
        }

        if (!status.isEmpty()) {
            const QFontMetricsF fontMetrics(baseFont);
            const qreal statusHeight = fontMetrics.height() + 12.0;
            const qreal statusTop = height() - statusHeight;
            appendRect(paneOverlayBackgrounds,
                       QRectF(0.0, statusTop, width(), statusHeight),
                       QColor(46, 52, 64, 230));
            paneOverlayTextState.append({
                .text = status,
                .font = baseFont,
                .color = QColor(QStringLiteral("#e5c07b")),
                .position = QPointF(8.0, statusTop),
                .baseline = statusHeight - 6.0 - fontMetrics.descent(),
                .lineWidth = std::max<qreal>(1.0, width() - 16.0),
            });
        }

        if (!linkPreview.isEmpty() && !linkPreviewRect.isEmpty()) {
            QColor previewBackground = frame.background;
            previewBackground.setAlpha(242);
            QColor previewForeground = frame.foreground;
            previewForeground.setAlpha(255);
            QColor previewOutline = previewForeground;
            previewOutline.setAlpha(100);
            appendRect(paneOverlayBackgrounds, linkPreviewRect,
                       previewBackground);

            constexpr qreal outlineWidth = 1.0;
            appendRect(paneOverlayDecorations,
                       QRectF(linkPreviewRect.left(), linkPreviewRect.top(),
                              linkPreviewRect.width(), outlineWidth),
                       previewOutline);
            appendRect(paneOverlayDecorations,
                       QRectF(linkPreviewRect.left(),
                              linkPreviewRect.bottom() - outlineWidth,
                              linkPreviewRect.width(), outlineWidth),
                       previewOutline);
            appendRect(paneOverlayDecorations,
                       QRectF(linkPreviewRect.left(), linkPreviewRect.top(),
                              outlineWidth, linkPreviewRect.height()),
                       previewOutline);
            appendRect(paneOverlayDecorations,
                       QRectF(linkPreviewRect.right() - outlineWidth,
                              linkPreviewRect.top(), outlineWidth,
                              linkPreviewRect.height()),
                       previewOutline);

            const QFontMetricsF fontMetrics(baseFont);
            paneOverlayTextState.append({
                .text = linkPreview,
                .font = baseFont,
                .color = previewForeground,
                .position =
                    QPointF(
                    linkPreviewRect.left()
                        + TerminalPaneRenderer::linkPreviewHorizontalPadding,
                    linkPreviewRect.top()
                        + TerminalPaneRenderer::linkPreviewVerticalPadding),
                .baseline = std::ceil(fontMetrics.ascent()),
                .lineWidth =
                    std::max<qreal>(
                        1.0,
                        linkPreviewRect.width()
                            - 2.0
                                * TerminalPaneRenderer::
                                    linkPreviewHorizontalPadding),
            });
        }
    }

    root->updateStartingText(startingTextState);
    root->updateOverlayText(overlayTextState);
    root->updatePaneOverlayText(paneOverlayTextState);
    root->commitRectLayers(softwareRenderer);

    QColor unfocusedSplitColor;
    const bool surfaceFocused =
        hasActiveFocus() && window() != nullptr && window()->isActive();
    if (split_ && !surfaceFocused && !searchUiActive_) {
        unfocusedSplitColor = splitAppearance.unfocusedFill.has_value()
                && splitAppearance.unfocusedFill->isValid()
            ? *splitAppearance.unfocusedFill
            : appearance.backgroundColor;
        if (unfocusedSplitColor.isValid()) {
            unfocusedSplitColor.setAlphaF(
                unfocusedSplitOverlayOpacity(splitAppearance.unfocusedOpacity));
        }
    }
    root->setUnfocusedSplitOverlay(viewport, unfocusedSplitColor);
#ifdef GHOSTTY_QT_RENDER_TEST_PROBE
    publishRenderProbe(this, *root, metrics, fontRoleCellCounts,
                       globalBackground, cellBackgroundLayers, glyphForegrounds,
                       decorationForegrounds, underlineColors,
                       renderedCursorColor, underlineRects, strikethroughRects,
                       overlineRects, cursorRects);
#endif
    return root;
}
