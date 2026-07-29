#include "terminal_clipboard.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QMimeData>
#include <QThread>

#include <memory>

namespace {

void assertGuiThread()
{
    Q_ASSERT(QCoreApplication::instance() != nullptr);
    Q_ASSERT(QThread::currentThread()
             == QCoreApplication::instance()->thread());
}

} // namespace

bool writeTerminalClipboard(QClipboard *clipboard,
                            const TerminalClipboardWrite &write)
{
    assertGuiThread();
    if (clipboard == nullptr || !validTerminalClipboardWritePayload(write)) {
        return false;
    }

    const std::optional<TerminalClipboardSource> target =
        terminalClipboardWriteTarget(write.location,
                                     clipboard->supportsSelection());
    if (!target.has_value()) {
        return false;
    }
    const QClipboard::Mode mode = *target == TerminalClipboardSource::Primary
        ? QClipboard::Selection
        : QClipboard::Clipboard;

    if (write.contents.isEmpty()) {
        clipboard->clear(mode);
        return true;
    }

    auto mimeData = std::make_unique<QMimeData>();
    for (const TerminalClipboardMimeRepresentation &content : write.contents) {
        mimeData->setData(QString::fromLatin1(content.mime), content.data);
    }
    clipboard->setMimeData(mimeData.release(), mode);
    return true;
}

TerminalClipboardWriteTargets
writeTerminalClipboard(QClipboard *clipboard, const QString &text,
                       TerminalClipboardDestination destination)
{
    assertGuiThread();
    if (clipboard == nullptr) {
        return {};
    }

    const TerminalClipboardWriteTargets targets = terminalClipboardWriteTargets(
        destination, clipboard->supportsSelection());
    if (targets.standard) {
        clipboard->setText(text, QClipboard::Clipboard);
    }
    if (targets.primary) {
        clipboard->setText(text, QClipboard::Selection);
    }
    return targets;
}

std::optional<QString> readTerminalClipboard(QClipboard *clipboard,
                                             TerminalClipboardSource source)
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
        copyOnSelect, clipboard != nullptr && clipboard->supportsSelection());
    return readTerminalClipboard(clipboard, source).value_or(QString{});
}
