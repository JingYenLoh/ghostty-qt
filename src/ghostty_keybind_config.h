#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <utility>
#include <variant>

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
    QVector<GhosttyKeybindDefinition> root;
    QVector<GhosttyKeybindTable> tables;

    bool operator==(const GhosttyKeybindConfig &) const = default;
};

// The frontend can receive keybindings from the structured helper export or
// from direct flattened-text injection in focused tests. Keeping the source
// tagged distinguishes both from an unavailable backend and prevents callers
// from combining unrelated representations into contradictory states.
class GhosttyKeybindSource final {
public:
    GhosttyKeybindSource() = default;

    [[nodiscard]] static GhosttyKeybindSource structured(
        GhosttyKeybindConfig config)
    {
        return GhosttyKeybindSource(std::move(config));
    }

    [[nodiscard]] static GhosttyKeybindSource text(QStringList values)
    {
        return GhosttyKeybindSource(std::move(values));
    }

    [[nodiscard]] bool isAvailable() const noexcept
    {
        return !std::holds_alternative<std::monostate>(value_);
    }

    [[nodiscard]] const GhosttyKeybindConfig *structured() const noexcept
    {
        return std::get_if<GhosttyKeybindConfig>(&value_);
    }

    [[nodiscard]] const QStringList *text() const noexcept
    {
        return std::get_if<QStringList>(&value_);
    }

    template<typename Visitor>
    decltype(auto) visit(Visitor &&visitor) const
    {
        return std::visit(std::forward<Visitor>(visitor), value_);
    }

    bool operator==(const GhosttyKeybindSource &) const = default;

private:
    explicit GhosttyKeybindSource(GhosttyKeybindConfig config)
        : value_(std::move(config))
    {
    }

    explicit GhosttyKeybindSource(QStringList values)
        : value_(std::move(values))
    {
    }

    std::variant<std::monostate, GhosttyKeybindConfig, QStringList> value_;
};
