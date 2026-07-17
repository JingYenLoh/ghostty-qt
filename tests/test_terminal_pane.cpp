#include "launch_options.h"
#include "terminal_controller.h"
#include "terminal_pane.h"
#include "terminal_types.h"

#include <QColor>
#include <QDir>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QImage>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <array>
#include <utility>

namespace {

QString frameText(const TerminalFrame &frame)
{
    QString text;
    text.reserve(frame.cells.size());
    for (const TerminalCell &cell : frame.cells) {
        text.append(cell.text);
    }
    return text;
}

bool updatesContain(const QSignalSpy &spy, const QString &needle)
{
    TerminalFrame frame;
    for (const QList<QVariant> &arguments : spy) {
        applyTerminalUpdate(
            &frame, qvariant_cast<TerminalUpdate>(arguments.constFirst()));
    }
    return frameText(frame).contains(needle);
}

bool approximatelyEqual(const QColor &left, const QColor &right)
{
    constexpr int tolerance = 2;
    return std::abs(left.red() - right.red()) <= tolerance
        && std::abs(left.green() - right.green()) <= tolerance
        && std::abs(left.blue() - right.blue()) <= tolerance;
}

} // namespace

class TerminalPaneTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void replacesStartingFrameInsteadOfAccumulatingSceneRoots();
    void reloadsFontWithoutOverwritingManualZoom();
    void rendersConfiguredCellCursorAndDecorationAppearance();
    void routesEmergencyTabShortcuts();
    void routesConfiguredBindingsAndDisablesEmergencyFallback();
    void routesViewportAndSelectionActions();
    void routesTerminalControlActions();
    void resetClearsTerminalMetadataAndSplitInheritance();
    void routesStructuredSequencesAndCancelsThemOnReload();
    void replaysInvalidStructuredSequenceThroughPty();
    void routesNamedKeyTablesAndClearsThemOnReload();
};

void TerminalPaneTest::replacesStartingFrameInsteadOfAccumulatingSceneRoots()
{
    qRegisterMetaType<TerminalUpdate>();

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "sleep 0.2; "
            "printf '\\033[2J\\033[H\\033[10;10Hscene-root-marker'; "
            "sleep 3"),
    };
    options.hold = true;

    QQuickWindow window;
    const QColor background(QStringLiteral("#1e222a"));
    window.setColor(background);
    window.resize(640, 384);

    auto *pane = new TerminalPane(options, window.contentItem());
    pane->setSize(window.size());
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updateSpy(controller, &TerminalController::terminalUpdated);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("scene-root-marker")), 5000);

    // Allow multiple scene-graph updates, including a cursor blink, before
    // grabbing the rendered pane. A retained initial root would still draw
    // "Starting terminal…" in this otherwise cleared region.
    QTest::qWait(700);
    const QImage image = window.grabWindow();
    QVERIFY(!image.isNull());

    const qreal scale = static_cast<qreal>(image.width()) / window.width();
    const QRect startingTextRegion(
        qRound(8.0 * scale), qRound(8.0 * scale),
        qRound(220.0 * scale), qRound(40.0 * scale));
    int unexpectedPixels = 0;
    for (int y = startingTextRegion.top(); y < startingTextRegion.bottom(); ++y) {
        for (int x = startingTextRegion.left(); x < startingTextRegion.right(); ++x) {
            if (!approximatelyEqual(image.pixelColor(x, y), background)) {
                ++unexpectedPixels;
            }
        }
    }
    QCOMPARE(unexpectedPixels, 0);

    int renderedPixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (!approximatelyEqual(image.pixelColor(x, y), background)) {
                ++renderedPixels;
            }
        }
    }
    QVERIFY(renderedPixels > 20);

    window.close();
    delete pane;
}

void TerminalPaneTest::reloadsFontWithoutOverwritingManualZoom()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.fontSize = 12.0;

    TerminalPane pane(options);
    LaunchOptions reloaded = options;
    reloaded.fontSize = 14.0;
    reloaded.fontFamily = QStringLiteral("Monospace");
    pane.applyRuntimeOptions(reloaded);
    QCOMPARE(pane.fontPointSize(), 14.0);
    QCOMPARE(pane.splitLaunchOptions().fontFamily, QStringLiteral("Monospace"));

    pane.zoomIn();
    QCOMPARE(pane.fontPointSize(), 15.0);
    reloaded.fontSize = 10.0;
    pane.applyRuntimeOptions(reloaded);
    QCOMPARE(pane.fontPointSize(), 15.0);
    QVERIFY(pane.splitLaunchOptions().fontSizeManuallyAdjusted);

    pane.resetZoom();
    QCOMPARE(pane.fontPointSize(), 10.0);
    QVERIFY(!pane.splitLaunchOptions().fontSizeManuallyAdjusted);
}

void TerminalPaneTest::rendersConfiguredCellCursorAndDecorationAppearance()
{
    qRegisterMetaType<TerminalUpdate>();

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("sleep 5"),
    };
    options.hold = true;
    options.fontFamily = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    options.appearance.foregroundColor = Qt::white;
    options.appearance.backgroundColor = Qt::black;
    options.appearance.palette.fill(Qt::black, 256);
    options.appearance.palette[8] = QColor(QStringLiteral("#00ff00"));
    options.appearance.boldColor.kind = TerminalBoldColorKind::Bright;
    options.appearance.faintOpacity = 0.25;
    options.appearance.selectionBackground.kind = TerminalColorKind::CellForeground;
    options.appearance.selectionForeground.kind = TerminalColorKind::CellBackground;
    options.appearance.cursorColor = TerminalColorValue::fromColor(
        QColor(QStringLiteral("#ff00ff")));
    options.appearance.cursorTextColor = TerminalColorValue::fromColor(
        QColor(QStringLiteral("#00ffff")));
    options.appearance.cursorOpacity = 0.5;

    QFont testFont(options.fontFamily);
    testFont.setPointSizeF(options.fontSize);
    testFont.setFixedPitch(true);
    testFont.setStyleHint(QFont::Monospace);
    const QFontMetricsF metrics(testFont);
    const qreal cellWidth = std::max(
        1.0, std::ceil(metrics.horizontalAdvance(QLatin1Char('M'))));
    const qreal cellHeight = std::max(1.0, std::ceil(metrics.height()));

    constexpr int columns = 12;
    // Leave enough room for a possible process-status overlay at the bottom;
    // appearance samples stay in the first two rows.
    constexpr int rows = 4;
    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(qCeil(cellWidth * columns), qCeil(cellHeight * rows));
    auto *pane = new TerminalPane(options, window.contentItem());
    pane->setSize(window.size());
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    pane->forceActiveFocus();
    QTRY_VERIFY_WITH_TIMEOUT(pane->hasActiveFocus(), 1000);
    QTest::qWait(150);

    TerminalUpdate update;
    update.columns = columns;
    update.rows = rows;
    update.fullFrame = true;
    update.colorsChanged = true;
    update.foreground = Qt::white;
    update.background = Qt::black;
    update.cursorColor = QColor(QStringLiteral("#ff00ff"));
    update.cursorColorExplicit = true;
    update.palette = options.appearance.palette;
    update.cursorChanged = true;
    update.cursorVisible = true;
    update.cursorBlinking = true;
    update.cursorColumn = 3;
    update.cursorRow = 0;
    update.cursorStyle = 1;
    update.cursorColumnSpan = 2;
    for (int row = 0; row < rows; ++row) {
        TerminalRowUpdate rowUpdate;
        rowUpdate.row = row;
        rowUpdate.cells.resize(columns);
        for (TerminalCell &cell : rowUpdate.cells) {
            cell.foreground = Qt::white;
            cell.background = Qt::black;
            cell.underlineColor = Qt::white;
        }
        update.dirtyRows.append(std::move(rowUpdate));
    }

    TerminalCell &selection = update.dirtyRows[0].cells[0];
    selection.foreground = QColor(QStringLiteral("#ff0000"));
    selection.background = QColor(QStringLiteral("#0000ff"));
    selection.selected = true;

    TerminalCell &bold = update.dirtyRows[0].cells[1];
    bold.text = QString(QChar(0x2588));
    bold.foreground = QColor(QStringLiteral("#800000"));
    bold.bold = true;
    bold.styleForegroundSource = TerminalColorSource::Palette;
    bold.styleForegroundPaletteIndex = 0;

    TerminalCell &faint = update.dirtyRows[0].cells[2];
    faint.text = QString(QChar(0x2588));
    faint.faint = true;

    TerminalCell &cursorHead = update.dirtyRows[0].cells[3];
    cursorHead.text = QStringLiteral("I");
    cursorHead.columnSpan = 2;
    cursorHead.faint = true;
    cursorHead.underlineStyle = TerminalUnderlineStyle::Single;
    cursorHead.underlineUsesForeground = false;
    cursorHead.underlineColor = Qt::red;
    cursorHead.strikeThrough = true;
    cursorHead.overline = true;
    TerminalCell &cursorTail = update.dirtyRows[0].cells[4];
    cursorTail.spacer = true;
    cursorTail.underlineStyle = TerminalUnderlineStyle::Single;
    cursorTail.underlineUsesForeground = false;
    cursorTail.underlineColor = Qt::red;
    cursorTail.strikeThrough = true;
    cursorTail.overline = true;

    const std::array<TerminalUnderlineStyle, 6> underlines{
        TerminalUnderlineStyle::None,
        TerminalUnderlineStyle::Single,
        TerminalUnderlineStyle::Double,
        TerminalUnderlineStyle::Curly,
        TerminalUnderlineStyle::Dotted,
        TerminalUnderlineStyle::Dashed,
    };
    for (int i = 0; i < static_cast<int>(underlines.size()); ++i) {
        TerminalCell &cell = update.dirtyRows[0].cells[6 + i];
        cell.underlineStyle = underlines[static_cast<size_t>(i)];
        cell.underlineUsesForeground = false;
    }

    TerminalCell &invisible = update.dirtyRows[1].cells[0];
    invisible.underlineStyle = TerminalUnderlineStyle::Single;
    invisible.underlineUsesForeground = false;
    invisible.invisible = true;
    TerminalCell &retainedBlink = update.dirtyRows[1].cells[1];
    retainedBlink.text = QString(QChar(0x2588));
    retainedBlink.foreground = QColor(QStringLiteral("#ffff00"));
    retainedBlink.textBlink = true;

    controller->terminalUpdated(update);
    // The synthetic frame was delivered synchronously. Keep a later PTY
    // readiness update from replacing it while the software scene graph is
    // being sampled.
    QObject::disconnect(controller, &TerminalController::terminalUpdated,
                        pane, nullptr);
    QTest::qWait(100);
    const QImage image = window.grabWindow();
    QVERIFY(!image.isNull());

    const qreal xScale = static_cast<qreal>(image.width()) / window.width();
    const qreal yScale = static_cast<qreal>(image.height()) / window.height();
    const auto centerColor = [&](int column) {
        return image.pixelColor(
            qBound(0, qRound((column + 0.5) * cellWidth * xScale), image.width() - 1),
            qBound(0, qRound(0.5 * cellHeight * yScale), image.height() - 1));
    };

    const QColor selectionPixel = centerColor(0);
    QVERIFY2(approximatelyEqual(selectionPixel, QColor(QStringLiteral("#ff0000"))),
             qPrintable(QStringLiteral("selection pixel=%1 image=%2x%3 cell=%4x%5")
                            .arg(selectionPixel.name(QColor::HexArgb))
                            .arg(image.width()).arg(image.height())
                            .arg(cellWidth).arg(cellHeight)));
    QVERIFY(approximatelyEqual(centerColor(1), QColor(QStringLiteral("#00ff00"))));
    const QColor faintPixel = centerColor(2);
    QVERIFY(faintPixel.red() >= 55 && faintPixel.red() <= 75);
    QVERIFY(faintPixel.green() >= 55 && faintPixel.green() <= 75);
    QVERIFY(faintPixel.blue() >= 55 && faintPixel.blue() <= 75);
    bool foundCursorBackground = false;
    bool foundCursorText = false;
    const int cursorLeft = qRound(3.0 * cellWidth * xScale);
    const int cursorRight = qRound(5.0 * cellWidth * xScale);
    for (int y = 0; y < qRound(cellHeight * yScale); ++y) {
        for (int x = cursorLeft; x < cursorRight; ++x) {
            const QColor pixel = image.pixelColor(x, y);
            foundCursorBackground = foundCursorBackground
                || (pixel.red() >= 120 && pixel.red() <= 140
                    && pixel.green() <= 10
                    && pixel.blue() >= 120 && pixel.blue() <= 140);
            foundCursorText = foundCursorText
                || (pixel.red() <= 10 && pixel.green() >= 245
                    && pixel.blue() >= 245);
        }
    }
    QVERIFY(foundCursorBackground);
    QVERIFY(foundCursorText);
    bool foundWideCursorDecoration = false;
    for (int y = 0; y < qRound(cellHeight * yScale); ++y) {
        for (int x = qRound(4.0 * cellWidth * xScale);
             x < cursorRight; ++x) {
            const QColor pixel = image.pixelColor(x, y);
            foundWideCursorDecoration = foundWideCursorDecoration
                || (pixel.red() <= 10 && pixel.green() >= 245
                    && pixel.blue() >= 245);
            QVERIFY(!(pixel.red() >= 245 && pixel.green() <= 10
                      && pixel.blue() <= 10));
        }
    }
    QVERIFY(foundWideCursorDecoration);

    std::array<int, 6> decorationPixels{};
    std::array<QSet<int>, 6> decorationRows;
    for (int style = 0; style < 6; ++style) {
        const int left = qRound((6 + style) * cellWidth * xScale);
        const int right = qRound((7 + style) * cellWidth * xScale);
        for (int y = 0; y < qRound(cellHeight * yScale); ++y) {
            for (int x = left; x < right; ++x) {
                if (!approximatelyEqual(image.pixelColor(x, y), Qt::black)) {
                    ++decorationPixels[static_cast<size_t>(style)];
                    decorationRows[static_cast<size_t>(style)].insert(y);
                }
            }
        }
    }
    QCOMPARE(decorationPixels[0], 0);
    QVERIFY(decorationPixels[1] > 0);
    QVERIFY(decorationPixels[2] > decorationPixels[1]);
    QVERIFY(decorationRows[3].size() >= 2);
    QVERIFY(decorationPixels[4] > 0);
    QVERIFY(decorationPixels[4] < decorationPixels[1]);
    QVERIFY(decorationPixels[5] > decorationPixels[4]);
    QVERIFY(decorationPixels[5] < decorationPixels[1]);

    const auto secondRowCenterColor = [&](int column, const QImage &source) {
        return source.pixelColor(
            qBound(0, qRound((column + 0.5) * cellWidth * xScale),
                   source.width() - 1),
            qBound(0, qRound(1.5 * cellHeight * yScale),
                   source.height() - 1));
    };
    int invisiblePixels = 0;
    for (int y = qRound(cellHeight * yScale);
         y < qRound(2.0 * cellHeight * yScale); ++y) {
        for (int x = 0; x < qRound(cellWidth * xScale); ++x) {
            if (!approximatelyEqual(image.pixelColor(x, y), Qt::black)) {
                ++invisiblePixels;
            }
        }
    }
    QCOMPARE(invisiblePixels, 0);
    QVERIFY(approximatelyEqual(secondRowCenterColor(1, image),
                               QColor(QStringLiteral("#ffff00"))));
    // Cross the 600 ms cursor-blink cadence: SGR text blink remains stable,
    // matching the pinned Ghostty renderer rather than sharing that timer.
    QTest::qWait(650);
    const QImage laterImage = window.grabWindow();
    QVERIFY(!laterImage.isNull());
    QVERIFY(approximatelyEqual(secondRowCenterColor(1, laterImage),
                               QColor(QStringLiteral("#ffff00"))));

    const auto cursorContains = [&](const QImage &source,
                                    const auto &predicate) {
        for (int y = 0; y < qRound(cellHeight * yScale); ++y) {
            for (int x = cursorLeft; x < cursorRight; ++x) {
                if (predicate(source.pixelColor(x, y))) {
                    return true;
                }
            }
        }
        return false;
    };
    QVERIFY(!cursorContains(laterImage, [](const QColor &pixel) {
        return pixel.red() >= 110 && pixel.green() <= 20
            && pixel.blue() >= 110;
    }));

    // Losing focus stops the blink timer and immediately presents Ghostty's
    // opaque hollow cursor. Regaining focus shows the configured cursor at
    // once and restarts its interval.
    QQuickItem focusTarget(window.contentItem());
    focusTarget.setFocusPolicy(Qt::StrongFocus);
    focusTarget.forceActiveFocus();
    QTRY_VERIFY_WITH_TIMEOUT(!pane->hasActiveFocus(), 1000);
    QTest::qWait(50);
    const QImage unfocusedImage = window.grabWindow();
    QVERIFY(cursorContains(unfocusedImage, [](const QColor &pixel) {
        return pixel.red() >= 245 && pixel.green() <= 10
            && pixel.blue() >= 245;
    }));

    pane->forceActiveFocus();
    QTRY_VERIFY_WITH_TIMEOUT(pane->hasActiveFocus(), 1000);
    QTest::qWait(50);
    const QImage refocusedImage = window.grabWindow();
    QVERIFY(cursorContains(refocusedImage, [](const QColor &pixel) {
        return pixel.red() >= 120 && pixel.red() <= 140
            && pixel.green() <= 10
            && pixel.blue() >= 120 && pixel.blue() <= 140;
    }));

    window.close();
    delete pane;
}

void TerminalPaneTest::routesEmergencyTabShortcuts()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;

    TerminalPane pane(options);
    QSignalSpy tabChange(&pane, &TerminalPane::requestTabChange);

    QKeyEvent next(QEvent::KeyPress, Qt::Key_Tab,
                   Qt::ControlModifier, QStringLiteral("\t"));
    QCoreApplication::sendEvent(&pane, &next);
    QCOMPARE(tabChange.count(), 1);
    QCOMPARE(tabChange.constLast().constFirst().toInt(), 1);

    QKeyEvent previous(QEvent::KeyPress, Qt::Key_Backtab,
                       Qt::ControlModifier | Qt::ShiftModifier,
                       QStringLiteral("\t"));
    QCoreApplication::sendEvent(&pane, &previous);
    QCOMPARE(tabChange.count(), 2);
    QCOMPARE(tabChange.constLast().constFirst().toInt(), -1);
}

void TerminalPaneTest::routesConfiguredBindingsAndDisablesEmergencyFallback()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindingsConfigured = true;
    options.keybindings = {
        QStringLiteral("alt+n=new_tab"),
        QStringLiteral("ctrl+x=increase_font_size:2.5"),
        QStringLiteral("ctrl+w=close_tab:this"),
        QStringLiteral("ctrl+r=reload_config"),
        QStringLiteral("alt+f4=close_window"),
        QStringLiteral("ctrl+y=open_config"),
        QStringLiteral("ctrl+b=copy_to_clipboard:mixed"),
        QStringLiteral("unconsumed:ctrl+l=reload_config"),
        QStringLiteral("unconsumed:ctrl+i=ignore"),
        QStringLiteral("performable:ctrl+g=goto_split:left"),
        QStringLiteral("performable:ctrl+j=open_config"),
        QStringLiteral("chain=new_tab"),
        QStringLiteral("ctrl+k=close_tab:this"),
        QStringLiteral("chain=new_tab"),
        QStringLiteral("ctrl+o=quit"),
        QStringLiteral("chain=ignore"),
        QStringLiteral("performable:ctrl+c=copy_to_clipboard:mixed"),
    };

    TerminalPane pane(options);
    QSignalSpy newTab(&pane, &TerminalPane::requestNewTab);
    QSignalSpy closeTab(&pane, &TerminalPane::requestCloseTab);
    QSignalSpy reload(&pane, &TerminalPane::requestConfigReload);
    QSignalSpy quit(&pane, &TerminalPane::requestQuit);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);

    QKeyEvent configuredNewTab(QEvent::KeyPress, Qt::Key_N,
                               Qt::AltModifier, QStringLiteral("n"));
    QCoreApplication::sendEvent(&pane, &configuredNewTab);
    QCOMPARE(newTab.count(), 1);

    // Once the flattened Ghostty set is available, an absent binding must
    // not fall through to the emergency hard-coded shortcuts: it may have
    // been explicitly unbound by the user's configuration.
    QKeyEvent unboundEmergency(
        QEvent::KeyPress, Qt::Key_T,
        Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("T"));
    QCoreApplication::sendEvent(&pane, &unboundEmergency);
    QCOMPARE(newTab.count(), 1);

    QKeyEvent zoom(QEvent::KeyPress, Qt::Key_X,
                   Qt::ControlModifier, QStringLiteral("x"));
    QCoreApplication::sendEvent(&pane, &zoom);
    QCOMPARE(pane.fontPointSize(), 14.5);

    QKeyEvent close(QEvent::KeyPress, Qt::Key_W,
                    Qt::ControlModifier, QStringLiteral("w"));
    QCoreApplication::sendEvent(&pane, &close);
    QCOMPARE(closeTab.count(), 1);

    QKeyEvent reloadEvent(QEvent::KeyPress, Qt::Key_R,
                          Qt::ControlModifier, QStringLiteral("r"));
    QCoreApplication::sendEvent(&pane, &reloadEvent);
    QCOMPARE(reload.count(), 1);

    QKeyEvent closeWindow(QEvent::KeyPress, Qt::Key_F4,
                          Qt::AltModifier);
    QCoreApplication::sendEvent(&pane, &closeWindow);
    QCOMPARE(quit.count(), 1);

    const int beforeUnsupported = forwarded.count();
    QKeyEvent unsupported(QEvent::KeyPress, Qt::Key_Y,
                          Qt::ControlModifier, QString(QChar(0x19)));
    QCoreApplication::sendEvent(&pane, &unsupported);
    QCOMPARE(forwarded.count(), beforeUnsupported);

    // A normal consumed binding suppresses terminal input even when its
    // state-dependent action has nothing to copy.
    QKeyEvent consumedEmptyCopy(QEvent::KeyPress, Qt::Key_B,
                                Qt::ControlModifier, QString(QChar(0x02)));
    QCoreApplication::sendEvent(&pane, &consumedEmptyCopy);
    QCOMPARE(forwarded.count(), beforeUnsupported);

    // Unconsumed actions still run, then allow normal VT encoding.
    const int beforeUnconsumedReload = forwarded.count();
    QKeyEvent unconsumedReload(QEvent::KeyPress, Qt::Key_L,
                               Qt::ControlModifier, QString(QChar(0x0c)));
    QCoreApplication::sendEvent(&pane, &unconsumedReload);
    QCOMPARE(reload.count(), 2);
    QCOMPARE(forwarded.count(), beforeUnconsumedReload + 1);

    // Ghostty's ignore action always suppresses encoding, even if the
    // binding itself carries the unconsumed flag.
    const int beforeIgnore = forwarded.count();
    QKeyEvent ignored(QEvent::KeyPress, Qt::Key_I,
                      Qt::ControlModifier, QString(QChar(0x09)));
    QCoreApplication::sendEvent(&pane, &ignored);
    QCOMPARE(forwarded.count(), beforeIgnore);

    // A performable chain succeeds when any supported action succeeds; one
    // unsupported action must not suppress the supported remainder.
    QKeyEvent partialChain(QEvent::KeyPress, Qt::Key_J,
                           Qt::ControlModifier, QString(QChar(0x0a)));
    QCoreApplication::sendEvent(&pane, &partialChain);
    QCOMPARE(newTab.count(), 2);

    // Ghostty executes the complete chain before reporting a surface-closing
    // outcome. Workspace removal is deferred, so the later action is safe.
    QKeyEvent closingChain(QEvent::KeyPress, Qt::Key_K,
                           Qt::ControlModifier, QString(QChar(0x0b)));
    QCoreApplication::sendEvent(&pane, &closingChain);
    QCOMPARE(closeTab.count(), 2);
    QCOMPARE(newTab.count(), 3);

    // `quit` is not one of Ghostty's closing-surface actions. A following
    // ignore still runs and therefore leaves the release unsuppressed.
    const int beforeQuitChain = forwarded.count();
    QKeyEvent quitChain(QEvent::KeyPress, Qt::Key_O,
                        Qt::ControlModifier, QString(QChar(0x0f)));
    QCoreApplication::sendEvent(&pane, &quitChain);
    QCOMPARE(quit.count(), 2);
    QCOMPARE(forwarded.count(), beforeQuitChain);
    QKeyEvent quitRelease(QEvent::KeyRelease, Qt::Key_O,
                          Qt::ControlModifier, QString(QChar(0x0f)));
    QCoreApplication::sendEvent(&pane, &quitRelease);
    QCOMPARE(forwarded.count(), beforeQuitChain + 1);

    // A performable copy without a selection is not performed and therefore
    // falls through to terminal input.
    const int beforeEmptyCopy = forwarded.count();
    QKeyEvent emptyCopy(QEvent::KeyPress, Qt::Key_C,
                        Qt::ControlModifier, QString(QChar(0x03)));
    QCoreApplication::sendEvent(&pane, &emptyCopy);
    QCOMPARE(forwarded.count(), beforeEmptyCopy + 1);

    // Performability comes from the typed workspace result, not merely from
    // emitting an action request. With no pane to the left, the binding acts
    // as absent and reaches the terminal.
    pane.setWorkspaceActionHandler([](WorkspaceActionRequest request) {
        return request.action != WorkspaceAction::NavigatePane;
    });
    const int beforeUnavailableNavigation = forwarded.count();
    QKeyEvent unavailableNavigation(QEvent::KeyPress, Qt::Key_G,
                                    Qt::ControlModifier, QString(QChar(0x07)));
    QCoreApplication::sendEvent(&pane, &unavailableNavigation);
    QCOMPARE(forwarded.count(), beforeUnavailableNavigation + 1);
}

void TerminalPaneTest::routesViewportAndSelectionActions()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalViewportRequest>();
    qRegisterMetaType<TerminalSelectionAdjustment>();

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("printf 'pane-action-selection'; sleep 5"),
    };
    options.hold = true;
    options.keybindingsConfigured = true;
    options.keybindings = {
        QStringLiteral("performable:alt+u=scroll_to_selection"),
        QStringLiteral("performable:alt+i=adjust_selection:left"),
        QStringLiteral("alt+y=select_all"),
        QStringLiteral("chain=adjust_selection:right"),
        QStringLiteral("chain=scroll_to_selection"),
    };

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updates(controller, &TerminalController::terminalUpdated);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QSignalSpy scrolls(controller, &TerminalController::scrollRequested);
    QSignalSpy selectAll(controller, &TerminalController::selectAllRequested);
    QSignalSpy adjustments(
        controller, &TerminalController::selectionAdjustmentRequested);

    // Before the first worker frame, page actions use the worker's actual
    // 24-row startup geometry rather than the legacy 20-row fallback.
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("scroll_page_down")));
    QCOMPARE(scrolls.count(), 1);
    QCOMPARE(qvariant_cast<TerminalViewportRequest>(
                 scrolls.constFirst().constFirst()).delta,
             qint64(24));
    scrolls.clear();

    // A resize request becomes the page basis immediately. An older worker
    // frame may still be queued back to the UI at this point.
    QSignalSpy resizes(controller, &TerminalController::resizeRequested);
    pane.setSize(QSizeF(640.0, 320.0));
    QVERIFY(!resizes.isEmpty());
    const int requestedRows = resizes.constLast().at(1).toInt();
    QVERIFY(requestedRows > 0);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("scroll_page_down")));
    QCOMPARE(qvariant_cast<TerminalViewportRequest>(
                 scrolls.constFirst().constFirst()).delta,
             qint64(requestedRows));
    scrolls.clear();

    // Selection-dependent performable bindings act as absent until the
    // worker's cached selection state says they can run.
    QKeyEvent missingScrollSelection(
        QEvent::KeyPress, Qt::Key_U, Qt::AltModifier, QStringLiteral("u"));
    QCoreApplication::sendEvent(&pane, &missingScrollSelection);
    QCOMPARE(forwarded.count(), 1);
    QCOMPARE(scrolls.count(), 0);
    QKeyEvent missingAdjustment(
        QEvent::KeyPress, Qt::Key_I, Qt::AltModifier, QStringLiteral("i"));
    QCoreApplication::sendEvent(&pane, &missingAdjustment);
    QCOMPARE(forwarded.count(), 2);
    QCOMPARE(adjustments.count(), 0);

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("pane-action-selection")), 5000);

    // Install a deterministic retained frame matching the queued resize. Page
    // actions must use all requested rows, specifically guarding against the
    // former rows-minus-one behavior.
    TerminalUpdate frame;
    frame.columns = 4;
    frame.rows = requestedRows;
    frame.fullFrame = true;
    for (int row = 0; row < frame.rows; ++row) {
        TerminalRowUpdate update;
        update.row = row;
        update.cells.resize(frame.columns);
        frame.dirtyRows.append(std::move(update));
    }
    controller->terminalUpdated(frame);

    const auto requestAt = [&scrolls](int index) {
        return qvariant_cast<TerminalViewportRequest>(
            scrolls.at(index).constFirst());
    };
    const auto executeScroll = [&pane, &scrolls](QStringView action) {
        const int before = scrolls.count();
        const bool performed = pane.executeConfiguredAction(action);
        return performed && scrolls.count() == before + 1;
    };

    QVERIFY(executeScroll(QStringLiteral("scroll_to_top")));
    QCOMPARE(requestAt(0).kind, TerminalViewportRequest::Kind::Top);
    QVERIFY(executeScroll(QStringLiteral("scroll_to_bottom")));
    QCOMPARE(requestAt(1).kind, TerminalViewportRequest::Kind::Bottom);
    QVERIFY(executeScroll(QStringLiteral("scroll_to_row:+1__2")));
    QCOMPARE(requestAt(2).kind, TerminalViewportRequest::Kind::Row);
    QCOMPARE(requestAt(2).row, quint64(12));
    QVERIFY(executeScroll(QStringLiteral("scroll_page_lines:-7")));
    QCOMPARE(requestAt(3).delta, qint64(-7));
    QVERIFY(executeScroll(QStringLiteral("scroll_page_up")));
    QCOMPARE(requestAt(4).delta, -qint64(requestedRows));
    QVERIFY(executeScroll(QStringLiteral("scroll_page_down")));
    QCOMPARE(requestAt(5).delta, qint64(requestedRows));
    QVERIFY(executeScroll(QStringLiteral("scroll_page_fractional:+0.5")));
    QCOMPARE(requestAt(6).delta,
             static_cast<qint64>(0.5F * static_cast<float>(requestedRows)));
    QVERIFY(executeScroll(QStringLiteral("scroll_page_fractional:-0.2")));
    // Binary32 multiplication happens before truncation toward zero.
    QCOMPARE(requestAt(7).delta,
             static_cast<qint64>(-0.2F * static_cast<float>(requestedRows)));

    const int beforeUnsafe = scrolls.count();
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("scroll_page_fractional:2e18")));
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("scroll_page_fractional:inf")));
    QCOMPARE(scrolls.count(), beforeUnsafe);

    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("scroll_to_selection")));
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("adjust_selection:right")));
    QCOMPARE(scrolls.count(), beforeUnsafe);
    QCOMPARE(adjustments.count(), 0);

    // Select-all intent makes its immediately following dependent actions
    // performable while all three requests retain their worker queue order.
    // This preserves Ghostty action chains without a UI round trip.
    QKeyEvent chainedSelection(
        QEvent::KeyPress, Qt::Key_Y, Qt::AltModifier, QStringLiteral("y"));
    QCoreApplication::sendEvent(&pane, &chainedSelection);
    QCOMPARE(selectAll.count(), 1);
    QCOMPARE(adjustments.count(), 1);
    QCOMPARE(scrolls.count(), beforeUnsafe + 1);
    QCOMPARE(requestAt(beforeUnsafe).kind,
             TerminalViewportRequest::Kind::Selection);
    QCOMPARE(qvariant_cast<TerminalSelectionAdjustment>(
                 adjustments.constFirst().constFirst()),
             TerminalSelectionAdjustment::Right);
    QTRY_VERIFY_WITH_TIMEOUT(controller->selectionAvailable(), 3000);

    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("scroll_to_selection")));
    QCOMPARE(scrolls.count(), beforeUnsafe + 2);
    QCOMPARE(requestAt(beforeUnsafe + 1).kind,
             TerminalViewportRequest::Kind::Selection);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("adjust_selection:right")));
    QCOMPARE(adjustments.count(), 2);
    QCOMPARE(qvariant_cast<TerminalSelectionAdjustment>(
                 adjustments.at(1).constFirst()),
             TerminalSelectionAdjustment::Right);

    const int beforeBoundActions = forwarded.count();
    QKeyEvent availableScrollSelection(
        QEvent::KeyPress, Qt::Key_U, Qt::AltModifier, QStringLiteral("u"));
    QCoreApplication::sendEvent(&pane, &availableScrollSelection);
    QKeyEvent availableAdjustment(
        QEvent::KeyPress, Qt::Key_I, Qt::AltModifier, QStringLiteral("i"));
    QCoreApplication::sendEvent(&pane, &availableAdjustment);
    QCOMPARE(forwarded.count(), beforeBoundActions);
    QCOMPARE(scrolls.count(), beforeUnsafe + 3);
    QCOMPARE(adjustments.count(), 3);
}

void TerminalPaneTest::routesTerminalControlActions()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindingsConfigured = true;
    options.keybindings = {
        QStringLiteral("alt+c=csi:31m"),
        QStringLiteral("chain=esc:7"),
        QStringLiteral(R"(chain=text:\\x00\\n\\u{1f47b})"),
        QStringLiteral("chain=reset"),
    };

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    QSignalSpy csi(controller, &TerminalController::csiRequested);
    QSignalSpy escape(controller, &TerminalController::escapeRequested);
    QSignalSpy rawText(controller, &TerminalController::rawTextRequested);
    QSignalSpy reset(controller, &TerminalController::resetTerminalRequested);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QStringList order;
    connect(controller, &TerminalController::csiRequested,
            this, [&order](const QByteArray &) { order.append(QStringLiteral("csi")); });
    connect(controller, &TerminalController::escapeRequested,
            this, [&order](const QByteArray &) { order.append(QStringLiteral("esc")); });
    connect(controller, &TerminalController::rawTextRequested,
            this, [&order](const QByteArray &) { order.append(QStringLiteral("text")); });
    connect(controller, &TerminalController::resetTerminalRequested,
            this, [&order] { order.append(QStringLiteral("reset")); });

    QKeyEvent trigger(QEvent::KeyPress, Qt::Key_C,
                      Qt::AltModifier, QStringLiteral("c"));
    QCoreApplication::sendEvent(&pane, &trigger);

    QCOMPARE(order, QStringList({QStringLiteral("csi"), QStringLiteral("esc"),
                                 QStringLiteral("text"), QStringLiteral("reset")}));
    QCOMPARE(csi.count(), 1);
    QCOMPARE(csi.constFirst().constFirst().toByteArray(), QByteArrayLiteral("31m"));
    QCOMPARE(escape.count(), 1);
    QCOMPARE(escape.constFirst().constFirst().toByteArray(), QByteArrayLiteral("7"));
    QCOMPARE(rawText.count(), 1);
    QCOMPARE(rawText.constFirst().constFirst().toByteArray(),
             QByteArrayLiteral(R"(\\x00\\n\\u{1f47b})"));
    QCOMPARE(reset.count(), 1);
    QCOMPARE(forwarded.count(), 0);

    // Literal validation belongs to the worker: an invalid escape remains a
    // performed/consumed action at the pane boundary, while invalid action
    // grammar never emits a worker request.
    QVERIFY(pane.executeConfiguredAction(QStringLiteral(R"(text:\\q)")));
    QCOMPARE(rawText.count(), 2);
    QCOMPARE(rawText.constLast().constFirst().toByteArray(),
             QByteArrayLiteral(R"(\\q)"));
    QVERIFY(!pane.executeConfiguredAction(QStringLiteral("csi")));
    QVERIFY(!pane.executeConfiguredAction(QStringLiteral("reset:")));
    QCOMPARE(csi.count(), 1);
    QCOMPARE(reset.count(), 1);
}

void TerminalPaneTest::resetClearsTerminalMetadataAndSplitInheritance()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "printf '\\033]0;metadata-title\\007"
            "\\033]7;file:///\\007metadata-ready\\n'; sleep 5"),
    };
    options.hold = true;

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updates(controller, &TerminalController::terminalUpdated);

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("metadata-ready")), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(pane.title(), QStringLiteral("metadata-title"),
                              1000);
    QTRY_COMPARE_WITH_TIMEOUT(pane.currentDirectory(), QStringLiteral("/"),
                              1000);

    QVERIFY(pane.executeConfiguredAction(QStringLiteral("reset")));
    QTRY_VERIFY_WITH_TIMEOUT(pane.currentDirectory().isEmpty(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(pane.title(), QStringLiteral("sh"), 1000);
    QCOMPARE(pane.splitLaunchOptions().workingDirectory, QDir::tempPath());
}

void TerminalPaneTest::routesStructuredSequencesAndCancelsThemOnReload()
{
    qRegisterMetaType<TerminalSequenceResolution>();

    const auto unicode = [](quint32 codepoint, quint8 modifiers = 0) {
        return GhosttyKeybindTrigger{
            .kind = GhosttyKeybindKeyKind::Unicode,
            .unicodeCodepoint = codepoint,
            .modifiers = modifiers,
        };
    };
    const auto sequence = [&](quint32 leaf, QString action,
                              GhosttyKeybindFlags flags = {}) {
        return GhosttyKeybindDefinition{
            .sequence = {unicode('x', GhosttyKeybindCtrl), unicode(leaf)},
            .actions = {std::move(action)},
            .flags = flags,
        };
    };

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindingsConfigured = true;
    options.keybindConfig.root = {
        sequence('n', QStringLiteral("new_tab")),
        sequence('u', QStringLiteral("reload_config"),
                 GhosttyKeybindFlags{.consumed = false}),
        sequence('p', QStringLiteral("goto_split:left"),
                 GhosttyKeybindFlags{.performable = true}),
        sequence('e', QStringLiteral("end_key_sequence")),
        sequence('f', QStringLiteral("end_key_sequence"),
                 GhosttyKeybindFlags{.consumed = false}),
    };

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy staged(controller,
                      &TerminalController::sequenceKeyStagingRequested);
    QSignalSpy resolved(controller,
                        &TerminalController::sequenceResolutionRequested);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QSignalSpy newTab(&pane, &TerminalPane::requestNewTab);
    QSignalSpy reload(&pane, &TerminalPane::requestConfigReload);

    const auto press = [&pane](int key, Qt::KeyboardModifiers modifiers,
                               QString text) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers, std::move(text));
        QCoreApplication::sendEvent(&pane, &event);
    };
    const auto release = [&pane](int key, Qt::KeyboardModifiers modifiers,
                                 QString text) {
        QKeyEvent event(QEvent::KeyRelease, key, modifiers, std::move(text));
        QCoreApplication::sendEvent(&pane, &event);
    };
    const auto leader = [&] {
        press(Qt::Key_X, Qt::ControlModifier, QString(QChar(0x18)));
    };
    const auto resolution = [&]() {
        return qvariant_cast<TerminalSequenceResolution>(
            resolved.constLast().at(1));
    };

    // Leaders hold only their press. Their release remains visible to the
    // terminal, while a consumed leaf suppresses both its press and release.
    leader();
    QCOMPARE(staged.count(), 1);
    QCOMPARE(forwarded.count(), 0);
    release(Qt::Key_X, Qt::ControlModifier, QString(QChar(0x18)));
    QCOMPARE(forwarded.count(), 1);
    press(Qt::Key_N, Qt::NoModifier, QStringLiteral("n"));
    QCOMPARE(newTab.count(), 1);
    QCOMPARE(resolution(), TerminalSequenceResolution::Drop);
    release(Qt::Key_N, Qt::NoModifier, QStringLiteral("n"));
    QCOMPARE(forwarded.count(), 1);

    // An invalid continuation atomically flushes the encoded prefix and the
    // current press; it must not also travel through the ordinary key signal.
    leader();
    press(Qt::Key_Z, Qt::NoModifier, QStringLiteral("z"));
    QCOMPARE(resolution(),
             TerminalSequenceResolution::FlushAndSendCurrent);
    QCOMPARE(forwarded.count(), 1);

    // Unconsumed leaves run their action and replay the entire sequence.
    leader();
    press(Qt::Key_U, Qt::NoModifier, QStringLiteral("u"));
    QCOMPARE(reload.count(), 1);
    QCOMPARE(resolution(),
             TerminalSequenceResolution::FlushAndSendCurrent);

    // A performable action that the workspace rejects behaves as absent.
    pane.setWorkspaceActionHandler([](WorkspaceActionRequest request) {
        return request.action != WorkspaceAction::NavigatePane;
    });
    leader();
    press(Qt::Key_P, Qt::NoModifier, QStringLiteral("p"));
    QCOMPARE(resolution(),
             TerminalSequenceResolution::FlushAndSendCurrent);

    // end_key_sequence flushes leaders but consumes the terminating key.
    leader();
    press(Qt::Key_E, Qt::NoModifier, QStringLiteral("e"));
    QCOMPARE(resolution(), TerminalSequenceResolution::Flush);

    // The action itself still flushes only the leaders when the binding is
    // unconsumed, but ordinary handling then encodes its terminating key.
    const int beforeUnconsumedEnd = forwarded.count();
    leader();
    press(Qt::Key_F, Qt::NoModifier, QStringLiteral("f"));
    QCOMPARE(resolution(), TerminalSequenceResolution::Flush);
    QCOMPARE(forwarded.count(), beforeUnconsumedEnd + 1);

    // Live reload is transactional at pane level: pending bytes are dropped
    // before the replacement trie becomes visible.
    leader();
    const int resolutionsBeforeReload = resolved.count();
    LaunchOptions reloaded = options;
    reloaded.keybindConfig.root = {
        GhosttyKeybindDefinition{
            .sequence = {unicode('q', GhosttyKeybindCtrl)},
            .actions = {QStringLiteral("new_tab")},
        },
    };
    pane.applyRuntimeOptions(reloaded);
    QCOMPARE(resolved.count(), resolutionsBeforeReload + 1);
    QCOMPARE(resolution(), TerminalSequenceResolution::Drop);
    const int beforeFormerLeaf = forwarded.count();
    press(Qt::Key_N, Qt::NoModifier, QStringLiteral("n"));
    QCOMPARE(forwarded.count(), beforeFormerLeaf + 1);
}

void TerminalPaneTest::routesNamedKeyTablesAndClearsThemOnReload()
{
    const auto unicode = [](quint32 codepoint, quint8 modifiers = 0) {
        return GhosttyKeybindTrigger{
            .kind = GhosttyKeybindKeyKind::Unicode,
            .unicodeCodepoint = codepoint,
            .modifiers = modifiers,
        };
    };
    const auto binding = [&](QVector<GhosttyKeybindTrigger> sequence,
                             QString action,
                             GhosttyKeybindFlags flags = {}) {
        return GhosttyKeybindDefinition{
            .sequence = std::move(sequence),
            .actions = {std::move(action)},
            .flags = flags,
        };
    };
    const GhosttyKeybindTrigger escape{
        .kind = GhosttyKeybindKeyKind::Physical,
        .physicalName = QStringLiteral("escape"),
    };

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindingsConfigured = true;
    options.keybindConfig.root = {
        binding({unicode('m', GhosttyKeybindCtrl)},
                QStringLiteral("activate_key_table:edit"),
                GhosttyKeybindFlags{.performable = true}),
        binding({unicode('o', GhosttyKeybindCtrl)},
                QStringLiteral("activate_key_table_once:once")),
    };
    options.keybindConfig.tables = {
        GhosttyKeybindTable{
            .name = QStringLiteral("edit"),
            .bindings = {
                binding({unicode('n')}, QStringLiteral("new_tab")),
                binding({escape},
                        QStringLiteral("deactivate_key_table")),
            },
        },
        GhosttyKeybindTable{
            .name = QStringLiteral("once"),
            .bindings = {
                binding({unicode('x'), unicode('y')},
                        QStringLiteral("new_tab")),
            },
        },
    };

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QSignalSpy newTab(&pane, &TerminalPane::requestNewTab);
    QSignalSpy tableChanges(&pane, &TerminalPane::activeKeyTablesChanged);

    const auto press = [&pane](int key, Qt::KeyboardModifiers modifiers,
                               QString text) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers, std::move(text));
        QCoreApplication::sendEvent(&pane, &event);
    };

    press(Qt::Key_N, Qt::NoModifier, QStringLiteral("n"));
    QCOMPARE(forwarded.count(), 1);

    press(Qt::Key_M, Qt::ControlModifier, QString(QChar(0x0d)));
    QCOMPARE(pane.activeKeyTables(), QStringList({QStringLiteral("edit")}));
    QCOMPARE(tableChanges.count(), 1);
    press(Qt::Key_N, Qt::NoModifier, QStringLiteral("n"));
    QCOMPARE(newTab.count(), 1);

    // A performable activation of the already-innermost table behaves as an
    // absent binding and reaches the terminal.
    const int beforeDuplicate = forwarded.count();
    press(Qt::Key_M, Qt::ControlModifier, QString(QChar(0x0d)));
    QCOMPARE(forwarded.count(), beforeDuplicate + 1);
    QCOMPARE(pane.activeKeyTables(), QStringList({QStringLiteral("edit")}));

    press(Qt::Key_Escape, Qt::NoModifier, QString{});
    QVERIFY(pane.activeKeyTables().isEmpty());

    // A one-shot table pops on the sequence leader, while the retained child
    // trie still resolves the continuation.
    press(Qt::Key_O, Qt::ControlModifier, QString(QChar(0x0f)));
    QCOMPARE(pane.activeKeyTables(), QStringList({QStringLiteral("once")}));
    press(Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    QVERIFY(pane.activeKeyTables().isEmpty());
    press(Qt::Key_Y, Qt::NoModifier, QStringLiteral("y"));
    QCOMPARE(newTab.count(), 2);

    press(Qt::Key_M, Qt::ControlModifier, QString(QChar(0x0d)));
    QCOMPARE(pane.activeKeyTables(), QStringList({QStringLiteral("edit")}));
    LaunchOptions reloaded = options;
    reloaded.keybindConfig.tables.clear();
    pane.applyRuntimeOptions(reloaded);
    QVERIFY(pane.activeKeyTables().isEmpty());
}

void TerminalPaneTest::replaysInvalidStructuredSequenceThroughPty()
{
    qRegisterMetaType<TerminalUpdate>();

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "stty raw -echo; "
            "printf 'keybind-ready'; "
            "payload=$(dd bs=1 count=2 2>/dev/null); "
            "stty sane; "
            "printf 'keybind-bytes:'; "
            "printf '%s' \"$payload\" | od -An -tx1 | tr -d ' \\n'; "
            "printf '\\n'")};
    options.hold = true;
    options.keybindingsConfigured = true;
    options.keybindConfig.root = {GhosttyKeybindDefinition{
        .sequence = {
            GhosttyKeybindTrigger{
                .kind = GhosttyKeybindKeyKind::Unicode,
                .unicodeCodepoint = 'x',
                .modifiers = GhosttyKeybindCtrl,
            },
            GhosttyKeybindTrigger{
                .kind = GhosttyKeybindKeyKind::Unicode,
                .unicodeCodepoint = 'n',
            },
        },
        .actions = {QStringLiteral("new_tab")},
    }};

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updates(controller, &TerminalController::terminalUpdated);
    QSignalSpy errors(controller, &TerminalController::errorOccurred);
    QSignalSpy newTab(&pane, &TerminalPane::requestNewTab);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("keybind-ready")), 5000);

    const auto send = [&pane](int key, Qt::KeyboardModifiers modifiers,
                              const QString &text) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
        QCoreApplication::sendEvent(&pane, &event);
    };

    // This valid sequence is consumed and must contribute no PTY bytes.
    send(Qt::Key_X, Qt::ControlModifier, QString(QChar(0x18)));
    send(Qt::Key_N, Qt::NoModifier, QStringLiteral("n"));
    QCOMPARE(newTab.count(), 1);

    // The same leader followed by an invalid key replays byte-exact input in
    // order: Ctrl-X (0x18), then z (0x7a).
    send(Qt::Key_X, Qt::ControlModifier, QString(QChar(0x18)));
    send(Qt::Key_Z, Qt::NoModifier, QStringLiteral("z"));
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("keybind-bytes:187a")), 5000);
    QVERIFY2(errors.isEmpty(),
             errors.isEmpty()
                 ? ""
                 : qPrintable(errors.constFirst().constFirst().toString()));
}

QTEST_MAIN(TerminalPaneTest)

#include "test_terminal_pane.moc"
