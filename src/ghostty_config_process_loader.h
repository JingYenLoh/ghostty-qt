#pragma once

#include "ghostty_config_loader.h"

#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

struct GhosttyConfigProcessLoaderOptions {
    QString helperPath;
    int timeoutMilliseconds = 5'000;
    // One load runs validation, three extraction queries, post-query
    // validation, and two consistency queries. Bound the whole transaction so
    // application shutdown cannot inherit seven independent timeouts.
    int overallTimeoutMilliseconds = 6'000;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
};

// Creates an exact Ghostty-backed loader. The helper is the small executable
// linked to the pinned ghostty-internal library; the Qt application never
// links that unstable API into its own process.
GhosttyConfigLoader makeGhosttyConfigProcessLoader(
    GhosttyConfigProcessLoaderOptions options);

// Exposed for contract tests and for diagnosing output drift when the pinned
// Ghostty revision changes. `defaultOutput` is from `+show-config --default`;
// `changesOutput` is from the ordinary changes-only `+show-config` action.
GhosttyConfigLoadResult parseGhosttyConfigShowOutputs(
    const QByteArray &defaultOutput,
    const QByteArray &changesOutput,
    const QStringList &candidatePaths);

// Candidate paths describe Ghostty's legacy file followed by its preferred
// file. This returns the XDG root which makes the helper load those files, or
// an empty string and a stable error message when they cannot be represented
// by Ghostty's standard Linux config lookup.
QString ghosttyConfigXdgHome(const QStringList &candidatePaths,
                             QString *errorMessage = nullptr);
