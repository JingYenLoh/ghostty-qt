#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::string environmentValue(const char *name)
{
    const char *value = std::getenv(name);
    return value ? value : "";
}

void logInvocation(int argc, char **argv)
{
    const std::string logPath =
        environmentValue("GHOSTTY_QT_FAKE_INVOCATION_LOG");
    if (logPath.empty()) {
        return;
    }

    std::ofstream log(logPath, std::ios::app);
    for (int i = 1; i < argc; ++i) {
        if (i > 1) {
            log << ' ';
        }
        log << argv[i];
    }
    log << '\n';
}

bool hasArgument(int argc, char **argv, const std::string &wanted)
{
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == wanted) {
            return true;
        }
    }
    return false;
}

int actionInvocationCount(const std::string &action)
{
    const std::string logPath =
        environmentValue("GHOSTTY_QT_FAKE_INVOCATION_LOG");
    if (logPath.empty()) {
        return 0;
    }
    std::ifstream log(logPath);
    int count = 0;
    std::string line;
    while (std::getline(log, line)) {
        if (line == action) {
            ++count;
        }
    }
    return count;
}

} // namespace

int main(int argc, char **argv)
{
    logInvocation(argc, argv);

    const std::string expectedXdg =
        environmentValue("GHOSTTY_QT_FAKE_EXPECT_XDG_CONFIG_HOME");
    if (!expectedXdg.empty()
        && environmentValue("XDG_CONFIG_HOME") != expectedXdg) {
        std::cerr << "unexpected XDG_CONFIG_HOME";
        return 91;
    }
    if (hasArgument(argc, argv, "--no-pager")) {
        std::cerr << "unexpected --no-pager";
        return 92;
    }
    if (argc < 2) {
        return 64;
    }

    const std::string action = argv[1];
    const std::string mode = environmentValue("GHOSTTY_QT_FAKE_MODE");
    if (action == "+validate-config") {
        if (mode == "validation-timeout") {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        } else if (mode == "validation-crash") {
            std::abort();
        } else if (mode == "validation-failure") {
            std::cout << "config.ghostty:2:1: invalid value\n";
            return 1;
        } else if (mode == "post-validation-failure"
                   && actionInvocationCount("+validate-config") >= 2) {
            std::cout << "config changed during query\n";
            return 1;
        }
        return 0;
    }

    if (action == "+show-keybinds-json") {
        if (mode == "keybinding-query-failure") {
            std::cerr << "keybinding query failed";
            return 8;
        }
        if (mode == "keybinding-query-malformed") {
            std::cout << "{not-json";
            return 0;
        }
        std::cout << environmentValue("GHOSTTY_QT_FAKE_KEYBIND_OUTPUT");
        return 0;
    }

    if (action != "+show-config") {
        return 64;
    }

    const bool defaults = hasArgument(argc, argv, "--default");
    if ((defaults && mode == "default-query-failure")
        || (!defaults && mode == "current-query-failure")) {
        std::cerr << "query failed";
        return 7;
    }

    const char *variable = defaults
        ? "GHOSTTY_QT_FAKE_DEFAULT_OUTPUT"
        : "GHOSTTY_QT_FAKE_CHANGES_OUTPUT";
    if (!defaults) {
        std::cerr << environmentValue("GHOSTTY_QT_FAKE_SUCCESS_WARNING");
    }
    std::cout << environmentValue(variable);
    if (!defaults && mode == "query-consistency-mismatch"
        && actionInvocationCount("+show-config") >= 2) {
        std::cout << "font-size = 18\n";
    }
    return 0;
}
