#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

enum class GhosttyKeybindKeyKind {
    Physical,
    Unicode,
    CatchAll,
};

enum GhosttyKeybindModifier : quint8 {
    GhosttyKeybindShift = 1U << 0U,
    GhosttyKeybindCtrl = 1U << 1U,
    GhosttyKeybindAlt = 1U << 2U,
    GhosttyKeybindSuper = 1U << 3U,
};

struct GhosttyKeybindTrigger {
    GhosttyKeybindKeyKind kind = GhosttyKeybindKeyKind::Unicode;
    QString physicalName;
    quint32 unicodeCodepoint = 0;
    quint8 modifiers = 0;

    bool operator==(const GhosttyKeybindTrigger &) const = default;
};

struct GhosttyKeybindFlags {
    bool consumed = true;
    bool all = false;
    bool global = false;
    bool performable = false;

    bool operator==(const GhosttyKeybindFlags &) const = default;
};

// One exported binding leaf. A sequence contains every trigger from the
// table root to that leaf; actions preserve Ghostty's canonical chain order.
struct GhosttyKeybindDefinition {
    QVector<GhosttyKeybindTrigger> sequence;
    QStringList actions;
    GhosttyKeybindFlags flags;

    bool operator==(const GhosttyKeybindDefinition &) const = default;
};

struct GhosttyKeybindTable {
    QString name;
    QVector<GhosttyKeybindDefinition> bindings;

    bool operator==(const GhosttyKeybindTable &) const = default;
};

struct GhosttyKeybindConfig {
    static constexpr int CurrentSchemaVersion = 1;

    int schemaVersion = CurrentSchemaVersion;
    QVector<GhosttyKeybindDefinition> root;
    QVector<GhosttyKeybindTable> tables;

    bool operator==(const GhosttyKeybindConfig &) const = default;
};
