#pragma once

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
    Sequence,
    KeyTable,
    CatchAll,
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
};

// The compatibility set intentionally implements one local trigger at a time.
// Unsupported Ghostty features are recorded and skipped instead of being
// approximated with subtly different Qt behavior.
class GhosttyKeybindSet final {
public:
    // Replaces the current set with the flattened values emitted by the pinned
    // `ghostty +show-config`. Later duplicate triggers replace earlier ones,
    // matching Ghostty's config semantics.
    [[nodiscard]] GhosttyKeybindLoadReport load(const QStringList &values);

    // qtKey is QKeyEvent::key(), modifiers is QKeyEvent::modifiers(), and text
    // is QKeyEvent::text(). Physical named triggers are checked before Unicode
    // triggers regardless of config order, as they are in Ghostty.
    [[nodiscard]] std::optional<GhosttyKeybindMatch> match(
        int qtKey,
        Qt::KeyboardModifiers modifiers,
        QStringView text = {}) const;

    // Prefer this overload for real QKeyEvents. Supplying nativeScanCode makes
    // physical Ghostty triggers layout independent and distinguishes physical
    // locations that share a Qt key (for example digit_1 and numpad_1).
    [[nodiscard]] std::optional<GhosttyKeybindMatch> match(
        const GhosttyKeybindEvent &event) const;

    [[nodiscard]] qsizetype size() const noexcept { return bindings_.size(); }
    [[nodiscard]] bool isEmpty() const noexcept { return bindings_.isEmpty(); }
    [[nodiscard]] QStringList serializedActions() const;
    void clear() noexcept { bindings_.clear(); }

private:
    enum class KeyKind {
        Physical,
        Unicode,
    };

    struct Binding {
        KeyKind keyKind = KeyKind::Unicode;
        int qtKey = Qt::Key_unknown;
        quint32 nativeScanCode = 0;
        bool keypad = false;
        QString unicode;
        Qt::KeyboardModifiers modifiers = Qt::NoModifier;
        QStringList actions;
        bool consumed = true;
        bool performable = false;
    };

    QVector<Binding> bindings_;
};
