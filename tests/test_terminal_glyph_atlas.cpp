#include "terminal_glyph_atlas.h"

#include <QFont>
#include <QFontDatabase>
#include <QRawFont>
#include <QTest>

#include <cmath>
#include <limits>

namespace {

quint32 glyphIndex(const QRawFont &font, QChar character)
{
    const QList<quint32> indexes =
        font.glyphIndexesForString(QString(character));
    return indexes.value(0, std::numeric_limits<quint32>::max());
}

bool paddingIsTransparent(const QImage &image,
                          const TerminalGlyphAtlasEntry &entry)
{
    for (int y = entry.paddedPixelRect.top();
         y <= entry.paddedPixelRect.bottom(); ++y) {
        const uchar *const row = image.constScanLine(y);
        for (int x = entry.paddedPixelRect.left();
             x <= entry.paddedPixelRect.right(); ++x) {
            if (!entry.pixelRect.contains(x, y) && row[x] != 0) return false;
        }
    }
    return true;
}

} // namespace

class TerminalGlyphAtlasTest final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void buildsDeduplicatedPaddedEntries();
    void cachesBlankGlyphsWithoutShelfSpace();
    void producesDeterministicPacking();
    void preservesLogicalScaleAtFractionalDpr();
    void failsSoftlyForInvalidInputsAndCapacity();

private:
    [[nodiscard]] QRawFont rawFont(int pixelSize = 24) const;

    int fontId_ = -1;
    QString family_;
};

void TerminalGlyphAtlasTest::initTestCase()
{
    const QString path =
        QFINDTESTDATA("../ghostty/src/font/res/Inconsolata-Regular.ttf");
    QVERIFY2(!path.isEmpty(), "Bundled glyph-atlas test font was not found");
    fontId_ = QFontDatabase::addApplicationFont(path);
    QVERIFY(fontId_ >= 0);
    family_ = QFontDatabase::applicationFontFamilies(fontId_).value(0);
    QVERIFY(!family_.isEmpty());
}

void TerminalGlyphAtlasTest::cleanupTestCase()
{
    QVERIFY(QFontDatabase::removeApplicationFont(fontId_));
}

QRawFont TerminalGlyphAtlasTest::rawFont(int pixelSize) const
{
    QFont font(family_);
    font.setPixelSize(pixelSize);
    font.setHintingPreference(QFont::PreferFullHinting);
    return QRawFont::fromFont(font);
}

void TerminalGlyphAtlasTest::buildsDeduplicatedPaddedEntries()
{
    const QRawFont font = rawFont();
    QVERIFY(font.isValid());
    const quint32 a = glyphIndex(font, u'A');
    const quint32 b = glyphIndex(font, u'B');
    const QVector<TerminalGlyphAtlasKey> requests{
        {.font = font, .glyphIndex = a},
        {.font = font, .glyphIndex = b},
        {.font = font, .glyphIndex = a},
    };

    const std::optional<TerminalGlyphAtlas> atlas =
        TerminalGlyphAtlas::build(requests,
                                  {.devicePixelRatio = 1.0,
                                   .paddingPixels = 2,
                                   .maxTextureDimension = 128});
    QVERIFY(atlas.has_value());
    QCOMPARE(atlas->entryCount(), qsizetype{2});
    QCOMPARE(atlas->image().format(), QImage::Format_Alpha8);
    QCOMPARE(atlas->pixelSize(), atlas->image().size());
    QCOMPARE(atlas->byteSize(), atlas->image().sizeInBytes());
    QCOMPARE(atlas->textureByteSize(),
             static_cast<qsizetype>(atlas->pixelSize().width())
                 * atlas->pixelSize().height() * 4);
    QCOMPARE(atlas->paddingPixels(), 2);

    const QImage texture = atlas->textureImage();
    QCOMPARE(texture.format(), QImage::Format_RGBA8888_Premultiplied);
    QCOMPARE(texture.size(), atlas->pixelSize());
    for (int y = 0; y < texture.height(); ++y) {
        const uchar *const coverage = atlas->image().constScanLine(y);
        const uchar *const rgba = texture.constScanLine(y);
        for (int x = 0; x < texture.width(); ++x) {
            QCOMPARE(rgba[x * 4], coverage[x]);
            QCOMPARE(rgba[x * 4 + 1], coverage[x]);
            QCOMPARE(rgba[x * 4 + 2], coverage[x]);
            QCOMPARE(rgba[x * 4 + 3], coverage[x]);
        }
    }

    for (const quint32 index : {a, b}) {
        const TerminalGlyphAtlasEntry *const entry = atlas->lookup(font, index);
        QVERIFY(entry != nullptr);
        QVERIFY(!entry->blank);
        QVERIFY(!entry->pixelRect.isEmpty());
        QVERIFY(entry->paddedPixelRect.contains(entry->pixelRect));
        QCOMPARE(entry->paddedPixelRect.width(), entry->pixelRect.width() + 4);
        QCOMPARE(entry->paddedPixelRect.height(),
                 entry->pixelRect.height() + 4);
        QVERIFY(paddingIsTransparent(atlas->image(), *entry));
        QCOMPARE(entry->logicalSize,
                 QSizeF(entry->pixelRect.width(), entry->pixelRect.height()));
        QCOMPARE(entry->logicalDestination(QPointF(10.0, 20.0)).topLeft(),
                 QPointF(10.0, 20.0) + entry->logicalBearing);
        QCOMPARE(entry->normalizedTextureRect,
                 QRectF(static_cast<qreal>(entry->pixelRect.x())
                            / atlas->pixelSize().width(),
                        static_cast<qreal>(entry->pixelRect.y())
                            / atlas->pixelSize().height(),
                        static_cast<qreal>(entry->pixelRect.width())
                            / atlas->pixelSize().width(),
                        static_cast<qreal>(entry->pixelRect.height())
                            / atlas->pixelSize().height()));
    }
}

void TerminalGlyphAtlasTest::cachesBlankGlyphsWithoutShelfSpace()
{
    const QRawFont font = rawFont();
    const quint32 space = glyphIndex(font, u' ');
    const QVector<TerminalGlyphAtlasKey> requests{
        {.font = font, .glyphIndex = space},
        {.font = font, .glyphIndex = space},
    };

    const std::optional<TerminalGlyphAtlas> atlas =
        TerminalGlyphAtlas::build(requests,
                                  {.devicePixelRatio = 2.0,
                                   .paddingPixels = 0,
                                   .maxTextureDimension = 1});
    QVERIFY(atlas.has_value());
    QCOMPARE(atlas->entryCount(), qsizetype{1});
    QCOMPARE(atlas->pixelSize(), QSize(1, 1));
    QCOMPARE(atlas->image().constScanLine(0)[0], uchar{0});
    const TerminalGlyphAtlasEntry *const entry = atlas->lookup(font, space);
    QVERIFY(entry != nullptr);
    QVERIFY(entry->blank);
    QVERIFY(entry->pixelRect.isEmpty());
    QVERIFY(entry->paddedPixelRect.isEmpty());
    QVERIFY(entry->normalizedTextureRect.isEmpty());
    QVERIFY(entry->logicalDestination({}).isEmpty());
}

void TerminalGlyphAtlasTest::producesDeterministicPacking()
{
    const QRawFont font = rawFont(18);
    QVector<TerminalGlyphAtlasKey> requests;
    for (const QChar character : QStringLiteral("Terminal atlas 0123456789")) {
        requests.append(
            {.font = font, .glyphIndex = glyphIndex(font, character)});
    }
    const TerminalGlyphAtlasOptions options{
        .devicePixelRatio = 1.25,
        .paddingPixels = 1,
        .maxTextureDimension = 256,
    };

    const std::optional<TerminalGlyphAtlas> first =
        TerminalGlyphAtlas::build(requests, options);
    const std::optional<TerminalGlyphAtlas> second =
        TerminalGlyphAtlas::build(requests, options);
    QVERIFY(first.has_value());
    QVERIFY(second.has_value());
    QCOMPARE(first->pixelSize(), second->pixelSize());
    QCOMPARE(first->image(), second->image());
    for (const TerminalGlyphAtlasKey &request : requests) {
        const TerminalGlyphAtlasEntry *const firstEntry =
            first->lookup(request);
        const TerminalGlyphAtlasEntry *const secondEntry =
            second->lookup(request);
        QVERIFY(firstEntry != nullptr);
        QVERIFY(secondEntry != nullptr);
        QCOMPARE(firstEntry->paddedPixelRect, secondEntry->paddedPixelRect);
        QCOMPARE(firstEntry->pixelRect, secondEntry->pixelRect);
        QCOMPARE(firstEntry->normalizedTextureRect,
                 secondEntry->normalizedTextureRect);
    }
}

void TerminalGlyphAtlasTest::preservesLogicalScaleAtFractionalDpr()
{
    const QRawFont font = rawFont(30);
    const TerminalGlyphAtlasKey request{
        .font = font,
        .glyphIndex = glyphIndex(font, u'W'),
    };
    const std::optional<TerminalGlyphAtlas> native =
        TerminalGlyphAtlas::build(std::span(&request, 1));
    const std::optional<TerminalGlyphAtlas> scaled = TerminalGlyphAtlas::build(
        std::span(&request, 1), {.devicePixelRatio = 1.5});
    QVERIFY(native.has_value());
    QVERIFY(scaled.has_value());
    QCOMPARE(scaled->devicePixelRatio(), 1.5);
    const TerminalGlyphAtlasEntry *const nativeEntry = native->lookup(request);
    const TerminalGlyphAtlasEntry *const scaledEntry = scaled->lookup(request);
    QVERIFY(nativeEntry != nullptr);
    QVERIFY(scaledEntry != nullptr);
    QVERIFY(scaledEntry->pixelRect.width() >= nativeEntry->pixelRect.width());
    QVERIFY(scaledEntry->pixelRect.height() >= nativeEntry->pixelRect.height());
    QVERIFY(std::abs(scaledEntry->logicalSize.width()
                     - nativeEntry->logicalSize.width())
            <= 1.0);
    QVERIFY(std::abs(scaledEntry->logicalSize.height()
                     - nativeEntry->logicalSize.height())
            <= 1.0);
}

void TerminalGlyphAtlasTest::failsSoftlyForInvalidInputsAndCapacity()
{
    const QRawFont font = rawFont();
    const TerminalGlyphAtlasKey valid{
        .font = font,
        .glyphIndex = glyphIndex(font, u'M'),
    };
    const QVector<TerminalGlyphAtlasKey> requests{valid};

    QVERIFY(!TerminalGlyphAtlas::build(requests, {.devicePixelRatio = 0.0}));
    QVERIFY(!TerminalGlyphAtlas::build(
        requests,
        {.devicePixelRatio = std::numeric_limits<qreal>::infinity()}));
    QVERIFY(!TerminalGlyphAtlas::build(
        requests, {.devicePixelRatio = 1.0, .paddingPixels = -1}));
    QVERIFY(!TerminalGlyphAtlas::build(requests,
                                       {.devicePixelRatio = 1.0,
                                        .paddingPixels = 1,
                                        .maxTextureDimension = 1}));

    const QVector<TerminalGlyphAtlasKey> invalidFont{
        {.font = QRawFont{}, .glyphIndex = 0},
    };
    QVERIFY(!TerminalGlyphAtlas::build(invalidFont));

    QVERIFY(
        TerminalGlyphAtlas::build(std::span<const TerminalGlyphAtlasKey>{}));
}

QTEST_MAIN(TerminalGlyphAtlasTest)

#include "test_terminal_glyph_atlas.moc"
