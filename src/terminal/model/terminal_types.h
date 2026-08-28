#pragma once

#include "terminal/model/terminal_actions.h"
#include "terminal/rendering/terminal_kitty_graphics.h"

#include <QBitArray>
#include <QByteArray>
#include <QColor>
#include <QDebug>
#include <QMetaType>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <compare>
#include <cstdint>
#include <iterator>
#include <memory>

// The renderer needs the original SGR foreground source in addition to the
// resolved RGB value. Ghostty's bold-color=bright behavior only promotes the
// first eight palette entries; direct RGB and the remaining palette entries
// must retain their color.
enum class TerminalColorSource : quint8 {
    Default,
    Palette,
    Rgb,
};

enum class TerminalUnderlineStyle : quint8 {
    None,
    Single,
    Double,
    Curly,
    Dotted,
    Dashed,
};

// Worker-owned hyperlink anchors can remain meaningful while their cell is
// temporarily outside the viewport or belongs to the inactive screen. Keep
// that distinct from permanent invalidation so a hover can reappear without
// rescanning when the tracked cell becomes visible again.
enum class TerminalHyperlinkState : quint8 {
    Invalid,
    // The viewport coordinate came from an older frame. This is retryable
    // once the UI installs the worker revision returned with the result.
    Stale,
    Hidden,
    Visible,
};

// OSC 8 and regex-detected links share the same hover and activation
// machinery, but opening a regex match may first resolve a relative path
// against the terminal working directory.
enum class TerminalLinkKind : quint8 {
    Osc8,
    Regex,
};

enum class TerminalSearchDirection : quint8 {
    Next,
    Previous,
};

// Cell colors originate in libghostty's opaque RGB8 render-state API. Keep
// that exact representation in the per-cell transport instead of retaining a
// 16-byte QColor three times for every visible grid position. Zero represents
// an invalid/default-constructed color; bit 24 distinguishes opaque black from
// that sentinel.
class TerminalCellColor final {
public:
    constexpr TerminalCellColor() noexcept = default;

    explicit TerminalCellColor(const QColor &color) noexcept { assign(color); }
    explicit TerminalCellColor(Qt::GlobalColor color) noexcept
        : TerminalCellColor(QColor(color))
    {}

    TerminalCellColor &operator=(const QColor &color) noexcept
    {
        assign(color);
        return *this;
    }

    TerminalCellColor &operator=(Qt::GlobalColor color) noexcept
    {
        return *this = QColor(color);
    }

    [[nodiscard]] static constexpr TerminalCellColor
    fromRgb(quint8 red, quint8 green, quint8 blue) noexcept
    {
        TerminalCellColor result;
        result.value_ = validMask | static_cast<quint32>(red) << 16U
            | static_cast<quint32>(green) << 8U | static_cast<quint32>(blue);
        return result;
    }

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return (value_ & validMask) != 0;
    }

    [[nodiscard]] QColor toQColor() const noexcept
    {
        return isValid() ? QColor::fromRgb(opaqueAlpha | rgb24()) : QColor{};
    }

    [[nodiscard]] QRgb rgba() const noexcept { return toQColor().rgba(); }

    operator QColor() const noexcept { return toQColor(); }

    friend constexpr bool operator==(TerminalCellColor,
                                     TerminalCellColor) noexcept = default;

    friend bool operator==(TerminalCellColor left, const QColor &right) noexcept
    {
        return left.toQColor() == right;
    }

    friend bool operator==(const QColor &left, TerminalCellColor right) noexcept
    {
        return left == right.toQColor();
    }

    friend QDebug operator<<(QDebug debug, TerminalCellColor color)
    {
        return debug << color.toQColor();
    }

private:
    static constexpr quint32 rgbMask = 0x00ff'ffffU;
    static constexpr quint32 validMask = 0x0100'0000U;
    static constexpr QRgb opaqueAlpha = 0xff00'0000U;

    [[nodiscard]] constexpr QRgb rgb24() const noexcept
    {
        return static_cast<QRgb>(value_ & rgbMask);
    }

    void assign(const QColor &color) noexcept
    {
        if (!color.isValid()) {
            value_ = 0;
            return;
        }
        const QColor rgbColor = color.toRgb();
        Q_ASSERT(rgbColor.alpha() == 255);
        if (rgbColor.alpha() != 255) {
            value_ = 0;
            return;
        }
        value_ = fromRgb(static_cast<quint8>(rgbColor.red()),
                         static_cast<quint8>(rgbColor.green()),
                         static_cast<quint8>(rgbColor.blue()))
                     .value_;
    }

    quint32 value_ = 0;
};

static_assert(sizeof(TerminalCellColor) == sizeof(quint32));
static_assert(alignof(TerminalCellColor) == alignof(quint32));

// Search results use full-screen coordinates so they remain independent of
// the current viewport. They are value-only snapshots, not libghostty grid
// references, and are valid only for the terminal revision that produced
// them.
struct TerminalSearchCell {
    quint16 column = 0;
    quint32 screenRow = 0;

    friend constexpr std::strong_ordering
    operator<=>(const TerminalSearchCell &left, const TerminalSearchCell &right)
    {
        if (const auto rowOrder = left.screenRow <=> right.screenRow;
            rowOrder != 0) {
            return rowOrder;
        }
        return left.column <=> right.column;
    }

    friend bool operator==(const TerminalSearchCell &,
                           const TerminalSearchCell &) = default;
};

struct TerminalSearchRange {
    TerminalSearchCell start;
    TerminalSearchCell end;

    friend bool operator==(const TerminalSearchRange &,
                           const TerminalSearchRange &) = default;
};

struct TerminalSearchUpdate {
    quint64 generation = 0;
    quint64 contentRevision = 0;
    bool active = false;
    bool complete = false;
    quint64 scannedRows = 0;
    quint64 totalRows = 0;
    quint64 totalMatches = 0;
    qint64 selectedMatch = -1;
    int columns = 0;
    int rows = 0;
    // Row-major viewport masks. For active searches, the worker sizes both
    // masks to columns * rows; inactive updates leave them empty. The visible
    // mask may include independently probed viewport candidates whose
    // canonical index is not reflected in totalMatches yet. The selected mask
    // is canonical.
    QBitArray visibleCellMask;
    QBitArray selectedCellMask;
};

// Terminal cells are copied across the worker/UI boundary and retained for
// every visible grid position. Keep their compact scalar state in one
// explicitly masked word rather than relying on implementation-defined C++
// bitfield layout.
class TerminalCellMetadata {
public:
    TerminalCellMetadata() noexcept = default;

private:
    friend struct TerminalCell;

    enum class Flag : quint32 {
        PlainCodepoint = 1U << 0,
        ExtendedGrapheme = 1U << 1,
        Bold = 1U << 2,
        Italic = 1U << 3,
        Faint = 1U << 4,
        TextBlink = 1U << 5,
        Inverse = 1U << 6,
        Invisible = 1U << 7,
        UnderlineUsesForeground = 1U << 8,
        StrikeThrough = 1U << 9,
        Overline = 1U << 10,
        Selected = 1U << 11,
        BackgroundExplicit = 1U << 12,
        MinimumContrastExemptGlyph = 1U << 13,
        HasHyperlink = 1U << 14,
        Spacer = 1U << 15,
        Wide = 1U << 30,
    };

    static constexpr quint32 foregroundSourceShift = 16;
    static constexpr quint32 foregroundSourceMask = 0x3U
        << foregroundSourceShift;
    static constexpr quint32 foregroundPaletteIndexShift = 18;
    static constexpr quint32 foregroundPaletteIndexMask = 0x1ffU
        << foregroundPaletteIndexShift;
    static constexpr quint32 underlineStyleShift = 27;
    static constexpr quint32 underlineStyleMask = 0x7U << underlineStyleShift;

    static_assert(static_cast<quint32>(TerminalColorSource::Rgb)
                  <= (foregroundSourceMask >> foregroundSourceShift));
    static_assert(static_cast<quint32>(TerminalUnderlineStyle::Dashed)
                  <= (underlineStyleMask >> underlineStyleShift));

    [[nodiscard]] bool flag(Flag flag) const noexcept
    {
        return (value_ & static_cast<quint32>(flag)) != 0;
    }

    void setFlag(Flag flag, bool enabled) noexcept
    {
        const quint32 mask = static_cast<quint32>(flag);
        value_ = enabled ? value_ | mask : value_ & ~mask;
    }

    [[nodiscard]] quint32 field(quint32 mask, quint32 shift) const noexcept
    {
        return (value_ & mask) >> shift;
    }

    void setField(quint32 mask, quint32 shift, quint32 value) noexcept
    {
        Q_ASSERT((value & ~(mask >> shift)) == 0);
        value_ = (value_ & ~mask) | ((value << shift) & mask);
    }

    quint32 value_ = static_cast<quint32>(Flag::UnderlineUsesForeground);
};

static_assert(sizeof(TerminalCellMetadata) == sizeof(quint32));
static_assert(alignof(TerminalCellMetadata) == alignof(quint32));

struct TerminalCell {
    QString text;
    TerminalCellColor foreground;
    TerminalCellColor background;
    TerminalCellColor underlineColor;
    // Preserve the terminal's authoritative base cell content separately
    // from its UTF-16 grapheme. Renderer shaping rules must distinguish a
    // plain `f` from a grapheme that merely begins with `f`.
    quint32 baseCodepoint = 0;

    [[nodiscard]] bool plainCodepoint() const noexcept
    {
        return metadata_.flag(TerminalCellMetadata::Flag::PlainCodepoint);
    }
    void setPlainCodepoint(bool value) noexcept
    {
        metadata_.setFlag(TerminalCellMetadata::Flag::PlainCodepoint, value);
    }

    // Ghostty's cursor shaping rule keeps cells with extra grapheme
    // codepoints joined, but still breaks around plain and empty cells.
    [[nodiscard]] bool extendedGrapheme() const noexcept
    {
        return metadata_.flag(TerminalCellMetadata::Flag::ExtendedGrapheme);
    }
    void setExtendedGrapheme(bool value) noexcept
    {
        metadata_.setFlag(TerminalCellMetadata::Flag::ExtendedGrapheme, value);
    }

    [[nodiscard]] TerminalColorSource styleForegroundSource() const noexcept
    {
        return static_cast<TerminalColorSource>(
            metadata_.field(TerminalCellMetadata::foregroundSourceMask,
                            TerminalCellMetadata::foregroundSourceShift));
    }
    void setStyleForegroundSource(TerminalColorSource value) noexcept
    {
        const quint32 encoded = static_cast<quint32>(value);
        Q_ASSERT(encoded <= static_cast<quint32>(TerminalColorSource::Rgb));
        metadata_.setField(
            TerminalCellMetadata::foregroundSourceMask,
            TerminalCellMetadata::foregroundSourceShift,
            encoded <= static_cast<quint32>(TerminalColorSource::Rgb)
                ? encoded
                : static_cast<quint32>(TerminalColorSource::Default));
    }

    [[nodiscard]] int styleForegroundPaletteIndex() const noexcept
    {
        return static_cast<int>(metadata_.field(
                   TerminalCellMetadata::foregroundPaletteIndexMask,
                   TerminalCellMetadata::foregroundPaletteIndexShift))
            - 1;
    }
    void setStyleForegroundPaletteIndex(int value) noexcept
    {
        Q_ASSERT(value >= -1 && value <= 255);
        const quint32 encoded =
            static_cast<quint32>(std::clamp(value, -1, 255) + 1);
        metadata_.setField(TerminalCellMetadata::foregroundPaletteIndexMask,
                           TerminalCellMetadata::foregroundPaletteIndexShift,
                           encoded);
    }

    [[nodiscard]] bool bold() const noexcept
    {
        return metadata_.flag(TerminalCellMetadata::Flag::Bold);
    }
    void setBold(bool value) noexcept
    {
        metadata_.setFlag(TerminalCellMetadata::Flag::Bold, value);
    }

    [[nodiscard]] bool italic() const noexcept
    {
        return metadata_.flag(TerminalCellMetadata::Flag::Italic);
    }
    void setItalic(bool value) noexcept
    {
        metadata_.setFlag(TerminalCellMetadata::Flag::Italic, value);
    }

    [[nodiscard]] bool faint() const noexcept
    {
        return metadata_.flag(TerminalCellMetadata::Flag::Faint);
    }
    void setFaint(bool value) noexcept
    {
        metadata_.setFlag(TerminalCellMetadata::Flag::Faint, value);
    }

    // Retained for semantic parity. The pinned Ghostty renderer currently
    // records SGR blink but deliberately does not animate text with it.
    [[nodiscard]] bool textBlink() const noexcept
    {
        return metadata_.flag(TerminalCellMetadata::Flag::TextBlink);
    }
    void setTextBlink(bool value) noexcept
    {
        metadata_.setFlag(TerminalCellMetadata::Flag::TextBlink, value);
    }

    [[nodiscard]] bool inverse() const noexcept
    {
        return metadata_.flag(TerminalCellMetadata::Flag::Inverse);
    }
    void setInverse(bool value) noexcept
    {
        metadata_.setFlag(TerminalCellMetadata::Flag::Inverse, value);
    }

    [[nodiscard]] bool invisible() const noexcept
    {
        return metadata_.flag(TerminalCellMetadata::Flag::Invisible);
    }
    void setInvisible(bool value) noexcept
    {
        metadata_.setFlag(TerminalCellMetadata::Flag::Invisible, value);
    }

    [[nodiscard]] bool underlineUsesForeground() const noexcept
    {
        return metadata_.flag(
            TerminalCellMetadata::Flag::UnderlineUsesForeground);
    }
    void setUnderlineUsesForeground(bool value) noexcept
    {
        metadata_.setFlag(TerminalCellMetadata::Flag::UnderlineUsesForeground,
                          value);
    }

    [[nodiscard]] TerminalUnderlineStyle underlineStyle() const noexcept
    {
        return static_cast<TerminalUnderlineStyle>(
            metadata_.field(TerminalCellMetadata::underlineStyleMask,
                            TerminalCellMetadata::underlineStyleShift));
    }
    void setUnderlineStyle(TerminalUnderlineStyle value) noexcept
    {
        const quint32 encoded = static_cast<quint32>(value);
        Q_ASSERT(encoded
                 <= static_cast<quint32>(TerminalUnderlineStyle::Dashed));
        metadata_.setField(
            TerminalCellMetadata::underlineStyleMask,
            TerminalCellMetadata::underlineStyleShift,
            encoded <= static_cast<quint32>(TerminalUnderlineStyle::Dashed)
                ? encoded
                : static_cast<quint32>(TerminalUnderlineStyle::None));
    }

    [[nodiscard]] bool strikeThrough() const noexcept
    {
        return metadata_.flag(TerminalCellMetadata::Flag::StrikeThrough);
    }
    void setStrikeThrough(bool value) noexcept
    {
        metadata_.setFlag(TerminalCellMetadata::Flag::StrikeThrough, value);
    }

    [[nodiscard]] bool overline() const noexcept
    {
        return metadata_.flag(TerminalCellMetadata::Flag::Overline);
    }
    void setOverline(bool value) noexcept
    {
        metadata_.setFlag(TerminalCellMetadata::Flag::Overline, value);
    }

    [[nodiscard]] bool selected() const noexcept
    {
        return metadata_.flag(TerminalCellMetadata::Flag::Selected);
    }
    void setSelected(bool value) noexcept
    {
        metadata_.setFlag(TerminalCellMetadata::Flag::Selected, value);
    }

    // True when libghostty reports a cell-owned background source. This
    // provenance cannot be recovered from the resolved RGB value because an
    // explicit color may equal the terminal's global background. Keep it
    // independent of inverse, selection, and search presentation policy.
    [[nodiscard]] bool backgroundExplicit() const noexcept
    {
        return metadata_.flag(TerminalCellMetadata::Flag::BackgroundExplicit);
    }
    void setBackgroundExplicit(bool value) noexcept
    {
        metadata_.setFlag(TerminalCellMetadata::Flag::BackgroundExplicit,
                          value);
    }

    // Derived from Ghostty's raw base codepoint, not from shaped QString
    // contents. Only the glyph is exempt; decorations still use contrast.
    [[nodiscard]] bool minimumContrastExemptGlyph() const noexcept
    {
        return metadata_.flag(
            TerminalCellMetadata::Flag::MinimumContrastExemptGlyph);
    }
    void setMinimumContrastExemptGlyph(bool value) noexcept
    {
        metadata_.setFlag(
            TerminalCellMetadata::Flag::MinimumContrastExemptGlyph, value);
    }

    // The URI remains worker-owned and is resolved only for an active hover.
    // This cheap bit lets the UI avoid querying ordinary cells.
    [[nodiscard]] bool hasHyperlink() const noexcept
    {
        return metadata_.flag(TerminalCellMetadata::Flag::HasHyperlink);
    }
    void setHasHyperlink(bool value) noexcept
    {
        metadata_.setFlag(TerminalCellMetadata::Flag::HasHyperlink, value);
    }

    [[nodiscard]] bool spacer() const noexcept
    {
        return metadata_.flag(TerminalCellMetadata::Flag::Spacer);
    }
    void setSpacer(bool value) noexcept
    {
        metadata_.setFlag(TerminalCellMetadata::Flag::Spacer, value);
    }

    [[nodiscard]] int columnSpan() const noexcept
    {
        return metadata_.flag(TerminalCellMetadata::Flag::Wide) ? 2 : 1;
    }
    void setColumnSpan(int value) noexcept
    {
        Q_ASSERT(value == 1 || value == 2);
        metadata_.setFlag(TerminalCellMetadata::Flag::Wide,
                          std::clamp(value, 1, 2) == 2);
    }

private:
    TerminalCellMetadata metadata_;
};

#if defined(Q_PROCESSOR_X86_64)
static_assert(sizeof(TerminalCell) == 48);
#endif

struct TerminalRowPresentation {
    // Ghostty's conservative vertical-padding heuristic rejects prompt rows,
    // default backgrounds, default-colored explicit backgrounds, and
    // perfect-fit powerline glyphs.
    bool paddingExtensionSafe = false;

    bool operator==(const TerminalRowPresentation &) const = default;
};

// Retained terminal cells remain logically flat for callers, but each physical
// row owns an independently implicitly-shared QVector payload. A render-thread
// frame copy therefore keeps every row alive without forcing the next dirty-row
// update to detach and copy the complete viewport.
//
// Mutation is intentionally limited to applyTerminalUpdate(). Render consumers
// should prefer rowAt() or cell(row, column) in row-major loops; at() and the
// read-only random-access iterator retain the former flat indexing contract for
// less performance-sensitive callers.
class TerminalFrameCellStorage final {
public:
    using Row = QVector<TerminalCell>;

    class const_iterator final {
    public:
        using difference_type = qsizetype;
        using value_type = TerminalCell;
        using pointer = const TerminalCell *;
        using reference = const TerminalCell &;
        using iterator_category = std::random_access_iterator_tag;
        using iterator_concept = std::random_access_iterator_tag;

        constexpr const_iterator() noexcept = default;

        [[nodiscard]] reference operator*() const
        {
            return storage_->at(index_);
        }
        [[nodiscard]] pointer operator->() const
        {
            return std::addressof(storage_->at(index_));
        }
        [[nodiscard]] reference operator[](difference_type offset) const
        {
            return storage_->at(index_ + offset);
        }

        const_iterator &operator++() noexcept
        {
            ++index_;
            return *this;
        }
        const_iterator operator++(int) noexcept
        {
            const const_iterator previous = *this;
            ++*this;
            return previous;
        }
        const_iterator &operator--() noexcept
        {
            --index_;
            return *this;
        }
        const_iterator operator--(int) noexcept
        {
            const const_iterator previous = *this;
            --*this;
            return previous;
        }
        const_iterator &operator+=(difference_type offset) noexcept
        {
            index_ += offset;
            return *this;
        }
        const_iterator &operator-=(difference_type offset) noexcept
        {
            index_ -= offset;
            return *this;
        }

        friend const_iterator operator+(const_iterator iterator,
                                        difference_type offset) noexcept
        {
            iterator += offset;
            return iterator;
        }
        friend const_iterator operator+(difference_type offset,
                                        const_iterator iterator) noexcept
        {
            iterator += offset;
            return iterator;
        }
        friend const_iterator operator-(const_iterator iterator,
                                        difference_type offset) noexcept
        {
            iterator -= offset;
            return iterator;
        }
        friend difference_type operator-(const const_iterator &left,
                                         const const_iterator &right) noexcept
        {
            Q_ASSERT(left.storage_ == right.storage_);
            return left.index_ - right.index_;
        }

        friend bool operator==(const const_iterator &,
                               const const_iterator &) = default;
        friend std::strong_ordering
        operator<=>(const const_iterator &left,
                    const const_iterator &right) noexcept
        {
            Q_ASSERT(left.storage_ == right.storage_);
            return left.index_ <=> right.index_;
        }

    private:
        friend class TerminalFrameCellStorage;

        constexpr const_iterator(const TerminalFrameCellStorage *storage,
                                 qsizetype index) noexcept
            : storage_(storage)
            , index_(index)
        {}

        const TerminalFrameCellStorage *storage_ = nullptr;
        qsizetype index_ = 0;
    };

    [[nodiscard]] qsizetype size() const noexcept { return size_; }
    [[nodiscard]] bool isEmpty() const noexcept { return size_ == 0; }
    [[nodiscard]] int columnCount() const noexcept { return columns_; }
    [[nodiscard]] qsizetype rowCount() const noexcept { return rows_.size(); }

    [[nodiscard]] const TerminalCell &at(qsizetype index) const
    {
        Q_ASSERT(index >= 0 && index < size_ && columns_ > 0);
        const qsizetype row = index / columns_;
        const qsizetype column = index - row * columns_;
        return rows_.at(row).at(column);
    }
    [[nodiscard]] const TerminalCell &operator[](qsizetype index) const
    {
        return at(index);
    }
    [[nodiscard]] const TerminalCell &constFirst() const { return at(0); }
    [[nodiscard]] const Row &rowAt(int row) const { return rows_.at(row); }
    [[nodiscard]] const TerminalCell &cell(int row, int column) const
    {
        return rows_.at(row).at(column);
    }

    [[nodiscard]] const_iterator begin() const noexcept { return cbegin(); }
    [[nodiscard]] const_iterator end() const noexcept { return cend(); }
    [[nodiscard]] const_iterator begin() noexcept { return cbegin(); }
    [[nodiscard]] const_iterator end() noexcept { return cend(); }
    [[nodiscard]] const_iterator cbegin() const noexcept
    {
        return const_iterator(this, 0);
    }
    [[nodiscard]] const_iterator cend() const noexcept
    {
        return const_iterator(this, size_);
    }

    // These identities are intended for deterministic copy-on-write tests and
    // benchmark counters. They expose no mutable storage.
    [[nodiscard]] const Row *rowTableData() const noexcept
    {
        return rows_.constData();
    }
    [[nodiscard]] const TerminalCell *rowData(int row) const
    {
        return rows_.at(row).constData();
    }
    [[nodiscard]] bool rowTableIsDetached() const noexcept
    {
        return rows_.isDetached();
    }

private:
    void resetGrid(int columns, int rows)
    {
        Q_ASSERT(columns > 0 && rows > 0);
        columns_ = columns;
        size_ = static_cast<qsizetype>(columns) * rows;
        rows_ = QVector<Row>(rows);
    }

    void replaceRow(int row, const Row &cells)
    {
        Q_ASSERT(row >= 0 && row < rows_.size());
        Q_ASSERT(cells.size() == columns_);
        rows_[row] = cells;
    }

    int columns_ = 0;
    qsizetype size_ = 0;
    QVector<Row> rows_;

    friend struct TerminalFrameCellStorageAccess;
};

struct TerminalFrameCellStorageAccess final {
    static void resetGrid(TerminalFrameCellStorage &storage, int columns,
                          int rows)
    {
        storage.resetGrid(columns, rows);
    }

    static void replaceRow(TerminalFrameCellStorage &storage, int row,
                           const TerminalFrameCellStorage::Row &cells)
    {
        storage.replaceRow(row, cells);
    }
};

struct TerminalFrameApplyMetrics {
    quint64 rowTableAllocations = 0;
    quint64 rowTableDetaches = 0;
    quint64 rowHeadersCopied = 0;
    quint64 rowPayloadsInstalled = 0;
    quint64 rowPayloadsReused = 0;
    quint64 cellPayloadAllocations = 0;
    quint64 terminalCellsCopied = 0;
};

struct TerminalFrame {
    int columns = 0;
    int rows = 0;
    TerminalFrameCellStorage cells;
    QVector<TerminalRowPresentation> rowPresentation;
    QColor foreground = QColor(QStringLiteral("#d8dee9"));
    QColor background = QColor(QStringLiteral("#1e222a"));
    QColor cursorColor = QColor(QStringLiteral("#d8dee9"));
    QVector<QColor> palette;
    bool cursorColorExplicit = false;
    bool cursorVisible = false;
    bool cursorBlinking = false;
    int cursorColumn = 0;
    int cursorRow = 0;
    int cursorStyle = 1;
    int cursorColumnSpan = 1;
    quint64 scrollTotal = 0;
    quint64 scrollOffset = 0;
    quint64 scrollLength = 0;
    std::shared_ptr<const TerminalKittyGraphicsSnapshot> kittyGraphics;
    // Monotonic worker-owned revision for terminal content and viewport
    // mutations. New hyperlink anchors require the exact frame revision that
    // supplied their initial viewport coordinate; accepted anchors then follow
    // the logical cell independently through later revisions.
    quint64 contentRevision = 0;
};

// A row is the smallest cell payload that crosses the worker/UI thread
// boundary after the initial frame. The row index is viewport-relative, and
// updates carry rows in strictly increasing order.
struct TerminalRowUpdate {
    int row = 0;
    QVector<TerminalCell> cells;
    TerminalRowPresentation presentation;
};

// Value-only delta produced from libghostty's render state. A full update is
// the fallback used for the first frame and whenever the viewport shape or
// global render state changes. Partial updates contain only dirty rows and
// independently identify non-cell visual changes.
struct TerminalUpdate {
    int columns = 0;
    int rows = 0;
    bool fullFrame = false;
    QVector<TerminalRowUpdate> dirtyRows;

    bool colorsChanged = false;
    QColor foreground = QColor(QStringLiteral("#d8dee9"));
    QColor background = QColor(QStringLiteral("#1e222a"));
    QColor cursorColor = QColor(QStringLiteral("#d8dee9"));
    QVector<QColor> palette;
    bool cursorColorExplicit = false;

    bool cursorChanged = false;
    bool cursorVisible = false;
    bool cursorBlinking = false;
    int cursorColumn = 0;
    int cursorRow = 0;
    int cursorStyle = 1;
    int cursorColumnSpan = 1;

    bool scrollbarChanged = false;
    quint64 scrollTotal = 0;
    quint64 scrollOffset = 0;
    quint64 scrollLength = 0;

    bool kittyGraphicsChanged = false;
    std::shared_ptr<const TerminalKittyGraphicsSnapshot> kittyGraphics;

    quint64 contentRevision = 0;

    // SessionWorker sets this only for PTY output activity. It is transport
    // metadata rather than terminal state and therefore is not retained in
    // TerminalFrame.
    bool resetCursorBlink = false;

    bool hasChanges() const
    {
        return fullFrame || !dirtyRows.isEmpty() || colorsChanged
            || cursorChanged || scrollbarChanged || kittyGraphicsChanged
            || resetCursorBlink;
    }
};

// Terminal-originated clipboard operations are normalized by libghostty
// before crossing this boundary. Keep every representation owned and
// binary-safe because the source callback lends its storage only for the
// callback duration.
enum class TerminalClipboardLocation : quint8 {
    Standard,
    Selection,
    Primary,
};

struct TerminalClipboardMimeRepresentation {
    QByteArray mime;
    QByteArray data;

    friend bool
    operator==(const TerminalClipboardMimeRepresentation &,
               const TerminalClipboardMimeRepresentation &) = default;
};

struct TerminalClipboardWrite {
    TerminalClipboardLocation location = TerminalClipboardLocation::Standard;
    // An empty collection means clear the destination. A representation with
    // empty data is a distinct, explicit empty value.
    QVector<TerminalClipboardMimeRepresentation> contents;

    friend bool operator==(const TerminalClipboardWrite &,
                           const TerminalClipboardWrite &) = default;
};

enum class TerminalClipboardWriteResult : quint8 {
    Success,
    Denied,
    Unsupported,
    Busy,
    InvalidData,
    IoError,
};

struct TerminalClipboardWriteRequest {
    quint64 requestId = 0;
    TerminalClipboardWrite write;
    QByteArray name;
    bool granted = false;
    bool canRemember = false;
    // This snapshots the live access policy at the point the escape sequence
    // was consumed. A Kitty password grant always bypasses confirmation.
    bool confirmationRequired = false;

    friend bool operator==(const TerminalClipboardWriteRequest &,
                           const TerminalClipboardWriteRequest &) = default;
};

struct TerminalClipboardWriteReply {
    TerminalClipboardWriteResult result = TerminalClipboardWriteResult::Denied;
    bool remember = false;

    friend bool operator==(const TerminalClipboardWriteReply &,
                           const TerminalClipboardWriteReply &) = default;
};

enum class TerminalClipboardReadResult : quint8 {
    Success,
    Denied,
    Unsupported,
    Busy,
    IoError,
};

struct TerminalClipboardReadRequest {
    quint64 requestId = 0;
    TerminalClipboardLocation location = TerminalClipboardLocation::Standard;
    QVector<QByteArray> mimes;
    bool list = false;
    QByteArray name;
    bool granted = false;
    bool canRemember = false;
    // This snapshots the live access policy when the escape sequence is
    // consumed. A Kitty paste-event grant always bypasses confirmation.
    bool confirmationRequired = false;

    friend bool operator==(const TerminalClipboardReadRequest &,
                           const TerminalClipboardReadRequest &) = default;
};

struct TerminalClipboardReadReply {
    TerminalClipboardReadResult result = TerminalClipboardReadResult::Denied;
    QVector<TerminalClipboardMimeRepresentation> contents;
    QVector<QByteArray> available;
    bool remember = false;

    friend bool operator==(const TerminalClipboardReadReply &,
                           const TerminalClipboardReadReply &) = default;
};

// Validates the value-only shape shared by every retained view of a render
// update. Keeping one representability ceiling prevents the GUI frame and
// worker-side indexes from accepting different viewport dimensions.
[[nodiscard]] inline bool validTerminalUpdateShape(const TerminalUpdate &update)
{
    if (update.columns <= 0 || update.rows <= 0) {
        return false;
    }

    const qsizetype columnCount = update.columns;
    const qsizetype rowCount = update.rows;
    if (columnCount > QVector<TerminalCell>::maxSize() / rowCount) {
        return false;
    }
    int previousRow = -1;
    for (const TerminalRowUpdate &row : update.dirtyRows) {
        if (row.row <= previousRow || row.row >= update.rows
            || row.cells.size() != update.columns) {
            return false;
        }
        previousRow = row.row;
    }
    if (update.fullFrame) {
        if (update.dirtyRows.size() != update.rows) {
            return false;
        }
    }
    return true;
}

// Applies an update without exposing terminal handles to the UI. Validation
// happens before mutation so a malformed or incomplete delta cannot leave the
// retained frame half-updated. Returns false for an invalid update or a
// partial update whose dimensions do not match the retained frame.
[[nodiscard]] inline bool
applyTerminalUpdate(TerminalFrame &frame, const TerminalUpdate &update,
                    TerminalFrameApplyMetrics *metrics = nullptr)
{
    if (metrics != nullptr) *metrics = {};
    if (!validTerminalUpdateShape(update)) {
        return false;
    }
    const qsizetype cellCount =
        static_cast<qsizetype>(update.columns) * update.rows;
    if (!update.fullFrame
        && (frame.columns != update.columns || frame.rows != update.rows
            || frame.cells.size() != cellCount
            || frame.cells.columnCount() != update.columns
            || frame.cells.rowCount() != update.rows)) {
        return false;
    }

    if (metrics != nullptr) {
        metrics->rowPayloadsInstalled =
            static_cast<quint64>(update.dirtyRows.size());
        metrics->rowPayloadsReused = update.fullFrame
            ? 0
            : static_cast<quint64>(update.rows - update.dirtyRows.size());
        if (update.fullFrame) {
            metrics->rowTableAllocations = 1;
        } else if (!update.dirtyRows.isEmpty()
                   && !frame.cells.rowTableIsDetached()) {
            metrics->rowTableAllocations = 1;
            metrics->rowTableDetaches = 1;
            metrics->rowHeadersCopied = static_cast<quint64>(update.rows);
        }
    }

    if (update.fullFrame) {
        frame.columns = update.columns;
        frame.rows = update.rows;
        TerminalFrameCellStorageAccess::resetGrid(frame.cells, update.columns,
                                                  update.rows);
        frame.rowPresentation.resize(update.rows);
    } else if (frame.rowPresentation.size() != update.rows) {
        frame.rowPresentation.resize(update.rows);
    }
    for (const TerminalRowUpdate &row : update.dirtyRows) {
        TerminalFrameCellStorageAccess::replaceRow(frame.cells, row.row,
                                                   row.cells);
        frame.rowPresentation[row.row] = row.presentation;
    }

    if (update.fullFrame || update.colorsChanged) {
        frame.foreground = update.foreground;
        frame.background = update.background;
        frame.cursorColor = update.cursorColor;
        frame.palette = update.palette;
        frame.cursorColorExplicit = update.cursorColorExplicit;
    }
    if (update.fullFrame || update.cursorChanged) {
        frame.cursorVisible = update.cursorVisible;
        frame.cursorBlinking = update.cursorBlinking;
        frame.cursorColumn = update.cursorColumn;
        frame.cursorRow = update.cursorRow;
        frame.cursorStyle = update.cursorStyle;
        frame.cursorColumnSpan = update.cursorColumnSpan;
    }
    if (update.fullFrame || update.scrollbarChanged) {
        frame.scrollTotal = update.scrollTotal;
        frame.scrollOffset = update.scrollOffset;
        frame.scrollLength = update.scrollLength;
    }
    if (update.fullFrame || update.kittyGraphicsChanged) {
        frame.kittyGraphics = update.kittyGraphics;
    }
    frame.contentRevision = update.contentRevision;
    return true;
}

struct TerminalKeyInput {
    int key = 0;
    int modifiers = 0;
    // Modifiers used by the active keyboard layout to produce `text`.
    // libghostty removes these when deciding whether printable input is
    // modified, while retaining the complete physical modifier state for
    // Kitty report-all mode.
    int consumedModifiers = 0;
    QString text;
    // Qt Wayland/X11 expose XKB keycodes here (Linux evdev code + 8).
    quint32 nativeScanCode = 0;
    // Effective XKB keysym captured alongside the event. The raw scan code is
    // retained for press/release pairing and writing-system physical keys;
    // libghostty consumers use both to honor functional-key remaps.
    quint32 resolvedKeysym = 0;
    bool pressed = true;
    bool autoRepeat = false;
    bool composing = false;
    // Qt::KeyboardModifiers has no Caps/Num lock bits. The compositor XKB
    // state supplies them separately for Kitty report-all encoding.
    bool capsLock = false;
    bool numLock = false;
    bool consumedCapsLock = false;
    uint32_t unshiftedCodepoint = 0;
    // Zero keeps ordinary input free of diagnostic work. A non-zero pair is
    // allocated only while this pane's inspector is actively capturing; it is
    // semantically inert to libghostty and lets the worker's bounded encoding
    // outcome correlate with the frontend decision that produced this input.
    quint64 inspectorTraceGeneration = 0;
    quint64 inspectorTraceId = 0;
};

enum class TerminalKeyboardTraceDecisionKind : quint8 {
    RootApplicationBinding,
    RootGlobalBinding,
    RootConsumedRelease,
    PaneUnmatched,
    PaneLeader,
    PaneInvalidSequence,
    PaneIgnoredSequence,
    PaneLocalBinding,
    PaneBroadBinding,
    PaneFallbackConsumed,
    PaneFallbackPassed,
    PaneConsumedRelease,
    PaneInspectorCancel,
};

// GUI-thread-only projection of one actual matcher decision. This is emitted
// only while inspector capture is active; the model performs bounded escaping
// after checking its pause state, and no trie traversal is repeated solely for
// diagnostics.
struct TerminalKeyboardTraceDecision {
    TerminalKeyInput input;
    TerminalKeyboardTraceDecisionKind kind =
        TerminalKeyboardTraceDecisionKind::PaneUnmatched;
    quint64 sequenceToken = 0;
    QStringList actions;
    QStringList activeTables;
    QStringList pendingSequence;
    bool consumed = false;
    bool performable = false;
    bool all = false;
    bool global = false;
    bool physical = false;
};

enum class TerminalKeyboardTraceOperation : quint8 {
    Key,
    SequenceStage,
    SequenceResolution,
};

enum class TerminalKeyboardTraceDisposition : quint8 {
    Queued,
    EncoderFailed,
    EncoderEmpty,
    KeyboardActionMode,
    ReadOnly,
    TerminalUnavailable,
    SessionUnavailable,
    ExitWaitConsumed,
    Staged,
    Dropped,
    Superseded,
    StaleSequence,
};

// Worker-to-GUI diagnostic value. `encodedPrefix` is capped at its producer;
// encodedByteCount always describes the complete output so the inspector never
// needs to copy or hex-format an unbounded byte array.
struct TerminalKeyboardTraceResult {
    static constexpr qsizetype MaximumEncodedPrefix = 64;

    quint64 generation = 0;
    quint64 traceId = 0;
    quint64 sequenceToken = 0;
    TerminalKeyboardTraceOperation operation =
        TerminalKeyboardTraceOperation::Key;
    TerminalKeyboardTraceDisposition disposition =
        TerminalKeyboardTraceDisposition::TerminalUnavailable;
    qint64 encodedByteCount = 0;
    QByteArray encodedPrefix;
    bool prefixTruncated = false;

    friend bool operator==(const TerminalKeyboardTraceResult &,
                           const TerminalKeyboardTraceResult &) = default;
};

// One GUI input-method callback becomes one worker operation so committed
// bytes and selection lifecycle cannot interleave with clipboard commands.
struct TerminalInputMethodInput {
    QString commitText;
    bool preeditTransition = false;

    friend bool operator==(const TerminalInputMethodInput &,
                           const TerminalInputMethodInput &) = default;
};

// Resolves terminal input held while the UI decides whether a key sequence
// matches. Leaders are encoded on the session thread when staged so later VT
// mode changes cannot alter the bytes they would originally have produced.
enum class TerminalSequenceResolution : quint8 {
    Drop,
    Flush,
    FlushAndSendCurrent,
};

struct TerminalMouseInput {
    enum Action {
        Press,
        Release,
        Motion,
    };

    Action action = Motion;
    int button = 0;
    int modifiers = 0;
    float x = 0.0F;
    float y = 0.0F;
    bool anyButtonPressed = false;
};

// Whole wheel rows and columns cross the session boundary as one value so the
// worker can choose alternate-scroll, DEC mouse reporting, or viewport
// movement against one current terminal-state snapshot. The frontend policy
// remains explicit because it is pane-owned rather than a terminal mode.
struct TerminalWheelInput {
    qint64 rows = 0;
    qint64 columns = 0;
    int modifiers = 0;
    float x = 0.0F;
    float y = 0.0F;
    bool mouseReportingEnabled = true;

    friend bool operator==(const TerminalWheelInput &,
                           const TerminalWheelInput &) = default;
};

// A local right press is resolved on the session thread because selection
// containment, link matching, and copy-or-paste branching must observe one
// authoritative terminal state. The GUI retains only the popup position.
struct TerminalRightClickInput {
    quint64 requestId = 0;
    quint64 contentRevision = 0;
    int column = 0;
    int row = 0;
    int modifiers = 0;
    // Shift is removed from Ghostty link matching only when it was the
    // physical escape hatch from an otherwise captured DEC mouse gesture.
    bool shiftBypassedMouseCapture = false;

    friend bool operator==(const TerminalRightClickInput &,
                           const TerminalRightClickInput &) = default;
};

enum class TerminalRightClickEffect : quint8 {
    None,
    Paste,
    ContextMenu,
};

struct TerminalRightClickResult {
    quint64 requestId = 0;
    quint64 contentRevision = 0;
    TerminalRightClickEffect effect = TerminalRightClickEffect::None;
    bool selectionAvailable = false;

    friend bool operator==(const TerminalRightClickResult &,
                           const TerminalRightClickResult &) = default;
};

// Qt owns pointer hit testing and timestamp capture, but libghostty owns the
// stateful repeat-click classification. Keep the cross-thread payload
// value-only and explicit about coordinate and timestamp units.
struct TerminalSelectionPressInput {
    int column = 0;
    int row = 0;
    double surfaceX = 0.0;
    double surfaceY = 0.0;
    quint64 timestampNanoseconds = 0;
    bool timestampValid = false;
    // On Linux, Ghostty maps Ctrl triple-clicks to semantic command output
    // rather than the ordinary logical line.
    bool controlModifier = false;
    // A released Shift press may extend the retained gesture after the
    // repeat-click interval. The pane resolves mouse-shift-capture before
    // setting this semantic candidate; the worker owns the timing decision.
    bool extendExistingSelection = false;
    // Linux Ctrl+Alt on the extension press selects a rectangular range
    // immediately, just as the same modifiers on a later drag do.
    bool rectangular = false;

    friend bool operator==(const TerminalSelectionPressInput &,
                           const TerminalSelectionPressInput &) = default;
};

struct TerminalSelectionDragInput {
    int column = 0;
    int row = 0;
    double surfaceX = 0.0;
    double surfaceY = 0.0;
    bool rectangular = false;

    friend bool operator==(const TerminalSelectionDragInput &,
                           const TerminalSelectionDragInput &) = default;
};

Q_DECLARE_METATYPE(TerminalFrame)
Q_DECLARE_METATYPE(TerminalUpdate)
Q_DECLARE_METATYPE(TerminalClipboardLocation)
Q_DECLARE_METATYPE(TerminalClipboardMimeRepresentation)
Q_DECLARE_METATYPE(TerminalClipboardWrite)
Q_DECLARE_METATYPE(TerminalClipboardWriteResult)
Q_DECLARE_METATYPE(TerminalClipboardWriteRequest)
Q_DECLARE_METATYPE(TerminalClipboardWriteReply)
Q_DECLARE_METATYPE(TerminalClipboardReadResult)
Q_DECLARE_METATYPE(TerminalClipboardReadRequest)
Q_DECLARE_METATYPE(TerminalClipboardReadReply)
Q_DECLARE_METATYPE(TerminalHyperlinkState)
Q_DECLARE_METATYPE(TerminalLinkKind)
Q_DECLARE_METATYPE(TerminalSearchDirection)
Q_DECLARE_METATYPE(TerminalSearchCell)
Q_DECLARE_METATYPE(TerminalSearchRange)
Q_DECLARE_METATYPE(TerminalSearchUpdate)
Q_DECLARE_METATYPE(TerminalKeyInput)
Q_DECLARE_METATYPE(TerminalKeyboardTraceDecisionKind)
Q_DECLARE_METATYPE(TerminalKeyboardTraceDecision)
Q_DECLARE_METATYPE(TerminalKeyboardTraceOperation)
Q_DECLARE_METATYPE(TerminalKeyboardTraceDisposition)
Q_DECLARE_METATYPE(TerminalKeyboardTraceResult)
Q_DECLARE_METATYPE(TerminalInputMethodInput)
Q_DECLARE_METATYPE(TerminalSequenceResolution)
Q_DECLARE_METATYPE(TerminalMouseInput)
Q_DECLARE_METATYPE(TerminalWheelInput)
Q_DECLARE_METATYPE(TerminalRightClickInput)
Q_DECLARE_METATYPE(TerminalRightClickEffect)
Q_DECLARE_METATYPE(TerminalRightClickResult)
Q_DECLARE_METATYPE(TerminalSelectionPressInput)
Q_DECLARE_METATYPE(TerminalSelectionDragInput)
