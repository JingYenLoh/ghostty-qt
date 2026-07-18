#include "terminal_clipboard.h"
#include "terminal_types.h"

#include <QTest>

#include <initializer_list>
#include <limits>

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
    void rejectsUnrepresentableFrameSize();
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

    QVERIFY(applyTerminalUpdate(&frame, full));
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

    QVERIFY(applyTerminalUpdate(&frame, partial));
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

    QVERIFY(!applyTerminalUpdate(&frame, malformed));
    QCOMPARE(frame.columns, 2);
    QCOMPARE(frame.rows, 1);
    QCOMPARE(frame.cells.size(), 2);
    QCOMPARE(frame.cells.at(0).text, QStringLiteral("a"));
    QCOMPARE(frame.cells.at(1).text, QStringLiteral("b"));
    QCOMPARE(frame.contentRevision, quint64{7});
}

void TerminalTypesTest::rejectsUnrepresentableFrameSize()
{
    TerminalFrame frame;
    TerminalUpdate update;
    update.columns = std::numeric_limits<int>::max();
    update.rows = std::numeric_limits<int>::max();
    update.fullFrame = true;

    QVERIFY(!applyTerminalUpdate(&frame, update));
    QCOMPARE(frame.columns, 0);
    QCOMPARE(frame.rows, 0);
    QVERIFY(frame.cells.isEmpty());
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
