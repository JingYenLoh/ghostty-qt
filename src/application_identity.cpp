#include "application_identity.h"

#include <QtGlobal>

namespace {

constexpr qsizetype MaximumApplicationIdBytes = 255;

constexpr bool isAsciiLetter(uchar character) noexcept
{
    return (character >= 'A' && character <= 'Z')
        || (character >= 'a' && character <= 'z');
}

constexpr bool isAsciiDigit(uchar character) noexcept
{
    return character >= '0' && character <= '9';
}

constexpr bool isComponentInitial(uchar character) noexcept
{
    return isAsciiLetter(character) || character == '_' || character == '-';
}

constexpr bool isComponentCharacter(uchar character) noexcept
{
    return isComponentInitial(character) || isAsciiDigit(character);
}

QString quotedBytes(QByteArrayView value)
{
    QString quoted;
    quoted.reserve(value.size() + 2);
    quoted += u'"';
    constexpr char HexDigits[] = "0123456789abcdef";
    for (const char rawCharacter : value) {
        const uchar character = static_cast<uchar>(rawCharacter);
        if (character >= 0x20 && character <= 0x7e && character != '"'
            && character != '\\') {
            quoted += QChar(character);
            continue;
        }
        if (character == '"' || character == '\\') {
            quoted += u'\\';
            quoted += QChar(character);
            continue;
        }
        quoted += QStringLiteral("\\x");
        quoted += QLatin1Char(HexDigits[character >> 4]);
        quoted += QLatin1Char(HexDigits[character & 0x0f]);
    }
    quoted += u'"';
    return quoted;
}

std::expected<ApplicationIdentityResolution, QString>
resolveBytes(const std::optional<QByteArray> &configuredClass,
             QStringView buildFallback)
{
    const QByteArray fallbackBytes = buildFallback.toUtf8();
    if (!isValidApplicationId(fallbackBytes)) {
        return std::unexpected(
            QStringLiteral("The build application ID %1 is not a valid "
                           "GApplication application ID")
                .arg(quotedBytes(fallbackBytes)));
    }

    const QString fallback = buildFallback.toString();
    if (!configuredClass.has_value()) {
        return ApplicationIdentityResolution{
            .applicationId = fallback,
            .diagnostic = std::nullopt,
        };
    }
    if (isValidApplicationId(*configuredClass)) {
        return ApplicationIdentityResolution{
            .applicationId = QString::fromLatin1(*configuredClass),
            .diagnostic = std::nullopt,
        };
    }
    return ApplicationIdentityResolution{
        .applicationId = fallback,
        .diagnostic =
            QStringLiteral("Ignoring invalid Ghostty class %1; using "
                           "application ID %2")
                .arg(quotedBytes(*configuredClass), quotedBytes(fallbackBytes)),
    };
}

} // namespace

bool isValidApplicationId(QByteArrayView applicationId) noexcept
{
    if (applicationId.isEmpty()
        || applicationId.size() > MaximumApplicationIdBytes) {
        return false;
    }

    bool componentInitial = true;
    bool hasSeparator = false;
    for (const char rawCharacter : applicationId) {
        const uchar character = static_cast<uchar>(rawCharacter);
        if (character == '.') {
            if (componentInitial) return false;
            componentInitial = true;
            hasSeparator = true;
            continue;
        }
        if (componentInitial ? !isComponentInitial(character)
                             : !isComponentCharacter(character)) {
            return false;
        }
        componentInitial = false;
    }
    return hasSeparator && !componentInitial;
}

std::expected<ApplicationIdentityResolution, QString>
resolveApplicationIdentity(const std::optional<QByteArray> &configuredClass,
                           QStringView buildFallback)
{
    return resolveBytes(configuredClass, buildFallback);
}

std::expected<ApplicationIdentityResolution, QString>
resolveApplicationIdentity(const std::optional<QString> &configuredClass,
                           QStringView buildFallback)
{
    if (!configuredClass.has_value()) {
        return resolveBytes(std::nullopt, buildFallback);
    }
    return resolveBytes(configuredClass->toUtf8(), buildFallback);
}
