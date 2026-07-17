#pragma once

#include <QByteArrayView>
#include <QtTypes>

#include <memory>

struct GhosttyLinkMatch {
    qsizetype beginByte = 0;
    qsizetype endByte = 0;

    friend bool operator==(const GhosttyLinkMatch &,
                           const GhosttyLinkMatch &) = default;
};

enum class GhosttyLinkMatchResult {
    Match,
    NoMatch,
    InvalidInput,
    EngineError,
    Unavailable,
};

// A narrow C++ owner for Ghostty's default URL/path matcher. Instances are
// worker-thread confined; byte ranges refer to the caller-owned UTF-8 input.
class GhosttyLinkMatcher final {
    class Impl;

public:
    GhosttyLinkMatcher();
    ~GhosttyLinkMatcher();

    GhosttyLinkMatcher(const GhosttyLinkMatcher &) = delete;
    GhosttyLinkMatcher &operator=(const GhosttyLinkMatcher &) = delete;
    GhosttyLinkMatcher(GhosttyLinkMatcher &&) = delete;
    GhosttyLinkMatcher &operator=(GhosttyLinkMatcher &&) = delete;

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] GhosttyLinkMatchResult findNext(
        QByteArrayView utf8,
        qsizetype searchOffset,
        GhosttyLinkMatch *match);
    [[nodiscard]] GhosttyLinkMatchResult matchAt(
        QByteArrayView utf8,
        qsizetype byteOffset,
        GhosttyLinkMatch *match);

private:
    std::unique_ptr<Impl> impl_;
};
