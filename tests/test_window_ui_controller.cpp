#include "workspace/window_ui_controller.h"

#include <QAbstractItemModel>
#include <QSignalSpy>
#include <QTest>

#include <chrono>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

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

CommandPaletteRow configuredRow(QString title, QString actionKey,
                                QString action, QString description = {})
{
    return {
        .title = std::move(title),
        .description = std::move(description),
        .actionKey = std::move(actionKey),
        .command = std::move(action),
    };
}

CommandPaletteRow jumpRow(QString title, SurfaceTarget target,
                          QString description = {},
                          QString ignoredActionKey = {})
{
    return {
        .title = std::move(title),
        .description = std::move(description),
        .actionKey = std::move(ignoredActionKey),
        .command = target,
    };
}

QString titleAt(const CommandPaletteModel &model, int row)
{
    return model.data(model.index(row, 0), CommandPaletteModel::TitleRole)
        .toString();
}

QString actionKeyAt(const CommandPaletteModel &model, int row)
{
    return model.data(model.index(row, 0), CommandPaletteModel::ActionKeyRole)
        .toString();
}

SurfaceTarget target(WindowId::Value window, PaneId::Value pane)
{
    return {
        .windowId = WindowId{window},
        .paneId = PaneId{pane},
    };
}

} // namespace

class WindowUiControllerTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void commandPaletteUsesColonNormalizedDeterministicOrdering();
    void commandPaletteFiltersTitleAndActionKeyOnly();
    void commandPaletteSelectionIsTypedAndStable();
    void commandPaletteActivationCapturesBeforeClose();
    void commandPaletteRefreshesImmediatelyBeforeOpening();
    void commandPaletteActivationMayDestroyController();
    void identicalCommandReplacementIsSilent();
    void modalsAreMutuallyExclusive();
    void configurationDiagnosticsDeduplicateAndRetryInPlace();
    void notificationsHaveExactTextAndDuration();
    void consecutiveDuplicateToastsAreCoalesced();
    void toastExpiryIsExplicitFifo();
    void toastQueueIsBoundedWithoutInterruptingCurrentToast();
    void invalidAndClearedToastsHaveDeterministicState();
};

void WindowUiControllerTest::
    commandPaletteUsesColonNormalizedDeterministicOrdering()
{
    CommandPaletteModel model;
    model.replaceRows({
        configuredRow(QStringLiteral("Foo Bar:"), QStringLiteral("third"),
                      QStringLiteral("text:third")),
        jumpRow(QStringLiteral("foo:"), target(9, 2),
                QStringLiteral("later target")),
        jumpRow(QStringLiteral("foo:"), target(3, 7),
                QStringLiteral("earlier target")),
        configuredRow(QStringLiteral("Foo:"), QStringLiteral("first"),
                      QStringLiteral("text:first")),
        configuredRow(QStringLiteral("Alpha"), QStringLiteral("alpha"),
                      QStringLiteral("text:alpha")),
    });

    QCOMPARE(model.count(), 5);
    QCOMPARE(titleAt(model, 0), QStringLiteral("Alpha"));
    // Replacing ':' with a tab for the comparison makes the category title
    // precede a longer title beginning with the same words.
    QCOMPARE(titleAt(model, 1), QStringLiteral("Foo:"));
    QCOMPARE(titleAt(model, 2), QStringLiteral("foo:"));
    QCOMPARE(std::get<SurfaceTarget>(*model.commandAt(2)), target(3, 7));
    QCOMPARE(std::get<SurfaceTarget>(*model.commandAt(3)), target(9, 2));
    QCOMPARE(titleAt(model, 4), QStringLiteral("Foo Bar:"));

    QVector<CommandPaletteRow> reversed{
        jumpRow(QStringLiteral("foo:"), target(9, 2)),
        configuredRow(QStringLiteral("Foo Bar:"), QStringLiteral("third"),
                      QStringLiteral("text:third")),
        configuredRow(QStringLiteral("Alpha"), QStringLiteral("alpha"),
                      QStringLiteral("text:alpha")),
        jumpRow(QStringLiteral("foo:"), target(3, 7)),
        configuredRow(QStringLiteral("Foo:"), QStringLiteral("first"),
                      QStringLiteral("text:first")),
    };
    model.replaceRows(std::move(reversed));
    QCOMPARE(titleAt(model, 0), QStringLiteral("Alpha"));
    QCOMPARE(titleAt(model, 1), QStringLiteral("Foo:"));
    QCOMPARE(titleAt(model, 2), QStringLiteral("foo:"));
    QCOMPARE(std::get<SurfaceTarget>(*model.commandAt(2)), target(3, 7));
    QCOMPARE(std::get<SurfaceTarget>(*model.commandAt(3)), target(9, 2));
    QCOMPARE(titleAt(model, 4), QStringLiteral("Foo Bar:"));
}

void WindowUiControllerTest::commandPaletteFiltersTitleAndActionKeyOnly()
{
    CommandPaletteModel model;
    model.replaceRows({
        configuredRow(QStringLiteral("Copy Selection"),
                      QStringLiteral("copy_to_clipboard"),
                      QStringLiteral("copy_to_clipboard:plain"),
                      QStringLiteral("A hidden needle")),
        configuredRow(QStringLiteral("Reset Terminal"), QStringLiteral("reset"),
                      QStringLiteral("reset")),
        jumpRow(QStringLiteral("Jump to build"), target(1, 2),
                QStringLiteral("A hidden destination"),
                QStringLiteral("ignored_jump_key")),
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
    QVERIFY(!model.selectedCommand().has_value());

    model.setFilter(QStringLiteral("ignored_jump_key"));
    QCOMPARE(model.count(), 0);

    model.setFilter(QStringLiteral("BUILD"));
    QCOMPARE(model.count(), 1);
    QCOMPARE(titleAt(model, 0), QStringLiteral("Jump to build"));
    QVERIFY(actionKeyAt(model, 0).isEmpty());

    model.setFilter({});
    QCOMPARE(model.count(), 3);
    QCOMPARE(model.selectedIndex(), 0);
}

void WindowUiControllerTest::commandPaletteSelectionIsTypedAndStable()
{
    CommandPaletteModel model;
    const SurfaceTarget betaTarget = target(4, 9);
    model.replaceRows({
        configuredRow(QStringLiteral("Alpha"), QStringLiteral("alpha"),
                      QStringLiteral("text:exact escaped payload")),
        jumpRow(QStringLiteral("Beta"), betaTarget),
        configuredRow(QStringLiteral("Gamma"), QStringLiteral("gamma"),
                      QStringLiteral("reset")),
    });

    QCOMPARE(model.selectedIndex(), 0);
    QCOMPARE(std::get<QString>(*model.selectedCommand()),
             QStringLiteral("text:exact escaped payload"));
    QCOMPARE(std::get<SurfaceTarget>(*model.commandAt(1)), betaTarget);
    QVERIFY(!model.commandAt(-1).has_value());

    QSignalSpy selectionChanged(&model,
                                &CommandPaletteModel::selectedIndexChanged);
    model.selectRelative(-1);
    QCOMPARE(model.selectedIndex(), 2);
    QCOMPARE(std::get<QString>(*model.selectedCommand()),
             QStringLiteral("reset"));
    QCOMPARE(selectionChanged.count(), 1);

    model.selectRelative(1);
    QCOMPARE(model.selectedIndex(), 0);
    model.setSelectedIndex(99);
    QCOMPARE(model.selectedIndex(), 2);
    model.setSelectedIndex(-99);
    QCOMPARE(model.selectedIndex(), 0);

    model.setFilter(QStringLiteral("beta"));
    QCOMPARE(model.selectedIndex(), 0);
    QCOMPARE(std::get<SurfaceTarget>(*model.selectedCommand()), betaTarget);

    // A dynamic rebuild can reorder duplicate-looking rows without changing
    // which stable surface target is selected.
    model.replaceRows({
        jumpRow(QStringLiteral("Beta"), target(2, 1)),
        jumpRow(QStringLiteral("Beta"), betaTarget),
        configuredRow(QStringLiteral("Alpha"), QStringLiteral("alpha"),
                      QStringLiteral("text:exact escaped payload")),
    });
    QCOMPARE(model.selectedIndex(), 1);
    QCOMPARE(std::get<SurfaceTarget>(*model.selectedCommand()), betaTarget);
}

void WindowUiControllerTest::commandPaletteActivationCapturesBeforeClose()
{
    WindowUiController controller;
    const SurfaceTarget selectedTarget = target(7, 11);
    controller.replaceCommandPaletteRows({
        configuredRow(QStringLiteral("Alpha"), QStringLiteral("alpha"),
                      QStringLiteral("text:alpha")),
        jumpRow(QStringLiteral("Beta"), selectedTarget),
    });
    controller.commandPaletteModel()->setSelectedIndex(1);

    std::optional<CommandPaletteCommand> activated;
    WindowUiController::Modal modalDuringActivation =
        WindowUiController::Modal::CommandPalette;
    controller.setCommandPaletteActivationCallback(
        [&](CommandPaletteCommand command) {
            modalDuringActivation = controller.modal();
            activated = std::move(command);
        });

    controller.showCommandPalette();
    controller.closeModal();
    // Simulate a dynamic catalog rebuild during popup focus restoration. The
    // delayed activation still consumes the command captured on close.
    controller.replaceCommandPaletteRows({
        configuredRow(QStringLiteral("Other"), QStringLiteral("other"),
                      QStringLiteral("text:other")),
    });
    QVERIFY(controller.activateSelectedCommand());
    QCOMPARE(modalDuringActivation, WindowUiController::Modal::None);
    QVERIFY(activated.has_value());
    QCOMPARE(std::get<SurfaceTarget>(*activated), selectedTarget);
    QVERIFY(!controller.activateSelectedCommand());
}

void WindowUiControllerTest::commandPaletteRefreshesImmediatelyBeforeOpening()
{
    WindowUiController controller;
    int refreshCount = 0;
    bool visibleDuringRefresh = true;
    controller.setCommandPaletteRefreshCallback([&] {
        ++refreshCount;
        visibleDuringRefresh = controller.commandPaletteVisible();
        controller.replaceCommandPaletteRows({
            jumpRow(QStringLiteral("Live target"),
                    target(1, static_cast<PaneId::Value>(refreshCount))),
        });
    });

    controller.showCommandPalette();
    QCOMPARE(refreshCount, 1);
    QVERIFY(!visibleDuringRefresh);
    QVERIFY(controller.commandPaletteVisible());
    QCOMPARE(std::get<SurfaceTarget>(
                 *controller.commandPaletteModel()->selectedCommand()),
             target(1, 1));

    // Showing an already-open palette is not another open operation.
    controller.showCommandPalette();
    QCOMPARE(refreshCount, 1);

    controller.toggleCommandPalette();
    QVERIFY(!controller.commandPaletteVisible());
    QCOMPARE(refreshCount, 1);
    controller.toggleCommandPalette();
    QCOMPARE(refreshCount, 2);
    QVERIFY(!visibleDuringRefresh);
    QCOMPARE(std::get<SurfaceTarget>(
                 *controller.commandPaletteModel()->selectedCommand()),
             target(1, 2));
}

void WindowUiControllerTest::commandPaletteActivationMayDestroyController()
{
    auto controller = std::make_unique<WindowUiController>();
    controller->replaceCommandPaletteRows({
        configuredRow(QStringLiteral("Close window"), QStringLiteral("close"),
                      QStringLiteral("close_surface")),
    });
    WindowUiController *const rawController = controller.get();
    bool called = false;
    controller->setCommandPaletteActivationCallback(
        [&](CommandPaletteCommand command) {
            called = true;
            QCOMPARE(std::get<QString>(command),
                     QStringLiteral("close_surface"));
            controller.reset();
        });
    rawController->showCommandPalette();
    rawController->closeModal();
    QVERIFY(rawController->activateSelectedCommand());
    QVERIFY(called);
    QVERIFY(controller == nullptr);
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
    model.replaceEntries(commands);
    QVERIFY(reset.isEmpty());
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

void WindowUiControllerTest::
    configurationDiagnosticsDeduplicateAndRetryInPlace()
{
    WindowUiController controller;
    QSignalSpy diagnosticsChanged(
        &controller, &WindowUiController::configurationDiagnosticsChanged);
    QSignalSpy visibilityChanged(
        &controller,
        &WindowUiController::configurationDiagnosticsVisibleChanged);
    int retries = 0;
    controller.setConfigurationRetryCallback([&retries] { ++retries; });

    const QString first =
        QStringLiteral("Ghostty configuration:\ninvalid shared option");
    controller.setConfigurationDiagnostics(first);
    QCOMPARE(controller.configurationDiagnosticsText(), first);
    QCOMPARE(controller.modal(),
             WindowUiController::Modal::ConfigurationDiagnostics);
    QVERIFY(controller.configurationDiagnosticsVisible());
    QCOMPARE(diagnosticsChanged.count(), 1);
    QCOMPARE(visibilityChanged.count(), 1);

    controller.setConfigurationDiagnostics(first);
    QCOMPARE(diagnosticsChanged.count(), 1);
    QCOMPARE(visibilityChanged.count(), 1);
    QVERIFY(controller.retryConfigurationDiagnostics());
    QCOMPARE(retries, 1);
    QVERIFY(controller.configurationDiagnosticsVisible());
    QCOMPARE(controller.configurationDiagnosticsText(), first);

    controller.ignoreConfigurationDiagnostics();
    QVERIFY(!controller.configurationDiagnosticsVisible());
    QCOMPARE(controller.modal(), WindowUiController::Modal::None);
    QCOMPARE(visibilityChanged.count(), 2);
    QVERIFY(!controller.retryConfigurationDiagnostics());
    controller.setConfigurationDiagnostics(first);
    QVERIFY(!controller.configurationDiagnosticsVisible());
    QCOMPARE(diagnosticsChanged.count(), 1);
    QCOMPARE(visibilityChanged.count(), 2);

    const QString second = QStringLiteral(
        "ghostty-qt frontend configuration:\ninvalid frontend option");
    controller.setConfigurationDiagnostics(second);
    QCOMPARE(controller.configurationDiagnosticsText(), second);
    QVERIFY(controller.configurationDiagnosticsVisible());
    QCOMPARE(diagnosticsChanged.count(), 2);
    QCOMPARE(visibilityChanged.count(), 3);

    controller.setConfigurationDiagnostics({});
    QVERIFY(controller.configurationDiagnosticsText().isEmpty());
    QVERIFY(!controller.configurationDiagnosticsVisible());
    QCOMPARE(controller.modal(), WindowUiController::Modal::None);
    QCOMPARE(diagnosticsChanged.count(), 3);
    QCOMPARE(visibilityChanged.count(), 4);

    // A successful generation resets deduplication: the same text is a new
    // failure cycle and must surface again.
    controller.setConfigurationDiagnostics(first);
    QVERIFY(controller.configurationDiagnosticsVisible());
    QCOMPARE(diagnosticsChanged.count(), 4);
    QCOMPARE(visibilityChanged.count(), 5);
}

void WindowUiControllerTest::notificationsHaveExactTextAndDuration()
{
    WindowUiController controller;

    controller.notifyClipboardCopied(false);
    QCOMPARE(controller.toastMessage(), QStringLiteral("Copied to clipboard"));
    QCOMPARE(controller.toastDurationMilliseconds(), 4000);

    QVERIFY(controller.expireToast());
    controller.notifyClipboardCopied(true);
    QCOMPARE(controller.toastMessage(), QStringLiteral("Cleared clipboard"));
    QCOMPARE(controller.toastDurationMilliseconds(), 4000);

    QVERIFY(controller.expireToast());
    controller.notifyConfigurationReloaded();
    QCOMPARE(controller.toastMessage(),
             QStringLiteral("Reloaded the configuration"));
    QCOMPARE(controller.toastDurationMilliseconds(), 4000);
}

void WindowUiControllerTest::consecutiveDuplicateToastsAreCoalesced()
{
    WindowUiController controller;
    QSignalSpy toastChanged(&controller, &WindowUiController::toastChanged);
    QSignalSpy depthChanged(&controller,
                            &WindowUiController::toastQueueDepthChanged);

    controller.notifyConfigurationReloaded();
    controller.notifyConfigurationReloaded();
    QCOMPARE(controller.toastQueueDepth(), 1);
    QCOMPARE(controller.toastMessage(),
             QStringLiteral("Reloaded the configuration"));
    QCOMPARE(toastChanged.count(), 1);
    QCOMPARE(depthChanged.count(), 1);

    controller.notifyClipboardCopied(false);
    controller.notifyClipboardCopied(false);
    QCOMPARE(controller.toastQueueDepth(), 2);
    QCOMPARE(toastChanged.count(), 1);
    QCOMPARE(depthChanged.count(), 2);

    QVERIFY(controller.expireToast());
    QCOMPARE(controller.toastMessage(), QStringLiteral("Copied to clipboard"));
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
