#include "session/terminfo_paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace {

QString normalizedDatabaseDirectory(const QString &directory)
{
    const QFileInfo directoryInfo(
        QDir::cleanPath(QDir(directory).absolutePath()));
    if (!directoryInfo.isDir()) {
        return {};
    }

    const QString normalized = directoryInfo.canonicalFilePath();
    const QDir database(normalized);
    const QStringList entryPaths{
        QStringLiteral("x/xterm-ghostty"),
        QStringLiteral("78/xterm-ghostty"),
    };
    for (const QString &entryPath : entryPaths) {
        const QFileInfo entry(database.filePath(entryPath));
        if (entry.isFile() && entry.isReadable()) {
            return normalized;
        }
    }
    return {};
}

} // namespace

TerminfoResolution
resolveTerminfoDirectory(const QString &executableDirectory,
                         const std::optional<QString> &overrideDirectory)
{
    if (overrideDirectory.has_value()) {
        if (overrideDirectory->isEmpty()) {
            return std::unexpected(
                QStringLiteral("GHOSTTY_QT_TERMINFO is set but empty."));
        }

        const QString overridePath =
            QDir::cleanPath(QDir(*overrideDirectory).absolutePath());
        const QString resolvedOverride =
            normalizedDatabaseDirectory(overridePath);
        if (resolvedOverride.isEmpty()) {
            return std::unexpected(
                QStringLiteral("GHOSTTY_QT_TERMINFO='%1' does not contain a "
                               "readable xterm-ghostty entry.")
                    .arg(overridePath));
        }
        return resolvedOverride;
    }

    const QDir executableDir(executableDirectory);
    const QString installedPath =
        QDir::cleanPath(executableDir.absoluteFilePath(
            QStringLiteral(GHOSTTY_QT_INSTALL_TERMINFO_RELATIVE_DIR)));
    const QString installedDatabase =
        normalizedDatabaseDirectory(installedPath);
    if (!installedDatabase.isEmpty()) {
        return installedDatabase;
    }

    // Build-tree executables live next to share/terminfo rather than in bin/.
    const QString buildPath = QDir::cleanPath(
        executableDir.absoluteFilePath(QStringLiteral("share/terminfo")));
    const QString buildDatabase = normalizedDatabaseDirectory(buildPath);
    if (!buildDatabase.isEmpty()) {
        return buildDatabase;
    }

    return std::unexpected(
        QStringLiteral(
            "Unable to locate the xterm-ghostty terminfo database. "
            "Checked installed path '%1' and build-tree path '%2'. "
            "Set GHOSTTY_QT_TERMINFO to an explicit database directory.")
            .arg(installedPath, buildPath));
}

TerminfoResolution resolveRuntimeTerminfoDirectory()
{
    std::optional<QString> overrideDirectory;
    if (qEnvironmentVariableIsSet("GHOSTTY_QT_TERMINFO")) {
        overrideDirectory = qEnvironmentVariable("GHOSTTY_QT_TERMINFO");
    }
    return resolveTerminfoDirectory(QCoreApplication::applicationDirPath(),
                                    overrideDirectory);
}
