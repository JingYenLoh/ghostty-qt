#include "ghostty_link_matcher.h"

#include <QTest>

class GhosttyLinkMatcherTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void matchesGhosttySchemesAndPaths();
    void locatesSuccessiveMatchesByUtf8ByteOffset();
    void preservesGhosttyPunctuationAndPathHeuristics();
    void rejectsInvalidCoordinates();
};

void GhosttyLinkMatcherTest::matchesGhosttySchemesAndPaths()
{
    GhosttyLinkMatcher matcher;
    QVERIFY(matcher.isValid());

    const QByteArray input =
        "https://example.com mailto:test@example.com src/config/url.zig";
    const QList<QByteArray> expected{
        "https://example.com",
        "mailto:test@example.com",
        "src/config/url.zig",
    };

    qsizetype offset = 0;
    for (const QByteArray &text : expected) {
        GhosttyLinkMatch match;
        QCOMPARE(matcher.findNext(input, offset, &match),
                 GhosttyLinkMatchResult::Match);
        QCOMPARE(input.sliced(match.beginByte, match.endByte - match.beginByte),
                 text);
        offset = match.endByte;
    }

    GhosttyLinkMatch ignored;
    QCOMPARE(matcher.findNext(input, offset, &ignored),
             GhosttyLinkMatchResult::NoMatch);
}

void GhosttyLinkMatcherTest::locatesSuccessiveMatchesByUtf8ByteOffset()
{
    GhosttyLinkMatcher matcher;
    const QByteArray input =
        QByteArray::fromStdString("π https://example.com 与 ./src/main.cpp");

    const qsizetype urlByte = input.indexOf("example") + 2;
    GhosttyLinkMatch match;
    QCOMPARE(matcher.matchAt(input, urlByte, &match),
             GhosttyLinkMatchResult::Match);
    QCOMPARE(input.sliced(match.beginByte, match.endByte - match.beginByte),
             QByteArray("https://example.com"));

    const qsizetype pathByte = input.indexOf("main.cpp");
    QCOMPARE(matcher.matchAt(input, pathByte, &match),
             GhosttyLinkMatchResult::Match);
    QCOMPARE(input.sliced(match.beginByte, match.endByte - match.beginByte),
             QByteArray("./src/main.cpp"));

    QCOMPARE(matcher.matchAt(input, 0, &match),
             GhosttyLinkMatchResult::NoMatch);
}

void GhosttyLinkMatcherTest::preservesGhosttyPunctuationAndPathHeuristics()
{
    GhosttyLinkMatcher matcher;
    const QList<QPair<QByteArray, QByteArray>> cases{
        {"Link inside (https://example.com).", "https://example.com"},
        {"https://example.com/foo(bar) more", "https://example.com/foo(bar)"},
        {"Serving HTTP on :: port 8000 (http://[::]:8000/)",
         "http://[::]:8000/"},
        {"open ~/Documents/notes.md please", "~/Documents/notes.md"},
        {"diff --git a/src/a.zig b/src/a.zig", "a/src/a.zig"},
        {"./foo bar,baz", "./foo bar"},
    };

    for (const auto &[input, expected] : cases) {
        GhosttyLinkMatch match;
        QCOMPARE(matcher.findNext(input, 0, &match),
                 GhosttyLinkMatchResult::Match);
        QCOMPARE(input.sliced(match.beginByte, match.endByte - match.beginByte),
                 expected);
    }

    const QList<QByteArray> noMatchCases{
        "input/output",
        "$10/bar.txt",
        "foo/bar,baz.txt",
        "//foo",
    };
    for (const QByteArray &input : noMatchCases) {
        GhosttyLinkMatch match;
        QCOMPARE(matcher.findNext(input, 0, &match),
                 GhosttyLinkMatchResult::NoMatch);
    }
}

void GhosttyLinkMatcherTest::rejectsInvalidCoordinates()
{
    GhosttyLinkMatcher matcher;
    GhosttyLinkMatch match;
    QCOMPARE(matcher.findNext("hello", -1, &match),
             GhosttyLinkMatchResult::InvalidInput);
    QCOMPARE(matcher.findNext("hello", 6, &match),
             GhosttyLinkMatchResult::InvalidInput);
    QCOMPARE(matcher.matchAt("hello", 5, &match),
             GhosttyLinkMatchResult::InvalidInput);
    QCOMPARE(matcher.findNext("hello", 0, nullptr),
             GhosttyLinkMatchResult::InvalidInput);
}

QTEST_GUILESS_MAIN(GhosttyLinkMatcherTest)

#include "test_ghostty_link_matcher.moc"
