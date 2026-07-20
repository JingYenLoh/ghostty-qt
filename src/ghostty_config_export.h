#pragma once

#include "ghostty_keybind_config.h"

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <expected>
#include <optional>

// Lossless project-private values exported from one pinned Ghostty Config
// generation. Milliseconds intentionally match Duration.asMilliseconds(),
// which is the conversion used by the pinned Linux GTK frontend.
struct GhosttyConfigExport {
    static constexpr int CurrentSchemaVersion = 1;
    static_assert(GhosttyKeybindConfig::CurrentSchemaVersion
                  == CurrentSchemaVersion);

    int schemaVersion = CurrentSchemaVersion;
    GhosttyKeybindConfig keybindings;
    bool quitAfterLastWindowClosed = true;
    std::optional<quint32> quitAfterLastWindowClosedDelayMilliseconds;

    bool operator==(const GhosttyConfigExport &) const = default;
};

// Parses the exact versioned JSON contract emitted by
// `ghostty-qt-config-helper +show-config-json`. Failure carries a diagnostic
// and cannot expose a partially parsed configuration.
[[nodiscard]] std::expected<GhosttyConfigExport, QString>
parseGhosttyConfigExportJson(const QByteArray &json);
