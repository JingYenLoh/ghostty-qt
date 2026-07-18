#pragma once

#include "terminal_appearance.h"
#include "terminal_types.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QPoint>
#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

// The libghostty-vt API is intentionally contained by this class. Its public
// surface consists only of project and Qt value types so that an upstream C
// API change does not leak into the application or its threading model.
class GhosttyVtAdapter final {
    class Impl;

public:
    // A short-lived, move-only snapshot of one unwrapped logical terminal
    // line. Byte offsets use the exact UTF-8 byte stream consumed by Ghostty's
    // regex matcher; targetByteOffset identifies the first byte mapped to the
    // viewport cell used to create the snapshot, or -1 when that cell emits no
    // text.
    class LogicalLineSnapshot final {
    public:
        LogicalLineSnapshot(LogicalLineSnapshot &&) noexcept;
        LogicalLineSnapshot &operator=(LogicalLineSnapshot &&) noexcept;
        ~LogicalLineSnapshot();

        LogicalLineSnapshot(const LogicalLineSnapshot &) = delete;
        LogicalLineSnapshot &operator=(const LogicalLineSnapshot &) = delete;

        const QByteArray &text() const;
        qsizetype targetByteOffset() const;
        bool byteRangeContainsTarget(qsizetype beginByte,
                                     qsizetype endByte) const;

    private:
        class Impl;

        friend class GhosttyVtAdapter;
        friend class GhosttyVtAdapter::Impl;

        explicit LogicalLineSnapshot(std::unique_ptr<Impl> impl);

        std::unique_ptr<Impl> impl_;
    };

    // An owned regex-text range. Both inclusive cell endpoints and the queried
    // target cell are tracked so the range and pointer anchor follow
    // scrollback movement and primary-screen reflow without exposing a
    // libghostty handle.
    class TrackedTextRange final {
    public:
        TrackedTextRange(TrackedTextRange &&) noexcept;
        TrackedTextRange &operator=(TrackedTextRange &&) noexcept;
        ~TrackedTextRange();

        TrackedTextRange(const TrackedTextRange &) = delete;
        TrackedTextRange &operator=(const TrackedTextRange &) = delete;

    private:
        class Impl;

        friend class GhosttyVtAdapter;
        friend class GhosttyVtAdapter::Impl;

        explicit TrackedTextRange(std::unique_ptr<Impl> impl);

        std::unique_ptr<Impl> impl_;
    };

    // An owned logical OSC 8 target. The underlying libghostty tracked grid
    // reference remains hidden in the implementation and must stay on the
    // adapter's worker thread. Moving transfers ownership; copying would create
    // ambiguous ownership of the tracked reference and is intentionally
    // disabled.
    class TrackedHyperlink final {
    public:
        TrackedHyperlink(TrackedHyperlink &&) noexcept;
        TrackedHyperlink &operator=(TrackedHyperlink &&) noexcept;
        ~TrackedHyperlink();

        TrackedHyperlink(const TrackedHyperlink &) = delete;
        TrackedHyperlink &operator=(const TrackedHyperlink &) = delete;

    private:
        class Impl;

        friend class GhosttyVtAdapter;
        friend class GhosttyVtAdapter::Impl;

        explicit TrackedHyperlink(std::unique_ptr<Impl> impl);

        std::unique_ptr<Impl> impl_;
    };

    struct Geometry {
        int columns = 80;
        int rows = 24;
        int cellWidthPixels = 8;
        int cellHeightPixels = 16;
        int surfaceWidthPixels = 640;
        int surfaceHeightPixels = 384;
    };

    struct Options {
        Geometry geometry;
        // libghostty's max_scrollback initialization option is byte-valued.
        quint64 scrollbackBytes = 10'000'000;
        TerminalAppearance appearance;
    };

    struct Callbacks {
        std::function<void(const QByteArray &)> writePty;
    };

    struct RenderSnapshot {
        TerminalUpdate update;
        bool mouseTracking = false;
    };

    enum class RenderResult {
        Ready,
        Unavailable,
        Retry,
    };

    struct DeferredEffects {
        // A null QString means that the effect did not occur. An empty,
        // non-null QString remains a valid title or working directory value.
        QString title;
        QString currentDirectory;
        bool bell = false;
    };

    struct HyperlinkMatch {
        // OSC 8 destinations are byte strings. Keep the original bytes for
        // exact clipboard output and defer QUrl conversion to the GUI thread.
        QByteArray uri;
        // Current viewport coordinate of the logical target. Unlike the
        // original pointer coordinate, this follows scrolling and reflow.
        QPoint targetCell{-1, -1};
        QVector<QPoint> cells;
    };

    struct TextRangeMatch {
        // The original regex match, not necessarily the complete grapheme
        // content of its inclusive endpoint cells.
        QByteArray text;
        QPoint targetCell{-1, -1};
        QVector<QPoint> cells;
        // Visible viewport rows belonging to the target's complete semantic,
        // unwrapped logical line. A worker can use this sparse set to decide
        // whether a dirty row requires re-running regex precedence.
        QVector<int> logicalLineRows;
    };

    enum class SearchScreen : quint8 {
        Primary,
        Alternate,
    };

    struct SearchExtent {
        quint32 totalRows = 0;
        int columns = 0;
        int rows = 0;
        quint64 viewportOffset = 0;
        quint64 viewportLength = 0;
        SearchScreen activeScreen = SearchScreen::Primary;
    };

    // A bounded, value-only snapshot of one physical row in full-screen
    // coordinates. Each emitted UTF-8 byte maps to its owning cell. The
    // caller uses wrapped to decide whether to concatenate the following row
    // or insert a hard newline mapped to newlineCell.
    struct SearchRowSnapshot {
        quint32 screenRow = 0;
        QByteArray text;
        QVector<TerminalSearchCell> byteCells;
        bool wrapped = false;
        TerminalSearchCell newlineCell;
    };

    static std::unique_ptr<GhosttyVtAdapter> create(
        const Options &options, Callbacks callbacks = {});
    static bool isPasteSafe(QByteArrayView text);
    ~GhosttyVtAdapter();

    GhosttyVtAdapter(const GhosttyVtAdapter &) = delete;
    GhosttyVtAdapter &operator=(const GhosttyVtAdapter &) = delete;
    GhosttyVtAdapter(GhosttyVtAdapter &&) = delete;
    GhosttyVtAdapter &operator=(GhosttyVtAdapter &&) = delete;

    bool resize(const Geometry &geometry);
    bool setAppearance(const TerminalAppearance &appearance);
    void writeVt(QByteArrayView data);
    void reset();
    void synchronizeInputModes();

    QByteArray encodeKey(const TerminalKeyInput &input);
    QByteArray encodeMouse(const TerminalMouseInput &input);
    QByteArray encodeFocus(bool focused) const;
    QByteArray encodePaste(const QString &text) const;

    QString selectedText() const;
    QString selectedTextForSearch(bool trim = false) const;
    bool hasSelection() const;
    void clearSelection();
    bool beginSelection(int column, int row, int clickCount, bool rectangular);
    bool updateSelection(int column, int row, bool rectangular);
    void endSelection(int column, int row);
    bool selectAll();
    bool adjustSelection(TerminalSelectionAdjustment adjustment);
    bool scrollViewport(const TerminalViewportRequest &request);
    std::optional<SearchExtent> searchExtent() const;
    std::optional<SearchRowSnapshot> snapshotSearchRow(
        quint32 screenRow) const;
    QVector<QPoint> visibleCellsForSearchRange(
        const TerminalSearchRange &range) const;
    bool scrollSearchRangeIntoView(const TerminalSearchRange &range);
    // Resolve a viewport-relative cell and, for hover rendering, every
    // candidate visible cell with the same URI. Public libghostty-vt does not
    // expose OSC 8 identity, so equal-URI links cannot be distinguished here.
    std::optional<HyperlinkMatch> hyperlinkAt(
        int column, int row, const QVector<QPoint> &candidateCells) const;
    // Create a sparse, owned logical anchor for an OSC 8 cell. The target can
    // move through scroll, pruning, and reflow without exposing a Ghostty
    // handle outside this adapter.
    std::optional<TrackedHyperlink> trackHyperlinkAt(int column, int row) const;
    // Whether the logical target still exists and retains its original URI.
    // This deliberately remains true while the target is off-screen or its
    // owning primary/alternate screen is inactive.
    bool trackedHyperlinkValid(const TrackedHyperlink &target) const;
    // Resolve a tracked target only when its owning screen is active and its
    // logical cell is visible in the current viewport.
    std::optional<HyperlinkMatch> resolveHyperlink(
        const TrackedHyperlink &target,
        const QVector<QPoint> &candidateCells) const;

    // Snapshot the complete logical line under a viewport cell. Soft wraps
    // are removed and semantic prompt-state transitions remain boundaries,
    // matching Ghostty's configured-link behavior.
    std::optional<LogicalLineSnapshot> snapshotLogicalLineAt(
        int column, int row) const;
    // Convert a non-empty half-open UTF-8 byte match into three owned logical
    // cell anchors: both inclusive endpoints and the queried target. The
    // snapshot must still belong to this adapter and no terminal mutation may
    // occur between snapshotting and this call.
    std::optional<TrackedTextRange> trackTextRange(
        const LogicalLineSnapshot &line,
        qsizetype beginByte, qsizetype endByte) const;
    // Whether both endpoints and the queried target still exist and the cell
    // range retains the text it covered when it was created. An inactive
    // owning screen is valid but cannot be resolved for viewport decoration.
    bool trackedTextRangeValid(const TrackedTextRange &range) const;
    // Resolve the currently visible portion of a tracked range. A valid range
    // that is wholly outside the viewport or on the inactive screen returns
    // no match; callers can distinguish that from invalidation with the
    // validity query above.
    std::optional<TextRangeMatch> resolveTextRange(
        const TrackedTextRange &range) const;

    std::uint64_t compressionActivity() const;
    bool compressScrollback();
    RenderResult renderFrame(RenderSnapshot *snapshot);
    DeferredEffects takeDeferredEffects();

private:
    explicit GhosttyVtAdapter(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};
