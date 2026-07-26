#include "ghostty_link_matcher.h"

#include "ghostty_qt_link_matcher.h"

#include <cstddef>
#include <cstdint>
#include <limits>

class GhosttyLinkMatcher::Impl final {
public:
    ~Impl() { ghostty_qt_link_matcher_destroy(matcher); }

    GhosttyQtLinkMatcher *matcher = nullptr;
};

GhosttyLinkMatcher::GhosttyLinkMatcher()
    : impl_(std::make_unique<Impl>())
{
    impl_->matcher = ghostty_qt_link_matcher_create();
    if (!impl_->matcher) {
        impl_.reset();
    }
}

GhosttyLinkMatcher::~GhosttyLinkMatcher() = default;

bool GhosttyLinkMatcher::isValid() const
{
    return impl_ && impl_->matcher;
}

GhosttyLinkMatchResult GhosttyLinkMatcher::findNext(QByteArrayView utf8,
                                                    qsizetype searchOffset,
                                                    GhosttyLinkMatch *match)
{
    if (!isValid()) {
        return GhosttyLinkMatchResult::Unavailable;
    }
    if (!match || searchOffset < 0 || searchOffset > utf8.size()) {
        return GhosttyLinkMatchResult::InvalidInput;
    }
    if (utf8.size() < 0
        || static_cast<quint64>(utf8.size())
            > std::numeric_limits<std::size_t>::max()) {
        return GhosttyLinkMatchResult::InvalidInput;
    }

    GhosttyQtLinkMatch rawMatch{};
    const auto status = ghostty_qt_link_matcher_find_next(
        impl_->matcher, reinterpret_cast<const std::uint8_t *>(utf8.data()),
        static_cast<std::size_t>(utf8.size()),
        static_cast<std::size_t>(searchOffset), &rawMatch);
    switch (status) {
    case GHOSTTY_QT_LINK_MATCHER_MATCH:
        if (rawMatch.begin > static_cast<std::size_t>(utf8.size())
            || rawMatch.end > static_cast<std::size_t>(utf8.size())
            || rawMatch.end <= rawMatch.begin
            || rawMatch.begin > static_cast<std::size_t>(
                   std::numeric_limits<qsizetype>::max())) {
            return GhosttyLinkMatchResult::EngineError;
        }
        match->beginByte = static_cast<qsizetype>(rawMatch.begin);
        match->endByte = static_cast<qsizetype>(rawMatch.end);
        return GhosttyLinkMatchResult::Match;
    case GHOSTTY_QT_LINK_MATCHER_NO_MATCH:
        return GhosttyLinkMatchResult::NoMatch;
    case GHOSTTY_QT_LINK_MATCHER_INVALID_ARGUMENT:
        return GhosttyLinkMatchResult::InvalidInput;
    case GHOSTTY_QT_LINK_MATCHER_ENGINE_ERROR:
        return GhosttyLinkMatchResult::EngineError;
    }
    return GhosttyLinkMatchResult::EngineError;
}

GhosttyLinkMatchResult GhosttyLinkMatcher::matchAt(QByteArrayView utf8,
                                                   qsizetype byteOffset,
                                                   GhosttyLinkMatch *match)
{
    if (!match || byteOffset < 0 || byteOffset >= utf8.size()) {
        return GhosttyLinkMatchResult::InvalidInput;
    }

    qsizetype searchOffset = 0;
    while (searchOffset < utf8.size()) {
        GhosttyLinkMatch candidate;
        const GhosttyLinkMatchResult result =
            findNext(utf8, searchOffset, &candidate);
        if (result != GhosttyLinkMatchResult::Match) {
            return result;
        }
        if (candidate.beginByte <= byteOffset
            && byteOffset < candidate.endByte) {
            *match = candidate;
            return GhosttyLinkMatchResult::Match;
        }
        if (candidate.beginByte > byteOffset) {
            return GhosttyLinkMatchResult::NoMatch;
        }
        searchOffset = candidate.endByte;
    }
    return GhosttyLinkMatchResult::NoMatch;
}
