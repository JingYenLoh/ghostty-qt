#pragma once

#include <QMetaType>
#include <QtGlobal>

#include <optional>

enum class TerminalProgressState : quint8 {
    Remove,
    Set,
    Error,
    Indeterminate,
    Pause,
};

// One parser-normalized OSC 9;4 report. A missing percentage is distinct from
// zero and lets the presentation retain state where the protocol requires it.
struct TerminalProgressReport {
    TerminalProgressState state = TerminalProgressState::Remove;
    std::optional<quint8> progress;
    // Ordinary parser reports leave this empty and let the pane derive one
    // pulse for an activity-producing state. Overflow compaction supplies an
    // exact modulo-period pulse count so bounded delivery preserves the final
    // activity phase without replaying hundreds of intermediate frames.
    std::optional<quint8> activityPulses = std::nullopt;

    friend bool operator==(const TerminalProgressReport &,
                           const TerminalProgressReport &) = default;
};

Q_DECLARE_METATYPE(TerminalProgressReport)
