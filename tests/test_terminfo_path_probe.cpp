#include "terminfo_paths.h"

#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    const TerminfoResolution resolution = resolveRuntimeTerminfoDirectory();
    if (!resolution) {
        QTextStream(stderr)
            << "ghostty-qt-terminfo-probe: " << resolution.error() << '\n';
        return 1;
    }

    QTextStream(stdout) << *resolution << '\n';
    return 0;
}
