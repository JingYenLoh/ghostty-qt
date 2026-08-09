#pragma once

#include "ghostty_shell_integration.h"

#include <QString>
#include <QtTypes>

// Internal deterministic observability for focused tests and microbenchmarks.
// Production behavior does not depend on these counters.
struct GhosttyShellIntegrationCacheSnapshot {
    quint64 hits = 0;
    quint64 misses = 0;
    quint64 coalesced = 0;
    quint64 launches = 0;
    quint64 insertions = 0;
    quint64 evictions = 0;
    quint64 bypasses = 0;
    quint64 oversizedResults = 0;
    quint64 unstableIdentities = 0;
    qsizetype entries = 0;
    qsizetype retainedBytes = 0;
    qsizetype inFlight = 0;
};

[[nodiscard]] GhosttyShellIntegrationCacheSnapshot
ghosttyShellIntegrationCacheSnapshotForTest();

// Production launch-time entry point. Successful equivalent preparations are
// retained in a bounded process-wide cache, and concurrent misses for one
// identity share a single helper transaction. Only the expected production
// helper/runtime layout is eligible; tests can explicitly trust one fixture.
[[nodiscard]] std::expected<GhosttyShellIntegrationResult, QString>
prepareCachedGhosttyShellIntegration(
    const GhosttyShellIntegrationProcessOptions &options,
    const GhosttyShellIntegrationRequest &request);

[[nodiscard]] bool
setGhosttyShellIntegrationTrustedHelperForTest(const QString &absolutePath);

// The caller must ensure no cache operation is active on another thread. The
// pending-miss guard returns false rather than disturbing known waiters.
[[nodiscard]] bool resetGhosttyShellIntegrationCacheForTest();
