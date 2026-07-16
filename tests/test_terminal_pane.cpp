#include "launch_options.h"
#include "terminal_controller.h"
#include "terminal_pane.h"
#include "terminal_types.h"

#include <QColor>
#include <QDir>
#include <QImage>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>

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

bool framesContain(const QSignalSpy &spy, const QString &needle)
{
    return std::any_of(spy.cbegin(), spy.cend(), [&needle](const QList<QVariant> &arguments) {
        return frameText(qvariant_cast<TerminalFrame>(arguments.constFirst()))
            .contains(needle);
    });
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
};

void TerminalPaneTest::replacesStartingFrameInsteadOfAccumulatingSceneRoots()
{
    qRegisterMetaType<TerminalFrame>();

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
    QSignalSpy frameSpy(controller, &TerminalController::frameReady);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        framesContain(frameSpy, QStringLiteral("scene-root-marker")), 5000);

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

QTEST_MAIN(TerminalPaneTest)

#include "test_terminal_pane.moc"
