#include "terminal_clipboard.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QMimeData>
#include <QThread>

namespace {

void assertGuiThread()
{
    Q_ASSERT(QCoreApplication::instance() != nullptr);
    Q_ASSERT(QThread::currentThread()
             == QCoreApplication::instance()->thread());
}

} // namespace

void writeTerminalClipboard(QClipboard *clipboard, const QString &text,
                            TerminalClipboardDestination destination)
{
    assertGuiThread();
    if (clipboard == nullptr) {
        return;
    }

    const TerminalClipboardWriteTargets targets =
        terminalClipboardWriteTargets(destination,
                                      clipboard->supportsSelection());
    if (targets.standard) {
        clipboard->setText(text, QClipboard::Clipboard);
    }
    if (targets.primary) {
        clipboard->setText(text, QClipboard::Selection);
    }
}

std::optional<QString> readTerminalClipboard(
    QClipboard *clipboard, TerminalClipboardSource source)
{
    assertGuiThread();
    if (clipboard == nullptr
        || (source == TerminalClipboardSource::Primary
            && !clipboard->supportsSelection())) {
        return std::nullopt;
    }

    const QClipboard::Mode mode = source == TerminalClipboardSource::Primary
        ? QClipboard::Selection
        : QClipboard::Clipboard;
    const QMimeData *const mimeData = clipboard->mimeData(mode);
    if (mimeData == nullptr || !mimeData->hasText()) {
        return std::nullopt;
    }
    return mimeData->text();
}

QString readMiddleClickClipboard(QClipboard *clipboard,
                                 TerminalCopyOnSelectMode copyOnSelect)
{
    const TerminalClipboardSource source = terminalMiddleClickSource(
        copyOnSelect,
        clipboard != nullptr && clipboard->supportsSelection());
    return readTerminalClipboard(clipboard, source).value_or(QString{});
}
