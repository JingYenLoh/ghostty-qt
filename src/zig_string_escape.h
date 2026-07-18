#pragma once

#include <QByteArray>
#include <QByteArrayView>

#include <optional>

// Invert std.zig.stringEscape at Binding.Action.format's byte-string boundary.
// In particular, \xNN restores one raw byte.
[[nodiscard]] std::optional<QByteArray> decodeGhosttyActionString(
    QByteArrayView serialized);

// Apply Ghostty config string-literal semantics. In particular, \xNN denotes
// a Unicode codepoint that is encoded as UTF-8.
[[nodiscard]] std::optional<QByteArray> decodeGhosttyConfigString(
    QByteArrayView serialized);
