#pragma once

#include "terminal/model/terminal_types.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>

#include <optional>
#include <utility>

// libghostty-vt's clipboard effects are synchronous, while Qt clipboard
// access and permission UI belong to the GUI thread. This shared bridge lets
// the session worker wait without lending any libghostty pointers across the
// thread boundary. cancelAll is deliberately callable outside the worker
// queue so controller teardown can wake an in-flight request before it waits
// for worker shutdown.
class TerminalClipboardBridge final {
public:
    [[nodiscard]] quint64 beginRead()
    {
        QMutexLocker locker(&mutex_);
        if (closed_) return 0;
        const quint64 requestId = nextId();
        reads_.insert(requestId, {});
        return requestId;
    }

    [[nodiscard]] std::optional<TerminalClipboardReadReply>
    waitRead(quint64 requestId)
    {
        return wait(requestId, reads_);
    }

    [[nodiscard]] bool resolveRead(quint64 requestId,
                                   TerminalClipboardReadReply reply)
    {
        return resolve(requestId, std::move(reply), reads_);
    }

    [[nodiscard]] quint64 beginWrite()
    {
        QMutexLocker locker(&mutex_);
        if (closed_) return 0;
        const quint64 requestId = nextId();
        writes_.insert(requestId, {});
        return requestId;
    }

    [[nodiscard]] std::optional<TerminalClipboardWriteReply>
    waitWrite(quint64 requestId)
    {
        return wait(requestId, writes_);
    }

    [[nodiscard]] bool resolveWrite(quint64 requestId,
                                    TerminalClipboardWriteReply reply)
    {
        return resolve(requestId, std::move(reply), writes_);
    }

    void cancelAll()
    {
        QMutexLocker locker(&mutex_);
        closed_ = true;
        changed_.wakeAll();
    }

private:
    template <typename Reply> struct RequestState {
        Reply reply{};
        bool resolved = false;
    };

    quint64 nextId()
    {
        quint64 requestId = 0;
        do {
            ++nextRequestId_;
            requestId = nextRequestId_;
        } while (requestId == 0 || reads_.contains(requestId)
                 || writes_.contains(requestId));
        return requestId;
    }

    template <typename Reply>
    [[nodiscard]] std::optional<Reply>
    wait(quint64 requestId, QHash<quint64, RequestState<Reply>> &requests)
    {
        if (requestId == 0) return std::nullopt;
        QMutexLocker locker(&mutex_);
        auto iterator = requests.find(requestId);
        if (iterator == requests.end()) return std::nullopt;
        while (!closed_ && !iterator->resolved) {
            changed_.wait(&mutex_);
            iterator = requests.find(requestId);
            if (iterator == requests.end()) return std::nullopt;
        }
        std::optional<Reply> reply;
        if (iterator->resolved) reply = std::move(iterator->reply);
        requests.erase(iterator);
        return reply;
    }

    template <typename Reply>
    [[nodiscard]] bool resolve(quint64 requestId, Reply reply,
                               QHash<quint64, RequestState<Reply>> &requests)
    {
        if (requestId == 0) return false;
        QMutexLocker locker(&mutex_);
        auto iterator = requests.find(requestId);
        if (closed_ || iterator == requests.end() || iterator->resolved) {
            return false;
        }
        iterator->reply = std::move(reply);
        iterator->resolved = true;
        changed_.wakeAll();
        return true;
    }

    QMutex mutex_;
    QWaitCondition changed_;
    QHash<quint64, RequestState<TerminalClipboardReadReply>> reads_;
    QHash<quint64, RequestState<TerminalClipboardWriteReply>> writes_;
    quint64 nextRequestId_ = 0;
    bool closed_ = false;
};
