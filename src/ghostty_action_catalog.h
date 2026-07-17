#pragma once

#include "terminal_actions.h"
#include "workspace_action.h"

#include <QMetaType>
#include <QString>
#include <QStringView>

#include <optional>

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

// Pane-local actions have state-dependent execution that cannot be represented
// by WorkspaceActionRequest. Keep their parsed values typed so TerminalPane
// never has to reinterpret Ghostty's serialized numeric or enum parameters.
enum class GhosttyPaneActionKind {
    ScrollViewport,
    ScrollPageUp,
    ScrollPageDown,
    ScrollPageFractional,
    SelectAll,
    AdjustSelection,
};

struct GhosttyPaneAction {
    GhosttyPaneActionKind kind = GhosttyPaneActionKind::ScrollViewport;
    TerminalViewportRequest viewport;
    float pageFraction = 0.0F;
    TerminalSelectionAdjustment selectionAdjustment =
        TerminalSelectionAdjustment::Left;
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

    [[nodiscard]] bool accepted() const noexcept
    {
        return request.has_value();
    }
};

class GhosttyActionCatalog final {
public:
    // Translate the Ghostty Action.parse wire format into the workspace's
    // typed action vocabulary. Parsing intentionally follows the pinned
    // ghostty/src/input/Binding.zig rules: the first colon separates the
    // parameter, names and enum values are case-sensitive, and void actions
    // reject even an empty parameter.
    [[nodiscard]] static GhosttyActionTranslation translate(
        QStringView serializedAction,
        WorkspaceActionContext context = {});

    // True when the current Qt frontend implements this exact serialized
    // action, including parameter validation. This includes pane-local
    // clipboard/zoom/scroll/reload actions as well as typed workspace actions.
    [[nodiscard]] static bool isImplemented(QStringView serializedAction);

    // Parse the implemented pane-local viewport and selection actions. Integer
    // syntax and f32 rounding follow Binding.Action.parse at the pinned
    // revision. Unlike upstream, non-finite or intrinsically out-of-range
    // fractional values are rejected so execution cannot reach Zig's
    // safety-checked illegal float-to-isize conversion.
    [[nodiscard]] static std::optional<GhosttyPaneAction> parsePaneAction(
        QStringView serializedAction);

    // Mirrors Binding.Action.scope() in the pinned Ghostty revision. This is
    // intentionally independent of WorkspaceAction: actions such as new_tab
    // are surface-scoped upstream even though Qt ultimately routes them
    // through the workspace model.
    [[nodiscard]] static GhosttyActionScope scope(
        QStringView serializedAction);
};

Q_DECLARE_METATYPE(GhosttyActionTranslationError)
Q_DECLARE_METATYPE(GhosttyActionScope)
Q_DECLARE_METATYPE(GhosttyPaneActionKind)
