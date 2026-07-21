#include "ghostty_action_catalog.h"

#include <QTest>
#include <QtCore/qnamespace.h>

#include <cmath>
#include <limits>

class GhosttyActionCatalogTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void tokenizesSerializedActions();
    void translatesParameterlessActions();
    void translatesParameterizedActions_data();
    void translatesParameterizedActions();
    void preservesRawParametersAndTargetContext();
    void translatesTitleActions();
    void rejectsMalformedAndUnsupportedStrings_data();
    void rejectsMalformedAndUnsupportedStrings();
    void matchesPinnedIntegerParsing_data();
    void matchesPinnedIntegerParsing();
    void parsesPaneActions();
    void parsesFontSizePaneActions();
    void parsesSearchPaneActions();
    void rejectsMalformedPaneActions();
    void rejectsMalformedFontSizePaneActions();
    void rejectsMalformedSearchPaneActions();
    void parsesApplicationActionsExactly();
    void classifiesPinnedActionScopes();
    void recognizesKeyTableActions();
};

void GhosttyActionCatalogTest::tokenizesSerializedActions()
{
    const QString parameterlessSource = QStringLiteral("reload_config");
    const GhosttySerializedActionView parameterless =
        GhosttyActionCatalog::parseSerializedAction(
            parameterlessSource);
    QCOMPARE(parameterless.name.toString(), QStringLiteral("reload_config"));
    QVERIFY(!parameterless.parameter.has_value());

    const QString emptyParameterSource = QStringLiteral("reload_config:");
    const GhosttySerializedActionView emptyParameter =
        GhosttyActionCatalog::parseSerializedAction(
            emptyParameterSource);
    QCOMPARE(emptyParameter.name.toString(), QStringLiteral("reload_config"));
    QVERIFY(emptyParameter.parameter.has_value());
    QVERIFY(emptyParameter.parameter->isEmpty());

    const QString embeddedColonsSource =
        QStringLiteral("csi:38:2:255:0:0m");
    const GhosttySerializedActionView embeddedColons =
        GhosttyActionCatalog::parseSerializedAction(
            embeddedColonsSource);
    QCOMPARE(embeddedColons.name.toString(), QStringLiteral("csi"));
    QVERIFY(embeddedColons.parameter.has_value());
    QCOMPARE(embeddedColons.parameter->toString(),
             QStringLiteral("38:2:255:0:0m"));
}

void GhosttyActionCatalogTest::translatesParameterlessActions()
{
    const WorkspaceActionContext source{TabId(17), PaneId(29), 123, 456};

    const struct {
        const char *serialized;
        WorkspaceAction action;
        qint64 value;
    } cases[] = {
        {"new_tab", WorkspaceAction::NewTab, 123},
        {"new_split", WorkspaceAction::SplitAuto, 123},
        {"close_surface", WorkspaceAction::ClosePane, 123},
        {"close_tab", WorkspaceAction::CloseTab, 123},
        {"previous_tab", WorkspaceAction::ChangeTabRelative, -1},
        {"next_tab", WorkspaceAction::ChangeTabRelative, 1},
        {"last_tab", WorkspaceAction::ActivateLastTab, 123},
        {"equalize_splits", WorkspaceAction::EqualizeSplits, 123},
        {"toggle_split_zoom", WorkspaceAction::ToggleSplitZoom, 123},
        {"toggle_fullscreen", WorkspaceAction::ToggleFullscreen, 123},
        {"prompt_surface_title", WorkspaceAction::PromptSurfaceTitle, 123},
        {"prompt_tab_title", WorkspaceAction::PromptTabTitle, 123},
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
        if (testCase.action == WorkspaceAction::CloseTab) {
            QCOMPARE(result.request->context.closeTabMode,
                     CloseTabMode::This);
        }
        QCOMPARE(result.actionName, serialized);
        QVERIFY(!result.parameter.has_value());
    }

    QVERIFY(GhosttyActionCatalog::isImplemented(
        QStringLiteral("end_key_sequence")));
    QVERIFY(!GhosttyActionCatalog::isImplemented(
        QStringLiteral("end_key_sequence:now")));
    QVERIFY(GhosttyActionCatalog::isImplemented(
        QStringLiteral("copy_url_to_clipboard")));
    QVERIFY(!GhosttyActionCatalog::isImplemented(
        QStringLiteral("copy_url_to_clipboard:")));
    QVERIFY(GhosttyActionCatalog::isImplemented(
        QStringLiteral("copy_title_to_clipboard")));
    QVERIFY(!GhosttyActionCatalog::isImplemented(
        QStringLiteral("copy_title_to_clipboard:")));
    QVERIFY(!GhosttyActionCatalog::isImplemented(
        QStringLiteral("copy_title_to_clipboard:ignored")));
    QVERIFY(GhosttyActionCatalog::isImplemented(
        QStringLiteral("prompt_surface_title")));
    QVERIFY(!GhosttyActionCatalog::isImplemented(
        QStringLiteral("prompt_surface_title:")));
    QVERIFY(!GhosttyActionCatalog::isImplemented(
        QStringLiteral("prompt_surface_title:project")));
    QVERIFY(GhosttyActionCatalog::isImplemented(
        QStringLiteral("prompt_tab_title")));
    QVERIFY(!GhosttyActionCatalog::isImplemented(
        QStringLiteral("prompt_tab_title:")));
    QVERIFY(!GhosttyActionCatalog::isImplemented(
        QStringLiteral("prompt_tab_title:project")));
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
    QTest::newRow("close-tab-other")
        << QStringLiteral("close_tab:other") << WorkspaceAction::CloseTab
        << qint64(700) << 0;
    QTest::newRow("close-tab-right")
        << QStringLiteral("close_tab:right") << WorkspaceAction::CloseTab
        << qint64(700) << 0;
    QTest::newRow("split-left")
        << QStringLiteral("new_split:left") << WorkspaceAction::SplitLeft
        << qint64(700) << 0;
    QTest::newRow("split-right")
        << QStringLiteral("new_split:right") << WorkspaceAction::SplitRight
        << qint64(700) << 0;
    QTest::newRow("split-up")
        << QStringLiteral("new_split:up") << WorkspaceAction::SplitUp
        << qint64(700) << 0;
    QTest::newRow("split-down")
        << QStringLiteral("new_split:down") << WorkspaceAction::SplitDown
        << qint64(700) << 0;
    QTest::newRow("split-auto")
        << QStringLiteral("new_split:auto") << WorkspaceAction::SplitAuto
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
    const CloseTabMode closeTabMode =
        serialized == QLatin1StringView("close_tab:other")
        ? CloseTabMode::Other
        : serialized == QLatin1StringView("close_tab:right")
        ? CloseTabMode::Right
        : CloseTabMode::This;
    QCOMPARE(result.request->context.closeTabMode, closeTabMode);
    QVERIFY(GhosttyActionCatalog::isImplemented(serialized));
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

void GhosttyActionCatalogTest::translatesTitleActions()
{
    const WorkspaceActionContext source{TabId(41), PaneId(73), 9, 22};

    const struct {
        QString encoded;
        QString expected;
    } valid[] = {
        {QString{}, QString{}},
        {QStringLiteral("project:main"), QStringLiteral("project:main")},
        {QStringLiteral(R"(\xf0\x9f\x91\xbb workspace)"),
         QStringLiteral("👻 workspace")},
        {QStringLiteral(R"(path\\quoted\")"),
         QStringLiteral("path\\quoted\"")},
        {QStringLiteral(R"(line\nnext)"),
         QStringLiteral("line\nnext")},
        {QStringLiteral(R"(left\x00right)"),
         QString::fromUtf8("left\0right", 10)},
        {QStringLiteral("直接"), QStringLiteral("直接")},
    };

    const struct {
        QString name;
        WorkspaceAction action;
    } actions[] = {
        {QStringLiteral("set_surface_title"),
         WorkspaceAction::SetSurfaceTitle},
        {QStringLiteral("set_tab_title"), WorkspaceAction::SetTabTitle},
    };

    for (const auto &action : actions) {
        for (const auto &testCase : valid) {
            const QString input = action.name + u':' + testCase.encoded;
            QString serialized = input;
            const GhosttyActionTranslation result =
                GhosttyActionCatalog::translate(serialized, source);
            serialized.clear();

            QVERIFY2(result.accepted(), qPrintable(input));
            QCOMPARE(result.error, GhosttyActionTranslationError::None);
            QCOMPARE(result.request->action, action.action);
            QCOMPARE(result.request->context, source);
            QCOMPARE(result.request->payload, testCase.expected);
            QCOMPARE(result.actionName, action.name);
            QVERIFY(result.parameter.has_value());
            QVERIFY(GhosttyActionCatalog::isImplemented(input));
        }

        const QStringList invalid{
            action.name,
            action.name + QStringLiteral(R"(:bad\q)"),
            action.name + QStringLiteral(R"(:\xc3)"),
            action.name + QStringLiteral(R"(:\xff)"),
            action.name + QStringLiteral(R"(:\u{d800})"),
        };
        for (const QString &serialized : invalid) {
            const GhosttyActionTranslation result =
                GhosttyActionCatalog::translate(serialized, source);
            QVERIFY2(!result.accepted(), qPrintable(serialized));
            QCOMPARE(result.error,
                     GhosttyActionTranslationError::InvalidFormat);
            QVERIFY2(!GhosttyActionCatalog::isImplemented(serialized),
                     qPrintable(serialized));
        }
    }
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
    QTest::newRow("close-surface-empty-parameter")
        << QStringLiteral("close_surface:") << Error::InvalidFormat;
    QTest::newRow("close-surface-parameter")
        << QStringLiteral("close_surface:active") << Error::InvalidFormat;
    QTest::newRow("empty-split-direction")
        << QStringLiteral("new_split:") << Error::InvalidFormat;
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
    QTest::newRow("empty-close-mode")
        << QStringLiteral("close_tab:") << Error::InvalidFormat;
    QTest::newRow("extra-close-component")
        << QStringLiteral("close_tab:right:other") << Error::InvalidFormat;
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
    QTest::newRow("fullscreen-empty-parameter")
        << QStringLiteral("toggle_fullscreen:") << Error::InvalidFormat;
    QTest::newRow("fullscreen-parameter")
        << QStringLiteral("toggle_fullscreen:true") << Error::InvalidFormat;
    QTest::newRow("fullscreen-case")
        << QStringLiteral("Toggle_fullscreen") << Error::UnsupportedAction;
    QTest::newRow("application-action-is-not-workspace-action")
        << QStringLiteral("new_window") << Error::UnsupportedAction;
    QTest::newRow("leading-space")
        << QStringLiteral(" new_tab") << Error::UnsupportedAction;
}

void GhosttyActionCatalogTest::parsesApplicationActionsExactly()
{
    const struct {
        const char *serialized;
        ApplicationAction action;
    } accepted[] = {
        {"ignore", ApplicationAction::Ignore},
        {"new_window", ApplicationAction::NewWindow},
        {"open_config", ApplicationAction::OpenConfig},
        {"reload_config", ApplicationAction::ReloadConfig},
        {"quit", ApplicationAction::Quit},
    };
    for (const auto &testCase : accepted) {
        const QString serialized = QString::fromLatin1(testCase.serialized);
        QCOMPARE(GhosttyActionCatalog::parseApplicationAction(serialized),
                 std::optional{testCase.action});
        QVERIFY(GhosttyActionCatalog::isImplemented(serialized));
    }

    for (const QString &rejected : {
             QStringLiteral("ignore:"),
             QStringLiteral("ignore:anything"),
             QStringLiteral("new_window:"),
             QStringLiteral("new_window:now"),
             QStringLiteral("open_config:"),
             QStringLiteral("open_config:now"),
             QStringLiteral("reload_config:"),
             QStringLiteral("reload_config:soft"),
             QStringLiteral("quit:"),
             QStringLiteral("quit:now"),
             QStringLiteral("Quit"),
         }) {
        QVERIFY(!GhosttyActionCatalog::parseApplicationAction(rejected));
        QVERIFY(!GhosttyActionCatalog::isImplemented(rejected));
    }
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

void GhosttyActionCatalogTest::parsesPaneActions()
{
    const auto parse = [](const char *serialized) {
        const QString action = QString::fromLatin1(serialized);
        const std::optional<GhosttyPaneAction> parsed =
            GhosttyActionCatalog::parsePaneAction(action);
        return parsed.value();
    };

    QCOMPARE(parse("scroll_to_top").viewport.kind,
             TerminalViewportRequest::Kind::Top);
    QCOMPARE(parse("scroll_to_bottom").viewport.kind,
             TerminalViewportRequest::Kind::Bottom);
    QCOMPARE(parse("scroll_to_selection").viewport.kind,
             TerminalViewportRequest::Kind::Selection);

    const GhosttyPaneAction row = parse("scroll_to_row:+1__2");
    QCOMPARE(row.kind, GhosttyPaneActionKind::ScrollViewport);
    QCOMPARE(row.viewport.kind, TerminalViewportRequest::Kind::Row);
    QCOMPARE(row.viewport.row, quint64(12));
    QCOMPARE(parse("scroll_to_row:-0").viewport.row, quint64(0));

    QCOMPARE(parse("scroll_page_up").kind,
             GhosttyPaneActionKind::ScrollPageUp);
    QCOMPARE(parse("scroll_page_down").kind,
             GhosttyPaneActionKind::ScrollPageDown);

    const GhosttyPaneAction fraction =
        parse("scroll_page_fractional:+0.5");
    QCOMPARE(fraction.kind,
             GhosttyPaneActionKind::ScrollPageFractional);
    QCOMPARE(fraction.pageFraction, 0.5F);
    QCOMPARE(parse("scroll_page_fractional:0x1p-1").pageFraction, 0.5F);
    QCOMPARE(parse("scroll_page_fractional:1e-1000").pageFraction, 0.0F);

    const GhosttyPaneAction lines = parse("scroll_page_lines:-32_768");
    QCOMPARE(lines.kind, GhosttyPaneActionKind::ScrollViewport);
    QCOMPARE(lines.viewport.kind, TerminalViewportRequest::Kind::Delta);
    QCOMPARE(lines.viewport.delta, qint64(-32768));
    QCOMPARE(parse("scroll_page_lines:+32_767").viewport.delta,
             qint64(32767));

    QCOMPARE(parse("select_all").kind, GhosttyPaneActionKind::SelectAll);

    const GhosttyPaneAction csi = parse("csi:38:2:255:0:0m");
    QCOMPARE(csi.kind, GhosttyPaneActionKind::Csi);
    QCOMPARE(csi.payload, QStringLiteral("38:2:255:0:0m"));
    QCOMPARE(parse("csi:").payload, QString{});

    const GhosttyPaneAction esc = parse("esc:]0:title:detail\a");
    QCOMPARE(esc.kind, GhosttyPaneActionKind::Esc);
    QCOMPARE(esc.payload, QStringLiteral("]0:title:detail\a"));
    QCOMPARE(parse("esc:").payload, QString{});

    const GhosttyPaneAction text = parse(R"(text:hello\n\x00world)");
    QCOMPARE(text.kind, GhosttyPaneActionKind::Text);
    QCOMPARE(text.payload, QStringLiteral(R"(hello\n\x00world)"));
    QCOMPARE(parse("text:").payload, QString{});

    // Binding.Action.parse intentionally defers Zig-literal validation until
    // action execution, where a malformed literal is consumed but writes no
    // bytes.
    QCOMPARE(parse(R"(text:\q)").payload, QStringLiteral(R"(\q)"));
    QCOMPARE(parse("reset").kind, GhosttyPaneActionKind::Reset);

    const GhosttyPaneAction readOnly = parse("toggle_readonly");
    QCOMPARE(readOnly.kind, GhosttyPaneActionKind::ToggleReadOnly);
    QVERIFY(GhosttyActionCatalog::isImplemented(
        QStringLiteral("toggle_readonly")));

    const GhosttyPaneAction mouseReporting =
        parse("toggle_mouse_reporting");
    QCOMPARE(mouseReporting.kind,
             GhosttyPaneActionKind::ToggleMouseReporting);
    QVERIFY(GhosttyActionCatalog::isImplemented(
        QStringLiteral("toggle_mouse_reporting")));
    QVERIFY(!GhosttyActionCatalog::isImplemented(
        QStringLiteral("toggle_mouse_reporting:")));
    QVERIFY(!GhosttyActionCatalog::isImplemented(
        QStringLiteral("toggle_mouse_reporting:false")));
    QCOMPARE(GhosttyActionCatalog::scope(
                 QStringLiteral("toggle_mouse_reporting")),
             GhosttyActionScope::Surface);

    const QStringList implementedControls{
        QStringLiteral("csi:"),
        QStringLiteral("csi:0m"),
        QStringLiteral("esc:"),
        QStringLiteral("esc:]0:title"),
        QStringLiteral("text:"),
        QStringLiteral(R"(text:\q)"),
        QStringLiteral("reset"),
    };
    for (const QString &serialized : implementedControls) {
        QVERIFY2(GhosttyActionCatalog::isImplemented(serialized),
                 qPrintable(serialized));
    }

    // The pinned Binding.Action.parse rejects CursorKey before considering
    // whether its parameter could otherwise be parsed.
    const QStringList rejectedCursorKeys{
        QStringLiteral("cursor_key"),
        QStringLiteral("cursor_key:"),
        QStringLiteral("cursor_key:normal,application"),
    };
    for (const QString &serialized : rejectedCursorKeys) {
        QVERIFY2(!GhosttyActionCatalog::parsePaneAction(serialized).has_value(),
                 qPrintable(serialized));
        QVERIFY2(!GhosttyActionCatalog::isImplemented(serialized),
                 qPrintable(serialized));
    }

    const struct {
        const char *parameter;
        TerminalSelectionAdjustment adjustment;
    } adjustments[] = {
        {"left", TerminalSelectionAdjustment::Left},
        {"right", TerminalSelectionAdjustment::Right},
        {"up", TerminalSelectionAdjustment::Up},
        {"down", TerminalSelectionAdjustment::Down},
        {"page_up", TerminalSelectionAdjustment::PageUp},
        {"page_down", TerminalSelectionAdjustment::PageDown},
        {"home", TerminalSelectionAdjustment::Home},
        {"end", TerminalSelectionAdjustment::End},
        {"beginning_of_line", TerminalSelectionAdjustment::BeginningOfLine},
        {"end_of_line", TerminalSelectionAdjustment::EndOfLine},
    };
    for (const auto &testCase : adjustments) {
        const QByteArray serialized = QByteArrayLiteral("adjust_selection:")
            + testCase.parameter;
        const GhosttyPaneAction action = parse(serialized.constData());
        QCOMPARE(action.kind, GhosttyPaneActionKind::AdjustSelection);
        QCOMPARE(action.selectionAdjustment, testCase.adjustment);
    }

    QVERIFY(!GhosttyActionCatalog::parsePaneAction(
        QStringLiteral("new_tab")).has_value());
}

void GhosttyActionCatalogTest::parsesFontSizePaneActions()
{
    const auto parse = [](QStringView serialized) {
        const std::optional<GhosttyPaneAction> action =
            GhosttyActionCatalog::parsePaneAction(serialized);
        return action.value();
    };

    const GhosttyPaneAction increase =
        parse(QStringLiteral("increase_font_size:1_2.5"));
    QCOMPARE(increase.kind, GhosttyPaneActionKind::FontSize);
    QCOMPARE(increase.fontSize.kind,
             TerminalFontSizeRequest::Kind::Increase);
    QCOMPARE(increase.fontSize.points, 12.5F);

    const GhosttyPaneAction decrease =
        parse(QStringLiteral("decrease_font_size:-0"));
    QCOMPARE(decrease.fontSize.kind,
             TerminalFontSizeRequest::Kind::Decrease);
    QCOMPARE(decrease.fontSize.points, 0.0F);
    QVERIFY(std::signbit(decrease.fontSize.points));

    const GhosttyPaneAction set =
        parse(QStringLiteral("set_font_size:0x1.8p1"));
    QCOMPARE(set.fontSize.kind, TerminalFontSizeRequest::Kind::Set);
    QCOMPARE(set.fontSize.points, 3.0F);

    const GhosttyPaneAction reset =
        parse(QStringLiteral("reset_font_size"));
    QCOMPARE(reset.fontSize.kind, TerminalFontSizeRequest::Kind::Reset);

    const GhosttyPaneAction infinity =
        parse(QStringLiteral("increase_font_size:InFiNiTy"));
    QVERIFY(std::isinf(infinity.fontSize.points));
    QVERIFY(infinity.fontSize.points > 0.0F);

    const GhosttyPaneAction negativeInfinity =
        parse(QStringLiteral("set_font_size:-INF"));
    QVERIFY(std::isinf(negativeInfinity.fontSize.points));
    QVERIFY(negativeInfinity.fontSize.points < 0.0F);

    const GhosttyPaneAction overflow =
        parse(QStringLiteral("decrease_font_size:1e999"));
    QVERIFY(std::isinf(overflow.fontSize.points));

    const GhosttyPaneAction nan =
        parse(QStringLiteral("set_font_size:-nAn"));
    QVERIFY(std::isnan(nan.fontSize.points));
    QVERIFY(!std::signbit(nan.fontSize.points));

    const QStringList implemented{
        QStringLiteral("increase_font_size:1."),
        QStringLiteral("decrease_font_size:.5e+1"),
        QStringLiteral("set_font_size:+0XAp-1"),
        QStringLiteral("reset_font_size"),
    };
    for (const QString &serialized : implemented) {
        QVERIFY2(GhosttyActionCatalog::isImplemented(serialized),
                 qPrintable(serialized));
    }
}

void GhosttyActionCatalogTest::parsesSearchPaneActions()
{
    const auto parse = [](QStringView serialized) {
        return GhosttyActionCatalog::parsePaneAction(serialized).value();
    };

    QCOMPARE(parse(QStringLiteral("start_search")).kind,
             GhosttyPaneActionKind::StartSearch);
    QCOMPARE(parse(QStringLiteral("end_search")).kind,
             GhosttyPaneActionKind::EndSearch);
    QCOMPARE(parse(QStringLiteral("search_selection")).kind,
             GhosttyPaneActionKind::SearchSelection);

    const GhosttyPaneAction emptySearch = parse(QStringLiteral("search:"));
    QCOMPARE(emptySearch.kind, GhosttyPaneActionKind::Search);
    QCOMPARE(emptySearch.payload, QString{});

    const GhosttyPaneAction search =
        parse(QStringLiteral("search:needle:with:colons"));
    QCOMPARE(search.kind, GhosttyPaneActionKind::Search);
    QCOMPARE(search.payload, QStringLiteral("needle:with:colons"));

    const GhosttyPaneAction previous =
        parse(QStringLiteral("navigate_search:previous"));
    QCOMPARE(previous.kind, GhosttyPaneActionKind::NavigateSearch);
    QCOMPARE(previous.searchDirection, TerminalSearchDirection::Previous);

    const GhosttyPaneAction next =
        parse(QStringLiteral("navigate_search:next"));
    QCOMPARE(next.kind, GhosttyPaneActionKind::NavigateSearch);
    QCOMPARE(next.searchDirection, TerminalSearchDirection::Next);

    const QStringList implemented{
        QStringLiteral("start_search"),
        QStringLiteral("end_search"),
        QStringLiteral("search_selection"),
        QStringLiteral("search:"),
        QStringLiteral("search:needle:with:colons"),
        QStringLiteral("navigate_search:previous"),
        QStringLiteral("navigate_search:next"),
    };
    for (const QString &serialized : implemented) {
        QVERIFY2(GhosttyActionCatalog::isImplemented(serialized),
                 qPrintable(serialized));
    }
}

void GhosttyActionCatalogTest::rejectsMalformedPaneActions()
{
    const QStringList invalid{
        QStringLiteral("scroll_to_top:"),
        QStringLiteral("scroll_to_bottom:now"),
        QStringLiteral("scroll_to_selection:"),
        QStringLiteral("scroll_to_row"),
        QStringLiteral("scroll_to_row:-1"),
        QStringLiteral("scroll_to_row:0x2"),
        QStringLiteral("scroll_to_row:18446744073709551616"),
        QStringLiteral("scroll_page_up:"),
        QStringLiteral("scroll_page_down:1"),
        QStringLiteral("scroll_page_fractional"),
        QStringLiteral("scroll_page_fractional:"),
        QStringLiteral("scroll_page_fractional: 0.5"),
        QStringLiteral("scroll_page_fractional:1__0"),
        QStringLiteral("scroll_page_fractional:nan"),
        QStringLiteral("scroll_page_fractional:-infinity"),
        QStringLiteral("scroll_page_fractional:1e20"),
        QStringLiteral("scroll_page_lines"),
        QStringLiteral("scroll_page_lines:-32769"),
        QStringLiteral("scroll_page_lines:32768"),
        QStringLiteral("select_all:"),
        QStringLiteral("csi"),
        QStringLiteral("esc"),
        QStringLiteral("text"),
        QStringLiteral("reset:"),
        QStringLiteral("reset:now"),
        QStringLiteral("toggle_readonly:"),
        QStringLiteral("toggle_readonly:on"),
        QStringLiteral("adjust_selection"),
        QStringLiteral("adjust_selection:"),
        QStringLiteral("adjust_selection:LEFT"),
        QStringLiteral("adjust_selection:word"),
    };

    for (const QString &serialized : invalid) {
        QVERIFY2(!GhosttyActionCatalog::parsePaneAction(serialized).has_value(),
                 qPrintable(serialized));
        QVERIFY2(!GhosttyActionCatalog::isImplemented(serialized),
                 qPrintable(serialized));
    }
}

void GhosttyActionCatalogTest::rejectsMalformedFontSizePaneActions()
{
    const QStringList invalid{
        QStringLiteral("increase_font_size"),
        QStringLiteral("increase_font_size:"),
        QStringLiteral("decrease_font_size"),
        QStringLiteral("decrease_font_size:"),
        QStringLiteral("set_font_size"),
        QStringLiteral("set_font_size:"),
        QStringLiteral("reset_font_size:"),
        QStringLiteral("reset_font_size:1"),
        QStringLiteral("increase_font_size: 1"),
        QStringLiteral("increase_font_size:1 "),
        QStringLiteral("increase_font_size:1__0"),
        QStringLiteral("increase_font_size:1e"),
        QStringLiteral("increase_font_size:nan(payload)"),
        QStringLiteral("increase_font_size:infinite"),
        QStringLiteral("set_font_size:1:2"),
    };

    for (const QString &serialized : invalid) {
        QVERIFY2(!GhosttyActionCatalog::parsePaneAction(serialized).has_value(),
                 qPrintable(serialized));
        QVERIFY2(!GhosttyActionCatalog::isImplemented(serialized),
                 qPrintable(serialized));
    }
}

void GhosttyActionCatalogTest::rejectsMalformedSearchPaneActions()
{
    const QStringList invalid{
        QStringLiteral("start_search:"),
        QStringLiteral("start_search:now"),
        QStringLiteral("end_search:"),
        QStringLiteral("end_search:now"),
        QStringLiteral("search_selection:"),
        QStringLiteral("search_selection:needle"),
        QStringLiteral("search"),
        QStringLiteral("navigate_search"),
        QStringLiteral("navigate_search:"),
        QStringLiteral("navigate_search:Previous"),
        QStringLiteral("navigate_search:next:previous"),
    };

    for (const QString &serialized : invalid) {
        QVERIFY2(!GhosttyActionCatalog::parsePaneAction(serialized).has_value(),
                 qPrintable(serialized));
        QVERIFY2(!GhosttyActionCatalog::isImplemented(serialized),
                 qPrintable(serialized));
    }
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
    QCOMPARE(GhosttyActionCatalog::scope(QStringLiteral("close_surface")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(QStringLiteral("close_tab:this")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(QStringLiteral("close_tab:other")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(QStringLiteral("close_tab:right")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(QStringLiteral("goto_tab:2")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(
                 QStringLiteral("toggle_split_zoom")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(
                 QStringLiteral("toggle_fullscreen")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(
                 QStringLiteral("activate_key_table:copy")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(
                 QStringLiteral("toggle_readonly")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(
                 QStringLiteral("set_surface_title:project")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(
                 QStringLiteral("prompt_surface_title")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(
                 QStringLiteral("copy_title_to_clipboard")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(
                 QStringLiteral("set_tab_title:project")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(
                 QStringLiteral("prompt_tab_title")),
             GhosttyActionScope::Surface);

    const QStringList searchActions{
        QStringLiteral("start_search"),
        QStringLiteral("end_search"),
        QStringLiteral("search_selection"),
        QStringLiteral("search:needle:with:colons"),
        QStringLiteral("navigate_search:next"),
        QStringLiteral("navigate_search:previous"),
    };
    for (const QString &serialized : searchActions) {
        QCOMPARE(GhosttyActionCatalog::scope(serialized),
                 GhosttyActionScope::Surface);
    }
}

void GhosttyActionCatalogTest::recognizesKeyTableActions()
{
    const auto parse = [](QStringView serialized) {
        return GhosttyActionCatalog::parsePaneAction(serialized).value();
    };

    const GhosttyPaneAction activate =
        parse(QStringLiteral("activate_key_table:copy:mode"));
    QCOMPARE(activate.kind, GhosttyPaneActionKind::KeyTable);
    QCOMPARE(activate.keyTable.kind,
             TerminalKeyTableRequest::Kind::Activate);
    QCOMPARE(activate.keyTable.name, QStringLiteral("copy:mode"));

    const GhosttyPaneAction escaped = parse(QStringLiteral(
        R"(activate_key_table:\xc3\xa9\\quoted\")"));
    QCOMPARE(escaped.keyTable.kind,
             TerminalKeyTableRequest::Kind::Activate);
    QCOMPARE(escaped.keyTable.name, QStringLiteral("é\\quoted\""));

    const GhosttyPaneAction leadingBom = parse(QStringLiteral(
        R"(activate_key_table:\xef\xbb\xbfedit)"));
    QCOMPARE(leadingBom.keyTable.name,
             QString(QChar(0xfeff)) + QStringLiteral("edit"));

    const GhosttyPaneAction activateOnce =
        parse(QStringLiteral("activate_key_table_once:"));
    QCOMPARE(activateOnce.kind, GhosttyPaneActionKind::KeyTable);
    QCOMPARE(activateOnce.keyTable.kind,
             TerminalKeyTableRequest::Kind::ActivateOnce);
    QVERIFY(activateOnce.keyTable.name.isEmpty());

    const GhosttyPaneAction deactivate =
        parse(QStringLiteral("deactivate_key_table"));
    QCOMPARE(deactivate.kind, GhosttyPaneActionKind::KeyTable);
    QCOMPARE(deactivate.keyTable.kind,
             TerminalKeyTableRequest::Kind::Deactivate);

    const GhosttyPaneAction deactivateAll =
        parse(QStringLiteral("deactivate_all_key_tables"));
    QCOMPARE(deactivateAll.kind, GhosttyPaneActionKind::KeyTable);
    QCOMPARE(deactivateAll.keyTable.kind,
             TerminalKeyTableRequest::Kind::DeactivateAll);

    const QStringList valid{
        QStringLiteral("activate_key_table:copy:mode"),
        QStringLiteral(R"(activate_key_table:\xc3\xa9\\quoted\")"),
        QStringLiteral(R"(activate_key_table:\xef\xbb\xbfedit)"),
        QStringLiteral("activate_key_table_once:"),
        QStringLiteral("deactivate_key_table"),
        QStringLiteral("deactivate_all_key_tables"),
    };
    for (const QString &serialized : valid) {
        QVERIFY2(GhosttyActionCatalog::isImplemented(serialized),
                 qPrintable(serialized));
    }

    const QStringList invalid{
        QStringLiteral("activate_key_table"),
        QStringLiteral("activate_key_table_once"),
        QStringLiteral(R"(activate_key_table:bad\q)"),
        QStringLiteral(R"(activate_key_table:edit\xc3)"),
        QStringLiteral(R"(activate_key_table:\xff)"),
        QStringLiteral("deactivate_key_table:"),
        QStringLiteral("deactivate_key_table:copy"),
        QStringLiteral("deactivate_all_key_tables:"),
        QStringLiteral("deactivate_all_key_tables:copy"),
    };
    for (const QString &serialized : invalid) {
        QVERIFY2(!GhosttyActionCatalog::parsePaneAction(serialized).has_value(),
                 qPrintable(serialized));
        QVERIFY2(!GhosttyActionCatalog::isImplemented(serialized),
                 qPrintable(serialized));
    }
}

QTEST_APPLESS_MAIN(GhosttyActionCatalogTest)

#include "test_ghostty_action_catalog.moc"
