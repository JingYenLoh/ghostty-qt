#pragma once

#include "config/ghostty_config_values.h"

#include <memory>
#include <optional>

// The low-level device boundary keeps platform and multimedia side effects out
// of pane tests. Implementations remain GUI-thread owned; calls are synchronous
// and must not invoke user callbacks or re-enter the owning player.
class TerminalBellDevice {
public:
    virtual ~TerminalBellDevice() = default;

    virtual void ringSystemBell() = 0;
    // Returns false when the source cannot be prepared. Callers retry a failed
    // source on the next BEL so a file created later becomes usable.
    [[nodiscard]] virtual bool
    setAudioSource(const GhosttyConfigPath &source) = 0;
    virtual void setAudioVolume(double volume) = 0;
    // Invalid media returns false so the orchestrator rebuilds the source on a
    // later BEL. Loading media may accept play and return true.
    [[nodiscard]] virtual bool restartAudio() = 0;
};

// One player belongs to one terminal pane. It preserves Ghostty's independent
// system/audio features, reuses a successfully prepared media pipeline for the
// same finalized path, and replays it for every BEL.
class TerminalBellPlayer final {
public:
    TerminalBellPlayer();
    explicit TerminalBellPlayer(std::unique_ptr<TerminalBellDevice> device);
    ~TerminalBellPlayer();

    TerminalBellPlayer(const TerminalBellPlayer &) = delete;
    TerminalBellPlayer &operator=(const TerminalBellPlayer &) = delete;
    TerminalBellPlayer(TerminalBellPlayer &&) = delete;
    TerminalBellPlayer &operator=(TerminalBellPlayer &&) = delete;

    void setDevice(std::unique_ptr<TerminalBellDevice> device);
    void ring(const BellFeatures &features,
              const std::optional<GhosttyConfigPath> &audioPath,
              double audioVolume);

private:
    std::unique_ptr<TerminalBellDevice> device_;
    std::optional<GhosttyConfigPath> preparedAudioSource_;
    bool audioSourceReady_ = false;
};
