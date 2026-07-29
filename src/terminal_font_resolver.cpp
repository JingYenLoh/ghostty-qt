#include "terminal_font_resolver.h"

#include <QFontDatabase>
#include <QFontInfo>
#include <QFontVariableAxis>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <optional>
#include <ranges>
#include <utility>
#include <variant>

namespace {

[[nodiscard]] double normalizedPointSize(double value) noexcept
{
    return std::isfinite(value) && value > 0.0 ? value : 12.0;
}

void appendUnique(QStringList &destination, const QStringList &families)
{
    for (const QString &family : families) {
        if (family.isEmpty()
            || std::ranges::any_of(
                destination, [&family](const QString &existing) {
                    return existing.compare(family, Qt::CaseInsensitive) == 0;
                })) {
            continue;
        }
        destination.append(family);
    }
}

[[nodiscard]] QStringList requestedFamilies(const QStringList &families)
{
    QStringList result;
    appendUnique(result, families);
    return result;
}

[[nodiscard]] QStringList fixedFamilies()
{
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    QStringList result;
    appendUnique(result, fixed.families());
    if (result.isEmpty() && !fixed.family().isEmpty()) {
        result.append(fixed.family());
    }
    if (result.isEmpty()) {
        const QStringList families = QFontDatabase::families();
        const auto fixedPitch =
            std::ranges::find_if(families, [](const QString &family) {
                return QFontDatabase::isFixedPitch(family);
            });
        if (fixedPitch != families.end()) {
            result.append(*fixedPitch);
        } else if (!families.isEmpty()) {
            result.append(families.front());
        }
    }
    if (result.isEmpty()) {
        result.append(QStringLiteral("monospace"));
    }
    return result;
}

[[nodiscard]] bool sameFamily(const QString &left, const QString &right)
{
    return left.compare(right, Qt::CaseInsensitive) == 0;
}

[[nodiscard]] bool isGenericFamily(const QString &family)
{
    static const QStringList genericFamilies{
        QStringLiteral("serif"),     QStringLiteral("sans-serif"),
        QStringLiteral("monospace"), QStringLiteral("cursive"),
        QStringLiteral("fantasy"),   QStringLiteral("system-ui"),
        QStringLiteral("emoji"),     QStringLiteral("math"),
        QStringLiteral("fangsong"),
    };
    return genericFamilies.contains(family, Qt::CaseInsensitive);
}

class FontDatabaseSnapshot {
public:
    FontDatabaseSnapshot()
    {
        const QStringList families = QFontDatabase::families();
        canonicalFamilies_.reserve(families.size());
        for (const QString &family : families) {
            canonicalFamilies_.insert(family.toCaseFolded(), family);
        }
    }

    [[nodiscard]] std::optional<QString>
    canonicalFamily(const QString &family, double pointSize)
    {
        if (family.isEmpty()) {
            return std::nullopt;
        }
        const QString key = family.toCaseFolded();
        if (const auto cached = resolvedAliases_.constFind(key);
            cached != resolvedAliases_.cend()) {
            return *cached;
        }
        if (missingAliases_.contains(key)) {
            return std::nullopt;
        }
        const bool listed = canonicalFamilies_.contains(key);
        if (!listed && !isGenericFamily(family)
            && QFont::substitutes(family).isEmpty()) {
            missingAliases_.insert(key);
            return std::nullopt;
        }

        // Database entries can themselves be platform aliases. Resolve every
        // accepted name through QFontInfo before querying its styles.
        QFont probe = baseFont({family}, pointSize, true);
        const QString resolved = QFontInfo(probe).family();
        const auto match =
            canonicalFamilies_.constFind(resolved.toCaseFolded());
        if (match == canonicalFamilies_.cend()) {
            missingAliases_.insert(key);
            return std::nullopt;
        }
        resolvedAliases_.insert(key, *match);
        return *match;
    }

    [[nodiscard]] const QStringList &styles(const QString &family)
    {
        auto [iterator, inserted] =
            styleCache_.tryEmplace(family, QStringList{});
        if (inserted) {
            iterator.value() = QFontDatabase::styles(family);
        }
        return iterator.value();
    }

    [[nodiscard]] static QFont baseFont(const QStringList &families,
                                        double pointSize, bool fixedPitch,
                                        const TerminalFreetypeLoadFlags &flags =
                                            {})
    {
        QFont result =
            QFontDatabase::systemFont(QFontDatabase::FixedFont);
        if (!families.isEmpty()) {
            result.setFamilies(families);
        }
        result.setPointSizeF(normalizedPointSize(pointSize));
        result.setFixedPitch(fixedPitch);
        result.setStyleHint(
            fixedPitch ? QFont::Monospace : QFont::AnyStyle,
            static_cast<QFont::StyleStrategy>(
                QFont::PreferDefault | QFont::ContextFontMerging
                | (flags.monochrome ? QFont::NoAntialias
                                    : QFont::PreferAntialias)));
        result.setHintingPreference(
            !flags.hinting
                ? QFont::PreferNoHinting
                : flags.light ? QFont::PreferVerticalHinting
                              : QFont::PreferFullHinting);
        result.setStyleName({});
        result.setWeight(QFont::Normal);
        result.setStyle(QFont::StyleNormal);
        return result;
    }

private:
    QHash<QString, QString> canonicalFamilies_;
    QHash<QString, QString> resolvedAliases_;
    QSet<QString> missingAliases_;
    QHash<QString, QStringList> styleCache_;
};

[[nodiscard]] QFont baseFont(const QStringList &families, double pointSize,
                             bool fixedPitch,
                             const TerminalFreetypeLoadFlags &flags)
{
    return FontDatabaseSnapshot::baseFont(families, pointSize, fixedPitch,
                                          flags);
}

[[nodiscard]] std::optional<QFont>
fontWithNamedStyle(FontDatabaseSnapshot &database,
                   const QStringList &specificFamilies,
                   const QStringList &fallbackFamilies, const QString &style,
                   double pointSize, bool fixedPitch,
                   const TerminalFreetypeLoadFlags &flags)
{
    if (specificFamilies.isEmpty() || style.isEmpty()) {
        return std::nullopt;
    }

    for (const QString &family : specificFamilies) {
        const auto canonical = database.canonicalFamily(family, pointSize);
        if (!canonical || !database.styles(*canonical).contains(style)) {
            continue;
        }
        // Keep the user's ordered family chain on the QFont. Unknown names
        // are meaningful fallbacks and Qt will skip them when resolving.
        QStringList families = specificFamilies;
        appendUnique(families, fallbackFamilies);
        QFont result = baseFont(families, pointSize, fixedPitch, flags);
        result.setStyleName(style);
        const QFontInfo resolved(result);
        if (sameFamily(resolved.family(), *canonical)
            && resolved.styleName() == style) {
            return result;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool roleBold(TerminalFontRole role) noexcept
{
    return role == TerminalFontRole::Bold
        || role == TerminalFontRole::BoldItalic;
}

[[nodiscard]] bool roleItalic(TerminalFontRole role) noexcept
{
    return role == TerminalFontRole::Italic
        || role == TerminalFontRole::BoldItalic;
}

[[nodiscard]] bool syntheticAllowed(const TerminalSyntheticStyle &style,
                                    TerminalFontRole role) noexcept
{
    switch (role) {
    case TerminalFontRole::Regular: return true;
    case TerminalFontRole::Bold: return style.bold;
    case TerminalFontRole::Italic: return style.italic;
    case TerminalFontRole::BoldItalic: return style.boldItalic;
    case TerminalFontRole::Count: return false;
    }
    return false;
}

void applyVariations(QFont &font,
                     const QVector<TerminalFontVariation> &variations)
{
    if (variations.isEmpty()) {
        return;
    }

    const QList<QFontVariableAxis> supported = QFontInfo(font).variableAxes();
    QSet<quint32> applied;
    applied.reserve(variations.size());
    for (const TerminalFontVariation &variation : variations) {
        const double value = variation.value();
        if (applied.contains(variation.tag)) {
            continue;
        }
        // Pinned FreeType stops at the first matching tag even when that
        // setting is unusable; a later duplicate must not take its place.
        applied.insert(variation.tag);
        if (!std::isfinite(value)) {
            continue;
        }
        const auto tag = QFont::Tag::fromValue(variation.tag);
        if (!tag) {
            continue;
        }
        const auto axis = std::ranges::find_if(
            supported, [&tag](const QFontVariableAxis &candidate) {
                return candidate.tag() == *tag;
            });
        if (axis == supported.end()
            || value < axis->minimumValue()
            || value > axis->maximumValue()) {
            continue;
        }
        font.setVariableAxis(*tag, static_cast<float>(value));
    }
}

void applyFeatures(QFont &font,
                   const QVector<TerminalFontFeature> &features)
{
    for (const TerminalFontFeature &feature : features) {
        if (const auto tag = QFont::Tag::fromValue(feature.tag)) {
            // Repeated tags intentionally overwrite in order; this matches
            // Ghostty/HarfBuzz's later-feature-wins behavior.
            font.setFeature(*tag, feature.value);
        }
    }
}

[[nodiscard]] std::optional<QFont>
nativeRoleFont(FontDatabaseSnapshot &database,
               const QStringList &specificFamilies,
               const QStringList &fallbackFamilies, TerminalFontRole role,
               double pointSize, bool fixedPitch,
               const TerminalFreetypeLoadFlags &flags)
{
    for (const QString &family : specificFamilies) {
        const auto canonical = database.canonicalFamily(family, pointSize);
        if (!canonical) {
            continue;
        }
        const QStringList &styles = database.styles(*canonical);
        const auto style = std::ranges::find_if(
            styles, [&](const QString &candidate) {
                return QFontDatabase::bold(*canonical, candidate)
                        == roleBold(role)
                    && QFontDatabase::italic(*canonical, candidate)
                        == roleItalic(role);
            });
        if (style == styles.end()) {
            continue;
        }

        // Preserve the configured fallback chain even though style probing
        // uses the concrete database family.
        QStringList families = specificFamilies;
        appendUnique(families, fallbackFamilies);
        QFont result = baseFont(families, pointSize, fixedPitch, flags);
        result.setStyleName(*style);
        const QFontInfo resolved(result);
        if (sameFamily(resolved.family(), *canonical)
            && resolved.styleName() == *style) {
            return result;
        }
    }
    return std::nullopt;
}

[[nodiscard]] QFont syntheticRoleFont(QStringList families,
                                      const QStringList &fallbackFamilies,
                                      TerminalFontRole role, double pointSize,
                                      bool fixedPitch,
                                      const TerminalFreetypeLoadFlags &flags)
{
    appendUnique(families, fallbackFamilies);
    QFont result = baseFont(families, pointSize, fixedPitch, flags);
    result.setBold(roleBold(role));
    result.setItalic(roleItalic(role));
    return result;
}

[[nodiscard]] QFont
resolveRoleFont(FontDatabaseSnapshot &database,
                const TerminalFontFace &face,
                const QStringList &fallbackFamilies,
                const QFont &regular, TerminalFontRole role, double pointSize,
                bool fixedPitch, const TerminalSyntheticStyle &syntheticStyle,
                const TerminalFreetypeLoadFlags &flags)
{
    const QStringList specificFamilies = requestedFamilies(face.families);
    QFont result = std::visit(
        [&](const auto &style) -> QFont {
            using Style = std::decay_t<decltype(style)>;
            if constexpr (std::same_as<Style,
                                       TerminalFontStyles::Disabled>) {
                return regular;
            } else if constexpr (std::same_as<
                                     Style, TerminalFontStyles::Named>) {
                if (const auto named = fontWithNamedStyle(
                        database, specificFamilies, fallbackFamilies,
                        style.name, pointSize, fixedPitch, flags)) {
                    return *named;
                }
                // An explicit named style is an exact request. Ghostty does
                // not reinterpret a typo as permission to synthesize a
                // different face.
                return regular;
            } else {
                if (const auto native = nativeRoleFont(
                        database, specificFamilies, fallbackFamilies, role,
                        pointSize, fixedPitch, flags)) {
                    return *native;
                }
                if (!face.variations.isEmpty()) {
                    QStringList families = specificFamilies;
                    appendUnique(families, fallbackFamilies);
                    return baseFont(families, pointSize, fixedPitch, flags);
                }
                return syntheticAllowed(syntheticStyle, role)
                    ? syntheticRoleFont(specificFamilies, fallbackFamilies,
                                        role, pointSize, fixedPitch, flags)
                    : regular;
            }
        },
        face.style);
    applyVariations(result, face.variations);
    return result;
}

[[nodiscard]] std::array<QFont, terminalEnumIndex(TerminalFontRole::Count)>
resolveRoleFonts(FontDatabaseSnapshot &database,
                 const TerminalTypography &typography,
                 QStringList regularFamilies, bool fixedPitch)
{
    const double pointSize = normalizedPointSize(typography.pointSize);
    const QStringList configuredRegularFamilies = regularFamilies;
    if (fixedPitch) {
        appendUnique(regularFamilies, fixedFamilies());
    }

    QFont regular = baseFont(regularFamilies, pointSize, fixedPitch,
                             typography.freetypeLoadFlags);
    const TerminalFontFace &regularFace =
        typography.face(TerminalFontRole::Regular);
    if (const auto *named =
            std::get_if<TerminalFontStyles::Named>(&regularFace.style)) {
        if (const auto resolved = fontWithNamedStyle(
                database, configuredRegularFamilies, regularFamilies,
                named->name, pointSize, fixedPitch,
                typography.freetypeLoadFlags)) {
            regular = *resolved;
        }
    }
    applyVariations(regular, regularFace.variations);

    std::array<QFont, terminalEnumIndex(TerminalFontRole::Count)> result;
    result[terminalEnumIndex(TerminalFontRole::Regular)] = regular;
    for (const TerminalFontRole role :
         {TerminalFontRole::Bold, TerminalFontRole::Italic,
          TerminalFontRole::BoldItalic}) {
        result[terminalEnumIndex(role)] = resolveRoleFont(
            database, typography.face(role), regularFamilies, regular, role,
            pointSize, fixedPitch, typography.syntheticStyle,
            typography.freetypeLoadFlags);
    }
    return result;
}

[[nodiscard]] std::optional<quint32> firstCodepoint(QStringView text) noexcept
{
    if (text.isEmpty()) {
        return std::nullopt;
    }
    const QChar first = text.front();
    if (first.isHighSurrogate() && text.size() > 1
        && text.at(1).isLowSurrogate()) {
        return QChar::surrogateToUcs4(first, text.at(1));
    }
    return first.unicode();
}

[[nodiscard]] bool supportsCompleteText(const QRawFont &font,
                                        QStringView text) noexcept
{
    if (!font.isValid()) {
        return false;
    }
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar current = text.at(index);
        quint32 codepoint = current.unicode();
        if (current.isHighSurrogate() && index + 1 < text.size()
            && text.at(index + 1).isLowSurrogate()) {
            codepoint = QChar::surrogateToUcs4(current, text.at(++index));
        }
        if (codepoint == 0x200dU || codepoint == 0xfe0eU
            || codepoint == 0xfe0fU) {
            continue;
        }
        if (!font.supportsCharacter(codepoint)) {
            return false;
        }
    }
    return true;
}

void overlayMappedFont(QVector<TerminalMappedFont> &segments,
                       TerminalMappedFont mapping)
{
    QVector<TerminalMappedFont> next;
    next.reserve(segments.size() + 2);
    bool inserted = false;
    for (const TerminalMappedFont &existing : std::as_const(segments)) {
        if (existing.range.last < mapping.range.first) {
            next.append(existing);
            continue;
        }
        if (mapping.range.last < existing.range.first) {
            if (!inserted) {
                next.append(mapping);
                inserted = true;
            }
            next.append(existing);
            continue;
        }

        if (existing.range.first < mapping.range.first) {
            TerminalMappedFont left = existing;
            left.range.last = mapping.range.first - 1;
            next.append(std::move(left));
        }
        if (mapping.range.last < existing.range.last) {
            if (!inserted) {
                next.append(mapping);
                inserted = true;
            }
            TerminalMappedFont right = existing;
            right.range.first = mapping.range.last + 1;
            next.append(std::move(right));
        }
    }
    if (!inserted) {
        next.append(std::move(mapping));
    }
    segments = std::move(next);
}

} // namespace

TerminalResolvedFonts
resolveTerminalFonts(const TerminalTypography &typography)
{
    FontDatabaseSnapshot database;
    TerminalResolvedFonts result;
    result.metricFonts = resolveRoleFonts(
        database, typography,
        requestedFamilies(
            typography.face(TerminalFontRole::Regular).families),
        true);
    result.fonts = result.metricFonts;
    for (QFont &font : result.fonts) {
        applyFeatures(font, typography.features);
    }

    result.mappedFonts.reserve(typography.codepointMap.size());
    for (const TerminalCodepointFontMap &mapping : typography.codepointMap) {
        TerminalMappedFont resolved;
        resolved.range = mapping;
        if (const auto canonical =
                database.canonicalFamily(mapping.family,
                                         typography.pointSize)) {
            resolved.font = baseFont(
                {*canonical}, typography.pointSize, false,
                typography.freetypeLoadFlags);
            applyFeatures(resolved.font, typography.features);
            resolved.rawFont = QRawFont::fromFont(resolved.font);
        }
        // Normalize overlapping declarations once. The resulting table is
        // sorted and disjoint, with each point owned by the latest config
        // entry, including an entry whose face could not be resolved.
        overlayMappedFont(result.mappedFonts, std::move(resolved));
    }
    return result;
}

const QFont &terminalFontForText(const TerminalResolvedFonts &fonts,
                                 TerminalFontRole role,
                                 QStringView text) noexcept
{
    return terminalFontForText(fonts.fonts, fonts.mappedFonts, role, text);
}

const QFont &terminalFontForText(
    const std::array<QFont, terminalEnumIndex(TerminalFontRole::Count)> &fonts,
    const QVector<TerminalMappedFont> &mappedFonts, TerminalFontRole role,
    QStringView text) noexcept
{
    const auto codepoint = firstCodepoint(text);
    if (!codepoint) {
        return fonts[terminalEnumIndex(role)];
    }

    const auto after = std::ranges::upper_bound(
        mappedFonts, *codepoint, {}, [](const TerminalMappedFont &mapping) {
            return mapping.range.first;
        });
    if (after != mappedFonts.cbegin()) {
        const TerminalMappedFont &mapping = *std::prev(after);
        if (*codepoint <= mapping.range.last) {
            return supportsCompleteText(mapping.rawFont, text)
                ? mapping.font
                : fonts[terminalEnumIndex(role)];
        }
    }
    return fonts[terminalEnumIndex(role)];
}
