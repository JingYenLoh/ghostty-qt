#include "terminal_glyph_batch.h"

#include <QSGGeometry>
#include <QSGTexture>
#include <QTest>

#include <array>
#include <limits>

namespace {

class StubTexture final : public QSGTexture {
public:
    explicit StubTexture(qint64 key, QSize size = {128, 128},
                         bool hasAlpha = true)
        : key_(key)
        , size_(size)
        , hasAlpha_(hasAlpha)
    {}

    qint64 comparisonKey() const override { return key_; }
    QSize textureSize() const override { return size_; }
    bool hasAlphaChannel() const override { return hasAlpha_; }
    bool hasMipmaps() const override { return false; }

private:
    qint64 key_ = 0;
    QSize size_;
    bool hasAlpha_ = true;
};

struct GlyphVertex {
    float x;
    float y;
    float u;
    float v;
    uchar red;
    uchar green;
    uchar blue;
    uchar alpha;
};

static_assert(sizeof(GlyphVertex) == 20);

[[nodiscard]] TerminalGlyphRect glyphRect(qreal x, qreal y, qreal width,
                                          qreal height)
{
    return TerminalGlyphRect::fromQRectF(QRectF(x, y, width, height));
}

void appendGlyph(QVector<TerminalGlyphQuad> &glyphs, int index)
{
    glyphs.append({
        .destination = glyphRect(index * 8.0, index * 16.0, 8.0, 16.0),
        .normalizedSource = glyphRect(index * 0.01, 0.0, 0.01, 0.02),
        .color = QColor::fromRgb(16 + index, 32 + index, 48 + index, 128),
    });
}

const GlyphVertex *vertices(const TerminalGlyphBatch &batch)
{
    return static_cast<const GlyphVertex *>(batch.geometry()->vertexData());
}

} // namespace

class TerminalGlyphBatchTest final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void retainsGeometryAndSkipsIdenticalUpdates();
    void exposesEmptyAndFallbackWithoutPartialGeometry();
    void rejectsInvalidPackedRectangles();
    void writesIndexedPremultipliedQuads();
    void enforcesUnsignedShortIndexBoundary();
    void changesTextureWithoutRewritingGeometry();
};

void TerminalGlyphBatchTest::retainsGeometryAndSkipsIdenticalUpdates()
{
    StubTexture texture(1);
    TerminalGlyphBatch batch;
    QVector<TerminalGlyphQuad> &first = batch.beginUpdate();
    appendGlyph(first, 0);
    appendGlyph(first, 1);
    QCOMPARE(batch.commit(&texture), TerminalGlyphBatchCommitResult::Batched);

    const TerminalGlyphBatchProbeSnapshot firstProbe = batch.probe();
    QCOMPARE(firstProbe.glyphCount, qsizetype{2});
    QCOMPARE(firstProbe.vertexCount, qsizetype{8});
    QCOMPARE(firstProbe.indexCount, qsizetype{12});
    QVERIFY(firstProbe.capacity >= 2);
    QCOMPARE(firstProbe.allocationGeneration, quint64{1});
    QCOMPARE(firstProbe.commitGeneration, quint64{1});
    QCOMPARE(firstProbe.geometryWriteCount, quint64{1});
    QCOMPARE(firstProbe.materialAssignmentCount, quint64{1});

    QVector<TerminalGlyphQuad> &identical = batch.beginUpdate();
    appendGlyph(identical, 0);
    appendGlyph(identical, 1);
    QCOMPARE(batch.commit(&texture), TerminalGlyphBatchCommitResult::Batched);
    QCOMPARE(batch.commitGeneration(), quint64{1});
    QCOMPARE(batch.geometryWriteCount(), quint64{1});

    const qsizetype retainedCapacity = batch.capacity();
    QVector<TerminalGlyphQuad> &smaller = batch.beginUpdate();
    appendGlyph(smaller, 3);
    QCOMPARE(batch.commit(&texture), TerminalGlyphBatchCommitResult::Batched);
    QCOMPARE(batch.capacity(), retainedCapacity);
    QCOMPARE(batch.allocationGeneration(), quint64{1});
    QCOMPARE(batch.commitGeneration(), quint64{2});
    QCOMPARE(batch.geometryWriteCount(), quint64{2});
}

void TerminalGlyphBatchTest::exposesEmptyAndFallbackWithoutPartialGeometry()
{
    StubTexture texture(1);
    TerminalGlyphBatch batch;

    (void)batch.beginUpdate();
    QCOMPARE(batch.commit(&texture), TerminalGlyphBatchCommitResult::Empty);
    QCOMPARE(batch.commitGeneration(), quint64{1});

    QVector<TerminalGlyphQuad> &missingTexture = batch.beginUpdate();
    appendGlyph(missingTexture, 0);
    QCOMPARE(batch.commit(nullptr), TerminalGlyphBatchCommitResult::Fallback);
    QCOMPARE(batch.size(), qsizetype{1});
    QCOMPARE(batch.geometry()->vertexCount(), 0);
    QCOMPARE(batch.fallbackCommitCount(), quint64{1});
    QCOMPARE(batch.geometryWriteCount(), quint64{0});

    QVector<TerminalGlyphQuad> &valid = batch.beginUpdate();
    appendGlyph(valid, 0);
    QCOMPARE(batch.commit(&texture), TerminalGlyphBatchCommitResult::Batched);
    QCOMPARE(batch.geometry()->vertexCount(), 4);
    QCOMPARE(batch.commitGeneration(), quint64{3});

    QVector<TerminalGlyphQuad> &invalid = batch.beginUpdate();
    appendGlyph(invalid, 0);
    invalid.front().destination.right = std::numeric_limits<float>::quiet_NaN();
    QCOMPARE(batch.commit(&texture), TerminalGlyphBatchCommitResult::Fallback);
    QCOMPARE(batch.geometry()->vertexCount(), 0);
    QCOMPARE(batch.fallbackCommitCount(), quint64{2});

    StubTexture colorOnlyTexture(2, {128, 128}, false);
    QVector<TerminalGlyphQuad> &colorOnly = batch.beginUpdate();
    appendGlyph(colorOnly, 0);
    QCOMPARE(batch.commit(&colorOnlyTexture),
             TerminalGlyphBatchCommitResult::Fallback);
    QCOMPARE(batch.fallbackCommitCount(), quint64{3});

    (void)batch.beginUpdate();
    QCOMPARE(batch.commit(&texture), TerminalGlyphBatchCommitResult::Empty);
    QCOMPARE(batch.size(), qsizetype{0});
    QCOMPARE(batch.geometry()->vertexCount(), 0);
    const quint64 generation = batch.commitGeneration();
    (void)batch.beginUpdate();
    QCOMPARE(batch.commit(nullptr), TerminalGlyphBatchCommitResult::Empty);
    QCOMPARE(batch.commitGeneration(), generation);
}

void TerminalGlyphBatchTest::rejectsInvalidPackedRectangles()
{
    const TerminalGlyphRect validDestination = glyphRect(2.0, 3.0, 4.0, 5.0);
    const TerminalGlyphRect validSource = glyphRect(0.1, 0.2, 0.3, 0.4);
    const float infinity = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::array invalidDestinations{
        glyphRect(2.0, 3.0, 0.0, 5.0),
        glyphRect(2.0, 3.0, 4.0, -1.0),
        TerminalGlyphRect{
            .left = infinity, .top = 3.0F, .right = infinity, .bottom = 8.0F},
        TerminalGlyphRect{
            .left = 2.0F, .top = 3.0F, .right = nan, .bottom = 8.0F},
    };
    const std::array invalidSources{
        glyphRect(0.1, 0.2, 0.0, 0.4),
        glyphRect(-0.01, 0.2, 0.3, 0.4),
        glyphRect(0.8, 0.2, 0.21, 0.4),
        TerminalGlyphRect{
            .left = 0.1F, .top = 0.2F, .right = 0.4F, .bottom = infinity},
    };

    StubTexture texture(1);
    TerminalGlyphBatch batch;
    const auto commit = [&batch, &texture](TerminalGlyphRect destination,
                                           TerminalGlyphRect source) {
        batch.beginUpdate().append({
            .destination = destination,
            .normalizedSource = source,
            .color = Qt::white,
        });
        return batch.commit(&texture);
    };
    for (const TerminalGlyphRect destination : invalidDestinations) {
        QCOMPARE(commit(destination, validSource),
                 TerminalGlyphBatchCommitResult::Fallback);
    }
    for (const TerminalGlyphRect source : invalidSources) {
        QCOMPARE(commit(validDestination, source),
                 TerminalGlyphBatchCommitResult::Fallback);
    }
}

void TerminalGlyphBatchTest::writesIndexedPremultipliedQuads()
{
    StubTexture texture(1);
    TerminalGlyphBatch batch;
    QVector<TerminalGlyphQuad> &glyphs = batch.beginUpdate();
    glyphs.append({
        .destination = glyphRect(2.25, 3.5, 4.125, 5.75),
        .normalizedSource = glyphRect(0.125, 0.25, 0.375, 0.5),
        .color = QColor::fromRgb(128, 64, 32, 128),
    });
    QCOMPARE(batch.commit(&texture), TerminalGlyphBatchCommitResult::Batched);

    const GlyphVertex *vertex = vertices(batch);
    QVERIFY(vertex != nullptr);
    QCOMPARE(vertex[0].x, 2.25F);
    QCOMPARE(vertex[0].y, 3.5F);
    QCOMPARE(vertex[0].u, 0.125F);
    QCOMPARE(vertex[0].v, 0.25F);
    QCOMPARE(vertex[3].x, 6.375F);
    QCOMPARE(vertex[3].y, 9.25F);
    QCOMPARE(vertex[3].u, 0.5F);
    QCOMPARE(vertex[3].v, 0.75F);
    QCOMPARE(vertex[0].red, uchar{64});
    QCOMPARE(vertex[0].green, uchar{32});
    QCOMPARE(vertex[0].blue, uchar{16});
    QCOMPARE(vertex[0].alpha, uchar{128});

    QCOMPARE(batch.geometry()->indexType(),
             static_cast<int>(QSGGeometry::UnsignedShortType));
    const quint16 *const indices = batch.geometry()->indexDataAsUShort();
    QCOMPARE(indices[0], quint16{0});
    QCOMPARE(indices[1], quint16{1});
    QCOMPARE(indices[2], quint16{2});
    QCOMPARE(indices[3], quint16{2});
    QCOMPARE(indices[4], quint16{1});
    QCOMPARE(indices[5], quint16{3});

    QVector<TerminalGlyphQuad> &linear = batch.beginUpdate();
    linear.append({
        .destination = glyphRect(2.25, 3.5, 4.125, 5.75),
        .normalizedSource = glyphRect(0.125, 0.25, 0.375, 0.5),
        .color = QColor::fromRgb(128, 64, 32, 128),
    });
    QCOMPARE(batch.commit(&texture, TerminalAlphaBlending::Linear),
             TerminalGlyphBatchCommitResult::Batched);
    vertex = vertices(batch);
    QCOMPARE(vertex[0].red, uchar{28});
    QCOMPARE(vertex[0].green, uchar{7});
    QCOMPARE(vertex[0].blue, uchar{2});
    QCOMPARE(vertex[0].alpha, uchar{128});
}

void TerminalGlyphBatchTest::enforcesUnsignedShortIndexBoundary()
{
    constexpr qsizetype verticesPerGlyph = 4;
    constexpr qsizetype indicesPerGlyph = 6;
    constexpr qsizetype maximumGlyphCount =
        (static_cast<qsizetype>(std::numeric_limits<quint16>::max()) + 1)
        / verticesPerGlyph;
    static_assert(maximumGlyphCount == 16'384);

    const TerminalGlyphQuad glyph{
        .destination = glyphRect(2.0, 3.0, 4.0, 5.0),
        .normalizedSource = glyphRect(0.1, 0.2, 0.3, 0.4),
        .color = QColor::fromRgb(128, 64, 32, 128),
    };
    StubTexture texture(1);
    TerminalGlyphBatch batch;

    QVector<TerminalGlyphQuad> &atLimit = batch.beginUpdate();
    atLimit.fill(glyph, maximumGlyphCount);
    QCOMPARE(batch.commit(&texture), TerminalGlyphBatchCommitResult::Batched);
    QCOMPARE(batch.capacity(), maximumGlyphCount);
    QCOMPARE(batch.geometry()->vertexCount(),
             static_cast<int>(maximumGlyphCount * verticesPerGlyph));
    QCOMPARE(batch.geometry()->indexCount(),
             static_cast<int>(maximumGlyphCount * indicesPerGlyph));

    const quint16 *const indices = batch.geometry()->indexDataAsUShort();
    QVERIFY(indices != nullptr);
    const qsizetype last = (maximumGlyphCount - 1) * indicesPerGlyph;
    QCOMPARE(indices[last], quint16{65'532});
    QCOMPARE(indices[last + 1], quint16{65'533});
    QCOMPARE(indices[last + 2], quint16{65'534});
    QCOMPARE(indices[last + 3], quint16{65'534});
    QCOMPARE(indices[last + 4], quint16{65'533});
    QCOMPARE(indices[last + 5], std::numeric_limits<quint16>::max());

    QVector<TerminalGlyphQuad> &overLimit = batch.beginUpdate();
    overLimit.fill(glyph, maximumGlyphCount + 1);
    QCOMPARE(batch.commit(&texture), TerminalGlyphBatchCommitResult::Fallback);
    QCOMPARE(batch.geometry()->vertexCount(), 0);
    QCOMPARE(batch.geometry()->indexCount(), 0);
    QCOMPARE(batch.capacity(), maximumGlyphCount);
}

void TerminalGlyphBatchTest::changesTextureWithoutRewritingGeometry()
{
    StubTexture firstTexture(1);
    StubTexture secondTexture(2);
    TerminalGlyphBatch batch;
    QVector<TerminalGlyphQuad> &first = batch.beginUpdate();
    appendGlyph(first, 0);
    QCOMPARE(batch.commit(&firstTexture),
             TerminalGlyphBatchCommitResult::Batched);

    QVector<TerminalGlyphQuad> &same = batch.beginUpdate();
    appendGlyph(same, 0);
    QCOMPARE(batch.commit(&secondTexture),
             TerminalGlyphBatchCommitResult::Batched);
    QCOMPARE(batch.geometryWriteCount(), quint64{1});
    QCOMPARE(batch.materialAssignmentCount(), quint64{2});
    QCOMPARE(batch.commitGeneration(), quint64{2});
}

QTEST_GUILESS_MAIN(TerminalGlyphBatchTest)

#include "test_terminal_glyph_batch.moc"
