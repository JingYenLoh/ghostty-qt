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
    void translatesCellStylesAndAppearanceMetadata();
    void preservesTerminalAppearanceOverrides();
    void encodesUsingTerminalModes();
};

void GhosttyVtAdapterTest::rendersTerminalValuesAndEffects()
{
    QByteArray ptyWrites;
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 16;
    options.geometry.rows = 3;
    options.appearance.foregroundColor = QColor(QStringLiteral("#cad3f5"));
    options.appearance.backgroundColor = QColor(QStringLiteral("#24273a"));
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
    QCOMPARE(frame.foreground, options.appearance.foregroundColor);
    QCOMPARE(frame.background, options.appearance.backgroundColor);
    QCOMPARE(frame.cursorColor, options.appearance.foregroundColor);
    QVERIFY(!frame.cursorColorExplicit);
    QCOMPARE(frame.palette.size(), 256);
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
    TerminalAppearance reloadedAppearance = options.appearance;
    reloadedAppearance.foregroundColor = reloadedForeground;
    reloadedAppearance.backgroundColor = reloadedBackground;
    reloadedAppearance.cursorColor =
        TerminalColorValue::fromColor(reloadedCursor);
    QVERIFY(adapter->setAppearance(reloadedAppearance));
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(snapshot.update.colorsChanged);
    QVERIFY(applyTerminalUpdate(&frame, snapshot.update));
    QCOMPARE(frame.foreground, reloadedForeground);
    QCOMPARE(frame.background, reloadedBackground);
    QCOMPARE(frame.cursorColor, reloadedCursor);
    QVERIFY(frame.cursorColorExplicit);

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

void GhosttyVtAdapterTest::translatesCellStylesAndAppearanceMetadata()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 16;
    options.geometry.rows = 2;
    options.appearance.palette.resize(256);
    for (int index = 0; index < options.appearance.palette.size(); ++index) {
        options.appearance.palette[index] = QColor::fromRgb(index, index, index);
    }
    options.appearance.palette[1] = QColor(QStringLiteral("#aa1122"));
    options.appearance.cursorColor = TerminalColorValue::fromColor(
        QColor(QStringLiteral("#123456")));

    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);
    adapter->writeVt(QByteArrayLiteral(
        "\033[31;1mP"
        "\033[0;38;2;12;34;56;2mR"
        "\033[0;3;5mD"
        "\033[0;7mI"
        "\033[0;8mX"
        "\033[0;4;58;2;1;2;3mS"
        "\033[0;4:2m2"
        "\033[0;4:3mC"
        "\033[0;4:4mO"
        "\033[0;4:5mH"
        "\033[0;9;53mK"));

    GhosttyVtAdapter::RenderSnapshot snapshot;
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(snapshot.update.fullFrame);
    QVERIFY(snapshot.update.colorsChanged);
    QCOMPARE(snapshot.update.palette.size(), 256);
    QCOMPARE(snapshot.update.palette.at(1), QColor(QStringLiteral("#aa1122")));
    QCOMPARE(snapshot.update.cursorColor, QColor(QStringLiteral("#123456")));
    QVERIFY(snapshot.update.cursorColorExplicit);

    const TerminalFrame frame = applyUpdate(snapshot.update);
    const TerminalCell &paletteBold = frame.cells.at(0);
    QCOMPARE(paletteBold.text, QStringLiteral("P"));
    QCOMPARE(paletteBold.styleForegroundSource, TerminalColorSource::Palette);
    QCOMPARE(paletteBold.styleForegroundPaletteIndex, 1);
    QVERIFY(paletteBold.bold);

    const TerminalCell &rgbFaint = frame.cells.at(1);
    QCOMPARE(rgbFaint.text, QStringLiteral("R"));
    QCOMPARE(rgbFaint.styleForegroundSource, TerminalColorSource::Rgb);
    QCOMPARE(rgbFaint.styleForegroundPaletteIndex, -1);
    QVERIFY(rgbFaint.faint);

    const TerminalCell &defaultEffects = frame.cells.at(2);
    QCOMPARE(defaultEffects.text, QStringLiteral("D"));
    QCOMPARE(defaultEffects.styleForegroundSource, TerminalColorSource::Default);
    QVERIFY(defaultEffects.italic);
    QVERIFY(defaultEffects.textBlink);

    const TerminalCell &inverse = frame.cells.at(3);
    QCOMPARE(inverse.text, QStringLiteral("I"));
    QVERIFY(inverse.inverse);
    QCOMPARE(inverse.foreground, frame.background);
    QCOMPARE(inverse.background, frame.foreground);

    const TerminalCell &invisible = frame.cells.at(4);
    QVERIFY(invisible.text.isEmpty());
    QVERIFY(invisible.invisible);

    const TerminalCell &single = frame.cells.at(5);
    QCOMPARE(single.underlineStyle, TerminalUnderlineStyle::Single);
    QVERIFY(!single.underlineUsesForeground);
    QCOMPARE(single.underlineColor, QColor::fromRgb(1, 2, 3));
    QCOMPARE(frame.cells.at(6).underlineStyle, TerminalUnderlineStyle::Double);
    QCOMPARE(frame.cells.at(7).underlineStyle, TerminalUnderlineStyle::Curly);
    QCOMPARE(frame.cells.at(8).underlineStyle, TerminalUnderlineStyle::Dotted);
    QCOMPARE(frame.cells.at(9).underlineStyle, TerminalUnderlineStyle::Dashed);

    const TerminalCell &decorations = frame.cells.at(10);
    QVERIFY(decorations.strikeThrough);
    QVERIFY(decorations.overline);
}

void GhosttyVtAdapterTest::preservesTerminalAppearanceOverrides()
{
    GhosttyVtAdapter::Options options;
    options.appearance.palette.resize(256);
    for (int index = 0; index < options.appearance.palette.size(); ++index) {
        options.appearance.palette[index] = QColor::fromRgb(index, index, index);
    }
    options.appearance.palette[1] = QColor(QStringLiteral("#aa0000"));
    options.appearance.cursorColor = TerminalColorValue::fromColor(
        QColor(QStringLiteral("#123456")));
    options.appearance.cursorStyle = TerminalCursorStyle::Bar;
    options.appearance.cursorBlink = false;

    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);
    GhosttyVtAdapter::RenderSnapshot snapshot;
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    TerminalFrame frame = applyUpdate(snapshot.update);
    QCOMPARE(frame.palette.at(1), QColor(QStringLiteral("#aa0000")));
    QCOMPARE(frame.cursorColor, QColor(QStringLiteral("#123456")));
    QVERIFY(frame.cursorColorExplicit);
    QCOMPARE(frame.cursorStyle, 0);
    QVERIFY(!frame.cursorBlinking);

    // Terminal OSC overrides take precedence over embedder defaults.
    adapter->writeVt(QByteArrayLiteral(
        "\033]4;1;#00bb00\007"
        "\033]12;#abcdef\007"));
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(applyTerminalUpdate(&frame, snapshot.update));
    QCOMPARE(frame.palette.at(1), QColor(QStringLiteral("#00bb00")));
    QCOMPARE(frame.cursorColor, QColor(QStringLiteral("#abcdef")));

    TerminalAppearance reloaded = options.appearance;
    reloaded.palette[1] = QColor(QStringLiteral("#0000cc"));
    reloaded.cursorColor = TerminalColorValue::fromColor(
        QColor(QStringLiteral("#fedcba")));
    reloaded.cursorStyle = TerminalCursorStyle::Underline;
    reloaded.cursorBlink = true;
    QVERIFY(adapter->setAppearance(reloaded));
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(applyTerminalUpdate(&frame, snapshot.update));
    QCOMPARE(frame.palette.at(1), QColor(QStringLiteral("#00bb00")));
    QCOMPARE(frame.cursorColor, QColor(QStringLiteral("#abcdef")));
    QCOMPARE(frame.cursorStyle, 2);
    QVERIFY(frame.cursorBlinking);

    // Reset sequences reveal the newest configured defaults, not the defaults
    // that were active when the application override was installed.
    adapter->writeVt(QByteArrayLiteral("\033]104;1\007\033]112\007"));
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(applyTerminalUpdate(&frame, snapshot.update));
    QCOMPARE(frame.palette.at(1), QColor(QStringLiteral("#0000cc")));
    QCOMPARE(frame.cursorColor, QColor(QStringLiteral("#fedcba")));

    // An explicit DECSCUSR request remains active across config reloads; CSI
    // 0 q returns to the latest configured style and blink state.
    adapter->writeVt(QByteArrayLiteral("\033[2 q"));
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(applyTerminalUpdate(&frame, snapshot.update));
    QCOMPARE(frame.cursorStyle, 1);
    QVERIFY(!frame.cursorBlinking);
    reloaded.cursorStyle = TerminalCursorStyle::Bar;
    QVERIFY(adapter->setAppearance(reloaded));
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(applyTerminalUpdate(&frame, snapshot.update));
    QCOMPARE(frame.cursorStyle, 1);
    adapter->writeVt(QByteArrayLiteral("\033[0 q"));
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(applyTerminalUpdate(&frame, snapshot.update));
    QCOMPARE(frame.cursorStyle, 0);
    QVERIFY(frame.cursorBlinking);
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
