#include "ghostty_config_edit.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>

namespace {

QString nativeError(const QString &operation, const QString &path,
                    int errorNumber)
{
    return QStringLiteral("%1 '%2': %3")
        .arg(operation, path,
             QString::fromLocal8Bit(std::strerror(errorNumber)));
}

std::expected<std::optional<qint64>, QString> candidateSize(const QString &path)
{
    const QByteArray nativePath = QFile::encodeName(path);
    const int descriptor = ::open(nativePath.constData(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        const int errorNumber = errno;
        if (errorNumber == ENOENT) return std::optional<qint64>{};
        return std::unexpected(nativeError(QStringLiteral("Could not inspect"),
                                           path, errorNumber));
    }

    struct stat status{};
    if (::fstat(descriptor, &status) != 0) {
        const int errorNumber = errno;
        (void)::close(descriptor);
        return std::unexpected(nativeError(QStringLiteral("Could not inspect"),
                                           path, errorNumber));
    }
    (void)::close(descriptor);
    return std::optional<qint64>{static_cast<qint64>(status.st_size)};
}

std::expected<void, QString> createConfigFile(const QString &path)
{
    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory)) {
        return std::unexpected(
            QStringLiteral("Could not create config directory '%1'")
                .arg(directory));
    }

    const QByteArray nativePath = QFile::encodeName(path);
    const int descriptor = ::open(
        nativePath.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (descriptor >= 0) {
        (void)::close(descriptor);
        return {};
    }

    const int errorNumber = errno;
    if (errorNumber == EEXIST) return {};
    return std::unexpected(nativeError(
        QStringLiteral("Could not create config file"), path, errorNumber));
}

} // namespace

std::expected<QString, QString>
prepareGhosttyConfigForEditing(const QStringList &editCandidatePaths)
{
    if (editCandidatePaths.isEmpty()) {
        return std::unexpected(
            QStringLiteral("No Ghostty config edit paths are available"));
    }

    std::optional<QString> selected;
    std::optional<QString> firstExisting;
    for (const QString &rawPath : editCandidatePaths) {
        if (rawPath.isEmpty() || !QDir::isAbsolutePath(rawPath)) {
            return std::unexpected(
                QStringLiteral("Ghostty config edit paths must be absolute"));
        }
        const std::expected<std::optional<qint64>, QString> size =
            candidateSize(rawPath);
        if (!size.has_value()) return std::unexpected(size.error());
        if (!size->has_value()) continue;
        if (**size > 0) {
            selected = rawPath;
            break;
        }
        if (!firstExisting.has_value()) firstExisting = rawPath;
    }

    const QString selectedPath = selected.value_or(
        firstExisting.value_or(editCandidatePaths.constFirst()));
    if (const auto created = createConfigFile(selectedPath);
        !created.has_value()) {
        return std::unexpected(created.error());
    }
    return selectedPath;
}

std::expected<QString, QString>
openGhosttyConfigForEditing(const QStringList &editCandidatePaths,
                            GhosttyConfigUrlOpener opener)
{
    std::expected<QString, QString> selected =
        prepareGhosttyConfigForEditing(editCandidatePaths);
    if (!selected.has_value()) return selected;

    if (!opener) {
        opener = [](const QUrl &url) { return QDesktopServices::openUrl(url); };
    }
    const QUrl url = QUrl::fromLocalFile(*selected);
    if (!opener(url)) {
        return std::unexpected(
            QStringLiteral("The desktop could not open Ghostty config '%1'")
                .arg(*selected));
    }
    return selected;
}
