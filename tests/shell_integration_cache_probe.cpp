#include "session/ghostty_shell_integration_p.h"

#include <QCoreApplication>
#include <QDebug>
#include <QProcessEnvironment>

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (application.arguments().size() != 2) {
        qCritical() << "usage: shell-integration-cache-probe HELPER";
        return 2;
    }

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("LD_PRELOAD"));
    environment.remove(QStringLiteral("LD_LIBRARY_PATH"));
    environment.remove(QStringLiteral("LD_AUDIT"));
    const GhosttyShellIntegrationProcessOptions options{
        .helperPath = application.arguments().at(1),
        .environment = environment,
    };
    const GhosttyShellIntegrationRequest request{
        .command = TerminalCommand::shell(QByteArrayLiteral("sh"), true),
        .mode = GhosttyShellIntegrationMode::None,
    };

    if (!resetGhosttyShellIntegrationCacheForTest()) return 3;
    const auto first = prepareCachedGhosttyShellIntegration(options, request);
    if (!first.has_value()) {
        qCritical().noquote() << first.error();
        return 4;
    }
    const auto second = prepareCachedGhosttyShellIntegration(options, request);
    if (!second.has_value()) {
        qCritical().noquote() << second.error();
        return 5;
    }

    const auto snapshot = ghosttyShellIntegrationCacheSnapshotForTest();
    if (*second != *first || snapshot.misses != 1 || snapshot.hits != 1
        || snapshot.launches != 1 || snapshot.bypasses != 0
        || snapshot.entries != 1) {
        qCritical() << "installed helper did not use one cached preparation"
                    << "misses" << snapshot.misses << "hits" << snapshot.hits
                    << "launches" << snapshot.launches << "bypasses"
                    << snapshot.bypasses << "entries" << snapshot.entries;
        return 6;
    }
    return 0;
}
