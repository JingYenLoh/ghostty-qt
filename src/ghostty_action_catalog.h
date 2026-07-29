#pragma once

#include "application_action.h"
#include "terminal_types.h"
#include "terminal_write_file_action.h"
#include "window_navigation_action.h"
#include "workspace_action.h"

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QVector>

#include <cmath>
#include <expected>
#include <optional>
#include <variant>

// The rejection categories are deliberately stable so configuration and
// command-palette callers can report the same result for the same input.
enum class GhosttyActionTranslationError {
    None,
    InvalidFormat,
    UnsupportedAction,
    UnsupportedParameter,
};

enum class GhosttyActionScope {
    Application,
    Surface,
};

enum class GhosttyActionInputEffect : quint8 {
    None,
    Ignore,
    ClosingAction,
};

// Non-owning tokenization of Ghostty's serialized Action.parse format. The
// views remain valid only while the source QStringView remains valid. Keeping
// the optional parameter preserves the semantic difference between `action`
// and `action:` for every validation and execution path.
struct GhosttySerializedActionView {
    QStringView name;
    std::optional<QStringView> parameter;
};

// Pane-local actions have state-dependent execution that cannot be represented
// by WorkspaceActionRequest. Each alternative owns exactly its valid payload;
// adding an action therefore requires the parser and executor to handle a new
// type instead of synchronizing a tag with unrelated optional fields.
namespace GhosttyPaneActions {

[[nodiscard]] inline bool equivalentFloat(float lhs, float rhs) noexcept
{
    return lhs == rhs || (std::isnan(lhs) && std::isnan(rhs));
}

struct ScrollToTop {
    bool operator==(const ScrollToTop &) const = default;
};
struct ScrollToBottom {
    bool operator==(const ScrollToBottom &) const = default;
};
struct ScrollToSelection {
    bool operator==(const ScrollToSelection &) const = default;
};
struct ScrollToRow {
    quint64 row = 0;
    bool operator==(const ScrollToRow &) const = default;
};
struct ScrollPageUp {
    bool operator==(const ScrollPageUp &) const = default;
};
struct ScrollPageDown {
    bool operator==(const ScrollPageDown &) const = default;
};
struct ScrollPageFractional {
    float fraction = 0.0F;
    bool operator==(const ScrollPageFractional &) const = default;
};
struct ScrollPageLines {
    qint16 lines = 0;
    bool operator==(const ScrollPageLines &) const = default;
};

struct IncreaseFontSize {
    float points = 0.0F;
    bool operator==(const IncreaseFontSize &other) const noexcept
    {
        return equivalentFloat(points, other.points);
    }
};
struct DecreaseFontSize {
    float points = 0.0F;
    bool operator==(const DecreaseFontSize &other) const noexcept
    {
        return equivalentFloat(points, other.points);
    }
};
struct SetFontSize {
    float points = 0.0F;
    bool operator==(const SetFontSize &other) const noexcept
    {
        return equivalentFloat(points, other.points);
    }
};
struct ResetFontSize {
    bool operator==(const ResetFontSize &) const = default;
};

struct ActivateKeyTable {
    QString name;
    bool operator==(const ActivateKeyTable &) const = default;
};
struct ActivateKeyTableOnce {
    QString name;
    bool operator==(const ActivateKeyTableOnce &) const = default;
};
struct DeactivateKeyTable {
    bool operator==(const DeactivateKeyTable &) const = default;
};
struct DeactivateAllKeyTables {
    bool operator==(const DeactivateAllKeyTables &) const = default;
};

struct SelectAll {
    bool operator==(const SelectAll &) const = default;
};
struct AdjustSelection {
    TerminalSelectionAdjustment adjustment = TerminalSelectionAdjustment::Left;
    bool operator==(const AdjustSelection &) const = default;
};

struct StartSearch {
    bool operator==(const StartSearch &) const = default;
};
struct EndSearch {
    bool operator==(const EndSearch &) const = default;
};
struct SearchSelection {
    bool operator==(const SearchSelection &) const = default;
};
struct Search {
    QByteArray serializedNeedle;
    bool operator==(const Search &) const = default;
};
struct NavigateSearch {
    TerminalSearchDirection direction = TerminalSearchDirection::Next;
    bool operator==(const NavigateSearch &) const = default;
};

// Binding.Action.parse preserves these byte-string parameters until
// execution. Text deliberately retains invalid Zig string escapes because
// Ghostty reports them only while performing the action. CSI and ESC likewise
// preserve every colon after the first one.
struct SendCsi {
    QByteArray serializedBytes;
    bool operator==(const SendCsi &) const = default;
};
struct SendEscape {
    QByteArray serializedBytes;
    bool operator==(const SendEscape &) const = default;
};
struct SendText {
    QByteArray serializedBytes;
    bool operator==(const SendText &) const = default;
};

struct ResetTerminal {
    bool operator==(const ResetTerminal &) const = default;
};
struct ToggleReadOnly {
    bool operator==(const ToggleReadOnly &) const = default;
};
struct ToggleMouseReporting {
    bool operator==(const ToggleMouseReporting &) const = default;
};

enum class CopyFormat : quint8 {
    Mixed,
    Plain,
};
struct CopyToClipboard {
    CopyFormat format = CopyFormat::Mixed;
    bool operator==(const CopyToClipboard &) const = default;
};

enum class PasteSource : quint8 {
    Clipboard,
    Selection,
};
struct Paste {
    PasteSource source = PasteSource::Clipboard;
    bool operator==(const Paste &) const = default;
};

struct CopyUrlToClipboard {
    bool operator==(const CopyUrlToClipboard &) const = default;
};
struct CopyTitleToClipboard {
    bool operator==(const CopyTitleToClipboard &) const = default;
};
struct EndKeySequence {
    bool operator==(const EndKeySequence &) const = default;
};
struct CloseWindow {
    bool operator==(const CloseWindow &) const = default;
};

} // namespace GhosttyPaneActions

using GhosttyPaneAction = std::variant<
    GhosttyPaneActions::ScrollToTop, GhosttyPaneActions::ScrollToBottom,
    GhosttyPaneActions::ScrollToSelection, GhosttyPaneActions::ScrollToRow,
    GhosttyPaneActions::ScrollPageUp, GhosttyPaneActions::ScrollPageDown,
    GhosttyPaneActions::ScrollPageFractional,
    GhosttyPaneActions::ScrollPageLines, GhosttyPaneActions::IncreaseFontSize,
    GhosttyPaneActions::DecreaseFontSize, GhosttyPaneActions::SetFontSize,
    GhosttyPaneActions::ResetFontSize, GhosttyPaneActions::ActivateKeyTable,
    GhosttyPaneActions::ActivateKeyTableOnce,
    GhosttyPaneActions::DeactivateKeyTable,
    GhosttyPaneActions::DeactivateAllKeyTables, GhosttyPaneActions::SelectAll,
    GhosttyPaneActions::AdjustSelection, GhosttyPaneActions::StartSearch,
    GhosttyPaneActions::EndSearch, GhosttyPaneActions::SearchSelection,
    GhosttyPaneActions::Search, GhosttyPaneActions::NavigateSearch,
    GhosttyPaneActions::SendCsi, GhosttyPaneActions::SendEscape,
    GhosttyPaneActions::SendText, GhosttyPaneActions::ResetTerminal,
    GhosttyPaneActions::ToggleReadOnly,
    GhosttyPaneActions::ToggleMouseReporting,
    GhosttyPaneActions::CopyToClipboard, GhosttyPaneActions::Paste,
    GhosttyPaneActions::CopyUrlToClipboard,
    GhosttyPaneActions::CopyTitleToClipboard, TerminalWriteFileAction,
    GhosttyPaneActions::EndKeySequence, GhosttyPaneActions::CloseWindow>;

using GhosttyDirectSurfaceActionParseResult =
    std::expected<GhosttyPaneAction, GhosttyActionTranslationError>;

using GhosttyConfiguredAction =
    std::variant<ApplicationAction, WindowNavigationAction, GhosttyPaneAction,
                 WorkspaceActionRequest, WorkspaceFrontendActionRequest>;

// One positional entry is retained for every serialized chain member,
// including unsupported and malformed actions. Scope is an upstream property
// independent of frontend support; action is present only when this frontend
// can execute the exact spelling.
struct GhosttyCompiledAction {
    QString serialized;
    GhosttyActionScope scope = GhosttyActionScope::Surface;
    std::optional<GhosttyConfiguredAction> action;

    bool operator==(const GhosttyCompiledAction &) const = default;

    template <typename Action>
    [[nodiscard]] const Action *getIf() const noexcept
    {
        return action.has_value() ? std::get_if<Action>(&*action) : nullptr;
    }
};

struct GhosttyCompiledActionChain {
    QVector<GhosttyCompiledAction> entries;
    GhosttyActionInputEffect inputEffect = GhosttyActionInputEffect::None;
    bool applicationOnly = true;

    bool operator==(const GhosttyCompiledActionChain &) const = default;

    [[nodiscard]] QStringList serializedActions() const;
};

struct GhosttyActionTranslation {
    std::optional<WorkspaceActionRequest> request;
    GhosttyActionTranslationError error =
        GhosttyActionTranslationError::InvalidFormat;

    // Preserve the parsed spelling so callers can associate a result with the
    // exact Ghostty action and parameter that produced it. A missing parameter
    // remains distinct from an explicitly empty parameter.
    QString actionName;
    std::optional<QString> parameter;

    [[nodiscard]] bool accepted() const noexcept { return request.has_value(); }
};

class GhosttyActionCatalog final {
public:
    // Split only at the first colon. Action-specific grammar is validated by
    // translate, parsePaneAction, and isImplemented.
    [[nodiscard]] static GhosttySerializedActionView
    parseSerializedAction(QStringView serializedAction);

    // Translate the Ghostty Action.parse wire format into the workspace's
    // typed action vocabulary. Parsing intentionally follows the pinned
    // ghostty/src/input/Binding.zig rules: the first colon separates the
    // parameter, names and enum values are case-sensitive, and void actions
    // reject even an empty parameter.
    [[nodiscard]] static GhosttyActionTranslation
    translate(QStringView serializedAction,
              WorkspaceActionContext context = {});

    // Parse one complete executable action into an owning value. This is the
    // preferred execution boundary: callers do not need separate validation,
    // performability, and dispatch parses, and payloads remain valid across
    // synchronous configuration reloads.
    [[nodiscard]] static std::optional<GhosttyConfiguredAction>
    parseConfiguredAction(QStringView serializedAction,
                          WorkspaceActionContext context = {});

    // True when the current Qt frontend implements this exact serialized
    // action, including parameter validation. This includes pane-local
    // clipboard/zoom/scroll/reload actions as well as typed workspace actions.
    [[nodiscard]] static bool isImplemented(QStringView serializedAction);

    // Parse the implemented pane-local viewport, font, key-table, selection,
    // search, and terminal control actions. Integer syntax and f32 rounding
    // follow Binding.Action.parse at the pinned revision. Unlike upstream,
    // non-finite or intrinsically out-of-range fractional values are rejected
    // so execution cannot reach Zig's safety-checked illegal float-to-isize
    // conversion.
    [[nodiscard]] static std::optional<GhosttyPaneAction>
    parsePaneAction(QStringView serializedAction);

    // Preserve precise diagnostics for the frontend-owned clipboard,
    // sequence, and window actions while the older pane parser retains its
    // optional compatibility API.
    [[nodiscard]] static GhosttyDirectSurfaceActionParseResult
    parseDirectSurfaceAction(QStringView serializedAction);

    // Parse the exact parameterless application action vocabulary. This is a
    // grammar boundary independent of runtime support: for example, the quick
    // terminal remains typed even when the platform backend cannot execute it.
    [[nodiscard]] static std::optional<ApplicationAction>
    parseApplicationAction(QStringView serializedAction);

    // Parse frontend-owned surface actions into payload-safe request types.
    // This grammar boundary deliberately includes Inspector and Crash even
    // while they remain blocked from executable configured-action chains.
    [[nodiscard]] static std::optional<WorkspaceFrontendActionRequest>
    parseFrontendAction(QStringView serializedAction,
                        WorkspaceActionContext context = {});

    // Parse the exact previous/next goto_window payload. The action remains
    // surface-scoped even though execution is relayed to the application
    // controller that owns the top-level window registry.
    [[nodiscard]] static std::optional<WindowNavigationAction>
    parseWindowNavigationAction(QStringView serializedAction);

    // Mirrors Binding.Action.scope() in the pinned Ghostty revision. This is
    // intentionally independent of WorkspaceAction: actions such as new_tab
    // are surface-scoped upstream even though Qt ultimately routes them
    // through the workspace model.
    [[nodiscard]] static GhosttyActionScope scope(QStringView serializedAction);

    // Input lifecycle policy is derived from typed action identity rather
    // than serialized spellings. ClosingAction includes every close-tab
    // mode, even when the originating pane itself survives.
    [[nodiscard]] static GhosttyActionInputEffect
    inputEffect(const GhosttyConfiguredAction &action) noexcept;

    // Compile a complete owning action chain once at keybinding-load time.
    // Positional entries preserve unsupported scopes and the aggregate stores
    // Ghostty's closing-before-ignore input precedence.
    [[nodiscard]] static GhosttyCompiledActionChain
    compileActionChain(const QStringList &actions);

    // Combine one serialized chain with Ghostty's closing-before-ignore
    // precedence. Unsupported entries have no input effect.
    [[nodiscard]] static GhosttyActionInputEffect
    combinedInputEffect(const QStringList &actions);

    // Broad close_surface, close_window, and close_tab:this operations
    // converge on one window close per workspace. Other/right tab modes keep
    // their ordinary per-surface fanout.
    [[nodiscard]] static bool
    shouldCoalesceBroadClose(const GhosttyConfiguredAction &action) noexcept;
};

Q_DECLARE_METATYPE(GhosttyActionTranslationError)
Q_DECLARE_METATYPE(GhosttyActionScope)
Q_DECLARE_METATYPE(GhosttyActionInputEffect)
Q_DECLARE_METATYPE(GhosttyConfiguredAction)
Q_DECLARE_METATYPE(GhosttyCompiledActionChain)
