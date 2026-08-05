#pragma once

#include <utility>

#include <unistd.h>

class UniqueFileDescriptor final {
public:
    explicit constexpr UniqueFileDescriptor(int descriptor) noexcept
        : descriptor_(descriptor)
    {}

    ~UniqueFileDescriptor() { close(); }

    UniqueFileDescriptor(const UniqueFileDescriptor &) = delete;
    UniqueFileDescriptor &operator=(const UniqueFileDescriptor &) = delete;

    constexpr UniqueFileDescriptor(UniqueFileDescriptor &&other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1))
    {}

    UniqueFileDescriptor &operator=(UniqueFileDescriptor &&other) noexcept
    {
        if (this != &other) {
            close();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    [[nodiscard]] constexpr int get() const noexcept { return descriptor_; }
    [[nodiscard]] constexpr int release() noexcept
    {
        return std::exchange(descriptor_, -1);
    }

private:
    void close() noexcept
    {
        const int previous = std::exchange(descriptor_, -1);
        if (previous >= 0) (void)::close(previous);
    }

    int descriptor_ = -1;
};
