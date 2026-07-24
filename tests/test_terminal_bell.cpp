#include "terminal_bell.h"

#include <QList>
#include <QStringList>
#include <QTest>

#include <limits>
#include <memory>
#include <utility>

namespace {

struct BellDeviceState {
    QStringList calls;
    QList<GhosttyConfigPath> sources;
    QList<double> volumes;
    bool sourceReady = true;
    bool restartReady = true;
};

class FakeBellDevice final : public TerminalBellDevice {
public:
    explicit FakeBellDevice(BellDeviceState &state)
        : state_(state)
    {}

    void ringSystemBell() override
    {
        state_.calls.append(QStringLiteral("system"));
    }

    bool setAudioSource(const GhosttyConfigPath &source) override
    {
        state_.calls.append(QStringLiteral("source"));
        state_.sources.append(source);
        return state_.sourceReady;
    }

    void setAudioVolume(double volume) override
    {
        state_.calls.append(QStringLiteral("volume"));
        state_.volumes.append(volume);
    }

    bool restartAudio() override
    {
        state_.calls.append(QStringLiteral("restart"));
        return state_.restartReady;
    }

private:
    BellDeviceState &state_;
};

TerminalBellPlayer playerFor(BellDeviceState &state)
{
    return TerminalBellPlayer(std::make_unique<FakeBellDevice>(state));
}

} // namespace

class TerminalBellTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void dispatchesOnlyEnabledEffectsForEveryBell();
    void reusesPreparedSourcesAndAppliesLiveVolume();
    void retriesFailedSourcesAndResetsWithDevice();
    void playersKeepIndependentPipelines();
};

void TerminalBellTest::dispatchesOnlyEnabledEffectsForEveryBell()
{
    BellDeviceState state;
    TerminalBellPlayer player = playerFor(state);
    const GhosttyConfigPath path{
        .path = QStringLiteral("/tmp/ghostty-qt-bell.ogg"),
        .optional = false,
    };

    BellFeatures features;
    player.ring(features, path, 0.5);
    QVERIFY(state.calls.isEmpty());

    features.system = true;
    player.ring(features, std::nullopt, 0.5);
    QCOMPARE(state.calls, QStringList{QStringLiteral("system")});

    state.calls.clear();
    features.audio = true;
    player.ring(features, path, 0.25);
    QCOMPARE(state.calls,
             QStringList({
                 QStringLiteral("system"),
                 QStringLiteral("source"),
                 QStringLiteral("volume"),
                 QStringLiteral("restart"),
             }));
    QVERIFY(state.sources == QList<GhosttyConfigPath>{path});
    QCOMPARE(state.volumes, QList<double>{0.25});

    state.calls.clear();
    player.ring(features, path, 0.25);
    QCOMPARE(state.calls,
             QStringList({
                 QStringLiteral("system"),
                 QStringLiteral("volume"),
                 QStringLiteral("restart"),
             }));
}

void TerminalBellTest::reusesPreparedSourcesAndAppliesLiveVolume()
{
    BellDeviceState state;
    TerminalBellPlayer player = playerFor(state);
    BellFeatures features;
    features.audio = true;
    const GhosttyConfigPath required{
        .path = QStringLiteral("/tmp/ghostty-qt-bell.ogg"),
        .optional = false,
    };

    player.ring(features, required, -4.0);
    player.ring(features, required, 4.0);
    QVERIFY(state.sources == QList<GhosttyConfigPath>{required});
    QCOMPARE(state.volumes, QList<double>({0.0, 1.0}));

    const GhosttyConfigPath optional{
        .path = required.path,
        .optional = true,
    };
    player.ring(features, optional, std::numeric_limits<double>::quiet_NaN());
    QVERIFY(state.sources == QList<GhosttyConfigPath>({required, optional}));
    QCOMPARE(state.volumes, QList<double>({0.0, 1.0, 0.5}));

    // Disabling audio retains the cached pipeline without touching it. The
    // same source can replay immediately when the feature is enabled again.
    features.audio = false;
    state.calls.clear();
    player.ring(features, optional, 0.75);
    QVERIFY(state.calls.isEmpty());

    features.audio = true;
    player.ring(features, optional, 0.75);
    QCOMPARE(state.calls,
             QStringList({
                 QStringLiteral("volume"),
                 QStringLiteral("restart"),
             }));
}

void TerminalBellTest::retriesFailedSourcesAndResetsWithDevice()
{
    BellDeviceState first;
    first.sourceReady = false;
    TerminalBellPlayer player = playerFor(first);
    BellFeatures features;
    features.audio = true;
    const GhosttyConfigPath source{
        .path = QStringLiteral("/tmp/created-after-first-bell.wav"),
        .optional = true,
    };

    player.ring(features, source, 0.5);
    player.ring(features, source, 0.5);
    QVERIFY(first.sources == QList<GhosttyConfigPath>({source, source}));
    QVERIFY(first.volumes.isEmpty());
    QVERIFY(!first.calls.contains(QStringLiteral("restart")));

    first.sourceReady = true;
    player.ring(features, source, 0.5);
    player.ring(features, source, 0.5);
    QVERIFY(first.sources
            == QList<GhosttyConfigPath>({source, source, source}));
    QCOMPARE(first.volumes, QList<double>({0.5, 0.5}));

    BellDeviceState replacement;
    player.setDevice(std::make_unique<FakeBellDevice>(replacement));
    player.ring(features, source, 0.5);
    QVERIFY(replacement.sources == QList<GhosttyConfigPath>{source});
    QCOMPARE(replacement.volumes, QList<double>{0.5});

    // An asynchronously invalidated player clears the ready cache so a later
    // BEL reconstructs the source instead of retaining a dead pipeline.
    replacement.restartReady = false;
    player.ring(features, source, 0.5);
    player.ring(features, source, 0.5);
    QCOMPARE(replacement.sources.size(), 2);
    replacement.restartReady = true;
    player.ring(features, source, 0.5);
    QCOMPARE(replacement.sources.size(), 3);
    player.ring(features, source, 0.5);
    QCOMPARE(replacement.sources.size(), 3);
}

void TerminalBellTest::playersKeepIndependentPipelines()
{
    BellDeviceState firstState;
    BellDeviceState secondState;
    TerminalBellPlayer first = playerFor(firstState);
    TerminalBellPlayer second = playerFor(secondState);
    BellFeatures features;
    features.audio = true;
    const GhosttyConfigPath source{
        .path = QStringLiteral("/tmp/shared-bell.oga"),
        .optional = false,
    };

    first.ring(features, source, 0.2);
    second.ring(features, source, 0.8);
    first.ring(features, source, 0.4);

    QCOMPARE(firstState.sources.size(), 1);
    QCOMPARE(firstState.volumes, QList<double>({0.2, 0.4}));
    QCOMPARE(firstState.calls.count(QStringLiteral("restart")), 2);
    QCOMPARE(secondState.sources.size(), 1);
    QCOMPARE(secondState.volumes, QList<double>{0.8});
    QCOMPARE(secondState.calls.count(QStringLiteral("restart")), 1);
}

QTEST_APPLESS_MAIN(TerminalBellTest)

#include "test_terminal_bell.moc"
