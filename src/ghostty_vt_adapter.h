#pragma once

#include "terminal_appearance.h"
#include "terminal_inspector_snapshot.h"
#include "terminal_session_options.h"
#include "terminal_types.h"
#include "terminal_write_file_action.h"

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
    struct EncodedKey {
        QByteArray bytes;
        bool success = false;
        bool modifier = false;
        bool escape = false;

        friend bool operator==(const EncodedKey &,
                               const EncodedKey &) = default;
    };

    enum class PasteDisposition : quint8 {
        Ready,
        ConfirmationRequired,
        Failed,
    };

    enum class PasteAuthorization : quint8 {
        Initial,
        Confirmed,
    };

    struct PastePreparationOptions {
        bool protection = true;
        bool bracketedSafe = true;
        PasteAuthorization authorization = PasteAuthorization::Initial;
    };

    struct PreparedPaste {
        PasteDisposition disposition = PasteDisposition::Failed;
        QByteArray bytes;

        friend bool operator==(const PreparedPaste &,
                               const PreparedPaste &) = default;
    };

    enum class PlainFileSnapshotStatus : quint8 {
        Ready,
        Unavailable,
        Failed,
    };

    struct PlainFileSnapshot {
        PlainFileSnapshotStatus status = PlainFileSnapshotStatus::Failed;
        QByteArray bytes;
    };

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

    using Geometry = TerminalSessionGeometry;

    struct Options {
        Geometry geometry;
        // libghostty's max_scrollback initialization option is byte-valued.
        quint64 scrollbackBytes = 10'000'000;
        quint64 kittyImageStorageLimitBytes = 320'000'000;
        TerminalAppearance appearance;
        TerminalColorScheme colorScheme = TerminalColorScheme::Light;
        TerminalClipboardAccess clipboardWriteAccess =
            TerminalClipboardAccess::Allow;
        // A concrete launch directory initializes terminal-owned PWD before
        // the child starts so immediately-created panes can inherit it.
        std::optional<QByteArray> initialWorkingDirectory;
        // The pinned public C bridge emits at most 255 response bytes. Retain
        // longer values without truncation so a future upstream widening is
        // inherited automatically; the current bridge treats them as silent.
        QByteArray enquiryResponse;
    };

    struct Callbacks {
        std::function<void(const QByteArray &)> writePty;
        std::function<QByteArray()> queryMachineHostName;
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

    enum class SemanticPromptState : quint8 {
        // The terminal state could not be queried. This is distinct from an
        // ordinary terminal that has not received shell integration markers.
        Unavailable,
        Away,
        AtPrompt,
    };

    enum class SelectionAutoscrollDirection : quint8 {
        None,
        Up,
        Down,
    };

    enum class SelectionAutoscrollTickResult : quint8 {
        // No active autoscroll gesture was mutated. This also covers an
        // anchor invalidated by a screen change; the installed selection must
        // remain untouched in that case.
        Unavailable,
        // A valid one-row viewport tick was applied and the resulting
        // selection was either installed or cleared according to Ghostty's
        // gesture result. The viewport may already be at its scroll boundary.
        Mutated,
    };

    struct DeferredEffects {
        // A null value means that the effect did not occur. An empty,
        // non-null value remains a valid title or working-directory update.
        QString title;
        QByteArray currentDirectory;
        bool bell = false;
        QVector<TerminalClipboardWriteRequest> clipboardWrites;
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

    static std::unique_ptr<GhosttyVtAdapter> create(const Options &options,
                                                    Callbacks callbacks = {});
    ~GhosttyVtAdapter();

    GhosttyVtAdapter(const GhosttyVtAdapter &) = delete;
    GhosttyVtAdapter &operator=(const GhosttyVtAdapter &) = delete;
    GhosttyVtAdapter(GhosttyVtAdapter &&) = delete;
    GhosttyVtAdapter &operator=(GhosttyVtAdapter &&) = delete;

    bool resize(const Geometry &geometry);
    bool setAppearance(const TerminalAppearance &appearance);
    bool setKittyImageStorageLimit(quint64 bytes);
    void setColorScheme(TerminalColorScheme scheme);
    void setClipboardWriteAccess(TerminalClipboardAccess access);
    void setEnquiryResponse(const QByteArray &response);
    void writeVt(QByteArrayView data);
    // Observe the active screen's final logical row at a renderer/frame
    // boundary. The first successful observation and every later PageList
    // node/y change return true. Cell rewrites on that row return false,
    // matching Ghostty's scroll-to-bottom-on-output renderer policy. Query
    // failures and synchronized output retain the previous observation.
    bool observeOutputBottomAnchorChanged();
    void reset();
    void synchronizeInputModes();
    // Child-exit waiting uses ordinary legacy press encoding regardless of
    // modes left behind by the application, matching Ghostty's surface
    // lifecycle normalization.
    void normalizeKeyboardAfterCommandExit();

    EncodedKey encodeKey(const TerminalKeyInput &input);
    // ANSI mode 2 is terminal state. Query it at the worker input boundary so
    // terminal output and live policy updates remain ordered with input.
    bool keyboardActionMode() const;
    bool mouseTracking() const;
    QByteArray encodeMouse(const TerminalMouseInput &input);
    // A present value means DECSET 1007 owns this wheel input. The bytes use
    // the terminal's current DECCKM mode and contain exactly one cursor-key
    // sequence per signed row. A horizontal-only input therefore produces a
    // present empty value: alternate scroll consumes it without generating
    // bytes. Absence leaves ordinary mouse/viewport routing to the worker.
    std::optional<QByteArray> alternateScrollSequence(qint64 rows) const;
    QByteArray encodeFocus(bool focused) const;
    QByteArray encodePaste(const QString &text) const;
    // Classification and encoding share one bracketed-mode snapshot so an
    // accepted request cannot be encoded under different terminal state.
    PreparedPaste preparePaste(const QString &text,
                               const PastePreparationOptions &options) const;

    QString selectedText(bool trim = true) const;
    // Snapshot the exact plain write_*_file range. Ready may contain zero
    // bytes; Unavailable distinguishes a missing selection/history range from
    // an adapter failure so the worker can preserve Ghostty's no-op behavior.
    PlainFileSnapshot snapshotPlainFile(TerminalFileLocation location) const;
    bool hasSelection() const;
    void clearSelection();
    void resetSelectionGesture();
    void clearSelectionAndResetGesture();
    bool setSelectionWordChars(const QVector<uint32_t> &wordBoundaryCodepoints);
    bool setClickRepeatIntervalMilliseconds(quint32 milliseconds);
    bool beginSelection(const TerminalSelectionPressInput &input);
    bool updateSelection(const TerminalSelectionDragInput &input);
    void endSelection(int column, int row);
    bool selectionGestureDragged() const;
    SelectionAutoscrollDirection selectionAutoscrollDirection() const;
    SelectionAutoscrollTickResult
    selectionAutoscrollTick(const TerminalSelectionDragInput &input);
    bool selectionContains(int column, int row) const;
    bool selectCell(int column, int row);
    bool selectWord(int column, int row);
    bool selectAll();
    bool adjustSelection(TerminalSelectionAdjustment adjustment);
    bool scrollViewport(const TerminalViewportRequest &request);
    std::optional<SearchExtent> searchExtent() const;
    std::optional<SearchRowSnapshot> snapshotSearchRow(quint32 screenRow) const;
    QVector<QPoint>
    visibleCellsForSearchRange(const TerminalSearchRange &range) const;
    bool scrollSearchRangeIntoView(const TerminalSearchRange &range);
    // Resolve a viewport-relative cell and, for hover rendering, every
    // candidate visible cell with the same URI. Public libghostty-vt does not
    // expose OSC 8 identity, so equal-URI links cannot be distinguished here.
    std::optional<HyperlinkMatch>
    hyperlinkAt(int column, int row,
                const QVector<QPoint> &candidateCells) const;
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
    std::optional<HyperlinkMatch>
    resolveHyperlink(const TrackedHyperlink &target,
                     const QVector<QPoint> &candidateCells) const;

    // Snapshot the complete logical line under a viewport cell. Soft wraps
    // are removed and semantic prompt-state transitions remain boundaries,
    // matching Ghostty's configured-link behavior.
    std::optional<LogicalLineSnapshot> snapshotLogicalLineAt(int column,
                                                             int row) const;
    // Convert a non-empty half-open UTF-8 byte match into three owned logical
    // cell anchors: both inclusive endpoints and the queried target. The
    // snapshot must still belong to this adapter and no terminal mutation may
    // occur between snapshotting and this call.
    std::optional<TrackedTextRange>
    trackTextRange(const LogicalLineSnapshot &line, qsizetype beginByte,
                   qsizetype endByte) const;
    // Whether both endpoints and the queried target still exist and the cell
    // range retains the text it covered when it was created. An inactive
    // owning screen is valid but cannot be resolved for viewport decoration.
    bool trackedTextRangeValid(const TrackedTextRange &range) const;
    // Install the current tracked endpoints as the active selection only if
    // their covered text and owning active screen still match.
    bool installTextRange(const TrackedTextRange &range);
    // Resolve the currently visible portion of a tracked range. A valid range
    // that is wholly outside the viewport or on the inactive screen returns
    // no match; callers can distinguish that from invalidation with the
    // validity query above.
    std::optional<TextRangeMatch>
    resolveTextRange(const TrackedTextRange &range) const;

    // Copy public terminal state into an owned diagnostic value. This is
    // intentionally request-driven rather than part of every render update.
    [[nodiscard]] TerminalInspectorSnapshot inspectorSnapshot() const;
    [[nodiscard]] TerminalInspectorCellSnapshot
    inspectorCellSnapshot(int viewportColumn, int viewportRow) const;

    // Mirror Ghostty's cursor-at-prompt policy using the public C surface:
    // alternate screens are always Away; a semantic prompt/continuation row
    // is AtPrompt; otherwise the stored cursor cell decides. Public
    // libghostty-vt does not expose the live cursor semantic mode or whether
    // shell integration has ever been observed, so Away also covers a
    // terminal without semantic prompt integration.
    SemanticPromptState semanticPromptState() const;
    std::uint64_t compressionActivity() const;
    bool compressScrollback();
    RenderResult renderFrame(RenderSnapshot *snapshot);
    DeferredEffects takeDeferredEffects();

private:
    explicit GhosttyVtAdapter(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};
