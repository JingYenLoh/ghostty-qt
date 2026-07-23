#pragma once

#include <QtTypes>

// Identifies the latest synchronous update transaction. Zero is reserved for
// state that has never begun an update, so every value returned by advance()
// can be used as a valid transaction token.
class RevisionCounter final {
public:
    using Value = quint64;

    [[nodiscard]] Value advance() noexcept
    {
        do {
            ++value_;
        } while (value_ == 0);
        return value_;
    }

    [[nodiscard]] bool isCurrent(Value revision) const noexcept
    {
        return revision != 0 && revision == value_;
    }

    [[nodiscard]] Value current() const noexcept { return value_; }

private:
    Value value_ = 0;
};
