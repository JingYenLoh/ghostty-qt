#pragma once

#include "terminal_session_options.h"

#include <QMetaType>
#include <QString>
#include <QtGlobal>

#include <optional>

enum class TerminalActionOutcome : quint8 {
    Success,
    Unavailable,
    Failed,
};

enum class TerminalActionEffect : quint8 {
    None,
    Clipboard,
    OpenFile,
};

struct TerminalActionResult {
    quint64 requestId = 0;
    TerminalActionOutcome outcome = TerminalActionOutcome::Failed;
    TerminalActionEffect effect = TerminalActionEffect::None;
    bool performed = false;
    QString payload;
    TerminalClipboardDestination clipboardDestination =
        TerminalClipboardDestination::Standard;

    friend bool operator==(const TerminalActionResult &,
                           const TerminalActionResult &) = default;
};

Q_DECLARE_METATYPE(TerminalActionResult)

struct TerminalActionExecutionResult {
    bool performed = false;
    std::optional<TerminalActionResult> terminalAction;
};

struct TerminalActionExecutionStart {
    bool pending = false;
    TerminalActionExecutionResult result;
};
