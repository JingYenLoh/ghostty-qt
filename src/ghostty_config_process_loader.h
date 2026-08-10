#pragma once

#include "ghostty_config_loader.h"

#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <expected>

struct GhosttyConfigProcessLoaderOptions {
    QString helperPath;
    // Ghostty finalizes an unset working-directory differently for a likely
    // CLI launch. Capture the original application process fact; the helper's
    // own private action must not reclassify an argc-1 desktop launch.
    bool probableCli = true;
    // Already-encoded Ghostty configuration CLI arguments. Both structured
    // queries receive their private action and selected color scheme first,
    // followed by these values in their original order. Each private query is
    // strict and validates the complete, effective, CLI-aware generation.
    QStringList configurationArguments;
    // Optional mixed ghostty-qt configuration. The helper filters Qt-owned
    // keys and applies every remaining line through the pinned Ghostty parser
    // after the shared configuration, then reapplies explicit CLI values.
    QString frontendConfigPath;
    int timeoutMilliseconds = 5'000;
    // One load runs two identical complete JSON queries and compares their
    // bytes. Each operation receives only the remaining transaction budget,
    // and no helper starts after it expires; forced child teardown may add a
    // small amount of wall-clock latency.
    int overallTimeoutMilliseconds = 6'000;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
};

// Creates an exact Ghostty-backed loader. The helper is the small executable
// linked to the pinned ghostty-internal library; the Qt application never
// links that unstable API into its own process.
GhosttyConfigLoader
makeGhosttyConfigProcessLoader(GhosttyConfigProcessLoaderOptions options);

// Candidate paths describe Ghostty's legacy file followed by its preferred
// file. This returns the XDG root which makes the helper load those files, or
// a stable error message when they cannot be represented by Ghostty's standard
// Linux config lookup.
std::expected<QString, QString>
ghosttyConfigXdgHome(const QStringList &candidatePaths);
