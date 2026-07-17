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
    bool hasSelection() const;
    void clearSelection();
    bool beginSelection(int column, int row, int clickCount, bool rectangular);
    bool updateSelection(int column, int row, bool rectangular);
    void endSelection(int column, int row);
    bool selectAll();
    bool adjustSelection(TerminalSelectionAdjustment adjustment);
    bool scrollViewport(const TerminalViewportRequest &request);
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

    std::uint64_t compressionActivity() const;
    bool compressScrollback();
    RenderResult renderFrame(RenderSnapshot *snapshot);
    DeferredEffects takeDeferredEffects();

private:
    explicit GhosttyVtAdapter(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};
