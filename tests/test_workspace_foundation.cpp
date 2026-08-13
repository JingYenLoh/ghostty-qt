#include "tab_list_model.h"
#include "workspace_action.h"

#include <QTest>

#include <optional>

template <typename Model>
concept PubliclyMutableTabModel =
    requires(Model &model, TabListEntry entry) { model.append(entry); }
    || requires(Model &model, TabListEntry entry) { model.insert(0, entry); }
    || requires(Model &model,
                TabListEntry entry) { model.replace(entry.id, entry); }
    || requires(Model &model, TabListEntry entry) { model.move(entry.id, 0); }
    || requires(Model &model) { model.removeAt(0); };

static_assert(!PubliclyMutableTabModel<TabListModel>);

class WorkspaceFoundationTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void tabModelExposesReadOnlyContract();
    void dispatcherPreservesTypedActionContext();
};

void WorkspaceFoundationTest::tabModelExposesReadOnlyContract()
{
    TabListModel model;
    QCOMPARE(model.count(), 0);
    QCOMPARE(model.rowCount(), 0);
    QCOMPARE(model.entryAt(0), nullptr);
    QCOMPARE(model.idAt(0), TabId{});
    QCOMPARE(model.indexOf(TabId(7)), -1);
    const QHash<int, QByteArray> roles = model.roleNames();
    QCOMPARE(roles.value(TabListModel::TabIdRole), QByteArrayLiteral("tabId"));
    QCOMPARE(roles.value(TabListModel::TitleRole), QByteArrayLiteral("title"));
    QCOMPARE(roles.value(TabListModel::ActivePaneIdRole),
             QByteArrayLiteral("activePaneId"));
}

void WorkspaceFoundationTest::dispatcherPreservesTypedActionContext()
{
    std::optional<WorkspaceActionRequest> observed;
    WorkspaceActionDispatcher dispatcher(
        [&observed](const WorkspaceActionRequest &request) {
            observed = request;
            return true;
        });

    const WorkspaceActionContext context{TabId(4), PaneId(9), Qt::Key_Right};
    QVERIFY(dispatcher.dispatch(WorkspaceAction::NavigatePane, context));
    QVERIFY(observed.has_value());
    QCOMPARE(observed->action, WorkspaceAction::NavigatePane);
    QCOMPARE(observed->context.tabId.value(), quint64(4));
    QCOMPARE(observed->context.paneId.value(), quint64(9));
    QCOMPARE(observed->context.value, int(Qt::Key_Right));

    const WorkspaceActionRequest titleRequest{
        WorkspaceAction::SetTabTitle,
        {TabId(8), PaneId(12), 0, 0},
        QStringLiteral("owned title"),
    };
    QVERIFY(dispatcher.dispatch(titleRequest));
    QCOMPARE(*observed, titleRequest);
}

QTEST_APPLESS_MAIN(WorkspaceFoundationTest)

#include "test_workspace_foundation.moc"
