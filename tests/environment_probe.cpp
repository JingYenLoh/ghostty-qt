#include <cstdio>
#include <string_view>

extern char **environ;

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    const std::string_view expectedValue(argv[1]);
    for (char **current = environ; *current != nullptr; ++current) {
        const std::string_view entry(*current);
        const std::size_t separator = entry.find('=');
        if (separator == std::string_view::npos
            || entry.substr(separator + 1) != expectedValue) {
            continue;
        }

        std::fputs("rawkey=", stdout);
        for (const char rawByte : entry) {
            const auto byte = static_cast<unsigned char>(rawByte);
            std::printf("%02x", static_cast<unsigned int>(byte));
        }
        std::fputc('\n', stdout);
        return std::ferror(stdout) == 0 ? 0 : 1;
    }
    return 1;
}
