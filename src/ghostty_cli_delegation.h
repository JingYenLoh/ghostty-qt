#pragma once

#include <array>
#include <filesystem>
#include <span>
#include <string_view>
#include <system_error>

enum class GhosttyCliFrontendSupport {
    Delegated,
    ApplicationIpc,
    Unsupported,
};

struct GhosttyCliActionCatalogEntry final {
    std::string_view argument;
    GhosttyCliFrontendSupport frontendSupport;

    [[nodiscard]] constexpr bool isDelegated() const noexcept
    {
        return frontendSupport == GhosttyCliFrontendSupport::Delegated;
    }

    [[nodiscard]] constexpr bool isApplicationIpc() const noexcept
    {
        return frontendSupport == GhosttyCliFrontendSupport::ApplicationIpc;
    }

    bool operator==(const GhosttyCliActionCatalogEntry &) const = default;
};

// Keep this catalog in the same order as the Action enum in the pinned
// Ghostty source. Recognition and frontend support are intentionally separate:
// an action can be valid Ghostty syntax without being handled by ghostty-qt.
inline constexpr auto GhosttyPinnedCliActions =
    std::to_array<GhosttyCliActionCatalogEntry>({
        {"+version", GhosttyCliFrontendSupport::Delegated},
        {"+help", GhosttyCliFrontendSupport::Delegated},
        {"+list-fonts", GhosttyCliFrontendSupport::Delegated},
        {"+list-keybinds", GhosttyCliFrontendSupport::Delegated},
        {"+list-themes", GhosttyCliFrontendSupport::Unsupported},
        {"+list-colors", GhosttyCliFrontendSupport::Delegated},
        {"+list-actions", GhosttyCliFrontendSupport::Delegated},
        {"+ssh", GhosttyCliFrontendSupport::Delegated},
        {"+ssh-cache", GhosttyCliFrontendSupport::Delegated},
        {"+edit-config", GhosttyCliFrontendSupport::Delegated},
        {"+show-config", GhosttyCliFrontendSupport::Delegated},
        {"+explain-config", GhosttyCliFrontendSupport::Delegated},
        {"+validate-config", GhosttyCliFrontendSupport::Delegated},
        {"+show-face", GhosttyCliFrontendSupport::Delegated},
        {"+crash-report", GhosttyCliFrontendSupport::Unsupported},
        {"+boo", GhosttyCliFrontendSupport::Unsupported},
        {"+new-window", GhosttyCliFrontendSupport::ApplicationIpc},
        {"+toggle-quick-terminal", GhosttyCliFrontendSupport::ApplicationIpc},
    });

enum class GhosttyCliActionDisposition {
    None,
    Delegate,
    ApplicationIpc,
    Unsupported,
    Multiple,
};

struct GhosttyCliActionSelection final {
    GhosttyCliActionDisposition disposition = GhosttyCliActionDisposition::None;
    std::string_view argument;

    bool operator==(const GhosttyCliActionSelection &) const = default;
};

// Classify raw process arguments before Qt or frontend option parsing. The
// returned string_view aliases argv and remains valid for the process lifetime.
[[nodiscard]] GhosttyCliActionSelection
selectGhosttyCliAction(std::span<char *const> arguments) noexcept;

struct GhosttyCliExecError final {
    std::filesystem::path target;
    std::error_code cause;

    [[nodiscard]] int exitCode() const noexcept;
};

// Replace this Linux process with a sibling helper while preserving its raw
// argv bytes, environment, non-close-on-exec descriptors, PID, and process
// relationship. A successful exec never returns; the value describes the
// failure otherwise.
[[nodiscard]] GhosttyCliExecError
execGhosttyCliHelper(std::span<char *const> arguments,
                     std::string_view helperName);
