#include <ghostty.h>

#include <cstdio>
#include <cstring>
#include <cstdint>

extern "C" ghostty_string_s ghostty_qt_config_json(bool defaults);

namespace {

constexpr auto kShowConfigJsonAction = "+show-config-json";

bool isShowConfigJsonAction(const char *argument)
{
    return std::strcmp(argument, kShowConfigJsonAction) == 0;
}

int showConfigJson(int argc, char **argv)
{
    bool defaults = false;
    bool foundAction = false;
    for (int index = 1; index < argc; ++index) {
        if (isShowConfigJsonAction(argv[index])) {
            if (foundAction) {
                std::fputs("ghostty-qt-config-helper: duplicate +show-config-json action\n",
                           stderr);
                return 64;
            }
            foundAction = true;
        } else if (std::strcmp(argv[index], "--default") == 0) {
            if (defaults) {
                std::fputs("ghostty-qt-config-helper: duplicate --default option\n", stderr);
                return 64;
            }
            defaults = true;
        } else {
            std::fprintf(stderr,
                         "ghostty-qt-config-helper: unsupported option for "
                         "+show-config-json: %s\n",
                         argv[index]);
            return 64;
        }
    }

    // Ghostty doesn't know this project-private action. Initialize its global
    // state with only argv[0], then let the private C export load either the
    // ordinary config files or the built-in defaults exactly once.
    char *initializationArguments[] = {argv[0]};
    const int initializationResult = ghostty_init(1, initializationArguments);
    if (initializationResult != 0) {
        return initializationResult;
    }

    const ghostty_string_s json = ghostty_qt_config_json(defaults);
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

} // namespace

int main(int argc, char **argv)
{
    for (int index = 1; index < argc; ++index) {
        if (isShowConfigJsonAction(argv[index])) {
            return showConfigJson(argc, argv);
        }
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
