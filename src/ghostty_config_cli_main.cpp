#include "ghostty_cli_delegation.h"

#include <ghostty.h>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <span>
#include <string_view>

extern "C" ghostty_string_s ghostty_qt_config_json();

namespace {

constexpr auto kShowConfigJsonAction = "+show-config-json";

bool isShowConfigJsonAction(const char *argument)
{
    return std::strcmp(argument, kShowConfigJsonAction) == 0;
}

int showConfigJson(int argc, char **argv)
{
    if (argc != 2) {
        std::fputs(
            "ghostty-qt-config-helper: +show-config-json takes no options\n",
            stderr);
        return 64;
    }

    // Ghostty doesn't know this project-private action. Give its global state
    // the recognized public action whose finalized values this export replaces.
    // Ghostty's config iterator ignores action tokens, while the extra argument
    // preserves its established probable-CLI defaults even when TERM_PROGRAM
    // is absent.
    char showConfigAction[] = "+show-config";
    char *initializationArguments[] = {argv[0], showConfigAction};
    const int initializationResult = ghostty_init(2, initializationArguments);
    if (initializationResult != 0) {
        return initializationResult;
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
        return showConfigJson(argc, argv);
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
