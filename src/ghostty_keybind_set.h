#pragma once

#include "ghostty_action_catalog.h"
#include "ghostty_keybind_config.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QVector>
#include <QtCore/qnamespace.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
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

class GhosttyKeybindState;
struct GhosttyKeybindCompilation;
struct GhosttyKeybindEvent;

// One immutable, cheaply copied keybinding generation. All application and
// pane matchers may share the same storage while keeping traversal and named
// table state local to their owning surface.
class GhosttyKeybindProgram final {
public:
    GhosttyKeybindProgram();

    [[nodiscard]] static GhosttyKeybindCompilation
    compile(const QStringList &values);
    [[nodiscard]] static GhosttyKeybindCompilation
    compile(const GhosttyKeybindConfig &config);
    [[nodiscard]] static GhosttyKeybindCompilation
    compile(const GhosttyKeybindSource &source);

    [[nodiscard]] qsizetype size() const noexcept;
    [[nodiscard]] bool isEmpty() const noexcept { return size() == 0; }
    [[nodiscard]] bool isAvailable() const noexcept;
    [[nodiscard]] QStringList serializedActions() const;
    // Every compile operation creates a distinct generation, even when its
    // input is structurally equal to an earlier one.
    [[nodiscard]] bool
    isSameGeneration(const GhosttyKeybindProgram &other) const noexcept;

private:
    friend class GhosttyKeybindState;

    struct NodeId {
        quint32 value = 0;

        bool operator==(const NodeId &) const = default;
    };

    enum class KeyKind {
        Physical,
        Unicode,
        CatchAll,
    };

    struct Binding {
        KeyKind keyKind = KeyKind::Unicode;
        int qtKey = Qt::Key_unknown;
        quint32 nativeScanCode = 0;
        // Numeric GhosttyKey identity. This is distinct from nativeScanCode:
        // NumLock changes KP1 into the semantic KP_End key without changing
        // its hardware/XKB keycode.
        int physicalIdentity = 0;
        bool keypad = false;
        QString foldedUnicode;
        // Human-readable text comes from the configured trigger that
        // installed this trie edge. It must not be reconstructed from the
        // QKeyEvent that happened to match it: physical and case-folded
        // bindings can intentionally differ from the event's logical label.
        QString label;
        Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    };

    enum class EntryKind {
        Leader,
        Leaf,
    };

    struct Entry {
        Binding trigger;
        EntryKind kind = EntryKind::Leaf;
        NodeId child;
        GhosttyCompiledActionChain actionChain;
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

    struct PreparedEvent {
        const GhosttyKeybindEvent &source;
        Qt::KeyboardModifiers modifiers = Qt::NoModifier;
        std::array<QString, 2> foldedUnicodeCandidates;
        std::size_t candidateCount = 0;
        bool unicodeCandidatesReady = false;
    };

    struct Data {
        QVector<Node> nodes{Node{}};
        QHash<QString, NodeId> tableRoots;
        qsizetype bindingCount = 0;
        bool available = false;
    };

    template <typename Visitor>
    static void forEachReachableLeaf(const Data &data, Visitor &&visitor);
    explicit GhosttyKeybindProgram(std::shared_ptr<const Data> data);
    [[nodiscard]] static std::shared_ptr<const Data> emptyData();
    [[nodiscard]] static PreparedEvent
    prepareEvent(const GhosttyKeybindEvent &event);
    static void prepareUnicodeCandidates(PreparedEvent &event);
    [[nodiscard]] Lookup lookup(NodeId node, PreparedEvent &event) const;
    [[nodiscard]] const Entry *bareCatchAll(NodeId root) const;

    std::shared_ptr<const Data> data_;
};

struct GhosttyKeybindCompilation {
    GhosttyKeybindProgram program;
    GhosttyKeybindLoadReport report;
};

struct GhosttyKeybindMatch {
    // The owning snapshot remains valid if an action synchronously reloads
    // configuration and replaces the matcher's program.
    GhosttyCompiledActionChain actionChain;
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
    quint32 resolvedKeysym = 0;
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
    // True only when this step popped the matched top one-shot table.
    bool activeTablesChanged = false;

    bool operator==(const GhosttyKeybindStep &) const = default;
};

// Mutable matching state for one application root or terminal surface.
// Frontend-wide delivery of `all` and `global` matches is deliberately left
// to the caller. Copying is disabled so a surface cannot accidentally inherit
// another surface's active sequence or table stack. Moving is also disabled:
// an active traversal is correlated with a worker staging token owned by the
// surrounding pane or application controller.
class GhosttyKeybindState final {
public:
    static constexpr qsizetype MaximumActiveTables = 8;

    GhosttyKeybindState();
    explicit GhosttyKeybindState(GhosttyKeybindProgram program);
    GhosttyKeybindState(const GhosttyKeybindState &) = delete;
    GhosttyKeybindState &operator=(const GhosttyKeybindState &) = delete;
    GhosttyKeybindState(GhosttyKeybindState &&) = delete;
    GhosttyKeybindState &operator=(GhosttyKeybindState &&) = delete;

    // Stateless matching always checks the root set; use advance() for a
    // pane's active table stack and sequences. qtKey is QKeyEvent::key(),
    // modifiers is QKeyEvent::modifiers(), and text is QKeyEvent::text().
    // Physical named triggers are checked before Unicode triggers regardless
    // of config order, as they are in Ghostty.
    [[nodiscard]] std::optional<GhosttyKeybindMatch>
    match(int qtKey, Qt::KeyboardModifiers modifiers,
          QStringView text = {}) const;

    // Prefer this overload for real QKeyEvents. Supplying nativeScanCode makes
    // physical Ghostty triggers layout independent and distinguishes physical
    // locations that share a Qt key (for example digit_1 and numpad_1).
    [[nodiscard]] std::optional<GhosttyKeybindMatch>
    match(const GhosttyKeybindEvent &event) const;

    // Advances the active sequence or performs a newest-table-to-root lookup.
    // Leader sequences have no timeout, matching Ghostty. Invalid
    // non-modifier continuations either request replay or are dropped by the
    // first bare catch_all=ignore entry in the active lookup order.
    [[nodiscard]] GhosttyKeybindStep advance(const GhosttyKeybindEvent &event);
    void resetSequence() noexcept;
    [[nodiscard]] bool sequenceActive() const noexcept
    {
        return activeNode_.has_value();
    }
    [[nodiscard]] const QStringList &activeSequenceLabels() const noexcept
    {
        return activeSequenceLabels_;
    }

    // Named key tables are surface-local stack state. Activation fails for an
    // unknown table, a duplicate top entry, or Ghostty's maximum depth of 8.
    // The same table may otherwise occur repeatedly in the stack.
    [[nodiscard]] bool hasTable(QStringView name) const;
    [[nodiscard]] bool canActivateTable(QStringView name) const;
    [[nodiscard]] bool activateTable(QStringView name, bool oneShot = false);
    [[nodiscard]] bool deactivateTable() noexcept;
    [[nodiscard]] bool deactivateAllTables() noexcept;
    [[nodiscard]] bool hasActiveTables() const noexcept
    {
        return !activeTables_.isEmpty();
    }
    [[nodiscard]] QStringList activeTableNames() const;

    [[nodiscard]] qsizetype size() const noexcept { return program_.size(); }
    [[nodiscard]] bool isEmpty() const noexcept { return program_.isEmpty(); }
    [[nodiscard]] QStringList serializedActions() const
    {
        return program_.serializedActions();
    }
    [[nodiscard]] const GhosttyKeybindProgram &program() const noexcept
    {
        return program_;
    }
    [[nodiscard]] bool replaceProgram(GhosttyKeybindProgram program) noexcept;

private:
    using NodeId = GhosttyKeybindProgram::NodeId;
    using Entry = GhosttyKeybindProgram::Entry;
    using EntryKind = GhosttyKeybindProgram::EntryKind;
    using Lookup = GhosttyKeybindProgram::Lookup;
    using PreparedEvent = GhosttyKeybindProgram::PreparedEvent;

    struct ActiveTable {
        QString name;
        NodeId root;
        bool oneShot = false;
    };

    [[nodiscard]] bool activeCatchAllIgnores() const;

    GhosttyKeybindProgram program_;
    QVector<ActiveTable> activeTables_;
    std::optional<NodeId> activeNode_;
    QVector<GhosttyKeybindEvent> queuedEvents_;
    QStringList activeSequenceLabels_;
};
