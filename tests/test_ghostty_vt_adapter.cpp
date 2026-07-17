#include "ghostty_vt_adapter.h"

#ifdef GHOSTTY_VT_H
#error "ghostty_vt_adapter.h must not expose the libghostty-vt C API"
#endif

#include <QTest>

#include <linux/input-event-codes.h>

#include <optional>

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

void renderInto(GhosttyVtAdapter *adapter, TerminalFrame *frame)
{
    GhosttyVtAdapter::RenderSnapshot snapshot;
    const auto result = adapter->renderFrame(&snapshot);
    QVERIFY(result == GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(applyTerminalUpdate(frame, snapshot.update));
}

QString frameRowText(const TerminalFrame &frame, int row)
{
    QString result;
    const int offset = row * frame.columns;
    for (int column = 0; column < frame.columns; ++column) {
        result.append(frame.cells.at(offset + column).text);
    }
    return result.trimmed();
}

} // namespace

class GhosttyVtAdapterTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void rendersTerminalValuesAndEffects();
    void translatesCellStylesAndAppearanceMetadata();
    void preservesTerminalAppearanceOverrides();
    void encodesUsingTerminalModes();
    void selectsAndNavigatesViewportAtomically();
    void adjustsSelectionAndScrollsLogicalEndpointIntoView();
    void mapsEverySelectionAdjustment();
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

void GhosttyVtAdapterTest::selectsAndNavigatesViewportAtomically()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 8;
    options.geometry.rows = 3;
    auto blank = GhosttyVtAdapter::create(options);
    QVERIFY(blank != nullptr);
    QVERIFY(!blank->selectAll());
    QVERIFY(!blank->hasSelection());

    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);

    adapter->writeVt(QByteArrayLiteral(
        "row-0\r\nrow-1\r\nrow-2\r\nrow-3\r\n"
        "row-4\r\nrow-5\r\nrow-6\r\nrow-7"));
    TerminalFrame frame;
    renderInto(adapter.get(), &frame);
    QCOMPARE(frame.scrollLength, quint64{3});
    QVERIFY(frame.scrollOffset > 0);
    QCOMPARE(frameRowText(frame, 2), QStringLiteral("row-7"));

    QVERIFY(adapter->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Top,
    }));
    renderInto(adapter.get(), &frame);
    QCOMPARE(frame.scrollOffset, quint64{0});
    QCOMPARE(frameRowText(frame, 0), QStringLiteral("row-0"));

    QVERIFY(adapter->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Row,
        .row = 2,
    }));
    renderInto(adapter.get(), &frame);
    QCOMPARE(frame.scrollOffset, quint64{2});
    QCOMPARE(frameRowText(frame, 0), QStringLiteral("row-2"));

    QVERIFY(adapter->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Delta,
        .delta = -1,
    }));
    renderInto(adapter.get(), &frame);
    QCOMPARE(frame.scrollOffset, quint64{1});
    QCOMPARE(frameRowText(frame, 0), QStringLiteral("row-1"));

    adapter->beginSelection(0, 0, 1, false);
    QVERIFY(adapter->updateSelection(5, 0, false));
    adapter->endSelection(5, 0);
    QCOMPARE(adapter->selectedText(), QStringLiteral("row-1"));

    QVERIFY(adapter->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Bottom,
    }));
    renderInto(adapter.get(), &frame);
    QVERIFY(frame.scrollOffset > 1);
    QVERIFY(adapter->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Selection,
    }));
    renderInto(adapter.get(), &frame);
    QCOMPARE(frame.scrollOffset, quint64{1});
    QCOMPARE(frameRowText(frame, 0), QStringLiteral("row-1"));

    QVERIFY(adapter->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Top,
    }));
    adapter->beginSelection(4, 2, 1, false);
    QVERIFY(adapter->updateSelection(1, 0, false));
    adapter->endSelection(1, 0);
    QVERIFY(adapter->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Bottom,
    }));
    QVERIFY(adapter->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Selection,
    }));
    renderInto(adapter.get(), &frame);
    QCOMPARE(frame.scrollOffset, quint64{0});
    QCOMPARE(frameRowText(frame, 0), QStringLiteral("row-0"));

    adapter->clearSelection();
    QVERIFY(!adapter->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Selection,
    }));
    QVERIFY(adapter->selectAll());
    QVERIFY(adapter->hasSelection());
    const QString all = adapter->selectedText();
    QVERIFY(all.startsWith(QStringLiteral("row-0")));
    QVERIFY(all.endsWith(QStringLiteral("row-7")));
}

void GhosttyVtAdapterTest::adjustsSelectionAndScrollsLogicalEndpointIntoView()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 8;
    options.geometry.rows = 3;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);
    adapter->writeVt(QByteArrayLiteral(
        "row-0\r\nrow-1\r\nrow-2\r\nrow-3\r\n"
        "row-4\r\nrow-5\r\nrow-6\r\nrow-7"));

    QVERIFY(!adapter->adjustSelection(TerminalSelectionAdjustment::Left));
    QVERIFY(adapter->selectAll());
    QVERIFY(adapter->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Top,
    }));
    QVERIFY(adapter->adjustSelection(TerminalSelectionAdjustment::Left));

    TerminalFrame frame;
    renderInto(adapter.get(), &frame);
    QCOMPARE(frame.scrollOffset, frame.scrollTotal - frame.scrollLength);
    QVERIFY(adapter->selectedText().endsWith(QStringLiteral("row-")));

    // A reversed selection keeps its logical end at the top. Adjusting that
    // endpoint while scrolled to the bottom must reveal the top, rather than
    // following the visually lower start endpoint.
    QVERIFY(adapter->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Top,
    }));
    adapter->beginSelection(4, 2, 1, false);
    QVERIFY(adapter->updateSelection(4, 0, false));
    adapter->endSelection(4, 0);
    QVERIFY(adapter->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Bottom,
    }));
    QVERIFY(adapter->adjustSelection(TerminalSelectionAdjustment::Left));
    renderInto(adapter.get(), &frame);
    QCOMPARE(frame.scrollOffset, quint64{0});

}

void GhosttyVtAdapterTest::mapsEverySelectionAdjustment()
{
    struct Case {
        TerminalSelectionAdjustment adjustment;
        const char *expected;
    };
    const Case cases[] = {
        {TerminalSelectionAdjustment::Left, "f"},
        {TerminalSelectionAdjustment::Right, "fgh"},
        {TerminalSelectionAdjustment::Up, "bcde\nf"},
        {TerminalSelectionAdjustment::Down, "fghij\nkl"},
        {TerminalSelectionAdjustment::PageUp, "abcde\nf"},
        {TerminalSelectionAdjustment::PageDown, "fghij\nklmno"},
        {TerminalSelectionAdjustment::Home, "abcde\nf"},
        {TerminalSelectionAdjustment::End, "fghij\nklmno"},
        {TerminalSelectionAdjustment::BeginningOfLine, "f"},
        {TerminalSelectionAdjustment::EndOfLine, "fghij"},
    };

    for (const Case &testCase : cases) {
        GhosttyVtAdapter::Options options;
        options.geometry.columns = 7;
        options.geometry.rows = 3;
        auto adapter = GhosttyVtAdapter::create(options);
        QVERIFY(adapter != nullptr);
        adapter->writeVt(QByteArrayLiteral("abcde\r\nfghij\r\nklmno"));

        adapter->beginSelection(0, 1, 1, false);
        QVERIFY(adapter->updateSelection(2, 1, false));
        adapter->endSelection(2, 1);
        QCOMPARE(adapter->selectedText(), QStringLiteral("fg"));
        QVERIFY(adapter->adjustSelection(testCase.adjustment));
        QCOMPARE(adapter->selectedText(), QString::fromLatin1(testCase.expected));
    }

    // With scrollback on both sides of the endpoint, page movement must stay
    // distinct from document home/end. The compact fixture above reaches a
    // boundary for both pairs and therefore cannot catch swapped enum values.
    const auto adjustInHistory = [](TerminalSelectionAdjustment adjustment) {
        GhosttyVtAdapter::Options options;
        options.geometry.columns = 8;
        options.geometry.rows = 2;
        auto adapter = GhosttyVtAdapter::create(options);
        if (adapter == nullptr) return std::optional<QString>{};
        adapter->writeVt(QByteArrayLiteral(
            "row-0\r\nrow-1\r\nrow-2\r\nrow-3\r\nrow-4\r\n"
            "row-5\r\nrow-6\r\nrow-7\r\nrow-8\r\nrow-9"));
        TerminalFrame frame;
        renderInto(adapter.get(), &frame);
        if (!adapter->scrollViewport({
                .kind = TerminalViewportRequest::Kind::Top,
            })) {
            return std::optional<QString>{};
        }
        renderInto(adapter.get(), &frame);
        if (!adapter->scrollViewport({
                .kind = TerminalViewportRequest::Kind::Row,
                .row = 3,
            })) {
            return std::optional<QString>{};
        }
        renderInto(adapter.get(), &frame);
        adapter->beginSelection(0, 0, 1, false);
        if (!adapter->updateSelection(2, 0, false)) {
            return std::optional<QString>{};
        }
        adapter->endSelection(2, 0);
        if (!adapter->adjustSelection(adjustment)) {
            return std::optional<QString>{};
        }
        return std::optional<QString>{adapter->selectedText()};
    };

    const std::optional<QString> pageUp =
        adjustInHistory(TerminalSelectionAdjustment::PageUp);
    const std::optional<QString> home =
        adjustInHistory(TerminalSelectionAdjustment::Home);
    const std::optional<QString> pageDown =
        adjustInHistory(TerminalSelectionAdjustment::PageDown);
    const std::optional<QString> end =
        adjustInHistory(TerminalSelectionAdjustment::End);
    QVERIFY(pageUp.has_value());
    QVERIFY(home.has_value());
    QVERIFY(pageDown.has_value());
    QVERIFY(end.has_value());
    QCOMPARE(*pageUp, QStringLiteral("ow-1\nrow-2\nr"));
    QCOMPARE(*home, QStringLiteral("row-0\nrow-1\nrow-2\nr"));
    QCOMPARE(*pageDown, QStringLiteral("row-3\nrow-4\nro"));
    QCOMPARE(*end, QStringLiteral(
        "row-3\nrow-4\nrow-5\nrow-6\nrow-7\nrow-8\nrow-9"));
}

QTEST_GUILESS_MAIN(GhosttyVtAdapterTest)

#include "test_ghostty_vt_adapter.moc"
