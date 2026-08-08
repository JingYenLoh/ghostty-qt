#pragma once

#include "terminal_alpha_blending.h"

#include <QColor>
#include <QRectF>
#include <QSGGeometryNode>
#include <QVector>
#include <QtGlobal>

class QSGGeometry;
class QSGMaterial;
class QSGTexture;

// Store the four edges that are uploaded to the GPU, rather than QRectF's
// qreal origin and size. On 64-bit builds this halves each retained rectangle
// while preserving the exact float values written to vertex geometry.
struct TerminalGlyphRect {
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;

    [[nodiscard]] static TerminalGlyphRect
    fromQRectF(const QRectF &rect) noexcept
    {
        return {
            .left = static_cast<float>(rect.left()),
            .top = static_cast<float>(rect.top()),
            .right = static_cast<float>(rect.right()),
            .bottom = static_cast<float>(rect.bottom()),
        };
    }

    bool operator==(const TerminalGlyphRect &) const = default;
};

static_assert(sizeof(TerminalGlyphRect) == sizeof(float) * 4);

// One already-rasterized monochrome glyph. The source rectangle is expressed
// in normalized atlas coordinates; its sampled alpha modulates the
// premultiplied vertex color.
struct TerminalGlyphQuad {
    TerminalGlyphRect destination;
    TerminalGlyphRect normalizedSource;
    QColor color;

    bool operator==(const TerminalGlyphQuad &) const = default;
};

#if defined(Q_PROCESSOR_X86_64)
static_assert(sizeof(TerminalGlyphQuad) == 48);
#endif

enum class TerminalGlyphBatchCommitResult : quint8 {
    Batched,
    Empty,
    Fallback,
};

struct TerminalGlyphBatchProbeSnapshot {
    TerminalGlyphBatchCommitResult result =
        TerminalGlyphBatchCommitResult::Empty;
    qsizetype glyphCount = 0;
    qsizetype vertexCount = 0;
    qsizetype indexCount = 0;
    qsizetype capacity = 0;
    quint64 allocationGeneration = 0;
    quint64 commitGeneration = 0;
    quint64 geometryWriteCount = 0;
    quint64 materialAssignmentCount = 0;
    quint64 fallbackCommitCount = 0;
};

// A render-thread-only retained geometry batch for monochrome glyph atlases.
// Call beginUpdate(), append the complete next batch, and commit() with the
// renderer-owned atlas texture. The texture pointer is non-owning and must
// remain alive until it is replaced or the batch is committed empty/fallback.
//
// Invalid quads and a missing atlas fail the whole update so the caller can
// render it through the general Qt text path without producing a partial row.
// Per-row geometry uses portable 16-bit indices and falls back when a row
// would require a vertex index larger than 65535.
class TerminalGlyphBatch final : public QSGGeometryNode {
public:
    TerminalGlyphBatch();
    ~TerminalGlyphBatch() override;

    TerminalGlyphBatch(const TerminalGlyphBatch &) = delete;
    TerminalGlyphBatch &operator=(const TerminalGlyphBatch &) = delete;

    [[nodiscard]] QVector<TerminalGlyphQuad> &beginUpdate() noexcept;
    [[nodiscard]] TerminalGlyphBatchCommitResult
    commit(QSGTexture *atlasTexture,
           TerminalAlphaBlending alphaBlending = TerminalAlphaBlending::Native);

    [[nodiscard]] TerminalGlyphBatchCommitResult result() const noexcept;
    [[nodiscard]] qsizetype size() const noexcept;
    [[nodiscard]] qsizetype capacity() const noexcept;
    [[nodiscard]] quint64 allocationGeneration() const noexcept;
    [[nodiscard]] quint64 commitGeneration() const noexcept;
    [[nodiscard]] quint64 geometryWriteCount() const noexcept;
    [[nodiscard]] quint64 materialAssignmentCount() const noexcept;
    [[nodiscard]] quint64 fallbackCommitCount() const noexcept;
    [[nodiscard]] TerminalGlyphBatchProbeSnapshot probe() const noexcept;

private:
    [[nodiscard]] TerminalGlyphBatchCommitResult
    requestedResult(QSGTexture *atlasTexture) const noexcept;
    void allocateFor(qsizetype glyphCount);
    void writeGeometry();
    void hideGeometry();

    QSGGeometry *geometry_ = nullptr;
    QSGMaterial *material_ = nullptr;
    QVector<TerminalGlyphQuad> pending_;
    QVector<TerminalGlyphQuad> committed_;
    QSGTexture *committedTexture_ = nullptr;
    qsizetype capacity_ = 0;
    quint64 allocationGeneration_ = 0;
    quint64 commitGeneration_ = 0;
    quint64 geometryWriteCount_ = 0;
    quint64 materialAssignmentCount_ = 0;
    quint64 fallbackCommitCount_ = 0;
    bool hasCommit_ = false;
    TerminalAlphaBlending committedAlphaBlending_ =
        TerminalAlphaBlending::Native;
    TerminalGlyphBatchCommitResult result_ =
        TerminalGlyphBatchCommitResult::Empty;
};
