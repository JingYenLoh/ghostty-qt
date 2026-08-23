#pragma once

#include <QString>

// A path finalized by Ghostty relative to its declaring config source. The
// provenance bit controls whether an inaccessible optional resource is quiet.
struct GhosttyConfigPath {
    QString path;
    bool optional = false;

    bool operator==(const GhosttyConfigPath &) const = default;
};
