#pragma once

#include "ghostty_config_values.h"
#include "ghostty_keybind_config.h"

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <expected>
#include <utility>

// One complete frontend configuration projection exported from a single
// pinned Ghostty Config generation. The platform-default binding set is kept
// beside the current set so compatibility diagnostics do not mistake Ghostty
// built-ins for user configuration. Private construction prevents a loader
// from manufacturing a partial generation outside the strict parser.
struct GhosttyConfigExport {
    static constexpr int CurrentSchemaVersion = 3;

    GhosttyConfigValues values;
    GhosttyKeybindConfig keybindings;
    GhosttyKeybindConfig defaultKeybindings;

    bool operator==(const GhosttyConfigExport &) const = default;

private:
    GhosttyConfigExport(GhosttyConfigValues exportedValues,
                        GhosttyKeybindConfig exportedKeybindings,
                        GhosttyKeybindConfig exportedDefaultKeybindings)
        : values(std::move(exportedValues))
        , keybindings(std::move(exportedKeybindings))
        , defaultKeybindings(std::move(exportedDefaultKeybindings))
    {}

    friend std::expected<GhosttyConfigExport, QString>
    parseGhosttyConfigExportJson(const QByteArray &json);
};

// Parses the exact versioned JSON contract emitted by
// `ghostty-qt-config-helper +show-config-json`. Failure carries a diagnostic
// and cannot expose a partially parsed configuration.
[[nodiscard]] std::expected<GhosttyConfigExport, QString>
parseGhosttyConfigExportJson(const QByteArray &json);
