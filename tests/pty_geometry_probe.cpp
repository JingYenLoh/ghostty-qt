#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdio>

int main(int argc, char **argv)
{
    if (argc > 2) {
        std::fputs("usage: pty-geometry-probe [output-file]\n", stderr);
        return 2;
    }

    struct winsize size{};
    if (::ioctl(STDIN_FILENO, TIOCGWINSZ, &size) < 0) {
        std::perror("TIOCGWINSZ");
        return 1;
    }

    std::FILE *output = stdout;
    if (argc == 2) {
        output = std::fopen(argv[1], "w");
        if (output == nullptr) {
            std::perror("fopen");
            return 1;
        }
    }

    const int written =
        std::fprintf(output, "ghostty-qt-pty-geometry:%u:%u:%u:%u\n",
                     static_cast<unsigned int>(size.ws_col),
                     static_cast<unsigned int>(size.ws_row),
                     static_cast<unsigned int>(size.ws_xpixel),
                     static_cast<unsigned int>(size.ws_ypixel));
    const bool flushed = written >= 0 && std::fflush(output) == 0;
    const bool closed = output == stdout || std::fclose(output) == 0;
    return flushed && closed ? 0 : 1;
}
