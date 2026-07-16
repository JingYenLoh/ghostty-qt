#include "ghostty_action_catalog.h"

#include <QTest>
#include <QtCore/qnamespace.h>

#include <limits>

class GhosttyActionCatalogTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void translatesParameterlessActions();
    void translatesParameterizedActions_data();
    void translatesParameterizedActions();
    void preservesRawParametersAndTargetContext();
    void rejectsValidButUnsupportedParameters_data();
    void rejectsValidButUnsupportedParameters();
    void rejectsMalformedAndUnsupportedStrings_data();
    void rejectsMalformedAndUnsupportedStrings();
    void matchesPinnedIntegerParsing_data();
    void matchesPinnedIntegerParsing();
    void classifiesPinnedActionScopes();
    void recognizesKeyTableActions();
};

void GhosttyActionCatalogTest::translatesParameterlessActions()
{
    const WorkspaceActionContext source{TabId(17), PaneId(29), 123, 456};

    const struct {
        const char *serialized;
        WorkspaceAction action;
        qint64 value;
    } cases[] = {
        {"new_tab", WorkspaceAction::NewTab, 123},
        {"close_surface", WorkspaceAction::ClosePane, 123},
        {"close_tab", WorkspaceAction::CloseTab, 123},
        {"previous_tab", WorkspaceAction::ChangeTabRelative, -1},
        {"next_tab", WorkspaceAction::ChangeTabRelative, 1},
        {"last_tab", WorkspaceAction::ActivateLastTab, 123},
        {"equalize_splits", WorkspaceAction::EqualizeSplits, 123},
        {"toggle_split_zoom", WorkspaceAction::ToggleSplitZoom, 123},
        {"quit", WorkspaceAction::RequestQuit, 123},
    };

    for (const auto &testCase : cases) {
        const QString serialized = QString::fromLatin1(testCase.serialized);
        const GhosttyActionTranslation result =
            GhosttyActionCatalog::translate(serialized, source);

        QVERIFY2(result.accepted(), testCase.serialized);
        QCOMPARE(result.error, GhosttyActionTranslationError::None);
        QVERIFY(result.request.has_value());
        QCOMPARE(result.request->action, testCase.action);
        QCOMPARE(result.request->context.tabId, source.tabId);
        QCOMPARE(result.request->context.paneId, source.paneId);
        QCOMPARE(result.request->context.value, testCase.value);
        QCOMPARE(result.request->context.amount, source.amount);
        QCOMPARE(result.actionName, serialized);
        QVERIFY(!result.parameter.has_value());
    }

    QVERIFY(GhosttyActionCatalog::isImplemented(
        QStringLiteral("end_key_sequence")));
    QVERIFY(!GhosttyActionCatalog::isImplemented(
        QStringLiteral("end_key_sequence:now")));
}

void GhosttyActionCatalogTest::translatesParameterizedActions_data()
{
    QTest::addColumn<QString>("serialized");
    QTest::addColumn<WorkspaceAction>("action");
    QTest::addColumn<qint64>("value");
    QTest::addColumn<int>("amount");

    QTest::newRow("close-tab-this")
        << QStringLiteral("close_tab:this") << WorkspaceAction::CloseTab
        << qint64(700) << 0;
    QTest::newRow("split-right")
        << QStringLiteral("new_split:right") << WorkspaceAction::SplitRight
        << qint64(700) << 0;
    QTest::newRow("split-down")
        << QStringLiteral("new_split:down") << WorkspaceAction::SplitDown
        << qint64(700) << 0;
    QTest::newRow("goto-third-tab")
        << QStringLiteral("goto_tab:3") << WorkspaceAction::ActivateTabByIndex
        << qint64(3) << 0;
    QTest::newRow("move-tab-back-two")
        << QStringLiteral("move_tab:-2") << WorkspaceAction::MoveTab
        << qint64(-2) << 0;
    QTest::newRow("focus-left")
        << QStringLiteral("goto_split:left") << WorkspaceAction::NavigatePane
        << qint64(Qt::Key_Left) << 0;
    QTest::newRow("focus-right")
        << QStringLiteral("goto_split:right") << WorkspaceAction::NavigatePane
        << qint64(Qt::Key_Right) << 0;
    QTest::newRow("focus-up")
        << QStringLiteral("goto_split:up") << WorkspaceAction::NavigatePane
        << qint64(Qt::Key_Up) << 0;
    QTest::newRow("focus-down")
        << QStringLiteral("goto_split:down") << WorkspaceAction::NavigatePane
        << qint64(Qt::Key_Down) << 0;
    QTest::newRow("focus-top-alias")
        << QStringLiteral("goto_split:top") << WorkspaceAction::NavigatePane
        << qint64(Qt::Key_Up) << 0;
    QTest::newRow("focus-bottom-alias")
        << QStringLiteral("goto_split:bottom") << WorkspaceAction::NavigatePane
        << qint64(Qt::Key_Down) << 0;
    QTest::newRow("focus-previous")
        << QStringLiteral("goto_split:previous")
        << WorkspaceAction::NavigatePaneRelative << qint64(-1) << 0;
    QTest::newRow("focus-next")
        << QStringLiteral("goto_split:next")
        << WorkspaceAction::NavigatePaneRelative << qint64(1) << 0;
    QTest::newRow("resize-up")
        << QStringLiteral("resize_split:up,10") << WorkspaceAction::ResizeSplit
        << qint64(Qt::Key_Up) << 10;
    QTest::newRow("resize-down")
        << QStringLiteral("resize_split:down,11")
        << WorkspaceAction::ResizeSplit << qint64(Qt::Key_Down) << 11;
    QTest::newRow("resize-left")
        << QStringLiteral("resize_split:left,12")
        << WorkspaceAction::ResizeSplit << qint64(Qt::Key_Left) << 12;
    QTest::newRow("resize-right")
        << QStringLiteral("resize_split:right,13")
        << WorkspaceAction::ResizeSplit << qint64(Qt::Key_Right) << 13;
}

void GhosttyActionCatalogTest::translatesParameterizedActions()
{
    QFETCH(QString, serialized);
    QFETCH(WorkspaceAction, action);
    QFETCH(qint64, value);
    QFETCH(int, amount);

    const WorkspaceActionContext source{TabId(9), PaneId(12), 700, 0};
    const GhosttyActionTranslation result =
        GhosttyActionCatalog::translate(serialized, source);

    QVERIFY(result.accepted());
    QVERIFY(result.request.has_value());
    QCOMPARE(result.request->action, action);
    QCOMPARE(result.request->context.tabId, source.tabId);
    QCOMPARE(result.request->context.paneId, source.paneId);
    QCOMPARE(result.request->context.value, value);
    QCOMPARE(result.request->context.amount, amount);
}

void GhosttyActionCatalogTest::preservesRawParametersAndTargetContext()
{
    const WorkspaceActionContext source{TabId(41), PaneId(73), 0, 22};
    const GhosttyActionTranslation result =
        GhosttyActionCatalog::translate(QStringLiteral("goto_split:top"), source);

    QVERIFY(result.accepted());
    QCOMPARE(result.actionName, QStringLiteral("goto_split"));
    QVERIFY(result.parameter.has_value());
    QCOMPARE(*result.parameter, QStringLiteral("top"));
    QCOMPARE(result.request->context.tabId, source.tabId);
    QCOMPARE(result.request->context.paneId, source.paneId);
    QCOMPARE(result.request->context.value, qint64(Qt::Key_Up));
    QCOMPARE(result.request->context.amount, source.amount);
}

void GhosttyActionCatalogTest::rejectsValidButUnsupportedParameters_data()
{
    QTest::addColumn<QString>("serialized");
    QTest::addColumn<bool>("hasParameter");

    // These values are all valid in the pinned Binding.zig, but the current
    // workspace does not yet implement their behavior.
    QTest::newRow("close-other") << QStringLiteral("close_tab:other") << true;
    QTest::newRow("close-right") << QStringLiteral("close_tab:right") << true;
    QTest::newRow("split-default-auto") << QStringLiteral("new_split") << false;
    QTest::newRow("split-auto") << QStringLiteral("new_split:auto") << true;
    QTest::newRow("split-left") << QStringLiteral("new_split:left") << true;
    QTest::newRow("split-up") << QStringLiteral("new_split:up") << true;
}

void GhosttyActionCatalogTest::rejectsValidButUnsupportedParameters()
{
    QFETCH(QString, serialized);
    QFETCH(bool, hasParameter);

    const GhosttyActionTranslation result =
        GhosttyActionCatalog::translate(serialized);

    QVERIFY(!result.accepted());
    QVERIFY(!result.request.has_value());
    QCOMPARE(result.error,
             GhosttyActionTranslationError::UnsupportedParameter);
    QCOMPARE(result.parameter.has_value(), hasParameter);
}

void GhosttyActionCatalogTest::rejectsMalformedAndUnsupportedStrings_data()
{
    QTest::addColumn<QString>("serialized");
    QTest::addColumn<GhosttyActionTranslationError>("error");

    using Error = GhosttyActionTranslationError;
    QTest::newRow("empty") << QString{} << Error::InvalidFormat;
    QTest::newRow("empty-name") << QStringLiteral(":right") << Error::InvalidFormat;
    QTest::newRow("void-empty-parameter")
        << QStringLiteral("new_tab:") << Error::InvalidFormat;
    QTest::newRow("void-parameter")
        << QStringLiteral("quit:now") << Error::InvalidFormat;
    QTest::newRow("missing-required-parameter")
        << QStringLiteral("goto_split") << Error::InvalidFormat;
    QTest::newRow("empty-enum")
        << QStringLiteral("goto_split:") << Error::InvalidFormat;
    QTest::newRow("bad-split-enum")
        << QStringLiteral("new_split:RIGHT") << Error::InvalidFormat;
    QTest::newRow("extra-colon")
        << QStringLiteral("new_split:right:down") << Error::InvalidFormat;
    QTest::newRow("bad-close-mode")
        << QStringLiteral("close_tab:This") << Error::InvalidFormat;
    QTest::newRow("missing-tab-index")
        << QStringLiteral("goto_tab") << Error::InvalidFormat;
    QTest::newRow("negative-tab-index")
        << QStringLiteral("goto_tab:-1") << Error::InvalidFormat;
    QTest::newRow("tab-index-overflow")
        << QStringLiteral("goto_tab:18446744073709551616")
        << Error::InvalidFormat;
    QTest::newRow("hex-tab-index")
        << QStringLiteral("goto_tab:0x2") << Error::InvalidFormat;
    QTest::newRow("missing-tab-offset")
        << QStringLiteral("move_tab") << Error::InvalidFormat;
    QTest::newRow("tab-offset-overflow")
        << QStringLiteral("move_tab:9223372036854775808")
        << Error::InvalidFormat;
    QTest::newRow("hex-tab-offset")
        << QStringLiteral("move_tab:-0x1") << Error::InvalidFormat;
    QTest::newRow("missing-resize-amount")
        << QStringLiteral("resize_split:up") << Error::InvalidFormat;
    QTest::newRow("extra-resize-component")
        << QStringLiteral("resize_split:up,10,12") << Error::InvalidFormat;
    QTest::newRow("bad-resize-direction")
        << QStringLiteral("resize_split:top,10") << Error::InvalidFormat;
    QTest::newRow("resize-amount-overflow")
        << QStringLiteral("resize_split:up,65536") << Error::InvalidFormat;
    QTest::newRow("binary-resize-amount")
        << QStringLiteral("resize_split:right,0b1010")
        << Error::InvalidFormat;
    QTest::newRow("void-layout-parameter")
        << QStringLiteral("equalize_splits:") << Error::InvalidFormat;
    QTest::newRow("unknown-action")
        << QStringLiteral("new_window") << Error::UnsupportedAction;
    QTest::newRow("leading-space")
        << QStringLiteral(" new_tab") << Error::UnsupportedAction;
}

void GhosttyActionCatalogTest::matchesPinnedIntegerParsing_data()
{
    QTest::addColumn<QString>("serialized");
    QTest::addColumn<WorkspaceAction>("action");
    QTest::addColumn<qint64>("value");
    QTest::addColumn<int>("amount");

    QTest::newRow("unsigned-plus")
        << QStringLiteral("goto_tab:+12")
        << WorkspaceAction::ActivateTabByIndex << qint64(12) << 0;
    QTest::newRow("unsigned-negative-zero")
        << QStringLiteral("goto_tab:-0")
        << WorkspaceAction::ActivateTabByIndex << qint64(0) << 0;
    QTest::newRow("unsigned-interior-underscores")
        << QStringLiteral("goto_tab:1__2")
        << WorkspaceAction::ActivateTabByIndex << qint64(12) << 0;
    QTest::newRow("unsigned-linux-max")
        << QStringLiteral("goto_tab:")
               + QString::number(std::numeric_limits<quintptr>::max())
        << WorkspaceAction::ActivateTabByIndex
        << static_cast<qint64>(std::numeric_limits<quintptr>::max()
                                  > static_cast<quintptr>(
                                      std::numeric_limits<qint64>::max())
                              ? std::numeric_limits<qint64>::max()
                              : std::numeric_limits<quintptr>::max())
        << 0;
    QTest::newRow("signed-linux-min")
        << QStringLiteral("move_tab:")
               + QString::number(std::numeric_limits<qintptr>::min())
        << WorkspaceAction::MoveTab
        << static_cast<qint64>(std::numeric_limits<qintptr>::min()) << 0;
    QTest::newRow("signed-linux-max")
        << QStringLiteral("move_tab:+")
               + QString::number(std::numeric_limits<qintptr>::max())
        << WorkspaceAction::MoveTab
        << static_cast<qint64>(std::numeric_limits<qintptr>::max()) << 0;
    QTest::newRow("resize-u16-max")
        << QStringLiteral("resize_split:right,+65_535")
        << WorkspaceAction::ResizeSplit << qint64(Qt::Key_Right) << 65535;
    QTest::newRow("resize-negative-zero")
        << QStringLiteral("resize_split:left,-0")
        << WorkspaceAction::ResizeSplit << qint64(Qt::Key_Left) << 0;
}

void GhosttyActionCatalogTest::matchesPinnedIntegerParsing()
{
    QFETCH(QString, serialized);
    QFETCH(WorkspaceAction, action);
    QFETCH(qint64, value);
    QFETCH(int, amount);

    const GhosttyActionTranslation result =
        GhosttyActionCatalog::translate(serialized);

    QVERIFY(result.accepted());
    QCOMPARE(result.error, GhosttyActionTranslationError::None);
    QCOMPARE(result.request->action, action);
    QCOMPARE(result.request->context.value, value);
    QCOMPARE(result.request->context.amount, amount);
    QVERIFY(GhosttyActionCatalog::isImplemented(serialized));
}

void GhosttyActionCatalogTest::rejectsMalformedAndUnsupportedStrings()
{
    QFETCH(QString, serialized);
    QFETCH(GhosttyActionTranslationError, error);

    const GhosttyActionTranslation first =
        GhosttyActionCatalog::translate(serialized);
    const GhosttyActionTranslation second =
        GhosttyActionCatalog::translate(serialized);

    QVERIFY(!first.accepted());
    QVERIFY(!first.request.has_value());
    QCOMPARE(first.error, error);
    QCOMPARE(second.error, first.error);
    QCOMPARE(second.actionName, first.actionName);
    QCOMPARE(second.parameter, first.parameter);
}

void GhosttyActionCatalogTest::classifiesPinnedActionScopes()
{
    QCOMPARE(GhosttyActionCatalog::scope(QStringLiteral("quit")),
             GhosttyActionScope::Application);
    QCOMPARE(GhosttyActionCatalog::scope(QStringLiteral("reload_config")),
             GhosttyActionScope::Application);
    QCOMPARE(GhosttyActionCatalog::scope(QStringLiteral("new_window")),
             GhosttyActionScope::Application);
    QCOMPARE(GhosttyActionCatalog::scope(QStringLiteral("new_tab")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(QStringLiteral("close_tab:this")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(QStringLiteral("goto_tab:2")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(
                 QStringLiteral("toggle_split_zoom")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(
                 QStringLiteral("activate_key_table:copy")),
             GhosttyActionScope::Surface);
}

void GhosttyActionCatalogTest::recognizesKeyTableActions()
{
    QVERIFY(GhosttyActionCatalog::isImplemented(
        QStringLiteral("activate_key_table:copy")));
    QVERIFY(GhosttyActionCatalog::isImplemented(
        QStringLiteral("activate_key_table_once:")));
    QVERIFY(GhosttyActionCatalog::isImplemented(
        QStringLiteral("deactivate_key_table")));
    QVERIFY(GhosttyActionCatalog::isImplemented(
        QStringLiteral("deactivate_all_key_tables")));
    QVERIFY(!GhosttyActionCatalog::isImplemented(
        QStringLiteral("activate_key_table")));
    QVERIFY(!GhosttyActionCatalog::isImplemented(
        QStringLiteral("deactivate_key_table:copy")));
}

QTEST_APPLESS_MAIN(GhosttyActionCatalogTest)

#include "test_ghostty_action_catalog.moc"
