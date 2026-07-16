#pragma once

#include "ghostty_config_snapshot.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <utility>

struct GhosttyConfigLoadResult {
    std::optional<GhosttyConfigSnapshot> snapshot;
    QString errorMessage;

    static GhosttyConfigLoadResult loaded(GhosttyConfigSnapshot value)
    {
        return GhosttyConfigLoadResult{std::move(value), {}};
    }

    static GhosttyConfigLoadResult failed(QString message)
    {
        return GhosttyConfigLoadResult{std::nullopt, std::move(message)};
    }

    bool succeeded() const { return snapshot.has_value(); }
};

// Candidate paths are ordered in Ghostty load order: earlier files are loaded
// first and later files override them. The loader owns parsing and Ghostty
// integration; the service controls lifecycle, watching, and publication of
// last-known-good snapshots.
using GhosttyConfigLoader =
    std::function<GhosttyConfigLoadResult(const QStringList &candidatePaths)>;
