#include "posix_regular_file.h"

#include "unique_file_descriptor.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct OpenedRegularFile {
    UniqueFileDescriptor descriptor;
    qint64 size;
};

std::expected<OpenedRegularFile, PosixRegularFileError>
openRegularFile(QByteArrayView path)
{
    if (path.contains('\0')) {
        return std::unexpected(PosixRegularFileError{
            .kind = PosixRegularFileErrorKind::InvalidPath,
            .systemError = EINVAL,
        });
    }

    const QByteArray nullTerminatedPath(path.data(), path.size());
    int descriptor = -1;
    do {
        descriptor = ::open(nullTerminatedPath.constData(),
                            O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        return std::unexpected(PosixRegularFileError{
            .kind = PosixRegularFileErrorKind::Open,
            .systemError = errno,
        });
    }

    UniqueFileDescriptor file(descriptor);
    struct stat status{};
    if (::fstat(file.get(), &status) != 0) {
        return std::unexpected(PosixRegularFileError{
            .kind = PosixRegularFileErrorKind::Inspect,
            .systemError = errno,
        });
    }
    if (!S_ISREG(status.st_mode)) {
        return std::unexpected(PosixRegularFileError{
            .kind = PosixRegularFileErrorKind::NotRegular,
        });
    }
    if (status.st_size < 0) {
        return std::unexpected(PosixRegularFileError{
            .kind = PosixRegularFileErrorKind::TooLarge,
        });
    }

    return OpenedRegularFile{
        .descriptor = std::move(file),
        .size = static_cast<qint64>(status.st_size),
    };
}

} // namespace

std::expected<qint64, PosixRegularFileError>
inspectPosixRegularFile(QByteArrayView path)
{
    auto file = openRegularFile(path);
    if (!file) return std::unexpected(file.error());
    return file->size;
}

std::expected<QByteArray, PosixRegularFileError>
readBoundedPosixRegularFile(QByteArrayView path, qsizetype maximumSize)
{
    auto file = openRegularFile(path);
    if (!file) return std::unexpected(file.error());
    if (maximumSize < 0 || file->size > maximumSize) {
        return std::unexpected(PosixRegularFileError{
            .kind = PosixRegularFileErrorKind::TooLarge,
        });
    }

    QByteArray contents;
    contents.reserve(static_cast<qsizetype>(file->size));
    std::array<char, 64 * 1024> buffer{};
    while (true) {
        const qsizetype remaining = maximumSize - contents.size();
        const qsizetype requestSize = remaining < std::ssize(buffer)
            ? remaining + 1
            : std::ssize(buffer);
        ssize_t count = -1;
        do {
            count = ::read(file->descriptor.get(), buffer.data(),
                           static_cast<size_t>(requestSize));
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            return std::unexpected(PosixRegularFileError{
                .kind = PosixRegularFileErrorKind::Read,
                .systemError = errno,
            });
        }
        if (count == 0) break;
        if (count > remaining) {
            return std::unexpected(PosixRegularFileError{
                .kind = PosixRegularFileErrorKind::TooLarge,
            });
        }
        contents.append(buffer.data(), static_cast<qsizetype>(count));
    }
    return contents;
}
