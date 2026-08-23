#include "terminal/core/terminal_drop.h"

#include <QMimeData>
#include <QUrl>

#include <utility>

namespace {

bool needsDropEscape(QChar character) noexcept
{
    switch (character.unicode()) {
    case '\\':
    case '"':
    case '\'':
    case '$':
    case '`':
    case '*':
    case '?':
    case ' ':
    case '|':
    case '(':
    case ')': return true;
    default: return false;
    }
}

} // namespace

QString escapeTerminalDropPath(QStringView path)
{
    QString escaped;
    escaped.reserve(path.size());
    for (const QChar character : path) {
        if (needsDropEscape(character)) escaped.append(u'\\');
        escaped.append(character);
    }
    return escaped;
}

TerminalDropContent terminalDropContent(const QMimeData &mimeData)
{
    if (mimeData.hasUrls()) {
        QString text;
        for (const QUrl &url : mimeData.urls()) {
            if (!url.isLocalFile()) continue;
            const QString path = url.toLocalFile();
            if (path.isEmpty()) continue;
            text.append(escapeTerminalDropPath(path));
            text.append(u'\n');
        }
        return {
            .recognized = true,
            .text = std::move(text),
        };
    }
    if (mimeData.hasText()) {
        return {
            .recognized = true,
            .text = mimeData.text(),
        };
    }
    return {};
}
