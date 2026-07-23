#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {

bool writeField(const char *name, std::string_view value)
{
    return std::fprintf(stdout, "%s %zu\n", name, value.size()) >= 0
        && std::fwrite(value.data(), 1, value.size(), stdout) == value.size()
        && std::fputc('\n', stdout) != EOF;
}

bool writeFile(const char *path, std::string_view value, const char *mode)
{
    if (path == nullptr || *path == '\0') return false;
    std::FILE *const file = std::fopen(path, mode);
    if (file == nullptr) return false;
    const bool written = value.empty()
        || std::fwrite(value.data(), 1, value.size(), file) == value.size();
    const bool closed = std::fclose(file) == 0;
    return written && closed;
}

int configuredExitCode(const char *name, int fallback)
{
    const char *const raw = std::getenv(name);
    if (raw == nullptr) return fallback;

    int result = 0;
    const std::string_view value(raw);
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()
            && result >= 0 && result <= 255
        ? result
        : fallback;
}

// When a phase-log path is present, the same executable becomes a deterministic
// three-phase SSH stand-in: destination resolution, terminfo upload, then the
// ordinary framed final-child report below.
int handleFakeSshPhase(int argc, char *argv[], std::string_view input)
{
    const char *const logPath =
        std::getenv("GHOSTTY_QT_FAKE_SSH_PHASE_LOG");
    if (logPath == nullptr) return -1;

    if (argc > 1 && std::string_view(argv[1]) == "-G") {
        if (!writeFile(logPath, "resolve\n", "ab")) return 74;
        if (std::fputs(
                "user fixture-user\nhostname fixture.example\n", stdout)
            == EOF) {
            return 74;
        }
        return configuredExitCode(
            "GHOSTTY_QT_FAKE_SSH_RESOLVE_EXIT", 0);
    }

    bool installing = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "ControlMaster=yes") {
            installing = true;
            break;
        }
    }
    if (!installing) {
        return writeFile(logPath, "final\n", "ab") ? -1 : 74;
    }

    if (!writeFile(logPath, "install\n", "ab")) return 74;
    const char *const payloadPath =
        std::getenv("GHOSTTY_QT_FAKE_SSH_TERMINFO_PAYLOAD");
    if (!writeFile(payloadPath, input, "wb")) return 74;
    return configuredExitCode("GHOSTTY_QT_FAKE_SSH_INSTALL_EXIT", 0);
}

} // namespace

int main(int argc, char *argv[])
{
    std::vector<char> input;
    std::array<char, 4096> buffer{};
    while (const std::size_t count =
               std::fread(buffer.data(), 1, buffer.size(), stdin)) {
        input.insert(input.end(), buffer.begin(), buffer.begin() +
            static_cast<std::ptrdiff_t>(count));
    }
    if (std::ferror(stdin)) return 74;

    const std::string_view inputView = input.empty()
        ? std::string_view{}
        : std::string_view(input.data(), input.size());
    const int fakeSshResult = handleFakeSshPhase(
        argc, argv, inputView);
    if (fakeSshResult >= 0) return fakeSshResult;

    std::error_code pathError;
    const std::string workingDirectory =
        std::filesystem::current_path(pathError).native();
    if (pathError) return 74;
    const char *const sentinel = std::getenv("GHOSTTY_QT_CLI_SENTINEL");

    if (std::fprintf(stdout, "PID %lld\nARGC %d\n",
                     static_cast<long long>(::getpid()), argc) < 0) {
        return 74;
    }
    for (int index = 0; index < argc; ++index) {
        if (!writeField("ARG", argv[index])) return 74;
    }
    if (!writeField("CWD", workingDirectory)
        || !writeField("ENV", sentinel == nullptr
                ? std::string_view{}
                : std::string_view(sentinel))
        || !writeField("STDIN", inputView)) {
        return 74;
    }

    constexpr char ErrorMarker[] = "fake-stderr\0binary";
    if (std::fwrite(ErrorMarker, 1, sizeof(ErrorMarker) - 1, stderr)
        != sizeof(ErrorMarker) - 1) {
        return 74;
    }
    return configuredExitCode("GHOSTTY_QT_FAKE_SSH_FINAL_EXIT", 73);
}
