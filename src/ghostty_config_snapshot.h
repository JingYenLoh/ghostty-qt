#pragma once

#include "ghostty_keybind_config.h"

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

#include <optional>

// Whether the Ghostty configuration backend produced a usable snapshot. An
// available snapshot may contain defaults only; sourcePaths identifies the
// files that contributed values when configuration files were present.
enum class GhosttyConfigAvailability {
    Unavailable,
    Available,
};

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

// The bridge is responsible for preserving the type of each Ghostty value in
// QVariant (for example bool, quint32, qint64, double, QString, or QStringList). Keeping
// this value-only lets snapshots cross Qt threads without retaining any
// Ghostty-owned handles.
struct GhosttyConfigSnapshot {
    GhosttyConfigAvailability availability = GhosttyConfigAvailability::Unavailable;
    QVariantMap values;
    // The structured helper export is authoritative for keybindings. Keep it
    // outside QVariant so schema fidelity is checked at the process boundary.
    std::optional<GhosttyKeybindConfig> keybindConfig;
    QVector<GhosttyConfigDiagnostic> diagnostics;
    QStringList sourcePaths;

    template<typename T>
    std::optional<T> value(const QString &key) const
    {
        const auto it = values.constFind(key);
        if (it == values.cend() || !it->isValid()
            || it->metaType() != QMetaType::fromType<T>()) {
            return std::nullopt;
        }
        return it->template value<T>();
    }

    bool operator==(const GhosttyConfigSnapshot &) const = default;
};

Q_DECLARE_METATYPE(GhosttyConfigAvailability)
Q_DECLARE_METATYPE(GhosttyConfigDiagnosticSeverity)
Q_DECLARE_METATYPE(GhosttyConfigDiagnostic)
Q_DECLARE_METATYPE(GhosttyConfigSnapshot)
