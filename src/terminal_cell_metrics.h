#pragma once

#include <QFont>
#include <QString>
#include <QtTypes>

struct TerminalCellMetrics {
    QFont font;
    qreal cellWidth = 1.0;
    qreal cellHeight = 1.0;
    qreal baseline = 1.0;
};

// These helpers use Qt's GUI font database and metrics. Call them only on the
// GUI thread after constructing QGuiApplication.
[[nodiscard]] QString resolveTerminalFontFamily(
    const QString &configuredFamily);

[[nodiscard]] TerminalCellMetrics terminalCellMetrics(
    const QString &configuredFamily, qreal pointSize);

// Preserve an already selected font while deriving the same integral logical
// pixel geometry used by TerminalPane's renderer and terminal resize path.
[[nodiscard]] TerminalCellMetrics terminalCellMetrics(const QFont &font);
