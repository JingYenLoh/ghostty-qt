#include <ghostty.h>

#include <cstdint>

int main(int argc, char **argv)
{
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
