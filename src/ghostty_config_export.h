#pragma once

#include "ghostty_keybind_config.h"

#include <QByteArray>
#include <QString>
#include <QVariantMap>
#include <QtGlobal>

#include <expected>

// One complete frontend configuration projection exported from a single
// pinned Ghostty Config generation. The platform-default binding set is kept
// beside the current set so compatibility diagnostics do not mistake Ghostty
// built-ins for user configuration.
struct GhosttyConfigExport {
    static constexpr int CurrentSchemaVersion = 1;
    static_assert(GhosttyKeybindConfig::CurrentSchemaVersion
                  == CurrentSchemaVersion);

    QVariantMap values;
    GhosttyKeybindConfig keybindings;
    GhosttyKeybindConfig defaultKeybindings;

    bool operator==(const GhosttyConfigExport &) const = default;
};

// Parses the exact versioned JSON contract emitted by
// `ghostty-qt-config-helper +show-config-json`. Failure carries a diagnostic
// and cannot expose a partially parsed configuration.
[[nodiscard]] std::expected<GhosttyConfigExport, QString>
parseGhosttyConfigExportJson(const QByteArray &json);
