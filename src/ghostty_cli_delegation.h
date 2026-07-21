#pragma once

#include <array>
#include <filesystem>
#include <span>
#include <string_view>
#include <system_error>

inline constexpr std::array<std::string_view, 7>
    GhosttyQtDelegatedCliActions{
        "+explain-config",
        "+help",
        "+list-actions",
        "+list-colors",
        "+list-keybinds",
        "+show-config",
        "+validate-config",
    };

enum class GhosttyCliActionDisposition {
    None,
    Delegate,
    Unsupported,
    Multiple,
};

struct GhosttyCliActionSelection final {
    GhosttyCliActionDisposition disposition =
        GhosttyCliActionDisposition::None;
    std::string_view argument;

    bool operator==(const GhosttyCliActionSelection &) const = default;
};

// Classify raw process arguments before Qt or frontend option parsing. The
// returned string_view aliases argv and remains valid for the process lifetime.
[[nodiscard]] GhosttyCliActionSelection selectGhosttyCliAction(
    std::span<char *const> arguments) noexcept;

struct GhosttyCliExecError final {
    std::filesystem::path target;
    std::error_code cause;

    [[nodiscard]] int exitCode() const noexcept;
};

// Replace this Linux process with a sibling helper while preserving its raw
// argv bytes, environment, non-close-on-exec descriptors, PID, and process
// relationship. A successful exec never returns; the value describes the
// failure otherwise.
[[nodiscard]] GhosttyCliExecError execGhosttyCliHelper(
    std::span<char *const> arguments,
    std::string_view helperName);
