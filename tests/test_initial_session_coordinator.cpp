#include "initial_session_coordinator.h"

#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <initializer_list>
#include <optional>
#include <thread>
#include <utility>

namespace {

using Coordinator = InitialSessionCoordinator;

Coordinator::Payload
payload(std::initializer_list<QString> program, bool hold = false,
        std::optional<TerminalCommand> command = std::nullopt)
{
    return {
        .program = QStringList(program),
        .command = std::move(command),
        .hold = hold,
    };
}

} // namespace

class InitialSessionCoordinatorTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void promotesWaitersInFifoOrder();
    void commitConsumesHolderAndWaiters();
    void cancellationDoesNotDisturbFifoOrder();
    void payloadUpdatesApplyToTheNextGrant();
    void emptyPayloadStillConsumesTheOpportunity();
    void staleTicketsCannotAffectNewRequests();
    void requestChangesAreQueuedToTheAffinityThread();
};

void InitialSessionCoordinatorTest::promotesWaitersInFifoOrder()
{
    const std::optional<TerminalCommand> initialCommand =
        TerminalCommand::shell(QByteArrayLiteral("first shell"));
    Coordinator coordinator(
        payload({QStringLiteral("first")}, true, initialCommand));

    const Coordinator::RequestResult first = coordinator.request();
    const Coordinator::RequestResult second = coordinator.request();
    const Coordinator::RequestResult third = coordinator.request();
    QVERIFY(first.granted());
    QVERIFY(first.payload
            == std::optional(
                payload({QStringLiteral("first")}, true, initialCommand)));
    QCOMPARE(second.status, Coordinator::RequestStatus::Waiting);
    QCOMPARE(third.status, Coordinator::RequestStatus::Waiting);
    QCOMPARE(coordinator.state(), Coordinator::State::Reserved);

    QCOMPARE(coordinator.request(second.ticket).status,
             Coordinator::RequestStatus::Waiting);
    QVERIFY(coordinator.release(first.ticket));
    QVERIFY(coordinator.request(second.ticket).granted());
    QCOMPARE(coordinator.request(third.ticket).status,
             Coordinator::RequestStatus::Waiting);

    QVERIFY(coordinator.release(second.ticket));
    QVERIFY(coordinator.request(third.ticket).granted());
    QVERIFY(coordinator.release(third.ticket));
    QCOMPARE(coordinator.state(), Coordinator::State::Available);
}

void InitialSessionCoordinatorTest::commitConsumesHolderAndWaiters()
{
    Coordinator coordinator(payload({QStringLiteral("once")}));
    const Coordinator::RequestResult holder = coordinator.request();
    const Coordinator::RequestResult firstWaiter = coordinator.request();
    const Coordinator::RequestResult secondWaiter = coordinator.request();

    QVERIFY(coordinator.commit(holder.ticket));
    QCOMPARE(coordinator.state(), Coordinator::State::Consumed);
    QCOMPARE(coordinator.state(), Coordinator::State::Consumed);
    QCOMPARE(coordinator.request(holder.ticket).status,
             Coordinator::RequestStatus::Consumed);
    QCOMPARE(coordinator.request(firstWaiter.ticket).status,
             Coordinator::RequestStatus::Consumed);
    QCOMPARE(coordinator.request(secondWaiter.ticket).status,
             Coordinator::RequestStatus::Consumed);

    const Coordinator::RequestResult late = coordinator.request();
    QCOMPARE(late.status, Coordinator::RequestStatus::Consumed);
    QVERIFY(!late.ticket.isValid());
    QVERIFY(!coordinator.commit(holder.ticket));
    QVERIFY(!coordinator.release(firstWaiter.ticket));
}

void InitialSessionCoordinatorTest::cancellationDoesNotDisturbFifoOrder()
{
    Coordinator coordinator;
    const Coordinator::RequestResult holder = coordinator.request();
    const Coordinator::RequestResult cancelledWaiter = coordinator.request();
    const Coordinator::RequestResult releasedWaiter = coordinator.request();
    const Coordinator::RequestResult nextWaiter = coordinator.request();

    QVERIFY(coordinator.cancel(cancelledWaiter.ticket));
    QVERIFY(coordinator.release(releasedWaiter.ticket));
    QCOMPARE(coordinator.request(cancelledWaiter.ticket).status,
             Coordinator::RequestStatus::Invalid);
    QCOMPARE(coordinator.request(releasedWaiter.ticket).status,
             Coordinator::RequestStatus::Invalid);
    QVERIFY(!coordinator.cancel(cancelledWaiter.ticket));

    QVERIFY(coordinator.cancel(holder.ticket));
    QVERIFY(coordinator.request(nextWaiter.ticket).granted());
    QVERIFY(coordinator.release(nextWaiter.ticket));
    QCOMPARE(coordinator.state(), Coordinator::State::Available);
}

void InitialSessionCoordinatorTest::payloadUpdatesApplyToTheNextGrant()
{
    const Coordinator::Payload original =
        payload({QStringLiteral("old")}, false,
                TerminalCommand::shell(QByteArrayLiteral("old command"), true));
    const Coordinator::Payload updated =
        payload({QStringLiteral("new"), QStringLiteral("argument")}, true,
                TerminalCommand::direct({
                    QByteArrayLiteral("/bin/new"),
                    QByteArray::fromHex("ff80"),
                    QByteArray{},
                }));
    const Coordinator::Payload ignored =
        payload({QStringLiteral("ignored")}, false,
                TerminalCommand::shell(QByteArrayLiteral("ignored command")));
    Coordinator coordinator(original);

    const Coordinator::RequestResult holder = coordinator.request();
    const Coordinator::RequestResult waiter = coordinator.request();
    QVERIFY(holder.payload == std::optional(original));

    QVERIFY(coordinator.updatePayload(updated));
    // A running initialization attempt keeps the exact payload it was granted.
    QVERIFY(coordinator.request(holder.ticket).payload
            == std::optional(original));

    QVERIFY(coordinator.release(holder.ticket));
    const Coordinator::RequestResult promoted =
        coordinator.request(waiter.ticket);
    QVERIFY(promoted.granted());
    QVERIFY(promoted.payload == std::optional(updated));

    QVERIFY(coordinator.updatePayload(ignored));
    QVERIFY(coordinator.request(waiter.ticket).payload
            == std::optional(updated));
    QVERIFY(coordinator.commit(waiter.ticket));
    QVERIFY(!coordinator.updatePayload(original));
}

void InitialSessionCoordinatorTest::emptyPayloadStillConsumesTheOpportunity()
{
    Coordinator coordinator;
    const Coordinator::RequestResult holder = coordinator.request();
    QVERIFY(holder.granted());
    QVERIFY(holder.payload == std::optional(Coordinator::Payload{}));

    QVERIFY(coordinator.commit(holder.ticket));
    QCOMPARE(coordinator.state(), Coordinator::State::Consumed);
    QCOMPARE(coordinator.request().status,
             Coordinator::RequestStatus::Consumed);
}

void InitialSessionCoordinatorTest::staleTicketsCannotAffectNewRequests()
{
    Coordinator coordinator;
    const Coordinator::RequestResult oldHolder = coordinator.request();
    const Coordinator::RequestResult oldWaiter = coordinator.request();

    QVERIFY(coordinator.cancel(oldWaiter.ticket));
    QVERIFY(coordinator.release(oldHolder.ticket));
    QCOMPARE(coordinator.request(oldHolder.ticket).status,
             Coordinator::RequestStatus::Invalid);
    QCOMPARE(coordinator.request(oldWaiter.ticket).status,
             Coordinator::RequestStatus::Invalid);

    const Coordinator::RequestResult replacement = coordinator.request();
    QVERIFY(replacement.granted());
    QVERIFY(replacement.ticket != oldHolder.ticket);
    QVERIFY(replacement.ticket != oldWaiter.ticket);
    QVERIFY(!coordinator.commit(oldHolder.ticket));
    QVERIFY(!coordinator.release(oldWaiter.ticket));
    QVERIFY(coordinator.request(replacement.ticket).granted());

    QVERIFY(coordinator.commit(replacement.ticket));
    QVERIFY(!coordinator.cancel(oldHolder.ticket));
    QCOMPARE(coordinator.request(oldHolder.ticket).status,
             Coordinator::RequestStatus::Invalid);
    QCOMPARE(coordinator.request(replacement.ticket).status,
             Coordinator::RequestStatus::Consumed);
}

void InitialSessionCoordinatorTest::requestChangesAreQueuedToTheAffinityThread()
{
    Coordinator coordinator;
    const Coordinator::RequestResult holder = coordinator.request();
    const Coordinator::RequestResult waiter = coordinator.request();
    QVERIFY(holder.granted());
    QCOMPARE(waiter.status, Coordinator::RequestStatus::Waiting);

    QSignalSpy changed(&coordinator, &Coordinator::requestsChanged);
    QThread *deliveryThread = nullptr;
    connect(&coordinator, &Coordinator::requestsChanged, &coordinator,
            [&deliveryThread] { deliveryThread = QThread::currentThread(); });

    bool released = false;
    std::jthread worker([&coordinator, &released, holder] {
        released = coordinator.release(holder.ticket);
    });
    worker.join();
    QVERIFY(released);

    // Mutation never emits synchronously from the calling worker thread.
    QCOMPARE(changed.count(), 0);
    QTRY_COMPARE(changed.count(), 1);
    QCOMPARE(deliveryThread, coordinator.thread());
    QVERIFY(coordinator.request(waiter.ticket).granted());
}

QTEST_GUILESS_MAIN(InitialSessionCoordinatorTest)

#include "test_initial_session_coordinator.moc"
