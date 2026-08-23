#pragma once

#include <QByteArray>

#include <variant>

namespace TerminalInitialInputs {

struct Raw {
    QByteArray bytes;

    bool operator==(const Raw &) const = default;
};

struct Path {
    QByteArray path;

    bool operator==(const Path &) const = default;
};

} // namespace TerminalInitialInputs

using TerminalInitialInput =
    std::variant<TerminalInitialInputs::Raw, TerminalInitialInputs::Path>;
