#include "pty_write_buffer.h"

#include <QTest>

#include <algorithm>

namespace {

QByteArray pendingBytes(const PtyWriteBuffer &buffer)
{
    return buffer.bytes().toByteArray();
}

} // namespace

class TestPtyWriteBuffer final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void consumesByAdvancingThePendingView();
    void preservesFifoOrderAcrossShortWritesAndAppends();
    void compactsALargeConsumedPrefixBeforeAppending();
    void safelyAppendsItsOwnLiveSuffix();
    void clearsFullyConsumedStorage();
};

void TestPtyWriteBuffer::consumesByAdvancingThePendingView()
{
    PtyWriteBuffer buffer;
    buffer.append(QByteArrayLiteral("abcdef"));

    const char *const originalData = buffer.bytes().data();
    buffer.consume(2);

    QCOMPARE(buffer.size(), 4);
    QCOMPARE(pendingBytes(buffer), QByteArrayLiteral("cdef"));
    QVERIFY(buffer.bytes().data() == originalData + 2);
}

void TestPtyWriteBuffer::preservesFifoOrderAcrossShortWritesAndAppends()
{
    PtyWriteBuffer buffer;
    QByteArray expected;

    for (int index = 0; index < 512; ++index) {
        const QByteArray chunk = QByteArray::number(index) + ',';
        buffer.append(chunk);
        expected.append(chunk);

        const qsizetype consumed =
            std::min<qsizetype>(index % 7, expected.size());
        buffer.consume(consumed);
        expected.remove(0, consumed);

        QCOMPARE(buffer.size(), expected.size());
        QCOMPARE(pendingBytes(buffer), expected);
    }
}

void TestPtyWriteBuffer::compactsALargeConsumedPrefixBeforeAppending()
{
    constexpr qsizetype consumedPrefixSize = 128 * 1024;
    const QByteArray consumedPrefix(consumedPrefixSize, 'x');
    const QByteArray liveSuffix = QByteArrayLiteral("live-suffix");
    const QByteArray appended = QByteArrayLiteral("-new-data");

    PtyWriteBuffer buffer;
    buffer.append(consumedPrefix);
    buffer.append(liveSuffix);
    buffer.consume(consumedPrefixSize);
    buffer.append(appended);

    QCOMPARE(buffer.size(), liveSuffix.size() + appended.size());
    QCOMPARE(pendingBytes(buffer), liveSuffix + appended);
}

void TestPtyWriteBuffer::safelyAppendsItsOwnLiveSuffix()
{
    PtyWriteBuffer buffer;
    buffer.append(QByteArray(96 * 1024, 'p'));
    buffer.append(QByteArray(32 * 1024, 's'));
    buffer.consume(96 * 1024);

    const QByteArray suffix = buffer.bytes().toByteArray();
    buffer.append(buffer.bytes());

    QCOMPARE(buffer.size(), suffix.size() * 2);
    QCOMPARE(buffer.bytes().toByteArray(), suffix + suffix);
}

void TestPtyWriteBuffer::clearsFullyConsumedStorage()
{
    PtyWriteBuffer buffer;
    buffer.append(QByteArrayLiteral("first"));
    buffer.consume(buffer.size());

    QVERIFY(buffer.isEmpty());
    QCOMPARE(buffer.size(), 0);
    QVERIFY(buffer.bytes().isEmpty());

    buffer.append(QByteArrayLiteral("second"));
    QCOMPARE(pendingBytes(buffer), QByteArrayLiteral("second"));

    buffer.clear();
    QVERIFY(buffer.isEmpty());
    QCOMPARE(buffer.size(), 0);
}

QTEST_GUILESS_MAIN(TestPtyWriteBuffer)

#include "test_pty_write_buffer.moc"
