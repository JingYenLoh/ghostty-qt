#include "tab_list_model.h"
#include "workspace_action.h"

#include <QSignalSpy>
#include <QTest>

#include <optional>

class WorkspaceFoundationTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void tabIdsRemainStableWhenRowsMoveAfterRemoval();
    void tabModelPublishesRoleChanges();
    void dispatcherPreservesTypedActionContext();
};

void WorkspaceFoundationTest::tabIdsRemainStableWhenRowsMoveAfterRemoval()
{
    TabListModel model;
    TabListEntry first;
    first.id = TabId(11);
    first.activePaneId = PaneId(101);
    first.title = QStringLiteral("one");
    model.append(first);
    TabListEntry second;
    second.id = TabId(22);
    second.activePaneId = PaneId(202);
    second.title = QStringLiteral("two");
    model.append(second);

    QCOMPARE(model.indexOf(TabId(11)), 0);
    QCOMPARE(model.indexOf(TabId(22)), 1);
    QVERIFY(model.remove(TabId(11)));

    QCOMPARE(model.count(), 1);
    QCOMPARE(model.idAt(0).value(), quint64(22));
    QCOMPARE(model.indexOf(TabId(22)), 0);
}

void WorkspaceFoundationTest::tabModelPublishesRoleChanges()
{
    TabListModel model;
    TabListEntry entry;
    entry.id = TabId(7);
    entry.activePaneId = PaneId(70);
    entry.title = QStringLiteral("shell");
    model.append(entry);

    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    entry.title = QStringLiteral("editor");
    entry.currentDirectory = QStringLiteral("/tmp/project");
    entry.running = true;
    QVERIFY(model.replace(TabId(7), entry));

    QCOMPARE(changed.count(), 1);
    const QModelIndex index = model.index(0, 0);
    QCOMPARE(model.data(index, TabListModel::TitleRole).toString(),
             QStringLiteral("editor"));
    QCOMPARE(model.data(index, TabListModel::CurrentDirectoryRole).toString(),
             QStringLiteral("/tmp/project"));
    QCOMPARE(model.data(index, TabListModel::RunningRole).toBool(), true);
    QCOMPARE(model.data(index, TabListModel::ActivePaneIdRole).toULongLong(),
             quint64(70));
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
}

QTEST_APPLESS_MAIN(WorkspaceFoundationTest)

#include "test_workspace_foundation.moc"
