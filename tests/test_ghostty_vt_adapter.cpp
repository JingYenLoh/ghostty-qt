#include "ghostty_vt_adapter.h"

#ifdef GHOSTTY_VT_H
#error "ghostty_vt_adapter.h must not expose the libghostty-vt C API"
#endif

#include <QTest>

#include <linux/input-event-codes.h>

namespace {

QString frameText(const TerminalFrame &frame)
{
    QString result;
    for (const TerminalCell &cell : frame.cells) {
        result.append(cell.text);
    }
    return result;
}

TerminalFrame applyUpdate(const TerminalUpdate &update)
{
    TerminalFrame frame;
    const bool applied = applyTerminalUpdate(&frame, update);
    Q_ASSERT(applied);
    return frame;
}

} // namespace

class GhosttyVtAdapterTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void rendersTerminalValuesAndEffects();
    void encodesUsingTerminalModes();
};

void GhosttyVtAdapterTest::rendersTerminalValuesAndEffects()
{
    QByteArray ptyWrites;
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 16;
    options.geometry.rows = 3;
    options.foregroundColor = QColor(QStringLiteral("#cad3f5"));
    options.backgroundColor = QColor(QStringLiteral("#24273a"));
    auto adapter = GhosttyVtAdapter::create(
        options, {.writePty = [&ptyWrites](const QByteArray &data) { ptyWrites += data; }});
    QVERIFY(adapter != nullptr);

    adapter->writeVt(
        QByteArrayLiteral("\033]2;adapter-title\007"
                          "\033]7;file:///tmp\007"
                          "\007A\033[31mB\033[c"));
    const GhosttyVtAdapter::DeferredEffects effects = adapter->takeDeferredEffects();
    QCOMPARE(effects.title, QStringLiteral("adapter-title"));
    QCOMPARE(effects.currentDirectory, QStringLiteral("/tmp"));
    QVERIFY(effects.bell);
    QCOMPARE(ptyWrites, QByteArrayLiteral("\033[?62;22c"));

    GhosttyVtAdapter::RenderSnapshot snapshot;
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(snapshot.update.fullFrame);
    QCOMPARE(snapshot.update.dirtyRows.size(), 3);
    TerminalFrame frame = applyUpdate(snapshot.update);
    QCOMPARE(frame.columns, 16);
    QCOMPARE(frame.rows, 3);
    QCOMPARE(frame.foreground, options.foregroundColor);
    QCOMPARE(frame.background, options.backgroundColor);
    QCOMPARE(frame.cursorColor, options.foregroundColor);
    QVERIFY(frameText(frame).contains(QStringLiteral("AB")));
    const QColor redCell = frame.cells.at(1).foreground;
    QVERIFY(redCell != frame.foreground);
    QVERIFY(redCell.red() > redCell.green());
    QVERIFY(redCell.red() > redCell.blue());

    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(!snapshot.update.hasChanges());

    const QColor reloadedForeground(QStringLiteral("#f4dbd6"));
    const QColor reloadedBackground(QStringLiteral("#1e2030"));
    const QColor reloadedCursor(QStringLiteral("#f5bde6"));
    QVERIFY(adapter->setColors(reloadedForeground, reloadedBackground,
                               reloadedCursor));
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(snapshot.update.colorsChanged);
    QVERIFY(applyTerminalUpdate(&frame, snapshot.update));
    QCOMPARE(frame.foreground, reloadedForeground);
    QCOMPARE(frame.background, reloadedBackground);
    QCOMPARE(frame.cursorColor, reloadedCursor);

    adapter->writeVt(QByteArrayLiteral("\rZ"));
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(!snapshot.update.fullFrame);
    QCOMPARE(snapshot.update.dirtyRows.size(), 1);
    QVERIFY(snapshot.update.cursorChanged);
    QVERIFY(applyTerminalUpdate(&frame, snapshot.update));
    QVERIFY(frameText(frame).startsWith(QStringLiteral("ZB")));

    GhosttyVtAdapter::Geometry resized = options.geometry;
    resized.columns = 10;
    resized.rows = 4;
    QVERIFY(adapter->resize(resized));
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(snapshot.update.fullFrame);
    QCOMPARE(snapshot.update.dirtyRows.size(), 4);
    QVERIFY(applyTerminalUpdate(&frame, snapshot.update));
    QCOMPARE(frame.columns, 10);
    QCOMPARE(frame.rows, 4);
}

void GhosttyVtAdapterTest::encodesUsingTerminalModes()
{
    auto adapter = GhosttyVtAdapter::create({});
    QVERIFY(adapter != nullptr);

    adapter->writeVt(QByteArrayLiteral("\033[?2004h\033[?1004h"));
    adapter->synchronizeInputModes();
    QCOMPARE(adapter->encodePaste(QStringLiteral("one\ntwo")),
             QByteArrayLiteral("\033[200~one\ntwo\033[201~"));
    QCOMPARE(adapter->encodeFocus(true), QByteArrayLiteral("\033[I"));
    QCOMPARE(adapter->encodeFocus(false), QByteArrayLiteral("\033[O"));

    TerminalKeyInput input;
    input.key = Qt::Key_A;
    input.text = QStringLiteral("a");
    QCOMPARE(adapter->encodeKey(input), QByteArrayLiteral("a"));

    // Physical location comes from Qt's Linux XKB scan code, even when the
    // logical key/modifier tuple does not identify a keypad key. Kitty's
    // disambiguation mode makes the physical identity observable without
    // relying on synthetic text that Qt would normally supply.
    adapter->writeVt(QByteArrayLiteral("\033[>1u"));
    TerminalKeyInput keypad;
    keypad.key = Qt::Key_Left;
    keypad.nativeScanCode = KEY_KP1 + 8U;
    QCOMPARE(adapter->encodeKey(keypad), QByteArrayLiteral("\033[57400u"));
}

QTEST_GUILESS_MAIN(GhosttyVtAdapterTest)

#include "test_ghostty_vt_adapter.moc"
