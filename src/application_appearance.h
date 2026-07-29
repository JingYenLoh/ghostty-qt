#pragma once

#include "ghostty_config_values.h"
#include "terminal_color_scheme.h"

#include <QColor>
#include <QStyleHints>

// Process-owned resolution of Ghostty's window-theme policy. The class is a
// regular value rather than a QObject so tests can supply deterministic
// platform changes and the application keeps exactly one QStyleHints
// connection. It does not mutate Qt's global palette or style hints.
class ApplicationAppearance final {
public:
    explicit ApplicationAppearance(
        TerminalColorScheme systemScheme = TerminalColorScheme::Light);

    [[nodiscard]] bool setSystemColorScheme(TerminalColorScheme scheme);
    [[nodiscard]] bool apply(const WindowAppearanceOptions &options,
                             const QColor &configuredBackground);

    [[nodiscard]] TerminalColorScheme systemColorScheme() const noexcept
    {
        return systemScheme_;
    }

    [[nodiscard]] TerminalColorScheme colorScheme() const noexcept
    {
        return colorScheme_;
    }

    [[nodiscard]] static TerminalColorScheme
    fromQtColorScheme(Qt::ColorScheme scheme) noexcept;
    [[nodiscard]] static TerminalColorScheme
    resolveColorScheme(WindowTheme theme, const QColor &configuredBackground,
                       TerminalColorScheme systemScheme) noexcept;

private:
    [[nodiscard]] bool resolve();

    TerminalColorScheme systemScheme_;
    TerminalColorScheme colorScheme_;
    WindowAppearanceOptions options_;
    QColor configuredBackground_;
};
