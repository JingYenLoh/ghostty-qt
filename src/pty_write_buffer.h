#pragma once

#include <QByteArray>
#include <QByteArrayView>

// FIFO storage for non-blocking PTY writes. Consuming bytes advances a view
// instead of moving the remaining payload on every short write. A sufficiently
// large, dominant consumed prefix is reclaimed immediately before a later
// append, amortizing copies and preventing stale prefixes from growing without
// bound while writes continue.
class PtyWriteBuffer final {
public:
    // Returns true when appending grew the backing allocation. The payload is
    // always copied; callers use the result only for deterministic transport
    // instrumentation.
    bool append(QByteArrayView bytes);
    void consume(qsizetype count) noexcept;
    void clear() noexcept;

    [[nodiscard]] bool isEmpty() const noexcept;
    [[nodiscard]] qsizetype size() const noexcept;
    [[nodiscard]] QByteArrayView bytes() const noexcept;

private:
    static constexpr qsizetype kCompactionThreshold = 64 * 1024;

    void compactBeforeAppend();

    QByteArray storage_;
    qsizetype consumed_ = 0;
};
