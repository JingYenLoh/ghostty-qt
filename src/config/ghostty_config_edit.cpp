#include "config/ghostty_config_edit.h"
#include "support/posix_regular_file.h"
#include "support/unique_file_descriptor.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <utility>

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
    const auto size = inspectPosixRegularFile(nativePath);
    if (size) return std::optional<qint64>{*size};

    const PosixRegularFileError error = size.error();
    if (error.kind == PosixRegularFileErrorKind::Open
        && error.systemError == ENOENT) {
        return std::optional<qint64>{};
    }
    if (error.kind == PosixRegularFileErrorKind::InvalidPath) {
        return std::unexpected(
            QStringLiteral("Could not inspect '%1': path contains a NUL byte")
                .arg(path));
    }
    if (error.kind == PosixRegularFileErrorKind::NotRegular) {
        return std::unexpected(
            QStringLiteral("Could not inspect '%1': not a regular file")
                .arg(path));
    }
    if (error.kind == PosixRegularFileErrorKind::TooLarge) {
        return std::unexpected(
            QStringLiteral("Could not inspect '%1': invalid file size")
                .arg(path));
    }
    return std::unexpected(nativeError(QStringLiteral("Could not inspect"),
                                       path, error.systemError));
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
    constexpr int maximumAttempts = 3;
    for (int attempt = 0; attempt < maximumAttempts; ++attempt) {
        int descriptor = -1;
        do {
            descriptor = ::open(nativePath.constData(),
                                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        } while (descriptor < 0 && errno == EINTR);
        if (descriptor >= 0) {
            const UniqueFileDescriptor file(descriptor);
            return {};
        }

        const int errorNumber = errno;
        if (errorNumber != EEXIST) {
            return std::unexpected(
                nativeError(QStringLiteral("Could not create config file"),
                            path, errorNumber));
        }

        const auto existing = candidateSize(path);
        if (existing && existing->has_value()) return {};
        if (!existing) return std::unexpected(existing.error());
        if (attempt + 1 == maximumAttempts) {
            return std::unexpected(
                QStringLiteral(
                    "Could not create config file '%1': path changed repeatedly")
                    .arg(path));
        }
    }
    std::unreachable();
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
