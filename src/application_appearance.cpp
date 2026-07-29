#include "application_appearance.h"

#include <QtGlobal>

namespace {

double perceivedLuminance(const QColor &color) noexcept
{
    if (!color.isValid()) return 0.0;
    // Ghostty's color utility uses the AERT coefficients over 8-bit RGB.
    return (0.299 * color.red() + 0.587 * color.green()
            + 0.114 * color.blue())
        / 255.0;
}

} // namespace

ApplicationAppearance::ApplicationAppearance(TerminalColorScheme systemScheme)
    : systemScheme_(systemScheme)
    , colorScheme_(systemScheme)
{}

bool ApplicationAppearance::setSystemColorScheme(TerminalColorScheme scheme)
{
    if (systemScheme_ == scheme) return false;
    systemScheme_ = scheme;
    return resolve();
}

bool ApplicationAppearance::apply(const WindowAppearanceOptions &options,
                                  const QColor &configuredBackground)
{
    if (options_ == options && configuredBackground_ == configuredBackground) {
        return false;
    }
    options_ = options;
    configuredBackground_ = configuredBackground;
    return resolve();
}

TerminalColorScheme
ApplicationAppearance::fromQtColorScheme(Qt::ColorScheme scheme) noexcept
{
    // Ghostty's conditional state begins light. Normalize Unknown to the same
    // concrete value so the config and terminal protocols never receive an
    // indeterminate state.
    return scheme == Qt::ColorScheme::Dark ? TerminalColorScheme::Dark
                                           : TerminalColorScheme::Light;
}

TerminalColorScheme ApplicationAppearance::resolveColorScheme(
    WindowTheme theme, const QColor &configuredBackground,
    TerminalColorScheme systemScheme) noexcept
{
    switch (theme) {
    case WindowTheme::System: return systemScheme;
    case WindowTheme::Light: return TerminalColorScheme::Light;
    case WindowTheme::Dark: return TerminalColorScheme::Dark;
    case WindowTheme::Auto:
    case WindowTheme::Ghostty:
        return perceivedLuminance(configuredBackground) > 0.5
            ? TerminalColorScheme::Light
            : TerminalColorScheme::Dark;
    }
    Q_UNREACHABLE_RETURN(systemScheme);
}

bool ApplicationAppearance::resolve()
{
    const TerminalColorScheme resolved = resolveColorScheme(
        options_.theme, configuredBackground_, systemScheme_);
    if (colorScheme_ == resolved) return false;
    colorScheme_ = resolved;
    return true;
}
