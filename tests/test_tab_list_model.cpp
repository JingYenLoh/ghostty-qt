#include "workspace/tab_list_model.h"

#include <QSignalSpy>
#include <QTest>

#include <utility>

// TabListModel deliberately exposes no public mutation API. Production
// mutations are owned by TerminalWorkspace, so this focused test harness uses
// the same friendship boundary without widening the model's public contract.
class TerminalWorkspace {
public:
    static void append(TabListModel &model, TabListEntry entry)
    {
        model.append(std::move(entry));
    }

    static bool replace(TabListModel &model, TabListEntry entry)
    {
        return model.replace(entry.id, std::move(entry));
    }
};

namespace {

TabListEntry entry()
{
    return {
        .id = TabId(7),
        .activePaneId = PaneId(11),
        .title = QStringLiteral("Surface title"),
    };
}

} // namespace

class TabListModelTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void bellDecoratesEffectiveTitleWithoutChangingRawValues();
    void bellChangeNotifiesOnlyTitleRole();
    void attentionChangeNotifiesOnlyAttentionRole();
};

void TabListModelTest::bellDecoratesEffectiveTitleWithoutChangingRawValues()
{
    TabListModel model;
    TabListEntry current = entry();
    TerminalWorkspace::append(model, current);
    const QModelIndex index = model.index(0, 0);

    QCOMPARE(model.data(index, TabListModel::TitleRole).toString(),
             QStringLiteral("Surface title"));
    QCOMPARE(model.data(index, TabListModel::TitleOverrideRole).toString(),
             QString{});

    current.bell = true;
    QVERIFY(TerminalWorkspace::replace(model, current));
    QCOMPARE(model.data(index, TabListModel::TitleRole).toString(),
             QStringLiteral("🔔 Surface title"));
    QCOMPARE(model.entryAt(0)->title, QStringLiteral("Surface title"));
    QCOMPARE(model.entryAt(0)->titleOverride, QString{});

    current.titleOverride = QStringLiteral("Pinned title");
    QVERIFY(TerminalWorkspace::replace(model, current));
    QCOMPARE(model.data(index, TabListModel::TitleRole).toString(),
             QStringLiteral("🔔 Pinned title"));
    QCOMPARE(model.data(index, TabListModel::TitleOverrideRole).toString(),
             QStringLiteral("Pinned title"));
    QCOMPARE(model.entryAt(0)->title, QStringLiteral("Surface title"));
    QCOMPARE(model.entryAt(0)->titleOverride, QStringLiteral("Pinned title"));

    current.zoomed = true;
    QVERIFY(TerminalWorkspace::replace(model, current));
    QCOMPARE(model.data(index, TabListModel::TitleRole).toString(),
             QStringLiteral("🔔 🔍 Pinned title"));

    current.bell = false;
    QVERIFY(TerminalWorkspace::replace(model, current));
    QCOMPARE(model.data(index, TabListModel::TitleRole).toString(),
             QStringLiteral("🔍 Pinned title"));
}

void TabListModelTest::bellChangeNotifiesOnlyTitleRole()
{
    TabListModel model;
    TabListEntry current = entry();
    TerminalWorkspace::append(model, current);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);

    current.bell = true;
    QVERIFY(TerminalWorkspace::replace(model, current));
    QCOMPARE(changed.count(), 1);
    const QList<QVariant> arguments = changed.takeFirst();
    QCOMPARE(arguments.at(0).value<QModelIndex>(), model.index(0, 0));
    QCOMPARE(arguments.at(1).value<QModelIndex>(), model.index(0, 0));
    QCOMPARE(arguments.at(2).value<QList<int>>(),
             QList<int>{TabListModel::TitleRole});

    QVERIFY(TerminalWorkspace::replace(model, current));
    QVERIFY(changed.isEmpty());
}

void TabListModelTest::attentionChangeNotifiesOnlyAttentionRole()
{
    TabListModel model;
    TabListEntry current = entry();
    current.bell = true;
    TerminalWorkspace::append(model, current);
    const QModelIndex index = model.index(0, 0);
    const QString displayTitle =
        model.data(index, TabListModel::TitleRole).toString();
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);

    QVERIFY(!model.data(index, TabListModel::AttentionRole).toBool());
    current.attention = true;
    QVERIFY(TerminalWorkspace::replace(model, current));
    QVERIFY(model.data(index, TabListModel::AttentionRole).toBool());
    QCOMPARE(model.data(index, TabListModel::TitleRole).toString(),
             displayTitle);
    QCOMPARE(model.entryAt(0)->title, QStringLiteral("Surface title"));

    QCOMPARE(changed.count(), 1);
    const QList<QVariant> arguments = changed.takeFirst();
    QCOMPARE(arguments.at(2).value<QList<int>>(),
             QList<int>{TabListModel::AttentionRole});
}

QTEST_APPLESS_MAIN(TabListModelTest)

#include "test_tab_list_model.moc"
