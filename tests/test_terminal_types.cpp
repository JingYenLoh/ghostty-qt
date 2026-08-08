#include "terminal_clipboard.h"
#include "terminal_osc8_index.h"
#include "terminal_types.h"

#include <QTest>

#include <array>
#include <cstddef>
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

using TerminalCellFlags = std::array<bool, 16>;

TerminalCellFlags terminalCellFlags(const TerminalCell &cell)
{
    return {
        cell.plainCodepoint(),
        cell.extendedGrapheme(),
        cell.bold(),
        cell.italic(),
        cell.faint(),
        cell.textBlink(),
        cell.inverse(),
        cell.invisible(),
        cell.underlineUsesForeground(),
        cell.strikeThrough(),
        cell.overline(),
        cell.selected(),
        cell.backgroundExplicit(),
        cell.minimumContrastExemptGlyph(),
        cell.hasHyperlink(),
        cell.spacer(),
    };
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

TerminalRowUpdate hyperlinkRow(
    int row, int columns, std::initializer_list<int> linkedColumns)
{
    TerminalRowUpdate update;
    update.row = row;
    update.cells.resize(columns);
    for (int column : linkedColumns) {
        update.cells[column].setHasHyperlink(true);
    }
    return update;
}

} // namespace

class TerminalTypesTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void packsCellMetadataWithoutChangingSemantics();
    void appliesFullAndPartialUpdates();
    void rejectsMalformedUpdateWithoutMutation();
    void validatesStrictDirtyRowOrder();
    void preservesCopyOnWriteAcrossIndependentDeltas();
    void rejectsUnrepresentableFrameSize();
    void indexesOsc8CellsAcrossSparseUpdates();
    void rejectsMalformedOsc8UpdatesAtomically();
    void ordersSearchCellsByRowThenColumn();
    void resolvesClipboardRoutingWithoutPlatformAssumptions();
    void validatesTerminalOriginatedClipboardWrites();
    void mapsPlainClipboardCodepointsInOneOrderedPass();
};

void TerminalTypesTest::packsCellMetadataWithoutChangingSemantics()
{
    constexpr std::size_t oldTerminalCellSize = 112;
    QVERIFY(sizeof(TerminalCell) <= oldTerminalCellSize);
    QCOMPARE(sizeof(TerminalCellColor), sizeof(quint32));
#if defined(Q_PROCESSOR_X86_64)
    QCOMPARE(sizeof(TerminalCell), std::size_t{48});
#endif

    constexpr TerminalCellFlags expectedDefaults{
        false, false, false, false, false, false, false, false,
        true,  false, false, false, false, false, false, false,
    };
    const TerminalCell defaults;
    QVERIFY(terminalCellFlags(defaults) == expectedDefaults);
    QCOMPARE(defaults.styleForegroundSource(), TerminalColorSource::Default);
    QCOMPARE(defaults.styleForegroundPaletteIndex(), -1);
    QCOMPARE(defaults.underlineStyle(), TerminalUnderlineStyle::None);
    QCOMPARE(defaults.columnSpan(), 1);
    QVERIFY(!defaults.foreground.isValid());
    QVERIFY(!defaults.background.isValid());
    QVERIFY(!defaults.underlineColor.isValid());

    TerminalCell colored;
    colored.foreground = Qt::black;
    colored.background = QColor::fromRgb(1, 2, 3);
    colored.underlineColor = TerminalCellColor::fromRgb(254, 128, 64);
    QCOMPARE(colored.foreground, QColor(Qt::black));
    QCOMPARE(colored.background, QColor::fromRgb(1, 2, 3));
    QCOMPARE(colored.underlineColor, QColor::fromRgb(254, 128, 64));
    QVERIFY(QColor::fromRgb(1, 2, 3) == colored.background);
    QCOMPARE(colored.background.rgba(), qRgba(1, 2, 3, 255));
    QCOMPARE(colored.background.toQColor().alpha(), 255);
    const TerminalCell copied = colored;
    QCOMPARE(copied.foreground, colored.foreground);
    QCOMPARE(copied.background, colored.background);
    QCOMPARE(copied.underlineColor, colored.underlineColor);
    colored.background = QColor{};
    QVERIFY(!colored.background.isValid());

    using FlagSetter = void (TerminalCell::*)(bool) noexcept;
    constexpr std::array<FlagSetter, expectedDefaults.size()> flagSetters{
        &TerminalCell::setPlainCodepoint,
        &TerminalCell::setExtendedGrapheme,
        &TerminalCell::setBold,
        &TerminalCell::setItalic,
        &TerminalCell::setFaint,
        &TerminalCell::setTextBlink,
        &TerminalCell::setInverse,
        &TerminalCell::setInvisible,
        &TerminalCell::setUnderlineUsesForeground,
        &TerminalCell::setStrikeThrough,
        &TerminalCell::setOverline,
        &TerminalCell::setSelected,
        &TerminalCell::setBackgroundExplicit,
        &TerminalCell::setMinimumContrastExemptGlyph,
        &TerminalCell::setHasHyperlink,
        &TerminalCell::setSpacer,
    };
    for (std::size_t index = 0; index < flagSetters.size(); ++index) {
        TerminalCell cell;
        cell.setStyleForegroundSource(TerminalColorSource::Rgb);
        cell.setStyleForegroundPaletteIndex(255);
        cell.setUnderlineStyle(TerminalUnderlineStyle::Dashed);
        cell.setColumnSpan(2);

        TerminalCellFlags expected = expectedDefaults;
        expected[index] = !expected[index];
        (cell.*flagSetters[index])(expected[index]);
        QVERIFY(terminalCellFlags(cell) == expected);
        QCOMPARE(cell.styleForegroundSource(), TerminalColorSource::Rgb);
        QCOMPARE(cell.styleForegroundPaletteIndex(), 255);
        QCOMPARE(cell.underlineStyle(), TerminalUnderlineStyle::Dashed);
        QCOMPARE(cell.columnSpan(), 2);

        (cell.*flagSetters[index])(expectedDefaults[index]);
        QVERIFY(terminalCellFlags(cell) == expectedDefaults);
    }

    TerminalCell adjacentFields;
    adjacentFields.setSpacer(true);
    adjacentFields.setStyleForegroundPaletteIndex(255);
    adjacentFields.setUnderlineStyle(TerminalUnderlineStyle::Dashed);
    adjacentFields.setColumnSpan(2);
    constexpr std::array colorSources{
        TerminalColorSource::Default,
        TerminalColorSource::Palette,
        TerminalColorSource::Rgb,
    };
    for (const TerminalColorSource source : colorSources) {
        adjacentFields.setStyleForegroundSource(source);
        QCOMPARE(adjacentFields.styleForegroundSource(), source);
        QCOMPARE(adjacentFields.styleForegroundPaletteIndex(), 255);
        QCOMPARE(adjacentFields.underlineStyle(),
                 TerminalUnderlineStyle::Dashed);
        QCOMPARE(adjacentFields.columnSpan(), 2);
        QVERIFY(adjacentFields.spacer());
    }

    for (const int paletteIndex : {-1, 0, 255}) {
        adjacentFields.setStyleForegroundPaletteIndex(paletteIndex);
        QCOMPARE(adjacentFields.styleForegroundSource(),
                 TerminalColorSource::Rgb);
        QCOMPARE(adjacentFields.styleForegroundPaletteIndex(), paletteIndex);
        QCOMPARE(adjacentFields.underlineStyle(),
                 TerminalUnderlineStyle::Dashed);
        QCOMPARE(adjacentFields.columnSpan(), 2);
        QVERIFY(adjacentFields.spacer());
    }

    constexpr std::array underlineStyles{
        TerminalUnderlineStyle::None,   TerminalUnderlineStyle::Single,
        TerminalUnderlineStyle::Double, TerminalUnderlineStyle::Curly,
        TerminalUnderlineStyle::Dotted, TerminalUnderlineStyle::Dashed,
    };
    for (const TerminalUnderlineStyle style : underlineStyles) {
        adjacentFields.setUnderlineStyle(style);
        QCOMPARE(adjacentFields.styleForegroundSource(),
                 TerminalColorSource::Rgb);
        QCOMPARE(adjacentFields.styleForegroundPaletteIndex(), 255);
        QCOMPARE(adjacentFields.underlineStyle(), style);
        QCOMPARE(adjacentFields.columnSpan(), 2);
        QVERIFY(adjacentFields.spacer());
    }

    for (const int columnSpan : {1, 2}) {
        adjacentFields.setColumnSpan(columnSpan);
        QCOMPARE(adjacentFields.styleForegroundSource(),
                 TerminalColorSource::Rgb);
        QCOMPARE(adjacentFields.styleForegroundPaletteIndex(), 255);
        QCOMPARE(adjacentFields.underlineStyle(),
                 TerminalUnderlineStyle::Dashed);
        QCOMPARE(adjacentFields.columnSpan(), columnSpan);
        QVERIFY(adjacentFields.spacer());
    }

    TerminalCell copySource;
    for (std::size_t index = 0; index < flagSetters.size(); ++index) {
        (copySource.*flagSetters[index])(!expectedDefaults[index]);
    }
    copySource.setStyleForegroundSource(TerminalColorSource::Rgb);
    copySource.setStyleForegroundPaletteIndex(255);
    copySource.setUnderlineStyle(TerminalUnderlineStyle::Dashed);
    copySource.setColumnSpan(2);
    const TerminalCell copy = copySource;
    QVERIFY(terminalCellFlags(copy) == terminalCellFlags(copySource));
    QCOMPARE(copy.styleForegroundSource(), copySource.styleForegroundSource());
    QCOMPARE(copy.styleForegroundPaletteIndex(),
             copySource.styleForegroundPaletteIndex());
    QCOMPARE(copy.underlineStyle(), copySource.underlineStyle());
    QCOMPARE(copy.columnSpan(), copySource.columnSpan());
}

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

void TerminalTypesTest::indexesOsc8CellsAcrossSparseUpdates()
{
    TerminalOsc8Index index;
    QVERIFY(!index.hasFrame());
    QVERIFY(index.candidates().isEmpty());

    TerminalUpdate full;
    full.columns = 5;
    full.rows = 4;
    full.fullFrame = true;
    full.contentRevision = 7;
    full.dirtyRows = {
        hyperlinkRow(0, 5, {1, 4}),
        hyperlinkRow(1, 5, {}),
        hyperlinkRow(2, 5, {2}),
        hyperlinkRow(3, 5, {0}),
    };
    QVERIFY(index.apply(full));
    QVERIFY(index.hasFrame());
    QCOMPARE(index.columns(), 5);
    QCOMPARE(index.rows(), 4);
    QCOMPARE(index.contentRevision(), quint64{7});
    QCOMPARE(index.candidates(), QVector<QPoint>({
        QPoint(1, 0), QPoint(4, 0), QPoint(2, 2), QPoint(0, 3),
    }));
    QVERIFY(index.containsCoordinate(0, 0));
    QVERIFY(index.containsCoordinate(4, 3));
    QVERIFY(!index.containsCoordinate(-1, 0));
    QVERIFY(!index.containsCoordinate(5, 0));
    QVERIFY(!index.containsCoordinate(0, 4));

    TerminalUpdate partial;
    partial.columns = 5;
    partial.rows = 4;
    partial.contentRevision = 8;
    partial.dirtyRows = {
        hyperlinkRow(1, 5, {0, 3}),
        hyperlinkRow(2, 5, {1, 4}),
    };
    QVERIFY(index.apply(partial));
    QCOMPARE(index.candidates(), QVector<QPoint>({
        QPoint(1, 0), QPoint(4, 0),
        QPoint(0, 1), QPoint(3, 1),
        QPoint(1, 2), QPoint(4, 2),
        QPoint(0, 3),
    }));

    const QPoint *const unchangedStorage = index.candidates().constData();
    TerminalUpdate unchangedLinks;
    unchangedLinks.columns = 5;
    unchangedLinks.rows = 4;
    unchangedLinks.contentRevision = 9;
    unchangedLinks.dirtyRows = {hyperlinkRow(2, 5, {1, 4})};
    QVERIFY(index.apply(unchangedLinks));
    QCOMPARE(index.contentRevision(), quint64{9});
    QCOMPARE(index.candidates().constData(), unchangedStorage);

    TerminalUpdate removal;
    removal.columns = 5;
    removal.rows = 4;
    removal.contentRevision = 10;
    removal.dirtyRows = {hyperlinkRow(1, 5, {})};
    QVERIFY(index.apply(removal));
    QCOMPARE(index.candidates(), QVector<QPoint>({
        QPoint(1, 0), QPoint(4, 0),
        QPoint(1, 2), QPoint(4, 2),
        QPoint(0, 3),
    }));

    const QPoint *const candidateStorage = index.candidates().constData();
    TerminalUpdate metadataOnly;
    metadataOnly.columns = 5;
    metadataOnly.rows = 4;
    metadataOnly.contentRevision = 12;
    metadataOnly.cursorChanged = true;
    QVERIFY(index.apply(metadataOnly));
    QCOMPARE(index.contentRevision(), quint64{12});
    QCOMPARE(index.candidates().constData(), candidateStorage);

    TerminalUpdate replacement;
    replacement.columns = 3;
    replacement.rows = 2;
    replacement.fullFrame = true;
    replacement.contentRevision = 13;
    replacement.dirtyRows = {
        hyperlinkRow(0, 3, {}),
        hyperlinkRow(1, 3, {2}),
    };
    QVERIFY(index.apply(replacement));
    QCOMPARE(index.columns(), 3);
    QCOMPARE(index.rows(), 2);
    QCOMPARE(index.candidates(), QVector<QPoint>({QPoint(2, 1)}));

    index.clear();
    QVERIFY(!index.hasFrame());
    QVERIFY(index.candidates().isEmpty());
    QVERIFY(!index.apply(removal));
    QVERIFY(!index.hasFrame());
}

void TerminalTypesTest::rejectsMalformedOsc8UpdatesAtomically()
{
    TerminalOsc8Index index;
    TerminalUpdate full;
    full.columns = 3;
    full.rows = 2;
    full.fullFrame = true;
    full.contentRevision = 4;
    full.dirtyRows = {
        hyperlinkRow(0, 3, {0, 2}),
        hyperlinkRow(1, 3, {1}),
    };
    QVERIFY(index.apply(full));
    const QVector<QPoint> expected = index.candidates();

    const auto verifyRejected = [&index, &expected](TerminalUpdate update) {
        QVERIFY(!index.apply(update));
        QVERIFY(index.hasFrame());
        QCOMPARE(index.columns(), 3);
        QCOMPARE(index.rows(), 2);
        QCOMPARE(index.contentRevision(), quint64{4});
        QCOMPARE(index.candidates(), expected);
    };

    TerminalUpdate wrongWidth;
    wrongWidth.columns = 3;
    wrongWidth.rows = 2;
    wrongWidth.contentRevision = 5;
    wrongWidth.dirtyRows = {hyperlinkRow(0, 2, {0})};
    verifyRejected(wrongWidth);

    TerminalUpdate unordered;
    unordered.columns = 3;
    unordered.rows = 2;
    unordered.contentRevision = 5;
    unordered.dirtyRows = {
        hyperlinkRow(1, 3, {}),
        hyperlinkRow(0, 3, {}),
    };
    verifyRejected(unordered);

    TerminalUpdate incompleteFull;
    incompleteFull.columns = 3;
    incompleteFull.rows = 2;
    incompleteFull.fullFrame = true;
    incompleteFull.contentRevision = 5;
    incompleteFull.dirtyRows = {hyperlinkRow(0, 3, {})};
    verifyRejected(incompleteFull);

    TerminalUpdate mismatchedPartial;
    mismatchedPartial.columns = 4;
    mismatchedPartial.rows = 2;
    mismatchedPartial.contentRevision = 5;
    verifyRejected(mismatchedPartial);

    TerminalUpdate unrepresentable;
    unrepresentable.columns = std::numeric_limits<int>::max();
    unrepresentable.rows = std::numeric_limits<int>::max();
    unrepresentable.fullFrame = true;
    unrepresentable.contentRevision = 5;
    verifyRejected(unrepresentable);
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

void TerminalTypesTest::validatesTerminalOriginatedClipboardWrites()
{
    const auto verifyTarget =
        [](TerminalClipboardLocation location, bool supportsPrimary,
           std::optional<TerminalClipboardSource> expected) {
            QCOMPARE(terminalClipboardWriteTarget(location, supportsPrimary),
                     expected);
        };

    verifyTarget(TerminalClipboardLocation::Standard, false,
                 TerminalClipboardSource::Standard);
    verifyTarget(TerminalClipboardLocation::Standard, true,
                 TerminalClipboardSource::Standard);
    for (const TerminalClipboardLocation location : {
             TerminalClipboardLocation::Selection,
             TerminalClipboardLocation::Primary,
         }) {
        verifyTarget(location, false, std::nullopt);
        verifyTarget(location, true, TerminalClipboardSource::Primary);
    }

    TerminalClipboardWrite clear{
        .location = TerminalClipboardLocation::Standard,
    };
    QVERIFY(clear.contents.isEmpty());
    QVERIFY(validTerminalClipboardWritePayload(clear));

    TerminalClipboardWrite multiple{
        .location = TerminalClipboardLocation::Standard,
        .contents =
            {
                {
                    .mime = QByteArrayLiteral("application/octet-stream"),
                    .data = QByteArray::fromRawData("a\0b", 3),
                },
                {
                    .mime = QByteArrayLiteral("text/plain"),
                    .data = {},
                },
            },
    };
    QVERIFY(validTerminalClipboardWritePayload(multiple));
    QCOMPARE(multiple.contents.at(0).data, QByteArray::fromRawData("a\0b", 3));
    QVERIFY(multiple.contents.at(1).data.isEmpty());
    QVERIFY(!multiple.contents.isEmpty());

    multiple.contents[0].mime.clear();
    QVERIFY(!validTerminalClipboardWritePayload(multiple));
    multiple.contents[0].mime = QByteArray::fromRawData("text/\0plain", 11);
    QVERIFY(!validTerminalClipboardWritePayload(multiple));
}

void TerminalTypesTest::mapsPlainClipboardCodepointsInOneOrderedPass()
{
    const TerminalClipboardCodepointMap mappings{
        {.first = 'a', .last = 'z', .replacement = QStringLiteral("lower")},
        {.first = 'b', .last = 'b', .replacement = quint32{'B'}},
        {.first = 0x2500, .last = 0x257f, .replacement = quint32{'-'}},
        {.first = 0x2500, .last = 0x2500, .replacement = QStringLiteral("=")},
        {.first = 0x1f642,
         .last = 0x1f642,
         .replacement = QStringLiteral("face")},
        {.first = 0x03a3, .last = 0x03a3, .replacement = QStringLiteral("SUM")},
        {.first = 0x200b, .last = 0x200b, .replacement = QString{}},
        {.first = 'X', .last = 'X', .replacement = quint32{0x110000}},
        {.first = 'Y', .last = 'Y', .replacement = quint32{0xd800}},
        {.first = 'Z', .last = 'Z', .replacement = quint32{0x1f47b}},
    };

    const QString mapped = applyTerminalClipboardCodepointMap(
        QString::fromUtf8("ab─│🙂Σ\u200bXYZ!"), std::span{mappings});
    QCOMPARE(mapped, QString::fromUtf8("lowerB=-faceSUM\uFFFD\uFFFD👻!"));

    const QString unchanged =
        applyTerminalClipboardCodepointMap(QStringLiteral("plain"), {});
    QCOMPARE(unchanged, QStringLiteral("plain"));

    const TerminalClipboardCodepointMap deletion{
        {.first = 'x', .last = 'x', .replacement = QString{}},
    };
    const QString deleted = applyTerminalClipboardCodepointMap(
        QStringLiteral("x"), std::span{deletion});
    QVERIFY(!deleted.isNull());
    QVERIFY(deleted.isEmpty());
}

QTEST_APPLESS_MAIN(TerminalTypesTest)

#include "test_terminal_types.moc"
