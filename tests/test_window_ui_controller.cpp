#include "window_ui_controller.h"

#include <QAbstractItemModel>
#include <QSignalSpy>
#include <QTest>

#include <chrono>
#include <utility>

using namespace std::chrono_literals;

namespace {

CommandPaletteEntry command(QString title, QString actionKey, QString action,
                            QString description = {})
{
    return {
        .title = std::move(title),
        .description = std::move(description),
        .actionKey = std::move(actionKey),
        .action = std::move(action),
    };
}

QString titleAt(const CommandPaletteModel &model, int row)
{
    return model.data(model.index(row, 0), CommandPaletteModel::TitleRole)
        .toString();
}

} // namespace

class WindowUiControllerTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void commandPaletteUsesColonNormalizedDeterministicOrdering();
    void commandPaletteFiltersTitleAndActionKeyOnly();
    void commandPaletteSelectionExposesCanonicalAction();
    void identicalCommandReplacementIsSilent();
    void modalsAreMutuallyExclusive();
    void notificationsHaveExactTextAndDuration();
    void toastExpiryIsExplicitFifo();
    void toastQueueIsBoundedWithoutInterruptingCurrentToast();
    void invalidAndClearedToastsHaveDeterministicState();
};

void WindowUiControllerTest::
    commandPaletteUsesColonNormalizedDeterministicOrdering()
{
    CommandPaletteModel model;
    model.replaceEntries({
        command(QStringLiteral("Foo Bar:"), QStringLiteral("third"),
                QStringLiteral("text:third")),
        command(QStringLiteral("foo:"), QStringLiteral("second"),
                QStringLiteral("text:second")),
        command(QStringLiteral("Foo:"), QStringLiteral("first"),
                QStringLiteral("text:first")),
        command(QStringLiteral("Alpha"), QStringLiteral("alpha"),
                QStringLiteral("text:alpha")),
    });

    QCOMPARE(model.count(), 4);
    QCOMPARE(titleAt(model, 0), QStringLiteral("Alpha"));
    // Replacing ':' with a tab for the comparison makes the category title
    // precede a longer title beginning with the same words.
    QCOMPARE(titleAt(model, 1), QStringLiteral("Foo:"));
    QCOMPARE(titleAt(model, 2), QStringLiteral("foo:"));
    QCOMPARE(titleAt(model, 3), QStringLiteral("Foo Bar:"));

    QVector<CommandPaletteEntry> reversed{
        command(QStringLiteral("foo:"), QStringLiteral("second"),
                QStringLiteral("text:second")),
        command(QStringLiteral("Foo Bar:"), QStringLiteral("third"),
                QStringLiteral("text:third")),
        command(QStringLiteral("Alpha"), QStringLiteral("alpha"),
                QStringLiteral("text:alpha")),
        command(QStringLiteral("Foo:"), QStringLiteral("first"),
                QStringLiteral("text:first")),
    };
    model.replaceEntries(std::move(reversed));
    QCOMPARE(titleAt(model, 0), QStringLiteral("Alpha"));
    QCOMPARE(titleAt(model, 1), QStringLiteral("Foo:"));
    QCOMPARE(titleAt(model, 2), QStringLiteral("foo:"));
    QCOMPARE(titleAt(model, 3), QStringLiteral("Foo Bar:"));
}

void WindowUiControllerTest::commandPaletteFiltersTitleAndActionKeyOnly()
{
    CommandPaletteModel model;
    model.replaceEntries({
        command(QStringLiteral("Copy Selection"),
                QStringLiteral("copy_to_clipboard"),
                QStringLiteral("copy_to_clipboard:plain"),
                QStringLiteral("A hidden needle")),
        command(QStringLiteral("Reset Terminal"), QStringLiteral("reset"),
                QStringLiteral("reset")),
    });

    model.setFilter(QStringLiteral("SELECTION"));
    QCOMPARE(model.count(), 1);
    QCOMPARE(titleAt(model, 0), QStringLiteral("Copy Selection"));

    model.setFilter(QStringLiteral("CLIPBOARD"));
    QCOMPARE(model.count(), 1);
    QCOMPARE(titleAt(model, 0), QStringLiteral("Copy Selection"));

    model.setFilter(QStringLiteral("needle"));
    QCOMPARE(model.count(), 0);
    QCOMPARE(model.selectedIndex(), -1);
    QVERIFY(model.selectedAction().isEmpty());

    model.setFilter({});
    QCOMPARE(model.count(), 2);
    QCOMPARE(model.selectedIndex(), 0);
}

void WindowUiControllerTest::commandPaletteSelectionExposesCanonicalAction()
{
    CommandPaletteModel model;
    model.replaceEntries({
        command(QStringLiteral("Alpha"), QStringLiteral("alpha"),
                QStringLiteral("text:exact escaped payload")),
        command(QStringLiteral("Beta"), QStringLiteral("beta"),
                QStringLiteral("goto_tab:2")),
        command(QStringLiteral("Gamma"), QStringLiteral("gamma"),
                QStringLiteral("reset")),
    });

    QCOMPARE(model.selectedIndex(), 0);
    QCOMPARE(model.selectedAction(),
             QStringLiteral("text:exact escaped payload"));
    QCOMPARE(model.actionAt(1), QStringLiteral("goto_tab:2"));

    QSignalSpy selectionChanged(&model,
                                &CommandPaletteModel::selectedIndexChanged);
    QSignalSpy actionChanged(&model,
                             &CommandPaletteModel::selectedActionChanged);
    model.selectRelative(-1);
    QCOMPARE(model.selectedIndex(), 2);
    QCOMPARE(model.selectedAction(), QStringLiteral("reset"));
    QCOMPARE(selectionChanged.count(), 1);
    QCOMPARE(actionChanged.count(), 1);

    model.selectRelative(1);
    QCOMPARE(model.selectedIndex(), 0);
    model.setSelectedIndex(99);
    QCOMPARE(model.selectedIndex(), 2);
    model.setSelectedIndex(-99);
    QCOMPARE(model.selectedIndex(), 0);

    model.setFilter(QStringLiteral("beta"));
    QCOMPARE(model.selectedIndex(), 0);
    QCOMPARE(model.selectedAction(), QStringLiteral("goto_tab:2"));
}

void WindowUiControllerTest::identicalCommandReplacementIsSilent()
{
    CommandPaletteModel model;
    const QVector<CommandPaletteEntry> commands{
        command(QStringLiteral("Beta"), QStringLiteral("beta"),
                QStringLiteral("text:beta")),
        command(QStringLiteral("Alpha"), QStringLiteral("alpha"),
                QStringLiteral("text:alpha")),
    };
    model.replaceEntries(commands);

    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    QSignalSpy actionChanged(&model,
                             &CommandPaletteModel::selectedActionChanged);
    model.replaceEntries(commands);
    QVERIFY(reset.isEmpty());
    QVERIFY(actionChanged.isEmpty());
}

void WindowUiControllerTest::modalsAreMutuallyExclusive()
{
    WindowUiController controller;
    QSignalSpy modalChanged(&controller, &WindowUiController::modalChanged);
    QSignalSpy paletteChanged(
        &controller, &WindowUiController::commandPaletteVisibleChanged);
    QSignalSpy overviewChanged(&controller,
                               &WindowUiController::tabOverviewVisibleChanged);

    controller.showCommandPalette();
    QCOMPARE(controller.modal(), WindowUiController::Modal::CommandPalette);
    QVERIFY(controller.commandPaletteVisible());
    QVERIFY(!controller.tabOverviewVisible());

    controller.showTabOverview();
    QCOMPARE(controller.modal(), WindowUiController::Modal::TabOverview);
    QVERIFY(!controller.commandPaletteVisible());
    QVERIFY(controller.tabOverviewVisible());

    controller.toggleTabOverview();
    QCOMPARE(controller.modal(), WindowUiController::Modal::None);
    controller.toggleCommandPalette();
    QCOMPARE(controller.modal(), WindowUiController::Modal::CommandPalette);
    controller.toggleCommandPalette();
    QCOMPARE(controller.modal(), WindowUiController::Modal::None);

    QCOMPARE(modalChanged.count(), 5);
    QCOMPARE(paletteChanged.count(), 4);
    QCOMPARE(overviewChanged.count(), 2);
}

void WindowUiControllerTest::notificationsHaveExactTextAndDuration()
{
    WindowUiController controller;

    controller.notifyClipboardCopied(false);
    QCOMPARE(controller.toastMessage(), QStringLiteral("Copied to clipboard"));
    QCOMPARE(controller.toastDurationMilliseconds(), 3000);

    QVERIFY(controller.expireToast());
    controller.notifyClipboardCopied(true);
    QCOMPARE(controller.toastMessage(), QStringLiteral("Cleared clipboard"));
    QCOMPARE(controller.toastDurationMilliseconds(), 3000);

    QVERIFY(controller.expireToast());
    controller.notifyConfigurationReloaded();
    QCOMPARE(controller.toastMessage(),
             QStringLiteral("Reloaded the configuration"));
    QCOMPARE(controller.toastDurationMilliseconds(), 3000);
}

void WindowUiControllerTest::toastExpiryIsExplicitFifo()
{
    WindowUiController controller;
    QSignalSpy toastChanged(&controller, &WindowUiController::toastChanged);
    QSignalSpy depthChanged(&controller,
                            &WindowUiController::toastQueueDepthChanged);

    controller.enqueueToast(QStringLiteral("first"), 25ms);
    controller.enqueueToast(QStringLiteral("second"), 50ms);
    controller.enqueueToast(QStringLiteral("third"), 75ms);
    QCOMPARE(controller.toastQueueDepth(), 3);
    QCOMPARE(controller.toastMessage(), QStringLiteral("first"));
    QCOMPARE(controller.toastDurationMilliseconds(), 25);
    QCOMPARE(toastChanged.count(), 1);
    QCOMPARE(depthChanged.count(), 3);

    const quint64 firstRevision = controller.toastRevision();
    QVERIFY(controller.expireToast());
    QCOMPARE(controller.toastMessage(), QStringLiteral("second"));
    QCOMPARE(controller.toastDurationMilliseconds(), 50);
    QCOMPARE(controller.toastRevision(), firstRevision + 1);

    QVERIFY(controller.expireToast());
    QCOMPARE(controller.toastMessage(), QStringLiteral("third"));
    QVERIFY(controller.expireToast());
    QVERIFY(!controller.toastVisible());
    QCOMPARE(controller.toastQueueDepth(), 0);
    QCOMPARE(controller.toastDurationMilliseconds(), 0);
    QCOMPARE(toastChanged.count(), 4);
    QCOMPARE(depthChanged.count(), 6);
    QVERIFY(!controller.expireToast());
}

void WindowUiControllerTest::
    toastQueueIsBoundedWithoutInterruptingCurrentToast()
{
    WindowUiController controller;
    controller.enqueueToast(QStringLiteral("0"));
    for (int index = 1; index <= WindowUiController::MaximumToastQueueDepth;
         ++index) {
        controller.enqueueToast(QString::number(index));
    }

    QCOMPARE(controller.toastQueueDepth(),
             WindowUiController::MaximumToastQueueDepth);
    QCOMPARE(controller.toastMessage(), QStringLiteral("0"));

    // The oldest pending toast was evicted, but enqueueing never replaced the
    // item already being presented.
    QVERIFY(controller.expireToast());
    QCOMPARE(controller.toastMessage(), QStringLiteral("2"));
    for (int index = 3; index <= WindowUiController::MaximumToastQueueDepth;
         ++index) {
        QVERIFY(controller.expireToast());
        QCOMPARE(controller.toastMessage(), QString::number(index));
    }
    QVERIFY(controller.expireToast());
    QVERIFY(!controller.toastVisible());
}

void WindowUiControllerTest::invalidAndClearedToastsHaveDeterministicState()
{
    WindowUiController controller;
    QSignalSpy toastChanged(&controller, &WindowUiController::toastChanged);

    controller.enqueueToast(QString{}, 3s);
    controller.enqueueToast(QStringLiteral("ignored"), 0ms);
    QVERIFY(!controller.toastVisible());
    QVERIFY(toastChanged.isEmpty());

    controller.enqueueToast(QStringLiteral("visible"), 3s);
    controller.enqueueToast(QStringLiteral("pending"), 3s);
    const quint64 visibleRevision = controller.toastRevision();
    controller.clearToasts();
    QVERIFY(!controller.toastVisible());
    QCOMPARE(controller.toastQueueDepth(), 0);
    QCOMPARE(controller.toastRevision(), visibleRevision + 1);
    QCOMPARE(toastChanged.count(), 2);

    controller.clearToasts();
    QCOMPARE(toastChanged.count(), 2);
}

QTEST_APPLESS_MAIN(WindowUiControllerTest)

#include "test_window_ui_controller.moc"
