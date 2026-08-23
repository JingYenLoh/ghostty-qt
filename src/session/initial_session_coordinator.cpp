#include "session/initial_session_coordinator.h"

#include <QMetaObject>
#include <QMutexLocker>

#include <algorithm>
#include <limits>
#include <utility>

InitialSessionCoordinator::InitialSessionCoordinator()
    : InitialSessionCoordinator(Payload{})
{}

InitialSessionCoordinator::InitialSessionCoordinator(Payload payload)
    : QObject(nullptr)
    , payload_(std::move(payload))
{}

InitialSessionCoordinator::RequestResult
InitialSessionCoordinator::request(std::optional<Ticket> existingTicket)
{
    const QMutexLocker lock(&mutex_);
    return requestLocked(existingTicket);
}

InitialSessionCoordinator::RequestResult
InitialSessionCoordinator::requestLocked(std::optional<Ticket> existingTicket)
{
    if (existingTicket.has_value()) {
        const Ticket ticket = *existingTicket;
        if (!ticket.isValid()) return {};
        if (ticket == holder_) {
            return {
                .ticket = ticket,
                .status = RequestStatus::Granted,
                .payload = holderPayload_,
            };
        }
        if (std::ranges::find(waiters_, ticket) != waiters_.end()) {
            return {
                .ticket = ticket,
                .status = RequestStatus::Waiting,
                .payload = std::nullopt,
            };
        }
        if (state_ == State::Consumed
            && consumedTicketValues_.contains(ticket.value_)) {
            return {
                .ticket = ticket,
                .status = RequestStatus::Consumed,
                .payload = std::nullopt,
            };
        }
        return {};
    }

    if (state_ == State::Consumed) {
        return {
            .ticket = {},
            .status = RequestStatus::Consumed,
            .payload = std::nullopt,
        };
    }

    const Ticket ticket = nextTicketLocked();
    if (!ticket.isValid()) return {};
    if (state_ == State::Available) {
        state_ = State::Reserved;
        holder_ = ticket;
        holderPayload_ = payload_;
        return {
            .ticket = ticket,
            .status = RequestStatus::Granted,
            .payload = holderPayload_,
        };
    }

    waiters_.push_back(ticket);
    return {
        .ticket = ticket,
        .status = RequestStatus::Waiting,
        .payload = std::nullopt,
    };
}

bool InitialSessionCoordinator::commit(Ticket holder)
{
    bool notify = false;
    {
        const QMutexLocker lock(&mutex_);
        if (state_ != State::Reserved || holder != holder_) return false;

        consumedTicketValues_.insert(holder_.value_);
        for (const Ticket waiter : waiters_) {
            consumedTicketValues_.insert(waiter.value_);
        }
        holderPayload_.reset();
        holder_ = {};
        waiters_.clear();
        state_ = State::Consumed;
        notify = markNotificationPendingLocked();
    }
    queueRequestsChanged(notify);
    return true;
}

bool InitialSessionCoordinator::release(Ticket ticket)
{
    return remove(ticket);
}

bool InitialSessionCoordinator::cancel(Ticket ticket)
{
    return remove(ticket);
}

bool InitialSessionCoordinator::remove(Ticket ticket)
{
    bool notify = false;
    {
        const QMutexLocker lock(&mutex_);
        if (!ticket.isValid() || state_ == State::Consumed) return false;

        if (ticket == holder_) {
            holderPayload_.reset();
            holder_ = {};
            if (waiters_.empty()) {
                state_ = State::Available;
            } else {
                holder_ = waiters_.front();
                waiters_.pop_front();
                holderPayload_ = payload_;
            }
            notify = markNotificationPendingLocked();
        } else {
            const auto waiter = std::ranges::find(waiters_, ticket);
            if (waiter == waiters_.end()) return false;
            waiters_.erase(waiter);
        }
    }
    queueRequestsChanged(notify);
    return true;
}

bool InitialSessionCoordinator::updatePayload(Payload payload)
{
    const QMutexLocker lock(&mutex_);
    if (state_ == State::Consumed || payload_ == payload) return false;
    payload_ = std::move(payload);
    return true;
}

InitialSessionCoordinator::State InitialSessionCoordinator::state() const
{
    const QMutexLocker lock(&mutex_);
    return state_;
}

InitialSessionCoordinator::Ticket InitialSessionCoordinator::nextTicketLocked()
{
    // Exhausting every non-zero 64-bit ticket is not operationally possible,
    // but refusing a request is safer than wrapping and reviving stale tickets.
    if (lastTicketValue_ == std::numeric_limits<quint64>::max()) return {};
    return Ticket(++lastTicketValue_);
}

bool InitialSessionCoordinator::markNotificationPendingLocked()
{
    if (notificationPending_) return false;
    notificationPending_ = true;
    return true;
}

void InitialSessionCoordinator::queueRequestsChanged(bool needed)
{
    if (!needed) return;
    const bool queued = QMetaObject::invokeMethod(
        this,
        [this] {
            {
                const QMutexLocker lock(&mutex_);
                notificationPending_ = false;
            }
            Q_EMIT requestsChanged();
        },
        Qt::QueuedConnection);
    Q_ASSERT(queued);
    Q_UNUSED(queued);
}
