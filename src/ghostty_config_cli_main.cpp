#include "ghostty_cli_delegation.h"

#include <ghostty.h>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

extern "C" ghostty_string_s ghostty_qt_config_json();

namespace {

constexpr auto kShowConfigJsonAction = "+show-config-json";

bool isShowConfigJsonAction(const char *argument)
{
    return std::strcmp(argument, kShowConfigJsonAction) == 0;
}

bool isPublicShowConfigOption(std::string_view argument)
{
    return argument == "-h"
        || argument == "--help"
        || argument == "--default"
        || argument == "--changes-only"
        || argument == "--docs"
        || argument == "--no-pager";
}

int validateEffectiveConfig()
{
    const std::unique_ptr<void, decltype(&ghostty_config_free)> config(
        ghostty_config_new(), &ghostty_config_free);
    if (!config) {
        std::fputs(
            "ghostty-qt-config-helper: failed to allocate config validation\n",
            stderr);
        return 1;
    }

    ghostty_config_load_default_files(config.get());
    ghostty_config_load_cli_args(config.get());
    ghostty_config_load_recursive_files(config.get());
    ghostty_config_finalize(config.get());

    const std::uint32_t diagnosticCount =
        ghostty_config_diagnostics_count(config.get());
    for (std::uint32_t index = 0; index < diagnosticCount; ++index) {
        const ghostty_diagnostic_s diagnostic =
            ghostty_config_get_diagnostic(config.get(), index);
        if (diagnostic.message == nullptr) continue;
        std::fputs(diagnostic.message, stderr);
        std::fputc('\n', stderr);
    }
    return diagnosticCount == 0 ? 0 : 1;
}

int showConfigJson(std::span<char *const> arguments)
{
    for (char *const argument : arguments.subspan(2)) {
        if (argument != nullptr
            && isPublicShowConfigOption(argument)) {
            std::fputs(
                "ghostty-qt-config-helper: +show-config-json takes no options "
                "from +show-config; pass configuration --key=value arguments\n",
                stderr);
            return 64;
        }
    }

    // Ghostty doesn't know this project-private action. Give its global state
    // the recognized public action whose finalized values this export replaces.
    // Keep every following config argument byte-for-byte so this query has the
    // same explicit CLI precedence as the terminal surfaces.
    char showConfigAction[] = "+show-config";
    std::vector<char *> initializationArguments(
        arguments.begin(), arguments.end());
    initializationArguments[1] = showConfigAction;
    const int initializationResult = ghostty_init(
        initializationArguments.size(), initializationArguments.data());
    if (initializationResult != 0) {
        return initializationResult;
    }
    if (const int validationResult = validateEffectiveConfig();
        validationResult != 0) {
        return validationResult;
    }

    const ghostty_string_s json = ghostty_qt_config_json();
    if (json.ptr == nullptr) {
        std::fputs("ghostty-qt-config-helper: failed to export structured config\n",
                   stderr);
        return 1;
    }

    const std::size_t written = std::fwrite(json.ptr, 1, json.len, stdout);
    const bool writeFailed = written != json.len || std::fputc('\n', stdout) == EOF;
    ghostty_string_free(json);
    return writeFailed ? 74 : 0;
}

bool isPrivateConfigExport(std::span<char *const> arguments)
{
    if (arguments.size() < 2
        || !isShowConfigJsonAction(arguments[1])) {
        return false;
    }
    for (char *const rawArgument : arguments.subspan(2)) {
        if (rawArgument != nullptr
            && std::string_view(rawArgument).starts_with('+')) {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    const std::span<char *const> arguments(
        argv, static_cast<std::size_t>(argc));
    if (isPrivateConfigExport(arguments)) {
        return showConfigJson(arguments);
    }

    const GhosttyCliActionSelection selection =
        selectGhosttyCliAction(arguments);
    if (selection.disposition != GhosttyCliActionDisposition::Delegate) {
        std::fputs(
            "ghostty-qt-config-helper: no supported public CLI action was selected\n",
            stderr);
        return 64;
    }

    const int initializationResult =
        ghostty_init(static_cast<std::uintptr_t>(argc), argv);
    if (initializationResult != 0) {
        return initializationResult;
    }

    // A recognized CLI action terminates the process from inside libghostty.
    // Returning means this helper was invoked without an action, which is a
    // caller error: it is not a general-purpose Ghostty executable.
    ghostty_cli_try_action();
    return 64;
}
