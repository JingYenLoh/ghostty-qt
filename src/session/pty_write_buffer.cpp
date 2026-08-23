#include "session/pty_write_buffer.h"

#include <QtAssert>

bool PtyWriteBuffer::append(QByteArrayView bytes)
{
    if (bytes.isEmpty()) return false;

    const qsizetype previousCapacity = storage_.capacity();

    const quintptr storageBegin =
        reinterpret_cast<quintptr>(storage_.constData());
    const quintptr storageEnd =
        storageBegin + static_cast<quintptr>(storage_.size());
    const quintptr inputBegin = reinterpret_cast<quintptr>(bytes.data());
    if (inputBegin >= storageBegin && inputBegin < storageEnd) {
        // compactBeforeAppend() can move the live suffix. Preserve an
        // overlapping caller view before changing storage.
        const QByteArray stable(bytes.data(), bytes.size());
        compactBeforeAppend();
        storage_.append(stable);
        return storage_.capacity() > previousCapacity;
    }

    compactBeforeAppend();
    storage_.append(bytes);
    return storage_.capacity() > previousCapacity;
}

void PtyWriteBuffer::consume(qsizetype count) noexcept
{
    Q_ASSERT(count >= 0);
    Q_ASSERT(count <= size());
    if (count <= 0) return;

    consumed_ += count;
    if (consumed_ == storage_.size()) {
        clear();
    }
}

void PtyWriteBuffer::clear() noexcept
{
    storage_.clear();
    consumed_ = 0;
}

bool PtyWriteBuffer::isEmpty() const noexcept
{
    return consumed_ == storage_.size();
}

qsizetype PtyWriteBuffer::size() const noexcept
{
    return storage_.size() - consumed_;
}

QByteArrayView PtyWriteBuffer::bytes() const noexcept
{
    return QByteArrayView(storage_).sliced(consumed_);
}

void PtyWriteBuffer::compactBeforeAppend()
{
    const qsizetype remaining = size();
    if (consumed_ < kCompactionThreshold || consumed_ < remaining) return;

    storage_.remove(0, consumed_);
    consumed_ = 0;
}
