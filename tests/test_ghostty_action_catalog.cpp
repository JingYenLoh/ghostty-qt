#include "ghostty_action_catalog.h"

#include <QTest>
#include <QtCore/qnamespace.h>

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
};

void GhosttyActionCatalogTest::translatesParameterlessActions()
{
    const WorkspaceActionContext source{TabId(17), PaneId(29), 123};

    const struct {
        const char *serialized;
        WorkspaceAction action;
        int value;
    } cases[] = {
        {"new_tab", WorkspaceAction::NewTab, 123},
        {"close_surface", WorkspaceAction::ClosePane, 123},
        {"close_tab", WorkspaceAction::CloseTab, 123},
        {"previous_tab", WorkspaceAction::ChangeTabRelative, -1},
        {"next_tab", WorkspaceAction::ChangeTabRelative, 1},
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
        QCOMPARE(result.actionName, serialized);
        QVERIFY(!result.parameter.has_value());
    }
}

void GhosttyActionCatalogTest::translatesParameterizedActions_data()
{
    QTest::addColumn<QString>("serialized");
    QTest::addColumn<WorkspaceAction>("action");
    QTest::addColumn<int>("value");

    QTest::newRow("close-tab-this")
        << QStringLiteral("close_tab:this") << WorkspaceAction::CloseTab << 700;
    QTest::newRow("split-right")
        << QStringLiteral("new_split:right") << WorkspaceAction::SplitRight << 700;
    QTest::newRow("split-down")
        << QStringLiteral("new_split:down") << WorkspaceAction::SplitDown << 700;
    QTest::newRow("focus-left")
        << QStringLiteral("goto_split:left") << WorkspaceAction::NavigatePane
        << int(Qt::Key_Left);
    QTest::newRow("focus-right")
        << QStringLiteral("goto_split:right") << WorkspaceAction::NavigatePane
        << int(Qt::Key_Right);
    QTest::newRow("focus-up")
        << QStringLiteral("goto_split:up") << WorkspaceAction::NavigatePane
        << int(Qt::Key_Up);
    QTest::newRow("focus-down")
        << QStringLiteral("goto_split:down") << WorkspaceAction::NavigatePane
        << int(Qt::Key_Down);
    QTest::newRow("focus-top-alias")
        << QStringLiteral("goto_split:top") << WorkspaceAction::NavigatePane
        << int(Qt::Key_Up);
    QTest::newRow("focus-bottom-alias")
        << QStringLiteral("goto_split:bottom") << WorkspaceAction::NavigatePane
        << int(Qt::Key_Down);
}

void GhosttyActionCatalogTest::translatesParameterizedActions()
{
    QFETCH(QString, serialized);
    QFETCH(WorkspaceAction, action);
    QFETCH(int, value);

    const WorkspaceActionContext source{TabId(9), PaneId(12), 700};
    const GhosttyActionTranslation result =
        GhosttyActionCatalog::translate(serialized, source);

    QVERIFY(result.accepted());
    QVERIFY(result.request.has_value());
    QCOMPARE(result.request->action, action);
    QCOMPARE(result.request->context.tabId, source.tabId);
    QCOMPARE(result.request->context.paneId, source.paneId);
    QCOMPARE(result.request->context.value, value);
}

void GhosttyActionCatalogTest::preservesRawParametersAndTargetContext()
{
    const WorkspaceActionContext source{TabId(41), PaneId(73), 0};
    const GhosttyActionTranslation result =
        GhosttyActionCatalog::translate(QStringLiteral("goto_split:top"), source);

    QVERIFY(result.accepted());
    QCOMPARE(result.actionName, QStringLiteral("goto_split"));
    QVERIFY(result.parameter.has_value());
    QCOMPARE(*result.parameter, QStringLiteral("top"));
    QCOMPARE(result.request->context.tabId, source.tabId);
    QCOMPARE(result.request->context.paneId, source.paneId);
    QCOMPARE(result.request->context.value, int(Qt::Key_Up));
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
    QTest::newRow("focus-previous") << QStringLiteral("goto_split:previous") << true;
    QTest::newRow("focus-next") << QStringLiteral("goto_split:next") << true;
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
    QTest::newRow("unknown-action")
        << QStringLiteral("new_window") << Error::UnsupportedAction;
    QTest::newRow("leading-space")
        << QStringLiteral(" new_tab") << Error::UnsupportedAction;
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

QTEST_APPLESS_MAIN(GhosttyActionCatalogTest)

#include "test_ghostty_action_catalog.moc"
