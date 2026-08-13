#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <string_view>
#include <vector>

#include <termios.h>
#include <unistd.h>

namespace {

bool parsePositive(std::string_view text, std::uint64_t *result)
{
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), *result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
        && *result > 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    std::uint64_t remaining = 0;
    std::uint64_t chunkBytes = 0;
    if (!parsePositive(argv[1], &remaining)
        || !parsePositive(argv[2], &chunkBytes)
        || chunkBytes > 1024ULL * 1024ULL) {
        return 2;
    }

    struct termios attributes{};
    if (::tcgetattr(STDOUT_FILENO, &attributes) == 0) {
        ::cfmakeraw(&attributes);
        (void)::tcsetattr(STDOUT_FILENO, TCSANOW, &attributes);
    }

    constexpr std::string_view pattern = "terminal-session-io ";
    std::vector<char> payload(static_cast<size_t>(chunkBytes));
    for (size_t index = 0; index < payload.size(); ++index) {
        payload[index] = pattern.at(index % pattern.size());
    }
    while (remaining > 0) {
        const size_t requested = static_cast<size_t>(
            std::min(remaining, static_cast<std::uint64_t>(payload.size())));
        size_t offset = 0;
        while (offset < requested) {
            ssize_t written = -1;
            do {
                written = ::write(STDOUT_FILENO, payload.data() + offset,
                                  requested - offset);
            } while (written < 0 && errno == EINTR);
            if (written <= 0) return 1;
            offset += static_cast<size_t>(written);
            remaining -= static_cast<std::uint64_t>(written);
        }
    }
    return 0;
}
