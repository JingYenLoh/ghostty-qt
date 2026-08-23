#include "terminal/core/terminal_clipboard.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QMimeData>
#include <QStringView>
#include <QThread>

#include <memory>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>

namespace {

void assertGuiThread()
{
    Q_ASSERT(QCoreApplication::instance() != nullptr);
    Q_ASSERT(QThread::currentThread()
             == QCoreApplication::instance()->thread());
}

} // namespace

QString applyTerminalClipboardCodepointMap(
    QString text, std::span<const TerminalClipboardCodepointMapEntry> mappings)
{
    if (mappings.empty() || text.isEmpty()) {
        return text;
    }

    const QStringView source(text);
    std::optional<QString> result;
    const auto reversedMappings = mappings | std::views::reverse;
    qsizetype unchangedStart = 0;
    for (qsizetype offset = 0; offset < text.size();) {
        const QChar first = source.at(offset);
        qsizetype width = 1;
        quint32 codepoint = first.unicode();
        if (first.isHighSurrogate() && offset + 1 < text.size()) {
            const QChar second = source.at(offset + 1);
            if (second.isLowSurrogate()) {
                codepoint =
                    static_cast<quint32>(QChar::surrogateToUcs4(first, second));
                width = 2;
            }
        }

        const auto mapping = std::ranges::find_if(
            reversedMappings, [codepoint](const auto &entry) {
                return entry.first <= codepoint && codepoint <= entry.last;
            });
        if (mapping == reversedMappings.end()) {
            offset += width;
            continue;
        }

        if (!result.has_value()) {
            // Keep an all-deleted result distinct from a null QString, which
            // callers use to identify formatter failure.
            result.emplace(QStringLiteral(""));
            result->reserve(text.size());
        }
        result->append(source.sliced(unchangedStart, offset - unchangedStart));
        std::visit(
            [&result](const auto &replacement) {
                using Replacement = std::remove_cvref_t<decltype(replacement)>;
                if constexpr (std::is_same_v<Replacement, quint32>) {
                    constexpr quint32 MaximumUnicodeScalar = 0x10ffffU;
                    const char32_t replacementCodepoint =
                        static_cast<char32_t>(replacement);
                    if (replacement > MaximumUnicodeScalar
                        || QChar::isSurrogate(replacementCodepoint)) {
                        result->append(QChar::ReplacementCharacter);
                    } else {
                        result->append(QChar::fromUcs4(replacementCodepoint));
                    }
                } else {
                    result->append(replacement);
                }
            },
            mapping->replacement);
        offset += width;
        unchangedStart = offset;
    }
    if (!result.has_value()) {
        return text;
    }
    result->append(source.sliced(unchangedStart));
    return std::move(*result);
}

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
