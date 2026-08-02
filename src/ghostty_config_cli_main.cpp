#include "ghostty_cli_delegation.h"

#include <ghostty.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

extern "C" ghostty_string_s
ghostty_qt_config_json(std::uint8_t colorScheme, std::uint8_t probableCli,
                       ghostty_string_s *errorMessage);
extern "C" ghostty_string_s
ghostty_qt_shell_integration_json(const std::uint8_t *request,
                                  std::size_t requestLength);

namespace {

constexpr auto kShowConfigJsonAction = "+show-config-json";
constexpr auto kShellIntegrationJsonAction = "+shell-integration-json";
constexpr std::string_view kColorSchemeOption = "--ghostty-qt-color-scheme=";
constexpr std::string_view kProbableCliOption = "--ghostty-qt-probable-cli=";
constexpr std::size_t kMaximumShellIntegrationRequestBytes = 4U * 1024U * 1024U;
constexpr auto kResourcesEnvironment = "GHOSTTY_RESOURCES_DIR";

enum class ConfigColorScheme : std::uint8_t {
    Light,
    Dark,
};

void configureGhosttyResourcesDirectory()
{
    const char *const configured = std::getenv(kResourcesEnvironment);
    if (configured != nullptr && configured[0] != '\0') return;

    std::error_code error;
    const std::filesystem::path executable =
        std::filesystem::read_symlink("/proc/self/exe", error);
    if (error || executable.empty()) return;

    const std::filesystem::path executableDirectory = executable.parent_path();
    const std::array candidates{
        executableDirectory,
        (executableDirectory
         / std::filesystem::path(GHOSTTY_QT_INSTALL_RESOURCES_RELATIVE_DIR))
            .lexically_normal(),
    };
    for (const std::filesystem::path &candidate : candidates) {
        error.clear();
        if (!std::filesystem::is_directory(candidate / "themes", error)
            || error) {
            continue;
        }
        const std::string encoded = candidate.native();
        static_cast<void>(::setenv(kResourcesEnvironment, encoded.c_str(), 1));
        return;
    }
}

bool isShowConfigJsonAction(const char *argument)
{
    return std::strcmp(argument, kShowConfigJsonAction) == 0;
}

bool isShellIntegrationJsonAction(const char *argument)
{
    return std::strcmp(argument, kShellIntegrationJsonAction) == 0;
}

bool isPublicShowConfigOption(std::string_view argument)
{
    return argument == "-h" || argument == "--help" || argument == "--default"
        || argument == "--changes-only" || argument == "--docs"
        || argument == "--no-pager";
}

int showConfigJson(std::span<char *const> arguments)
{
    std::optional<ConfigColorScheme> colorScheme;
    std::optional<bool> probableCli;
    std::vector<char *> configurationArguments;
    configurationArguments.reserve(arguments.size());
    configurationArguments.push_back(arguments.front());

    char showConfigAction[] = "+show-config";
    configurationArguments.push_back(showConfigAction);
    for (char *const argument : arguments.subspan(2)) {
        if (argument == nullptr) continue;
        const std::string_view value(argument);
        if (value.starts_with(kColorSchemeOption)) {
            const std::string_view scheme =
                value.substr(kColorSchemeOption.size());
            if (colorScheme.has_value() || scheme.empty()
                || (scheme != "light" && scheme != "dark")) {
                std::fputs(
                    "ghostty-qt-config-helper: +show-config-json requires "
                    "exactly one --ghostty-qt-color-scheme=light|dark option\n",
                    stderr);
                return 64;
            }
            colorScheme = scheme == "light" ? ConfigColorScheme::Light
                                            : ConfigColorScheme::Dark;
            continue;
        }
        if (value.starts_with("--ghostty-qt-color-scheme")) {
            std::fputs(
                "ghostty-qt-config-helper: +show-config-json requires "
                "exactly one --ghostty-qt-color-scheme=light|dark option\n",
                stderr);
            return 64;
        }
        if (value.starts_with(kProbableCliOption)) {
            const std::string_view enabled =
                value.substr(kProbableCliOption.size());
            if (probableCli.has_value()
                || (enabled != "true" && enabled != "false")) {
                std::fputs(
                    "ghostty-qt-config-helper: +show-config-json accepts at "
                    "most one --ghostty-qt-probable-cli=true|false option\n",
                    stderr);
                return 64;
            }
            probableCli = enabled == "true";
            continue;
        }
        if (value.starts_with("--ghostty-qt-probable-cli")) {
            std::fputs(
                "ghostty-qt-config-helper: +show-config-json accepts at most "
                "one --ghostty-qt-probable-cli=true|false option\n",
                stderr);
            return 64;
        }
        if (isPublicShowConfigOption(value)) {
            std::fputs(
                "ghostty-qt-config-helper: +show-config-json takes no options "
                "from +show-config; pass configuration --key=value arguments\n",
                stderr);
            return 64;
        }
        configurationArguments.push_back(argument);
    }
    if (!colorScheme.has_value()) {
        std::fputs(
            "ghostty-qt-config-helper: +show-config-json requires exactly one "
            "--ghostty-qt-color-scheme=light|dark option\n",
            stderr);
        return 64;
    }

    // Ghostty doesn't know this project-private action. Give its global state
    // the recognized public action whose finalized values this export replaces.
    // Keep every configuration argument byte-for-byte while stripping the
    // helper-only color-scheme selector so this query has the same explicit
    // CLI precedence as the terminal surfaces.
    const int initializationResult = ghostty_init(
        configurationArguments.size(), configurationArguments.data());
    if (initializationResult != 0) {
        return initializationResult;
    }

    ghostty_string_s errorMessage{};
    const ghostty_string_s json = ghostty_qt_config_json(
        static_cast<std::uint8_t>(*colorScheme),
        static_cast<std::uint8_t>(probableCli.value_or(true)), &errorMessage);
    if (json.ptr == nullptr) {
        if (errorMessage.ptr != nullptr) {
            std::fwrite(errorMessage.ptr, 1, errorMessage.len, stderr);
            if (errorMessage.len == 0
                || errorMessage.ptr[errorMessage.len - 1] != '\n') {
                std::fputc('\n', stderr);
            }
            ghostty_string_free(errorMessage);
        } else {
            std::fputs(
                "ghostty-qt-config-helper: failed to export structured config\n",
                stderr);
        }
        return 1;
    }
    if (errorMessage.ptr != nullptr) {
        ghostty_string_free(errorMessage);
    }

    const std::size_t written = std::fwrite(json.ptr, 1, json.len, stdout);
    const bool writeFailed =
        written != json.len || std::fputc('\n', stdout) == EOF;
    ghostty_string_free(json);
    return writeFailed ? 74 : 0;
}

int prepareShellIntegrationJson(std::span<char *const> arguments)
{
    if (arguments.size() != 2) {
        std::fputs("ghostty-qt-config-helper: +shell-integration-json takes no "
                   "arguments; pass one JSON request on standard input\n",
                   stderr);
        return 64;
    }

    // This private action only needs Ghostty's allocator and module runtime.
    // Initialize through a recognized public action without asking Ghostty to
    // dispatch it; the request itself carries already-finalized configuration.
    char showConfigAction[] = "+show-config";
    std::vector<char *> initializationArguments(arguments.begin(),
                                                arguments.end());
    initializationArguments[1] = showConfigAction;
    const int initializationResult = ghostty_init(
        initializationArguments.size(), initializationArguments.data());
    if (initializationResult != 0) {
        return initializationResult;
    }

    std::vector<std::uint8_t> request;
    request.reserve(64U * 1024U);
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    for (;;) {
        const std::size_t count =
            std::fread(buffer.data(), 1, buffer.size(), stdin);
        if (count > kMaximumShellIntegrationRequestBytes - request.size()) {
            std::fputs(
                "ghostty-qt-config-helper: shell-integration request exceeds "
                "the 4 MiB protocol limit\n",
                stderr);
            return 65;
        }
        request.insert(request.end(), buffer.begin(),
                       buffer.begin() + static_cast<std::ptrdiff_t>(count));
        if (count == buffer.size()) continue;
        if (std::ferror(stdin) != 0) {
            std::fputs(
                "ghostty-qt-config-helper: failed to read shell-integration "
                "request\n",
                stderr);
            return 74;
        }
        break;
    }
    if (request.empty()) {
        std::fputs(
            "ghostty-qt-config-helper: shell-integration request is empty\n",
            stderr);
        return 65;
    }

    const ghostty_string_s json =
        ghostty_qt_shell_integration_json(request.data(), request.size());
    if (json.ptr == nullptr) {
        std::fputs(
            "ghostty-qt-config-helper: failed to prepare shell integration\n",
            stderr);
        return 65;
    }

    const std::size_t written = std::fwrite(json.ptr, 1, json.len, stdout);
    const bool writeFailed =
        written != json.len || std::fputc('\n', stdout) == EOF;
    ghostty_string_free(json);
    return writeFailed ? 74 : 0;
}

bool isPrivateConfigExport(std::span<char *const> arguments)
{
    if (arguments.size() < 2 || !isShowConfigJsonAction(arguments[1])) {
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

bool isPrivateShellIntegration(std::span<char *const> arguments)
{
    return arguments.size() >= 2 && isShellIntegrationJsonAction(arguments[1]);
}

} // namespace

int main(int argc, char **argv)
{
    configureGhosttyResourcesDirectory();

    const std::span<char *const> arguments(argv,
                                           static_cast<std::size_t>(argc));
    if (isPrivateConfigExport(arguments)) {
        return showConfigJson(arguments);
    }
    if (isPrivateShellIntegration(arguments)) {
        return prepareShellIntegrationJson(arguments);
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
