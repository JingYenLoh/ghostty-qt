#include "ghostty_shell_integration.h"
#include "ghostty_shell_integration_p.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcessEnvironment>
#include <QStringList>

#include <algorithm>
#include <barrier>
#include <chrono>
#include <cmath>
#include <expected>
#include <iostream>
#include <numeric>
#include <optional>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Preparation = std::expected<GhosttyShellIntegrationResult, QString>;

struct Samples {
    std::vector<double> microseconds;

    void print(const char *mode, const char *name) const
    {
        std::vector<double> sorted = microseconds;
        std::ranges::sort(sorted);
        const auto percentile = [&sorted](double fraction) {
            const size_t index = static_cast<size_t>(std::clamp(
                std::ceil(fraction * static_cast<double>(sorted.size())) - 1.0,
                0.0, static_cast<double>(sorted.size() - 1)));
            return sorted.at(index);
        };
        const double mean = std::accumulate(sorted.begin(), sorted.end(), 0.0)
            / static_cast<double>(sorted.size());
        std::cout << "mode=" << mode << " case=" << name
                  << " samples=" << sorted.size() << " mean_us=" << mean
                  << " p50_us=" << percentile(0.50)
                  << " p90_us=" << percentile(0.90) << '\n';
    }
};

TerminalEnvironment inheritedEnvironment()
{
    const QProcessEnvironment process =
        QProcessEnvironment::systemEnvironment();
    QStringList keys = process.keys();
    std::ranges::sort(keys);
    TerminalEnvironment result;
    result.reserve(keys.size());
    for (const QString &key : keys) {
        if (key == QLatin1StringView("GHOSTTY_QT_BENCH_VARIANT")) continue;
        result.append({
            .key = key.toUtf8(),
            .value = process.value(key).toUtf8(),
        });
    }
    return result;
}

QProcessEnvironment cacheableProcessEnvironment()
{
    QProcessEnvironment result = QProcessEnvironment::systemEnvironment();
    result.remove(QStringLiteral("LD_PRELOAD"));
    result.remove(QStringLiteral("LD_LIBRARY_PATH"));
    result.remove(QStringLiteral("LD_AUDIT"));
    return result;
}

GhosttyShellIntegrationRequest requestFor(GhosttyShellIntegrationMode mode)
{
    return {
        .command =
            TerminalCommand::shell(mode == GhosttyShellIntegrationMode::None
                                       ? QByteArrayLiteral("sh")
                                       : QByteArrayLiteral("bash"),
                                   true),
        .environment = inheritedEnvironment(),
        .mode = mode,
        .resourceDirectory = mode == GhosttyShellIntegrationMode::None
            ? QByteArray{}
            : QFile::encodeName(
                  QStringLiteral(GHOSTTY_QT_BENCH_SHELL_RESOURCES)),
    };
}

quint64 resultChecksum(const GhosttyShellIntegrationResult &result)
{
    quint64 checksum = static_cast<quint64>(result.command.shellCommand.size())
        + static_cast<quint64>(result.command.directArguments.size());
    for (const TerminalEnvironmentEntry &entry : result.environment) {
        checksum += static_cast<quint64>(entry.key.size() + entry.value.size());
    }
    return checksum + static_cast<quint64>(result.shell.has_value());
}

template <typename Operation>
bool measureSequential(int iterations, Samples *samples, quint64 *checksum,
                       Operation operation)
{
    samples->microseconds.reserve(samples->microseconds.size()
                                  + static_cast<size_t>(iterations));
    for (int index = 0; index < iterations; ++index) {
        const auto started = Clock::now();
        Preparation result = operation();
        const auto finished = Clock::now();
        if (!result.has_value()) {
            std::cerr << result.error().toStdString() << '\n';
            return false;
        }
        samples->microseconds.push_back(
            std::chrono::duration<double, std::micro>(finished - started)
                .count());
        *checksum += resultChecksum(*result);
    }
    return true;
}

bool measureBurst(const GhosttyShellIntegrationProcessOptions &options,
                  const GhosttyShellIntegrationRequest &baseRequest, int width,
                  bool distinct, Samples *samples, quint64 *checksum)
{
    std::barrier<> start(static_cast<std::ptrdiff_t>(width + 1));
    std::vector<std::optional<Preparation>> outcomes(
        static_cast<size_t>(width));
    std::vector<std::jthread> threads;
    threads.reserve(static_cast<size_t>(width));
    for (int index = 0; index < width; ++index) {
        GhosttyShellIntegrationRequest request = baseRequest;
        if (distinct) {
            request.environment.append({
                .key = QByteArrayLiteral("GHOSTTY_QT_BENCH_VARIANT"),
                .value = QByteArray::number(index),
            });
        }
        threads.emplace_back(
            [&, index, request = std::move(request)]() mutable {
                start.arrive_and_wait();
                outcomes.at(static_cast<size_t>(index)) =
                    prepareCachedGhosttyShellIntegration(options, request);
            });
    }
    const auto started = Clock::now();
    start.arrive_and_wait();
    threads.clear();
    const auto finished = Clock::now();
    for (const auto &outcome : outcomes) {
        if (!outcome.has_value() || !outcome->has_value()) {
            std::cerr << (outcome.has_value() ? outcome->error().toStdString()
                                              : "worker returned no outcome")
                      << '\n';
            return false;
        }
        *checksum += resultChecksum(**outcome);
    }
    samples->microseconds.push_back(
        std::chrono::duration<double, std::micro>(finished - started).count());
    return true;
}

bool validateBurst(int width, bool distinct)
{
    const auto snapshot = ghosttyShellIntegrationCacheSnapshotForTest();
    const quint64 expectedWidth = static_cast<quint64>(width);
    const bool valid = distinct
        ? snapshot.bypasses == 0 && snapshot.launches == expectedWidth
            && snapshot.misses == expectedWidth && snapshot.coalesced == 0
            && snapshot.hits == 0 && snapshot.entries == width
        : snapshot.bypasses == 0 && snapshot.launches == 1
            && snapshot.misses == 1
            && snapshot.hits + snapshot.coalesced == expectedWidth - 1
            && snapshot.entries == 1;
    if (!valid) {
        std::cerr << (distinct ? "distinct" : "identical")
                  << " burst did not exercise the expected cache path\n";
    }
    return valid;
}

bool benchmarkMode(const char *name, GhosttyShellIntegrationMode mode,
                   const GhosttyShellIntegrationProcessOptions &options,
                   int iterations, int bursts, quint64 *checksum)
{
    const GhosttyShellIntegrationRequest request = requestFor(mode);
    Samples uncached;
    if (!measureSequential(iterations, &uncached, checksum, [&] {
            return prepareGhosttyShellIntegration(options, request);
        })) {
        return false;
    }
    uncached.print(name, "uncached");

    if (!resetGhosttyShellIntegrationCacheForTest()) return false;
    Samples cold;
    if (!measureSequential(1, &cold, checksum, [&] {
            return prepareCachedGhosttyShellIntegration(options, request);
        })) {
        return false;
    }
    cold.print(name, "cached-cold");
    Samples warm;
    if (!measureSequential(iterations, &warm, checksum, [&] {
            return prepareCachedGhosttyShellIntegration(options, request);
        })) {
        return false;
    }
    warm.print(name, "cached-warm");
    const auto warmSnapshot = ghosttyShellIntegrationCacheSnapshotForTest();
    if (warmSnapshot.bypasses != 0 || warmSnapshot.misses != 1
        || warmSnapshot.launches != 1
        || warmSnapshot.hits != static_cast<quint64>(iterations)) {
        std::cerr << "cached-warm case did not exercise the cache\n";
        return false;
    }
    std::cout << "mode=" << name << " cache_hits=" << warmSnapshot.hits
              << " launches=" << warmSnapshot.launches
              << " retained_bytes=" << warmSnapshot.retainedBytes << '\n';

    constexpr int Width = 8;
    Samples identical;
    for (int index = 0; index < bursts; ++index) {
        if (!resetGhosttyShellIntegrationCacheForTest()
            || !measureBurst(options, request, Width, false, &identical,
                             checksum)
            || !validateBurst(Width, false)) {
            return false;
        }
    }
    identical.print(name, "burst-identical-8");
    const auto identicalSnapshot =
        ghosttyShellIntegrationCacheSnapshotForTest();
    std::cout << "mode=" << name
              << " burst_identical_launches=" << identicalSnapshot.launches
              << " coalesced=" << identicalSnapshot.coalesced << '\n';

    Samples distinct;
    for (int index = 0; index < bursts; ++index) {
        if (!resetGhosttyShellIntegrationCacheForTest()
            || !measureBurst(options, request, Width, true, &distinct, checksum)
            || !validateBurst(Width, true)) {
            return false;
        }
    }
    distinct.print(name, "burst-distinct-8");
    const auto distinctSnapshot = ghosttyShellIntegrationCacheSnapshotForTest();
    std::cout << "mode=" << name
              << " burst_distinct_launches=" << distinctSnapshot.launches
              << " coalesced=" << distinctSnapshot.coalesced << '\n';
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    Q_UNUSED(application);
    const int configuredIterations =
        qEnvironmentVariableIntValue("GHOSTTY_QT_BENCH_ITERATIONS");
    const int effectiveIterations =
        configuredIterations > 0 ? configuredIterations : 25;
    const int configuredBursts =
        qEnvironmentVariableIntValue("GHOSTTY_QT_BENCH_BURSTS");
    const int bursts = configuredBursts > 0 ? configuredBursts : 5;
    const GhosttyShellIntegrationProcessOptions options{
        .helperPath = QStringLiteral(GHOSTTY_QT_BENCH_SHELL_HELPER),
        .environment = cacheableProcessEnvironment(),
        .timeoutMilliseconds = 2'000,
    };

    quint64 checksum = 0;
    if (!benchmarkMode("none", GhosttyShellIntegrationMode::None, options,
                       effectiveIterations, bursts, &checksum)
        || !benchmarkMode("bash", GhosttyShellIntegrationMode::Bash, options,
                          effectiveIterations, bursts, &checksum)) {
        return 1;
    }
    std::cout << "checksum=" << checksum << '\n';
    return 0;
}
