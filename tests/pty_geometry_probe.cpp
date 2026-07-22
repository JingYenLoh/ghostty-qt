#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdio>

int main()
{
    struct winsize size {};
    if (::ioctl(STDIN_FILENO, TIOCGWINSZ, &size) < 0) {
        std::perror("TIOCGWINSZ");
        return 1;
    }

    const int written = std::printf(
        "ghostty-qt-pty-geometry:%u:%u:%u:%u\n",
        static_cast<unsigned int>(size.ws_col),
        static_cast<unsigned int>(size.ws_row),
        static_cast<unsigned int>(size.ws_xpixel),
        static_cast<unsigned int>(size.ws_ypixel));
    return written >= 0 && std::fflush(stdout) == 0 ? 0 : 1;
}
