#pragma once

#include "ghostty_keybind_config.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QVector>
#include <QtCore/qnamespace.h>

#include <cstdint>
#include <optional>

// A flattened `keybind = ...` entry can be syntactically valid Ghostty
// configuration while requiring application integration that ghostty-qt does
// not have yet. Keep those cases distinct from malformed input so config
// reloads can explain precisely what was ignored.
enum class GhosttyKeybindEntryDisposition {
    Installed,
    Chained,
    Unsupported,
    Invalid,
};

enum class GhosttyKeybindUnsupportedReason {
    None,
    KeyTable,
    NonLocal,
    ClearDirective,
    OrphanChain,
};

struct GhosttyKeybindParseRecord {
    QString input;
    GhosttyKeybindEntryDisposition disposition =
        GhosttyKeybindEntryDisposition::Invalid;
    GhosttyKeybindUnsupportedReason reason =
        GhosttyKeybindUnsupportedReason::None;
    QString detail;

    bool operator==(const GhosttyKeybindParseRecord &) const = default;
};

struct GhosttyKeybindLoadReport {
    QVector<GhosttyKeybindParseRecord> records;

    [[nodiscard]] int count(GhosttyKeybindEntryDisposition disposition) const;
};

struct GhosttyKeybindMatch {
    // Chained actions retain config order. The action strings use Ghostty's
    // canonical Action.parse representation and are intentionally not
    // translated into workspace actions at this layer.
    QStringList actions;
    bool consumed = true;
    bool all = false;
    bool global = false;
    bool performable = false;
    bool physical = false;

    bool operator==(const GhosttyKeybindMatch &) const = default;
};

// Native key information is kept together so call sites cannot accidentally
// swap the native scan code and the unshifted Unicode codepoint. On Linux,
// Qt's Wayland and X11 backends expose XKB keycodes through nativeScanCode.
// Synthetic events may leave it at zero; the matcher then falls back to the
// logical Qt key for compatibility.
struct GhosttyKeybindEvent {
    int qtKey = Qt::Key_unknown;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    QString text;
    quint32 nativeScanCode = 0;
    std::uint32_t unshiftedCodepoint = 0;

    bool operator==(const GhosttyKeybindEvent &) const = default;
};

enum class GhosttyKeybindStepKind {
    Unmatched,
    Leader,
    Binding,
    InvalidSequence,
    IgnoredSequence,
};

// Stateful lookup result used by a pane. queuedEvents contains the leader
// presses that preceded this step. The terminal bytes for those events are
// staged independently on the session thread; retaining the value events here
// keeps the state machine directly testable without exposing terminal handles.
struct GhosttyKeybindStep {
    GhosttyKeybindStepKind kind = GhosttyKeybindStepKind::Unmatched;
    GhosttyKeybindMatch match;
    QVector<GhosttyKeybindEvent> queuedEvents;

    bool operator==(const GhosttyKeybindStep &) const = default;
};

// The compatibility set implements Ghostty's finalized binding tries and the
// per-surface named-table stack. Frontend-wide delivery of `all` and `global`
// matches is deliberately left to the caller.
class GhosttyKeybindSet final {
public:
    static constexpr qsizetype MaximumActiveTables = 8;

    // Replaces the current set with the flattened values emitted by the pinned
    // `ghostty +show-config`. Later duplicate triggers replace earlier ones,
    // matching Ghostty's config semantics.
    [[nodiscard]] GhosttyKeybindLoadReport load(const QStringList &values);

    // Production configuration uses the versioned, value-only dump of
    // Ghostty's finalized binding tries. Named tables and non-local leaves are
    // installed; matches expose the non-local flags to the frontend.
    [[nodiscard]] GhosttyKeybindLoadReport load(
        const GhosttyKeybindConfig &config);

    // Stateless matching always checks the root set; use advance() for a
    // pane's active table stack and sequences. qtKey is QKeyEvent::key(),
    // modifiers is QKeyEvent::modifiers(), and text is QKeyEvent::text().
    // Physical named triggers are checked before Unicode triggers regardless
    // of config order, as they are in Ghostty.
    [[nodiscard]] std::optional<GhosttyKeybindMatch> match(
        int qtKey,
        Qt::KeyboardModifiers modifiers,
        QStringView text = {}) const;

    // Prefer this overload for real QKeyEvents. Supplying nativeScanCode makes
    // physical Ghostty triggers layout independent and distinguishes physical
    // locations that share a Qt key (for example digit_1 and numpad_1).
    [[nodiscard]] std::optional<GhosttyKeybindMatch> match(
        const GhosttyKeybindEvent &event) const;

    // Advances the active sequence or performs a newest-table-to-root lookup.
    // Leader sequences have no timeout, matching Ghostty. Invalid
    // non-modifier continuations either request replay or are dropped by the
    // first bare catch_all=ignore entry in the active lookup order.
    [[nodiscard]] GhosttyKeybindStep advance(
        const GhosttyKeybindEvent &event);
    void resetSequence() noexcept;
    [[nodiscard]] bool sequenceActive() const noexcept
    {
        return activeNode_.has_value();
    }

    // Named key tables are surface-local stack state. Activation fails for an
    // unknown table, a duplicate top entry, or Ghostty's maximum depth of 8.
    // The same table may otherwise occur repeatedly in the stack.
    [[nodiscard]] bool hasTable(QStringView name) const;
    [[nodiscard]] bool canActivateTable(QStringView name) const;
    [[nodiscard]] bool activateTable(QStringView name, bool oneShot = false);
    [[nodiscard]] bool deactivateTable() noexcept;
    [[nodiscard]] bool deactivateAllTables() noexcept;
    [[nodiscard]] QStringList activeTableNames() const;

    [[nodiscard]] qsizetype size() const noexcept { return bindingCount_; }
    [[nodiscard]] bool isEmpty() const noexcept { return bindingCount_ == 0; }
    [[nodiscard]] QStringList serializedActions() const;
    void clear() noexcept;

private:
    enum class KeyKind {
        Physical,
        Unicode,
        CatchAll,
    };

    struct Binding {
        KeyKind keyKind = KeyKind::Unicode;
        int qtKey = Qt::Key_unknown;
        quint32 nativeScanCode = 0;
        bool keypad = false;
        QString unicode;
        Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    };

    enum class EntryKind {
        Leader,
        Leaf,
    };

    struct Entry {
        Binding trigger;
        EntryKind kind = EntryKind::Leaf;
        quint32 child = 0;
        QStringList actions;
        bool consumed = true;
        bool all = false;
        bool global = false;
        bool performable = false;
    };

    struct Node {
        QVector<Entry> entries;
    };

    struct Lookup {
        const Entry *entry = nullptr;
        bool physical = false;
    };

    struct ActiveTable {
        QString name;
        quint32 root = 0;
        bool oneShot = false;
    };

    [[nodiscard]] Lookup lookup(quint32 node,
                                const GhosttyKeybindEvent &event) const;
    [[nodiscard]] const Entry *bareCatchAll(quint32 root) const;
    [[nodiscard]] bool activeCatchAllIgnores() const;

    QVector<Node> nodes_{Node{}};
    QHash<QString, quint32> tableRoots_;
    QVector<ActiveTable> activeTables_;
    qsizetype bindingCount_ = 0;
    std::optional<quint32> activeNode_;
    QVector<GhosttyKeybindEvent> queuedEvents_;
};
