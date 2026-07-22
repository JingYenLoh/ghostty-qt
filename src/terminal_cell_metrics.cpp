#include "terminal_cell_metrics.h"

#include <QFontDatabase>
#include <QFontMetricsF>

#include <algorithm>
#include <cmath>

TerminalCellMetrics terminalCellMetrics(const QFont &font)
{
    const QFontMetricsF metrics(font);
    const qreal cellWidth = std::max<qreal>(
        1.0, std::ceil(metrics.horizontalAdvance(QLatin1Char('M'))));
    const qreal cellHeight = std::max<qreal>(
        1.0, std::ceil(metrics.height()));

    return {
        .font = font,
        .cellWidth = cellWidth,
        .cellHeight = cellHeight,
        .baseline = std::ceil(
            metrics.ascent() + (cellHeight - metrics.height()) / 2.0),
    };
}

QString resolveTerminalFontFamily(const QString &configuredFamily)
{
    return configuredFamily.isEmpty()
        ? QFontDatabase::systemFont(QFontDatabase::FixedFont).family()
        : configuredFamily;
}

TerminalCellMetrics terminalCellMetrics(const QString &configuredFamily,
                                        qreal pointSize)
{
    QFont font;
    font.setFamily(resolveTerminalFontFamily(configuredFamily));
    font.setPointSizeF(pointSize);
    font.setFixedPitch(true);
    font.setStyleHint(QFont::Monospace);
    return terminalCellMetrics(font);
}
