#pragma once

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
};

Q_DECLARE_METATYPE(GhosttyActionTranslationError)
