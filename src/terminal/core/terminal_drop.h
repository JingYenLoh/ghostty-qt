#pragma once

#include <QString>
#include <QStringView>

class QMimeData;

struct TerminalDropContent {
    bool recognized = false;
    QString text;

    bool operator==(const TerminalDropContent &) const = default;
};

// Escape one local path for insertion into a shell command line. This mirrors
// Ghostty's frontend drop contract rather than attempting shell-specific
// quoting: only the documented metacharacters receive a leading backslash.
[[nodiscard]] QString escapeTerminalDropPath(QStringView path);

// URLs take precedence over text. A URL set is recognized even when every URL
// is remote, allowing the caller to consume it without falling back to an
// accompanying text representation.
[[nodiscard]] TerminalDropContent
terminalDropContent(const QMimeData &mimeData);
