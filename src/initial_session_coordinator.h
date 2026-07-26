#pragma once

#include "terminal_command.h"

#include <QMutex>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QtGlobal>

#include <deque>
#include <optional>

// Serializes the application-wide first-session decision while terminal
// initialization happens asynchronously. A granted request owns an immutable
// snapshot of the initial program; configuration updates remain available to
// the next FIFO waiter if that holder releases its reservation.
class InitialSessionCoordinator final : public QObject {
    Q_OBJECT

public:
    struct Payload {
        // Positional CLI argv has highest execution precedence. The selected
        // tagged command is initial-command when configured and otherwise the
        // current ordinary fallback; assigning even null lets a reload reset
        // a pane snapshot before the first lease is granted.
        QStringList program;
        std::optional<TerminalCommand> command;
        bool hold = false;

        bool operator==(const Payload &) const = default;
    };

    class Ticket final {
    public:
        constexpr Ticket() = default;

        [[nodiscard]] constexpr bool isValid() const noexcept
        {
            return value_ != 0;
        }

        bool operator==(const Ticket &) const = default;

    private:
        explicit constexpr Ticket(quint64 value) noexcept
            : value_(value)
        {}

        quint64 value_ = 0;

        friend class InitialSessionCoordinator;
    };

    enum class State : quint8 {
        Available,
        Reserved,
        Consumed,
    };

    enum class RequestStatus : quint8 {
        Granted,
        Waiting,
        Consumed,
        Invalid,
    };

    struct RequestResult {
        Ticket ticket;
        RequestStatus status = RequestStatus::Invalid;
        std::optional<Payload> payload;

        [[nodiscard]] bool granted() const noexcept
        {
            return status == RequestStatus::Granted;
        }
    };

    InitialSessionCoordinator();
    explicit InitialSessionCoordinator(Payload payload);

    // With no ticket, enqueue a new request. Passing a previously returned
    // ticket polls that same request without changing its FIFO position.
    [[nodiscard]] RequestResult
    request(std::optional<Ticket> existingTicket = std::nullopt);

    // Commit succeeds only for the current holder. It permanently consumes the
    // application-wide first-session opportunity and resolves every waiter.
    [[nodiscard]] bool commit(Ticket holder);

    // Both operations remove either a holder or a waiter. Releasing the holder
    // promotes the oldest waiter; separate names keep failure and cancellation
    // call sites expressive without giving them different state semantics.
    [[nodiscard]] bool release(Ticket ticket);
    [[nodiscard]] bool cancel(Ticket ticket);

    // A granted holder keeps its snapshot. Updates made while it is reserved
    // are used only if a waiter is later promoted. Consumed state is immutable.
    [[nodiscard]] bool updatePayload(Payload payload);

    [[nodiscard]] State state() const;

Q_SIGNALS:
    // Emitted when a previously returned ticket may have become granted or
    // consumed. Delivery is asynchronous on this object's affinity thread;
    // rapid mutations may coalesce, so observers must poll their own ticket.
    void requestsChanged();

private:
    [[nodiscard]] Ticket nextTicketLocked();
    [[nodiscard]] RequestResult
    requestLocked(std::optional<Ticket> existingTicket);
    [[nodiscard]] bool remove(Ticket ticket);
    [[nodiscard]] bool markNotificationPendingLocked();
    void queueRequestsChanged(bool needed);

    mutable QMutex mutex_;
    Payload payload_;
    std::optional<Payload> holderPayload_;
    Ticket holder_;
    std::deque<Ticket> waiters_;
    QSet<quint64> consumedTicketValues_;
    quint64 lastTicketValue_ = 0;
    State state_ = State::Available;
    bool notificationPending_ = false;
};
