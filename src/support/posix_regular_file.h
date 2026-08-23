#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QtTypes>

#include <expected>

enum class PosixRegularFileErrorKind {
    InvalidPath,
    Open,
    Inspect,
    NotRegular,
    TooLarge,
    Read,
};

struct PosixRegularFileError {
    PosixRegularFileErrorKind kind;
    int systemError = 0;
};

[[nodiscard]] std::expected<qint64, PosixRegularFileError>
inspectPosixRegularFile(QByteArrayView path);

[[nodiscard]] std::expected<QByteArray, PosixRegularFileError>
readBoundedPosixRegularFile(QByteArrayView path, qsizetype maximumSize);
