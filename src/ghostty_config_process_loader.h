#pragma once

#include "ghostty_config_loader.h"

#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <expected>

struct GhosttyConfigProcessLoaderOptions {
    QString helperPath;
    // Already-encoded Ghostty configuration CLI arguments. Both structured
    // queries receive their private action first followed by these values in
    // their original order. Pinned +validate-config accepts only its own
    // action options, so the separate file validators retain that exact public
    // grammar; each structured query validates the effective CLI generation.
    QStringList configurationArguments;
    int timeoutMilliseconds = 5'000;
    // One load runs validation, a complete JSON query, post-query validation,
    // and one JSON consistency query. Each operation receives only the
    // remaining transaction budget, and no helper starts after it expires;
    // forced child teardown may add a small amount of wall-clock latency.
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
