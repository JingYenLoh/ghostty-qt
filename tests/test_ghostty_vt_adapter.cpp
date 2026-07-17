#include "ghostty_vt_adapter.h"

#ifdef GHOSTTY_VT_H
#error "ghostty_vt_adapter.h must not expose the libghostty-vt C API"
#endif

#include <QTest>

#include <linux/input-event-codes.h>

#include <algorithm>
#include <optional>
#include <utility>

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

QVector<QPoint> hyperlinkCandidates(const TerminalFrame &frame)
{
    QVector<QPoint> candidates;
    for (int index = 0; index < frame.cells.size(); ++index) {
        if (frame.cells.at(index).hasHyperlink) {
            candidates.append(QPoint(index % frame.columns,
                                     index / frame.columns));
        }
    }
    return candidates;
}

} // namespace

class GhosttyVtAdapterTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void rendersTerminalValuesAndEffects();
    void translatesCellStylesAndAppearanceMetadata();
    void preservesTerminalAppearanceOverrides();
    void encodesUsingTerminalModes();
    void resetsAllTerminalStateAndPublishesFullFrame();
    void resolvesOsc8HyperlinksAcrossViewportState();
    void tracksOsc8HyperlinksAcrossOutputAndReflow();
    void tracksOsc8HyperlinksAcrossViewportAndScreenChanges();
    void invalidatesTrackedOsc8HyperlinksAfterReplacementAndReset();
    void invalidatesTrackedOsc8HyperlinksAfterScrollbackPruning();
    void snapshotsLogicalLineBytesAcrossGraphemesAndWideWraps();
    void tracksTextRangesAcrossReflowViewportAndScreenChanges();
    void invalidatesTrackedTextRangesAfterCoveredTextMutation();
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

void GhosttyVtAdapterTest::resolvesOsc8HyperlinksAcrossViewportState()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 12;
    options.geometry.rows = 2;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);

    const QByteArray uri = QByteArrayLiteral("https://example.test/")
        + QStringLiteral("ghost-👻-").toUtf8() + QByteArray(160, 'x');
    QByteArray linked;
    linked += QByteArrayLiteral("\033]8;id=primary;");
    linked += uri;
    linked += QByteArrayLiteral("\033\\LINK\033]8;;\033\\ plain");
    adapter->writeVt(linked);

    TerminalFrame frame;
    renderInto(adapter.get(), &frame);
    QVector<QPoint> candidates;
    for (int index = 0; index < frame.cells.size(); ++index) {
        if (frame.cells.at(index).hasHyperlink) {
            candidates.append(QPoint(index % frame.columns,
                                     index / frame.columns));
        }
    }
    QCOMPARE(candidates.size(), 4);
    const auto match = adapter->hyperlinkAt(1, 0, candidates);
    QVERIFY(match.has_value());
    QCOMPARE(match->uri, uri);
    QCOMPARE(match->cells, candidates);
    QVERIFY(!adapter->hyperlinkAt(5, 0, candidates).has_value());
    QVERIFY(!adapter->hyperlinkAt(-1, 0, candidates).has_value());
    QVERIFY(!adapter->hyperlinkAt(12, 0, candidates).has_value());
    QVERIFY(!adapter->hyperlinkAt(0, 2, candidates).has_value());

    // Push the primary link into history. Viewport-relative lookup follows
    // the displayed scrollback rather than retaining a stale active-area ref.
    adapter->writeVt(QByteArrayLiteral("\r\nrow-2\r\nrow-3"));
    renderInto(adapter.get(), &frame);
    QVERIFY(std::none_of(frame.cells.cbegin(), frame.cells.cend(),
                         [](const TerminalCell &cell) {
                             return cell.hasHyperlink;
                         }));
    QVERIFY(adapter->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Top,
    }));
    renderInto(adapter.get(), &frame);
    candidates.clear();
    for (int index = 0; index < frame.cells.size(); ++index) {
        if (frame.cells.at(index).hasHyperlink) {
            candidates.append(QPoint(index % frame.columns,
                                     index / frame.columns));
        }
    }
    QVERIFY(!candidates.isEmpty());
    const auto historyMatch = adapter->hyperlinkAt(
        candidates.constFirst().x(), candidates.constFirst().y(), candidates);
    QVERIFY(historyMatch.has_value());
    QCOMPARE(historyMatch->uri, uri);

    // Alternate-screen links are isolated, and leaving it restores the
    // primary screen's viewport-relative destination.
    adapter->writeVt(QByteArrayLiteral("\033[?1049h"));
    const QByteArray alternateUri("file:///tmp/alternate-link");
    QByteArray alternate;
    alternate += QByteArrayLiteral("\033]8;;");
    alternate += alternateUri;
    alternate += QByteArrayLiteral("\033\\ALT\033]8;;\033\\");
    adapter->writeVt(alternate);
    renderInto(adapter.get(), &frame);
    QVector<QPoint> alternateCandidates;
    for (int index = 0; index < frame.cells.size(); ++index) {
        if (frame.cells.at(index).hasHyperlink) {
            alternateCandidates.append(QPoint(index % frame.columns,
                                              index / frame.columns));
        }
    }
    QCOMPARE(alternateCandidates.size(), 3);
    const auto alternateMatch = adapter->hyperlinkAt(
        alternateCandidates.constFirst().x(),
        alternateCandidates.constFirst().y(), alternateCandidates);
    QVERIFY(alternateMatch.has_value());
    QCOMPARE(alternateMatch->uri, alternateUri);

    adapter->writeVt(QByteArrayLiteral("\033[?1049l"));
    renderInto(adapter.get(), &frame);
    QVERIFY(adapter->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Top,
    }));
    renderInto(adapter.get(), &frame);
    const auto restored = adapter->hyperlinkAt(
        candidates.constFirst().x(), candidates.constFirst().y(), candidates);
    QVERIFY(restored.has_value());
    QCOMPARE(restored->uri, uri);

    adapter->reset();
    QVERIFY(!adapter->hyperlinkAt(0, 0, {QPoint(0, 0)}).has_value());

    // The C API contract is bytes, not QString. Preserve a non-UTF-8
    // destination exactly so clipboard export never depends on QUrl or a
    // lossy Unicode round trip.
    QByteArray byteUri = QByteArrayLiteral("https://example.test/raw-");
    byteUri.append(static_cast<char>(0xff));
    QByteArray byteLink = QByteArrayLiteral("\033]8;;");
    byteLink += byteUri;
    byteLink += QByteArrayLiteral("\033\\R\033]8;;\033\\");
    adapter->writeVt(byteLink);
    renderInto(adapter.get(), &frame);
    QVERIFY(frame.cells.constFirst().hasHyperlink);
    const auto byteMatch = adapter->hyperlinkAt(0, 0, {QPoint(0, 0)});
    QVERIFY(byteMatch.has_value());
    QCOMPARE(byteMatch->uri, byteUri);
}

void GhosttyVtAdapterTest::tracksOsc8HyperlinksAcrossOutputAndReflow()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 12;
    options.geometry.rows = 4;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);

    const QByteArray uri = QByteArrayLiteral("https://example.test/tracked-reflow");
    QByteArray output = QByteArrayLiteral("12345678\033]8;id=reflow;");
    output += uri;
    output += QByteArrayLiteral("\033\\ABCDEFGH\033]8;;\033\\");
    adapter->writeVt(output);

    TerminalFrame frame;
    renderInto(adapter.get(), &frame);
    QCOMPARE(frameRowText(frame, 0), QStringLiteral("12345678ABCD"));
    QCOMPARE(frameRowText(frame, 1), QStringLiteral("EFGH"));
    const QVector<QPoint> initialCandidates = hyperlinkCandidates(frame);
    QCOMPARE(initialCandidates.size(), 8);

    auto tracked = adapter->trackHyperlinkAt(1, 1);
    QVERIFY(tracked.has_value());
    QVERIFY(!adapter->trackHyperlinkAt(-1, 0).has_value());
    QVERIFY(!adapter->trackHyperlinkAt(12, 0).has_value());
    QVERIFY(!adapter->trackHyperlinkAt(0, 4).has_value());
    QVERIFY(!adapter->trackHyperlinkAt(0, 0).has_value());

    auto match = adapter->resolveHyperlink(*tracked, initialCandidates);
    QVERIFY(match.has_value());
    QCOMPARE(match->uri, uri);
    QCOMPARE(match->targetCell, QPoint(1, 1));
    QCOMPARE(match->cells, initialCandidates);

    // Output elsewhere in the viewport mutates libghostty's terminal state
    // and invalidates every ordinary grid ref. The owned tracked ref must
    // continue to resolve without requiring the pointer to move.
    adapter->writeVt(QByteArrayLiteral("\0337\033[4;1HTICK\0338"));
    renderInto(adapter.get(), &frame);
    match = adapter->resolveHyperlink(*tracked, hyperlinkCandidates(frame));
    QVERIFY(match.has_value());
    QCOMPARE(match->uri, uri);
    QCOMPARE(match->targetCell, QPoint(1, 1));

    // Reflow moves the tracked F cell from column 1 to column 5. Resolving
    // the old viewport coordinate would now identify a different cell.
    GhosttyVtAdapter::Geometry resized = options.geometry;
    resized.columns = 8;
    resized.rows = 5;
    QVERIFY(adapter->resize(resized));
    renderInto(adapter.get(), &frame);
    QCOMPARE(frameRowText(frame, 0), QStringLiteral("12345678"));
    QCOMPARE(frameRowText(frame, 1), QStringLiteral("ABCDEFGH"));
    const QVector<QPoint> reflowedCandidates = hyperlinkCandidates(frame);
    QCOMPARE(reflowedCandidates.size(), 8);
    match = adapter->resolveHyperlink(*tracked, reflowedCandidates);
    QVERIFY(match.has_value());
    QCOMPARE(match->uri, uri);
    QCOMPARE(match->targetCell, QPoint(5, 1));
    QCOMPARE(match->cells, reflowedCandidates);

    GhosttyVtAdapter::TrackedHyperlink moved = std::move(*tracked);
    QVERIFY(!adapter->resolveHyperlink(
        *tracked, reflowedCandidates).has_value());
    match = adapter->resolveHyperlink(moved, reflowedCandidates);
    QVERIFY(match.has_value());
    QCOMPARE(match->targetCell, QPoint(5, 1));

    auto foreignAdapter = GhosttyVtAdapter::create(options);
    QVERIFY(foreignAdapter != nullptr);
    QVERIFY(!foreignAdapter->trackedHyperlinkValid(moved));
    QVERIFY(!foreignAdapter->resolveHyperlink(
        moved, reflowedCandidates).has_value());
}

void GhosttyVtAdapterTest::tracksOsc8HyperlinksAcrossViewportAndScreenChanges()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 12;
    options.geometry.rows = 2;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);

    const QByteArray primaryUri = QByteArrayLiteral(
        "https://example.test/tracked-primary");
    QByteArray primary = QByteArrayLiteral("\033]8;id=primary;");
    primary += primaryUri;
    primary += QByteArrayLiteral("\033\\PRIMARY\033]8;;\033\\");
    adapter->writeVt(primary);

    TerminalFrame frame;
    renderInto(adapter.get(), &frame);
    auto primaryTracked = adapter->trackHyperlinkAt(2, 0);
    QVERIFY(primaryTracked.has_value());
    auto match = adapter->resolveHyperlink(
        *primaryTracked, hyperlinkCandidates(frame));
    QVERIFY(match.has_value());
    QCOMPARE(match->targetCell, QPoint(2, 0));

    // The logical target remains owned while it is outside the live
    // viewport, then becomes representable again when history is displayed.
    adapter->writeVt(QByteArrayLiteral("\r\nrow-2\r\nrow-3"));
    renderInto(adapter.get(), &frame);
    QVERIFY(!adapter->resolveHyperlink(
        *primaryTracked, hyperlinkCandidates(frame)).has_value());
    QVERIFY(adapter->trackedHyperlinkValid(*primaryTracked));
    QVERIFY(adapter->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Top,
    }));
    renderInto(adapter.get(), &frame);
    match = adapter->resolveHyperlink(
        *primaryTracked, hyperlinkCandidates(frame));
    QVERIFY(match.has_value());
    QCOMPARE(match->uri, primaryUri);
    QVERIFY(match->cells.contains(match->targetCell));

    // Tracked-ref point conversion deliberately resolves against the owning
    // page list even while another screen is active. The adapter must add an
    // active-screen check so a primary link cannot appear over alternate
    // screen content at the same viewport coordinates.
    adapter->writeVt(QByteArrayLiteral("\033[?1049h"));
    const QByteArray alternateUri = QByteArrayLiteral(
        "https://example.test/tracked-alternate");
    QByteArray alternate = QByteArrayLiteral("\033]8;id=alternate;");
    alternate += alternateUri;
    alternate += QByteArrayLiteral("\033\\ALT\033]8;;\033\\");
    adapter->writeVt(alternate);
    renderInto(adapter.get(), &frame);
    QVERIFY(!adapter->resolveHyperlink(
        *primaryTracked, hyperlinkCandidates(frame)).has_value());
    QVERIFY(adapter->trackedHyperlinkValid(*primaryTracked));

    adapter->writeVt(QByteArrayLiteral("\033[?1049l"));
    QVERIFY(adapter->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Top,
    }));
    renderInto(adapter.get(), &frame);
    match = adapter->resolveHyperlink(
        *primaryTracked, hyperlinkCandidates(frame));
    QVERIFY(match.has_value());
    QCOMPARE(match->uri, primaryUri);
    QVERIFY(match->cells.contains(match->targetCell));
}

void GhosttyVtAdapterTest::invalidatesTrackedOsc8HyperlinksAfterReplacementAndReset()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 8;
    options.geometry.rows = 2;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);

    const QByteArray originalUri = QByteArrayLiteral(
        "https://example.test/original");
    QByteArray original = QByteArrayLiteral("\033]8;;");
    original += originalUri;
    original += QByteArrayLiteral("\033\\X\033]8;;\033\\");
    adapter->writeVt(original);
    TerminalFrame frame;
    renderInto(adapter.get(), &frame);
    auto tracked = adapter->trackHyperlinkAt(0, 0);
    QVERIFY(tracked.has_value());

    // Replacing the tracked cell with a different destination must not turn
    // a pending click on the original link into activation of the new one.
    const QByteArray replacementUri = QByteArrayLiteral(
        "https://example.test/replacement");
    QByteArray replacement = QByteArrayLiteral("\033[1;1H\033]8;;");
    replacement += replacementUri;
    replacement += QByteArrayLiteral("\033\\Y\033]8;;\033\\");
    adapter->writeVt(replacement);
    renderInto(adapter.get(), &frame);
    QVERIFY(!adapter->resolveHyperlink(
        *tracked, hyperlinkCandidates(frame)).has_value());
    QVERIFY(!adapter->trackedHyperlinkValid(*tracked));

    auto replacementTracked = adapter->trackHyperlinkAt(0, 0);
    QVERIFY(replacementTracked.has_value());
    QVERIFY(adapter->resolveHyperlink(
        *replacementTracked, hyperlinkCandidates(frame)).has_value());

    adapter->writeVt(QByteArrayLiteral("\033[1;1H "));
    renderInto(adapter.get(), &frame);
    QVERIFY(!adapter->resolveHyperlink(
        *replacementTracked, hyperlinkCandidates(frame)).has_value());
    QVERIFY(!adapter->trackedHyperlinkValid(*replacementTracked));

    adapter->writeVt(original);
    renderInto(adapter.get(), &frame);
    auto resetTracked = adapter->trackHyperlinkAt(1, 0);
    QVERIFY(resetTracked.has_value());
    adapter->reset();
    renderInto(adapter.get(), &frame);
    QVERIFY(!adapter->resolveHyperlink(
        *resetTracked, hyperlinkCandidates(frame)).has_value());
    QVERIFY(!adapter->trackedHyperlinkValid(*resetTracked));
}

void GhosttyVtAdapterTest::invalidatesTrackedOsc8HyperlinksAfterScrollbackPruning()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 8;
    options.geometry.rows = 2;
    // The active area still reserves libghostty's minimum page-list memory.
    // Once a third page is needed, this zero history budget guarantees that
    // the first page is genuinely pruned rather than merely hidden.
    options.scrollbackBytes = 0;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);

    const QByteArray uri = QByteArrayLiteral("https://example.test/pruned");
    QByteArray linked = QByteArrayLiteral("\033]8;;");
    linked += uri;
    linked += QByteArrayLiteral("\033\\P\033]8;;\033\\");
    adapter->writeVt(linked);
    TerminalFrame frame;
    renderInto(adapter.get(), &frame);
    auto tracked = adapter->trackHyperlinkAt(0, 0);
    QVERIFY(tracked.has_value());

    QByteArray history;
    history.reserve(20'000 * 7);
    for (int row = 0; row < 20'000; ++row) {
        history += QByteArrayLiteral("\r\nline");
    }
    adapter->writeVt(history);
    QVERIFY(adapter->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Top,
    }));
    renderInto(adapter.get(), &frame);
    QVERIFY(!adapter->resolveHyperlink(
        *tracked, hyperlinkCandidates(frame)).has_value());
    QVERIFY(!adapter->trackedHyperlinkValid(*tracked));
}

void GhosttyVtAdapterTest::snapshotsLogicalLineBytesAcrossGraphemesAndWideWraps()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 8;
    options.geometry.rows = 4;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);

    // The combining mark remains part of e's grapheme. The wide character
    // cannot fit in the final column, so libghostty inserts a spacer head and
    // soft-wraps it onto the next physical row.
    const QByteArray line = QStringLiteral(
        "abcde\u0301fg\u754c/path").toUtf8();
    const QByteArray wide = QStringLiteral("\u754c").toUtf8();
    const qsizetype wideBegin = line.indexOf(wide);
    QVERIFY(wideBegin >= 0);
    adapter->writeVt(line);

    auto snapshot = adapter->snapshotLogicalLineAt(1, 1);
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->text(), line);
    QCOMPARE(snapshot->targetByteOffset(), wideBegin);
    QVERIFY(snapshot->byteRangeContainsTarget(wideBegin,
                                              wideBegin + wide.size()));
    QVERIFY(!snapshot->byteRangeContainsTarget(0, wideBegin));
    QVERIFY(!snapshot->byteRangeContainsTarget(wideBegin, wideBegin));

    // The right-edge spacer head has no formatter byte of its own. Ghostty's
    // inclusive selection hit test includes it only when a match starts before
    // the spacer and ends after the wrapped wide glyph.
    auto spacerSnapshot = adapter->snapshotLogicalLineAt(7, 0);
    QVERIFY(spacerSnapshot.has_value());
    QCOMPARE(spacerSnapshot->text(), line);
    QCOMPARE(spacerSnapshot->targetByteOffset(), qsizetype(-1));
    QVERIFY(spacerSnapshot->byteRangeContainsTarget(0, line.size()));
    QVERIFY(!spacerSnapshot->byteRangeContainsTarget(
        wideBegin, line.size()));
    auto spacerTracked = adapter->trackTextRange(
        *spacerSnapshot, 0, line.size());
    QVERIFY(spacerTracked.has_value());
    auto spacerMatch = adapter->resolveTextRange(*spacerTracked);
    QVERIFY(spacerMatch.has_value());
    QCOMPARE(spacerMatch->text, line);
    QCOMPARE(spacerMatch->targetCell, QPoint(7, 0));
    QVERIFY(spacerMatch->cells.contains(QPoint(7, 0)));

    auto tracked = adapter->trackTextRange(
        *snapshot, wideBegin, line.size());
    QVERIFY(tracked.has_value());
    auto match = adapter->resolveTextRange(*tracked);
    QVERIFY(match.has_value());
    QCOMPARE(match->text, line.sliced(wideBegin));
    QCOMPARE(match->targetCell, QPoint(1, 1));
    const QVector<QPoint> expectedCells{
        QPoint(0, 1), QPoint(1, 1), QPoint(2, 1), QPoint(3, 1),
        QPoint(4, 1), QPoint(5, 1), QPoint(6, 1),
    };
    QCOMPARE(match->cells, expectedCells);
    QCOMPARE(match->logicalLineRows, QVector<int>({0, 1}));

    // A regex can legally match only the combining codepoint. Every UTF-8
    // byte still maps back to the owning terminal cell, so such a sub-
    // grapheme match remains trackable without widening its returned text.
    const qsizetype combiningBegin = line.indexOf(QByteArray("\xcc\x81", 2));
    QCOMPARE(combiningBegin, qsizetype(5));
    auto combiningSnapshot = adapter->snapshotLogicalLineAt(4, 0);
    QVERIFY(combiningSnapshot.has_value());
    QVERIFY(combiningSnapshot->byteRangeContainsTarget(
        combiningBegin, combiningBegin + 2));
    auto combiningTracked = adapter->trackTextRange(
        *combiningSnapshot, combiningBegin, combiningBegin + 2);
    QVERIFY(combiningTracked.has_value());
    match = adapter->resolveTextRange(*combiningTracked);
    QVERIFY(match.has_value());
    QCOMPARE(match->text, QByteArray("\xcc\x81", 2));
    QCOMPARE(match->targetCell, QPoint(4, 0));
    QCOMPARE(match->cells, QVector<QPoint>({QPoint(4, 0)}));

    // Empty terminal storage cells between text are represented exactly as
    // ASCII spaces, while empty storage after the final text remains trimmed.
    GhosttyVtAdapter::Options gapOptions;
    gapOptions.geometry.columns = 8;
    gapOptions.geometry.rows = 2;
    auto gapAdapter = GhosttyVtAdapter::create(gapOptions);
    QVERIFY(gapAdapter != nullptr);
    gapAdapter->writeVt(QByteArrayLiteral("A\033[4GB"));
    auto gap = gapAdapter->snapshotLogicalLineAt(1, 0);
    QVERIFY(gap.has_value());
    QCOMPARE(gap->text(), QByteArrayLiteral("A  B"));
    QCOMPARE(gap->targetByteOffset(), qsizetype(1));
    auto gapTracked = gapAdapter->trackTextRange(*gap, 1, 2);
    QVERIFY(gapTracked.has_value());
    match = gapAdapter->resolveTextRange(*gapTracked);
    QVERIFY(match.has_value());
    QCOMPARE(match->text, QByteArrayLiteral(" "));
    QCOMPARE(match->targetCell, QPoint(1, 0));
    QCOMPARE(match->cells, QVector<QPoint>({QPoint(1, 0)}));
}

void GhosttyVtAdapterTest::tracksTextRangesAcrossReflowViewportAndScreenChanges()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 8;
    options.geometry.rows = 4;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);

    const QByteArray line = QStringLiteral(
        "abcde\u0301fg\u754c/path").toUtf8();
    const qsizetype matchBegin = line.indexOf(
        QStringLiteral("\u754c").toUtf8());
    QVERIFY(matchBegin >= 0);
    adapter->writeVt(line);
    auto snapshot = adapter->snapshotLogicalLineAt(1, 1);
    QVERIFY(snapshot.has_value());
    auto tracked = adapter->trackTextRange(
        *snapshot, matchBegin, line.size());
    QVERIFY(tracked.has_value());

    // Mutating an unrelated row invalidates ordinary grid refs, but all three
    // owned anchors (range endpoints and queried target) continue to resolve.
    adapter->writeVt(QByteArrayLiteral("\0337\033[4;1HTICK\0338"));
    QVERIFY(adapter->trackedTextRangeValid(*tracked));
    auto match = adapter->resolveTextRange(*tracked);
    QVERIFY(match.has_value());
    QCOMPARE(match->targetCell, QPoint(1, 1));

    GhosttyVtAdapter::Geometry resized = options.geometry;
    resized.columns = 6;
    resized.rows = 5;
    QVERIFY(adapter->resize(resized));
    QVERIFY(adapter->trackedTextRangeValid(*tracked));
    match = adapter->resolveTextRange(*tracked);
    QVERIFY(match.has_value());
    QCOMPARE(match->text, line.sliced(matchBegin));
    QCOMPARE(match->targetCell, QPoint(2, 1));
    const QVector<QPoint> reflowedCells{
        QPoint(1, 1), QPoint(2, 1), QPoint(3, 1), QPoint(4, 1),
        QPoint(5, 1), QPoint(0, 2), QPoint(1, 2),
    };
    QCOMPARE(match->cells, reflowedCells);
    QCOMPARE(match->logicalLineRows, QVector<int>({0, 1, 2}));

    adapter->writeVt(QByteArrayLiteral(
        "\r\none\r\ntwo\r\nthree\r\nfour\r\nfive\r\nsix"));
    QVERIFY(adapter->trackedTextRangeValid(*tracked));
    QVERIFY(!adapter->resolveTextRange(*tracked).has_value());
    QVERIFY(adapter->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Top,
    }));
    match = adapter->resolveTextRange(*tracked);
    QVERIFY(match.has_value());
    QCOMPARE(match->targetCell, QPoint(2, 1));

    adapter->writeVt(QByteArrayLiteral("\033[?1049hALT"));
    QVERIFY(adapter->trackedTextRangeValid(*tracked));
    QVERIFY(!adapter->resolveTextRange(*tracked).has_value());
    adapter->writeVt(QByteArrayLiteral("\033[?1049l"));
    QVERIFY(adapter->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Top,
    }));
    match = adapter->resolveTextRange(*tracked);
    QVERIFY(match.has_value());

    GhosttyVtAdapter::TrackedTextRange moved = std::move(*tracked);
    QVERIFY(!adapter->trackedTextRangeValid(*tracked));
    QVERIFY(adapter->trackedTextRangeValid(moved));
    auto foreignAdapter = GhosttyVtAdapter::create(options);
    QVERIFY(foreignAdapter != nullptr);
    QVERIFY(!foreignAdapter->trackedTextRangeValid(moved));
    QVERIFY(!foreignAdapter->resolveTextRange(moved).has_value());
}

void GhosttyVtAdapterTest::invalidatesTrackedTextRangesAfterCoveredTextMutation()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 20;
    options.geometry.rows = 3;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);

    const QByteArray line = QByteArrayLiteral("prefix /path suffix");
    adapter->writeVt(line);
    auto snapshot = adapter->snapshotLogicalLineAt(9, 0);
    QVERIFY(snapshot.has_value());
    const qsizetype begin = line.indexOf(QByteArrayLiteral("/path"));
    auto tracked = adapter->trackTextRange(
        *snapshot, begin, begin + qsizetype(5));
    QVERIFY(tracked.has_value());
    QVERIFY(adapter->trackedTextRangeValid(*tracked));

    // Changes outside the inclusive endpoint cells do not invalidate the
    // lease. The worker will re-run whole-line regex precedence separately.
    adapter->writeVt(QByteArrayLiteral("\033[1;1HQ"));
    QVERIFY(adapter->trackedTextRangeValid(*tracked));
    QVERIFY(adapter->resolveTextRange(*tracked).has_value());

    adapter->writeVt(QByteArrayLiteral("\033[1;9HX"));
    QVERIFY(!adapter->trackedTextRangeValid(*tracked));
    QVERIFY(!adapter->resolveTextRange(*tracked).has_value());

    adapter->reset();
    QVERIFY(!adapter->trackedTextRangeValid(*tracked));
    QVERIFY(!adapter->resolveTextRange(*tracked).has_value());
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

void GhosttyVtAdapterTest::resetsAllTerminalStateAndPublishesFullFrame()
{
    QByteArray ptyWrites;
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 12;
    options.geometry.rows = 3;
    auto adapter = GhosttyVtAdapter::create(
        options, {.writePty = [&ptyWrites](const QByteArray &data) {
            ptyWrites.append(data);
        }});
    QVERIFY(adapter != nullptr);

    adapter->writeVt(QByteArrayLiteral(
        "primary-0\r\nprimary-1\r\nprimary-2\r\nprimary-3\r\n"
        "primary-4\r\nprimary-5\033[?1049halternate"
        "\033[?1003h\033[?2004h\033[?1004h"
        "\033]0;reset-title\a\033]7;file:///tmp/reset-cwd\a\033[c"));
    adapter->synchronizeInputModes();
    QVERIFY(adapter->selectAll());
    QVERIFY(adapter->hasSelection());

    TerminalFrame frame;
    GhosttyVtAdapter::RenderSnapshot before;
    QCOMPARE(adapter->renderFrame(&before),
             GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(applyTerminalUpdate(&frame, before.update));
    QVERIFY(before.mouseTracking);
    QCOMPARE(adapter->encodePaste(QStringLiteral("paste")),
             QByteArrayLiteral("\033[200~paste\033[201~"));
    QCOMPARE(adapter->encodeFocus(true), QByteArrayLiteral("\033[I"));
    QVERIFY(frameText(frame).contains(QStringLiteral("alternate")));
    QVERIFY(!ptyWrites.isEmpty());
    const GhosttyVtAdapter::DeferredEffects beforeEffects =
        adapter->takeDeferredEffects();
    QCOMPARE(beforeEffects.title, QStringLiteral("reset-title"));
    QCOMPARE(beforeEffects.currentDirectory,
             QStringLiteral("/tmp/reset-cwd"));

    // Reset is a local emulator mutation: it must neither synthesize input
    // for the child nor leave refs and mode caches tied to the old grids.
    ptyWrites.clear();
    adapter->reset();
    QCOMPARE(ptyWrites, QByteArray{});
    QVERIFY(!adapter->hasSelection());
    const GhosttyVtAdapter::DeferredEffects resetEffects =
        adapter->takeDeferredEffects();
    QVERIFY(!resetEffects.title.isNull());
    QVERIFY(resetEffects.title.isEmpty());
    QVERIFY(!resetEffects.currentDirectory.isNull());
    QVERIFY(resetEffects.currentDirectory.isEmpty());

    GhosttyVtAdapter::RenderSnapshot after;
    QCOMPARE(adapter->renderFrame(&after),
             GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(after.update.fullFrame);
    QCOMPARE(after.update.dirtyRows.size(), options.geometry.rows);
    QVERIFY(!after.mouseTracking);
    QCOMPARE(adapter->encodePaste(QStringLiteral("paste")),
             QByteArrayLiteral("paste"));
    QCOMPARE(adapter->encodeFocus(true), QByteArray{});
    QVERIFY(applyTerminalUpdate(&frame, after.update));
    QCOMPARE(frame.columns, options.geometry.columns);
    QCOMPARE(frame.rows, options.geometry.rows);
    QCOMPARE(frame.cursorColumn, 0);
    QCOMPARE(frame.cursorRow, 0);
    QVERIFY(!frameText(frame).contains(QStringLiteral("primary")));
    QVERIFY(!frameText(frame).contains(QStringLiteral("alternate")));
    QCOMPARE(frame.scrollOffset, quint64{0});
    QCOMPARE(frame.scrollTotal, frame.scrollLength);
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
