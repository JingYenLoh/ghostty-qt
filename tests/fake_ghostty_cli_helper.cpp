#include <array>
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
        || !writeField("STDIN", input.empty()
                ? std::string_view{}
                : std::string_view(input.data(), input.size()))) {
        return 74;
    }

    constexpr char ErrorMarker[] = "fake-stderr\0binary";
    if (std::fwrite(ErrorMarker, 1, sizeof(ErrorMarker) - 1, stderr)
        != sizeof(ErrorMarker) - 1) {
        return 74;
    }
    return 73;
}
