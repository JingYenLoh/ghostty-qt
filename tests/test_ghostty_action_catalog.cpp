#include "ghostty_action_catalog.h"

#include <QTest>
#include <QtCore/qnamespace.h>

#include <cmath>
#include <limits>

namespace PaneAction = GhosttyPaneActions;
namespace FrontendAction = WorkspaceFrontendActions;

class GhosttyActionCatalogTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void tokenizesSerializedActions();
    void parsesOwningConfiguredActions();
    void compilesOwningActionChains();
    void diagnosesDirectSurfaceActions();
    void parsesWriteFileActionsExactly();
    void rejectsMalformedWriteFileActions();
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
    void parsesFrontendActionsExactly();
    void parsesWindowNavigationActionsExactly();
    void classifiesPinnedActionScopes();
    void recognizesKeyTableActions();
};

void GhosttyActionCatalogTest::tokenizesSerializedActions()
{
    const QString parameterlessSource = QStringLiteral("reload_config");
    const GhosttySerializedActionView parameterless =
        GhosttyActionCatalog::parseSerializedAction(parameterlessSource);
    QCOMPARE(parameterless.name.toString(), QStringLiteral("reload_config"));
    QVERIFY(!parameterless.parameter.has_value());

    const QString emptyParameterSource = QStringLiteral("reload_config:");
    const GhosttySerializedActionView emptyParameter =
        GhosttyActionCatalog::parseSerializedAction(emptyParameterSource);
    QCOMPARE(emptyParameter.name.toString(), QStringLiteral("reload_config"));
    QVERIFY(emptyParameter.parameter.has_value());
    QVERIFY(emptyParameter.parameter->isEmpty());

    const QString embeddedColonsSource = QStringLiteral("csi:38:2:255:0:0m");
    const GhosttySerializedActionView embeddedColons =
        GhosttyActionCatalog::parseSerializedAction(embeddedColonsSource);
    QCOMPARE(embeddedColons.name.toString(), QStringLiteral("csi"));
    QVERIFY(embeddedColons.parameter.has_value());
    QCOMPARE(embeddedColons.parameter->toString(),
             QStringLiteral("38:2:255:0:0m"));
}

void GhosttyActionCatalogTest::parsesOwningConfiguredActions()
{
    const std::optional<GhosttyConfiguredAction> application =
        GhosttyActionCatalog::parseConfiguredAction(
            QStringLiteral("reload_config"));
    QVERIFY(application.has_value());
    QCOMPARE(std::get<ApplicationAction>(*application),
             ApplicationAction::ReloadConfig);
    QCOMPARE(GhosttyActionCatalog::inputEffect(*application),
             GhosttyActionInputEffect::None);

    const std::optional<GhosttyConfiguredAction> ignored =
        GhosttyActionCatalog::parseConfiguredAction(QStringLiteral("ignore"));
    QVERIFY(ignored.has_value());
    QCOMPARE(GhosttyActionCatalog::inputEffect(*ignored),
             GhosttyActionInputEffect::Ignore);

    const std::optional<GhosttyConfiguredAction> pane =
        GhosttyActionCatalog::parseConfiguredAction(
            QStringLiteral("scroll_to_row:12"));
    QVERIFY(pane.has_value());
    QCOMPARE(
        std::get<PaneAction::ScrollToRow>(std::get<GhosttyPaneAction>(*pane))
            .row,
        quint64(12));

    const WorkspaceActionContext context{TabId(7), PaneId(11), 13, 17,
                                         CloseTabMode::Other};
    const std::optional<GhosttyConfiguredAction> workspace =
        GhosttyActionCatalog::parseConfiguredAction(
            QStringLiteral("new_split:left"), context);
    QVERIFY(workspace.has_value());
    const WorkspaceActionRequest &request =
        std::get<WorkspaceActionRequest>(*workspace);
    QCOMPARE(request.action, WorkspaceAction::SplitLeft);
    QCOMPARE(request.context.tabId, context.tabId);
    QCOMPARE(request.context.paneId, context.paneId);

    for (const QString &unsupportedApplication : {
             QStringLiteral("undo"),
             QStringLiteral("redo"),
             QStringLiteral("unbind"),
         }) {
        QVERIFY(!GhosttyActionCatalog::parseConfiguredAction(
            unsupportedApplication));
        QCOMPARE(GhosttyActionCatalog::scope(unsupportedApplication),
                 GhosttyActionScope::Application);
    }

    const auto closePane = GhosttyActionCatalog::parseConfiguredAction(
        QStringLiteral("close_surface"));
    const auto closeThis = GhosttyActionCatalog::parseConfiguredAction(
        QStringLiteral("close_tab:this"));
    const auto closeOther = GhosttyActionCatalog::parseConfiguredAction(
        QStringLiteral("close_tab:other"));
    const auto closeWindow = GhosttyActionCatalog::parseConfiguredAction(
        QStringLiteral("close_window"));
    QVERIFY(closePane && closeThis && closeOther && closeWindow);
    for (const GhosttyConfiguredAction *closing :
         {&*closePane, &*closeThis, &*closeOther, &*closeWindow}) {
        QCOMPARE(GhosttyActionCatalog::inputEffect(*closing),
                 GhosttyActionInputEffect::ClosingAction);
    }
    QVERIFY(GhosttyActionCatalog::shouldCoalesceBroadClose(*closePane));
    QVERIFY(GhosttyActionCatalog::shouldCoalesceBroadClose(*closeThis));
    QVERIFY(!GhosttyActionCatalog::shouldCoalesceBroadClose(*closeOther));
    QVERIFY(GhosttyActionCatalog::shouldCoalesceBroadClose(*closeWindow));
    QCOMPARE(
        GhosttyActionCatalog::combinedInputEffect({QStringLiteral("ignore")}),
        GhosttyActionInputEffect::Ignore);
    QCOMPARE(GhosttyActionCatalog::combinedInputEffect(
                 {QStringLiteral("close_tab:right"), QStringLiteral("ignore")}),
             GhosttyActionInputEffect::ClosingAction);
    QCOMPARE(GhosttyActionCatalog::combinedInputEffect(
                 {QStringLiteral("unsupported"), QStringLiteral("new_tab")}),
             GhosttyActionInputEffect::None);
}

void GhosttyActionCatalogTest::compilesOwningActionChains()
{
    GhosttyCompiledActionChain compiled;
    {
        QStringList source{
            QString::fromUtf8("reload_config"),
            QString::fromUtf8("search:needle:with:colons"),
            QString::fromUtf8("new_split:left"),
            QString::fromUtf8("unsupported:surface"),
            QString::fromUtf8("toggle_visibility"),
            QString::fromUtf8("reload_config:bogus"),
        };
        compiled = GhosttyActionCatalog::compileActionChain(source);

        source[0] = QStringLiteral("quit");
        source[1].fill(u'x');
        source.clear();
    }

    const QStringList serialized{
        QStringLiteral("reload_config"),
        QStringLiteral("search:needle:with:colons"),
        QStringLiteral("new_split:left"),
        QStringLiteral("unsupported:surface"),
        QStringLiteral("toggle_visibility"),
        QStringLiteral("reload_config:bogus"),
    };

    QCOMPARE(compiled.serializedActions(), serialized);
    QCOMPARE(compiled.entries.size(), serialized.size());
    QCOMPARE(compiled.inputEffect, GhosttyActionInputEffect::None);
    QVERIFY(!compiled.applicationOnly);

    const GhosttyCompiledAction &application = compiled.entries[0];
    QCOMPARE(application.serialized, QStringLiteral("reload_config"));
    QCOMPARE(application.scope, GhosttyActionScope::Application);
    QVERIFY(application.action.has_value());
    QCOMPARE(std::get<ApplicationAction>(*application.action),
             ApplicationAction::ReloadConfig);

    const GhosttyCompiledAction &pane = compiled.entries[1];
    QCOMPARE(pane.serialized, QStringLiteral("search:needle:with:colons"));
    QCOMPARE(pane.scope, GhosttyActionScope::Surface);
    QVERIFY(pane.action.has_value());
    const GhosttyPaneAction &paneAction =
        std::get<GhosttyPaneAction>(*pane.action);
    QCOMPARE(std::get<PaneAction::Search>(paneAction).serializedNeedle,
             QByteArrayLiteral("needle:with:colons"));

    const GhosttyCompiledAction &workspace = compiled.entries[2];
    QCOMPARE(workspace.serialized, QStringLiteral("new_split:left"));
    QCOMPARE(workspace.scope, GhosttyActionScope::Surface);
    QVERIFY(workspace.action.has_value());
    QCOMPARE(std::get<WorkspaceActionRequest>(*workspace.action).action,
             WorkspaceAction::SplitLeft);

    const GhosttyCompiledAction &unsupportedSurface = compiled.entries[3];
    QCOMPARE(unsupportedSurface.serialized,
             QStringLiteral("unsupported:surface"));
    QCOMPARE(unsupportedSurface.scope, GhosttyActionScope::Surface);
    QVERIFY(!unsupportedSurface.action.has_value());

    const GhosttyCompiledAction &unsupportedApplication = compiled.entries[4];
    QCOMPARE(unsupportedApplication.serialized,
             QStringLiteral("toggle_visibility"));
    QCOMPARE(unsupportedApplication.scope, GhosttyActionScope::Application);
    QVERIFY(!unsupportedApplication.action.has_value());

    const GhosttyCompiledAction &malformedApplication = compiled.entries[5];
    QCOMPARE(malformedApplication.serialized,
             QStringLiteral("reload_config:bogus"));
    QCOMPARE(malformedApplication.scope, GhosttyActionScope::Application);
    QVERIFY(!malformedApplication.action.has_value());

    const GhosttyCompiledActionChain applicationOnly =
        GhosttyActionCatalog::compileActionChain({
            QStringLiteral("reload_config"),
            QStringLiteral("toggle_visibility"),
            QStringLiteral("reload_config:bogus"),
        });
    QVERIFY(applicationOnly.applicationOnly);

    const GhosttyCompiledActionChain surfaceScopedUnsupported =
        GhosttyActionCatalog::compileActionChain({
            QStringLiteral("toggle_visibility"),
            QStringLiteral("unsupported"),
        });
    QVERIFY(!surfaceScopedUnsupported.applicationOnly);

    const GhosttyCompiledActionChain closingBeforeIgnore =
        GhosttyActionCatalog::compileActionChain({
            QStringLiteral("ignore"),
            QStringLiteral("close_tab:right"),
            QStringLiteral("ignore"),
        });
    QCOMPARE(closingBeforeIgnore.inputEffect,
             GhosttyActionInputEffect::ClosingAction);

    const GhosttyCompiledActionChain ignored =
        GhosttyActionCatalog::compileActionChain({
            QStringLiteral("unsupported"),
            QStringLiteral("ignore"),
        });
    QCOMPARE(ignored.inputEffect, GhosttyActionInputEffect::Ignore);

    const GhosttyCompiledActionChain empty =
        GhosttyActionCatalog::compileActionChain({});
    QVERIFY(empty.entries.isEmpty());
    QVERIFY(empty.serializedActions().isEmpty());
    QCOMPARE(empty.inputEffect, GhosttyActionInputEffect::None);
    QVERIFY(empty.applicationOnly);

    const GhosttyCompiledActionChain nanFontSize =
        GhosttyActionCatalog::compileActionChain({
            QStringLiteral("set_font_size:nan"),
        });
    const GhosttyCompiledActionChain nanFontSizeCopy = nanFontSize;
    QVERIFY(nanFontSize == nanFontSizeCopy);
}

void GhosttyActionCatalogTest::diagnosesDirectSurfaceActions()
{
    const struct {
        const char *serialized;
        GhosttyPaneAction expected;
    } accepted[] = {
        {"copy_to_clipboard",
         PaneAction::CopyToClipboard{.format = PaneAction::CopyFormat::Mixed}},
        {"copy_to_clipboard:mixed",
         PaneAction::CopyToClipboard{.format = PaneAction::CopyFormat::Mixed}},
        {"copy_to_clipboard:plain",
         PaneAction::CopyToClipboard{.format = PaneAction::CopyFormat::Plain}},
        {"paste_from_clipboard",
         PaneAction::Paste{.source = PaneAction::PasteSource::Clipboard}},
        {"paste_from_selection",
         PaneAction::Paste{.source = PaneAction::PasteSource::Selection}},
        {"copy_url_to_clipboard", PaneAction::CopyUrlToClipboard{}},
        {"copy_title_to_clipboard", PaneAction::CopyTitleToClipboard{}},
        {"end_key_sequence", PaneAction::EndKeySequence{}},
        {"close_window", PaneAction::CloseWindow{}},
    };

    for (const auto &testCase : accepted) {
        const QString serialized = QString::fromLatin1(testCase.serialized);
        const GhosttyDirectSurfaceActionParseResult direct =
            GhosttyActionCatalog::parseDirectSurfaceAction(serialized);
        QVERIFY2(direct.has_value(), testCase.serialized);
        QVERIFY(*direct == testCase.expected);

        const std::optional<GhosttyPaneAction> pane =
            GhosttyActionCatalog::parsePaneAction(serialized);
        QVERIFY(pane.has_value());
        QVERIFY(*pane == testCase.expected);
        const std::optional<GhosttyConfiguredAction> configured =
            GhosttyActionCatalog::parseConfiguredAction(serialized);
        QVERIFY(configured.has_value());
        QVERIFY(std::get<GhosttyPaneAction>(*configured) == testCase.expected);
        QVERIFY(GhosttyActionCatalog::isImplemented(serialized));
        QCOMPARE(GhosttyActionCatalog::scope(serialized),
                 GhosttyActionScope::Surface);
        QCOMPARE(GhosttyActionCatalog::translate(serialized).error,
                 GhosttyActionTranslationError::UnsupportedAction);
    }

    const struct {
        const char *serialized;
        GhosttyActionTranslationError error;
    } rejected[] = {
        {"", GhosttyActionTranslationError::InvalidFormat},
        {"copy_to_clipboard:", GhosttyActionTranslationError::InvalidFormat},
        {"copy_to_clipboard:bogus",
         GhosttyActionTranslationError::InvalidFormat},
        {"copy_to_clipboard:mixed:extra",
         GhosttyActionTranslationError::InvalidFormat},
        {"copy_to_clipboard:vt",
         GhosttyActionTranslationError::UnsupportedParameter},
        {"copy_to_clipboard:html",
         GhosttyActionTranslationError::UnsupportedParameter},
        {"Copy_to_clipboard", GhosttyActionTranslationError::UnsupportedAction},
        {"paste_from_clipboard:", GhosttyActionTranslationError::InvalidFormat},
        {"close_window:now", GhosttyActionTranslationError::InvalidFormat},
        {"new_tab", GhosttyActionTranslationError::UnsupportedAction},
    };
    for (const auto &testCase : rejected) {
        const QString serialized = QString::fromLatin1(testCase.serialized);
        const GhosttyDirectSurfaceActionParseResult result =
            GhosttyActionCatalog::parseDirectSurfaceAction(serialized);
        QVERIFY2(!result.has_value(), testCase.serialized);
        QCOMPARE(result.error(), testCase.error);
        if (serialized == QLatin1StringView("new_tab")) {
            QVERIFY(GhosttyActionCatalog::parseConfiguredAction(serialized));
            QVERIFY(GhosttyActionCatalog::isImplemented(serialized));
            continue;
        }
        QVERIFY(!GhosttyActionCatalog::parsePaneAction(serialized));
        QVERIFY(!GhosttyActionCatalog::parseConfiguredAction(serialized));
        QVERIFY(!GhosttyActionCatalog::isImplemented(serialized));
    }
}

void GhosttyActionCatalogTest::parsesWriteFileActionsExactly()
{
    const struct {
        const char *name;
        TerminalFileLocation location;
    } locations[] = {
        {"write_screen_file", TerminalFileLocation::Screen},
        {"write_scrollback_file", TerminalFileLocation::Scrollback},
        {"write_selection_file", TerminalFileLocation::Selection},
    };
    const struct {
        const char *name;
        TerminalFileDisposition disposition;
    } dispositions[] = {
        {"copy", TerminalFileDisposition::Copy},
        {"paste", TerminalFileDisposition::Paste},
        {"open", TerminalFileDisposition::Open},
    };

    for (const auto &location : locations) {
        for (const auto &disposition : dispositions) {
            for (const QString &formatSuffix :
                 {QString{}, QStringLiteral(",plain")}) {
                const QString serialized = QString::fromLatin1(location.name)
                    + u':' + QString::fromLatin1(disposition.name)
                    + formatSuffix;
                const GhosttyDirectSurfaceActionParseResult direct =
                    GhosttyActionCatalog::parseDirectSurfaceAction(serialized);
                QVERIFY2(direct.has_value(), qPrintable(serialized));
                const auto *writeFile =
                    std::get_if<TerminalWriteFileAction>(&*direct);
                QVERIFY2(writeFile != nullptr, qPrintable(serialized));
                QCOMPARE(writeFile->location, location.location);
                QCOMPARE(writeFile->disposition, disposition.disposition);
                QCOMPARE(writeFile->format, TerminalFileFormat::Plain);

                const std::optional<GhosttyPaneAction> pane =
                    GhosttyActionCatalog::parsePaneAction(serialized);
                QVERIFY2(pane.has_value(), qPrintable(serialized));
                QVERIFY(*pane == *direct);

                const std::optional<GhosttyConfiguredAction> configured =
                    GhosttyActionCatalog::parseConfiguredAction(serialized);
                QVERIFY2(configured.has_value(), qPrintable(serialized));
                QVERIFY(std::get<GhosttyPaneAction>(*configured) == *direct);
                QCOMPARE(GhosttyActionCatalog::inputEffect(*configured),
                         GhosttyActionInputEffect::None);
                QVERIFY(GhosttyActionCatalog::isImplemented(serialized));
                QCOMPARE(GhosttyActionCatalog::scope(serialized),
                         GhosttyActionScope::Surface);

                const GhosttyCompiledActionChain compiled =
                    GhosttyActionCatalog::compileActionChain({serialized});
                QCOMPARE(compiled.entries.size(), 1);
                QCOMPARE(compiled.entries.constFirst().scope,
                         GhosttyActionScope::Surface);
                QVERIFY(compiled.entries.constFirst().action.has_value());
                QCOMPARE(compiled.inputEffect, GhosttyActionInputEffect::None);
                QVERIFY(!compiled.applicationOnly);
            }
        }
    }
}

void GhosttyActionCatalogTest::rejectsMalformedWriteFileActions()
{
    const QStringList malformed{
        QStringLiteral("write_screen_file"),
        QStringLiteral("write_screen_file:"),
        QStringLiteral("write_screen_file:,plain"),
        QStringLiteral("write_screen_file:bogus"),
        QStringLiteral("write_screen_file:Copy"),
        QStringLiteral("write_screen_file:copy,"),
        QStringLiteral("write_screen_file:copy,Plain"),
        QStringLiteral("write_screen_file:copy,plain,extra"),
        QStringLiteral("write_screen_file:copy,,plain"),
        QStringLiteral("write_screen_file:copy:plain"),
        QStringLiteral("write_screen_file: copy"),
        QStringLiteral("write_screen_file:copy "),
        QStringLiteral("write_screen_file:copy, plain"),
        QStringLiteral("write_scrollback_file"),
        QStringLiteral("write_scrollback_file:"),
        QStringLiteral("write_selection_file"),
        QStringLiteral("write_selection_file:"),
    };
    for (const QString &serialized : malformed) {
        const GhosttyDirectSurfaceActionParseResult result =
            GhosttyActionCatalog::parseDirectSurfaceAction(serialized);
        QVERIFY2(!result.has_value(), qPrintable(serialized));
        QCOMPARE(result.error(), GhosttyActionTranslationError::InvalidFormat);
        QVERIFY(!GhosttyActionCatalog::parsePaneAction(serialized));
        QVERIFY(!GhosttyActionCatalog::parseConfiguredAction(serialized));
        QVERIFY(!GhosttyActionCatalog::isImplemented(serialized));
    }

    for (const QString &name : {QStringLiteral("write_screen_file"),
                                QStringLiteral("write_scrollback_file"),
                                QStringLiteral("write_selection_file")}) {
        for (const QString &format :
             {QStringLiteral("vt"), QStringLiteral("html")}) {
            const QString serialized = name + QStringLiteral(":copy,") + format;
            const GhosttyDirectSurfaceActionParseResult result =
                GhosttyActionCatalog::parseDirectSurfaceAction(serialized);
            QVERIFY2(!result.has_value(), qPrintable(serialized));
            QCOMPARE(result.error(),
                     GhosttyActionTranslationError::UnsupportedParameter);
            QVERIFY(!GhosttyActionCatalog::parsePaneAction(serialized));
            QVERIFY(!GhosttyActionCatalog::parseConfiguredAction(serialized));
            QVERIFY(!GhosttyActionCatalog::isImplemented(serialized));
        }
    }
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
        {"move_tab_to_new_window", WorkspaceAction::MoveTabToNewWindow, 123},
        {"equalize_splits", WorkspaceAction::EqualizeSplits, 123},
        {"toggle_split_zoom", WorkspaceAction::ToggleSplitZoom, 123},
        {"toggle_fullscreen", WorkspaceAction::ToggleFullscreen, 123},
        {"toggle_maximize", WorkspaceAction::ToggleMaximize, 123},
        {"toggle_window_decorations", WorkspaceAction::ToggleWindowDecorations,
         123},
        {"prompt_surface_title", WorkspaceAction::PromptSurfaceTitle, 123},
        {"prompt_tab_title", WorkspaceAction::PromptTabTitle, 123},
        {"prompt_window_title", WorkspaceAction::PromptWindowTitle, 123},
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
            QCOMPARE(result.request->context.closeTabMode, CloseTabMode::This);
        }
        QCOMPARE(result.actionName, serialized);
        QVERIFY(!result.parameter.has_value());
    }

    const QStringList directVoidActions{
        QStringLiteral("paste_from_clipboard"),
        QStringLiteral("paste_from_selection"),
        QStringLiteral("copy_url_to_clipboard"),
        QStringLiteral("copy_title_to_clipboard"),
        QStringLiteral("end_key_sequence"),
        QStringLiteral("close_window"),
    };
    for (const QString &action : directVoidActions) {
        QVERIFY2(GhosttyActionCatalog::isImplemented(action),
                 qPrintable(action));
        QVERIFY2(!GhosttyActionCatalog::isImplemented(action + u':'),
                 qPrintable(action));
        QVERIFY2(!GhosttyActionCatalog::isImplemented(
                     action + QStringLiteral(":ignored")),
                 qPrintable(action));
    }
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
    QVERIFY(GhosttyActionCatalog::isImplemented(
        QStringLiteral("prompt_window_title")));
    QVERIFY(!GhosttyActionCatalog::isImplemented(
        QStringLiteral("prompt_window_title:")));
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
    QTest::newRow("split-up") << QStringLiteral("new_split:up")
                              << WorkspaceAction::SplitUp << qint64(700) << 0;
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
        serialized == QLatin1StringView("close_tab:other") ? CloseTabMode::Other
        : serialized == QLatin1StringView("close_tab:right")
        ? CloseTabMode::Right
        : CloseTabMode::This;
    QCOMPARE(result.request->context.closeTabMode, closeTabMode);
    QVERIFY(GhosttyActionCatalog::isImplemented(serialized));
}

void GhosttyActionCatalogTest::preservesRawParametersAndTargetContext()
{
    const WorkspaceActionContext source{TabId(41), PaneId(73), 0, 22};
    const GhosttyActionTranslation result = GhosttyActionCatalog::translate(
        QStringLiteral("goto_split:top"), source);

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
        {QStringLiteral(R"(path\\quoted\")"), QStringLiteral("path\\quoted\"")},
        {QStringLiteral(R"(line\nnext)"), QStringLiteral("line\nnext")},
        {QStringLiteral(R"(left\x00right)"),
         QString::fromUtf8("left\0right", 10)},
        {QStringLiteral("直接"), QStringLiteral("直接")},
    };

    const struct {
        QString name;
        WorkspaceAction action;
    } actions[] = {
        {QStringLiteral("set_surface_title"), WorkspaceAction::SetSurfaceTitle},
        {QStringLiteral("set_tab_title"), WorkspaceAction::SetTabTitle},
        {QStringLiteral("set_window_title"), WorkspaceAction::SetWindowTitle},
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
    QTest::newRow("empty-name")
        << QStringLiteral(":right") << Error::InvalidFormat;
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
        << QStringLiteral("resize_split:right,0b1010") << Error::InvalidFormat;
    QTest::newRow("void-layout-parameter")
        << QStringLiteral("equalize_splits:") << Error::InvalidFormat;
    QTest::newRow("fullscreen-empty-parameter")
        << QStringLiteral("toggle_fullscreen:") << Error::InvalidFormat;
    QTest::newRow("fullscreen-parameter")
        << QStringLiteral("toggle_fullscreen:true") << Error::InvalidFormat;
    QTest::newRow("fullscreen-case")
        << QStringLiteral("Toggle_fullscreen") << Error::UnsupportedAction;
    QTest::newRow("maximize-empty-parameter")
        << QStringLiteral("toggle_maximize:") << Error::InvalidFormat;
    QTest::newRow("maximize-parameter")
        << QStringLiteral("toggle_maximize:true") << Error::InvalidFormat;
    QTest::newRow("maximize-case")
        << QStringLiteral("Toggle_maximize") << Error::UnsupportedAction;
    QTest::newRow("window-decorations-empty-parameter")
        << QStringLiteral("toggle_window_decorations:") << Error::InvalidFormat;
    QTest::newRow("window-decorations-parameter")
        << QStringLiteral("toggle_window_decorations:true")
        << Error::InvalidFormat;
    QTest::newRow("window-decorations-case")
        << QStringLiteral("Toggle_window_decorations")
        << Error::UnsupportedAction;
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
        {"close_all_windows", ApplicationAction::DeprecatedCloseAllWindows},
        {"new_window", ApplicationAction::NewWindow},
        {"open_config", ApplicationAction::OpenConfig},
        {"open_config:os_open", ApplicationAction::OpenConfig},
        {"open_config:new_window", ApplicationAction::OpenConfigNewWindow},
        {"reload_config", ApplicationAction::ReloadConfig},
        {"toggle_quick_terminal", ApplicationAction::ToggleQuickTerminal},
        {"quit", ApplicationAction::Quit},
    };
    for (const auto &testCase : accepted) {
        const QString serialized = QString::fromLatin1(testCase.serialized);
        QCOMPARE(GhosttyActionCatalog::parseApplicationAction(serialized),
                 std::optional{testCase.action});
        QVERIFY(GhosttyActionCatalog::isImplemented(serialized));
    }

    // The process-owned layer-shell route makes the exact application action
    // executable without changing its application-only scope.
    constexpr auto quickTerminal = ApplicationAction::ToggleQuickTerminal;
    const GhosttyCompiledActionChain quickTerminalBinding =
        GhosttyActionCatalog::compileActionChain(
            {QStringLiteral("toggle_quick_terminal")});
    QVERIFY(quickTerminalBinding.applicationOnly);
    QCOMPARE(quickTerminalBinding.entries.size(), 1);
    QCOMPARE(quickTerminalBinding.entries.constFirst().scope,
             GhosttyActionScope::Application);
    const ApplicationAction *const compiledQuickTerminal =
        quickTerminalBinding.entries.constFirst().getIf<ApplicationAction>();
    QVERIFY(compiledQuickTerminal != nullptr);
    QCOMPARE(*compiledQuickTerminal, quickTerminal);

    for (const QString &unsupported : {
             QStringLiteral("unbind"),
             QStringLiteral("toggle_visibility"),
             QStringLiteral("check_for_updates"),
             QStringLiteral("show_gtk_inspector"),
             QStringLiteral("undo"),
             QStringLiteral("redo"),
         }) {
        QVERIFY(!GhosttyActionCatalog::parseApplicationAction(unsupported));
        QVERIFY(!GhosttyActionCatalog::isImplemented(unsupported));
        QCOMPARE(GhosttyActionCatalog::scope(unsupported),
                 GhosttyActionScope::Application);
    }

    for (const QString &rejected : {
             QStringLiteral("ignore:"),
             QStringLiteral("ignore:anything"),
             QStringLiteral("close_all_windows:"),
             QStringLiteral("close_all_windows:now"),
             QStringLiteral("new_window:"),
             QStringLiteral("new_window:now"),
             QStringLiteral("open_config:"),
             QStringLiteral("open_config:now"),
             QStringLiteral("reload_config:"),
             QStringLiteral("reload_config:soft"),
             QStringLiteral("toggle_quick_terminal:"),
             QStringLiteral("toggle_quick_terminal:now"),
             QStringLiteral("quit:"),
             QStringLiteral("quit:now"),
             QStringLiteral("Quit"),
         }) {
        QVERIFY(!GhosttyActionCatalog::parseApplicationAction(rejected));
        QVERIFY(!GhosttyActionCatalog::isImplemented(rejected));
    }
}

void GhosttyActionCatalogTest::parsesFrontendActionsExactly()
{
    const WorkspaceActionContext source{
        .tabId = TabId(4),
        .paneId = PaneId(9),
        .value = 17,
        .amount = 3,
        .closeTabMode = CloseTabMode::Right,
    };

    const QStringList executableVoidActions{
        QStringLiteral("toggle_command_palette"),
        QStringLiteral("toggle_tab_overview"),
        QStringLiteral("show_on_screen_keyboard"),
    };
    for (const QString &serialized : executableVoidActions) {
        const std::optional<WorkspaceFrontendActionRequest> request =
            GhosttyActionCatalog::parseFrontendAction(serialized, source);
        QVERIFY2(request.has_value(), qPrintable(serialized));
        QCOMPARE(request->context, source);

        if (serialized == QStringLiteral("toggle_command_palette")) {
            QVERIFY(request->getIf<FrontendAction::ToggleCommandPalette>()
                    != nullptr);
        } else if (serialized == QStringLiteral("toggle_tab_overview")) {
            QVERIFY(request->getIf<FrontendAction::ToggleTabOverview>()
                    != nullptr);
        } else {
            QVERIFY(request->getIf<FrontendAction::ShowOnScreenKeyboard>()
                    != nullptr);
        }

        const std::optional<GhosttyConfiguredAction> configured =
            GhosttyActionCatalog::parseConfiguredAction(serialized, source);
        QVERIFY2(configured.has_value(), qPrintable(serialized));
        QCOMPARE(std::get<WorkspaceFrontendActionRequest>(*configured),
                 *request);
        QVERIFY2(GhosttyActionCatalog::isImplemented(serialized),
                 qPrintable(serialized));
        QCOMPARE(GhosttyActionCatalog::scope(serialized),
                 GhosttyActionScope::Surface);
    }

    // The pinned default Ctrl/Super+Shift+P command-palette binding becomes
    // executable only because its typed frontend route is present.
    const GhosttyCompiledActionChain defaultPaletteBinding =
        GhosttyActionCatalog::compileActionChain(
            {QStringLiteral("toggle_command_palette")});
    QVERIFY(!defaultPaletteBinding.applicationOnly);
    QCOMPARE(defaultPaletteBinding.entries.size(), 1);
    const WorkspaceFrontendActionRequest *const compiledPalette =
        defaultPaletteBinding.entries.constFirst()
            .getIf<WorkspaceFrontendActionRequest>();
    QVERIFY(compiledPalette != nullptr);
    QVERIFY(compiledPalette->getIf<FrontendAction::ToggleCommandPalette>()
            != nullptr);
    const GhosttyConfiguredAction configuredPalette{*compiledPalette};
    QCOMPARE(GhosttyActionCatalog::inputEffect(configuredPalette),
             GhosttyActionInputEffect::None);
    QVERIFY(!GhosttyActionCatalog::shouldCoalesceBroadClose(configuredPalette));

    const struct {
        const char *serialized;
        FrontendAction::InspectorMode mode;
    } inspectorActions[] = {
        {"inspector:toggle", FrontendAction::InspectorMode::Toggle},
        {"inspector:show", FrontendAction::InspectorMode::Show},
        {"inspector:hide", FrontendAction::InspectorMode::Hide},
    };
    for (const auto &testCase : inspectorActions) {
        const QString serialized = QString::fromLatin1(testCase.serialized);
        const std::optional<WorkspaceFrontendActionRequest> request =
            GhosttyActionCatalog::parseFrontendAction(serialized, source);
        QVERIFY(request.has_value());
        const auto *const inspector =
            request->getIf<FrontendAction::Inspector>();
        QVERIFY(inspector != nullptr);
        QCOMPARE(inspector->mode, testCase.mode);
        QCOMPARE(request->context, source);

        const std::optional<GhosttyConfiguredAction> configured =
            GhosttyActionCatalog::parseConfiguredAction(serialized, source);
        QVERIFY(configured.has_value());
        QCOMPARE(std::get<WorkspaceFrontendActionRequest>(*configured),
                 *request);
        QCOMPARE(GhosttyActionCatalog::inputEffect(*configured),
                 GhosttyActionInputEffect::None);
        QVERIFY(!GhosttyActionCatalog::shouldCoalesceBroadClose(*configured));
        QVERIFY(GhosttyActionCatalog::isImplemented(serialized));
        QCOMPARE(GhosttyActionCatalog::scope(serialized),
                 GhosttyActionScope::Surface);

        const GhosttyCompiledActionChain compiled =
            GhosttyActionCatalog::compileActionChain({serialized});
        QVERIFY(!compiled.applicationOnly);
        QCOMPARE(compiled.entries.size(), 1);
        const WorkspaceFrontendActionRequest *const compiledRequest =
            compiled.entries.constFirst()
                .getIf<WorkspaceFrontendActionRequest>();
        QVERIFY(compiledRequest != nullptr);
        QCOMPARE(compiledRequest->action, request->action);
        QCOMPARE(compiledRequest->context, WorkspaceActionContext{});
    }

    const struct {
        const char *serialized;
        FrontendAction::CrashTarget target;
    } crashActions[] = {
        {"crash:main", FrontendAction::CrashTarget::Main},
        {"crash:io", FrontendAction::CrashTarget::Io},
        {"crash:render", FrontendAction::CrashTarget::Render},
    };
    for (const auto &testCase : crashActions) {
        const QString serialized = QString::fromLatin1(testCase.serialized);
        const std::optional<WorkspaceFrontendActionRequest> request =
            GhosttyActionCatalog::parseFrontendAction(serialized, source);
        QVERIFY(request.has_value());
        const auto *const crash = request->getIf<FrontendAction::Crash>();
        QVERIFY(crash != nullptr);
        QCOMPARE(crash->target, testCase.target);
        QCOMPARE(request->context, source);

        const std::optional<GhosttyConfiguredAction> configured =
            GhosttyActionCatalog::parseConfiguredAction(serialized, source);
        QVERIFY(configured.has_value());
        QCOMPARE(std::get<WorkspaceFrontendActionRequest>(*configured),
                 *request);
        QVERIFY(GhosttyActionCatalog::isImplemented(serialized));
        QCOMPARE(GhosttyActionCatalog::scope(serialized),
                 GhosttyActionScope::Surface);

        const GhosttyCompiledActionChain compiled =
            GhosttyActionCatalog::compileActionChain({serialized});
        QVERIFY(!compiled.applicationOnly);
        QCOMPARE(compiled.entries.size(), 1);
        const auto *const compiledRequest =
            compiled.entries.constFirst()
                .getIf<WorkspaceFrontendActionRequest>();
        QVERIFY(compiledRequest != nullptr);
        QCOMPARE(compiledRequest->action, request->action);
    }

    const QStringList rejected{
        QStringLiteral("toggle_command_palette:"),
        QStringLiteral("toggle_command_palette:now"),
        QStringLiteral("Toggle_command_palette"),
        QStringLiteral("toggle_tab_overview:"),
        QStringLiteral("toggle_tab_overview:now"),
        QStringLiteral("Toggle_tab_overview"),
        QStringLiteral("show_on_screen_keyboard:"),
        QStringLiteral("show_on_screen_keyboard:now"),
        QStringLiteral("Show_on_screen_keyboard"),
        QStringLiteral("inspector"),
        QStringLiteral("inspector:"),
        QStringLiteral("inspector:toggle:extra"),
        QStringLiteral("inspector:Toggle"),
        QStringLiteral("inspector:unknown"),
        QStringLiteral("Inspector:toggle"),
        QStringLiteral("crash"),
        QStringLiteral("crash:"),
        QStringLiteral("crash:main:extra"),
        QStringLiteral("crash:Main"),
        QStringLiteral("crash:unknown"),
        QStringLiteral("Crash:main"),
    };
    for (const QString &serialized : rejected) {
        QVERIFY2(!GhosttyActionCatalog::parseFrontendAction(serialized),
                 qPrintable(serialized));
        QVERIFY2(!GhosttyActionCatalog::isImplemented(serialized),
                 qPrintable(serialized));
    }

    // This spelling is deliberately not a frontend action: it remains an
    // implemented application action routed by ApplicationController.
    const QString quickTerminal = QStringLiteral("toggle_quick_terminal");
    QVERIFY(!GhosttyActionCatalog::parseFrontendAction(quickTerminal));
    QVERIFY(GhosttyActionCatalog::isImplemented(quickTerminal));
}

void GhosttyActionCatalogTest::parsesWindowNavigationActionsExactly()
{
    const struct {
        const char *serialized;
        WindowNavigationAction action;
    } accepted[] = {
        {"goto_window:previous", WindowNavigationAction::Previous},
        {"goto_window:next", WindowNavigationAction::Next},
    };
    for (const auto &testCase : accepted) {
        const QString serialized = QString::fromLatin1(testCase.serialized);
        QCOMPARE(GhosttyActionCatalog::parseWindowNavigationAction(serialized),
                 std::optional{testCase.action});
        const std::optional<GhosttyConfiguredAction> configured =
            GhosttyActionCatalog::parseConfiguredAction(serialized);
        QVERIFY(configured.has_value());
        QCOMPARE(std::get<WindowNavigationAction>(*configured),
                 testCase.action);
        QVERIFY(GhosttyActionCatalog::isImplemented(serialized));
        QCOMPARE(GhosttyActionCatalog::scope(serialized),
                 GhosttyActionScope::Surface);
        QVERIFY(!GhosttyActionCatalog::parseApplicationAction(serialized));
    }

    for (const QString &rejected : {
             QStringLiteral("goto_window"),
             QStringLiteral("goto_window:"),
             QStringLiteral("goto_window:previous:extra"),
             QStringLiteral("goto_window:next:extra"),
             QStringLiteral("goto_window:left"),
             QStringLiteral("goto_window:Previous"),
             QStringLiteral("Goto_window:next"),
         }) {
        QVERIFY(!GhosttyActionCatalog::parseWindowNavigationAction(rejected));
        QVERIFY(!GhosttyActionCatalog::parseConfiguredAction(rejected));
        QVERIFY(!GhosttyActionCatalog::isImplemented(rejected));
    }

    const GhosttyCompiledActionChain compiled =
        GhosttyActionCatalog::compileActionChain({
            QStringLiteral("goto_window:next"),
        });
    QCOMPARE(compiled.entries.size(), 1);
    QCOMPARE(compiled.entries.constFirst().scope, GhosttyActionScope::Surface);
    QVERIFY(!compiled.applicationOnly);
    QCOMPARE(*compiled.entries.constFirst().getIf<WindowNavigationAction>(),
             WindowNavigationAction::Next);
}

void GhosttyActionCatalogTest::matchesPinnedIntegerParsing_data()
{
    QTest::addColumn<QString>("serialized");
    QTest::addColumn<WorkspaceAction>("action");
    QTest::addColumn<qint64>("value");
    QTest::addColumn<int>("amount");

    QTest::newRow("unsigned-plus")
        << QStringLiteral("goto_tab:+12") << WorkspaceAction::ActivateTabByIndex
        << qint64(12) << 0;
    QTest::newRow("unsigned-negative-zero")
        << QStringLiteral("goto_tab:-0") << WorkspaceAction::ActivateTabByIndex
        << qint64(0) << 0;
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

    QVERIFY(std::holds_alternative<PaneAction::ScrollToTop>(
        parse("scroll_to_top")));
    QVERIFY(std::holds_alternative<PaneAction::ScrollToBottom>(
        parse("scroll_to_bottom")));
    QVERIFY(std::holds_alternative<PaneAction::ScrollToSelection>(
        parse("scroll_to_selection")));

    const GhosttyPaneAction row = parse("scroll_to_row:+1__2");
    QCOMPARE(std::get<PaneAction::ScrollToRow>(row).row, quint64(12));
    QCOMPARE(std::get<PaneAction::ScrollToRow>(parse("scroll_to_row:-0")).row,
             quint64(0));

    QVERIFY(std::holds_alternative<PaneAction::ScrollPageUp>(
        parse("scroll_page_up")));
    QVERIFY(std::holds_alternative<PaneAction::ScrollPageDown>(
        parse("scroll_page_down")));

    const GhosttyPaneAction fraction = parse("scroll_page_fractional:+0.5");
    QCOMPARE(std::get<PaneAction::ScrollPageFractional>(fraction).fraction,
             0.5F);
    QCOMPARE(std::get<PaneAction::ScrollPageFractional>(
                 parse("scroll_page_fractional:0x1p-1"))
                 .fraction,
             0.5F);
    QCOMPARE(std::get<PaneAction::ScrollPageFractional>(
                 parse("scroll_page_fractional:1e-1000"))
                 .fraction,
             0.0F);

    const GhosttyPaneAction lines = parse("scroll_page_lines:-32_768");
    QCOMPARE(std::get<PaneAction::ScrollPageLines>(lines).lines,
             qint16(-32768));
    QCOMPARE(std::get<PaneAction::ScrollPageLines>(
                 parse("scroll_page_lines:+32_767"))
                 .lines,
             qint16(32767));

    QVERIFY(std::holds_alternative<PaneAction::SelectAll>(parse("select_all")));

    const GhosttyPaneAction csi = parse("csi:38:2:255:0:0m");
    QCOMPARE(std::get<PaneAction::SendCsi>(csi).serializedBytes,
             QByteArrayLiteral("38:2:255:0:0m"));
    QCOMPARE(std::get<PaneAction::SendCsi>(parse("csi:")).serializedBytes,
             QByteArray{});

    const GhosttyPaneAction esc = parse("esc:]0:title:detail\a");
    QCOMPARE(std::get<PaneAction::SendEscape>(esc).serializedBytes,
             QByteArrayLiteral("]0:title:detail\a"));
    QCOMPARE(std::get<PaneAction::SendEscape>(parse("esc:")).serializedBytes,
             QByteArray{});

    const GhosttyPaneAction text = parse(R"(text:hello\n\x00world)");
    QCOMPARE(std::get<PaneAction::SendText>(text).serializedBytes,
             QByteArrayLiteral(R"(hello\n\x00world)"));
    QCOMPARE(std::get<PaneAction::SendText>(parse("text:")).serializedBytes,
             QByteArray{});

    // Binding.Action.parse intentionally defers Zig-literal validation until
    // action execution, where a malformed literal is consumed but writes no
    // bytes.
    QCOMPARE(
        std::get<PaneAction::SendText>(parse(R"(text:\q)")).serializedBytes,
        QByteArrayLiteral(R"(\q)"));
    QVERIFY(std::holds_alternative<PaneAction::ResetTerminal>(parse("reset")));

    const GhosttyPaneAction readOnly = parse("toggle_readonly");
    QVERIFY(std::holds_alternative<PaneAction::ToggleReadOnly>(readOnly));
    QVERIFY(
        GhosttyActionCatalog::isImplemented(QStringLiteral("toggle_readonly")));

    const GhosttyPaneAction mouseReporting = parse("toggle_mouse_reporting");
    QVERIFY(std::holds_alternative<PaneAction::ToggleMouseReporting>(
        mouseReporting));
    QVERIFY(GhosttyActionCatalog::isImplemented(
        QStringLiteral("toggle_mouse_reporting")));
    QVERIFY(!GhosttyActionCatalog::isImplemented(
        QStringLiteral("toggle_mouse_reporting:")));
    QVERIFY(!GhosttyActionCatalog::isImplemented(
        QStringLiteral("toggle_mouse_reporting:false")));
    QCOMPARE(
        GhosttyActionCatalog::scope(QStringLiteral("toggle_mouse_reporting")),
        GhosttyActionScope::Surface);

    const QStringList implementedControls{
        QStringLiteral("csi:"),  QStringLiteral("csi:0m"),
        QStringLiteral("esc:"),  QStringLiteral("esc:]0:title"),
        QStringLiteral("text:"), QStringLiteral(R"(text:\q)"),
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
        const QByteArray serialized =
            QByteArrayLiteral("adjust_selection:") + testCase.parameter;
        const GhosttyPaneAction action = parse(serialized.constData());
        QCOMPARE(std::get<PaneAction::AdjustSelection>(action).adjustment,
                 testCase.adjustment);
    }

    QVERIFY(!GhosttyActionCatalog::parsePaneAction(QStringLiteral("new_tab"))
                 .has_value());
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
    QCOMPARE(std::get<PaneAction::IncreaseFontSize>(increase).points, 12.5F);

    const GhosttyPaneAction decrease =
        parse(QStringLiteral("decrease_font_size:-0"));
    const float decreasedPoints =
        std::get<PaneAction::DecreaseFontSize>(decrease).points;
    QCOMPARE(decreasedPoints, 0.0F);
    QVERIFY(std::signbit(decreasedPoints));

    const GhosttyPaneAction set =
        parse(QStringLiteral("set_font_size:0x1.8p1"));
    QCOMPARE(std::get<PaneAction::SetFontSize>(set).points, 3.0F);

    const GhosttyPaneAction reset = parse(QStringLiteral("reset_font_size"));
    QVERIFY(std::holds_alternative<PaneAction::ResetFontSize>(reset));

    const GhosttyPaneAction infinity =
        parse(QStringLiteral("increase_font_size:InFiNiTy"));
    const float infinityPoints =
        std::get<PaneAction::IncreaseFontSize>(infinity).points;
    QVERIFY(std::isinf(infinityPoints));
    QVERIFY(infinityPoints > 0.0F);

    const GhosttyPaneAction negativeInfinity =
        parse(QStringLiteral("set_font_size:-INF"));
    const float negativeInfinityPoints =
        std::get<PaneAction::SetFontSize>(negativeInfinity).points;
    QVERIFY(std::isinf(negativeInfinityPoints));
    QVERIFY(negativeInfinityPoints < 0.0F);

    const GhosttyPaneAction overflow =
        parse(QStringLiteral("decrease_font_size:1e999"));
    QVERIFY(
        std::isinf(std::get<PaneAction::DecreaseFontSize>(overflow).points));

    const GhosttyPaneAction nan = parse(QStringLiteral("set_font_size:-nAn"));
    const float nanPoints = std::get<PaneAction::SetFontSize>(nan).points;
    QVERIFY(std::isnan(nanPoints));
    QVERIFY(!std::signbit(nanPoints));

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

    QVERIFY(std::holds_alternative<PaneAction::StartSearch>(
        parse(QStringLiteral("start_search"))));
    QVERIFY(std::holds_alternative<PaneAction::EndSearch>(
        parse(QStringLiteral("end_search"))));
    QVERIFY(std::holds_alternative<PaneAction::SearchSelection>(
        parse(QStringLiteral("search_selection"))));

    const GhosttyPaneAction emptySearch = parse(QStringLiteral("search:"));
    QCOMPARE(std::get<PaneAction::Search>(emptySearch).serializedNeedle,
             QByteArray{});

    const GhosttyPaneAction search =
        parse(QStringLiteral("search:needle:with:colons"));
    QCOMPARE(std::get<PaneAction::Search>(search).serializedNeedle,
             QByteArrayLiteral("needle:with:colons"));

    const GhosttyPaneAction previous =
        parse(QStringLiteral("navigate_search:previous"));
    QCOMPARE(std::get<PaneAction::NavigateSearch>(previous).direction,
             TerminalSearchDirection::Previous);

    const GhosttyPaneAction next =
        parse(QStringLiteral("navigate_search:next"));
    QCOMPARE(std::get<PaneAction::NavigateSearch>(next).direction,
             TerminalSearchDirection::Next);

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
    const QStringList applicationActions{
        QStringLiteral("ignore"),
        QStringLiteral("unbind"),
        QStringLiteral("open_config"),
        QStringLiteral("reload_config"),
        QStringLiteral("close_all_windows"),
        QStringLiteral("quit"),
        QStringLiteral("toggle_quick_terminal"),
        QStringLiteral("toggle_visibility"),
        QStringLiteral("check_for_updates"),
        QStringLiteral("show_gtk_inspector"),
        QStringLiteral("new_window"),
        QStringLiteral("undo"),
        QStringLiteral("redo"),
    };
    for (const QString &action : applicationActions) {
        QCOMPARE(GhosttyActionCatalog::scope(action),
                 GhosttyActionScope::Application);
        // Scope follows the action name in Binding.Action.scope(), not the
        // validity of the serialized parameter spelling.
        QCOMPARE(GhosttyActionCatalog::scope(action + u':'),
                 GhosttyActionScope::Application);
    }
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
    for (const QString &action : {
             QStringLiteral("toggle_command_palette"),
             QStringLiteral("toggle_tab_overview"),
             QStringLiteral("show_on_screen_keyboard"),
             QStringLiteral("inspector"),
             QStringLiteral("crash"),
         }) {
        QCOMPARE(GhosttyActionCatalog::scope(action),
                 GhosttyActionScope::Surface);
        QCOMPARE(GhosttyActionCatalog::scope(action + u':'),
                 GhosttyActionScope::Surface);
    }
    QCOMPARE(GhosttyActionCatalog::scope(QStringLiteral("goto_tab:2")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(QStringLiteral("toggle_split_zoom")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(QStringLiteral("toggle_fullscreen")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(QStringLiteral("toggle_maximize")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(
                 QStringLiteral("toggle_window_decorations")),
             GhosttyActionScope::Surface);
    QCOMPARE(
        GhosttyActionCatalog::scope(QStringLiteral("activate_key_table:copy")),
        GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(QStringLiteral("toggle_readonly")),
             GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(
                 QStringLiteral("set_surface_title:project")),
             GhosttyActionScope::Surface);
    QCOMPARE(
        GhosttyActionCatalog::scope(QStringLiteral("prompt_surface_title")),
        GhosttyActionScope::Surface);
    QCOMPARE(
        GhosttyActionCatalog::scope(QStringLiteral("copy_title_to_clipboard")),
        GhosttyActionScope::Surface);
    QCOMPARE(
        GhosttyActionCatalog::scope(QStringLiteral("set_tab_title:project")),
        GhosttyActionScope::Surface);
    QCOMPARE(GhosttyActionCatalog::scope(QStringLiteral("prompt_tab_title")),
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
    QCOMPARE(std::get<PaneAction::ActivateKeyTable>(activate).name,
             QStringLiteral("copy:mode"));

    const GhosttyPaneAction escaped =
        parse(QStringLiteral(R"(activate_key_table:\xc3\xa9\\quoted\")"));
    QCOMPARE(std::get<PaneAction::ActivateKeyTable>(escaped).name,
             QStringLiteral("é\\quoted\""));

    const GhosttyPaneAction leadingBom =
        parse(QStringLiteral(R"(activate_key_table:\xef\xbb\xbfedit)"));
    QCOMPARE(std::get<PaneAction::ActivateKeyTable>(leadingBom).name,
             QString(QChar(0xfeff)) + QStringLiteral("edit"));

    const GhosttyPaneAction activateOnce =
        parse(QStringLiteral("activate_key_table_once:"));
    QVERIFY(std::get<PaneAction::ActivateKeyTableOnce>(activateOnce)
                .name.isEmpty());

    const GhosttyPaneAction deactivate =
        parse(QStringLiteral("deactivate_key_table"));
    QVERIFY(std::holds_alternative<PaneAction::DeactivateKeyTable>(deactivate));

    const GhosttyPaneAction deactivateAll =
        parse(QStringLiteral("deactivate_all_key_tables"));
    QVERIFY(std::holds_alternative<PaneAction::DeactivateAllKeyTables>(
        deactivateAll));

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
