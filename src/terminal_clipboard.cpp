#include "terminal_clipboard.h"

#include <QClipboard>
#include <QCoreApplication>
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

QString readMiddleClickClipboard(QClipboard *clipboard,
                                 TerminalCopyOnSelectMode copyOnSelect)
{
    assertGuiThread();
    if (clipboard == nullptr) {
        return {};
    }

    const TerminalClipboardSource source = terminalMiddleClickSource(
        copyOnSelect, clipboard->supportsSelection());
    return clipboard->text(source == TerminalClipboardSource::Primary
                               ? QClipboard::Selection
                               : QClipboard::Clipboard);
}
