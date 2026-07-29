#include "ghostty_vt_adapter.h"

#ifdef GHOSTTY_VT_H
#error "ghostty_vt_adapter.h must not expose the libghostty-vt C API"
#endif

#include <QSysInfo>
#include <QTest>
#include <QUrl>

#include <linux/input-event-codes.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace {

constexpr quint64 nanosecondsPerMillisecond = 1'000'000;

TerminalSelectionPressInput
selectionPress(const GhosttyVtAdapter::Geometry &geometry, int column, int row,
               std::optional<quint64> timestampNanoseconds = std::nullopt,
               bool controlModifier = false,
               bool extendExistingSelection = false, bool rectangular = false)
{
    return {
        .column = column,
        .row = row,
        .surfaceX =
            (static_cast<double>(column) + 0.5) * geometry.cellWidthPixels,
        .surfaceY =
            (static_cast<double>(row) + 0.5) * geometry.cellHeightPixels,
        .timestampNanoseconds = timestampNanoseconds.value_or(0),
        .timestampValid = timestampNanoseconds.has_value(),
        .controlModifier = controlModifier,
        .extendExistingSelection = extendExistingSelection,
        .rectangular = rectangular,
    };
}

TerminalSelectionDragInput
selectionDrag(const GhosttyVtAdapter::Geometry &geometry, int column, int row,
              bool rectangular = false)
{
    return {
        .column = column,
        .row = row,
        .surfaceX =
            (static_cast<double>(column) + 0.5) * geometry.cellWidthPixels,
        .surfaceY =
            (static_cast<double>(row) + 0.5) * geometry.cellHeightPixels,
        .rectangular = rectangular,
    };
}

bool beginFreshDoubleClick(GhosttyVtAdapter *adapter,
                           const GhosttyVtAdapter::Geometry &geometry,
                           int column, int row,
                           quint64 firstTimestampNanoseconds = 1'000
                               * nanosecondsPerMillisecond,
                           bool controlModifier = false)
{
    adapter->clearSelectionAndResetGesture();
    if (adapter->beginSelection(selectionPress(geometry, column, row,
                                               firstTimestampNanoseconds,
                                               controlModifier))) {
        return false;
    }
    adapter->endSelection(column, row);
    return adapter->beginSelection(
        selectionPress(geometry, column, row,
                       firstTimestampNanoseconds + nanosecondsPerMillisecond,
                       controlModifier));
}

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
    const bool applied = applyTerminalUpdate(frame, update);
    Q_ASSERT(applied);
    return frame;
}

void renderInto(GhosttyVtAdapter *adapter, TerminalFrame *frame)
{
    GhosttyVtAdapter::RenderSnapshot snapshot;
    const auto result = adapter->renderFrame(&snapshot);
    QVERIFY(result == GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(applyTerminalUpdate(*frame, snapshot.update));
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
    void respondsToEnquiryWithConfiguredBytes();
    void reportsConfiguredColorSchemeAndLiveChanges();
    void observesActiveScreenBottomAnchorChanges();
    void normalizesTerminalClipboardWritesAndPolicies();
    void validatesDynamicAndMacShapedOsc7Hostnames();
    void translatesCellStylesAndAppearanceMetadata();
    void preservesAuthoritativeCellCodepointsForShaping();
    void preservesTerminalAppearanceOverrides();
    void queriesSemanticPromptStateFromPublicTerminalData();
    void queriesKeyboardActionMode();
    void marksMinimumContrastExemptGlyphs();
    void encodesUsingTerminalModes();
    void preparesPasteUsingExactSafetyPolicy();
    void clearsSelectionWithoutCancellingGesture();
    void resetsAllTerminalStateAndPublishesFullFrame();
    void resolvesOsc8HyperlinksAcrossViewportState();
    void tracksOsc8HyperlinksAcrossOutputAndReflow();
    void tracksOsc8HyperlinksAcrossViewportAndScreenChanges();
    void invalidatesTrackedOsc8HyperlinksAfterReplacementAndReset();
    void invalidatesTrackedOsc8HyperlinksAfterScrollbackPruning();
    void snapshotsLogicalLineBytesAcrossGraphemesAndWideWraps();
    void snapshotsPhysicalSearchRowsAcrossHistoryAndScreens();
    void searchSnapshotsFollowLiveDeccolmDimensions();
    void tracksTextRangesAcrossReflowViewportAndScreenChanges();
    void invalidatesTrackedTextRangesAfterCoveredTextMutation();
    void installsTrackedTextRangesAsSelections();
    void selectsAndNavigatesViewportAtomically();
    void mapsAndRevealsSearchRanges();
    void formatsSelectionWithConfigurableTrimming();
    void selectsCellsWordsAndQueriesSelectionContainment();
    void classifiesRepeatedSelectionPresses();
    void extendsSelectionOnDelayedShiftPress();
    void appliesConfiguredWordBoundariesToPressAndDrag();
    void snapshotsPlainWriteFileRanges();
    void snapshotsPlainWriteFileFormattingAndAlternateScreen();
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
                          "\033]7;file://localhost/tmp\007"
                          "\007A\033[31mB\033[c"));
    const GhosttyVtAdapter::DeferredEffects effects = adapter->takeDeferredEffects();
    QCOMPARE(effects.title, QStringLiteral("adapter-title"));
    QCOMPARE(effects.currentDirectory, QStringLiteral("/tmp"));
    QVERIFY(effects.bell);
    QCOMPARE(ptyWrites, QByteArrayLiteral("\033[?62;22;52c"));

    adapter->writeVt(QByteArrayLiteral(
        "\033]7;file://definitely-remote.invalid/remote/path\007"));
    const GhosttyVtAdapter::DeferredEffects remoteEffects =
        adapter->takeDeferredEffects();
    QVERIFY(remoteEffects.currentDirectory.isNull());

    adapter->writeVt(QByteArrayLiteral(
        "\033]7;file://LOCALHOST/case-mismatch\007"));
    QVERIFY(adapter->takeDeferredEffects().currentDirectory.isNull());

    adapter->writeVt(QByteArrayLiteral(
        "\033]7;file://localhost/tmp/encoded%20directory\007"));
    const GhosttyVtAdapter::DeferredEffects localEffects =
        adapter->takeDeferredEffects();
    QCOMPARE(localEffects.currentDirectory,
             QStringLiteral("/tmp/encoded directory"));

    adapter->writeVt(QByteArrayLiteral(
        "\033]7;file://localhost/tmp/literal space/%zz/line%+Afeed\007"));
    QCOMPARE(adapter->takeDeferredEffects().currentDirectory,
             QStringLiteral("/tmp/literal space/%zz/line\nfeed"));

    for (const QByteArray &port : {
             QByteArrayLiteral("+80"),
             QByteArrayLiteral("8_0"),
             QByteArrayLiteral("-0"),
         }) {
        adapter->writeVt(QByteArrayLiteral("\033]7;file://localhost:")
                         + port
                         + QByteArrayLiteral("/tmp/zig-port\007"));
        QCOMPARE(adapter->takeDeferredEffects().currentDirectory,
                 QStringLiteral("/tmp/zig-port"));
    }
    for (const QByteArray &port : {
             QByteArrayLiteral("_80"),
             QByteArrayLiteral("-1"),
             QByteArrayLiteral("65536"),
         }) {
        adapter->writeVt(QByteArrayLiteral("\033]7;file://localhost:")
                         + port
                         + QByteArrayLiteral("/tmp/invalid-port\007"));
        QVERIFY(adapter->takeDeferredEffects().currentDirectory.isNull());
    }

    const QString machineHost = QSysInfo::machineHostName();
    QVERIFY(!machineHost.isEmpty());
    QUrl machineUrl;
    machineUrl.setScheme(QStringLiteral("file"));
    machineUrl.setHost(machineHost);
    machineUrl.setPath(QStringLiteral("/tmp/machine-host"));
    adapter->writeVt(QByteArrayLiteral("\033]7;")
                     + machineUrl.toEncoded()
                     + QByteArrayLiteral("\007"));
    QCOMPARE(adapter->takeDeferredEffects().currentDirectory,
             QStringLiteral("/tmp/machine-host"));

    adapter->writeVt(QByteArrayLiteral(
        "\033]7;kitty-shell-cwd://localhost/tmp/literal%20 raw??#fragment\007"));
    QCOMPARE(adapter->takeDeferredEffects().currentDirectory,
             QStringLiteral("/tmp/literal%20 raw??#fragment"));

    adapter->writeVt(
        QByteArrayLiteral("\033]7;file:///missing-host\007"));
    QVERIFY(adapter->takeDeferredEffects().currentDirectory.isNull());

    adapter->writeVt(
        QByteArrayLiteral("\033]7;file://localhost\007"));
    const QString emptyPath =
        adapter->takeDeferredEffects().currentDirectory;
    QVERIFY(!emptyPath.isNull());
    QVERIFY(emptyPath.isEmpty());

    adapter->writeVt(
        QByteArrayLiteral("\033]7;https://localhost/not-a-file\007"));
    QVERIFY(adapter->takeDeferredEffects().currentDirectory.isNull());

    adapter->writeVt(QByteArrayLiteral(
        "\033]7;file://localhost/tmp/batched-local\007"
        "\033]7;file://remote.invalid/ignored\007"));
    QCOMPARE(adapter->takeDeferredEffects().currentDirectory,
             QStringLiteral("/tmp/batched-local"));

    adapter->writeVt(QByteArrayLiteral(
        "\033]7;\007"
        "\033]7;file://remote.invalid/ignored\007"));
    const QString batchedClear =
        adapter->takeDeferredEffects().currentDirectory;
    QVERIFY(!batchedClear.isNull());
    QVERIFY(batchedClear.isEmpty());

    adapter->writeVt(QByteArrayLiteral(
        "\033]7;file://remote.invalid/ignored\007"
        "\033]7;file://localhost/tmp/final-local\007"));
    QCOMPARE(adapter->takeDeferredEffects().currentDirectory,
             QStringLiteral("/tmp/final-local"));

    adapter->writeVt(QByteArrayLiteral("\033]7;\007"));
    const GhosttyVtAdapter::DeferredEffects clearedEffects =
        adapter->takeDeferredEffects();
    QVERIFY(!clearedEffects.currentDirectory.isNull());
    QVERIFY(clearedEffects.currentDirectory.isEmpty());

    GhosttyVtAdapter::RenderSnapshot snapshot;
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(snapshot.update.fullFrame);
    QVERIFY(snapshot.update.colorsChanged);
    QCOMPARE(snapshot.update.palette.size(), 256);
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
    QVERIFY(!snapshot.update.colorsChanged);
    QVERIFY(snapshot.update.palette.isEmpty());

    const QColor reloadedForeground(QStringLiteral("#f4dbd6"));
    const QColor reloadedBackground(QStringLiteral("#1e2030"));
    const QColor reloadedCursor(QStringLiteral("#f5bde6"));
    TerminalAppearance reloadedAppearance = options.appearance;
    reloadedAppearance.foregroundColor = reloadedForeground;
    reloadedAppearance.backgroundColor = reloadedBackground;
    reloadedAppearance.cursorColor =
        TerminalColorValue::fromColor(reloadedCursor);
    const QColor *const paletteBeforeReload = frame.palette.constData();
    QVERIFY(adapter->setAppearance(reloadedAppearance));
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(snapshot.update.colorsChanged);
    QCOMPARE(snapshot.update.palette.size(), 256);
    QCOMPARE(snapshot.update.palette.constData(), paletteBeforeReload);
    QVERIFY(applyTerminalUpdate(frame, snapshot.update));
    QCOMPARE(frame.foreground, reloadedForeground);
    QCOMPARE(frame.background, reloadedBackground);
    QCOMPARE(frame.cursorColor, reloadedCursor);
    QVERIFY(frame.cursorColorExplicit);
    QCOMPARE(frame.palette.constData(), paletteBeforeReload);

    adapter->writeVt(QByteArrayLiteral("\rZ"));
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(!snapshot.update.fullFrame);
    QCOMPARE(snapshot.update.dirtyRows.size(), 1);
    QVERIFY(!snapshot.update.colorsChanged);
    QVERIFY(snapshot.update.palette.isEmpty());
    QVERIFY(snapshot.update.cursorChanged);
    QVERIFY(applyTerminalUpdate(frame, snapshot.update));
    QVERIFY(frameText(frame).startsWith(QStringLiteral("ZB")));

    GhosttyVtAdapter::Geometry resized = options.geometry;
    resized.columns = 10;
    resized.rows = 4;
    QVERIFY(adapter->resize(resized));
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(snapshot.update.fullFrame);
    QVERIFY(snapshot.update.colorsChanged);
    QCOMPARE(snapshot.update.palette.size(), 256);
    QCOMPARE(snapshot.update.dirtyRows.size(), 4);
    QVERIFY(applyTerminalUpdate(frame, snapshot.update));
    QCOMPARE(frame.columns, 10);
    QCOMPARE(frame.rows, 4);
}

void GhosttyVtAdapterTest::respondsToEnquiryWithConfiguredBytes()
{
    QVector<QByteArray> defaultWrites;
    auto defaultAdapter = GhosttyVtAdapter::create(
        {}, {.writePty = [&defaultWrites](const QByteArray &data) {
            defaultWrites.append(data);
        }});
    QVERIFY(defaultAdapter != nullptr);
    defaultAdapter->writeVt(QByteArray(1, '\x05'));
    QVERIFY(defaultWrites.isEmpty());

    QByteArray response;
    response.append('\0');
    response.append(static_cast<char>(0x80));
    response.append(static_cast<char>(0xff));
    response.append('A');

    QVector<QByteArray> writes;
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 32;
    options.geometry.rows = 2;
    options.enquiryResponse = response;
    auto adapter = GhosttyVtAdapter::create(
        options, {.writePty = [&writes](const QByteArray &data) {
            writes.append(data);
        }});
    QVERIFY(adapter != nullptr);

    // The adapter owns the configured bytes; changing the source after
    // construction cannot invalidate the synchronous callback response.
    options.enquiryResponse.fill('x');
    QByteArray input = QByteArrayLiteral("left");
    input.append('\x05');
    input.append(QByteArrayLiteral("middle"));
    input.append('\x05');
    input.append(QByteArrayLiteral("right"));
    adapter->writeVt(input);
    QCOMPARE(writes.size(), 2);
    QCOMPARE(writes.at(0), response);
    QCOMPARE(writes.at(1), response);

    TerminalFrame frame;
    renderInto(adapter.get(), &frame);
    QCOMPARE(frameRowText(frame, 0), QStringLiteral("leftmiddleright"));

    QByteArray reloaded = QByteArrayLiteral("next");
    reloaded.append('\0');
    adapter->setEnquiryResponse(reloaded);
    adapter->writeVt(QByteArray(1, '\x05'));
    QCOMPARE(writes.size(), 3);
    QCOMPARE(writes.constLast(), reloaded);

    const QByteArray maximumBridgeResponse(255, 'y');
    adapter->setEnquiryResponse(maximumBridgeResponse);
    adapter->writeVt(QByteArray(1, '\x05'));
    QCOMPARE(writes.size(), 4);
    QCOMPARE(writes.constLast(), maximumBridgeResponse);

    // The pinned public C bridge reserves one byte in a 256-byte stack buffer
    // for its terminator and silently ignores responses that do not fit.
    adapter->setEnquiryResponse(QByteArray(256, 'z'));
    adapter->writeVt(QByteArray(1, '\x05'));
    QCOMPARE(writes.size(), 4);

    adapter->setEnquiryResponse({});
    adapter->writeVt(QByteArray(1, '\x05'));
    QCOMPARE(writes.size(), 4);
}

void GhosttyVtAdapterTest::reportsConfiguredColorSchemeAndLiveChanges()
{
    QVector<QByteArray> writes;
    GhosttyVtAdapter::Options options;
    options.colorScheme = TerminalColorScheme::Dark;
    auto adapter = GhosttyVtAdapter::create(
        options, {.writePty = [&writes](const QByteArray &data) {
            writes.append(data);
        }});
    QVERIFY(adapter != nullptr);

    // Construction only supplies the value queried by the terminal. It must
    // not synthesize an unsolicited report before the application enables
    // mode 2031.
    QVERIFY(writes.isEmpty());
    adapter->writeVt(QByteArrayLiteral("\033[?996n"));
    QCOMPARE(writes, QVector{QByteArrayLiteral("\033[?997;1n")});

    writes.clear();
    adapter->setColorScheme(TerminalColorScheme::Light);
    QVERIFY(writes.isEmpty());
    adapter->writeVt(QByteArrayLiteral("\033[?996n"));
    QCOMPARE(writes, QVector{QByteArrayLiteral("\033[?997;2n")});

    writes.clear();
    adapter->writeVt(QByteArrayLiteral("\033[?2031h"));
    adapter->setColorScheme(TerminalColorScheme::Dark);
    QCOMPARE(writes, QVector{QByteArrayLiteral("\033[?997;1n")});
    adapter->setColorScheme(TerminalColorScheme::Dark);
    QCOMPARE(writes.size(), 1);

    writes.clear();
    adapter->writeVt(QByteArrayLiteral("\033[?2031l"));
    adapter->setColorScheme(TerminalColorScheme::Light);
    QVERIFY(writes.isEmpty());

    // A terminal reset also disables mode 2031, without discarding the
    // frontend-owned scheme subsequently returned to a direct query.
    adapter->writeVt(QByteArrayLiteral("\033[?2031h"));
    adapter->reset();
    adapter->setColorScheme(TerminalColorScheme::Dark);
    QVERIFY(writes.isEmpty());
    adapter->writeVt(QByteArrayLiteral("\033[?996n"));
    QCOMPARE(writes, QVector{QByteArrayLiteral("\033[?997;1n")});
}

void GhosttyVtAdapterTest::queriesSemanticPromptStateFromPublicTerminalData()
{
    using State = GhosttyVtAdapter::SemanticPromptState;

    GhosttyVtAdapter::Options options;
    options.geometry.columns = 12;
    options.geometry.rows = 4;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);

    // An unmarked terminal is queryable but is not at a semantic prompt.
    QCOMPARE(adapter->semanticPromptState(), State::Away);

    adapter->writeVt(QByteArrayLiteral("\033]133;A\a$ \033]133;B\acmd"));
    QCOMPARE(adapter->semanticPromptState(), State::AtPrompt);

    // Starting output on the prompt row remains at a prompt, matching
    // Terminal.cursorIsAtPrompt's row-first rule.
    adapter->writeVt(QByteArrayLiteral("\033]133;C\a"));
    QCOMPARE(adapter->semanticPromptState(), State::AtPrompt);
    adapter->writeVt(QByteArrayLiteral("\r\noutput"));
    QCOMPARE(adapter->semanticPromptState(), State::Away);

    // A continuation marker is sufficient even before text is printed.
    adapter->writeVt(QByteArrayLiteral("\r\n\033]133;P;k=c\a"));
    QCOMPARE(adapter->semanticPromptState(), State::AtPrompt);

    // The public fallback reads the stored cell under the cursor. Exercise it
    // on an input cell in a row with no prompt marker.
    adapter->writeVt(QByteArrayLiteral("\r\n\033]133;B\ax\b"));
    QCOMPARE(adapter->semanticPromptState(), State::AtPrompt);

    // Semantic markers never make the alternate screen a shell prompt.
    adapter->writeVt(QByteArrayLiteral("\033[?1049h\033]133;A\a$ "));
    QCOMPARE(adapter->semanticPromptState(), State::Away);
    adapter->writeVt(QByteArrayLiteral("\033[?1049l"));
    QCOMPARE(adapter->semanticPromptState(), State::AtPrompt);

    adapter->reset();
    QCOMPARE(adapter->semanticPromptState(), State::Away);
}

void GhosttyVtAdapterTest::observesActiveScreenBottomAnchorChanges()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 8;
    options.geometry.rows = 3;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);

    // Ghostty initializes its renderer-side identity to null. Synchronized
    // output must not seed that identity before the first renderable frame.
    adapter->writeVt(QByteArrayLiteral("\033[?2026h"));
    QVERIFY(!adapter->observeOutputBottomAnchorChanged());
    adapter->writeVt(QByteArrayLiteral("\033[?2026l"));
    // The first successful observation establishes the baseline and therefore
    // changes.
    QVERIFY(adapter->observeOutputBottomAnchorChanged());
    QVERIFY(!adapter->observeOutputBottomAnchorChanged());

    // Dirty cells, cursor movement, styles, BEL, and title changes do not
    // constitute new output until the physical final row advances.
    adapter->writeVt(
        QByteArrayLiteral("abc\rXYZ\033[31m\007\033]2;anchor-title\007"));
    QVERIFY(!adapter->observeOutputBottomAnchorChanged());
    adapter->writeVt(QByteArrayLiteral("\r\nrow-1\r\nrow-2"));
    QVERIFY(!adapter->observeOutputBottomAnchorChanged());

    // Scrolling at the final row advances the PageList bottom node/y. Multiple
    // writes before one observation coalesce into one change notification.
    adapter->writeVt(QByteArrayLiteral("\r\nrow-3\r\nrow-4"));
    QVERIFY(adapter->observeOutputBottomAnchorChanged());
    QVERIFY(!adapter->observeOutputBottomAnchorChanged());
    adapter->writeVt(QByteArrayLiteral("\rROW-4"));
    QVERIFY(!adapter->observeOutputBottomAnchorChanged());

    // Synchronized output suppresses renderer updates, so observations
    // neither report nor consume bottom changes until mode 2026 ends.
    adapter->writeVt(QByteArrayLiteral("\033[?2026h\r\nsync-5\r\nsync-6"));
    QVERIFY(!adapter->observeOutputBottomAnchorChanged());
    adapter->writeVt(QByteArrayLiteral("\r\nsync-7"));
    QVERIFY(!adapter->observeOutputBottomAnchorChanged());
    adapter->writeVt(QByteArrayLiteral("\033[?2026l"));
    QVERIFY(adapter->observeOutputBottomAnchorChanged());
    QVERIFY(!adapter->observeOutputBottomAnchorChanged());

    // Each screen owns a different PageList, so entering and leaving the
    // alternate screen changes the identity even when neither has scrollback.
    adapter->writeVt(QByteArrayLiteral("\033[?1049h"));
    QVERIFY(adapter->observeOutputBottomAnchorChanged());
    QVERIFY(!adapter->observeOutputBottomAnchorChanged());
    adapter->writeVt(QByteArrayLiteral("alternate"));
    QVERIFY(!adapter->observeOutputBottomAnchorChanged());
    adapter->writeVt(QByteArrayLiteral("\033[?1049l"));
    QVERIFY(adapter->observeOutputBottomAnchorChanged());
    QVERIFY(!adapter->observeOutputBottomAnchorChanged());

    // Resizing and resetting may rebuild or reuse the same PageList identity.
    // Whatever Ghostty exposes becomes a stable new baseline without retaining
    // an expired grid reference.
    GhosttyVtAdapter::Geometry resized = options.geometry;
    resized.rows = 4;
    QVERIFY(adapter->resize(resized));
    (void)adapter->observeOutputBottomAnchorChanged();
    QVERIFY(!adapter->observeOutputBottomAnchorChanged());
    adapter->reset();
    (void)adapter->observeOutputBottomAnchorChanged();
    QVERIFY(!adapter->observeOutputBottomAnchorChanged());
}

void GhosttyVtAdapterTest::normalizesTerminalClipboardWritesAndPolicies()
{
    QByteArray ptyWrites;
    GhosttyVtAdapter::Options options;
    auto adapter = GhosttyVtAdapter::create(
        options, {.writePty = [&ptyWrites](const QByteArray &data) {
            ptyWrites.append(data);
        }});
    QVERIFY(adapter != nullptr);

    // The default allow policy advertises OSC 52 and a fragmented sequence
    // becomes one owned, binary-safe write only after its terminator arrives.
    adapter->writeVt(QByteArrayLiteral("\033[c"));
    QCOMPARE(ptyWrites, QByteArrayLiteral("\033[?62;22;52c"));
    ptyWrites.clear();
    adapter->writeVt(QByteArrayLiteral("\033]52;c;aGVs"));
    QVERIFY(adapter->takeDeferredEffects().clipboardWrites.isEmpty());
    adapter->writeVt(QByteArrayLiteral("bG8Ad29ybGQ=\033\\"));
    auto effects = adapter->takeDeferredEffects();
    QCOMPARE(effects.clipboardWrites.size(), 1);
    const TerminalClipboardWriteRequest embeddedNul =
        effects.clipboardWrites.constFirst();
    QCOMPARE(embeddedNul.write.location, TerminalClipboardLocation::Standard);
    QVERIFY(!embeddedNul.confirmationRequired);
    QCOMPARE(embeddedNul.write.contents.size(), 1);
    QCOMPARE(embeddedNul.write.contents.constFirst().mime,
             QByteArrayLiteral("text/plain"));
    QCOMPARE(embeddedNul.write.contents.constFirst().data,
             QByteArray("hello\0world", 11));

    // The callback's borrowed decode buffer is gone by the next VT write; the
    // retained value must remain unchanged, while writes preserve wire order
    // and normalized destinations. Empty OSC 52 data is a clear operation.
    adapter->writeVt(QByteArrayLiteral("\033]52;s;eA==\033\\"
                                       "\033]52;p;eQ==\a"
                                       "\033]52;s;\033\\"
                                       "\033]52;q;eg==\033\\"));
    effects = adapter->takeDeferredEffects();
    QCOMPARE(effects.clipboardWrites.size(), 4);
    QCOMPARE(effects.clipboardWrites.at(0).write.location,
             TerminalClipboardLocation::Selection);
    QCOMPARE(effects.clipboardWrites.at(0).write.contents.constFirst().data,
             QByteArrayLiteral("x"));
    QCOMPARE(effects.clipboardWrites.at(1).write.location,
             TerminalClipboardLocation::Primary);
    QCOMPARE(effects.clipboardWrites.at(1).write.contents.constFirst().data,
             QByteArrayLiteral("y"));
    QCOMPARE(effects.clipboardWrites.at(2).write.location,
             TerminalClipboardLocation::Selection);
    QVERIFY(effects.clipboardWrites.at(2).write.contents.isEmpty());
    QCOMPARE(effects.clipboardWrites.at(3).write.location,
             TerminalClipboardLocation::Standard);
    QCOMPARE(effects.clipboardWrites.at(3).write.contents.constFirst().data,
             QByteArrayLiteral("z"));
    QCOMPARE(embeddedNul.write.contents.constFirst().data,
             QByteArray("hello\0world", 11));

    // Read requests and malformed base64 are intentionally silent.
    adapter->writeVt(QByteArrayLiteral("\033]52;c;?\033\\"
                                       "\033]52;c;%%%\033\\"));
    QVERIFY(adapter->takeDeferredEffects().clipboardWrites.isEmpty());

    // iTerm2 Copy shares the same normalized representation.
    adapter->writeVt(QByteArrayLiteral("\033]1337;Copy=:aVRlcm0=\033\\"));
    effects = adapter->takeDeferredEffects();
    QCOMPARE(effects.clipboardWrites.size(), 1);
    QCOMPARE(effects.clipboardWrites.constFirst().write.location,
             TerminalClipboardLocation::Standard);
    QCOMPARE(
        effects.clipboardWrites.constFirst().write.contents.constFirst().mime,
        QByteArrayLiteral("text/plain"));
    QCOMPARE(
        effects.clipboardWrites.constFirst().write.contents.constFirst().data,
        QByteArrayLiteral("iTerm"));

    // Ask remains an advertised capability and snapshots confirmation onto
    // the request. Deny removes the capability and emits no deferred effect;
    // allowing it again takes effect without reconstructing the terminal.
    adapter->setClipboardWriteAccess(TerminalClipboardAccess::Ask);
    adapter->writeVt(QByteArrayLiteral("\033[c\033]52;c;YXNr\033\\"));
    effects = adapter->takeDeferredEffects();
    QCOMPARE(ptyWrites, QByteArrayLiteral("\033[?62;22;52c"));
    QCOMPARE(effects.clipboardWrites.size(), 1);
    QVERIFY(effects.clipboardWrites.constFirst().confirmationRequired);
    QCOMPARE(
        effects.clipboardWrites.constFirst().write.contents.constFirst().data,
        QByteArrayLiteral("ask"));

    ptyWrites.clear();
    adapter->setClipboardWriteAccess(TerminalClipboardAccess::Deny);
    adapter->writeVt(QByteArrayLiteral("\033[c\033]52;c;ZGVuaWVk\033\\"));
    QCOMPARE(ptyWrites, QByteArrayLiteral("\033[?62;22c"));
    QVERIFY(adapter->takeDeferredEffects().clipboardWrites.isEmpty());

    ptyWrites.clear();
    adapter->setClipboardWriteAccess(TerminalClipboardAccess::Allow);
    adapter->writeVt(QByteArrayLiteral("\033[c\033]52;c;YWxsb3dlZA==\033\\"));
    effects = adapter->takeDeferredEffects();
    QCOMPARE(ptyWrites, QByteArrayLiteral("\033[?62;22;52c"));
    QCOMPARE(effects.clipboardWrites.size(), 1);
    QVERIFY(!effects.clipboardWrites.constFirst().confirmationRequired);
    QCOMPARE(
        effects.clipboardWrites.constFirst().write.contents.constFirst().data,
        QByteArrayLiteral("allowed"));

    // Byte accounting alone cannot bound clear requests because they carry no
    // representations. One adapter drain retains at most 64 writes, then a
    // drain restores capacity for the next request.
    QByteArray clearFlood;
    constexpr int retainedClipboardWriteLimit = 64;
    for (int index = 0; index < retainedClipboardWriteLimit + 1; ++index) {
        clearFlood.append(QByteArrayLiteral("\033]52;c;\033\\"));
    }
    adapter->writeVt(clearFlood);
    effects = adapter->takeDeferredEffects();
    QCOMPARE(effects.clipboardWrites.size(), retainedClipboardWriteLimit);
    for (const TerminalClipboardWriteRequest &request :
         effects.clipboardWrites) {
        QVERIFY(request.write.contents.isEmpty());
    }
    adapter->writeVt(QByteArrayLiteral("\033]52;c;\033\\"));
    QCOMPARE(adapter->takeDeferredEffects().clipboardWrites.size(), 1);
}

void GhosttyVtAdapterTest::validatesDynamicAndMacShapedOsc7Hostnames()
{
    QByteArray machineHost = QByteArrayLiteral("00:12:34:56:78:90");
    auto adapter = GhosttyVtAdapter::create(
        {}, {.queryMachineHostName = [&machineHost] { return machineHost; }});
    QVERIFY(adapter != nullptr);

    adapter->writeVt(QByteArrayLiteral(
        "\033]7;file://00:12:34:56:78:90/tmp/mac-host\007"));
    QCOMPARE(adapter->takeDeferredEffects().currentDirectory,
             QStringLiteral("/tmp/mac-host"));

    adapter->writeVt(QByteArrayLiteral(
        "\033]7;file://user@00:12:34:56:78:90/tmp/mac-userinfo\007"));
    QCOMPARE(adapter->takeDeferredEffects().currentDirectory,
             QStringLiteral("/tmp/mac-userinfo"));

    adapter->writeVt(QByteArrayLiteral(
        "\033]7;kitty-shell-cwd://00:12:34:56:78:90:999/tmp/raw%20path??#fragment\007"));
    QCOMPARE(adapter->takeDeferredEffects().currentDirectory,
             QStringLiteral("/tmp/raw%20path??#fragment"));

    machineHost = QByteArrayLiteral("replacement-host");
    adapter->writeVt(QByteArrayLiteral(
        "\033]7;file://00:12:34:56:78:90/tmp/stale-host\007"));
    QVERIFY(adapter->takeDeferredEffects().currentDirectory.isNull());

    adapter->writeVt(QByteArrayLiteral(
        "\033]7;file://replacement-host/tmp/current-host\007"));
    QCOMPARE(adapter->takeDeferredEffects().currentDirectory,
             QStringLiteral("/tmp/current-host"));

    machineHost = QByteArrayLiteral("b@localhost");
    adapter->writeVt(QByteArrayLiteral(
        "\033]7;file://a@b@localhost/tmp/first-userinfo-separator\007"));
    QCOMPARE(adapter->takeDeferredEffects().currentDirectory,
             QStringLiteral("/tmp/first-userinfo-separator"));

    machineHost = QByteArray(1, static_cast<char>(0x81));
    adapter->writeVt(QByteArrayLiteral(
        "\033]7;file://%80/tmp/distinct-non-utf8-host\007"));
    QVERIFY(adapter->takeDeferredEffects().currentDirectory.isNull());
    machineHost = QByteArray(1, static_cast<char>(0x80));
    adapter->writeVt(QByteArrayLiteral(
        "\033]7;file://%80/tmp/exact-non-utf8-host\007"));
    QCOMPARE(adapter->takeDeferredEffects().currentDirectory,
             QStringLiteral("/tmp/exact-non-utf8-host"));

    machineHost = QByteArray::fromHex(QByteArrayLiteral("efbfbd"));
    QByteArray rawInvalidHost = QByteArrayLiteral("\033]7;file://");
    rawInvalidHost.append(static_cast<char>(0x80));
    rawInvalidHost += QByteArrayLiteral("/tmp/raw-invalid-host\007");
    adapter->writeVt(rawInvalidHost);
    QVERIFY(adapter->takeDeferredEffects().currentDirectory.isNull());
    machineHost = QByteArray(1, static_cast<char>(0x80));
    adapter->writeVt(rawInvalidHost);
    QCOMPARE(adapter->takeDeferredEffects().currentDirectory,
             QStringLiteral("/tmp/raw-invalid-host"));

    machineHost = QByteArrayLiteral("[::1]");
    adapter->writeVt(QByteArrayLiteral(
        "\033]7;file://[::1]/tmp/bracketed-host\007"));
    QCOMPARE(adapter->takeDeferredEffects().currentDirectory,
             QStringLiteral("/tmp/bracketed-host"));

    adapter->writeVt(QByteArrayLiteral(
        "\033]7;file://[::1]ignored/tmp/bracket-trailing-text\007"));
    QCOMPARE(adapter->takeDeferredEffects().currentDirectory,
             QStringLiteral("/tmp/bracket-trailing-text"));

    machineHost = QByteArrayLiteral("::1");
    adapter->writeVt(QByteArrayLiteral(
        "\033]7;file://[::1]/tmp/unbracketed-mismatch\007"));
    QVERIFY(adapter->takeDeferredEffects().currentDirectory.isNull());

    machineHost = QByteArrayLiteral("ab:cd:ef:ab:cd:ef");
    adapter->writeVt(QByteArrayLiteral(
        "\033]7;file://user@ab:cd:ef:ab:cd:ef/tmp/fallback-userinfo\007"));
    QVERIFY(adapter->takeDeferredEffects().currentDirectory.isNull());

    adapter->writeVt(QByteArrayLiteral(
        "\033]7;file://user@ab:cd:ef:ab:cd:ef:+9_99/tmp/standard-userinfo\007"));
    QCOMPARE(adapter->takeDeferredEffects().currentDirectory,
             QStringLiteral("/tmp/standard-userinfo"));

    adapter->writeVt(QByteArrayLiteral(
        "\033]7;kitty-shell-cwd://ab:cd:ef:ab:cd:ef?raw\007"));
    QVERIFY(adapter->takeDeferredEffects().currentDirectory.isNull());

    adapter->writeVt(QByteArrayLiteral(
        "\033]7;kitty-shell-cwd://ab:cd:ef:ab:cd:ef/tmp/raw?path\007"));
    QCOMPARE(adapter->takeDeferredEffects().currentDirectory,
             QStringLiteral("/tmp/raw?path"));

    adapter->writeVt(QByteArrayLiteral(
        "\033]7;kitty-shell-cwd://ab:cd:ef:ab:cd:ef//ignored/tmp/reparsed?raw\007"));
    QCOMPARE(adapter->takeDeferredEffects().currentDirectory,
             QStringLiteral("/tmp/reparsed?raw"));

    adapter->writeVt(QByteArrayLiteral(
        "\033]7;kitty-shell-cwd://ab:cd:ef:ab:cd:ef//userinfo@/tmp/no-host-authority\007"));
    QCOMPARE(adapter->takeDeferredEffects().currentDirectory,
             QStringLiteral("/tmp/no-host-authority"));

    machineHost = QByteArrayLiteral("00:12:34:56:78:90");
    adapter->writeVt(QByteArrayLiteral(
        "\033]7;file://00:12:34:56:78:9g/tmp/invalid-mac\007"));
    QVERIFY(adapter->takeDeferredEffects().currentDirectory.isNull());
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

void GhosttyVtAdapterTest::snapshotsPhysicalSearchRowsAcrossHistoryAndScreens()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 8;
    options.geometry.rows = 3;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);

    const QByteArray wrappedLine = QStringLiteral(
        "abcde\u0301fg\u754c/path").toUtf8();
    QByteArray output = QByteArrayLiteral("A\033[4GB  \r\n");
    output += wrappedLine;
    output += QByteArrayLiteral(
        "\r\ntail-0\r\ntail-1\r\ntail-2\r\ntail-3");
    adapter->writeVt(output);

    const std::optional<GhosttyVtAdapter::SearchExtent> extent =
        adapter->searchExtent();
    QVERIFY(extent.has_value());
    QCOMPARE(extent->columns, 8);
    QVERIFY(extent->totalRows > static_cast<quint32>(options.geometry.rows));
    QCOMPARE(extent->activeScreen,
             GhosttyVtAdapter::SearchScreen::Primary);
    QVERIFY(!adapter->snapshotSearchRow(extent->totalRows).has_value());

    std::optional<GhosttyVtAdapter::SearchRowSnapshot> gapRow;
    std::optional<GhosttyVtAdapter::SearchRowSnapshot> wrapHead;
    std::optional<GhosttyVtAdapter::SearchRowSnapshot> wrapTail;
    const QByteArray expectedHead = QStringLiteral("abcde\u0301fg").toUtf8();
    const QByteArray expectedTail = QStringLiteral("\u754c/path").toUtf8();
    for (quint32 row = 0; row < extent->totalRows; ++row) {
        auto snapshot = adapter->snapshotSearchRow(row);
        QVERIFY(snapshot.has_value());
        QCOMPARE(snapshot->screenRow, row);
        QCOMPARE(snapshot->text.size(), snapshot->byteCells.size());
        if (snapshot->text == QByteArrayLiteral("A  B")) {
            gapRow = std::move(snapshot);
        } else if (snapshot->text == expectedHead) {
            wrapHead = std::move(snapshot);
        } else if (snapshot->text == expectedTail) {
            wrapTail = std::move(snapshot);
        }
    }

    QVERIFY(gapRow.has_value());
    QVERIFY(!gapRow->wrapped);
    QCOMPARE(gapRow->byteCells,
             QVector<TerminalSearchCell>({
                 {.column = 0, .screenRow = gapRow->screenRow},
                 {.column = 1, .screenRow = gapRow->screenRow},
                 {.column = 2, .screenRow = gapRow->screenRow},
                 {.column = 3, .screenRow = gapRow->screenRow},
             }));
    const TerminalSearchCell expectedGapNewline{
        .column = 3,
        .screenRow = gapRow->screenRow,
    };
    QCOMPARE(gapRow->newlineCell, expectedGapNewline);

    QVERIFY(wrapHead.has_value());
    QVERIFY(wrapHead->wrapped);
    QCOMPARE(wrapHead->byteCells.at(4).column, quint16{4});
    QCOMPARE(wrapHead->byteCells.at(5).column, quint16{4});
    QCOMPARE(wrapHead->byteCells.at(6).column, quint16{4});
    QCOMPARE(wrapHead->newlineCell, wrapHead->byteCells.constLast());

    QVERIFY(wrapTail.has_value());
    QVERIFY(!wrapTail->wrapped);
    const QByteArray wide = QStringLiteral("\u754c").toUtf8();
    for (qsizetype index = 0; index < wide.size(); ++index) {
        QCOMPARE(wrapTail->byteCells.at(index).column, quint16{0});
    }
    QCOMPARE(wrapTail->byteCells.at(wide.size()).column, quint16{2});

    // A soft wrap can follow a row whose final cells are spaces. Those bytes
    // are part of the logical search text and must not be trimmed like the
    // trailing spaces at a hard line ending.
    auto spacedWrapAdapter = GhosttyVtAdapter::create(options);
    QVERIFY(spacedWrapAdapter != nullptr);
    spacedWrapAdapter->writeVt(QByteArrayLiteral("abc     Z"));
    const auto spacedWrapHead = spacedWrapAdapter->snapshotSearchRow(0);
    QVERIFY(spacedWrapHead.has_value());
    QVERIFY(spacedWrapHead->wrapped);
    QCOMPARE(spacedWrapHead->text, QByteArrayLiteral("abc     "));
    QCOMPARE(spacedWrapHead->byteCells.size(), qsizetype{8});
    for (quint16 column = 0; column < 8; ++column) {
        const TerminalSearchCell expected{
            .column = column,
            .screenRow = 0,
        };
        QCOMPARE(spacedWrapHead->byteCells.at(column), expected);
    }
    QCOMPARE(spacedWrapHead->newlineCell,
             spacedWrapHead->byteCells.constLast());
    const auto spacedWrapTail = spacedWrapAdapter->snapshotSearchRow(1);
    QVERIFY(spacedWrapTail.has_value());
    QVERIFY(!spacedWrapTail->wrapped);
    QCOMPARE(spacedWrapTail->text, QByteArrayLiteral("Z"));

    adapter->writeVt(QByteArrayLiteral("\033[?1049h\033[H\033[2JALT"));
    const auto alternateExtent = adapter->searchExtent();
    QVERIFY(alternateExtent.has_value());
    QCOMPARE(alternateExtent->activeScreen,
             GhosttyVtAdapter::SearchScreen::Alternate);
    QCOMPARE(alternateExtent->totalRows,
             static_cast<quint32>(options.geometry.rows));
    bool foundAlternateText = false;
    for (quint32 row = 0; row < alternateExtent->totalRows; ++row) {
        const auto alternateRow = adapter->snapshotSearchRow(row);
        QVERIFY(alternateRow.has_value());
        foundAlternateText = foundAlternateText
            || alternateRow->text == QByteArrayLiteral("ALT");
    }
    QVERIFY(foundAlternateText);
}

void GhosttyVtAdapterTest::searchSnapshotsFollowLiveDeccolmDimensions()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 80;
    options.geometry.rows = 3;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);

    // DEC mode 40 opts into DECCOLM; mode 3 then resizes libghostty's grid
    // internally without going through the frontend resize path.
    adapter->writeVt(QByteArrayLiteral("\033[?40h\033[?3hwide-grid"));
    const auto extent = adapter->searchExtent();
    QVERIFY(extent.has_value());
    QCOMPARE(extent->columns, 132);
    QCOMPARE(extent->rows, 3);

    const auto row = adapter->snapshotSearchRow(0);
    QVERIFY(row.has_value());
    QCOMPARE(row->text, QByteArrayLiteral("wide-grid"));
    QCOMPARE(row->byteCells.size(), row->text.size());
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

void GhosttyVtAdapterTest::installsTrackedTextRangesAsSelections()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 48;
    options.geometry.rows = 2;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);

    const QByteArray line =
        QByteArrayLiteral("prefix https://example.test/path suffix");
    const QByteArray matched = QByteArrayLiteral("https://example.test/path");
    const qsizetype begin = line.indexOf(matched);
    QVERIFY(begin >= 0);
    const qsizetype end = begin + matched.size();
    adapter->writeVt(line);

    auto snapshot =
        adapter->snapshotLogicalLineAt(static_cast<int>(begin + 8), 0);
    QVERIFY(snapshot.has_value());
    auto tracked = adapter->trackTextRange(*snapshot, begin, end);
    QVERIFY(tracked.has_value());

    QVERIFY(adapter->installTextRange(*tracked));
    QCOMPARE(adapter->selectedText(false), QString::fromUtf8(matched));
    QVERIFY(adapter->selectionContains(static_cast<int>(begin), 0));
    QVERIFY(adapter->selectionContains(static_cast<int>(end - 1), 0));
    QVERIFY(!adapter->selectionContains(static_cast<int>(begin - 1), 0));
    QVERIFY(!adapter->selectionContains(static_cast<int>(end), 0));

    auto foreign = GhosttyVtAdapter::create(options);
    QVERIFY(foreign != nullptr);
    foreign->writeVt(line);
    QVERIFY(!foreign->installTextRange(*tracked));
    QVERIFY(!foreign->hasSelection());

    adapter->clearSelection();
    adapter->writeVt(QByteArrayLiteral("\033[1;8HX"));
    QVERIFY(!adapter->trackedTextRangeValid(*tracked));
    QVERIFY(!adapter->installTextRange(*tracked));
    QVERIFY(!adapter->hasSelection());
}

void GhosttyVtAdapterTest::translatesCellStylesAndAppearanceMetadata()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 16;
    options.geometry.rows = 2;
    const QColor globalBackground(QStringLiteral("#1e222a"));
    options.appearance.backgroundColor = globalBackground;
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
        "\033[0;9;53mK"
        "\033[0;48;2;30;34;42mB"
        "\033[0mD"
        "\033[48;2;1;2;3m\033[K"
        "\033[0m"));

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
    QVERIFY(!inverse.backgroundExplicit);
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

    const TerminalCell &matchingExplicitBackground = frame.cells.at(11);
    QCOMPARE(matchingExplicitBackground.text, QStringLiteral("B"));
    QVERIFY(matchingExplicitBackground.backgroundExplicit);
    QCOMPARE(matchingExplicitBackground.background, globalBackground);
    QCOMPARE(matchingExplicitBackground.background, frame.background);

    const TerminalCell &defaultBackground = frame.cells.at(12);
    QCOMPARE(defaultBackground.text, QStringLiteral("D"));
    QVERIFY(!defaultBackground.backgroundExplicit);
    QCOMPARE(defaultBackground.background, frame.background);

    // Erasing with a background pen uses Ghostty's compact bg-color content
    // cell rather than a styled text cell. The public resolved-color query
    // deliberately treats both representations as explicit.
    const TerminalCell &contentBackground = frame.cells.at(13);
    QVERIFY(contentBackground.text.isEmpty());
    QVERIFY(contentBackground.backgroundExplicit);
    QCOMPARE(contentBackground.background, QColor::fromRgb(1, 2, 3));
}

void GhosttyVtAdapterTest::preservesAuthoritativeCellCodepointsForShaping()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 10;
    options.geometry.rows = 2;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);
    adapter->writeVt(QStringLiteral("fi e\u0301 \U0001f600").toUtf8());

    GhosttyVtAdapter::RenderSnapshot snapshot;
    QCOMPARE(adapter->renderFrame(&snapshot),
             GhosttyVtAdapter::RenderResult::Ready);
    const TerminalFrame frame = applyUpdate(snapshot.update);

    QCOMPARE(frame.cells.at(0).text, QStringLiteral("f"));
    QCOMPARE(frame.cells.at(0).baseCodepoint, quint32{U'f'});
    QVERIFY(frame.cells.at(0).plainCodepoint);
    QCOMPARE(frame.cells.at(1).baseCodepoint, quint32{U'i'});
    QVERIFY(frame.cells.at(1).plainCodepoint);
    QCOMPARE(frame.cells.at(3).text, QStringLiteral("e\u0301"));
    QCOMPARE(frame.cells.at(3).baseCodepoint, quint32{U'e'});
    QVERIFY(!frame.cells.at(3).plainCodepoint);
    QVERIFY(frame.cells.at(3).extendedGrapheme);
    QCOMPARE(frame.cells.at(5).text, QStringLiteral("\U0001f600"));
    QCOMPARE(frame.cells.at(5).baseCodepoint, quint32{0x1f600});
    QVERIFY(frame.cells.at(5).plainCodepoint);
    QVERIFY(!frame.cells.at(5).extendedGrapheme);
    QVERIFY(frame.cells.at(6).spacer);
    QVERIFY(!frame.cells.at(6).plainCodepoint);
    QVERIFY(!frame.cells.at(6).extendedGrapheme);
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
    QVERIFY(applyTerminalUpdate(frame, snapshot.update));
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
    QVERIFY(applyTerminalUpdate(frame, snapshot.update));
    QCOMPARE(frame.palette.at(1), QColor(QStringLiteral("#00bb00")));
    QCOMPARE(frame.cursorColor, QColor(QStringLiteral("#abcdef")));
    QCOMPARE(frame.cursorStyle, 2);
    QVERIFY(frame.cursorBlinking);

    // Reset sequences reveal the newest configured defaults, not the defaults
    // that were active when the application override was installed.
    adapter->writeVt(QByteArrayLiteral("\033]104;1\007\033]112\007"));
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(applyTerminalUpdate(frame, snapshot.update));
    QCOMPARE(frame.palette.at(1), QColor(QStringLiteral("#0000cc")));
    QCOMPARE(frame.cursorColor, QColor(QStringLiteral("#fedcba")));

    // An explicit DECSCUSR request remains active across config reloads; CSI
    // 0 q returns to the latest configured style and blink state.
    adapter->writeVt(QByteArrayLiteral("\033[2 q"));
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(applyTerminalUpdate(frame, snapshot.update));
    QCOMPARE(frame.cursorStyle, 1);
    QVERIFY(!frame.cursorBlinking);
    reloaded.cursorStyle = TerminalCursorStyle::Bar;
    QVERIFY(adapter->setAppearance(reloaded));
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(applyTerminalUpdate(frame, snapshot.update));
    QCOMPARE(frame.cursorStyle, 1);
    adapter->writeVt(QByteArrayLiteral("\033[0 q"));
    QCOMPARE(adapter->renderFrame(&snapshot), GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(applyTerminalUpdate(frame, snapshot.update));
    QCOMPARE(frame.cursorStyle, 0);
    QVERIFY(frame.cursorBlinking);
}

void GhosttyVtAdapterTest::queriesKeyboardActionMode()
{
    auto adapter = GhosttyVtAdapter::create({});
    QVERIFY(adapter != nullptr);
    QVERIFY(!adapter->keyboardActionMode());

    adapter->writeVt(QByteArrayLiteral("\033[2h"));
    QVERIFY(adapter->keyboardActionMode());

    adapter->writeVt(QByteArrayLiteral("\033[2l"));
    QVERIFY(!adapter->keyboardActionMode());

    adapter->writeVt(QByteArrayLiteral("\033[2h"));
    QVERIFY(adapter->keyboardActionMode());
    adapter->reset();
    QVERIFY(!adapter->keyboardActionMode());
}

void GhosttyVtAdapterTest::marksMinimumContrastExemptGlyphs()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 80;
    options.geometry.rows = 2;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);

    struct Sample {
        char32_t codepoint;
        bool exempt;
    };
    constexpr std::array samples{
        Sample{U'A', false},   Sample{0x24ff, false},  Sample{0x2500, true},
        Sample{0x257f, true},  Sample{0x2580, true},   Sample{0x259f, true},
        Sample{0x25a0, false}, Sample{0x1faff, false}, Sample{0x1fb00, true},
        Sample{0x1fbff, true}, Sample{0x1fc00, false}, Sample{0x1cbff, false},
        Sample{0x1cc00, true}, Sample{0x1cebf, true},  Sample{0x1cec0, false},
        Sample{0xe0af, false}, Sample{0xe0b0, true},   Sample{0xe0d7, true},
        Sample{0xe0d8, false},
    };

    QString text;
    for (const Sample &sample : samples) {
        text.append(QString::fromUcs4(&sample.codepoint, 1));
    }
    adapter->writeVt(text.toUtf8());

    TerminalFrame frame;
    renderInto(adapter.get(), &frame);
    qsizetype sampleIndex = 0;
    for (const TerminalCell &cell : frame.cells) {
        if (cell.text.isEmpty()) {
            continue;
        }
        QVERIFY(sampleIndex < static_cast<qsizetype>(samples.size()));
        QCOMPARE(cell.minimumContrastExemptGlyph,
                 samples[static_cast<size_t>(sampleIndex)].exempt);
        ++sampleIndex;
    }
    QCOMPARE(sampleIndex, static_cast<qsizetype>(samples.size()));
}

void GhosttyVtAdapterTest::encodesUsingTerminalModes()
{
    auto adapter = GhosttyVtAdapter::create({});
    QVERIFY(adapter != nullptr);
    QVERIFY(!adapter->mouseTracking());

    adapter->writeVt(QByteArrayLiteral(
        "\033[?2004h\033[?1004h\033[?1002h"));
    adapter->synchronizeInputModes();
    QVERIFY(adapter->mouseTracking());
    QCOMPARE(adapter->encodePaste(QStringLiteral("one\ntwo")),
             QByteArrayLiteral("\033[200~one\ntwo\033[201~"));
    QCOMPARE(adapter->encodeFocus(true), QByteArrayLiteral("\033[I"));
    QCOMPARE(adapter->encodeFocus(false), QByteArrayLiteral("\033[O"));

    TerminalKeyInput input;
    input.key = Qt::Key_A;
    input.text = QStringLiteral("a");
    const GhosttyVtAdapter::EncodedKey encodedA = adapter->encodeKey(input);
    QCOMPARE(encodedA.bytes, QByteArrayLiteral("a"));
    QVERIFY(!encodedA.modifier);
    QVERIFY(!encodedA.escape);

    // Physical location comes from Qt's Linux XKB scan code, even when the
    // logical key/modifier tuple does not identify a keypad key. Kitty's
    // disambiguation mode makes the physical identity observable without
    // relying on synthetic text that Qt would normally supply.
    adapter->writeVt(QByteArrayLiteral("\033[>1u"));
    TerminalKeyInput keypad;
    keypad.key = Qt::Key_Left;
    keypad.nativeScanCode = KEY_KP1 + 8U;
    QCOMPARE(adapter->encodeKey(keypad).bytes,
             QByteArrayLiteral("\033[57400u"));

    // Classification follows the physical key that the encoder actually
    // receives, even when the logical Qt key says something else.
    adapter->writeVt(QByteArrayLiteral("\033[>11u"));
    TerminalKeyInput physicalShift;
    physicalShift.key = Qt::Key_A;
    physicalShift.nativeScanCode = KEY_LEFTSHIFT + 8U;
    const GhosttyVtAdapter::EncodedKey encodedShift =
        adapter->encodeKey(physicalShift);
    QVERIFY(!encodedShift.bytes.isEmpty());
    QVERIFY(encodedShift.modifier);
    QVERIFY(!encodedShift.escape);

    TerminalKeyInput physicalEscape;
    physicalEscape.key = Qt::Key_A;
    physicalEscape.nativeScanCode = KEY_ESC + 8U;
    const GhosttyVtAdapter::EncodedKey encodedEscape =
        adapter->encodeKey(physicalEscape);
    QVERIFY(!encodedEscape.bytes.isEmpty());
    QVERIFY(!encodedEscape.modifier);
    QVERIFY(encodedEscape.escape);

    TerminalKeyInput releasedA = input;
    releasedA.nativeScanCode = KEY_A + 8U;
    releasedA.pressed = false;
    const GhosttyVtAdapter::EncodedKey encodedRelease =
        adapter->encodeKey(releasedA);
    QVERIFY(!encodedRelease.bytes.isEmpty());
    QVERIFY(!encodedRelease.modifier);
    QVERIFY(!encodedRelease.escape);

    adapter->writeVt(QByteArrayLiteral("\033[?1002l"));
    QVERIFY(!adapter->mouseTracking());
}

void GhosttyVtAdapterTest::preparesPasteUsingExactSafetyPolicy()
{
    auto adapter = GhosttyVtAdapter::create({});
    QVERIFY(adapter != nullptr);

    const auto prepare = [&adapter](const QString &text,
                                    bool protection = true,
                                    bool bracketedSafe = true,
                                    bool confirmed = false) {
        return adapter->preparePaste(text, {
            .protection = protection,
            .bracketedSafe = bracketedSafe,
            .authorization = confirmed
                ? GhosttyVtAdapter::PasteAuthorization::Confirmed
                : GhosttyVtAdapter::PasteAuthorization::Initial,
        });
    };
    const auto expectReady = [](const GhosttyVtAdapter::PreparedPaste &paste,
                                const QByteArray &bytes) {
        QCOMPARE(paste.disposition,
                 GhosttyVtAdapter::PasteDisposition::Ready);
        QCOMPARE(paste.bytes, bytes);
    };
    const auto expectConfirmation = [](
        const GhosttyVtAdapter::PreparedPaste &paste) {
        QCOMPARE(paste.disposition,
                 GhosttyVtAdapter::PasteDisposition::ConfirmationRequired);
        QVERIFY(paste.bytes.isEmpty());
    };

    expectReady(prepare(QString{}), {});
    expectReady(prepare(QStringLiteral("plain")), QByteArrayLiteral("plain"));
    expectReady(prepare(QStringLiteral("one\rtwo")),
                QByteArrayLiteral("one\rtwo"));
    expectConfirmation(prepare(QStringLiteral("one\ntwo")));

    const QString fence = QStringLiteral("one\x1b[201~two");
    expectConfirmation(prepare(fence));
    expectReady(prepare(QStringLiteral("one\ntwo"), true, true, true),
                QByteArrayLiteral("one\rtwo"));
    expectReady(prepare(fence, false), QByteArrayLiteral("one [201~two"));

    const QString arbitraryEscape = QStringLiteral("one\x1b[31mtwo");
    expectReady(prepare(arbitraryEscape), QByteArrayLiteral("one [31mtwo"));

    adapter->writeVt(QByteArrayLiteral("\033[?2004h"));
    expectReady(prepare(QStringLiteral("one\ntwo")),
                QByteArrayLiteral("\033[200~one\ntwo\033[201~"));
    expectConfirmation(prepare(QStringLiteral("one\ntwo"), true, false));
    expectConfirmation(prepare(fence));
    expectReady(prepare(fence, false),
                QByteArrayLiteral("\033[200~one [201~two\033[201~"));
    expectReady(prepare(fence, true, true, true),
                QByteArrayLiteral("\033[200~one [201~two\033[201~"));
}

void GhosttyVtAdapterTest::clearsSelectionWithoutCancellingGesture()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 16;
    options.geometry.rows = 2;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);
    adapter->writeVt(QByteArrayLiteral("selection-target"));

    adapter->beginSelection(selectionPress(options.geometry, 0, 0));
    QVERIFY(adapter->updateSelection(selectionDrag(options.geometry, 5, 0)));
    QVERIFY(adapter->hasSelection());

    adapter->clearSelection();
    QVERIFY(!adapter->hasSelection());

    // Ghostty's setSelection(null) keeps the active drag anchor. A later
    // motion in that same gesture can establish a new installed range.
    QVERIFY(adapter->updateSelection(selectionDrag(options.geometry, 8, 0)));
    QVERIFY(adapter->hasSelection());

    // Reported physical buttons clear the installed range and release the
    // gesture's tracked grid reference. A later drag cannot resurrect it.
    adapter->clearSelectionAndResetGesture();
    QVERIFY(!adapter->hasSelection());
    QVERIFY(!adapter->updateSelection(selectionDrag(options.geometry, 10, 0)));
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
        "\033]0;reset-title\a"
        "\033]7;file://localhost/tmp/reset-cwd\a\033[c"));
    adapter->synchronizeInputModes();
    QVERIFY(adapter->selectAll());
    QVERIFY(adapter->hasSelection());

    TerminalFrame frame;
    GhosttyVtAdapter::RenderSnapshot before;
    QCOMPARE(adapter->renderFrame(&before),
             GhosttyVtAdapter::RenderResult::Ready);
    QVERIFY(applyTerminalUpdate(frame, before.update));
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
    // Upstream reset publishes no apprt title update, so the frontend keeps
    // the last base title regardless of which path published it.
    QVERIFY(resetEffects.title.isNull());
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
    QVERIFY(applyTerminalUpdate(frame, after.update));
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

    adapter->beginSelection(selectionPress(options.geometry, 0, 0));
    QVERIFY(adapter->updateSelection(selectionDrag(options.geometry, 5, 0)));
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
    adapter->beginSelection(selectionPress(options.geometry, 4, 2));
    QVERIFY(adapter->updateSelection(selectionDrag(options.geometry, 1, 0)));
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

void GhosttyVtAdapterTest::mapsAndRevealsSearchRanges()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 8;
    options.geometry.rows = 3;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);
    adapter->writeVt(QByteArrayLiteral(
        "row-0\r\nrow-1\r\nrow-2\r\nrow-3\r\n"
        "row-4\r\nrow-5\r\nrow-6\r\nrow-7"));

    const auto extent = adapter->searchExtent();
    QVERIFY(extent.has_value());
    QCOMPARE(extent->totalRows, quint32{8});

    const TerminalSearchRange topRange{
        .start = {.column = 1, .screenRow = 0},
        .end = {.column = 3, .screenRow = 0},
    };
    QVERIFY(adapter->visibleCellsForSearchRange(topRange).isEmpty());
    QVERIFY(adapter->scrollSearchRangeIntoView(topRange));
    QCOMPARE(adapter->visibleCellsForSearchRange(topRange),
             QVector<QPoint>({QPoint(1, 0), QPoint(2, 0), QPoint(3, 0)}));
    QVERIFY(!adapter->scrollSearchRangeIntoView(topRange));

    // Reversed endpoints are normalized before clipping to the viewport.
    const TerminalSearchRange reversed{
        .start = {.column = 6, .screenRow = 1},
        .end = {.column = 2, .screenRow = 0},
    };
    QVector<QPoint> expected;
    for (int column = 2; column < options.geometry.columns; ++column) {
        expected.append(QPoint(column, 0));
    }
    for (int column = 0; column <= 6; ++column) {
        expected.append(QPoint(column, 1));
    }
    QCOMPARE(adapter->visibleCellsForSearchRange(reversed), expected);

    // A result below the viewport is pinned by its start row. Row four has
    // enough following content that libghostty does not need to clamp the
    // viewport against the bottom of history.
    const TerminalSearchRange belowRange{
        .start = {.column = 2, .screenRow = 4},
        .end = {.column = 4, .screenRow = 4},
    };
    QVERIFY(adapter->visibleCellsForSearchRange(belowRange).isEmpty());
    QVERIFY(adapter->scrollSearchRangeIntoView(belowRange));
    const auto revealedExtent = adapter->searchExtent();
    QVERIFY(revealedExtent.has_value());
    QCOMPARE(revealedExtent->viewportOffset, quint64{4});
    QCOMPARE(adapter->visibleCellsForSearchRange(belowRange),
             QVector<QPoint>({QPoint(2, 0), QPoint(3, 0), QPoint(4, 0)}));

    QVERIFY(adapter->scrollViewport({
        .kind = TerminalViewportRequest::Kind::Row,
        .row = 3,
    }));
    const TerminalSearchRange overlap{
        .start = {.column = 6, .screenRow = 2},
        .end = {.column = 1, .screenRow = 3},
    };
    QVERIFY(!adapter->scrollSearchRangeIntoView(overlap));
    QCOMPARE(adapter->visibleCellsForSearchRange(overlap),
             QVector<QPoint>({QPoint(0, 0), QPoint(1, 0)}));

    const TerminalSearchRange bottomRange{
        .start = {.column = 0, .screenRow = 7},
        .end = {.column = 2, .screenRow = 7},
    };
    QVERIFY(adapter->scrollSearchRangeIntoView(bottomRange));
    QCOMPARE(adapter->visibleCellsForSearchRange(bottomRange),
             QVector<QPoint>({QPoint(0, 2), QPoint(1, 2), QPoint(2, 2)}));

    const TerminalSearchRange invalid{
        .start = {.column = 8, .screenRow = 0},
        .end = {.column = 8, .screenRow = 0},
    };
    QVERIFY(adapter->visibleCellsForSearchRange(invalid).isEmpty());
    QVERIFY(!adapter->scrollSearchRangeIntoView(invalid));
}

void GhosttyVtAdapterTest::formatsSelectionWithConfigurableTrimming()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 10;
    options.geometry.rows = 2;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);
    adapter->writeVt(QByteArrayLiteral("abc   "));

    adapter->beginSelection(selectionPress(options.geometry, 0, 0,
                                           1'000 * nanosecondsPerMillisecond));
    QVERIFY(adapter->updateSelection(selectionDrag(options.geometry, 6, 0)));
    adapter->endSelection(6, 0);
    QCOMPARE(adapter->selectedText(), QStringLiteral("abc"));
    QCOMPARE(adapter->selectedText(true), QStringLiteral("abc"));
    QCOMPARE(adapter->selectedText(false), QStringLiteral("abc   "));

    // A repeat press that expands to no value does not clear an existing
    // selection. Establish the repeat anchor on an empty cell, then install an
    // unrelated range without resetting the gesture.
    adapter->clearSelectionAndResetGesture();
    QVERIFY(!adapter->beginSelection(selectionPress(
        options.geometry, 0, 1, 2'000 * nanosecondsPerMillisecond)));
    adapter->endSelection(0, 1);
    QVERIFY(adapter->selectAll());
    QVERIFY(!adapter->beginSelection(selectionPress(
        options.geometry, 0, 1, 2'001 * nanosecondsPerMillisecond)));
    QVERIFY(adapter->hasSelection());

    // A fresh single press clears the installed selection while retaining
    // the new drag anchor maintained by Ghostty's gesture state.
    QVERIFY(adapter->beginSelection(selectionPress(
        options.geometry, 1, 0, 3'000 * nanosecondsPerMillisecond)));
    QVERIFY(!adapter->hasSelection());
    QVERIFY(adapter->updateSelection(selectionDrag(options.geometry, 3, 0)));
    adapter->endSelection(3, 0);
    QVERIFY(adapter->hasSelection());
}

void GhosttyVtAdapterTest::selectsCellsWordsAndQueriesSelectionContainment()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 24;
    options.geometry.rows = 3;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);
    adapter->writeVt(QByteArrayLiteral("alpha;beta gamma\r\ncell target"));

    QVERIFY(!adapter->selectionContains(0, 0));
    QVERIFY(!adapter->selectionContains(-1, 0));
    QVERIFY(!adapter->selectionContains(0, -1));
    QVERIFY(!adapter->selectionContains(options.geometry.columns, 0));
    QVERIFY(!adapter->selectionContains(0, options.geometry.rows));
    QVERIFY(!adapter->selectionContains(std::numeric_limits<int>::max(),
                                        std::numeric_limits<int>::max()));

    QVERIFY(adapter->selectWord(7, 0));
    QCOMPARE(adapter->selectedText(false), QStringLiteral("beta"));
    QVERIFY(adapter->selectionContains(7, 0));
    QVERIFY(!adapter->selectionContains(5, 0));
    QVERIFY(!adapter->selectionContains(10, 0));

    const QVector<uint32_t> spaceOnly{0, uint32_t{' '}};
    QVERIFY(adapter->setSelectionWordChars(spaceOnly));
    QVERIFY(adapter->selectWord(7, 0));
    QCOMPARE(adapter->selectedText(false), QStringLiteral("alpha;beta"));
    QVERIFY(adapter->selectionContains(0, 0));
    QVERIFY(adapter->selectionContains(9, 0));
    QVERIFY(!adapter->selectionContains(10, 0));

    QVERIFY(adapter->selectCell(1, 1));
    QCOMPARE(adapter->selectedText(false), QStringLiteral("e"));
    QVERIFY(adapter->selectionContains(1, 1));
    QVERIFY(!adapter->selectionContains(0, 1));
    QVERIFY(!adapter->selectionContains(2, 1));
    QVERIFY(!adapter->selectCell(-1, 1));
    QVERIFY(!adapter->selectCell(options.geometry.columns, 1));

    adapter->clearSelection();
    QVERIFY(!adapter->selectWord(0, 2));
    QVERIFY(!adapter->hasSelection());
}

void GhosttyVtAdapterTest::classifiesRepeatedSelectionPresses()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 20;
    options.geometry.rows = 6;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);
    adapter->writeVt(QByteArrayLiteral("alpha beta\r\n"
                                       "second line\r\n"
                                       "third line"));

    const auto press = [&adapter, &options](int column, int row,
                                            quint64 timestampNanoseconds,
                                            bool controlModifier = false) {
        return adapter->beginSelection(selectionPress(options.geometry, column,
                                                      row, timestampNanoseconds,
                                                      controlModifier));
    };

    TerminalSelectionPressInput invalidPosition =
        selectionPress(options.geometry, 1, 0, 1);
    invalidPosition.surfaceX = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(!adapter->beginSelection(invalidPosition));
    invalidPosition.surfaceX = 1.0;
    invalidPosition.surfaceY = std::numeric_limits<double>::infinity();
    QVERIFY(!adapter->beginSelection(invalidPosition));

    QVERIFY(adapter->setClickRepeatIntervalMilliseconds(100));
    QVERIFY(!press(1, 0, 1'000 * nanosecondsPerMillisecond));
    QVERIFY(!adapter->hasSelection());
    adapter->endSelection(1, 0);

    // Both the time and physical-distance boundaries are inclusive. Moving
    // exactly one cell between the first two presses still selects the word.
    QVERIFY(press(2, 0, 1'100 * nanosecondsPerMillisecond));
    QCOMPARE(adapter->selectedText(), QStringLiteral("alpha"));
    adapter->endSelection(2, 0);

    // Distance remains anchored at the original press rather than following
    // the previous press. Another one-cell move is therefore too far and
    // starts a new single-click gesture, clearing the installed word.
    QVERIFY(press(3, 0, 1'100 * nanosecondsPerMillisecond));
    QVERIFY(!adapter->hasSelection());
    adapter->endSelection(3, 0);

    adapter->clearSelectionAndResetGesture();
    QVERIFY(!press(1, 0, 1'500 * nanosecondsPerMillisecond));
    adapter->endSelection(1, 0);
    TerminalSelectionPressInput beyondRadius = selectionPress(
        options.geometry, 2, 0, 1'501 * nanosecondsPerMillisecond);
    beyondRadius.surfaceX += 0.001;
    QVERIFY(!adapter->beginSelection(beyondRadius));
    QVERIFY(!adapter->hasSelection());
    adapter->endSelection(2, 0);

    adapter->clearSelectionAndResetGesture();
    QVERIFY(!press(1, 0, 2'000 * nanosecondsPerMillisecond));
    adapter->endSelection(1, 0);
    QVERIFY(press(1, 0, 2'001 * nanosecondsPerMillisecond));
    QCOMPARE(adapter->selectedText(), QStringLiteral("alpha"));
    adapter->endSelection(1, 0);
    QVERIFY(press(1, 0, 2'002 * nanosecondsPerMillisecond));
    QCOMPARE(adapter->selectedText(), QStringLiteral("alpha beta"));
    adapter->endSelection(1, 0);

    // Further repeats clamp at triple-click rather than wrapping back to a
    // single click.
    QVERIFY(press(1, 0, 2'003 * nanosecondsPerMillisecond));
    QCOMPARE(adapter->selectedText(), QStringLiteral("alpha beta"));

    // A live interval update applies to the next press without resetting the
    // current gesture.
    adapter->clearSelectionAndResetGesture();
    QVERIFY(adapter->setClickRepeatIntervalMilliseconds(100));
    QVERIFY(!press(1, 0, 3'000 * nanosecondsPerMillisecond));
    adapter->endSelection(1, 0);
    QVERIFY(adapter->setClickRepeatIntervalMilliseconds(99));
    QVERIFY(!press(1, 0, 3'100 * nanosecondsPerMillisecond));
    QVERIFY(!adapter->hasSelection());
    adapter->endSelection(1, 0);
    QVERIFY(adapter->setClickRepeatIntervalMilliseconds(100));
    QVERIFY(press(1, 0, 3'200 * nanosecondsPerMillisecond));
    QCOMPARE(adapter->selectedText(), QStringLiteral("alpha"));

    // Invalid direct updates are rejected without disturbing the last valid
    // interval. The full u32 range remains usable and inclusive.
    QVERIFY(!adapter->setClickRepeatIntervalMilliseconds(0));
    QVERIFY(adapter->setClickRepeatIntervalMilliseconds(
        std::numeric_limits<quint32>::max()));
    adapter->clearSelectionAndResetGesture();
    constexpr quint64 maxIntervalNanoseconds =
        static_cast<quint64>(std::numeric_limits<quint32>::max())
        * nanosecondsPerMillisecond;
    QVERIFY(!press(1, 0, 1));
    adapter->endSelection(1, 0);
    QVERIFY(press(1, 0, 1 + maxIntervalNanoseconds));
    QCOMPARE(adapter->selectedText(), QStringLiteral("alpha"));

    QVERIFY(adapter->setClickRepeatIntervalMilliseconds(100));
    adapter->clearSelectionAndResetGesture();
    QVERIFY(!press(1, 0, 4'000 * nanosecondsPerMillisecond));
    adapter->endSelection(1, 0);
    QVERIFY(!press(1, 0, 4'101 * nanosecondsPerMillisecond));
    QVERIFY(!adapter->hasSelection());

    adapter->clearSelectionAndResetGesture();
    QVERIFY(!press(1, 0, 5'000 * nanosecondsPerMillisecond));
    adapter->endSelection(1, 0);
    QVERIFY(!press(1, 0, 4'999 * nanosecondsPerMillisecond));
    QVERIFY(!adapter->hasSelection());

    adapter->clearSelectionAndResetGesture();
    QVERIFY(!press(1, 0, 6'000 * nanosecondsPerMillisecond));
    adapter->endSelection(1, 0);
    QVERIFY(!adapter->beginSelection(selectionPress(options.geometry, 1, 0)));
    QVERIFY(!adapter->hasSelection());
    adapter->endSelection(1, 0);
    QVERIFY(!press(1, 0, 6'001 * nanosecondsPerMillisecond));
    QVERIFY(!adapter->hasSelection());

    // Switching active screens changes the tracked screen generation, so a
    // nearby press cannot continue the primary-screen click sequence.
    adapter->clearSelectionAndResetGesture();
    QVERIFY(!press(1, 0, 7'000 * nanosecondsPerMillisecond));
    adapter->endSelection(1, 0);
    adapter->writeVt(QByteArrayLiteral("\033[?1049h\033[Halternate"));
    QVERIFY(!press(1, 0, 7'001 * nanosecondsPerMillisecond));
    QVERIFY(!adapter->hasSelection());
    adapter->endSelection(1, 0);
    QVERIFY(press(1, 0, 7'002 * nanosecondsPerMillisecond));
    QCOMPARE(adapter->selectedText(), QStringLiteral("alternate"));

    GhosttyVtAdapter::Options semanticOptions;
    semanticOptions.geometry.columns = 12;
    semanticOptions.geometry.rows = 6;
    auto semantic = GhosttyVtAdapter::create(semanticOptions);
    QVERIFY(semantic != nullptr);
    semantic->writeVt(QByteArrayLiteral("out-a\r\n"
                                        "out-b"
                                        "\033]133;A\a"
                                        "$ \033]133;B\acmd\033]133;C\a\r\n"
                                        "out-c"));

    QVERIFY(!semantic->beginSelection(
        selectionPress(semanticOptions.geometry, 1, 0,
                       8'000 * nanosecondsPerMillisecond, true)));
    semantic->endSelection(1, 0);
    QVERIFY(semantic->beginSelection(
        selectionPress(semanticOptions.geometry, 1, 0,
                       8'001 * nanosecondsPerMillisecond, true)));
    semantic->endSelection(1, 0);
    QVERIFY(semantic->beginSelection(
        selectionPress(semanticOptions.geometry, 1, 0,
                       8'002 * nanosecondsPerMillisecond, true)));
    QCOMPARE(semantic->selectedText(), QStringLiteral("out-a\nout-b"));
}

void GhosttyVtAdapterTest::extendsSelectionOnDelayedShiftPress()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 6;
    options.geometry.rows = 3;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);
    adapter->writeVt(QByteArrayLiteral("abcdef\r\n"
                                       "uvwxyz"));
    QVERIFY(adapter->setClickRepeatIntervalMilliseconds(100));

    const auto establishCellSelection = [&adapter, &options](
                                            quint64 timestampNanoseconds) {
        adapter->clearSelectionAndResetGesture();
        QVERIFY(!adapter->beginSelection(
            selectionPress(options.geometry, 1, 0, timestampNanoseconds)));
        QVERIFY(
            adapter->updateSelection(selectionDrag(options.geometry, 3, 0)));
        adapter->endSelection(3, 0);
        QCOMPARE(adapter->selectedText(false), QStringLiteral("bc"));
    };
    const auto shiftPress =
        [&adapter, &options](int column, int row,
                             std::optional<quint64> timestampNanoseconds,
                             bool rectangular = false) {
            return adapter->beginSelection(
                selectionPress(options.geometry, column, row,
                               timestampNanoseconds, false, true, rectangular));
        };

    constexpr quint64 first = 1'000 * nanosecondsPerMillisecond;
    constexpr quint64 interval = 100 * nanosecondsPerMillisecond;

    // The repeat boundary is inclusive, so an exact-boundary Shift press
    // follows the ordinary press path. This distant press becomes a new
    // anchor and clears the old installed range.
    establishCellSelection(first);
    QVERIFY(shiftPress(4, 0, first + interval));
    QVERIFY(!adapter->hasSelection());

    // One nanosecond later, the same press is a drag from the retained anchor.
    establishCellSelection(first);
    QVERIFY(shiftPress(4, 0, first + interval + 1));
    QCOMPARE(adapter->selectedText(false), QStringLiteral("bcd"));
    QVERIFY(adapter->selectionGestureDragged());

    // Extension does not replace the original press timestamp or anchor.
    // This second extension remains delayed relative to the original press
    // even though it closely follows the first extension.
    QVERIFY(shiftPress(5, 0, first + interval + 2));
    QCOMPARE(adapter->selectedText(false), QStringLiteral("bcde"));

    // Without comparable monotonic time, conservatively use an ordinary
    // press. Both an untimed press and a reversed timestamp re-anchor.
    establishCellSelection(2'000 * nanosecondsPerMillisecond);
    QVERIFY(shiftPress(4, 0, std::nullopt));
    QVERIFY(!adapter->hasSelection());

    establishCellSelection(3'000 * nanosecondsPerMillisecond);
    QVERIFY(shiftPress(4, 0, 2'999 * nanosecondsPerMillisecond));
    QVERIFY(!adapter->hasSelection());

    // A gesture alone is insufficient: if the installed selection was
    // cleared without resetting repeat history, delayed Shift starts a new
    // ordinary gesture at the clicked cell.
    establishCellSelection(4'000 * nanosecondsPerMillisecond);
    adapter->clearSelection();
    QVERIFY(!shiftPress(4, 0, 4'101 * nanosecondsPerMillisecond));
    QVERIFY(adapter->updateSelection(selectionDrag(options.geometry, 5, 0)));
    QCOMPARE(adapter->selectedText(false), QStringLiteral("e"));

    // The pane maps Linux Ctrl+Alt to rectangle mode on the delayed press.
    establishCellSelection(5'000 * nanosecondsPerMillisecond);
    QVERIFY(shiftPress(4, 1, 5'101 * nanosecondsPerMillisecond, true));
    QCOMPARE(adapter->selectedText(false), QStringLiteral("bcd\nvwx"));

    // A valid same-anchor drag has no range and therefore collapses the old
    // installed selection rather than leaving stale highlighted text.
    establishCellSelection(6'000 * nanosecondsPerMillisecond);
    QVERIFY(shiftPress(1, 0, 6'101 * nanosecondsPerMillisecond));
    QVERIFY(!adapter->hasSelection());

    // Every ordinary press replaces the mirrored time. Although this Shift
    // press is delayed relative to the first click, it remains inside the
    // interval measured from the intervening double-click press.
    establishCellSelection(7'000 * nanosecondsPerMillisecond);
    QVERIFY(adapter->beginSelection(selectionPress(
        options.geometry, 1, 0, 7'080 * nanosecondsPerMillisecond)));
    adapter->endSelection(1, 0);
    QCOMPARE(adapter->selectedText(false), QStringLiteral("abcdef"));
    QVERIFY(shiftPress(4, 0, 7'150 * nanosecondsPerMillisecond));
    QVERIFY(!adapter->hasSelection());

    // An anchor on an inactive screen is different: Ghostty consumes the
    // extension without clearing that screen's independent selection or
    // falling through to a new press.
    establishCellSelection(8'000 * nanosecondsPerMillisecond);
    adapter->writeVt(QByteArrayLiteral("\033[?1049h\033[Halternate"));
    QVERIFY(adapter->selectCell(0, 0));
    QVERIFY(!shiftPress(4, 0, 8'101 * nanosecondsPerMillisecond));
    QCOMPARE(adapter->selectedText(false), QStringLiteral("a"));

    // A gesture reset also discards the mirrored frontend timestamp, so a
    // later candidate cannot resurrect the old tracked anchor.
    adapter->clearSelectionAndResetGesture();
    QVERIFY(!shiftPress(5, 1, 6'000 * nanosecondsPerMillisecond));
    QVERIFY(!adapter->hasSelection());
}

void GhosttyVtAdapterTest::appliesConfiguredWordBoundariesToPressAndDrag()
{
    GhosttyVtAdapter::Options options;
    options.geometry.columns = 24;
    options.geometry.rows = 3;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);
    adapter->writeVt(QStringLiteral("alpha;beta gamma\r\n"
                                    "snow\u2603flake\r\n"
                                    "one\U0001F680two")
                         .toUtf8());

    // Ghostty's default boundary set includes semicolon.
    QVERIFY(beginFreshDoubleClick(adapter.get(), options.geometry, 7, 0));
    QCOMPARE(adapter->selectedText(false), QStringLiteral("beta"));

    // A finalized custom set can make punctuation part of a word. The drag
    // operation reads its own current boundary list, so live reload can
    // change both the original and current endpoint expansion.
    QVector<uint32_t> spaceOnly{0, uint32_t{' '}};
    QVERIFY(adapter->setSelectionWordChars(spaceOnly));
    spaceOnly[1] = uint32_t{';'};
    QVERIFY(beginFreshDoubleClick(adapter.get(), options.geometry, 7, 0));
    QCOMPARE(adapter->selectedText(false), QStringLiteral("alpha;beta"));

    // Reject invalid direct API input without disturbing either reusable
    // gesture event's previously copied boundary list.
    QVERIFY(!adapter->setSelectionWordChars(
        QVector<uint32_t>{0, uint32_t{0x110000}}));
    QVERIFY(beginFreshDoubleClick(adapter.get(), options.geometry, 7, 0));
    QCOMPARE(adapter->selectedText(false), QStringLiteral("alpha;beta"));

    const QVector<uint32_t> spaceAndSemicolon{0, uint32_t{' '}, uint32_t{';'}};
    QVERIFY(adapter->setSelectionWordChars(spaceAndSemicolon));
    QVERIFY(adapter->updateSelection(selectionDrag(options.geometry, 12, 0)));
    QCOMPARE(adapter->selectedText(false), QStringLiteral("beta gamma"));
    adapter->endSelection(12, 0);

    const QVector<uint32_t> snowmanBoundary{0, 0x2603};
    QVERIFY(adapter->setSelectionWordChars(snowmanBoundary));
    QVERIFY(beginFreshDoubleClick(adapter.get(), options.geometry, 6, 1));
    QCOMPARE(adapter->selectedText(false), QStringLiteral("flake"));

    // Keep non-BMP values as Unicode scalars rather than UTF-16 code units.
    const QVector<uint32_t> rocketBoundary{0, 0x1F680};
    QVERIFY(adapter->setSelectionWordChars(rocketBoundary));
    QVERIFY(beginFreshDoubleClick(adapter.get(), options.geometry, 6, 2));
    QCOMPARE(adapter->selectedText(false), QStringLiteral("two"));

    QVERIFY(adapter->setSelectionWordChars(QVector<uint32_t>{}));
    QVERIFY(beginFreshDoubleClick(adapter.get(), options.geometry, 7, 0));
    QCOMPARE(adapter->selectedText(false), QStringLiteral("beta"));
}

void GhosttyVtAdapterTest::snapshotsPlainWriteFileRanges()
{
    using Status = GhosttyVtAdapter::PlainFileSnapshotStatus;

    GhosttyVtAdapter::Options options;
    options.geometry.columns = 12;
    options.geometry.rows = 3;
    auto adapter = GhosttyVtAdapter::create(options);
    QVERIFY(adapter != nullptr);

    const auto emptyScreen =
        adapter->snapshotPlainFile(TerminalFileLocation::Screen);
    QCOMPARE(emptyScreen.status, Status::Ready);
    QCOMPARE(emptyScreen.bytes, QByteArray{});
    QCOMPARE(adapter->snapshotPlainFile(
                 TerminalFileLocation::Scrollback).status,
             Status::Unavailable);
    QCOMPARE(adapter->snapshotPlainFile(
                 TerminalFileLocation::Selection).status,
             Status::Unavailable);

    adapter->writeVt(QByteArrayLiteral(
        "history-0  \r\n"
        "history-1\r\n"
        "screen-0  \r\n"
        "screen-1\r\n"
        "screen-2  "));

    const auto screen =
        adapter->snapshotPlainFile(TerminalFileLocation::Screen);
    QCOMPARE(screen.status, Status::Ready);
    QCOMPARE(screen.bytes, QByteArrayLiteral(
        "history-0  \n"
        "history-1\n"
        "screen-0  \n"
        "screen-1\n"
        "screen-2  "));

    const auto scrollback =
        adapter->snapshotPlainFile(TerminalFileLocation::Scrollback);
    QCOMPARE(scrollback.status, Status::Ready);
    QCOMPARE(scrollback.bytes, QByteArrayLiteral(
        "history-0  \n"
        "history-1"));

    adapter->beginSelection(selectionPress(options.geometry, 0, 2));
    QVERIFY(adapter->updateSelection(selectionDrag(options.geometry, 8, 2)));
    adapter->endSelection(8, 2);
    const auto selection =
        adapter->snapshotPlainFile(TerminalFileLocation::Selection);
    QCOMPARE(selection.status, Status::Ready);
    QCOMPARE(selection.bytes, QByteArrayLiteral("screen-2"));

    adapter->clearSelection();
    QCOMPARE(adapter->snapshotPlainFile(
                 TerminalFileLocation::Selection).status,
             Status::Unavailable);

    GhosttyVtAdapter::Options rectangleOptions;
    rectangleOptions.geometry.columns = 6;
    rectangleOptions.geometry.rows = 3;
    auto rectangle = GhosttyVtAdapter::create(rectangleOptions);
    QVERIFY(rectangle != nullptr);
    rectangle->writeVt(QByteArrayLiteral(
        "abcdef\r\n"
        "uvwxyz"));
    rectangle->beginSelection(selectionPress(rectangleOptions.geometry, 1, 0));
    QVERIFY(rectangle->updateSelection(
        selectionDrag(rectangleOptions.geometry, 4, 1, true)));
    rectangle->endSelection(4, 1);
    const auto rectangularSelection =
        rectangle->snapshotPlainFile(TerminalFileLocation::Selection);
    QCOMPARE(rectangularSelection.status, Status::Ready);
    QCOMPARE(rectangularSelection.bytes, QByteArrayLiteral(
        "bcd\n"
        "vwx"));
}

void GhosttyVtAdapterTest::
    snapshotsPlainWriteFileFormattingAndAlternateScreen()
{
    using Status = GhosttyVtAdapter::PlainFileSnapshotStatus;

    GhosttyVtAdapter::Options formattingOptions;
    formattingOptions.geometry.columns = 6;
    formattingOptions.geometry.rows = 4;
    auto formatting = GhosttyVtAdapter::create(formattingOptions);
    QVERIFY(formatting != nullptr);
    formatting->writeVt(QByteArrayLiteral(
        "abc  \r\n"
        "ABCDEFGHI\r\n"
        "last"));

    const auto formatted =
        formatting->snapshotPlainFile(TerminalFileLocation::Screen);
    QCOMPARE(formatted.status, Status::Ready);
    QCOMPARE(formatted.bytes, QByteArrayLiteral(
        "abc  \n"
        "ABCDEFGHI\n"
        "last"));

    GhosttyVtAdapter::Options screenOptions;
    screenOptions.geometry.columns = 12;
    screenOptions.geometry.rows = 3;
    auto screen = GhosttyVtAdapter::create(screenOptions);
    QVERIFY(screen != nullptr);
    screen->writeVt(QByteArrayLiteral(
        "primary-0\r\n"
        "primary-1\r\n"
        "primary-2\r\n"
        "primary-3"));

    const auto primary =
        screen->snapshotPlainFile(TerminalFileLocation::Screen);
    QCOMPARE(primary.status, Status::Ready);
    QCOMPARE(primary.bytes, QByteArrayLiteral(
        "primary-0\n"
        "primary-1\n"
        "primary-2\n"
        "primary-3"));
    const auto primaryScrollback =
        screen->snapshotPlainFile(TerminalFileLocation::Scrollback);
    QCOMPARE(primaryScrollback.status, Status::Ready);
    QCOMPARE(primaryScrollback.bytes, QByteArrayLiteral("primary-0"));

    screen->writeVt(QByteArrayLiteral(
        "\033[?1049h"
        "\033[H\033[2J"
        "alternate"));
    const auto alternate =
        screen->snapshotPlainFile(TerminalFileLocation::Screen);
    QCOMPARE(alternate.status, Status::Ready);
    QCOMPARE(alternate.bytes, QByteArrayLiteral("alternate"));
    QCOMPARE(screen->snapshotPlainFile(
                 TerminalFileLocation::Scrollback).status,
             Status::Unavailable);

    screen->writeVt(QByteArrayLiteral("\033[?1049l"));
    const auto restored =
        screen->snapshotPlainFile(TerminalFileLocation::Screen);
    QCOMPARE(restored.status, Status::Ready);
    QCOMPARE(restored.bytes, primary.bytes);
    const auto restoredScrollback =
        screen->snapshotPlainFile(TerminalFileLocation::Scrollback);
    QCOMPARE(restoredScrollback.status, Status::Ready);
    QCOMPARE(restoredScrollback.bytes, QByteArrayLiteral("primary-0"));
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
    adapter->beginSelection(selectionPress(options.geometry, 4, 2));
    QVERIFY(adapter->updateSelection(selectionDrag(options.geometry, 4, 0)));
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

        adapter->beginSelection(selectionPress(options.geometry, 0, 1));
        QVERIFY(
            adapter->updateSelection(selectionDrag(options.geometry, 2, 1)));
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
        adapter->beginSelection(selectionPress(options.geometry, 0, 0));
        if (!adapter->updateSelection(selectionDrag(options.geometry, 2, 0))) {
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
