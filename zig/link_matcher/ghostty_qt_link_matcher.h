#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GhosttyQtLinkMatcher GhosttyQtLinkMatcher;

typedef enum GhosttyQtLinkMatcherStatus {
    GHOSTTY_QT_LINK_MATCHER_MATCH = 0,
    GHOSTTY_QT_LINK_MATCHER_NO_MATCH = 1,
    GHOSTTY_QT_LINK_MATCHER_INVALID_ARGUMENT = 2,
    GHOSTTY_QT_LINK_MATCHER_ENGINE_ERROR = 3,
} GhosttyQtLinkMatcherStatus;

typedef struct GhosttyQtLinkMatch {
    size_t begin;
    size_t end;
} GhosttyQtLinkMatch;

// Matcher instances own a compiled copy of Ghostty's default URL/path regex.
// A matcher is confined to one thread; separate instances may be used from
// separate threads. Creation returns NULL when the engine cannot initialize or
// the compiled default expression cannot be allocated.
GhosttyQtLinkMatcher *ghostty_qt_link_matcher_create(void);
void ghostty_qt_link_matcher_destroy(GhosttyQtLinkMatcher *matcher);

// Search the suffix beginning at search_offset and report byte offsets into
// the original UTF-8 input. Calling again with the previous match's end offset
// has the same subject-slicing semantics as Ghostty's StringMap iterator.
GhosttyQtLinkMatcherStatus ghostty_qt_link_matcher_find_next(
    GhosttyQtLinkMatcher *matcher,
    const uint8_t *input,
    size_t input_length,
    size_t search_offset,
    GhosttyQtLinkMatch *out_match);

#ifdef __cplusplus
}
#endif
