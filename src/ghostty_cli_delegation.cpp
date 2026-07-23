#include "ghostty_cli_delegation.h"

#include <algorithm>
#include <cerrno>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

const GhosttyCliActionCatalogEntry *findPinnedAction(
    std::string_view argument) noexcept
{
    const auto action = std::ranges::find(
        GhosttyPinnedCliActions, argument,
        &GhosttyCliActionCatalogEntry::argument);
    return action == GhosttyPinnedCliActions.end()
        ? nullptr
        : &*action;
}

std::error_code invalidArgument() noexcept
{
    return std::make_error_code(std::errc::invalid_argument);
}

} // namespace

GhosttyCliActionSelection selectGhosttyCliAction(
    std::span<char *const> arguments) noexcept
{
    if (arguments.empty()) return {};
    const GhosttyCliActionCatalogEntry *selected = nullptr;
    std::string_view selectedArgument;
    for (char *const rawArgument : arguments.subspan(1)) {
        if (rawArgument == nullptr) {
            return {
                .disposition = GhosttyCliActionDisposition::Unsupported,
                .argument = {},
            };
        }
        const std::string_view argument(rawArgument);

        // Ghostty gives --version unconditional priority when reached. Keep
        // that invocation on ghostty-qt's deliberately separate version path.
        if (argument == "--version") return {};

        // Pinned Ghostty stops looking at -e before an action. This frontend
        // additionally documents -- as its command boundary.
        if (selected == nullptr
            && (argument == "-e" || argument == "--")) {
            return {};
        }
        if (!argument.starts_with('+')) continue;

        if (selected != nullptr) {
            return {
                .disposition = GhosttyCliActionDisposition::Multiple,
                .argument = argument,
            };
        }
        selected = findPinnedAction(argument);
        if (selected == nullptr) {
            return {
                .disposition = GhosttyCliActionDisposition::Unsupported,
                .argument = argument,
            };
        }
        selectedArgument = argument;
    }

    if (selected == nullptr) return {};
    return {
        .disposition = selected->isDelegated()
            ? GhosttyCliActionDisposition::Delegate
            : GhosttyCliActionDisposition::Unsupported,
        .argument = selectedArgument,
    };
}

int GhosttyCliExecError::exitCode() const noexcept
{
    return cause == std::errc::no_such_file_or_directory
            || cause == std::errc::not_a_directory
        ? 127
        : 126;
}

GhosttyCliExecError execGhosttyCliHelper(
    std::span<char *const> arguments,
    std::string_view helperName)
{
    const std::filesystem::path helperFilename{std::string(helperName)};
    if (arguments.empty() || helperName.empty()
        || helperFilename.has_parent_path()
        || std::ranges::find(arguments, nullptr) != arguments.end()) {
        return {
            .target = {},
            .cause = invalidArgument(),
        };
    }

    std::error_code pathError;
    std::filesystem::path executable =
        std::filesystem::read_symlink("/proc/self/exe", pathError);
    if (pathError) {
        return {
            .target = "/proc/self/exe",
            .cause = pathError,
        };
    }
    if (!executable.is_absolute() || executable.parent_path().empty()) {
        return {
            .target = std::move(executable),
            .cause = invalidArgument(),
        };
    }

    std::filesystem::path helper =
        executable.parent_path() / helperFilename;
    std::string encodedHelper = helper.native();
    std::vector<char *> helperArguments;
    helperArguments.reserve(arguments.size() + 1);
    helperArguments.push_back(encodedHelper.data());
    helperArguments.insert(helperArguments.end(),
                           arguments.begin() + 1, arguments.end());
    helperArguments.push_back(nullptr);

    ::execv(encodedHelper.c_str(), helperArguments.data());
    const int execError = errno;
    return {
        .target = std::move(helper),
        .cause = std::error_code(execError, std::generic_category()),
    };
}
