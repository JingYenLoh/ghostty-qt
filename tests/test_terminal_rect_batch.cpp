#include "terminal_rect_batch.h"

#include <QSGSimpleRectNode>
#include <QTest>

namespace {

void appendRect(QVector<TerminalColoredRect> &rects, int index)
{
    rects.append({
        .rect = QRectF(index * 4.0, index * 2.0, 3.0, 1.0),
        .color = QColor::fromRgb(16 + index, 32 + index, 48 + index, 128),
    });
}

qsizetype childCount(const QSGNode &parent)
{
    qsizetype result = 0;
    for (const QSGNode *node = parent.firstChild(); node != nullptr;
         node = node->nextSibling()) {
        ++result;
    }
    return result;
}

QVector<QSGSimpleRectNode *> softwareNodes(TerminalRectBatch &batch)
{
    QVector<QSGSimpleRectNode *> result;
    for (QSGNode *node = batch.firstChild(); node != nullptr;
         node = node->nextSibling()) {
        if (auto *rect = dynamic_cast<QSGSimpleRectNode *>(node)) {
            result.append(rect);
        }
    }
    return result;
}

} // namespace

class TerminalRectBatchTest final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void reusesHardwareGeometry();
    void reusesSoftwareNodePool();
};

void TerminalRectBatchTest::reusesHardwareGeometry()
{
    TerminalRectBatch batch;
    QVector<TerminalColoredRect> &first = batch.beginUpdate();
    appendRect(first, 0);
    appendRect(first, 1);
    batch.commit(false);

    QCOMPARE(batch.size(), qsizetype{2});
    QCOMPARE(batch.allocationGeneration(), quint64{1});
    QCOMPARE(childCount(batch), qsizetype{1});

    QVector<TerminalColoredRect> &identical = batch.beginUpdate();
    appendRect(identical, 0);
    appendRect(identical, 1);
    batch.commit(false);
    QCOMPARE(batch.allocationGeneration(), quint64{1});

    QVector<TerminalColoredRect> &smaller = batch.beginUpdate();
    appendRect(smaller, 0);
    batch.commit(false);
    QCOMPARE(batch.size(), qsizetype{1});
    QCOMPARE(batch.allocationGeneration(), quint64{1});

    QVector<TerminalColoredRect> &larger = batch.beginUpdate();
    for (int index = 0; index < 10; ++index) appendRect(larger, index);
    batch.commit(false);
    QCOMPARE(batch.size(), qsizetype{10});
    QCOMPARE(batch.allocationGeneration(), quint64{2});

    (void)batch.beginUpdate();
    batch.commit(false);
    QCOMPARE(batch.size(), qsizetype{0});
    QCOMPARE(batch.allocationGeneration(), quint64{2});
}

void TerminalRectBatchTest::reusesSoftwareNodePool()
{
    TerminalRectBatch batch;
    QVector<TerminalColoredRect> &first = batch.beginUpdate();
    for (int index = 0; index < 3; ++index) appendRect(first, index);
    batch.commit(true);

    QCOMPARE(batch.size(), qsizetype{3});
    QCOMPARE(batch.allocationGeneration(), quint64{3});
    QCOMPARE(childCount(batch), qsizetype{4});
    const QVector<QSGSimpleRectNode *> firstNodes = softwareNodes(batch);
    QCOMPARE(firstNodes.size(), 3);
    QCOMPARE(firstNodes.at(1)->rect(), QRectF(4.0, 2.0, 3.0, 1.0));
    QCOMPARE(firstNodes.at(1)->color(),
             QColor::fromRgb(17, 33, 49, 128));

    QVector<TerminalColoredRect> &smaller = batch.beginUpdate();
    appendRect(smaller, 7);
    batch.commit(true);
    QCOMPARE(batch.size(), qsizetype{1});
    QCOMPARE(batch.allocationGeneration(), quint64{3});
    QCOMPARE(softwareNodes(batch), firstNodes);
    QVERIFY(firstNodes.at(1)->rect().isEmpty());
    QVERIFY(firstNodes.at(2)->rect().isEmpty());

    QVector<TerminalColoredRect> &larger = batch.beginUpdate();
    for (int index = 0; index < 5; ++index) appendRect(larger, index);
    batch.commit(true);
    QCOMPARE(batch.allocationGeneration(), quint64{5});
    QCOMPARE(childCount(batch), qsizetype{6});
    const QVector<QSGSimpleRectNode *> fiveNodes = softwareNodes(batch);

    QVector<TerminalColoredRect> &hardware = batch.beginUpdate();
    for (int index = 0; index < 5; ++index) appendRect(hardware, index);
    batch.commit(false);
    QCOMPARE(batch.allocationGeneration(), quint64{6});
    for (QSGSimpleRectNode *node : softwareNodes(batch)) {
        QVERIFY(node->rect().isEmpty());
    }

    QVector<TerminalColoredRect> &software = batch.beginUpdate();
    for (int index = 0; index < 5; ++index) appendRect(software, index);
    batch.commit(true);
    QCOMPARE(batch.allocationGeneration(), quint64{6});
    QCOMPARE(softwareNodes(batch), fiveNodes);
}

QTEST_GUILESS_MAIN(TerminalRectBatchTest)

#include "test_terminal_rect_batch.moc"
