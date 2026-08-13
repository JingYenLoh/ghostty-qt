#include "terminal_cell_metrics.h"

#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QFontVariableAxis>
#include <QRawFont>
#include <QTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>

namespace {

constexpr auto kRoles = std::to_array({
    TerminalFontRole::Regular,
    TerminalFontRole::Bold,
    TerminalFontRole::Italic,
    TerminalFontRole::BoldItalic,
});

constexpr auto kMetrics = std::to_array({
    TerminalMetric::CellWidth,
    TerminalMetric::CellHeight,
    TerminalMetric::FontBaseline,
    TerminalMetric::UnderlinePosition,
    TerminalMetric::UnderlineThickness,
    TerminalMetric::StrikethroughPosition,
    TerminalMetric::StrikethroughThickness,
    TerminalMetric::OverlinePosition,
    TerminalMetric::OverlineThickness,
    TerminalMetric::CursorThickness,
    TerminalMetric::CursorHeight,
});

TerminalTypography typographyFor(const QStringList &families = {})
{
    TerminalTypography result;
    result.face(TerminalFontRole::Regular).families = families;
    return result;
}

qint64 physical(qreal logical, qreal dpr)
{
    return static_cast<qint64>(std::llround(logical * dpr));
}

std::optional<double> physicalTableThickness(const QRawFont &font,
                                             const char *tableName,
                                             qsizetype offset, qreal dpr)
{
    const QByteArray table = font.fontTable(tableName);
    if (!font.isValid() || offset < 0 || table.size() < 2
        || offset > table.size() - 2 || font.unitsPerEm() <= 0.0
        || font.pixelSize() <= 0.0) {
        return std::nullopt;
    }
    const auto high = static_cast<quint8>(table[offset]);
    const auto low = static_cast<quint8>(table[offset + 1]);
    const quint16 designUnits =
        static_cast<quint16>((static_cast<quint16>(high) << 8U) | low);
    if (designUnits == 0
        || designUnits
            > static_cast<quint16>(std::numeric_limits<qint16>::max())) {
        return std::nullopt;
    }
    return static_cast<double>(designUnits)
        * static_cast<double>(font.pixelSize()) * dpr
        / static_cast<double>(font.unitsPerEm());
}

qint64 expectedThickness(const QFont &font, const QFontMetricsF &metrics,
                         const char *tableName, qsizetype offset, qreal dpr,
                         std::optional<double> fallback = std::nullopt)
{
    const double value =
        physicalTableThickness(QRawFont::fromFont(font), tableName, offset, dpr)
            .value_or(fallback.value_or(
                static_cast<double>(metrics.lineWidth() * dpr)));
    return std::max<qint64>(1, static_cast<qint64>(std::ceil(value)));
}

qreal metricValue(const TerminalCellMetrics &metrics, TerminalMetric key)
{
    switch (key) {
    case TerminalMetric::CellWidth: return metrics.cellWidth;
    case TerminalMetric::CellHeight: return metrics.cellHeight;
    case TerminalMetric::FontBaseline: return metrics.baseline;
    case TerminalMetric::UnderlinePosition: return metrics.underlinePosition;
    case TerminalMetric::UnderlineThickness: return metrics.underlineThickness;
    case TerminalMetric::StrikethroughPosition:
        return metrics.strikethroughPosition;
    case TerminalMetric::StrikethroughThickness:
        return metrics.strikethroughThickness;
    case TerminalMetric::OverlinePosition: return metrics.overlinePosition;
    case TerminalMetric::OverlineThickness: return metrics.overlineThickness;
    case TerminalMetric::CursorThickness: return metrics.cursorThickness;
    case TerminalMetric::CursorHeight: return metrics.cursorHeight;
    case TerminalMetric::Count: return 0.0;
    }
    Q_UNREACHABLE_RETURN(0.0);
}

QStringList fixedPitchFamilies()
{
    QStringList result;
    for (const QString &family : QFontDatabase::families()) {
        if (QFontDatabase::isFixedPitch(family)) {
            result.append(family);
        }
    }
    return result;
}

struct NamedFace {
    QString family;
    QString style;
};

std::optional<NamedFace> namedFace()
{
    for (const QString &family : fixedPitchFamilies()) {
        for (const QString &style : QFontDatabase::styles(family)) {
            if (!QFontDatabase::bold(family, style)
                && !QFontDatabase::italic(family, style)) {
                continue;
            }
            QFont probe({family}, 12);
            probe.setFixedPitch(true);
            probe.setStyleHint(
                QFont::Monospace,
                static_cast<QFont::StyleStrategy>(QFont::PreferDefault
                                                  | QFont::ContextFontMerging));
            probe.setStyleName(style);
            const QFontInfo info(probe);
            if (info.family().compare(family, Qt::CaseInsensitive) == 0
                && info.styleName() == style) {
                return NamedFace{family, style};
            }
        }
    }
    return std::nullopt;
}

void verifyPhysicalGeometry(const TerminalCellMetrics &metrics, qreal dpr)
{
    for (const qreal value :
         {metrics.cellWidth, metrics.cellHeight, metrics.baseline,
          metrics.underlinePosition, metrics.underlineThickness,
          metrics.strikethroughPosition, metrics.strikethroughThickness,
          metrics.overlinePosition, metrics.overlineThickness,
          metrics.cursorThickness, metrics.cursorHeight, metrics.cursorTop,
          metrics.cursorBarLeft, metrics.underlineMaximumPosition,
          metrics.overlineMinimumPosition}) {
        QVERIFY(std::isfinite(value));
        const qreal pixels = value * dpr;
        QCOMPARE(pixels, std::trunc(pixels));
    }
    QVERIFY(metrics.cellWidth > 0.0);
    QVERIFY(metrics.cellHeight > 0.0);
    QVERIFY(metrics.underlineThickness > 0.0);
    QVERIFY(metrics.strikethroughThickness > 0.0);
    QVERIFY(metrics.overlineThickness > 0.0);
    QVERIFY(metrics.cursorThickness > 0.0);
    QVERIFY(metrics.cursorHeight > 0.0);
}

} // namespace

class TerminalCellMetricsTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void resolvesRegularAndAutomaticFaces();
    void preservesOrderedFallbacksAndAliases();
    void disablesStyledFaces();
    void resolvesExactNamedStyleWithoutGenericFlags();
    void resolvesNamedStyleThroughGenericAlias();
    void resolvesNamedStylesForRegularAndItalicRoles();
    void synthesizesMissingNamedStyleWhenAllowed();
    void appliesOrderedFeaturesWithoutChangingGrid();
    void appliesFirstMatchingVariation();
    void appliesVariationsToEveryRole();
    void resolvesCodepointMapsWithLaterEntryPolicy();
    void normalizesCodepointMapIntervals();
    void obeysSyntheticStylePermissions();
    void approximatesFreetypeLoadFlags();
    void sharesImmutableFontPrograms();
    void projectsBaseMetricsToPhysicalPixels_data();
    void projectsBaseMetricsToPhysicalPixels();
    void usesIndependentFontStrokeThicknesses();
    void appliesAbsoluteModifierToEveryMetric_data();
    void appliesAbsoluteModifierToEveryMetric();
    void appliesPercentageModifierToEveryMetric_data();
    void appliesPercentageModifierToEveryMetric();
    void clampsMinimumsAndUnsignedPositions();
    void baselineModifierUsesBottomRelativeSign();
    void recentersCellHeightButNotCursor_data();
    void recentersCellHeightButNotCursor();
    void obeysExportedModifierApplicationOrder();
    void derivesCenteredCursorAndDecorationLimits();
    void saturatesExtremeModifiers();
    void normalizesInvalidDprAndPointSize();
    void repeatedResolutionDoesNotAccumulateRounding();
};

void TerminalCellMetricsTest::resolvesRegularAndAutomaticFaces()
{
    const QString family =
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    TerminalTypography typography = typographyFor({family});
    typography.pointSize = 15.5;

    const TerminalCellMetrics actual = terminalCellMetrics(typography);
    QCOMPARE(actual.font(TerminalFontRole::Regular).pointSizeF(), 15.5);

    for (const TerminalFontRole role : kRoles) {
        const QFont &font = actual.font(role);
        QVERIFY(font.fixedPitch());
        QCOMPARE(font.styleHint(), QFont::Monospace);
        QVERIFY(static_cast<int>(font.styleStrategy())
                & static_cast<int>(QFont::ContextFontMerging));
    }
    QVERIFY(!actual.font(TerminalFontRole::Regular).bold());
    QVERIFY(!actual.font(TerminalFontRole::Regular).italic());
    QVERIFY(actual.font(TerminalFontRole::Bold).bold());
    QVERIFY(!actual.font(TerminalFontRole::Bold).italic());
    QVERIFY(!actual.font(TerminalFontRole::Italic).bold());
    QVERIFY(actual.font(TerminalFontRole::Italic).italic());
    QVERIFY(actual.font(TerminalFontRole::BoldItalic).bold());
    QVERIFY(actual.font(TerminalFontRole::BoldItalic).italic());
}

void TerminalCellMetricsTest::preservesOrderedFallbacksAndAliases()
{
    const QStringList available = fixedPitchFamilies();
    if (available.size() < 2) {
        QSKIP("Two fixed-pitch families are required");
    }

    TerminalTypography typography = typographyFor({
        QStringLiteral("ghostty-qt-certainly-missing"),
        available.at(1),
        available.at(0),
        available.at(1),
    });
    typography.face(TerminalFontRole::Bold).families = {
        QStringLiteral("ghostty-qt-also-missing"),
        available.at(0),
    };

    const TerminalCellMetrics actual = terminalCellMetrics(typography);
    const QStringList regular =
        actual.font(TerminalFontRole::Regular).families();
    QCOMPARE(regular.at(0), QStringLiteral("ghostty-qt-certainly-missing"));
    QCOMPARE(regular.at(1), available.at(1));
    QCOMPARE(regular.at(2), available.at(0));
    QCOMPARE(regular.count(available.at(1)), 1);
    QVERIFY(QFontInfo(actual.font(TerminalFontRole::Regular))
                .family()
                .compare(available.at(1), Qt::CaseInsensitive)
            == 0);

    const QStringList bold = actual.font(TerminalFontRole::Bold).families();
    QCOMPARE(bold.front(), QStringLiteral("ghostty-qt-also-missing"));
    QCOMPARE(bold.at(1), available.at(0));
    QVERIFY(bold.contains(available.at(1)));
    QVERIFY(QFontInfo(actual.font(TerminalFontRole::Bold))
                .family()
                .compare(available.at(0), Qt::CaseInsensitive)
            == 0);
}

void TerminalCellMetricsTest::disablesStyledFaces()
{
    TerminalTypography typography;
    typography.face(TerminalFontRole::Bold).style =
        TerminalFontStyles::Disabled{};
    typography.face(TerminalFontRole::Italic).style =
        TerminalFontStyles::Disabled{};
    typography.face(TerminalFontRole::BoldItalic).style =
        TerminalFontStyles::Disabled{};
    // Disabled on regular has no special meaning and behaves as automatic.
    typography.face(TerminalFontRole::Regular).style =
        TerminalFontStyles::Disabled{};

    const TerminalCellMetrics actual = terminalCellMetrics(typography);
    const QFont &regular = actual.font(TerminalFontRole::Regular);
    QCOMPARE(actual.font(TerminalFontRole::Bold), regular);
    QCOMPARE(actual.font(TerminalFontRole::Italic), regular);
    QCOMPARE(actual.font(TerminalFontRole::BoldItalic), regular);
}

void TerminalCellMetricsTest::resolvesExactNamedStyleWithoutGenericFlags()
{
    const std::optional<NamedFace> choice = namedFace();
    if (!choice) {
        QSKIP("No exact styled fixed-pitch face is available");
    }

    TerminalTypography typography = typographyFor({choice->family});
    typography.face(TerminalFontRole::Bold).families = {choice->family};
    typography.face(TerminalFontRole::Bold).style =
        TerminalFontStyles::Named{choice->style};

    const TerminalCellMetrics metrics = terminalCellMetrics(typography);
    const QFont &font = metrics.font(TerminalFontRole::Bold);
    QCOMPARE(font.styleName(), choice->style);
    QVERIFY(!font.bold());
    QVERIFY(!font.italic());
    const QFontInfo resolved(font);
    QCOMPARE(resolved.styleName(), choice->style);
    QVERIFY(resolved.family().compare(choice->family, Qt::CaseInsensitive)
            == 0);
}

void TerminalCellMetricsTest::resolvesNamedStyleThroughGenericAlias()
{
    QFont alias = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    alias.setFamilies({QStringLiteral("monospace")});
    alias.setPointSizeF(12.0);
    alias.setFixedPitch(true);
    alias.setStyleHint(QFont::Monospace,
                       static_cast<QFont::StyleStrategy>(
                           QFont::PreferDefault | QFont::ContextFontMerging));
    alias.setStyleName({});
    alias.setWeight(QFont::Normal);
    alias.setStyle(QFont::StyleNormal);
    const QString canonical = QFontInfo(alias).family();
    const QStringList styles = QFontDatabase::styles(canonical);
    const auto style =
        std::ranges::find_if(styles, [&canonical](const QString &candidate) {
            QFont probe({canonical}, 12);
            probe.setFixedPitch(true);
            probe.setStyleHint(
                QFont::Monospace,
                static_cast<QFont::StyleStrategy>(QFont::PreferDefault
                                                  | QFont::ContextFontMerging));
            probe.setWeight(QFont::Normal);
            probe.setStyle(QFont::StyleNormal);
            probe.setStyleName(candidate);
            const QFontInfo resolved(probe);
            return resolved.family().compare(canonical, Qt::CaseInsensitive)
                == 0
                && resolved.styleName() == candidate;
        });
    if (style == styles.end()) {
        QSKIP("The system monospace alias has no resolvable named style");
    }

    TerminalTypography typography = typographyFor({canonical});
    typography.face(TerminalFontRole::Bold).families = {
        QStringLiteral("monospace")};
    typography.face(TerminalFontRole::Bold).style =
        TerminalFontStyles::Named{*style};

    const TerminalCellMetrics metrics = terminalCellMetrics(typography);
    const QFont &font = metrics.font(TerminalFontRole::Bold);
    QCOMPARE(font.styleName(), *style);
    QCOMPARE(QFontInfo(font).styleName(), *style);
    QVERIFY(QFontInfo(font).family().compare(canonical, Qt::CaseInsensitive)
            == 0);
}

void TerminalCellMetricsTest::resolvesNamedStylesForRegularAndItalicRoles()
{
    const std::optional<NamedFace> choice = namedFace();
    if (!choice) {
        QSKIP("No exact styled fixed-pitch face is available");
    }

    for (const TerminalFontRole role :
         {TerminalFontRole::Regular, TerminalFontRole::Italic,
          TerminalFontRole::BoldItalic}) {
        TerminalTypography typography = typographyFor({choice->family});
        typography.face(role).families = {choice->family};
        typography.face(role).style = TerminalFontStyles::Named{choice->style};

        const TerminalCellMetrics metrics = terminalCellMetrics(typography);
        const QFont &font = metrics.font(role);
        QCOMPARE(font.styleName(), choice->style);
        const QFontInfo resolved(font);
        QCOMPARE(resolved.styleName(), choice->style);
        QVERIFY(resolved.family().compare(choice->family, Qt::CaseInsensitive)
                == 0);
    }
}

void TerminalCellMetricsTest::synthesizesMissingNamedStyleWhenAllowed()
{
    const QString family =
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    TerminalTypography typography = typographyFor({family});
    typography.face(TerminalFontRole::Bold).families = {family};
    typography.face(TerminalFontRole::Bold).style =
        TerminalFontStyles::Named{QStringLiteral("ghostty-qt-no-such-style")};

    const TerminalCellMetrics actual = terminalCellMetrics(typography);
    QVERIFY(actual.font(TerminalFontRole::Bold).bold());
    typography.syntheticStyle.bold = false;
    const TerminalCellMetrics synthesisDisabled =
        terminalCellMetrics(typography);
    QCOMPARE(synthesisDisabled.font(TerminalFontRole::Bold),
             synthesisDisabled.font(TerminalFontRole::Regular));

    typography.syntheticStyle.bold = true;
    typography.face(TerminalFontRole::Bold).families.clear();
    typography.face(TerminalFontRole::Bold).style =
        TerminalFontStyles::Named{QFontDatabase::styles(family).value(0)};
    const TerminalCellMetrics styleWithoutFamily =
        terminalCellMetrics(typography);
    QVERIFY(styleWithoutFamily.font(TerminalFontRole::Bold).bold());

    const QStringList styles = QFontDatabase::styles(family);
    if (!styles.isEmpty()) {
        typography.face(TerminalFontRole::Bold).families = {
            QStringLiteral("ghostty-qt-certainly-missing")};
        typography.face(TerminalFontRole::Bold).style =
            TerminalFontStyles::Named{styles.front()};
        const TerminalCellMetrics missingFamily =
            terminalCellMetrics(typography);
        QVERIFY(missingFamily.font(TerminalFontRole::Bold).bold());
    }
}

void TerminalCellMetricsTest::appliesOrderedFeaturesWithoutChangingGrid()
{
    const QString family =
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    TerminalTypography typography = typographyFor({family});
    const TerminalCellMetrics baseline = terminalCellMetrics(typography);
    constexpr quint32 Liga = 0x6c696761U;
    constexpr quint32 Calt = 0x63616c74U;
    typography.features = {
        {.tag = Liga, .value = 1},
        {.tag = Calt, .value = 0},
        {.tag = Calt, .value = 3},
    };

    const TerminalCellMetrics actual = terminalCellMetrics(typography);
    const auto liga = QFont::Tag::fromValue(Liga);
    const auto calt = QFont::Tag::fromValue(Calt);
    QVERIFY(liga.has_value());
    QVERIFY(calt.has_value());
    for (const TerminalFontRole role : kRoles) {
        const QFont &font = actual.font(role);
        QVERIFY(font.isFeatureSet(*liga));
        QCOMPARE(font.featureValue(*liga), quint32{1});
        QVERIFY(font.isFeatureSet(*calt));
        QCOMPARE(font.featureValue(*calt), quint32{3});
    }
    QCOMPARE(actual.cellWidth, baseline.cellWidth);
    QCOMPARE(actual.cellHeight, baseline.cellHeight);
    QCOMPARE(actual.baseline, baseline.baseline);
}

void TerminalCellMetricsTest::appliesFirstMatchingVariation()
{
    const QString path = QFINDTESTDATA("../ghostty/src/font/res/Lilex-VF.ttf");
    QVERIFY2(!path.isEmpty(), "Bundled variable test font was not found");
    const int fontId = QFontDatabase::addApplicationFont(path);
    QVERIFY(fontId >= 0);
    const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
    QVERIFY(!families.isEmpty());

    TerminalTypography typography = typographyFor({families.front()});
    const QFont probe({families.front()}, 12);
    const QList<QFontVariableAxis> axes = QFontInfo(probe).variableAxes();
    if (axes.isEmpty()) {
        QVERIFY(QFontDatabase::removeApplicationFont(fontId));
        QSKIP("Bundled font exposes no variable axes through this Qt backend");
    }
    const QFontVariableAxis &axis = axes.front();
    const double first = axis.minimumValue();
    const double second = axis.maximumValue();
    typography.face(TerminalFontRole::Regular).variations = {
        TerminalFontVariation::fromValue(axis.tag().value(), first),
        TerminalFontVariation::fromValue(axis.tag().value(), second),
    };

    TerminalCellMetrics actual = terminalCellMetrics(typography);
    QVERIFY(
        actual.font(TerminalFontRole::Regular).isVariableAxisSet(axis.tag()));
    QCOMPARE(
        actual.font(TerminalFontRole::Regular).variableAxisValue(axis.tag()),
        static_cast<float>(first));

    typography.face(TerminalFontRole::Regular).variations = {
        TerminalFontVariation::fromValue(
            axis.tag().value(), std::numeric_limits<double>::infinity()),
        TerminalFontVariation::fromValue(axis.tag().value(), second),
    };
    actual = terminalCellMetrics(typography);
    QVERIFY(
        !actual.font(TerminalFontRole::Regular).isVariableAxisSet(axis.tag()));
    QVERIFY(QFontDatabase::removeApplicationFont(fontId));
}

void TerminalCellMetricsTest::appliesVariationsToEveryRole()
{
    const QString path = QFINDTESTDATA("../ghostty/src/font/res/Lilex-VF.ttf");
    QVERIFY2(!path.isEmpty(), "Bundled variable test font was not found");
    const int fontId = QFontDatabase::addApplicationFont(path);
    QVERIFY(fontId >= 0);
    const QString family =
        QFontDatabase::applicationFontFamilies(fontId).value(0);
    QVERIFY(!family.isEmpty());

    const QList<QFontVariableAxis> axes =
        QFontInfo(QFont({family}, 12)).variableAxes();
    if (axes.isEmpty()) {
        QVERIFY(QFontDatabase::removeApplicationFont(fontId));
        QSKIP("Bundled font exposes no variable axes through this Qt backend");
    }
    const QFontVariableAxis &axis = axes.front();
    TerminalTypography typography = typographyFor({family});
    std::array<float, kRoles.size()> expected{};
    for (std::size_t index = 0; index < kRoles.size(); ++index) {
        const TerminalFontRole role = kRoles[index];
        typography.face(role).families = {family};
        const double fraction = static_cast<double>(index + 1U)
            / static_cast<double>(kRoles.size() + 1U);
        const double value = axis.minimumValue()
            + (axis.maximumValue() - axis.minimumValue()) * fraction;
        expected[index] = static_cast<float>(value);
        typography.face(role).variations = {
            TerminalFontVariation::fromValue(axis.tag().value(), value),
        };
    }

    const TerminalCellMetrics metrics = terminalCellMetrics(typography);
    for (std::size_t index = 0; index < kRoles.size(); ++index) {
        const QFont &font = metrics.font(kRoles[index]);
        QVERIFY(font.isVariableAxisSet(axis.tag()));
        QCOMPARE(font.variableAxisValue(axis.tag()), expected[index]);
    }
    QVERIFY(QFontDatabase::removeApplicationFont(fontId));
}

void TerminalCellMetricsTest::resolvesCodepointMapsWithLaterEntryPolicy()
{
    const QString basePath = QFINDTESTDATA(
        "../ghostty/src/font/res/JetBrainsMonoNerdFont-Regular.ttf");
    const QString mappedPath =
        QFINDTESTDATA("../ghostty/src/font/res/CodeNewRoman-Regular.otf");
    QVERIFY(!basePath.isEmpty());
    QVERIFY(!mappedPath.isEmpty());
    const int baseId = QFontDatabase::addApplicationFont(basePath);
    const int mappedId = QFontDatabase::addApplicationFont(mappedPath);
    QVERIFY(baseId >= 0);
    QVERIFY(mappedId >= 0);
    const QString baseFamily =
        QFontDatabase::applicationFontFamilies(baseId).value(0);
    const QString mappedFamily =
        QFontDatabase::applicationFontFamilies(mappedId).value(0);
    QVERIFY(!baseFamily.isEmpty());
    QVERIFY(!mappedFamily.isEmpty());

    TerminalTypography typography = typographyFor({baseFamily});
    typography.codepointMap = {
        {.first = U'A', .last = U'Z', .family = baseFamily},
        {.first = U'M', .last = U'M', .family = mappedFamily},
    };
    TerminalCellMetrics metrics = terminalCellMetrics(typography);
    const QFont &mapped =
        metrics.fontForText(TerminalFontRole::Bold, QStringLiteral("M"));
    QVERIFY(
        QFontInfo(mapped).family().compare(mappedFamily, Qt::CaseInsensitive)
        == 0);
    QVERIFY(!mapped.bold());
    // The broad earlier mapping remains active on both sides of the later
    // single-codepoint overlay. A bold request still resolves its regular
    // mapped face rather than the ordinary bold role.
    for (const QString &text : {QStringLiteral("A"), QStringLiteral("Z")}) {
        const QFont &remainingBroadMap =
            metrics.fontForText(TerminalFontRole::Bold, text);
        QVERIFY(QFontInfo(remainingBroadMap)
                    .family()
                    .compare(baseFamily, Qt::CaseInsensitive)
                == 0);
        QVERIFY(!remainingBroadMap.bold());
    }

    // The latest overlapping declaration owns the range even when its face
    // cannot be loaded; Ghostty then falls back normally instead of retrying
    // an earlier mapping.
    typography.codepointMap.append(
        {.first = U'M',
         .last = U'M',
         .family = QStringLiteral("ghostty-qt-missing-map")});
    metrics = terminalCellMetrics(typography);
    QCOMPARE(
        metrics.fontForText(TerminalFontRole::Regular, QStringLiteral("M")),
        metrics.font(TerminalFontRole::Regular));

    typography.codepointMap.removeLast();
    metrics = terminalCellMetrics(typography);
    QCOMPARE(metrics.fontForText(TerminalFontRole::Regular,
                                 QString::fromUtf8("M\xf0\x9f\x98\x80")),
             metrics.font(TerminalFontRole::Regular));
    const QString ignoredModifiers =
        QStringLiteral("M") + QChar(0x200d) + QChar(0xfe0f);
    QVERIFY(QFontInfo(metrics.fontForText(TerminalFontRole::Regular,
                                          ignoredModifiers))
                .family()
                .compare(mappedFamily, Qt::CaseInsensitive)
            == 0);

    QVERIFY(QFontDatabase::removeApplicationFont(mappedId));
    QVERIFY(QFontDatabase::removeApplicationFont(baseId));
}

void TerminalCellMetricsTest::normalizesCodepointMapIntervals()
{
    TerminalTypography typography;
    typography.codepointMap = {
        {.first = 10, .last = 30, .family = QStringLiteral("zero")},
        {.first = 20, .last = 40, .family = QStringLiteral("one")},
        {.first = 25, .last = 26, .family = QStringLiteral("two")},
        {.first = 0, .last = 15, .family = QStringLiteral("three")},
        // Invalid direct-construction input is ignored defensively. Ghostty's
        // finalized parser never emits a descending range.
        {.first = 50, .last = 49, .family = QStringLiteral("invalid")},
    };

    const TerminalCellMetrics metrics = terminalCellMetrics(typography);
    QVERIFY(metrics.fontProgram != nullptr);
    const QVector<TerminalMappedFontInterval> &segments =
        metrics.fontProgram->mappedIntervals;
    QCOMPARE(segments.size(), 5);
    const auto verify = [&segments](qsizetype index, quint32 first,
                                    quint32 last, qsizetype sourceIndex) {
        QCOMPARE(segments.at(index).first, first);
        QCOMPARE(segments.at(index).last, last);
        QCOMPARE(segments.at(index).sourceIndex, sourceIndex);
    };
    verify(0, 0, 15, 3);
    verify(1, 16, 19, 0);
    verify(2, 20, 24, 1);
    verify(3, 25, 26, 2);
    verify(4, 27, 40, 1);
    QVERIFY(std::ranges::all_of(segments,
                                [](const TerminalMappedFontInterval &segment) {
                                    return segment.faceIndex < 0;
                                }));

    // Fully shadowed declarations never need face resolution and collapse to
    // one winner rather than repeatedly copying an interval table.
    typography.codepointMap.clear();
    for (int index = 0; index < 4096; ++index) {
        typography.codepointMap.append({
            .first = 0,
            .last = 0x1fffffU,
            .family = QStringLiteral("shadow-%1").arg(index),
        });
    }
    const TerminalCellMetrics shadowed = terminalCellMetrics(typography);
    QCOMPARE(shadowed.fontProgram->mappedIntervals.size(), 1);
    QCOMPARE(shadowed.fontProgram->mappedIntervals.front().sourceIndex,
             qsizetype{4095});
    QCOMPARE(shadowed.fontProgram->mappedFaces.size(), 0);

    const QString family =
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    TerminalTypography repeatedFace;
    repeatedFace.codepointMap = {
        {.first = 1, .last = 2, .family = family},
        {.first = 4, .last = 5, .family = family},
    };
    const TerminalCellMetrics deduplicated = terminalCellMetrics(repeatedFace);
    QCOMPARE(deduplicated.fontProgram->mappedIntervals.size(), 2);
    QCOMPARE(deduplicated.fontProgram->mappedFaces.size(), 1);
    QCOMPARE(deduplicated.fontProgram->mappedIntervals.at(0).faceIndex,
             deduplicated.fontProgram->mappedIntervals.at(1).faceIndex);
}

void TerminalCellMetricsTest::obeysSyntheticStylePermissions()
{
    const QString family =
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    TerminalTypography typography = typographyFor({family});
    typography.syntheticStyle = {
        .bold = false,
        .italic = false,
        .boldItalic = false,
    };

    const TerminalCellMetrics disabled = terminalCellMetrics(typography);
    const QFont &regular = disabled.font(TerminalFontRole::Regular);
    QCOMPARE(disabled.font(TerminalFontRole::Bold), regular);
    QCOMPARE(disabled.font(TerminalFontRole::Italic), regular);
    QCOMPARE(disabled.font(TerminalFontRole::BoldItalic), regular);

    typography.syntheticStyle = {};
    const TerminalCellMetrics enabled = terminalCellMetrics(typography);
    QVERIFY(enabled.font(TerminalFontRole::Bold).bold());
    QVERIFY(enabled.font(TerminalFontRole::Italic).italic());
    QVERIFY(enabled.font(TerminalFontRole::BoldItalic).bold());
    QVERIFY(enabled.font(TerminalFontRole::BoldItalic).italic());
}

void TerminalCellMetricsTest::approximatesFreetypeLoadFlags()
{
    TerminalTypography typography;
    typography.freetypeLoadFlags.hinting = false;
    typography.freetypeLoadFlags.monochrome = true;
    const TerminalCellMetrics metrics = terminalCellMetrics(typography);

    for (const TerminalFontRole role : kRoles) {
        const QFont &font = metrics.font(role);
        QCOMPARE(font.hintingPreference(), QFont::PreferNoHinting);
        QVERIFY(static_cast<int>(font.styleStrategy())
                & static_cast<int>(QFont::NoAntialias));
    }

    TerminalTypography light;
    const TerminalCellMetrics lightMetrics = terminalCellMetrics(light);
    QCOMPARE(lightMetrics.font(TerminalFontRole::Regular).hintingPreference(),
             QFont::PreferVerticalHinting);
    light.freetypeLoadFlags.light = false;
    const TerminalCellMetrics fullMetrics = terminalCellMetrics(light);
    QCOMPARE(fullMetrics.font(TerminalFontRole::Regular).hintingPreference(),
             QFont::PreferFullHinting);
}

void TerminalCellMetricsTest::sharesImmutableFontPrograms()
{
    TerminalTypography typography;
    const TerminalCellMetrics first = terminalCellMetrics(typography, 1.0);
    const TerminalCellMetrics otherDpr = terminalCellMetrics(typography, 1.5);
    QVERIFY(first.fontProgram != nullptr);
    QVERIFY(first.fontProgram == otherDpr.fontProgram);

    TerminalTypography layoutOnly = typography;
    layoutOnly.shapingBreakCursor = false;
    layoutOnly.metricModifiers[TerminalMetric::CellWidth] =
        TerminalMetricModifiers::Absolute{2};
    const TerminalCellMetrics adjusted = terminalCellMetrics(layoutOnly, 1.0);
    QVERIFY(first.fontProgram == adjusted.fontProgram);
    QVERIFY(first.cellWidth != adjusted.cellWidth);

    TerminalTypography transportOnly = typography;
    transportOnly.freetypeLoadFlags.forceAutohint = true;
    transportOnly.freetypeLoadFlags.autohint = false;
    const TerminalCellMetrics ignoredFlags =
        terminalCellMetrics(transportOnly, 1.0);
    QVERIFY(first.fontProgram == ignoredFlags.fontProgram);
    QCOMPARE(first, ignoredFlags);

    TerminalTypography shaping = typography;
    shaping.features = {{.tag = 0x6c696761U, .value = 0}};
    const TerminalCellMetrics changed = terminalCellMetrics(shaping, 1.0);
    QVERIFY(first.fontProgram != changed.fontProgram);

    const auto verifyDifferent = [&first](TerminalTypography source) {
        const TerminalCellMetrics metrics = terminalCellMetrics(source, 1.0);
        QVERIFY(metrics.fontProgram != first.fontProgram);
    };
    TerminalTypography source = typography;
    source.pointSize = 13.0;
    verifyDifferent(source);
    source = typography;
    source.face(TerminalFontRole::Regular).families = {
        QStringLiteral("ghostty-qt-cache-key")};
    verifyDifferent(source);
    source = typography;
    source.face(TerminalFontRole::Regular).style =
        TerminalFontStyles::Named{QStringLiteral("cache-key")};
    verifyDifferent(source);
    source = typography;
    source.face(TerminalFontRole::Regular).variations = {
        TerminalFontVariation::fromValue(0x77676874U, 500.0)};
    verifyDifferent(source);
    source = typography;
    source.codepointMap = {{.first = U'A',
                            .last = U'Z',
                            .family = QStringLiteral("ghostty-qt-cache-map")}};
    verifyDifferent(source);
    source = typography;
    source.syntheticStyle.bold = false;
    verifyDifferent(source);
    source = typography;
    source.freetypeLoadFlags.hinting = false;
    verifyDifferent(source);
    source = typography;
    source.freetypeLoadFlags.light = false;
    verifyDifferent(source);
    source = typography;
    source.freetypeLoadFlags.monochrome = true;
    verifyDifferent(source);

    const QString path =
        QFINDTESTDATA("../ghostty/src/font/res/CodeNewRoman-Regular.otf");
    QVERIFY2(!path.isEmpty(), "Bundled font was not found");
    const int fontId = QFontDatabase::addApplicationFont(path);
    QVERIFY(fontId >= 0);
    const TerminalCellMetrics afterDatabaseChange =
        terminalCellMetrics(typography, 1.0);
    QVERIFY(first.fontProgram != afterDatabaseChange.fontProgram);
    QVERIFY(QFontDatabase::removeApplicationFont(fontId));

    std::weak_ptr<const TerminalFontProgram> released;
    {
        TerminalTypography transient = typography;
        transient.pointSize = 17.25;
        const TerminalCellMetrics metrics = terminalCellMetrics(transient, 1.0);
        released = metrics.fontProgram;
        QVERIFY(!released.expired());
    }
    QVERIFY(released.expired());
}

void TerminalCellMetricsTest::projectsBaseMetricsToPhysicalPixels_data()
{
    QTest::addColumn<qreal>("dpr");
    QTest::newRow("1") << 1.0;
    QTest::newRow("1.25") << 1.25;
    QTest::newRow("1.5") << 1.5;
    QTest::newRow("2") << 2.0;
}

void TerminalCellMetricsTest::projectsBaseMetricsToPhysicalPixels()
{
    QFETCH(qreal, dpr);
    const TerminalCellMetrics actual =
        terminalCellMetrics(TerminalTypography{}, dpr);
    const QFontMetricsF fontMetrics(actual.font(TerminalFontRole::Regular));

    qreal maximumAdvance = 0.0;
    for (char value = 0x20; value <= 0x7e; ++value) {
        maximumAdvance = std::max(
            maximumAdvance, fontMetrics.horizontalAdvance(QLatin1Char(value)));
    }
    const qint64 expectedWidth =
        std::max<qint64>(1, std::llround(maximumAdvance * dpr));
    const qreal faceHeight = fontMetrics.lineSpacing() * dpr;
    const qint64 expectedHeight = std::max<qint64>(1, std::llround(faceHeight));
    const qreal faceBaseline =
        (fontMetrics.leading() / 2.0 + fontMetrics.descent()) * dpr;
    const qint64 expectedBottomBaseline = std::llround(
        faceBaseline - (static_cast<qreal>(expectedHeight) - faceHeight) / 2.0);
    const qint64 expectedTopBaseline = expectedHeight - expectedBottomBaseline;
    const qint64 expectedUnderlineThickness = expectedThickness(
        actual.font(TerminalFontRole::Regular), fontMetrics, "post", 10, dpr);
    const double underlineFallback =
        physicalTableThickness(
            QRawFont::fromFont(actual.font(TerminalFontRole::Regular)), "post",
            10, dpr)
            .value_or(fontMetrics.lineWidth() * dpr);
    const qint64 expectedStrikethroughThickness =
        expectedThickness(actual.font(TerminalFontRole::Regular), fontMetrics,
                          "OS/2", 26, dpr, underlineFallback);

    QCOMPARE(physical(actual.cellWidth, dpr), expectedWidth);
    QCOMPARE(physical(actual.cellHeight, dpr), expectedHeight);
    QCOMPARE(physical(actual.baseline, dpr), expectedTopBaseline);
    QCOMPARE(physical(actual.underlinePosition, dpr),
             std::llround(static_cast<qreal>(expectedTopBaseline)
                          + fontMetrics.underlinePos() * dpr));
    QCOMPARE(physical(actual.strikethroughPosition, dpr),
             std::llround(static_cast<qreal>(expectedTopBaseline)
                          - fontMetrics.strikeOutPos() * dpr));
    QCOMPARE(physical(actual.underlineThickness, dpr),
             expectedUnderlineThickness);
    QCOMPARE(physical(actual.strikethroughThickness, dpr),
             expectedStrikethroughThickness);
    QCOMPARE(physical(actual.overlinePosition, dpr), 0);
    QCOMPARE(physical(actual.overlineThickness, dpr),
             expectedUnderlineThickness);
    QCOMPARE(physical(actual.cursorThickness, dpr), 1);
    QCOMPARE(physical(actual.cursorHeight, dpr), expectedHeight);
    verifyPhysicalGeometry(actual, dpr);
}

void TerminalCellMetricsTest::usesIndependentFontStrokeThicknesses()
{
    const QString path =
        QFINDTESTDATA("../ghostty/src/font/res/CodeNewRoman-Regular.otf");
    QVERIFY2(!path.isEmpty(), "Bundled metric test font was not found");
    const int fontId = QFontDatabase::addApplicationFont(path);
    QVERIFY(fontId >= 0);
    const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
    QVERIFY(!families.isEmpty());

    constexpr qreal dpr = 1.5;
    TerminalTypography typography = typographyFor({families.front()});
    typography.pointSize = 36.0;
    const TerminalCellMetrics actual = terminalCellMetrics(typography, dpr);
    const QFont &font = actual.font(TerminalFontRole::Regular);
    const QRawFont rawFont = QRawFont::fromFont(font);
    const auto underline = physicalTableThickness(rawFont, "post", 10, dpr);
    const auto strikethrough = physicalTableThickness(rawFont, "OS/2", 26, dpr);
    QVERIFY(underline);
    QVERIFY(strikethrough);

    const qint64 expectedUnderline = std::max<qint64>(1, std::ceil(*underline));
    const qint64 expectedStrikethrough =
        std::max<qint64>(1, std::ceil(*strikethrough));
    QVERIFY(expectedUnderline != expectedStrikethrough);
    QCOMPARE(physical(actual.underlineThickness, dpr), expectedUnderline);
    QCOMPARE(physical(actual.strikethroughThickness, dpr),
             expectedStrikethrough);
    QCOMPARE(physical(actual.overlineThickness, dpr), expectedUnderline);

    QVERIFY(QFontDatabase::removeApplicationFont(fontId));
}

void TerminalCellMetricsTest::appliesAbsoluteModifierToEveryMetric_data()
{
    QTest::addColumn<int>("metric");
    for (const TerminalMetric metric : kMetrics) {
        const QByteArray name = QByteArrayLiteral("metric-")
            + QByteArray::number(static_cast<int>(metric));
        QTest::newRow(name.constData()) << static_cast<int>(metric);
    }
}

void TerminalCellMetricsTest::appliesAbsoluteModifierToEveryMetric()
{
    QFETCH(int, metric);
    const auto key = static_cast<TerminalMetric>(metric);
    constexpr qint32 delta = 3;
    const TerminalCellMetrics base = terminalCellMetrics(TerminalTypography{});
    TerminalTypography typography;
    typography.metricModifiers[key] = TerminalMetricModifiers::Absolute{delta};
    const TerminalCellMetrics adjusted = terminalCellMetrics(typography);

    const qint64 expected = key == TerminalMetric::FontBaseline
        ? physical(base.baseline, 1.0) - delta
        : physical(metricValue(base, key), 1.0) + delta;
    QCOMPARE(physical(metricValue(adjusted, key), 1.0), expected);
}

void TerminalCellMetricsTest::appliesPercentageModifierToEveryMetric_data()
{
    appliesAbsoluteModifierToEveryMetric_data();
}

void TerminalCellMetricsTest::appliesPercentageModifierToEveryMetric()
{
    QFETCH(int, metric);
    const auto key = static_cast<TerminalMetric>(metric);
    constexpr double multiplier = 1.5;
    const TerminalCellMetrics base = terminalCellMetrics(TerminalTypography{});
    TerminalTypography typography;
    typography.metricModifiers[key] =
        TerminalMetricModifiers::Percentage{multiplier};
    const TerminalCellMetrics adjusted = terminalCellMetrics(typography);

    qint64 expected = 0;
    if (key == TerminalMetric::FontBaseline) {
        const qint64 height = physical(base.cellHeight, 1.0);
        const qint64 bottomBaseline = height - physical(base.baseline, 1.0);
        expected = height
            - std::llround(static_cast<double>(bottomBaseline) * multiplier);
    } else {
        expected = std::llround(
            static_cast<double>(physical(metricValue(base, key), 1.0))
            * multiplier);
    }
    QCOMPARE(physical(metricValue(adjusted, key), 1.0), expected);
}

void TerminalCellMetricsTest::clampsMinimumsAndUnsignedPositions()
{
    constexpr qint32 veryNegative = std::numeric_limits<qint32>::min();
    TerminalTypography typography;
    for (const TerminalMetric metric : kMetrics) {
        typography.metricModifiers[metric] =
            TerminalMetricModifiers::Absolute{veryNegative};
    }
    const TerminalCellMetrics actual = terminalCellMetrics(typography);

    QCOMPARE(actual.cellWidth, 1.0);
    QCOMPARE(actual.cellHeight, 1.0);
    QCOMPARE(actual.underlinePosition, 0.0);
    QCOMPARE(actual.underlineThickness, 1.0);
    QCOMPARE(actual.strikethroughPosition, 0.0);
    QCOMPARE(actual.strikethroughThickness, 1.0);
    QCOMPARE(actual.overlinePosition,
             static_cast<qreal>(std::numeric_limits<qint32>::min()));
    QCOMPARE(actual.overlineThickness, 1.0);
    QCOMPARE(actual.cursorThickness, 1.0);
    QCOMPARE(actual.cursorHeight, 1.0);
    // The bottom-relative baseline saturates at zero.
    QCOMPARE(actual.baseline, actual.cellHeight);
}

void TerminalCellMetricsTest::baselineModifierUsesBottomRelativeSign()
{
    const TerminalCellMetrics base = terminalCellMetrics(TerminalTypography{});
    TerminalTypography up;
    up.metricModifiers[TerminalMetric::FontBaseline] =
        TerminalMetricModifiers::Absolute{1};
    TerminalTypography down;
    down.metricModifiers[TerminalMetric::FontBaseline] =
        TerminalMetricModifiers::Absolute{-1};

    QCOMPARE(terminalCellMetrics(up).baseline, base.baseline - 1.0);
    QCOMPARE(terminalCellMetrics(down).baseline, base.baseline + 1.0);
}

void TerminalCellMetricsTest::recentersCellHeightButNotCursor_data()
{
    QTest::addColumn<qint32>("delta");
    QTest::newRow("grow odd") << qint32{3};
    QTest::newRow("shrink odd") << qint32{-3};
}

void TerminalCellMetricsTest::recentersCellHeightButNotCursor()
{
    QFETCH(qint32, delta);
    const TerminalCellMetrics base = terminalCellMetrics(TerminalTypography{});
    if (base.cellHeight + delta < 1.0) {
        QSKIP("The system fixed font is too small for this case");
    }
    TerminalTypography typography;
    typography.metricModifiers[TerminalMetric::CellHeight] =
        TerminalMetricModifiers::Absolute{delta};
    const TerminalCellMetrics adjusted = terminalCellMetrics(typography);

    QCOMPARE(adjusted.cellHeight, base.cellHeight + delta);
    QCOMPARE(adjusted.cursorHeight, base.cursorHeight);
    const qreal shiftFromTop = adjusted.baseline - base.baseline;
    QVERIFY(shiftFromTop == std::floor(delta / 2.0)
            || shiftFromTop == std::ceil(delta / 2.0));
    QCOMPARE(adjusted.underlinePosition - base.underlinePosition, shiftFromTop);
    QCOMPARE(adjusted.strikethroughPosition - base.strikethroughPosition,
             shiftFromTop);
    QCOMPARE(adjusted.overlinePosition - base.overlinePosition, shiftFromTop);
    QCOMPARE(adjusted.cursorTop,
             static_cast<qreal>(static_cast<qint64>(adjusted.cellHeight
                                                    - adjusted.cursorHeight)
                                / 2));
}

void TerminalCellMetricsTest::obeysExportedModifierApplicationOrder()
{
    TerminalTypography heightThenPosition;
    heightThenPosition.metricModifiers[TerminalMetric::CellHeight] =
        TerminalMetricModifiers::Absolute{2};
    heightThenPosition.metricModifiers[TerminalMetric::UnderlinePosition] =
        TerminalMetricModifiers::Percentage{2.0};
    heightThenPosition.metricModifiers.applicationOrder = {
        TerminalMetric::CellHeight,
        TerminalMetric::UnderlinePosition,
    };

    TerminalTypography positionThenHeight = heightThenPosition;
    positionThenHeight.metricModifiers.applicationOrder = {
        TerminalMetric::UnderlinePosition,
        TerminalMetric::CellHeight,
    };
    TerminalTypography enumFallback = heightThenPosition;
    enumFallback.metricModifiers.applicationOrder.clear();

    const TerminalCellMetrics first = terminalCellMetrics(heightThenPosition);
    const TerminalCellMetrics second = terminalCellMetrics(positionThenHeight);
    const TerminalCellMetrics fallback = terminalCellMetrics(enumFallback);
    QCOMPARE(first, fallback);
    QCOMPARE(first.underlinePosition - second.underlinePosition, 1.0);
}

void TerminalCellMetricsTest::derivesCenteredCursorAndDecorationLimits()
{
    TerminalTypography typography;
    typography.metricModifiers[TerminalMetric::CellHeight] =
        TerminalMetricModifiers::Absolute{5};
    typography.metricModifiers[TerminalMetric::CursorHeight] =
        TerminalMetricModifiers::Absolute{-3};
    typography.metricModifiers[TerminalMetric::CursorThickness] =
        TerminalMetricModifiers::Absolute{4};
    typography.metricModifiers[TerminalMetric::UnderlineThickness] =
        TerminalMetricModifiers::Absolute{2};
    const TerminalCellMetrics actual = terminalCellMetrics(typography);

    const qint64 cellHeight = physical(actual.cellHeight, 1.0);
    const qint64 cursorHeight = physical(actual.cursorHeight, 1.0);
    const qint64 cursorThickness = physical(actual.cursorThickness, 1.0);
    const qint64 underlineThickness = physical(actual.underlineThickness, 1.0);
    QCOMPARE(physical(actual.cursorTop, 1.0), (cellHeight - cursorHeight) / 2);
    QCOMPARE(physical(actual.cursorBarLeft, 1.0), -((cursorThickness + 1) / 2));
    QCOMPARE(
        physical(actual.underlineMaximumPosition, 1.0),
        std::max<qint64>(0, cellHeight + cellHeight / 4 - underlineThickness));
    QCOMPARE(physical(actual.overlineMinimumPosition, 1.0), -(cellHeight / 4));
}

void TerminalCellMetricsTest::saturatesExtremeModifiers()
{
    TerminalTypography typography;
    typography.metricModifiers[TerminalMetric::CellWidth] =
        TerminalMetricModifiers::Percentage{
            std::numeric_limits<double>::infinity()};
    typography.metricModifiers[TerminalMetric::OverlinePosition] =
        TerminalMetricModifiers::Absolute{std::numeric_limits<qint32>::max()};

    const TerminalCellMetrics actual = terminalCellMetrics(typography);
    QCOMPARE(actual.cellWidth,
             static_cast<qreal>(std::numeric_limits<quint32>::max()));
    QCOMPARE(actual.overlinePosition,
             static_cast<qreal>(std::numeric_limits<qint32>::max()));
}

void TerminalCellMetricsTest::normalizesInvalidDprAndPointSize()
{
    TerminalTypography typography;
    typography.pointSize = std::numeric_limits<double>::quiet_NaN();
    const TerminalCellMetrics expected = terminalCellMetrics(typography, 1.0);
    QCOMPARE(terminalCellMetrics(typography, 0.0), expected);
    QCOMPARE(terminalCellMetrics(typography, -2.0), expected);
    QCOMPARE(terminalCellMetrics(typography,
                                 std::numeric_limits<qreal>::quiet_NaN()),
             expected);
    QCOMPARE(expected.font(TerminalFontRole::Regular).pointSizeF(), 12.0);
}

void TerminalCellMetricsTest::repeatedResolutionDoesNotAccumulateRounding()
{
    TerminalTypography typography;
    typography.pointSize = 13.375;
    typography.metricModifiers[TerminalMetric::CellWidth] =
        TerminalMetricModifiers::Percentage{1.17};
    typography.metricModifiers[TerminalMetric::CellHeight] =
        TerminalMetricModifiers::Absolute{3};
    typography.metricModifiers[TerminalMetric::FontBaseline] =
        TerminalMetricModifiers::Absolute{-2};
    typography.metricModifiers.applicationOrder = {
        TerminalMetric::FontBaseline,
        TerminalMetric::CellHeight,
        TerminalMetric::CellWidth,
    };

    const TerminalCellMetrics first = terminalCellMetrics(typography, 1.25);
    for (int iteration = 0; iteration < 20; ++iteration) {
        QCOMPARE(terminalCellMetrics(typography, 1.25), first);
    }
}

QTEST_MAIN(TerminalCellMetricsTest)

#include "test_terminal_cell_metrics.moc"
