#include "terminal_clipboard.h"
#include "terminal_types.h"

#include <QTest>

#include <initializer_list>
#include <limits>
#include <utility>

namespace {

TerminalCell textCell(QStringView text)
{
    TerminalCell cell;
    cell.text = text.toString();
    return cell;
}

TerminalRowUpdate textRow(int row, std::initializer_list<QStringView> cells)
{
    TerminalRowUpdate update;
    update.row = row;
    for (QStringView text : cells) {
        update.cells.append(textCell(text));
    }
    return update;
}

} // namespace

class TerminalTypesTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void appliesFullAndPartialUpdates();
    void rejectsMalformedUpdateWithoutMutation();
    void validatesStrictDirtyRowOrder();
    void preservesCopyOnWriteAcrossIndependentDeltas();
    void rejectsUnrepresentableFrameSize();
    void ordersSearchCellsByRowThenColumn();
    void resolvesClipboardRoutingWithoutPlatformAssumptions();
};

void TerminalTypesTest::appliesFullAndPartialUpdates()
{
    TerminalFrame frame;
    TerminalUpdate full;
    full.columns = 2;
    full.rows = 2;
    full.fullFrame = true;
    full.dirtyRows = {
        textRow(0, {u"a", u"b"}),
        textRow(1, {u"c", u"d"}),
    };
    full.contentRevision = 1;

    QVERIFY(applyTerminalUpdate(frame, full));
    QCOMPARE(frame.columns, 2);
    QCOMPARE(frame.rows, 2);
    QCOMPARE(frame.cells.size(), 4);
    QCOMPARE(frame.cells.at(0).text, QStringLiteral("a"));
    QCOMPARE(frame.cells.at(3).text, QStringLiteral("d"));
    QCOMPARE(frame.contentRevision, quint64{1});

    TerminalUpdate partial;
    partial.columns = 2;
    partial.rows = 2;
    partial.dirtyRows = {textRow(1, {u"x", u"y"})};
    partial.contentRevision = 2;

    QVERIFY(applyTerminalUpdate(frame, partial));
    QCOMPARE(frame.cells.at(0).text, QStringLiteral("a"));
    QCOMPARE(frame.cells.at(1).text, QStringLiteral("b"));
    QCOMPARE(frame.cells.at(2).text, QStringLiteral("x"));
    QCOMPARE(frame.cells.at(3).text, QStringLiteral("y"));
    QCOMPARE(frame.contentRevision, quint64{2});
}

void TerminalTypesTest::rejectsMalformedUpdateWithoutMutation()
{
    TerminalFrame frame;
    frame.columns = 2;
    frame.rows = 1;
    frame.cells = {textCell(u"a"), textCell(u"b")};
    frame.contentRevision = 7;

    TerminalUpdate malformed;
    malformed.columns = 2;
    malformed.rows = 1;
    malformed.dirtyRows = {textRow(0, {u"only-one-cell"})};
    malformed.contentRevision = 8;

    QVERIFY(!applyTerminalUpdate(frame, malformed));
    QCOMPARE(frame.columns, 2);
    QCOMPARE(frame.rows, 1);
    QCOMPARE(frame.cells.size(), 2);
    QCOMPARE(frame.cells.at(0).text, QStringLiteral("a"));
    QCOMPARE(frame.cells.at(1).text, QStringLiteral("b"));
    QCOMPARE(frame.contentRevision, quint64{7});
}

void TerminalTypesTest::validatesStrictDirtyRowOrder()
{
    TerminalFrame baseline;
    baseline.columns = 2;
    baseline.rows = 3;
    baseline.cells = {
        textCell(u"a"), textCell(u"b"),
        textCell(u"c"), textCell(u"d"),
        textCell(u"e"), textCell(u"f"),
    };
    baseline.palette = {Qt::red, Qt::green};
    baseline.cursorColumn = 1;
    baseline.scrollOffset = 4;
    baseline.contentRevision = 7;

    const auto verifyRejected = [&baseline](
                                    QVector<TerminalRowUpdate> dirtyRows,
                                    bool fullFrame = false) {
        TerminalFrame frame = baseline;
        const TerminalCell *const cells = frame.cells.constData();
        const QColor *const palette = frame.palette.constData();
        TerminalUpdate update;
        update.columns = baseline.columns;
        update.rows = baseline.rows;
        update.fullFrame = fullFrame;
        update.dirtyRows = std::move(dirtyRows);
        update.contentRevision = 8;

        QVERIFY(!applyTerminalUpdate(frame, update));
        QCOMPARE(frame.cells.constData(), cells);
        QCOMPARE(frame.palette.constData(), palette);
        QCOMPARE(frame.columns, baseline.columns);
        QCOMPARE(frame.rows, baseline.rows);
        QCOMPARE(frame.cursorColumn, baseline.cursorColumn);
        QCOMPARE(frame.scrollOffset, baseline.scrollOffset);
        QCOMPARE(frame.contentRevision, baseline.contentRevision);
    };

    verifyRejected({textRow(0, {u"x", u"y"}),
                    textRow(0, {u"z", u"w"})});
    verifyRejected({textRow(2, {u"x", u"y"}),
                    textRow(0, {u"z", u"w"})});
    verifyRejected({textRow(-1, {u"x", u"y"})});
    verifyRejected({textRow(3, {u"x", u"y"})});
    verifyRejected({textRow(1, {u"wrong-width"})});
    verifyRejected({textRow(0, {u"x", u"y"}),
                    textRow(1, {u"z", u"w"})},
                   true);

    TerminalFrame sparse = baseline;
    TerminalUpdate ascending;
    ascending.columns = baseline.columns;
    ascending.rows = baseline.rows;
    ascending.dirtyRows = {
        textRow(0, {u"x", u"y"}),
        textRow(2, {u"z", u"w"}),
    };
    ascending.contentRevision = 8;
    QVERIFY(applyTerminalUpdate(sparse, ascending));
    QCOMPARE(sparse.cells.at(0).text, QStringLiteral("x"));
    QCOMPARE(sparse.cells.at(2).text, QStringLiteral("c"));
    QCOMPARE(sparse.cells.at(4).text, QStringLiteral("z"));
    QCOMPARE(sparse.contentRevision, quint64{8});
}

void TerminalTypesTest::preservesCopyOnWriteAcrossIndependentDeltas()
{
    TerminalUpdate full;
    full.columns = 2;
    full.rows = 1;
    full.fullFrame = true;
    full.dirtyRows = {textRow(0, {u"a", u"b"})};
    full.colorsChanged = true;
    full.foreground = Qt::white;
    full.background = Qt::black;
    full.cursorColor = Qt::yellow;
    full.palette.resize(256);
    for (int index = 0; index < full.palette.size(); ++index) {
        full.palette[index] = QColor::fromRgb(index, 255 - index, index / 2);
    }
    full.cursorColorExplicit = true;
    full.cursorColumn = 1;
    full.scrollOffset = 3;
    full.contentRevision = 1;

    TerminalFrame frame;
    QVERIFY(applyTerminalUpdate(frame, full));
    const QColor *const paletteStorage = frame.palette.constData();
    const TerminalFrame renderSnapshot = frame;

    TerminalUpdate rowOnly;
    rowOnly.columns = 2;
    rowOnly.rows = 1;
    rowOnly.dirtyRows = {textRow(0, {u"x", u"y"})};
    rowOnly.contentRevision = 2;
    QVERIFY(rowOnly.palette.isEmpty());
    QVERIFY(applyTerminalUpdate(frame, rowOnly));
    QCOMPARE(frame.palette.constData(), paletteStorage);
    QCOMPARE(renderSnapshot.palette.constData(), paletteStorage);
    QCOMPARE(frame.cells.at(0).text, QStringLiteral("x"));
    QCOMPARE(renderSnapshot.cells.at(0).text, QStringLiteral("a"));

    const TerminalCell *const cellStorage = frame.cells.constData();
    TerminalUpdate colorsOnly;
    colorsOnly.columns = 2;
    colorsOnly.rows = 1;
    colorsOnly.colorsChanged = true;
    colorsOnly.foreground = Qt::cyan;
    colorsOnly.background = Qt::darkBlue;
    colorsOnly.cursorColor = Qt::magenta;
    colorsOnly.palette.fill(Qt::darkGreen, 256);
    colorsOnly.contentRevision = 3;
    QVERIFY(applyTerminalUpdate(frame, colorsOnly));
    QCOMPARE(frame.cells.constData(), cellStorage);
    QCOMPARE(frame.cells.at(0).text, QStringLiteral("x"));
    QCOMPARE(frame.foreground, QColor(Qt::cyan));
    QCOMPARE(frame.palette.size(), 256);
    QCOMPARE(frame.palette.at(42), QColor(Qt::darkGreen));
    QCOMPARE(frame.cursorColumn, 1);
    QCOMPARE(frame.scrollOffset, quint64{3});
    QCOMPARE(frame.contentRevision, quint64{3});
}

void TerminalTypesTest::rejectsUnrepresentableFrameSize()
{
    TerminalFrame frame;
    TerminalUpdate update;
    update.columns = std::numeric_limits<int>::max();
    update.rows = std::numeric_limits<int>::max();
    update.fullFrame = true;

    QVERIFY(!applyTerminalUpdate(frame, update));
    QCOMPARE(frame.columns, 0);
    QCOMPARE(frame.rows, 0);
    QVERIFY(frame.cells.isEmpty());
}

void TerminalTypesTest::ordersSearchCellsByRowThenColumn()
{
    const TerminalSearchCell first{.column = 2, .screenRow = 3};
    const TerminalSearchCell laterColumn{.column = 7, .screenRow = 3};
    const TerminalSearchCell laterRow{.column = 0, .screenRow = 4};

    QVERIFY(first < laterColumn);
    QVERIFY(laterColumn < laterRow);
    QVERIFY(laterRow > first);
    QCOMPARE(first, (TerminalSearchCell{.column = 2, .screenRow = 3}));
}

void TerminalTypesTest::resolvesClipboardRoutingWithoutPlatformAssumptions()
{
    const auto verifyTargets = [](TerminalClipboardDestination destination,
                                  bool supportsPrimary, bool standard,
                                  bool primary) {
        const TerminalClipboardWriteTargets targets =
            terminalClipboardWriteTargets(destination, supportsPrimary);
        QCOMPARE(targets.standard, standard);
        QCOMPARE(targets.primary, primary);
    };

    verifyTargets(TerminalClipboardDestination::Standard, false, true, false);
    verifyTargets(TerminalClipboardDestination::Standard, true, true, false);
    verifyTargets(TerminalClipboardDestination::Primary, false, true, false);
    verifyTargets(TerminalClipboardDestination::Primary, true, false, true);
    verifyTargets(TerminalClipboardDestination::PrimaryAndStandard,
                  false, true, false);
    verifyTargets(TerminalClipboardDestination::PrimaryAndStandard,
                  true, true, true);

    for (const TerminalCopyOnSelectMode mode : {
             TerminalCopyOnSelectMode::Disabled,
             TerminalCopyOnSelectMode::Primary,
         }) {
        QCOMPARE(terminalMiddleClickSource(mode, false),
                 TerminalClipboardSource::Standard);
        QCOMPARE(terminalMiddleClickSource(mode, true),
                 TerminalClipboardSource::Primary);
    }
    QCOMPARE(terminalMiddleClickSource(
                 TerminalCopyOnSelectMode::PrimaryAndClipboard, false),
             TerminalClipboardSource::Standard);
    QCOMPARE(terminalMiddleClickSource(
                 TerminalCopyOnSelectMode::PrimaryAndClipboard, true),
             TerminalClipboardSource::Standard);
}

QTEST_APPLESS_MAIN(TerminalTypesTest)

#include "test_terminal_types.moc"
