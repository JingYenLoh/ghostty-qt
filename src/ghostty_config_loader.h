#pragma once

#include "ghostty_config_snapshot.h"
#include "terminal_color_scheme.h"

#include <QString>
#include <QStringList>

#include <expected>
#include <functional>

using GhosttyConfigLoadResult = std::expected<GhosttyConfigSnapshot, QString>;

struct GhosttyConfigLoadRequest {
    QStringList candidatePaths;
    TerminalColorScheme colorScheme = TerminalColorScheme::Light;

    friend bool operator==(const GhosttyConfigLoadRequest &,
                           const GhosttyConfigLoadRequest &) = default;
};

// Candidate paths are ordered in Ghostty load order: earlier files are loaded
// first and later files override them. The loader owns parsing and Ghostty
// integration; the service controls lifecycle, watching, and publication of
// last-known-good snapshots.
using GhosttyConfigLoader = std::function<GhosttyConfigLoadResult(
    const GhosttyConfigLoadRequest &request)>;
