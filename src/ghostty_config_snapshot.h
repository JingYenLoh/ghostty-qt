#pragma once

#include "ghostty_config_export.h"

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

#include <utility>

enum class GhosttyConfigDiagnosticSeverity {
    Warning,
    Error,
};

struct GhosttyConfigDiagnostic {
    GhosttyConfigDiagnosticSeverity severity = GhosttyConfigDiagnosticSeverity::Warning;
    QString message;
    QString sourcePath;
    int line = 0;
    int column = 0;

    bool operator==(const GhosttyConfigDiagnostic &) const = default;
};

// Every snapshot is a complete, usable configuration generation. Absence is
// represented by GhosttyConfigService::hasSnapshot(), while loader failures
// retain the service's previous snapshot. This avoids a separate availability
// flag that could contradict the contained values. Its only constructor
// consumes a complete parser-produced export.
struct GhosttyConfigSnapshot {
    explicit GhosttyConfigSnapshot(GhosttyConfigExport exported)
        : values(std::move(exported.values))
        , keybindings(std::move(exported.keybindings))
    {
    }

    GhosttyConfigValues values;
    GhosttyKeybindConfig keybindings;
    QVector<GhosttyConfigDiagnostic> diagnostics;
    QStringList sourcePaths;

    bool operator==(const GhosttyConfigSnapshot &) const = default;
};

Q_DECLARE_METATYPE(GhosttyConfigDiagnosticSeverity)
Q_DECLARE_METATYPE(GhosttyConfigDiagnostic)
Q_DECLARE_METATYPE(GhosttyConfigSnapshot)
