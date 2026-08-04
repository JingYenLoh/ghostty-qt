#include "terminal_inspector_event_model.h"

#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QTest>

namespace {

quint64 sequenceAt(const TerminalInspectorEventModel &model, int row)
{
    return model
        .data(model.index(row, 0), TerminalInspectorEventModel::SequenceRole)
        .toULongLong();
}

QString textAt(const TerminalInspectorEventModel &model, int row, int role)
{
    return model.data(model.index(row, 0), role).toString();
}

} // namespace

class TerminalInspectorEventModelTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void exposesRolesAndNewestEventFirst();
    void evictsOldestEventsAtCapacity();
    void reentrantEvictionRemainsBounded();
    void reentrantClearDropsPendingEvents();
    void preservesSequenceGapsWhilePaused();
    void clearRetainsSequenceProgress();
    void filtersByCategoryAndText();
    void truncatesCopiedTextAtBounds();
    void normalizesInvalidCategoryFilters();
};

void TerminalInspectorEventModelTest::exposesRolesAndNewestEventFirst()
{
    TerminalInspectorEventModel model;
    QAbstractItemModelTester modelTester(
        &model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    const QHash<int, QByteArray> roles = model.roleNames();

    QCOMPARE(roles.size(), 7);
    QCOMPARE(roles.value(TerminalInspectorEventModel::SequenceRole),
             QByteArrayLiteral("sequence"));
    QCOMPARE(roles.value(TerminalInspectorEventModel::TraceIdRole),
             QByteArrayLiteral("traceId"));
    QCOMPARE(roles.value(TerminalInspectorEventModel::ElapsedTextRole),
             QByteArrayLiteral("elapsedText"));
    QCOMPARE(roles.value(TerminalInspectorEventModel::CategoryRole),
             QByteArrayLiteral("category"));
    QCOMPARE(roles.value(TerminalInspectorEventModel::KindRole),
             QByteArrayLiteral("kind"));
    QCOMPARE(roles.value(TerminalInspectorEventModel::SummaryRole),
             QByteArrayLiteral("summary"));
    QCOMPARE(roles.value(TerminalInspectorEventModel::DetailsRole),
             QByteArrayLiteral("details"));

    QCOMPARE(model.append(TerminalInspectorEventModel::Category::Input,
                          QStringLiteral("key"), QStringLiteral("pressed"),
                          QStringLiteral("code=65"), quint64{41}),
             quint64{1});
    QCOMPARE(model.append(TerminalInspectorEventModel::Category::Terminal,
                          QStringLiteral("title"), QStringLiteral("changed")),
             quint64{2});

    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.count(), 2);
    QCOMPARE(model.retainedCount(), 2);
    QCOMPARE(sequenceAt(model, 0), quint64{2});
    QCOMPARE(sequenceAt(model, 1), quint64{1});
    QCOMPARE(
        model.data(model.index(1, 0), TerminalInspectorEventModel::TraceIdRole)
            .toULongLong(),
        quint64{41});
    QCOMPARE(
        model.data(model.index(0, 0), TerminalInspectorEventModel::TraceIdRole)
            .toULongLong(),
        quint64{0});
    QCOMPARE(textAt(model, 0, TerminalInspectorEventModel::CategoryRole),
             QStringLiteral("Terminal"));
    QCOMPARE(textAt(model, 0, TerminalInspectorEventModel::KindRole),
             QStringLiteral("title"));
    QCOMPARE(textAt(model, 0, TerminalInspectorEventModel::SummaryRole),
             QStringLiteral("changed"));
    QCOMPARE(textAt(model, 1, TerminalInspectorEventModel::DetailsRole),
             QStringLiteral("code=65"));
    QVERIFY(!textAt(model, 0, TerminalInspectorEventModel::ElapsedTextRole)
                 .isEmpty());
    QCOMPARE(model.rowCount(model.index(0, 0)), 0);
}

void TerminalInspectorEventModelTest::evictsOldestEventsAtCapacity()
{
    TerminalInspectorEventModel model(nullptr, 2);
    QSignalSpy evicted(&model, &TerminalInspectorEventModel::eventEvicted);

    model.append(TerminalInspectorEventModel::Category::Input,
                 QStringLiteral("one"), QStringLiteral("first"));
    model.append(TerminalInspectorEventModel::Category::Terminal,
                 QStringLiteral("two"), QStringLiteral("second"));
    model.append(TerminalInspectorEventModel::Category::State,
                 QStringLiteral("three"), QStringLiteral("third"));

    QCOMPARE(model.capacity(), 2);
    QCOMPARE(model.retainedCount(), 2);
    QCOMPARE(model.count(), 2);
    QCOMPARE(model.evictedCount(), quint64{1});
    QCOMPARE(evicted.count(), 1);
    QCOMPARE(evicted.constFirst().constFirst().toULongLong(), quint64{1});
    QCOMPARE(sequenceAt(model, 0), quint64{3});
    QCOMPARE(sequenceAt(model, 1), quint64{2});

    // Eviction must also be safe when the oldest retained event is hidden.
    model.setCategoryFilter(
        static_cast<int>(TerminalInspectorEventModel::Category::State));
    QCOMPARE(model.count(), 1);
    model.append(TerminalInspectorEventModel::Category::State,
                 QStringLiteral("four"), QStringLiteral("fourth"));
    QCOMPARE(model.retainedCount(), 2);
    QCOMPARE(model.count(), 2);
    QCOMPARE(model.evictedCount(), quint64{2});
    QCOMPARE(evicted.count(), 2);
    QCOMPARE(evicted.constLast().constFirst().toULongLong(), quint64{2});
    QCOMPARE(sequenceAt(model, 0), quint64{4});
    QCOMPARE(sequenceAt(model, 1), quint64{3});
}

void TerminalInspectorEventModelTest::reentrantEvictionRemainsBounded()
{
    TerminalInspectorEventModel model(nullptr, 1);
    QAbstractItemModelTester modelTester(
        &model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    QSignalSpy evicted(&model, &TerminalInspectorEventModel::eventEvicted);
    bool appendedReentrantly = false;
    connect(&model, &TerminalInspectorEventModel::eventEvicted, &model,
            [&model, &appendedReentrantly] {
                if (appendedReentrantly) return;
                appendedReentrantly = true;
                QCOMPARE(
                    model.append(TerminalInspectorEventModel::Category::State,
                                 QStringLiteral("nested"),
                                 QStringLiteral("from observer")),
                    quint64{3});
            });

    QCOMPARE(model.append(TerminalInspectorEventModel::Category::Input,
                          QStringLiteral("first"), QStringLiteral("retained")),
             quint64{1});
    QCOMPARE(model.append(TerminalInspectorEventModel::Category::Terminal,
                          QStringLiteral("outer"),
                          QStringLiteral("replacement")),
             quint64{2});

    QVERIFY(appendedReentrantly);
    QCOMPARE(model.capacity(), 1);
    QCOMPARE(model.retainedCount(), 1);
    QCOMPARE(model.count(), 1);
    QCOMPARE(model.evictedCount(), quint64{2});
    QCOMPARE(evicted.count(), 2);
    QCOMPARE(sequenceAt(model, 0), quint64{3});
    QCOMPARE(textAt(model, 0, TerminalInspectorEventModel::KindRole),
             QStringLiteral("nested"));
}

void TerminalInspectorEventModelTest::reentrantClearDropsPendingEvents()
{
    TerminalInspectorEventModel model(nullptr, 1);
    QAbstractItemModelTester modelTester(
        &model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    bool clearedReentrantly = false;
    connect(&model, &TerminalInspectorEventModel::eventEvicted, &model,
            [&model, &clearedReentrantly] {
                if (clearedReentrantly) return;
                clearedReentrantly = true;
                model.append(TerminalInspectorEventModel::Category::Input,
                             QStringLiteral("pending secret"),
                             QStringLiteral("must be discarded"));
                model.clear();
            });
    connect(&model, &TerminalInspectorEventModel::cleared, &model, [&model] {
        model.append(TerminalInspectorEventModel::Category::Input,
                     QStringLiteral("clear observer secret"),
                     QStringLiteral("must also be discarded"));
    });

    model.append(TerminalInspectorEventModel::Category::Input,
                 QStringLiteral("first"), QStringLiteral("retained"));
    model.append(TerminalInspectorEventModel::Category::Terminal,
                 QStringLiteral("outer"), QStringLiteral("replacement"));

    QVERIFY(clearedReentrantly);
    QCOMPARE(model.retainedCount(), 0);
    QCOMPARE(model.count(), 0);
}

void TerminalInspectorEventModelTest::preservesSequenceGapsWhilePaused()
{
    TerminalInspectorEventModel model;
    QSignalSpy skippedChanged(
        &model, &TerminalInspectorEventModel::skippedWhilePausedChanged);

    QCOMPARE(model.append(TerminalInspectorEventModel::Category::Input,
                          QStringLiteral("one"), QStringLiteral("visible")),
             quint64{1});
    model.setPaused(true);
    QCOMPARE(model.append(TerminalInspectorEventModel::Category::Input,
                          QStringLiteral("two"), QStringLiteral("skipped")),
             quint64{2});
    QCOMPARE(model.append(TerminalInspectorEventModel::Category::State,
                          QStringLiteral("three"), QStringLiteral("skipped")),
             quint64{3});

    QCOMPARE(model.count(), 1);
    QCOMPARE(model.retainedCount(), 1);
    QCOMPARE(model.skippedWhilePaused(), quint64{2});
    QCOMPARE(skippedChanged.count(), 0);

    model.setPaused(false);
    QCOMPARE(skippedChanged.count(), 1);
    QCOMPARE(model.append(TerminalInspectorEventModel::Category::Terminal,
                          QStringLiteral("four"), QStringLiteral("visible")),
             quint64{4});
    QCOMPARE(sequenceAt(model, 0), quint64{4});
    QCOMPARE(sequenceAt(model, 1), quint64{1});
}

void TerminalInspectorEventModelTest::clearRetainsSequenceProgress()
{
    TerminalInspectorEventModel model;

    QCOMPARE(model.append(TerminalInspectorEventModel::Category::Input,
                          QStringLiteral("before"), QStringLiteral("clear")),
             quint64{1});
    model.clear();
    QCOMPARE(model.count(), 0);
    QCOMPARE(model.retainedCount(), 0);

    QCOMPARE(model.append(TerminalInspectorEventModel::Category::State,
                          QStringLiteral("after"), QStringLiteral("clear")),
             quint64{2});
    QCOMPARE(sequenceAt(model, 0), quint64{2});
}

void TerminalInspectorEventModelTest::filtersByCategoryAndText()
{
    TerminalInspectorEventModel model;
    model.append(TerminalInspectorEventModel::Category::Input,
                 QStringLiteral("KeyPress"), QStringLiteral("Ctrl+C"),
                 QStringLiteral("bytes=03"), quint64{77});
    model.append(TerminalInspectorEventModel::Category::Terminal,
                 QStringLiteral("Title"), QStringLiteral("Project Alpha"),
                 QStringLiteral("OSC 2"));
    model.append(TerminalInspectorEventModel::Category::State,
                 QStringLiteral("Focus"), QStringLiteral("Focused"),
                 QStringLiteral("active=true"));

    model.setFilterText(QStringLiteral("keypress"));
    QCOMPARE(model.count(), 1);
    QCOMPARE(sequenceAt(model, 0), quint64{1});
    model.setFilterText(QStringLiteral("PROJECT ALPHA"));
    QCOMPARE(model.count(), 1);
    QCOMPARE(sequenceAt(model, 0), quint64{2});
    model.setFilterText(QStringLiteral("bytes=03"));
    QCOMPARE(model.count(), 1);
    QCOMPARE(sequenceAt(model, 0), quint64{1});
    model.setFilterText(QStringLiteral("K#77"));
    QCOMPARE(model.count(), 1);
    QCOMPARE(sequenceAt(model, 0), quint64{1});
    model.setFilterText(QStringLiteral("state"));
    QCOMPARE(model.count(), 1);
    QCOMPARE(sequenceAt(model, 0), quint64{3});

    model.setFilterText({});
    model.setCategoryFilter(
        static_cast<int>(TerminalInspectorEventModel::Category::Terminal));
    QCOMPARE(model.count(), 1);
    QCOMPARE(sequenceAt(model, 0), quint64{2});

    model.setFilterText(QStringLiteral("no match"));
    QCOMPARE(model.count(), 0);
    QCOMPARE(model.retainedCount(), 3);

    model.setFilterText(QString(300, QLatin1Char('x')));
    QCOMPARE(model.filterText().size(),
             TerminalInspectorEventModel::MaximumFilterLength);
}

void TerminalInspectorEventModelTest::truncatesCopiedTextAtBounds()
{
    TerminalInspectorEventModel model;
    const QString kind(300, QLatin1Char('k'));
    const QString summary(300, QLatin1Char('s'));
    const QString details(1200, QLatin1Char('d'));

    model.append(TerminalInspectorEventModel::Category::Input, kind, summary,
                 details);

    const QString storedKind =
        textAt(model, 0, TerminalInspectorEventModel::KindRole);
    const QString storedSummary =
        textAt(model, 0, TerminalInspectorEventModel::SummaryRole);
    const QString storedDetails =
        textAt(model, 0, TerminalInspectorEventModel::DetailsRole);
    QCOMPARE(storedKind.size(), 256);
    QCOMPARE(storedSummary.size(), 256);
    QCOMPARE(storedDetails.size(), 1024);
    QCOMPARE(storedKind.left(255), kind.left(255));
    QCOMPARE(storedSummary.left(255), summary.left(255));
    QCOMPARE(storedDetails.left(1023), details.left(1023));
    QCOMPARE(storedKind.back(), QChar(0x2026));
    QCOMPARE(storedSummary.back(), QChar(0x2026));
    QCOMPARE(storedDetails.back(), QChar(0x2026));
}

void TerminalInspectorEventModelTest::normalizesInvalidCategoryFilters()
{
    TerminalInspectorEventModel model;
    model.append(TerminalInspectorEventModel::Category::Input,
                 QStringLiteral("input"), QStringLiteral("event"));
    model.append(TerminalInspectorEventModel::Category::State,
                 QStringLiteral("state"), QStringLiteral("event"));

    model.setCategoryFilter(
        static_cast<int>(TerminalInspectorEventModel::Category::Input));
    QCOMPARE(model.count(), 1);
    model.setCategoryFilter(99);
    QCOMPARE(model.categoryFilter(), -1);
    QCOMPARE(model.count(), 2);

    model.setCategoryFilter(
        static_cast<int>(TerminalInspectorEventModel::Category::State));
    QCOMPARE(model.count(), 1);
    model.setCategoryFilter(-99);
    QCOMPARE(model.categoryFilter(), -1);
    QCOMPARE(model.count(), 2);

    model.append(static_cast<TerminalInspectorEventModel::Category>(99),
                 QStringLiteral("future"), QStringLiteral("category"));
    QCOMPARE(textAt(model, 0, TerminalInspectorEventModel::CategoryRole),
             QStringLiteral("Unknown"));
}

QTEST_APPLESS_MAIN(TerminalInspectorEventModelTest)

#include "test_terminal_inspector_events.moc"
