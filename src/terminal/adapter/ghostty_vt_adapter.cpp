#include "terminal/adapter/ghostty_vt_adapter.h"

#include "input/ghostty_key_identity.h"
#include "terminal/adapter/terminal_kitty_image_materialization.h"

#include <ghostty/vt.h>

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QImageReader>
#include <QScopeGuard>
#include <QSet>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstring>
#include <expected>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>
#include <unistd.h>
#include <utility>

namespace {

struct AdapterOwnerToken final {};

struct ScreenCell final {
    uint16_t x = 0;
    uint32_t y = 0;

    friend constexpr std::strong_ordering operator<=>(const ScreenCell &left,
                                                      const ScreenCell &right)
    {
        if (const auto rowOrder = left.y <=> right.y; rowOrder != 0) {
            return rowOrder;
        }
        return left.x <=> right.x;
    }

    friend bool operator==(const ScreenCell &, const ScreenCell &) = default;
};

template <typename Coordinate> struct BasicTextMapData final {
    QByteArray text;
    QVector<Coordinate> byteCells;
};

using TextMapData = BasicTextMapData<ScreenCell>;
using SearchRowTextMapData = BasicTextMapData<quint16>;

constexpr quint64 maximumLogicalLineCells = 131'072;
constexpr qsizetype maximumLogicalLineBytes = 4 * 1024 * 1024;
constexpr size_t maximumClipboardRepresentations = 256;
constexpr qsizetype maximumPendingClipboardWrites = 64;
constexpr quint64 maximumPendingClipboardBytes = 64 * 1024 * 1024;
constexpr qsizetype maximumPendingDesktopNotifications = 64;
constexpr size_t maximumPendingDesktopNotificationBytes = 1024 * 1024;
constexpr qsizetype maximumPendingProgressReports = 256;
constexpr uint32_t kittyUnicodePlaceholder = 0x10eeeeU;
constexpr int kittyMaximumDecodedImageMegabytes = 400;

bool appendFormatterBytes(void *userdata, const uint8_t *data,
                          size_t length) noexcept
{
    auto *output = static_cast<QByteArray *>(userdata);
    if (output == nullptr || data == nullptr || length == 0
        || length > static_cast<size_t>(QByteArray::maxSize())
        || static_cast<size_t>(output->size())
            > static_cast<size_t>(QByteArray::maxSize()) - length) {
        return false;
    }

    try {
        output->append(reinterpret_cast<const char *>(data),
                       static_cast<qsizetype>(length));
    } catch (...) {
        return false;
    }
    return true;
}

[[nodiscard]] std::optional<QByteArray>
formatToByteArray(GhosttyFormatter formatter)
{
    QByteArray output;
    const GhosttyWriter writer{
        .write = &appendFormatterBytes,
        .userdata = &output,
    };
    if (ghostty_formatter_format(formatter, writer) != GHOSTTY_SUCCESS) {
        return std::nullopt;
    }
    return output;
}

[[nodiscard]] constexpr bool isUnicodeScalar(uint32_t codepoint) noexcept
{
    return codepoint <= 0x10ffffU
        && !(codepoint >= 0xd800U && codepoint <= 0xdfffU);
}

[[nodiscard]] QString singleCodepointText(uint32_t codepoint)
{
    constexpr uint32_t firstPrintableAscii = 0x20U;
    constexpr uint32_t lastPrintableAscii = 0x7eU;
    static constexpr auto printableAscii = [] {
        std::array<char16_t, lastPrintableAscii - firstPrintableAscii + 1U>
            characters{};
        for (size_t index = 0; index < characters.size(); ++index) {
            characters[index] =
                static_cast<char16_t>(firstPrintableAscii + index);
        }
        return characters;
    }();

    if (codepoint >= firstPrintableAscii && codepoint <= lastPrintableAscii) {
        return QString::fromRawData(
            printableAscii.data() + (codepoint - firstPrintableAscii), 1);
    }

    const char32_t scalar = static_cast<char32_t>(codepoint);
    return QString::fromUcs4(&scalar, 1);
}

bool decodeKittyPng(void *, const GhosttyAllocator *allocator,
                    const uint8_t *data, size_t dataLength,
                    GhosttySysImage *output)
{
    if (output == nullptr || (data == nullptr && dataLength != 0)
        || dataLength > static_cast<size_t>(QByteArray::maxSize())) {
        return false;
    }

    const QByteArray encoded = dataLength == 0
        ? QByteArray{}
        : QByteArray::fromRawData(reinterpret_cast<const char *>(data),
                                  static_cast<qsizetype>(dataLength));
    QBuffer buffer;
    buffer.setData(encoded);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return false;
    }
    QImageReader reader(&buffer, QByteArrayLiteral("png"));
    reader.setAutoTransform(false);
    QImage image = reader.read().convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return false;
    }

    const quint64 rowBytes = static_cast<quint64>(image.width()) * 4U;
    const quint64 byteCount = rowBytes * static_cast<quint64>(image.height());
    if (byteCount > static_cast<quint64>(std::numeric_limits<size_t>::max())) {
        return false;
    }
    auto *pixels = static_cast<uint8_t *>(
        ghostty_alloc(allocator, static_cast<size_t>(byteCount)));
    if (pixels == nullptr) {
        return false;
    }
    for (int row = 0; row < image.height(); ++row) {
        std::memcpy(
            pixels + static_cast<size_t>(row) * static_cast<size_t>(rowBytes),
            image.constScanLine(row), static_cast<size_t>(rowBytes));
    }

    output->width = static_cast<uint32_t>(image.width());
    output->height = static_cast<uint32_t>(image.height());
    output->data = pixels;
    output->data_len = static_cast<size_t>(byteCount);
    return true;
}

bool installKittyPngDecoder()
{
    static std::once_flag once;
    static bool installed = false;
    std::call_once(once, [] {
        // Ghostty and Kitty accept decoded payloads up to 400 MiB. Qt's
        // process-wide default is currently lower, so leaving it unchanged
        // would reject protocol-valid images before Ghostty applies its own
        // dimension, decoded-size, and storage-budget checks.
        if (QImageReader::allocationLimit()
            < kittyMaximumDecodedImageMegabytes) {
            QImageReader::setAllocationLimit(kittyMaximumDecodedImageMegabytes);
        }
        installed =
            ghostty_sys_set(GHOSTTY_SYS_OPT_DECODE_PNG,
                            reinterpret_cast<const void *>(&decodeKittyPng))
            == GHOSTTY_SUCCESS;
    });
    return installed;
}

TerminalKittyGraphicsLayer kittyLayer(qint32 z) noexcept
{
    constexpr qint32 backgroundLimit = std::numeric_limits<qint32>::min() / 2;
    if (z < backgroundLimit) {
        return TerminalKittyGraphicsLayer::BelowBackground;
    }
    if (z < 0) {
        return TerminalKittyGraphicsLayer::BelowText;
    }
    return TerminalKittyGraphicsLayer::AboveText;
}

struct KittyImageView final {
    quint32 imageId = 0;
    quint32 width = 0;
    quint32 height = 0;
    GhosttyKittyImageFormat format = GHOSTTY_KITTY_IMAGE_FORMAT_RGBA;
    const uint8_t *pixels = nullptr;
    size_t dataLength = 0;
    quint64 generation = 0;
    quint64 bytesPerPixel = 0;
};

std::optional<KittyImageView> queryKittyImage(GhosttyKittyGraphicsImage image,
                                              quint32 expectedImageId)
{
    KittyImageView result;
    GhosttyKittyImageCompression compression =
        GHOSTTY_KITTY_IMAGE_COMPRESSION_NONE;
    constexpr std::array keys{
        GHOSTTY_KITTY_IMAGE_DATA_ID,
        GHOSTTY_KITTY_IMAGE_DATA_WIDTH,
        GHOSTTY_KITTY_IMAGE_DATA_HEIGHT,
        GHOSTTY_KITTY_IMAGE_DATA_FORMAT,
        GHOSTTY_KITTY_IMAGE_DATA_COMPRESSION,
        GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR,
        GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN,
        GHOSTTY_KITTY_IMAGE_DATA_GENERATION,
    };
    std::array<void *, keys.size()> values{
        &result.imageId, &result.width,  &result.height,     &result.format,
        &compression,    &result.pixels, &result.dataLength, &result.generation,
    };
    if (ghostty_kitty_graphics_image_get_multi(image, keys.size(), keys.data(),
                                               values.data(), nullptr)
            != GHOSTTY_SUCCESS
        || result.imageId != expectedImageId || result.generation == 0
        || result.width == 0 || result.height == 0
        || result.width > static_cast<quint32>(INT_MAX)
        || result.height > static_cast<quint32>(INT_MAX)
        || compression != GHOSTTY_KITTY_IMAGE_COMPRESSION_NONE) {
        return std::nullopt;
    }

    switch (result.format) {
    case GHOSTTY_KITTY_IMAGE_FORMAT_RGB: result.bytesPerPixel = 3; break;
    case GHOSTTY_KITTY_IMAGE_FORMAT_RGBA: result.bytesPerPixel = 4; break;
    case GHOSTTY_KITTY_IMAGE_FORMAT_GRAY_ALPHA: result.bytesPerPixel = 2; break;
    case GHOSTTY_KITTY_IMAGE_FORMAT_GRAY: result.bytesPerPixel = 1; break;
    default: return std::nullopt;
    }
    const quint64 pixelCount =
        static_cast<quint64>(result.width) * result.height;
    if (pixelCount > std::numeric_limits<quint64>::max() / result.bytesPerPixel
        || pixelCount * result.bytesPerPixel != result.dataLength
        || (result.dataLength != 0 && result.pixels == nullptr)) {
        return std::nullopt;
    }
    return result;
}

bool minimumContrastExemptGlyph(uint32_t codepoint)
{
    return (codepoint >= 0x2500 && codepoint <= 0x257f)
        || (codepoint >= 0x2580 && codepoint <= 0x259f)
        || (codepoint >= 0x1fb00 && codepoint <= 0x1fbff)
        || (codepoint >= 0x1cc00 && codepoint <= 0x1cebf)
        || (codepoint >= 0xe0b0 && codepoint <= 0xe0d7);
}

bool isPowerlinePaddingGlyph(uint32_t codepoint)
{
    return (codepoint >= 0xe0b0 && codepoint <= 0xe0c8) || codepoint == 0xe0ca
        || (codepoint >= 0xe0cc && codepoint <= 0xe0d2) || codepoint == 0xe0d4;
}

bool sameRgb(const GhosttyColorRgb &left, const GhosttyColorRgb &right)
{
    return left.r == right.r && left.g == right.g && left.b == right.b;
}

bool isMacAddress(std::string_view value)
{
    if (value.size() != 17) {
        return false;
    }

    for (std::size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (index % 3 == 2) {
            if (character != ':') {
                return false;
            }
        } else if (!((character >= '0' && character <= '9')
                     || (character >= 'A' && character <= 'F')
                     || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::optional<quint16> parseZigPort(std::string_view value)
{
    if (value.empty()) {
        return std::nullopt;
    }

    bool negative = false;
    if (value.front() == '+' || value.front() == '-') {
        negative = value.front() == '-';
        value.remove_prefix(1);
    }
    if (value.empty() || value.front() == '_' || value.back() == '_') {
        return std::nullopt;
    }

    quint32 result = 0;
    for (const char character : value) {
        if (character == '_') {
            continue;
        }
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        const quint32 digit = static_cast<quint32>(character - '0');
        if (negative) {
            if (digit != 0) {
                return std::nullopt;
            }
            continue;
        }
        if (result > (std::numeric_limits<quint16>::max() - digit) / 10) {
            return std::nullopt;
        }
        result = result * 10 + digit;
    }
    return static_cast<quint16>(result);
}

int hexDigit(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

std::optional<quint8> parseZigHexByte(char first, char second)
{
    if (first == '+') {
        const int digit = hexDigit(second);
        return digit >= 0 ? std::optional<quint8>(digit) : std::nullopt;
    }
    if (first == '-') {
        return second == '0' ? std::optional<quint8>(0) : std::nullopt;
    }
    const int high = hexDigit(first);
    const int low = hexDigit(second);
    if (high < 0 || low < 0) {
        return std::nullopt;
    }
    return static_cast<quint8>(high * 16 + low);
}

QByteArray percentDecode(std::string_view value)
{
    QByteArray decoded;
    decoded.reserve(static_cast<qsizetype>(value.size()));
    for (std::size_t index = 0; index < value.size();) {
        if (value[index] == '%' && index + 2 < value.size()) {
            if (const auto byte =
                    parseZigHexByte(value[index + 1], value[index + 2])) {
                decoded.append(static_cast<char>(*byte));
                index += 3;
                continue;
            }
        }
        decoded.append(value[index]);
        ++index;
    }
    return decoded;
}

enum class Osc7ParseError {
    InvalidFormat,
    InvalidPort,
};

struct ParsedOsc7Uri {
    std::optional<std::string_view> host;
    std::string_view path;
    std::optional<quint16> port;
    std::size_t hostStart = 0;
    std::size_t pathStart = 0;
};

std::expected<ParsedOsc7Uri, Osc7ParseError>
parseOsc7AfterScheme(std::string_view value, std::size_t textStart)
{
    ParsedOsc7Uri result;
    std::size_t pathStart = textStart;
    if (value.substr(textStart).starts_with("//")) {
        const std::size_t authorityStart = textStart + 2;
        const std::size_t delimiter =
            value.find_first_of("/?#", authorityStart);
        const std::size_t authorityEnd =
            delimiter == std::string_view::npos ? value.size() : delimiter;
        const std::string_view authority =
            value.substr(authorityStart, authorityEnd - authorityStart);
        pathStart = authorityEnd;
        if (authority.empty()) {
            if (authorityStart >= value.size()
                || value[authorityStart] != '/') {
                return std::unexpected(Osc7ParseError::InvalidFormat);
            }
        } else {
            std::size_t hostStart = 0;
            if (const std::size_t userInfoEnd = authority.find('@');
                userInfoEnd != std::string_view::npos) {
                hostStart = userInfoEnd + 1;
            }
            if (hostStart < authority.size()) {
                std::size_t hostEnd = authority.size();
                if (authority[hostStart] == ']') {
                    return std::unexpected(Osc7ParseError::InvalidFormat);
                }
                if (authority[hostStart] == '[') {
                    const std::size_t closingBracket = authority.rfind(']');
                    if (closingBracket == std::string_view::npos) {
                        return std::unexpected(Osc7ParseError::InvalidFormat);
                    }
                    hostEnd = closingBracket + 1;
                    const std::size_t lastColon = authority.rfind(':');
                    if (lastColon != std::string_view::npos
                        && lastColon >= hostEnd) {
                        const auto port =
                            parseZigPort(authority.substr(lastColon + 1));
                        if (!port) {
                            return std::unexpected(Osc7ParseError::InvalidPort);
                        }
                        result.port = *port;
                    }
                } else if (const std::size_t lastColon = authority.rfind(':');
                           lastColon != std::string_view::npos
                           && lastColon >= hostStart) {
                    hostEnd = lastColon;
                    const auto port =
                        parseZigPort(authority.substr(lastColon + 1));
                    if (!port) {
                        return std::unexpected(Osc7ParseError::InvalidPort);
                    }
                    result.port = *port;
                }
                if (hostStart >= hostEnd) {
                    return std::unexpected(Osc7ParseError::InvalidFormat);
                }
                result.hostStart = authorityStart + hostStart;
                result.host =
                    value.substr(result.hostStart, hostEnd - hostStart);
            }
        }
    }

    result.pathStart = pathStart;
    const std::size_t delimiter = value.find_first_of("?#", pathStart);
    const std::size_t pathEnd =
        delimiter == std::string_view::npos ? value.size() : delimiter;
    result.path = value.substr(pathStart, pathEnd - pathStart);
    return result;
}

std::optional<ParsedOsc7Uri> parseOsc7Uri(std::string_view value,
                                          std::size_t schemeEnd)
{
    auto parsed = parseOsc7AfterScheme(value, schemeEnd + 1);
    if (!parsed) {
        if (parsed.error() != Osc7ParseError::InvalidPort) {
            return std::nullopt;
        }

        const std::size_t hostStart = schemeEnd + 3;
        const std::size_t slash = value.find('/', hostStart);
        const std::size_t hostEnd =
            slash == std::string_view::npos ? value.size() : slash;
        const std::string_view macAddress =
            value.substr(hostStart, hostEnd - hostStart);
        if (!isMacAddress(macAddress)) {
            return std::nullopt;
        }

        parsed = parseOsc7AfterScheme(value, hostEnd);
        if (!parsed) {
            return std::nullopt;
        }
        parsed->hostStart = hostStart;
        parsed->host = macAddress;
    }

    if (!parsed->host) {
        return std::nullopt;
    }
    const std::string_view host = *parsed->host;
    if (host.size() == 14 && std::count(host.begin(), host.end(), ':') == 4
        && parsed->port && *parsed->port <= 99) {
        const std::string_view macAddress = value.substr(
            parsed->hostStart, parsed->pathStart - parsed->hostStart);
        if (!isMacAddress(macAddress)) {
            return std::nullopt;
        }
        parsed->host = macAddress;
        parsed->port.reset();
    }
    return *parsed;
}

QByteArray machineHostName()
{
    std::array<char, HOST_NAME_MAX + 1> buffer{};
    if (::gethostname(buffer.data(), buffer.size() - 1) != 0) {
        return {};
    }
    return QByteArray(buffer.data());
}

std::optional<QByteArray>
validatedOsc7Directory(std::string_view reported,
                       const std::function<QByteArray()> &queryMachineHostName)
{
    if (reported.empty()) {
        return QByteArrayLiteral("");
    }

    constexpr std::string_view filePrefix = "file://";
    constexpr std::string_view kittyPrefix = "kitty-shell-cwd://";
    const bool encodedFilePath = reported.starts_with(filePrefix);
    const bool rawKittyPath = reported.starts_with(kittyPrefix);
    if (!encodedFilePath && !rawKittyPath) {
        return std::nullopt;
    }
    const std::size_t schemeEnd =
        encodedFilePath ? filePrefix.size() - 3 : kittyPrefix.size() - 3;
    const auto uri = parseOsc7Uri(reported, schemeEnd);
    if (!uri) {
        return std::nullopt;
    }

    const QByteArray decodedHost = percentDecode(*uri->host);
    if (decodedHost.isEmpty() || decodedHost.size() > 255) {
        return std::nullopt;
    }
    if (decodedHost != QByteArrayLiteral("localhost")
        && decodedHost != queryMachineHostName()) {
        return std::nullopt;
    }

    if (!rawKittyPath) {
        return percentDecode(uri->path);
    }

    // Ghostty's kitty-shell-cwd variant intentionally treats the URI path as
    // raw text. Preserve literal percent sequences instead of URL-decoding
    // them a second time.
    const std::string_view path = reported.substr(uri->pathStart);
    return QByteArray(path.data(), static_cast<qsizetype>(path.size()));
}

QColor toQColor(GhosttyColorRgb color)
{
    return QColor::fromRgb(color.r, color.g, color.b);
}

std::optional<TerminalInspectorStyleColor>
inspectorStyleColor(GhosttyStyleColor color)
{
    TerminalInspectorStyleColor result;
    switch (color.tag) {
    case GHOSTTY_STYLE_COLOR_NONE: return result;
    case GHOSTTY_STYLE_COLOR_PALETTE:
        result.kind = TerminalInspectorStyleColorKind::Palette;
        result.paletteIndex = color.value.palette;
        return result;
    case GHOSTTY_STYLE_COLOR_RGB:
        result.kind = TerminalInspectorStyleColorKind::Rgb;
        result.rgb = toQColor(color.value.rgb);
        return result;
    default: return std::nullopt;
    }
}

std::optional<TerminalInspectorUnderlineStyle>
inspectorUnderlineStyle(int underline)
{
    switch (underline) {
    case GHOSTTY_SGR_UNDERLINE_NONE:
        return TerminalInspectorUnderlineStyle::None;
    case GHOSTTY_SGR_UNDERLINE_SINGLE:
        return TerminalInspectorUnderlineStyle::Single;
    case GHOSTTY_SGR_UNDERLINE_DOUBLE:
        return TerminalInspectorUnderlineStyle::Double;
    case GHOSTTY_SGR_UNDERLINE_CURLY:
        return TerminalInspectorUnderlineStyle::Curly;
    case GHOSTTY_SGR_UNDERLINE_DOTTED:
        return TerminalInspectorUnderlineStyle::Dotted;
    case GHOSTTY_SGR_UNDERLINE_DASHED:
        return TerminalInspectorUnderlineStyle::Dashed;
    default: return std::nullopt;
    }
}

GhosttyColorRgb toGhosttyColor(const QColor &color)
{
    const QColor rgb = color.toRgb();
    return GhosttyColorRgb{
        static_cast<uint8_t>(rgb.red()),
        static_cast<uint8_t>(rgb.green()),
        static_cast<uint8_t>(rgb.blue()),
    };
}

struct InspectorModeSpec {
    GhosttyMode mode;
    const char *name;
};

const std::array inspectorModeSpecs{
    InspectorModeSpec{GHOSTTY_MODE_KAM, "KAM"},
    InspectorModeSpec{GHOSTTY_MODE_INSERT, "IRM"},
    InspectorModeSpec{GHOSTTY_MODE_SRM, "SRM"},
    InspectorModeSpec{GHOSTTY_MODE_LINEFEED, "LNM"},
    InspectorModeSpec{GHOSTTY_MODE_DECCKM, "DECCKM"},
    InspectorModeSpec{GHOSTTY_MODE_132_COLUMN, "DECCOLM"},
    InspectorModeSpec{GHOSTTY_MODE_SLOW_SCROLL, "DECSCLM"},
    InspectorModeSpec{GHOSTTY_MODE_REVERSE_COLORS, "DECSCNM"},
    InspectorModeSpec{GHOSTTY_MODE_ORIGIN, "DECOM"},
    InspectorModeSpec{GHOSTTY_MODE_WRAPAROUND, "DECAWM"},
    InspectorModeSpec{GHOSTTY_MODE_AUTOREPEAT, "DECARM"},
    InspectorModeSpec{GHOSTTY_MODE_X10_MOUSE, "X10 mouse"},
    InspectorModeSpec{GHOSTTY_MODE_CURSOR_BLINKING, "Cursor blinking"},
    InspectorModeSpec{GHOSTTY_MODE_CURSOR_VISIBLE, "DECTCEM"},
    InspectorModeSpec{GHOSTTY_MODE_ENABLE_MODE_3, "Allow DECCOLM"},
    InspectorModeSpec{GHOSTTY_MODE_REVERSE_WRAP, "Reverse wrap"},
    InspectorModeSpec{GHOSTTY_MODE_ALT_SCREEN_LEGACY, "Alternate screen (47)"},
    InspectorModeSpec{GHOSTTY_MODE_KEYPAD_KEYS, "Application keypad"},
    InspectorModeSpec{GHOSTTY_MODE_BACKARROW_KEY_MODE, "DECBKM"},
    InspectorModeSpec{GHOSTTY_MODE_LEFT_RIGHT_MARGIN, "DECLRMM"},
    InspectorModeSpec{GHOSTTY_MODE_NORMAL_MOUSE, "Normal mouse"},
    InspectorModeSpec{GHOSTTY_MODE_BUTTON_MOUSE, "Button-event mouse"},
    InspectorModeSpec{GHOSTTY_MODE_ANY_MOUSE, "Any-event mouse"},
    InspectorModeSpec{GHOSTTY_MODE_FOCUS_EVENT, "Focus events"},
    InspectorModeSpec{GHOSTTY_MODE_UTF8_MOUSE, "UTF-8 mouse"},
    InspectorModeSpec{GHOSTTY_MODE_SGR_MOUSE, "SGR mouse"},
    InspectorModeSpec{GHOSTTY_MODE_ALT_SCROLL, "Alternate scroll"},
    InspectorModeSpec{GHOSTTY_MODE_URXVT_MOUSE, "URXVT mouse"},
    InspectorModeSpec{GHOSTTY_MODE_SGR_PIXELS_MOUSE, "SGR pixel mouse"},
    InspectorModeSpec{GHOSTTY_MODE_NUMLOCK_KEYPAD, "NumLock keypad"},
    InspectorModeSpec{GHOSTTY_MODE_ALT_ESC_PREFIX, "Alt ESC prefix"},
    InspectorModeSpec{GHOSTTY_MODE_ALT_SENDS_ESC, "Alt sends ESC"},
    InspectorModeSpec{GHOSTTY_MODE_REVERSE_WRAP_EXT, "Extended reverse wrap"},
    InspectorModeSpec{GHOSTTY_MODE_ALT_SCREEN, "Alternate screen (1047)"},
    InspectorModeSpec{GHOSTTY_MODE_SAVE_CURSOR, "Save cursor"},
    InspectorModeSpec{GHOSTTY_MODE_ALT_SCREEN_SAVE,
                      "Alternate screen + save cursor (1049)"},
    InspectorModeSpec{GHOSTTY_MODE_BRACKETED_PASTE, "Bracketed paste"},
    InspectorModeSpec{GHOSTTY_MODE_SYNC_OUTPUT, "Synchronized output"},
    InspectorModeSpec{GHOSTTY_MODE_GRAPHEME_CLUSTER, "Grapheme clusters"},
    InspectorModeSpec{GHOSTTY_MODE_COLOR_SCHEME_REPORT, "Color-scheme reports"},
    InspectorModeSpec{GHOSTTY_MODE_VISIBILITY_REPORT, "Visibility reports"},
    InspectorModeSpec{GHOSTTY_MODE_IN_BAND_RESIZE, "In-band resize"},
};

GhosttyResult terminalModeGet(GhosttyTerminal terminal, GhosttyMode mode,
                              bool *value)
{
    if (value == nullptr) return GHOSTTY_INVALID_VALUE;

    GhosttyTerminalModeConfig config{
        .mode = mode,
        .value = false,
    };
    const GhosttyResult result =
        ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_MODE, &config);
    if (result == GHOSTTY_SUCCESS) *value = config.value;
    return result;
}

GhosttyResult terminalModeSet(GhosttyTerminal terminal, GhosttyMode mode,
                              bool value)
{
    const GhosttyTerminalModeConfig config{
        .mode = mode,
        .value = value,
    };
    return ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_MODE, &config);
}

GhosttyResult terminalModeSetDefault(GhosttyTerminal terminal, GhosttyMode mode,
                                     bool value)
{
    const GhosttyTerminalModeConfig config{
        .mode = mode,
        .value = value,
    };
    return ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_MODE_DEFAULT,
                                &config);
}

constexpr GhosttyColorScheme
toGhosttyColorScheme(TerminalColorScheme scheme) noexcept
{
    return scheme == TerminalColorScheme::Dark ? GHOSTTY_COLOR_SCHEME_DARK
                                               : GHOSTTY_COLOR_SCHEME_LIGHT;
}

GhosttyTerminalCursorStyle toGhosttyCursorStyle(TerminalCursorStyle style)
{
    switch (style) {
    case TerminalCursorStyle::Block: return GHOSTTY_TERMINAL_CURSOR_STYLE_BLOCK;
    case TerminalCursorStyle::Bar: return GHOSTTY_TERMINAL_CURSOR_STYLE_BAR;
    case TerminalCursorStyle::Underline:
        return GHOSTTY_TERMINAL_CURSOR_STYLE_UNDERLINE;
    case TerminalCursorStyle::BlockHollow:
        return GHOSTTY_TERMINAL_CURSOR_STYLE_BLOCK_HOLLOW;
    }
    return GHOSTTY_TERMINAL_CURSOR_STYLE_BLOCK;
}

bool toGhosttySelectionAdjustment(TerminalSelectionAdjustment adjustment,
                                  GhosttySelectionAdjust *out)
{
    if (out == nullptr) return false;

    switch (adjustment) {
    case TerminalSelectionAdjustment::Left:
        *out = GHOSTTY_SELECTION_ADJUST_LEFT;
        return true;
    case TerminalSelectionAdjustment::Right:
        *out = GHOSTTY_SELECTION_ADJUST_RIGHT;
        return true;
    case TerminalSelectionAdjustment::Up:
        *out = GHOSTTY_SELECTION_ADJUST_UP;
        return true;
    case TerminalSelectionAdjustment::Down:
        *out = GHOSTTY_SELECTION_ADJUST_DOWN;
        return true;
    case TerminalSelectionAdjustment::PageUp:
        *out = GHOSTTY_SELECTION_ADJUST_PAGE_UP;
        return true;
    case TerminalSelectionAdjustment::PageDown:
        *out = GHOSTTY_SELECTION_ADJUST_PAGE_DOWN;
        return true;
    case TerminalSelectionAdjustment::Home:
        *out = GHOSTTY_SELECTION_ADJUST_HOME;
        return true;
    case TerminalSelectionAdjustment::End:
        *out = GHOSTTY_SELECTION_ADJUST_END;
        return true;
    case TerminalSelectionAdjustment::BeginningOfLine:
        *out = GHOSTTY_SELECTION_ADJUST_BEGINNING_OF_LINE;
        return true;
    case TerminalSelectionAdjustment::EndOfLine:
        *out = GHOSTTY_SELECTION_ADJUST_END_OF_LINE;
        return true;
    }
    return false;
}

GhosttyColorRgb resolveStyleColor(const GhosttyStyleColor &color,
                                  const GhosttyRenderStateColors &colors,
                                  GhosttyColorRgb fallback)
{
    switch (color.tag) {
    case GHOSTTY_STYLE_COLOR_RGB: return color.value.rgb;
    case GHOSTTY_STYLE_COLOR_PALETTE:
        return colors.palette[color.value.palette];
    default: return fallback;
    }
}

uint16_t boundedU16(int value)
{
    return static_cast<uint16_t>(
        std::clamp(value, 1, static_cast<int>(UINT16_MAX)));
}

uint32_t boundedU32(int value)
{
    return static_cast<uint32_t>(std::max(value, 1));
}

uint32_t nonnegativeU32(int value)
{
    return static_cast<uint32_t>(std::max(value, 0));
}

GhosttyVtAdapter::Geometry
normalizedGeometry(GhosttyVtAdapter::Geometry geometry)
{
    return normalizedTerminalSessionGeometry(std::move(geometry));
}

bool containsControlText(const QByteArray &text)
{
    return std::any_of(text.cbegin(), text.cend(), [](char value) {
        const auto byte = static_cast<unsigned char>(value);
        return byte < 0x20U || byte == 0x7fU;
    });
}

GhosttyMods mapQtModifiers(int modifiers)
{
    const auto qtModifiers = static_cast<Qt::KeyboardModifiers>(modifiers);
    GhosttyMods result = 0;
    if (qtModifiers.testFlag(Qt::ShiftModifier)) result |= GHOSTTY_MODS_SHIFT;
    if (qtModifiers.testFlag(Qt::ControlModifier)) result |= GHOSTTY_MODS_CTRL;
    if (qtModifiers.testFlag(Qt::AltModifier)) result |= GHOSTTY_MODS_ALT;
    if (qtModifiers.testFlag(Qt::MetaModifier)) result |= GHOSTTY_MODS_SUPER;
    return result;
}

GhosttyMouseButton mapQtMouseButton(int button)
{
    switch (button) {
    case 1: return GHOSTTY_MOUSE_BUTTON_LEFT;
    case 2: return GHOSTTY_MOUSE_BUTTON_RIGHT;
    case 3: return GHOSTTY_MOUSE_BUTTON_MIDDLE;
    case 4: return GHOSTTY_MOUSE_BUTTON_FOUR;
    case 5: return GHOSTTY_MOUSE_BUTTON_FIVE;
    case 6: return GHOSTTY_MOUSE_BUTTON_SIX;
    case 7: return GHOSTTY_MOUSE_BUTTON_SEVEN;
    case 8: return GHOSTTY_MOUSE_BUTTON_EIGHT;
    case 9: return GHOSTTY_MOUSE_BUTTON_NINE;
    default: return GHOSTTY_MOUSE_BUTTON_UNKNOWN;
    }
}

} // namespace

class GhosttyVtAdapter::TrackedHyperlink::Impl final {
public:
    Impl(std::shared_ptr<const AdapterOwnerToken> owner,
         GhosttyTerminalScreen screen)
        : owner_(std::move(owner))
        , screen_(screen)
    {}

    ~Impl() { ghostty_tracked_grid_ref_free(reference_); }

    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;
    Impl(Impl &&) = delete;
    Impl &operator=(Impl &&) = delete;

    std::shared_ptr<const AdapterOwnerToken> owner_;
    GhosttyTrackedGridRef reference_ = nullptr;
    GhosttyTerminalScreen screen_ = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
    QByteArray uri_;
};

class GhosttyVtAdapter::LogicalLineSnapshot::Impl final {
public:
    Impl(std::shared_ptr<const AdapterOwnerToken> owner,
         GhosttyTerminalScreen screen, TextMapData data,
         qsizetype targetByteOffset, ScreenCell targetCell,
         ScreenCell mappedTargetCell, bool targetIsSpacerHead)
        : owner_(std::move(owner))
        , screen_(screen)
        , data_(std::move(data))
        , targetByteOffset_(targetByteOffset)
        , targetCell_(targetCell)
        , mappedTargetCell_(mappedTargetCell)
        , targetIsSpacerHead_(targetIsSpacerHead)
    {}

    bool byteRangeContainsTarget(qsizetype beginByte, qsizetype endByte) const
    {
        if (beginByte < 0 || endByte <= beginByte
            || endByte > data_.byteCells.size()) {
            return false;
        }
        const auto begin = data_.byteCells.cbegin() + beginByte;
        const auto end = data_.byteCells.cbegin() + endByte;
        if (std::find(begin, end, mappedTargetCell_) != end) {
            return true;
        }
        if (!targetIsSpacerHead_) {
            return false;
        }

        // A wide glyph wrapping at the right edge leaves a spacer-head cell
        // between the preceding bytes and the glyph on the next row. Ghostty
        // link activation uses the inclusive matched selection, so that cell
        // belongs only to matches whose byte-mapped endpoints straddle it.
        return *begin < targetCell_ && targetCell_ < *(end - 1);
    }

    std::shared_ptr<const AdapterOwnerToken> owner_;
    GhosttyTerminalScreen screen_ = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
    TextMapData data_;
    qsizetype targetByteOffset_ = -1;
    ScreenCell targetCell_;
    ScreenCell mappedTargetCell_;
    bool targetIsSpacerHead_ = false;
};

class GhosttyVtAdapter::TrackedTextRange::Impl final {
public:
    Impl(std::shared_ptr<const AdapterOwnerToken> owner,
         GhosttyTerminalScreen screen)
        : owner_(std::move(owner))
        , screen_(screen)
    {}

    ~Impl()
    {
        ghostty_tracked_grid_ref_free(start_);
        ghostty_tracked_grid_ref_free(end_);
        ghostty_tracked_grid_ref_free(target_);
    }

    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;
    Impl(Impl &&) = delete;
    Impl &operator=(Impl &&) = delete;

    std::shared_ptr<const AdapterOwnerToken> owner_;
    GhosttyTrackedGridRef start_ = nullptr;
    GhosttyTrackedGridRef end_ = nullptr;
    GhosttyTrackedGridRef target_ = nullptr;
    GhosttyTerminalScreen screen_ = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
    QByteArray matchedText_;
    QByteArray coveredText_;
};

class GhosttyVtAdapter::Impl final {
public:
    Impl(Geometry geometry, Callbacks callbacks)
        : geometry_(normalizedGeometry(geometry))
        , callbacks_(std::move(callbacks))
        , ownerToken_(std::make_shared<AdapterOwnerToken>())
    {
        if (!callbacks_.queryMachineHostName) {
            callbacks_.queryMachineHostName = machineHostName;
        }
    }

    ~Impl() { destroy(); }

    bool initialize(const Options &adapterOptions)
    {
        if (!installKittyPngDecoder()) {
            return false;
        }
        colorScheme_ = adapterOptions.colorScheme;
        clipboardWriteAccess_ = adapterOptions.clipboardWriteAccess;
        enquiryResponse_ = adapterOptions.enquiryResponse;
        const auto boundedLimit = [](std::optional<quint64> value) {
            if (!value.has_value()) return std::optional<size_t>{};
            const quint64 maximum =
                static_cast<quint64>(std::numeric_limits<size_t>::max());
            return std::optional<size_t>{
                static_cast<size_t>(std::min(*value, maximum))};
        };
        const std::optional<size_t> scrollbackBytes =
            boundedLimit(adapterOptions.scrollbackBytes);
        const std::optional<size_t> scrollbackLines =
            boundedLimit(adapterOptions.scrollbackLines);
        if (ghostty_terminal_new(nullptr, &terminal_,
                                 boundedU16(geometry_.columns),
                                 boundedU16(geometry_.rows))
                != GHOSTTY_SUCCESS
            || terminalModeSetDefault(terminal_, GHOSTTY_MODE_GRAPHEME_CLUSTER,
                                      adapterOptions.graphemeWidthUnicode)
                != GHOSTTY_SUCCESS
            || ghostty_terminal_set(terminal_,
                                    GHOSTTY_TERMINAL_OPT_TITLE_REPORT,
                                    &adapterOptions.titleReport)
                != GHOSTTY_SUCCESS
            || ghostty_terminal_set(
                   terminal_, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES,
                   scrollbackBytes ? &*scrollbackBytes : nullptr)
                != GHOSTTY_SUCCESS
            || ghostty_terminal_set(
                   terminal_, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES,
                   scrollbackLines ? &*scrollbackLines : nullptr)
                != GHOSTTY_SUCCESS
            || !resize(geometry_)) {
            return false;
        }

        if (!adapterOptions.terminfoName.isEmpty()
            && adapterOptions.terminfoName.size() <= 128
            && !adapterOptions.terminfoName.contains('\0')) {
            const GhosttyString terminfoName{
                .ptr = reinterpret_cast<const uint8_t *>(
                    adapterOptions.terminfoName.constData()),
                .len = static_cast<size_t>(adapterOptions.terminfoName.size()),
            };
            if (ghostty_terminal_set(terminal_,
                                     GHOSTTY_TERMINAL_OPT_TERMINFO_NAME,
                                     &terminfoName)
                != GHOSTTY_SUCCESS) {
                return false;
            }
        }

        const bool imageMediumEnabled = true;
        const QByteArray temporaryDirectory =
            QFile::encodeName(QDir::tempPath());
        const GhosttyString temporaryDirectoryString{
            .ptr = reinterpret_cast<const uint8_t *>(
                temporaryDirectory.constData()),
            .len = static_cast<size_t>(temporaryDirectory.size()),
        };
        if (!setKittyImageStorageLimit(
                adapterOptions.kittyImageStorageLimitBytes)
            || ghostty_terminal_set(
                   terminal_, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_FILE,
                   &imageMediumEnabled)
                != GHOSTTY_SUCCESS
            || ghostty_terminal_set(
                   terminal_, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_TEMP_FILE,
                   temporaryDirectory.isEmpty() ? nullptr
                                                : &temporaryDirectoryString)
                != GHOSTTY_SUCCESS
            || ghostty_terminal_set(
                   terminal_,
                   GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_SHARED_MEM,
                   &imageMediumEnabled)
                != GHOSTTY_SUCCESS) {
            return false;
        }

        if (!setAppearance(adapterOptions.appearance)) {
            return false;
        }

        if (adapterOptions.initialWorkingDirectory.has_value()) {
            const QByteArray &directory =
                *adapterOptions.initialWorkingDirectory;
            const GhosttyString pwd{
                .ptr = reinterpret_cast<const uint8_t *>(directory.constData()),
                .len = static_cast<size_t>(directory.size()),
            };
            if (ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_PWD, &pwd)
                != GHOSTTY_SUCCESS) {
                return false;
            }
        }

        ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_USERDATA, this);
        ghostty_terminal_set(
            terminal_, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
            reinterpret_cast<const void *>(&Impl::writePtyCallback));
        ghostty_terminal_set(
            terminal_, GHOSTTY_TERMINAL_OPT_ENQUIRY,
            reinterpret_cast<const void *>(&Impl::enquiryCallback));
        ghostty_terminal_set(
            terminal_, GHOSTTY_TERMINAL_OPT_BELL,
            reinterpret_cast<const void *>(&Impl::bellCallback));
        ghostty_terminal_set(
            terminal_, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED,
            reinterpret_cast<const void *>(&Impl::titleCallback));
        ghostty_terminal_set(
            terminal_, GHOSTTY_TERMINAL_OPT_PWD_CHANGED,
            reinterpret_cast<const void *>(&Impl::pwdCallback));
        ghostty_terminal_set(
            terminal_, GHOSTTY_TERMINAL_OPT_SIZE,
            reinterpret_cast<const void *>(&Impl::sizeCallback));
        ghostty_terminal_set(
            terminal_, GHOSTTY_TERMINAL_OPT_COLOR_SCHEME,
            reinterpret_cast<const void *>(&Impl::colorSchemeCallback));
        ghostty_terminal_set(
            terminal_, GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES,
            reinterpret_cast<const void *>(&Impl::deviceAttributesCallback));
        ghostty_terminal_set(
            terminal_, GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE,
            reinterpret_cast<const void *>(&Impl::clipboardWriteCallback));
        if (ghostty_terminal_set(terminal_,
                                 GHOSTTY_TERMINAL_OPT_DESKTOP_NOTIFICATION,
                                 reinterpret_cast<const void *>(
                                     &Impl::desktopNotificationCallback))
            != GHOSTTY_SUCCESS) {
            return false;
        }
        if (ghostty_terminal_set(
                terminal_, GHOSTTY_TERMINAL_OPT_PROGRESS_REPORT,
                reinterpret_cast<const void *>(&Impl::progressReportCallback))
            != GHOSTTY_SUCCESS) {
            return false;
        }

        if (ghostty_render_state_new(nullptr, &renderState_) != GHOSTTY_SUCCESS
            || ghostty_render_state_row_iterator_new(nullptr, &rowIterator_)
                != GHOSTTY_SUCCESS
            || ghostty_render_state_row_cells_new(nullptr, &rowCells_)
                != GHOSTTY_SUCCESS
            || ghostty_kitty_graphics_placement_iterator_new(
                   nullptr, &kittyPlacementIterator_)
                != GHOSTTY_SUCCESS
            || ghostty_key_encoder_new(nullptr, &keyEncoder_) != GHOSTTY_SUCCESS
            || ghostty_key_event_new(nullptr, &keyEvent_) != GHOSTTY_SUCCESS
            || ghostty_mouse_encoder_new(nullptr, &mouseEncoder_)
                != GHOSTTY_SUCCESS
            || ghostty_mouse_event_new(nullptr, &mouseEvent_) != GHOSTTY_SUCCESS
            || ghostty_selection_gesture_new(nullptr, &selectionGesture_)
                != GHOSTTY_SUCCESS
            || ghostty_selection_gesture_event_new(
                   nullptr, &selectionPressEvent_,
                   GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_PRESS)
                != GHOSTTY_SUCCESS
            || ghostty_selection_gesture_event_new(
                   nullptr, &selectionDragEvent_,
                   GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_DRAG)
                != GHOSTTY_SUCCESS
            || ghostty_selection_gesture_event_new(
                   nullptr, &selectionAutoscrollTickEvent_,
                   GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_AUTOSCROLL_TICK)
                != GHOSTTY_SUCCESS
            || ghostty_selection_gesture_event_new(
                   nullptr, &selectionReleaseEvent_,
                   GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_RELEASE)
                != GHOSTTY_SUCCESS) {
            return false;
        }

        synchronizeInputModes();
        const bool deduplicateMotion = true;
        ghostty_mouse_encoder_setopt(mouseEncoder_,
                                     GHOSTTY_MOUSE_ENCODER_OPT_TRACK_LAST_CELL,
                                     &deduplicateMotion);
        return true;
    }

    bool setAppearance(const TerminalAppearance &appearance)
    {
        if (!appearance.foregroundColor.isValid()
            || !appearance.backgroundColor.isValid()
            || (!appearance.palette.isEmpty()
                && appearance.palette.size() != 256)) {
            return false;
        }

        std::array<GhosttyColorRgb, 256> palette{};
        for (qsizetype index = 0; index < appearance.palette.size(); ++index) {
            if (!appearance.palette.at(index).isValid()) {
                return false;
            }
            palette[static_cast<size_t>(index)] =
                toGhosttyColor(appearance.palette.at(index));
        }
        if (appearance.cursorColor.kind == TerminalColorKind::Color
            && !appearance.cursorColor.color.isValid()) {
            return false;
        }

        const GhosttyColorRgb foreground =
            toGhosttyColor(appearance.foregroundColor);
        const GhosttyColorRgb background =
            toGhosttyColor(appearance.backgroundColor);
        const GhosttyColorRgb cursor =
            appearance.cursorColor.kind == TerminalColorKind::Color
            ? toGhosttyColor(appearance.cursorColor.color)
            : GhosttyColorRgb{};
        const GhosttyTerminalCursorStyle cursorStyle =
            toGhosttyCursorStyle(appearance.cursorStyle);
        // Ghostty's application default for a null cursor-style-blink is true.
        // The pinned libghostty-vt C surface only accepts a bool, so it cannot
        // reproduce the application's distinction where an explicit value
        // also suppresses DEC mode 12. Preserve the visual default here and
        // track the semantic limitation in the parity ledger.
        const bool cursorBlink = appearance.cursorBlink.value_or(true);

        const void *paletteValue = appearance.palette.isEmpty()
            ? nullptr
            : static_cast<const void *>(palette.data());
        const void *cursorValue =
            appearance.cursorColor.kind == TerminalColorKind::Color
            ? static_cast<const void *>(&cursor)
            : nullptr;
        return ghostty_terminal_set(terminal_,
                                    GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND,
                                    &foreground)
            == GHOSTTY_SUCCESS
            && ghostty_terminal_set(terminal_,
                                    GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND,
                                    &background)
            == GHOSTTY_SUCCESS
            && ghostty_terminal_set(
                   terminal_, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, paletteValue)
            == GHOSTTY_SUCCESS
            && ghostty_terminal_set(
                   terminal_, GHOSTTY_TERMINAL_OPT_COLOR_CURSOR, cursorValue)
            == GHOSTTY_SUCCESS
            && ghostty_terminal_set(terminal_,
                                    GHOSTTY_TERMINAL_OPT_DEFAULT_CURSOR_STYLE,
                                    &cursorStyle)
            == GHOSTTY_SUCCESS
            && ghostty_terminal_set(terminal_,
                                    GHOSTTY_TERMINAL_OPT_DEFAULT_CURSOR_BLINK,
                                    &cursorBlink)
            == GHOSTTY_SUCCESS;
    }

    void setClipboardWriteAccess(TerminalClipboardAccess access)
    {
        clipboardWriteAccess_ = access;
    }

    void setColorScheme(TerminalColorScheme scheme)
    {
        if (colorScheme_ == scheme) {
            return;
        }
        colorScheme_ = scheme;

        bool reportEnabled = false;
        if (terminalModeGet(terminal_, GHOSTTY_MODE_COLOR_SCHEME_REPORT,
                            &reportEnabled)
                != GHOSTTY_SUCCESS
            || !reportEnabled || !callbacks_.writePty) {
            return;
        }

        std::array<char, 16> report{};
        size_t written = 0;
        if (ghostty_color_scheme_report_encode(toGhosttyColorScheme(scheme),
                                               report.data(), report.size(),
                                               &written)
            == GHOSTTY_SUCCESS) {
            callbacks_.writePty(
                QByteArrayView(report.data(), static_cast<qsizetype>(written)));
        }
    }

    void setEnquiryResponse(const QByteArray &response)
    {
        enquiryResponse_ = response;
    }

    bool setKittyImageStorageLimit(quint64 bytes)
    {
        const uint64_t limit = bytes;
        return terminal_ != nullptr
            && ghostty_terminal_set(
                   terminal_, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,
                   &limit)
            == GHOSTTY_SUCCESS;
    }

    void destroy()
    {
        if (selectionReleaseEvent_ != nullptr) {
            ghostty_selection_gesture_event_free(selectionReleaseEvent_);
            selectionReleaseEvent_ = nullptr;
        }
        if (selectionAutoscrollTickEvent_ != nullptr) {
            ghostty_selection_gesture_event_free(selectionAutoscrollTickEvent_);
            selectionAutoscrollTickEvent_ = nullptr;
        }
        if (selectionDragEvent_ != nullptr) {
            ghostty_selection_gesture_event_free(selectionDragEvent_);
            selectionDragEvent_ = nullptr;
        }
        if (selectionPressEvent_ != nullptr) {
            ghostty_selection_gesture_event_free(selectionPressEvent_);
            selectionPressEvent_ = nullptr;
        }
        if (selectionGesture_ != nullptr) {
            ghostty_selection_gesture_free(selectionGesture_, terminal_);
            selectionGesture_ = nullptr;
        }
        if (mouseEvent_ != nullptr) {
            ghostty_mouse_event_free(mouseEvent_);
            mouseEvent_ = nullptr;
        }
        if (mouseEncoder_ != nullptr) {
            ghostty_mouse_encoder_free(mouseEncoder_);
            mouseEncoder_ = nullptr;
        }
        if (keyEvent_ != nullptr) {
            ghostty_key_event_free(keyEvent_);
            keyEvent_ = nullptr;
        }
        if (keyEncoder_ != nullptr) {
            ghostty_key_encoder_free(keyEncoder_);
            keyEncoder_ = nullptr;
        }
        if (rowCells_ != nullptr) {
            ghostty_render_state_row_cells_free(rowCells_);
            rowCells_ = nullptr;
        }
        if (kittyPlacementIterator_ != nullptr) {
            ghostty_kitty_graphics_placement_iterator_free(
                kittyPlacementIterator_);
            kittyPlacementIterator_ = nullptr;
        }
        if (rowIterator_ != nullptr) {
            ghostty_render_state_row_iterator_free(rowIterator_);
            rowIterator_ = nullptr;
        }
        if (renderState_ != nullptr) {
            ghostty_render_state_free(renderState_);
            renderState_ = nullptr;
        }
        if (terminal_ != nullptr) {
            ghostty_terminal_free(terminal_);
            terminal_ = nullptr;
        }
    }

    bool resize(const Geometry &geometry)
    {
        const Geometry normalized = normalizedGeometry(geometry);
        if (ghostty_terminal_resize(
                terminal_, static_cast<uint16_t>(normalized.columns),
                static_cast<uint16_t>(normalized.rows),
                static_cast<uint32_t>(normalized.cellWidthPixels),
                static_cast<uint32_t>(normalized.cellHeightPixels))
            != GHOSTTY_SUCCESS) {
            return false;
        }
        geometry_ = normalized;
        return true;
    }

    void writeVt(QByteArrayView data)
    {
        if (!data.isEmpty()) {
            ghostty_terminal_vt_write(
                terminal_, reinterpret_cast<const uint8_t *>(data.data()),
                static_cast<size_t>(data.size()));
        }
    }

    bool observeOutputBottomAnchorChanged()
    {
        bool synchronizedOutput = false;
        if (terminalModeGet(terminal_, GHOSTTY_MODE_SYNC_OUTPUT,
                            &synchronizedOutput)
                != GHOSTTY_SUCCESS
            || synchronizedOutput) {
            // Ghostty skips its entire renderer update while mode 2026 is
            // active, including both comparison and cache advancement.
            return false;
        }

        uint16_t rows = 0;
        if (ghostty_terminal_get(terminal_, GHOSTTY_TERMINAL_DATA_ROWS, &rows)
                != GHOSTTY_SUCCESS
            || rows == 0) {
            return false;
        }

        // getBottomRight(.screen), used by Ghostty's renderer, and the final
        // row of the active area always resolve to the same PageList node/y.
        // Resolve x=0 because the renderer deliberately ignores x; this also
        // remains valid across mixed-width pages left behind by lazy reflow.
        GhosttyPoint point{};
        point.tag = GHOSTTY_POINT_TAG_ACTIVE;
        point.value.coordinate = {
            .x = 0,
            .y = static_cast<uint32_t>(rows - 1),
        };
        GhosttyGridRef reference{};
        reference.size = sizeof(reference);
        if (ghostty_terminal_grid_ref(terminal_, point, &reference)
                != GHOSTTY_SUCCESS
            || reference.node == nullptr) {
            return false;
        }

        // Store the opaque address only as an integer identity. Untracked
        // GhosttyGridRef values expire on the next mutation and must not be
        // retained or dereferenced.
        const quintptr node = reinterpret_cast<quintptr>(reference.node);
        const bool changed = !lastOutputBottomNode_.has_value()
            || *lastOutputBottomNode_ != node
            || lastOutputBottomY_ != reference.y;
        lastOutputBottomNode_ = node;
        lastOutputBottomY_ = reference.y;
        return changed;
    }

    void reset()
    {
        ghostty_terminal_reset(terminal_);
        normalizeKeyboardAfterCommandExit_ = false;

        // A full terminal reset invalidates selection grid references and
        // resets the modes mirrored by the input encoders. Keep every piece
        // of adapter-side state synchronized with that single mutation.
        ghostty_selection_gesture_reset(selectionGesture_, terminal_);
        lastSelectionPressTimestampNanoseconds_.reset();
        ghostty_mouse_encoder_reset(mouseEncoder_);
        mouseEncoderConfigured_ = false;
        synchronizeInputModes();

        // fullReset clears terminal-owned pwd without going through its OSC
        // callback. Reset publishes no application title update, so the
        // frontend retains the last base title published by either OSC or
        // set_surface_title.
        pendingCurrentDirectory_ = QByteArrayLiteral("");

        // Do not rely on the C API's dirty-state implementation detail. A
        // reset discards screen and scrollback contents, so the next update
        // must replace the complete frame held by TerminalPane.
        hasPublishedFrame_ = false;
    }

    void normalizeKeyboardAfterCommandExit()
    {
        // KAM is terminal-owned and can be reset through the public API.
        // Kitty's active flag stack is read-only in libghostty-vt's current C
        // surface, so force the equivalent disabled value on the encoder after
        // each subsequent terminal-state synchronization.
        (void)terminalModeSet(terminal_, GHOSTTY_MODE_KAM, false);
        normalizeKeyboardAfterCommandExit_ = true;
    }

    void synchronizeInputModes()
    {
        const std::array<GhosttyMode, 8> modes{
            GHOSTTY_MODE_X10_MOUSE,    GHOSTTY_MODE_NORMAL_MOUSE,
            GHOSTTY_MODE_BUTTON_MOUSE, GHOSTTY_MODE_ANY_MOUSE,
            GHOSTTY_MODE_UTF8_MOUSE,   GHOSTTY_MODE_SGR_MOUSE,
            GHOSTTY_MODE_URXVT_MOUSE,  GHOSTTY_MODE_SGR_PIXELS_MOUSE,
        };
        std::array<GhosttyTerminalModeConfig, modes.size()> configs{};
        std::array<GhosttyTerminalData, modes.size()> keys{};
        std::array<void *, modes.size()> values{};
        for (size_t index = 0; index < modes.size(); ++index) {
            configs[index].mode = modes[index];
            keys[index] = GHOSTTY_TERMINAL_DATA_MODE;
            values[index] = &configs[index];
        }
        size_t written = 0;
        if (ghostty_terminal_get_multi(terminal_, keys.size(), keys.data(),
                                       values.data(), &written)
                != GHOSTTY_SUCCESS
            || written != keys.size()) {
            return;
        }

        uint32_t fingerprint = 0;
        for (size_t index = 0; index < configs.size(); ++index) {
            if (configs[index].value) {
                fingerprint |= uint32_t{1} << static_cast<uint32_t>(index);
            }
        }
        if (!mouseEncoderConfigured_ || fingerprint != mouseModeFingerprint_) {
            ghostty_mouse_encoder_setopt_from_terminal(mouseEncoder_,
                                                       terminal_);
            mouseModeFingerprint_ = fingerprint;
            mouseEncoderConfigured_ = true;
        }
    }

    GhosttyVtAdapter::EncodedKey encodeKey(const TerminalKeyInput &input)
    {
        ghostty_key_encoder_setopt_from_terminal(keyEncoder_, terminal_);
        if (normalizeKeyboardAfterCommandExit_) {
            const GhosttyKittyKeyFlags flags = GHOSTTY_KITTY_KEY_DISABLED;
            ghostty_key_encoder_setopt(
                keyEncoder_, GHOSTTY_KEY_ENCODER_OPT_KITTY_FLAGS, &flags);
        }
        ghostty_key_event_set_action(
            keyEvent_,
            input.pressed ? (input.autoRepeat ? GHOSTTY_KEY_ACTION_REPEAT
                                              : GHOSTTY_KEY_ACTION_PRESS)
                          : GHOSTTY_KEY_ACTION_RELEASE);

        const auto qtModifiers =
            static_cast<Qt::KeyboardModifiers>(input.modifiers);
        const GhosttyKey key = ghosttyEffectiveKey(
            input.nativeScanCode, input.resolvedKeysym, input.key, qtModifiers);

        ghostty_key_event_set_key(keyEvent_, key);
        GhosttyMods mods = mapQtModifiers(input.modifiers);
        if (input.capsLock) mods |= GHOSTTY_MODS_CAPS_LOCK;
        if (input.numLock) mods |= GHOSTTY_MODS_NUM_LOCK;
        ghostty_key_event_set_mods(keyEvent_, mods);
        GhosttyMods consumedMods = mapQtModifiers(input.consumedModifiers);
        if (input.consumedCapsLock) {
            consumedMods |= GHOSTTY_MODS_CAPS_LOCK;
        }
        ghostty_key_event_set_consumed_mods(keyEvent_, consumedMods);
        ghostty_key_event_set_composing(keyEvent_, input.composing);
        ghostty_key_event_set_unshifted_codepoint(keyEvent_,
                                                  input.unshiftedCodepoint);

        QByteArray utf8 = input.text.toUtf8();
        if (containsControlText(utf8)) {
            utf8.clear();
        }
        ghostty_key_event_set_utf8(keyEvent_,
                                   utf8.isEmpty() ? nullptr : utf8.constData(),
                                   static_cast<size_t>(utf8.size()));

        QByteArray encoded(128, Qt::Uninitialized);
        size_t written = 0;
        GhosttyResult result = ghostty_key_encoder_encode(
            keyEncoder_, keyEvent_, encoded.data(),
            static_cast<size_t>(encoded.size()), &written);
        if (result == GHOSTTY_OUT_OF_SPACE) {
            encoded.resize(static_cast<qsizetype>(written));
            result = ghostty_key_encoder_encode(
                keyEncoder_, keyEvent_, encoded.data(),
                static_cast<size_t>(encoded.size()), &written);
        }
        if (result != GHOSTTY_SUCCESS) {
            encoded.clear();
        } else {
            encoded.resize(static_cast<qsizetype>(written));
        }
        return {
            .bytes = std::move(encoded),
            .success = result == GHOSTTY_SUCCESS,
            .modifier = ghosttyKeyIsModifier(key),
            .escape = key == GHOSTTY_KEY_ESCAPE,
        };
    }

    bool keyboardActionMode() const
    {
        bool enabled = false;
        return terminalModeGet(terminal_, GHOSTTY_MODE_KAM, &enabled)
            == GHOSTTY_SUCCESS
            && enabled;
    }

    QByteArray encodeMouse(const TerminalMouseInput &input)
    {
        GhosttyMouseEncoderSize size{};
        size.size = sizeof(size);
        size.screen_width = boundedU32(geometry_.surfaceWidthPixels);
        size.screen_height = boundedU32(geometry_.surfaceHeightPixels);
        size.cell_width = boundedU32(geometry_.cellWidthPixels);
        size.cell_height = boundedU32(geometry_.cellHeightPixels);
        size.padding_top = nonnegativeU32(geometry_.padding.top);
        size.padding_right = nonnegativeU32(geometry_.padding.right);
        size.padding_bottom = nonnegativeU32(geometry_.padding.bottom);
        size.padding_left = nonnegativeU32(geometry_.padding.left);
        ghostty_mouse_encoder_setopt(mouseEncoder_,
                                     GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &size);
        ghostty_mouse_encoder_setopt(
            mouseEncoder_, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED,
            &input.anyButtonPressed);
        GhosttyMouseAction action = GHOSTTY_MOUSE_ACTION_MOTION;
        if (input.action == TerminalMouseInput::Press) {
            action = GHOSTTY_MOUSE_ACTION_PRESS;
        } else if (input.action == TerminalMouseInput::Release) {
            action = GHOSTTY_MOUSE_ACTION_RELEASE;
        }
        ghostty_mouse_event_set_action(mouseEvent_, action);
        const GhosttyMouseButton button = mapQtMouseButton(input.button);
        if (button == GHOSTTY_MOUSE_BUTTON_UNKNOWN) {
            ghostty_mouse_event_clear_button(mouseEvent_);
        } else {
            ghostty_mouse_event_set_button(mouseEvent_, button);
        }
        ghostty_mouse_event_set_mods(mouseEvent_,
                                     mapQtModifiers(input.modifiers));
        ghostty_mouse_event_set_position(
            mouseEvent_, GhosttyMousePosition{input.x, input.y});

        QByteArray encoded(128, Qt::Uninitialized);
        size_t written = 0;
        GhosttyResult result = ghostty_mouse_encoder_encode(
            mouseEncoder_, mouseEvent_, encoded.data(),
            static_cast<size_t>(encoded.size()), &written);
        if (result == GHOSTTY_OUT_OF_SPACE) {
            encoded.resize(static_cast<qsizetype>(written));
            result = ghostty_mouse_encoder_encode(
                mouseEncoder_, mouseEvent_, encoded.data(),
                static_cast<size_t>(encoded.size()), &written);
        }
        if (result != GHOSTTY_SUCCESS) {
            return {};
        }
        encoded.resize(static_cast<qsizetype>(written));
        return encoded;
    }

    bool mouseTracking() const
    {
        bool tracking = false;
        return ghostty_terminal_get(
                   terminal_, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &tracking)
            == GHOSTTY_SUCCESS
            && tracking;
    }

    std::optional<QByteArray> alternateScrollSequence(qint64 rows) const
    {
        if (mouseTracking()) {
            return std::nullopt;
        }

        GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
        bool alternateScroll = false;
        if (ghostty_terminal_get(terminal_, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN,
                                 &screen)
                != GHOSTTY_SUCCESS
            || screen != GHOSTTY_TERMINAL_SCREEN_ALTERNATE
            || terminalModeGet(terminal_, GHOSTTY_MODE_ALT_SCROLL,
                               &alternateScroll)
                != GHOSTTY_SUCCESS
            || !alternateScroll) {
            return std::nullopt;
        }

        if (rows == 0) {
            return QByteArray{};
        }

        bool applicationCursorKeys = false;
        (void)terminalModeGet(terminal_, GHOSTTY_MODE_DECCKM,
                              &applicationCursorKeys);
        const QByteArrayView sequence = rows > 0 ? applicationCursorKeys
                ? QByteArrayView("\033OA", 3)
                : QByteArrayView("\033[A", 3)
            : applicationCursorKeys              ? QByteArrayView("\033OB", 3)
                                                 : QByteArrayView("\033[B", 3);
        const quint64 magnitude = rows > 0
            ? static_cast<quint64>(rows)
            : static_cast<quint64>(-(rows + 1)) + 1U;
        if (magnitude
            > static_cast<quint64>(std::numeric_limits<qsizetype>::max())
                / static_cast<quint64>(sequence.size())) {
            return QByteArray{};
        }

        QByteArray encoded;
        encoded.reserve(static_cast<qsizetype>(
            magnitude * static_cast<quint64>(sequence.size())));
        for (quint64 remaining = magnitude; remaining > 0; --remaining) {
            encoded.append(sequence);
        }
        return encoded;
    }

    QByteArray encodeFocus(bool focused) const
    {
        bool reportFocus = false;
        if (terminalModeGet(terminal_, GHOSTTY_MODE_FOCUS_EVENT, &reportFocus)
                != GHOSTTY_SUCCESS
            || !reportFocus) {
            return {};
        }

        std::array<char, 16> buffer{};
        size_t written = 0;
        if (ghostty_focus_encode(focused ? GHOSTTY_FOCUS_GAINED
                                         : GHOSTTY_FOCUS_LOST,
                                 buffer.data(), buffer.size(), &written)
            != GHOSTTY_SUCCESS) {
            return {};
        }
        return QByteArray(buffer.data(), static_cast<qsizetype>(written));
    }

    bool bracketedPasteMode() const
    {
        bool bracketed = false;
        (void)terminalModeGet(terminal_, GHOSTTY_MODE_BRACKETED_PASTE,
                              &bracketed);
        return bracketed;
    }

    QByteArray encodePaste(QByteArray mutableInput, bool bracketed) const
    {
        if (mutableInput.isEmpty()) {
            return {};
        }
        QByteArray encoded(mutableInput.size() + 32, Qt::Uninitialized);
        size_t written = 0;
        GhosttyResult result = ghostty_paste_encode(
            mutableInput.data(), static_cast<size_t>(mutableInput.size()),
            bracketed, encoded.data(), static_cast<size_t>(encoded.size()),
            &written);
        if (result == GHOSTTY_OUT_OF_SPACE) {
            encoded.resize(static_cast<qsizetype>(written));
            result = ghostty_paste_encode(
                mutableInput.data(), static_cast<size_t>(mutableInput.size()),
                bracketed, encoded.data(), static_cast<size_t>(encoded.size()),
                &written);
        }
        if (result != GHOSTTY_SUCCESS) {
            return {};
        }
        encoded.resize(static_cast<qsizetype>(written));
        return encoded;
    }

    QByteArray encodePaste(const QString &text) const
    {
        return encodePaste(text.toUtf8(), bracketedPasteMode());
    }

    GhosttyVtAdapter::PreparedPaste
    preparePaste(const QString &text,
                 const GhosttyVtAdapter::PastePreparationOptions &options) const
    {
        if (text.isEmpty()) {
            return {
                .disposition = GhosttyVtAdapter::PasteDisposition::Ready,
                .bytes = {},
            };
        }

        const bool bracketed = bracketedPasteMode();
        QByteArray utf8 = text.toUtf8();
        if (options.protection
            && options.authorization
                == GhosttyVtAdapter::PasteAuthorization::Initial) {
            const bool containsEndFence =
                utf8.contains(QByteArrayLiteral("\x1b[201~"));
            const bool baselineSafe = ghostty_paste_is_safe(
                utf8.constData(), static_cast<size_t>(utf8.size()));
            if ((bracketed && containsEndFence)
                || ((!bracketed || !options.bracketedSafe) && !baselineSafe)) {
                return {
                    .disposition = GhosttyVtAdapter::PasteDisposition::
                        ConfirmationRequired,
                    .bytes = {},
                };
            }
        }

        QByteArray encoded = encodePaste(std::move(utf8), bracketed);
        if (encoded.isEmpty()) {
            return {
                .disposition = GhosttyVtAdapter::PasteDisposition::Failed,
                .bytes = {},
            };
        }
        return {
            .disposition = GhosttyVtAdapter::PasteDisposition::Ready,
            .bytes = std::move(encoded),
        };
    }

    QString selectedText(bool trim = true) const
    {
        GhosttyTerminalSelectionFormatOptions options{};
        options.size = sizeof(options);
        options.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
        options.unwrap = true;
        options.trim = trim;
        options.selection = nullptr;

        size_t required = 0;
        GhosttyResult result = ghostty_terminal_selection_format_buf(
            terminal_, options, nullptr, 0, &required);
        if (result == GHOSTTY_NO_VALUE) {
            return {};
        }
        if (result != GHOSTTY_OUT_OF_SPACE && result != GHOSTTY_SUCCESS) {
            return {};
        }
        if (required == 0) {
            return QStringLiteral("");
        }

        QByteArray output(static_cast<qsizetype>(required), Qt::Uninitialized);
        size_t written = 0;
        result = ghostty_terminal_selection_format_buf(
            terminal_, options, reinterpret_cast<uint8_t *>(output.data()),
            static_cast<size_t>(output.size()), &written);
        if (result != GHOSTTY_SUCCESS) {
            return {};
        }
        output.resize(static_cast<qsizetype>(written));
        return QString::fromUtf8(output);
    }

    PlainFileSnapshot snapshotPlainFile(TerminalFileLocation location) const
    {
        const auto withoutBytes = [](PlainFileSnapshotStatus status) {
            return PlainFileSnapshot{
                .status = status,
                .bytes = {},
            };
        };
        GhosttySelection selection{};
        selection.size = sizeof(selection);
        const GhosttySelection *selectionPointer = nullptr;

        switch (location) {
        case TerminalFileLocation::Screen:
            // A null formatter selection means the complete active PageList.
            // Let libghostty choose its actual mixed-width bottom-right
            // instead of synthesizing an endpoint from the desired columns.
            break;
        case TerminalFileLocation::Selection: {
            const GhosttyResult selectionResult = ghostty_terminal_get(
                terminal_, GHOSTTY_TERMINAL_DATA_SELECTION, &selection);
            if (selectionResult == GHOSTTY_NO_VALUE) {
                return withoutBytes(PlainFileSnapshotStatus::Unavailable);
            }
            if (selectionResult != GHOSTTY_SUCCESS) {
                return withoutBytes(PlainFileSnapshotStatus::Failed);
            }
            selectionPointer = &selection;
            break;
        }
        case TerminalFileLocation::Scrollback: {
            size_t scrollbackRows = 0;
            if (ghostty_terminal_get(terminal_,
                                     GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS,
                                     &scrollbackRows)
                != GHOSTTY_SUCCESS) {
                return withoutBytes(PlainFileSnapshotStatus::Failed);
            }
            if (scrollbackRows == 0) {
                return withoutBytes(PlainFileSnapshotStatus::Unavailable);
            }
            GhosttyPoint start{};
            start.tag = GHOSTTY_POINT_TAG_SCREEN;
            start.value.coordinate = {
                .x = 0,
                .y = 0,
            };
            GhosttyPoint end{};
            end.tag = GHOSTTY_POINT_TAG_ACTIVE;
            end.value.coordinate = {
                .x = 0,
                .y = 0,
            };
            if (ghostty_terminal_grid_ref(terminal_, start, &selection.start)
                    != GHOSTTY_SUCCESS
                || ghostty_terminal_grid_ref(terminal_, end, &selection.end)
                    != GHOSTTY_SUCCESS) {
                return withoutBytes(PlainFileSnapshotStatus::Failed);
            }
            selection.rectangle = false;
            // Move from active top to the immediately preceding history row,
            // then ask libghostty for that stored page's real last column.
            // This remains exact while lazy reflow leaves page widths mixed.
            if (ghostty_terminal_selection_adjust(terminal_, &selection,
                                                  GHOSTTY_SELECTION_ADJUST_UP)
                    != GHOSTTY_SUCCESS
                || ghostty_terminal_selection_adjust(
                       terminal_, &selection,
                       GHOSTTY_SELECTION_ADJUST_END_OF_LINE)
                    != GHOSTTY_SUCCESS) {
                return withoutBytes(PlainFileSnapshotStatus::Failed);
            }
            selectionPointer = &selection;
            break;
        }
        }

        GhosttyFormatterTerminalOptions options{};
        options.size = sizeof(options);
        options.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
        options.unwrap = true;
        options.trim = false;
        options.selection = selectionPointer;
        options.extra.size = sizeof(options.extra);
        options.extra.screen.size = sizeof(options.extra.screen);

        GhosttyFormatter formatter = nullptr;
        if (ghostty_formatter_terminal_new(nullptr, &formatter, terminal_,
                                           options)
            != GHOSTTY_SUCCESS) {
            return withoutBytes(PlainFileSnapshotStatus::Failed);
        }
        const auto formatterCleanup =
            qScopeGuard([formatter] { ghostty_formatter_free(formatter); });

        auto output = formatToByteArray(formatter);
        if (!output.has_value()) {
            return withoutBytes(PlainFileSnapshotStatus::Failed);
        }
        return {
            .status = PlainFileSnapshotStatus::Ready,
            .bytes = std::move(*output),
        };
    }

    bool hasSelection() const
    {
        GhosttySelection selection{};
        selection.size = sizeof(selection);
        return ghostty_terminal_get(terminal_, GHOSTTY_TERMINAL_DATA_SELECTION,
                                    &selection)
            == GHOSTTY_SUCCESS;
    }

    void clearSelection()
    {
        ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_SELECTION,
                             nullptr);
    }

    void resetSelectionGesture()
    {
        ghostty_selection_gesture_reset(selectionGesture_, terminal_);
        lastSelectionPressTimestampNanoseconds_.reset();
    }

    void clearSelectionAndResetGesture()
    {
        clearSelection();
        resetSelectionGesture();
    }

    bool pointToGridRef(int column, int row, GhosttyGridRef *out) const
    {
        return pointToGridRefExact(std::clamp(column, 0, geometry_.columns - 1),
                                   std::clamp(row, 0, geometry_.rows - 1), out);
    }

    bool pointToGridRefExact(int column, int row, GhosttyGridRef *out) const
    {
        if (out == nullptr) {
            return false;
        }
        if (column < 0 || column >= geometry_.columns || row < 0
            || row >= geometry_.rows) {
            return false;
        }
        GhosttyPoint point{};
        point.tag = GHOSTTY_POINT_TAG_VIEWPORT;
        point.value.coordinate.x = static_cast<uint16_t>(column);
        point.value.coordinate.y = static_cast<uint32_t>(row);
        *out = GhosttyGridRef{};
        out->size = sizeof(*out);
        return ghostty_terminal_grid_ref(terminal_, point, out)
            == GHOSTTY_SUCCESS;
    }

    std::optional<GhosttyTerminalScreen> activeScreen() const
    {
        GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
        if (ghostty_terminal_get(terminal_, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN,
                                 &screen)
            != GHOSTTY_SUCCESS) {
            return std::nullopt;
        }
        return screen;
    }

    TerminalInspectorSnapshot inspectorSnapshot() const
    {
        TerminalInspectorSnapshot snapshot;
        if (terminal_ == nullptr) return snapshot;
        snapshot.status = TerminalInspectorStatus::Failed;

        GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
        GhosttyTerminalScrollbar scrollbar{};
        size_t totalRows = 0;
        size_t scrollbackRows = 0;
        GhosttyKittyKeyFlags kittyKeyboardFlags = GHOSTTY_KITTY_KEY_DISABLED;
        constexpr std::array keys{
            GHOSTTY_TERMINAL_DATA_COLS,
            GHOSTTY_TERMINAL_DATA_ROWS,
            GHOSTTY_TERMINAL_DATA_CURSOR_X,
            GHOSTTY_TERMINAL_DATA_CURSOR_Y,
            GHOSTTY_TERMINAL_DATA_CURSOR_PENDING_WRAP,
            GHOSTTY_TERMINAL_DATA_CURSOR_VISIBLE,
            GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN,
            GHOSTTY_TERMINAL_DATA_VIEWPORT_ACTIVE,
            GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING,
            GHOSTTY_TERMINAL_DATA_TOTAL_ROWS,
            GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS,
            GHOSTTY_TERMINAL_DATA_SCROLLBAR,
            GHOSTTY_TERMINAL_DATA_WIDTH_PX,
            GHOSTTY_TERMINAL_DATA_HEIGHT_PX,
            GHOSTTY_TERMINAL_DATA_KITTY_KEYBOARD_FLAGS,
        };
        std::array<void *, keys.size()> values{
            &snapshot.columns,
            &snapshot.rows,
            &snapshot.cursorColumn,
            &snapshot.cursorRow,
            &snapshot.cursorPendingWrap,
            &snapshot.cursorVisible,
            &screen,
            &snapshot.viewportActive,
            &snapshot.terminalMouseTracking,
            &totalRows,
            &scrollbackRows,
            &scrollbar,
            &snapshot.widthPixels,
            &snapshot.heightPixels,
            &kittyKeyboardFlags,
        };
        size_t written = 0;
        if (ghostty_terminal_get_multi(terminal_, keys.size(), keys.data(),
                                       values.data(), &written)
                != GHOSTTY_SUCCESS
            || written != keys.size()
            || (screen != GHOSTTY_TERMINAL_SCREEN_PRIMARY
                && screen != GHOSTTY_TERMINAL_SCREEN_ALTERNATE)) {
            return snapshot;
        }

        snapshot.activeScreen = screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE
            ? TerminalInspectorScreen::Alternate
            : TerminalInspectorScreen::Primary;
        snapshot.totalRows = static_cast<quint64>(totalRows);
        snapshot.scrollbackRows = static_cast<quint64>(scrollbackRows);
        snapshot.scrollTotal = scrollbar.total;
        snapshot.scrollOffset = scrollbar.offset;
        snapshot.scrollLength = scrollbar.len;
        snapshot.kittyKeyboardFlags = kittyKeyboardFlags;

        const auto queryColor = [this](GhosttyTerminalData data,
                                       QColor *result) {
            GhosttyColorRgb color{};
            const GhosttyResult query =
                ghostty_terminal_get(terminal_, data, &color);
            if (query == GHOSTTY_SUCCESS) {
                *result = toQColor(color);
                return true;
            }
            if (query == GHOSTTY_NO_VALUE) {
                *result = QColor{};
                return true;
            }
            return false;
        };
        if (!queryColor(GHOSTTY_TERMINAL_DATA_COLOR_FOREGROUND,
                        &snapshot.effectiveForeground)
            || !queryColor(GHOSTTY_TERMINAL_DATA_COLOR_BACKGROUND,
                           &snapshot.effectiveBackground)
            || !queryColor(GHOSTTY_TERMINAL_DATA_COLOR_CURSOR,
                           &snapshot.effectiveCursor)
            || !queryColor(GHOSTTY_TERMINAL_DATA_COLOR_FOREGROUND_DEFAULT,
                           &snapshot.defaultForeground)
            || !queryColor(GHOSTTY_TERMINAL_DATA_COLOR_BACKGROUND_DEFAULT,
                           &snapshot.defaultBackground)
            || !queryColor(GHOSTTY_TERMINAL_DATA_COLOR_CURSOR_DEFAULT,
                           &snapshot.defaultCursor)) {
            return snapshot;
        }

        const auto queryPalette = [this](GhosttyTerminalData data,
                                         QVector<QColor> *result) {
            std::array<GhosttyColorRgb, 256> colors{};
            if (ghostty_terminal_get(terminal_, data, colors.data())
                != GHOSTTY_SUCCESS) {
                return false;
            }
            result->reserve(static_cast<qsizetype>(colors.size()));
            for (const GhosttyColorRgb color : colors) {
                result->append(toQColor(color));
            }
            return true;
        };
        if (!queryPalette(GHOSTTY_TERMINAL_DATA_COLOR_PALETTE,
                          &snapshot.effectivePalette)
            || !queryPalette(GHOSTTY_TERMINAL_DATA_COLOR_PALETTE_DEFAULT,
                             &snapshot.defaultPalette)) {
            return snapshot;
        }

        uint64_t kittyStorageLimit = 0;
        const GhosttyResult kittyResult = ghostty_terminal_get(
            terminal_, GHOSTTY_TERMINAL_DATA_KITTY_IMAGE_STORAGE_LIMIT,
            &kittyStorageLimit);
        if (kittyResult == GHOSTTY_SUCCESS) {
            snapshot.kittyGraphicsAvailable = true;
            snapshot.kittyImageStorageLimitBytes = kittyStorageLimit;
            GhosttyString kittyTemporaryDirectory{};
            if (ghostty_terminal_get(
                    terminal_, GHOSTTY_TERMINAL_DATA_KITTY_IMAGE_MEDIUM_FILE,
                    &snapshot.kittyFileMedium)
                    != GHOSTTY_SUCCESS
                || ghostty_terminal_get(
                       terminal_,
                       GHOSTTY_TERMINAL_DATA_KITTY_IMAGE_MEDIUM_TEMP_FILE,
                       &kittyTemporaryDirectory)
                    != GHOSTTY_SUCCESS
                || ghostty_terminal_get(
                       terminal_,
                       GHOSTTY_TERMINAL_DATA_KITTY_IMAGE_MEDIUM_SHARED_MEM,
                       &snapshot.kittySharedMemoryMedium)
                    != GHOSTTY_SUCCESS) {
                return snapshot;
            }
            snapshot.kittyTemporaryFileMedium =
                kittyTemporaryDirectory.ptr != nullptr
                && kittyTemporaryDirectory.len != 0;
        } else if (kittyResult != GHOSTTY_NO_VALUE) {
            return snapshot;
        }

        std::array<GhosttyTerminalModeConfig, inspectorModeSpecs.size()>
            modeConfigs{};
        std::array<GhosttyTerminalData, inspectorModeSpecs.size()> modeKeys{};
        std::array<void *, inspectorModeSpecs.size()> modeValues{};
        for (size_t index = 0; index < inspectorModeSpecs.size(); ++index) {
            modeConfigs[index].mode = inspectorModeSpecs[index].mode;
            modeKeys[index] = GHOSTTY_TERMINAL_DATA_MODE;
            modeValues[index] = &modeConfigs[index];
        }
        written = 0;
        if (ghostty_terminal_get_multi(terminal_, modeKeys.size(),
                                       modeKeys.data(), modeValues.data(),
                                       &written)
                != GHOSTTY_SUCCESS
            || written != modeKeys.size()) {
            return snapshot;
        }

        snapshot.modes.reserve(
            static_cast<qsizetype>(inspectorModeSpecs.size()));
        for (size_t index = 0; index < inspectorModeSpecs.size(); ++index) {
            const InspectorModeSpec &spec = inspectorModeSpecs[index];
            TerminalInspectorModeState mode{
                .name = QString::fromLatin1(spec.name),
                .number = ghostty_mode_value(spec.mode),
                .ansi = ghostty_mode_ansi(spec.mode),
                .enabled = modeConfigs[index].value,
            };
            snapshot.modes.append(std::move(mode));
        }
        snapshot.status = TerminalInspectorStatus::Ready;
        return snapshot;
    }

    TerminalInspectorCellSnapshot inspectorCellSnapshot(int viewportColumn,
                                                        int viewportRow) const
    {
        TerminalInspectorCellSnapshot snapshot;
        snapshot.viewportColumn = viewportColumn;
        snapshot.viewportRow = viewportRow;
        if (terminal_ == nullptr) return snapshot;
        snapshot.status = TerminalInspectorCellStatus::Failed;

        uint16_t columns = 0;
        uint16_t rows = 0;
        GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
        constexpr std::array terminalFields{
            GHOSTTY_TERMINAL_DATA_COLS,
            GHOSTTY_TERMINAL_DATA_ROWS,
            GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN,
        };
        std::array<void *, terminalFields.size()> terminalValues{
            &columns,
            &rows,
            &screen,
        };
        if (ghostty_terminal_get_multi(terminal_, terminalFields.size(),
                                       terminalFields.data(),
                                       terminalValues.data(), nullptr)
                != GHOSTTY_SUCCESS
            || (screen != GHOSTTY_TERMINAL_SCREEN_PRIMARY
                && screen != GHOSTTY_TERMINAL_SCREEN_ALTERNATE)) {
            return snapshot;
        }
        snapshot.activeScreen = screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE
            ? TerminalInspectorScreen::Alternate
            : TerminalInspectorScreen::Primary;
        if (viewportColumn < 0 || viewportRow < 0
            || viewportColumn >= static_cast<int>(columns)
            || viewportRow >= static_cast<int>(rows)) {
            snapshot.status = TerminalInspectorCellStatus::OutOfBounds;
            return snapshot;
        }

        GhosttyPoint point{};
        point.tag = GHOSTTY_POINT_TAG_VIEWPORT;
        point.value.coordinate.x = static_cast<uint16_t>(viewportColumn);
        point.value.coordinate.y = static_cast<uint32_t>(viewportRow);
        GhosttyGridRef reference{};
        reference.size = sizeof(reference);
        const GhosttyResult referenceResult =
            ghostty_terminal_grid_ref(terminal_, point, &reference);
        if (referenceResult != GHOSTTY_SUCCESS) {
            if (referenceResult == GHOSTTY_INVALID_VALUE) {
                snapshot.status = TerminalInspectorCellStatus::OutOfBounds;
            }
            return snapshot;
        }

        GhosttyCell cell = 0;
        GhosttyRow row = 0;
        if (ghostty_grid_ref_cell(&reference, &cell) != GHOSTTY_SUCCESS
            || ghostty_grid_ref_row(&reference, &row) != GHOSTTY_SUCCESS) {
            return snapshot;
        }

        uint32_t codepoint = 0;
        GhosttyCellContentTag contentTag = GHOSTTY_CELL_CONTENT_CODEPOINT;
        GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
        GhosttyCellSemanticContent semantic = GHOSTTY_CELL_SEMANTIC_OUTPUT;
        constexpr std::array cellFields{
            GHOSTTY_CELL_DATA_CODEPOINT,
            GHOSTTY_CELL_DATA_CONTENT_TAG,
            GHOSTTY_CELL_DATA_WIDE,
            GHOSTTY_CELL_DATA_HAS_TEXT,
            GHOSTTY_CELL_DATA_HAS_STYLING,
            GHOSTTY_CELL_DATA_STYLE_ID,
            GHOSTTY_CELL_DATA_HAS_HYPERLINK,
            GHOSTTY_CELL_DATA_PROTECTED,
            GHOSTTY_CELL_DATA_SEMANTIC_CONTENT,
        };
        std::array<void *, cellFields.size()> cellValues{
            &codepoint,
            &contentTag,
            &wide,
            &snapshot.hasText,
            &snapshot.hasStyling,
            &snapshot.styleId,
            &snapshot.hasHyperlink,
            &snapshot.protectedCell,
            &semantic,
        };
        if (ghostty_cell_get_multi(cell, cellFields.size(), cellFields.data(),
                                   cellValues.data(), nullptr)
            != GHOSTTY_SUCCESS) {
            return snapshot;
        }

        switch (contentTag) {
        case GHOSTTY_CELL_CONTENT_CODEPOINT:
            snapshot.contentKind = TerminalInspectorCellContentKind::Codepoint;
            break;
        case GHOSTTY_CELL_CONTENT_CODEPOINT_GRAPHEME:
            snapshot.contentKind = TerminalInspectorCellContentKind::Grapheme;
            break;
        case GHOSTTY_CELL_CONTENT_BG_COLOR_PALETTE: {
            snapshot.contentKind =
                TerminalInspectorCellContentKind::BackgroundPalette;
            GhosttyColorPaletteIndex paletteIndex = 0;
            if (ghostty_cell_get(cell, GHOSTTY_CELL_DATA_COLOR_PALETTE,
                                 &paletteIndex)
                != GHOSTTY_SUCCESS) {
                return snapshot;
            }
            snapshot.contentBackground.kind =
                TerminalInspectorStyleColorKind::Palette;
            snapshot.contentBackground.paletteIndex = paletteIndex;
            break;
        }
        case GHOSTTY_CELL_CONTENT_BG_COLOR_RGB: {
            snapshot.contentKind =
                TerminalInspectorCellContentKind::BackgroundRgb;
            GhosttyColorRgb rgb{};
            if (ghostty_cell_get(cell, GHOSTTY_CELL_DATA_COLOR_RGB, &rgb)
                != GHOSTTY_SUCCESS) {
                return snapshot;
            }
            snapshot.contentBackground.kind =
                TerminalInspectorStyleColorKind::Rgb;
            snapshot.contentBackground.rgb = toQColor(rgb);
            break;
        }
        default: return snapshot;
        }

        switch (wide) {
        case GHOSTTY_CELL_WIDE_NARROW:
            snapshot.widthRole = TerminalInspectorCellWidthRole::Narrow;
            break;
        case GHOSTTY_CELL_WIDE_WIDE:
            snapshot.widthRole = TerminalInspectorCellWidthRole::Wide;
            break;
        case GHOSTTY_CELL_WIDE_SPACER_TAIL:
            snapshot.widthRole = TerminalInspectorCellWidthRole::SpacerTail;
            break;
        case GHOSTTY_CELL_WIDE_SPACER_HEAD:
            snapshot.widthRole = TerminalInspectorCellWidthRole::SpacerHead;
            break;
        default: return snapshot;
        }

        switch (semantic) {
        case GHOSTTY_CELL_SEMANTIC_OUTPUT:
            snapshot.semantic = TerminalInspectorCellSemantic::Output;
            break;
        case GHOSTTY_CELL_SEMANTIC_INPUT:
            snapshot.semantic = TerminalInspectorCellSemantic::Input;
            break;
        case GHOSTTY_CELL_SEMANTIC_PROMPT:
            snapshot.semantic = TerminalInspectorCellSemantic::Prompt;
            break;
        default: return snapshot;
        }

        const std::optional<QByteArray> grapheme = graphemeUtf8(reference);
        if (!grapheme.has_value()) return snapshot;
        snapshot.text = QString::fromUtf8(*grapheme);
        const QList<uint> codepoints = snapshot.text.toUcs4();
        snapshot.codepoints.reserve(codepoints.size());
        for (const uint value : codepoints) {
            snapshot.codepoints.append(value);
        }
        if ((snapshot.codepoints.isEmpty() && codepoint != 0)
            || (!snapshot.codepoints.isEmpty()
                && snapshot.codepoints.constFirst() != codepoint)) {
            return snapshot;
        }

        GhosttyStyle rawStyle{};
        rawStyle.size = sizeof(rawStyle);
        if (ghostty_grid_ref_style(&reference, &rawStyle) != GHOSTTY_SUCCESS) {
            return snapshot;
        }
        const std::optional<TerminalInspectorStyleColor> foreground =
            inspectorStyleColor(rawStyle.fg_color);
        const std::optional<TerminalInspectorStyleColor> background =
            inspectorStyleColor(rawStyle.bg_color);
        const std::optional<TerminalInspectorStyleColor> underlineColor =
            inspectorStyleColor(rawStyle.underline_color);
        const std::optional<TerminalInspectorUnderlineStyle> underline =
            inspectorUnderlineStyle(rawStyle.underline);
        if (!foreground.has_value() || !background.has_value()
            || !underlineColor.has_value() || !underline.has_value()) {
            return snapshot;
        }
        snapshot.style = {
            .foreground = *foreground,
            .background = *background,
            .underlineColor = *underlineColor,
            .bold = rawStyle.bold,
            .italic = rawStyle.italic,
            .faint = rawStyle.faint,
            .blink = rawStyle.blink,
            .inverse = rawStyle.inverse,
            .invisible = rawStyle.invisible,
            .strikethrough = rawStyle.strikethrough,
            .overline = rawStyle.overline,
            .underline = *underline,
        };

        if (snapshot.hasHyperlink) {
            const std::optional<QByteArray> uri = hyperlinkUri(reference);
            if (!uri.has_value()) return snapshot;
            snapshot.hyperlinkUri = *uri;
        }

        GhosttyRowSemanticPrompt rowSemantic = GHOSTTY_ROW_SEMANTIC_NONE;
        constexpr std::array rowFields{
            GHOSTTY_ROW_DATA_WRAP,
            GHOSTTY_ROW_DATA_WRAP_CONTINUATION,
            GHOSTTY_ROW_DATA_SEMANTIC_PROMPT,
        };
        std::array<void *, rowFields.size()> rowValues{
            &snapshot.rowWrapped,
            &snapshot.rowWrapContinuation,
            &rowSemantic,
        };
        if (ghostty_row_get_multi(row, rowFields.size(), rowFields.data(),
                                  rowValues.data(), nullptr)
            != GHOSTTY_SUCCESS) {
            return snapshot;
        }
        switch (rowSemantic) {
        case GHOSTTY_ROW_SEMANTIC_NONE:
            snapshot.rowSemantic = TerminalInspectorRowSemantic::None;
            break;
        case GHOSTTY_ROW_SEMANTIC_PROMPT:
            snapshot.rowSemantic = TerminalInspectorRowSemantic::Prompt;
            break;
        case GHOSTTY_ROW_SEMANTIC_PROMPT_CONTINUATION:
            snapshot.rowSemantic =
                TerminalInspectorRowSemantic::PromptContinuation;
            break;
        default: return snapshot;
        }

        snapshot.status = TerminalInspectorCellStatus::Ready;
        return snapshot;
    }

    GhosttyVtAdapter::SemanticPromptState semanticPromptState() const
    {
        bool atPrompt = false;
        return ghostty_terminal_get(
                   terminal_, GHOSTTY_TERMINAL_DATA_CURSOR_AT_PROMPT, &atPrompt)
                == GHOSTTY_SUCCESS
            ? (atPrompt ? SemanticPromptState::AtPrompt
                        : SemanticPromptState::Away)
            : SemanticPromptState::Unavailable;
    }

    std::optional<QByteArray>
    hyperlinkUri(const GhosttyGridRef &reference) const
    {
        size_t required = 0;
        GhosttyResult result =
            ghostty_grid_ref_hyperlink_uri(&reference, nullptr, 0, &required);
        if (result == GHOSTTY_SUCCESS && required == 0) {
            return QByteArray{};
        }
        if ((result != GHOSTTY_OUT_OF_SPACE && result != GHOSTTY_SUCCESS)
            || required == 0
            || required > static_cast<size_t>(maximumLogicalLineBytes)
            || required
                > static_cast<size_t>(std::numeric_limits<qsizetype>::max())) {
            return std::nullopt;
        }

        QByteArray uri(static_cast<qsizetype>(required), Qt::Uninitialized);
        size_t written = 0;
        result = ghostty_grid_ref_hyperlink_uri(
            &reference, reinterpret_cast<uint8_t *>(uri.data()),
            static_cast<size_t>(uri.size()), &written);
        if (result != GHOSTTY_SUCCESS || written > required) {
            return std::nullopt;
        }
        uri.resize(static_cast<qsizetype>(written));
        return uri;
    }

    std::optional<QByteArray> hyperlinkUriAt(int column, int row) const
    {
        GhosttyGridRef reference{};
        if (!pointToGridRefExact(column, row, &reference)) {
            return std::nullopt;
        }
        return hyperlinkUri(reference);
    }

    std::optional<TrackedHyperlink> trackHyperlinkAt(int column, int row) const
    {
        if (column < 0 || column >= geometry_.columns || row < 0
            || row >= geometry_.rows) {
            return std::nullopt;
        }

        const std::optional<GhosttyTerminalScreen> screen = activeScreen();
        if (!screen.has_value()) {
            return std::nullopt;
        }

        auto tracked =
            std::make_unique<TrackedHyperlink::Impl>(ownerToken_, *screen);
        GhosttyPoint point{};
        point.tag = GHOSTTY_POINT_TAG_VIEWPORT;
        point.value.coordinate.x = static_cast<uint16_t>(column);
        point.value.coordinate.y = static_cast<uint32_t>(row);
        if (ghostty_terminal_grid_ref_track(terminal_, point,
                                            &tracked->reference_)
                != GHOSTTY_SUCCESS
            || tracked->reference_ == nullptr) {
            return std::nullopt;
        }

        GhosttyGridRef snapshot{};
        snapshot.size = sizeof(snapshot);
        if (ghostty_tracked_grid_ref_snapshot(tracked->reference_, &snapshot)
            != GHOSTTY_SUCCESS) {
            return std::nullopt;
        }
        std::optional<QByteArray> uri = hyperlinkUri(snapshot);
        if (!uri.has_value() || uri->isEmpty()) {
            return std::nullopt;
        }
        tracked->uri_ = std::move(*uri);
        return TrackedHyperlink(std::move(tracked));
    }

    std::optional<QByteArray>
    currentTrackedHyperlinkUri(const TrackedHyperlink::Impl &target) const
    {
        if (target.reference_ == nullptr
            || target.owner_.get() != ownerToken_.get()) {
            return std::nullopt;
        }

        GhosttyGridRef snapshot{};
        snapshot.size = sizeof(snapshot);
        if (ghostty_tracked_grid_ref_snapshot(target.reference_, &snapshot)
            != GHOSTTY_SUCCESS) {
            return std::nullopt;
        }

        std::optional<QByteArray> uri = hyperlinkUri(snapshot);
        if (!uri.has_value() || uri->isEmpty() || *uri != target.uri_) {
            return std::nullopt;
        }
        return uri;
    }

    bool trackedHyperlinkValid(const TrackedHyperlink::Impl &target) const
    {
        return currentTrackedHyperlinkUri(target).has_value();
    }

    std::optional<HyperlinkMatch>
    resolveHyperlink(const TrackedHyperlink::Impl &target,
                     const QVector<QPoint> &candidateCells) const
    {
        if (target.reference_ == nullptr
            || target.owner_.get() != ownerToken_.get()) {
            return std::nullopt;
        }

        const std::optional<GhosttyTerminalScreen> screen = activeScreen();
        if (!screen.has_value() || *screen != target.screen_) {
            return std::nullopt;
        }

        GhosttyPointCoordinate coordinate{};
        if (ghostty_tracked_grid_ref_point(
                target.reference_, GHOSTTY_POINT_TAG_VIEWPORT, &coordinate)
                != GHOSTTY_SUCCESS
            || static_cast<int>(coordinate.x) >= geometry_.columns
            || coordinate.y > static_cast<uint32_t>(INT_MAX)
            || static_cast<int>(coordinate.y) >= geometry_.rows) {
            return std::nullopt;
        }

        const std::optional<QByteArray> uri =
            currentTrackedHyperlinkUri(target);
        if (!uri.has_value()) {
            return std::nullopt;
        }

        const QPoint targetCell(static_cast<int>(coordinate.x),
                                static_cast<int>(coordinate.y));
        HyperlinkMatch match;
        match.uri = *uri;
        match.targetCell = targetCell;
        match.cells.reserve(candidateCells.size());
        for (const QPoint &candidate : candidateCells) {
            const std::optional<QByteArray> candidateUri =
                hyperlinkUriAt(candidate.x(), candidate.y());
            if (candidateUri.has_value() && *candidateUri == match.uri) {
                match.cells.append(candidate);
            }
        }
        if (!match.cells.contains(targetCell)) {
            match.cells.append(targetCell);
        }
        return match;
    }

    bool gridRefAtScreen(const ScreenCell &cell, GhosttyGridRef *out) const
    {
        if (out == nullptr) {
            return false;
        }
        GhosttyPoint point{};
        point.tag = GHOSTTY_POINT_TAG_SCREEN;
        point.value.coordinate.x = cell.x;
        point.value.coordinate.y = cell.y;
        *out = GhosttyGridRef{};
        out->size = sizeof(*out);
        return ghostty_terminal_grid_ref(terminal_, point, out)
            == GHOSTTY_SUCCESS;
    }

    static bool appendUtf8Codepoint(QByteArray *output, uint32_t codepoint)
    {
        if (output == nullptr || codepoint > 0x10ffffU
            || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }

        char encoded[4];
        qsizetype length = 0;
        if (codepoint <= 0x7fU) {
            encoded[0] = static_cast<char>(codepoint);
            length = 1;
        } else if (codepoint <= 0x7ffU) {
            encoded[0] = static_cast<char>(0xc0U | (codepoint >> 6U));
            encoded[1] = static_cast<char>(0x80U | (codepoint & 0x3fU));
            length = 2;
        } else if (codepoint <= 0xffffU) {
            encoded[0] = static_cast<char>(0xe0U | (codepoint >> 12U));
            encoded[1] = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU));
            encoded[2] = static_cast<char>(0x80U | (codepoint & 0x3fU));
            length = 3;
        } else {
            encoded[0] = static_cast<char>(0xf0U | (codepoint >> 18U));
            encoded[1] =
                static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU));
            encoded[2] = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU));
            encoded[3] = static_cast<char>(0x80U | (codepoint & 0x3fU));
            length = 4;
        }
        if (output->size() > maximumLogicalLineBytes - length) {
            return false;
        }
        output->append(encoded, length);
        return true;
    }

    std::optional<QByteArray>
    graphemeUtf8(const GhosttyGridRef &reference) const
    {
        std::array<uint32_t, 16> storage{};
        size_t length = 0;
        GhosttyResult result = ghostty_grid_ref_graphemes(
            &reference, storage.data(), storage.size(), &length);

        QVector<uint32_t> dynamic;
        const uint32_t *codepoints = storage.data();
        if (result == GHOSTTY_OUT_OF_SPACE) {
            if (length > static_cast<size_t>(maximumLogicalLineBytes)
                || length > static_cast<size_t>(
                       std::numeric_limits<qsizetype>::max())) {
                return std::nullopt;
            }
            dynamic.resize(static_cast<qsizetype>(length));
            result = ghostty_grid_ref_graphemes(
                &reference, dynamic.data(), static_cast<size_t>(dynamic.size()),
                &length);
            codepoints = dynamic.constData();
        }
        if (result != GHOSTTY_SUCCESS
            || length
                > static_cast<size_t>(std::numeric_limits<qsizetype>::max())) {
            return std::nullopt;
        }

        QByteArray output;
        output.reserve(static_cast<qsizetype>(std::min<size_t>(
            length * 4U, static_cast<size_t>(maximumLogicalLineBytes))));
        for (size_t index = 0; index < length; ++index) {
            if (!appendUtf8Codepoint(&output, codepoints[index])) {
                return std::nullopt;
            }
        }
        return output;
    }

    template <typename Coordinate, typename ProjectCoordinate>
    std::optional<BasicTextMapData<Coordinate>>
    textMapBetweenMapped(ScreenCell start, ScreenCell end,
                         bool includeTrailingEmptyStorage,
                         ProjectCoordinate projectCoordinate) const
    {
        if (end < start) {
            std::swap(start, end);
        }

        uint16_t liveColumns = 0;
        if (ghostty_terminal_get(terminal_, GHOSTTY_TERMINAL_DATA_COLS,
                                 &liveColumns)
                != GHOSTTY_SUCCESS
            || liveColumns == 0) {
            return std::nullopt;
        }
        const int terminalColumns = static_cast<int>(liveColumns);
        const quint64 rowCount = static_cast<quint64>(end.y) - start.y + 1U;
        const quint64 columnCount = static_cast<quint64>(terminalColumns);
        if (rowCount > maximumLogicalLineCells
            || rowCount * columnCount > maximumLogicalLineCells) {
            return std::nullopt;
        }

        BasicTextMapData<Coordinate> data;
        QVector<Coordinate> pendingBlanks;
        const auto appendMapped = [&data](QByteArrayView bytes,
                                          const Coordinate &cell) {
            if (bytes.size() < 0
                || data.text.size() > maximumLogicalLineBytes - bytes.size()) {
                return false;
            }
            data.text.append(bytes.data(), bytes.size());
            const qsizetype oldSize = data.byteCells.size();
            if (oldSize > maximumLogicalLineBytes - bytes.size()) {
                return false;
            }
            data.byteCells.resize(oldSize + bytes.size());
            std::fill(data.byteCells.begin() + oldSize, data.byteCells.end(),
                      cell);
            return true;
        };
        const auto flushBlanks = [&pendingBlanks, &appendMapped]() {
            for (const Coordinate &blank : std::as_const(pendingBlanks)) {
                if (!appendMapped(QByteArrayView(" ", 1), blank)) {
                    return false;
                }
            }
            pendingBlanks.clear();
            return true;
        };

        uint32_t y = start.y;
        for (;;) {
            const int firstColumn = y == start.y ? start.x : 0;
            const int lastColumn = y == end.y ? end.x : terminalColumns - 1;
            if (firstColumn < 0 || lastColumn < firstColumn
                || lastColumn >= terminalColumns) {
                return std::nullopt;
            }

            // A screen-coordinate lookup may traverse the complete page
            // list. Resolve both endpoints and require one page/local row
            // before advancing the public x field through the validated
            // span. Calling gridRefAtScreen for every cell turns one history
            // row into columns x scrollback work, while validating only the
            // first point could forge an out-of-bounds ref on a narrow page
            // left behind by an incomplete reflow.
            GhosttyGridRef rowReference{};
            GhosttyGridRef lastReference{};
            if (!gridRefAtScreen(
                    ScreenCell{
                        .x = static_cast<uint16_t>(firstColumn),
                        .y = y,
                    },
                    &rowReference)
                || !gridRefAtScreen(
                    ScreenCell{
                        .x = static_cast<uint16_t>(lastColumn),
                        .y = y,
                    },
                    &lastReference)
                || rowReference.node != lastReference.node
                || rowReference.y != lastReference.y
                || rowReference.x != static_cast<uint16_t>(firstColumn)
                || lastReference.x != static_cast<uint16_t>(lastColumn)) {
                return std::nullopt;
            }
            for (int column = firstColumn; column <= lastColumn; ++column) {
                const ScreenCell cell{
                    .x = static_cast<uint16_t>(column),
                    .y = y,
                };
                const Coordinate mappedCell = projectCoordinate(cell);
                GhosttyGridRef reference = rowReference;
                reference.x = static_cast<uint16_t>(column);

                GhosttyCell rawCell = 0;
                GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
                if (ghostty_grid_ref_cell(&reference, &rawCell)
                        != GHOSTTY_SUCCESS
                    || ghostty_cell_get(rawCell, GHOSTTY_CELL_DATA_WIDE, &wide)
                        != GHOSTTY_SUCCESS) {
                    return std::nullopt;
                }
                if (wide == GHOSTTY_CELL_WIDE_SPACER_HEAD
                    || wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) {
                    continue;
                }

                const std::optional<QByteArray> grapheme =
                    graphemeUtf8(reference);
                if (!grapheme.has_value()) {
                    return std::nullopt;
                }
                if (grapheme->isEmpty()) {
                    if (pendingBlanks.size()
                        >= static_cast<qsizetype>(maximumLogicalLineCells)) {
                        return std::nullopt;
                    }
                    pendingBlanks.append(mappedCell);
                    continue;
                }

                if (!flushBlanks() || !appendMapped(*grapheme, mappedCell)) {
                    return std::nullopt;
                }
            }

            if (y == end.y) {
                break;
            }
            ++y;
        }

        // ScreenFormatter deliberately drops terminal storage cells that are
        // empty after the final text cell. Do the same for a complete logical
        // line. Endpoint-cell validation opts in to the pending spaces because
        // its final cell may be an empty storage cell that was followed by text
        // in the complete line snapshot.
        if (includeTrailingEmptyStorage && !flushBlanks()) {
            return std::nullopt;
        }
        if (data.byteCells.size() != data.text.size()) {
            return std::nullopt;
        }
        return data;
    }

    std::optional<TextMapData>
    textMapBetween(ScreenCell start, ScreenCell end,
                   bool includeTrailingEmptyStorage = false) const
    {
        return textMapBetweenMapped<ScreenCell>(
            start, end, includeTrailingEmptyStorage,
            [](const ScreenCell &cell) { return cell; });
    }

    std::optional<SearchRowTextMapData>
    textMapSearchRow(ScreenCell start, ScreenCell end,
                     bool includeTrailingEmptyStorage) const
    {
        return textMapBetweenMapped<quint16>(
            start, end, includeTrailingEmptyStorage,
            [](const ScreenCell &cell) { return cell.x; });
    }

    std::optional<LogicalLineSnapshot> snapshotLogicalLineAt(int column,
                                                             int row) const
    {
        GhosttyGridRef targetReference{};
        if (!pointToGridRefExact(column, row, &targetReference)) {
            return std::nullopt;
        }
        const std::optional<GhosttyTerminalScreen> screen = activeScreen();
        if (!screen.has_value()) {
            return std::nullopt;
        }

        // A non-null pointer with a zero count is the C API's explicit empty
        // whitespace set. This keeps literal spaces while avoiding Ghostty's
        // default line-edge trimming; empty storage cells are never emitted by
        // the plain formatter in either case.
        const uint32_t noWhitespace = 0;
        GhosttyTerminalSelectLineOptions options{};
        options.size = sizeof(options);
        options.ref = targetReference;
        options.whitespace = &noWhitespace;
        options.whitespace_len = 0;
        options.semantic_prompt_boundary = true;
        GhosttySelection selection{};
        selection.size = sizeof(selection);
        if (ghostty_terminal_select_line(terminal_, &options, &selection)
            != GHOSTTY_SUCCESS) {
            return std::nullopt;
        }

        GhosttySelection ordered{};
        ordered.size = sizeof(ordered);
        if (ghostty_terminal_selection_ordered(terminal_, &selection,
                                               GHOSTTY_SELECTION_ORDER_FORWARD,
                                               &ordered)
            != GHOSTTY_SUCCESS) {
            return std::nullopt;
        }

        GhosttyPointCoordinate startCoordinate{};
        GhosttyPointCoordinate endCoordinate{};
        GhosttyPointCoordinate targetCoordinate{};
        if (ghostty_terminal_point_from_grid_ref(terminal_, &ordered.start,
                                                 GHOSTTY_POINT_TAG_SCREEN,
                                                 &startCoordinate)
                != GHOSTTY_SUCCESS
            || ghostty_terminal_point_from_grid_ref(terminal_, &ordered.end,
                                                    GHOSTTY_POINT_TAG_SCREEN,
                                                    &endCoordinate)
                != GHOSTTY_SUCCESS
            || ghostty_terminal_point_from_grid_ref(terminal_, &targetReference,
                                                    GHOSTTY_POINT_TAG_SCREEN,
                                                    &targetCoordinate)
                != GHOSTTY_SUCCESS) {
            return std::nullopt;
        }

        std::optional<TextMapData> data =
            textMapBetween(ScreenCell{startCoordinate.x, startCoordinate.y},
                           ScreenCell{endCoordinate.x, endCoordinate.y});
        if (!data.has_value()) {
            return std::nullopt;
        }

        GhosttyCell targetCell = 0;
        GhosttyCellWide targetWide = GHOSTTY_CELL_WIDE_NARROW;
        if (ghostty_grid_ref_cell(&targetReference, &targetCell)
                != GHOSTTY_SUCCESS
            || ghostty_cell_get(targetCell, GHOSTTY_CELL_DATA_WIDE, &targetWide)
                != GHOSTTY_SUCCESS) {
            return std::nullopt;
        }
        ScreenCell mappedTarget{targetCoordinate.x, targetCoordinate.y};
        if (targetWide == GHOSTTY_CELL_WIDE_SPACER_TAIL && mappedTarget.x > 0) {
            --mappedTarget.x;
        }

        qsizetype targetByteOffset = -1;
        for (qsizetype index = 0; index < data->byteCells.size(); ++index) {
            if (data->byteCells.at(index) == mappedTarget) {
                targetByteOffset = index;
                break;
            }
        }

        auto impl = std::make_unique<LogicalLineSnapshot::Impl>(
            ownerToken_, *screen, std::move(*data), targetByteOffset,
            ScreenCell{targetCoordinate.x, targetCoordinate.y}, mappedTarget,
            targetWide == GHOSTTY_CELL_WIDE_SPACER_HEAD);
        return LogicalLineSnapshot(std::move(impl));
    }

    bool trackScreenCell(const ScreenCell &cell,
                         GhosttyTrackedGridRef *out) const
    {
        if (out == nullptr) {
            return false;
        }
        *out = nullptr;
        GhosttyPoint point{};
        point.tag = GHOSTTY_POINT_TAG_SCREEN;
        point.value.coordinate.x = cell.x;
        point.value.coordinate.y = cell.y;
        return ghostty_terminal_grid_ref_track(terminal_, point, out)
            == GHOSTTY_SUCCESS
            && *out != nullptr;
    }

    std::optional<TrackedTextRange>
    trackTextRange(const LogicalLineSnapshot::Impl &line, qsizetype beginByte,
                   qsizetype endByte) const
    {
        if (line.owner_.get() != ownerToken_.get() || beginByte < 0
            || endByte <= beginByte || endByte > line.data_.text.size()
            || line.data_.byteCells.size() != line.data_.text.size()) {
            return std::nullopt;
        }
        if (!line.byteRangeContainsTarget(beginByte, endByte)) {
            return std::nullopt;
        }
        const std::optional<GhosttyTerminalScreen> screen = activeScreen();
        if (!screen.has_value() || *screen != line.screen_) {
            return std::nullopt;
        }

        const ScreenCell startCell = line.data_.byteCells.at(beginByte);
        const ScreenCell endCell = line.data_.byteCells.at(endByte - 1);
        auto tracked =
            std::make_unique<TrackedTextRange::Impl>(ownerToken_, line.screen_);
        if (!trackScreenCell(startCell, &tracked->start_)
            || !trackScreenCell(endCell, &tracked->end_)
            || !trackScreenCell(line.targetCell_, &tracked->target_)) {
            return std::nullopt;
        }

        const std::optional<TextMapData> covered =
            textMapBetween(startCell, endCell, true);
        if (!covered.has_value() || covered->text.isEmpty()) {
            return std::nullopt;
        }
        tracked->matchedText_ =
            line.data_.text.sliced(beginByte, endByte - beginByte);
        tracked->coveredText_ = covered->text;
        return TrackedTextRange(std::move(tracked));
    }

    bool
    trackedTextRangeEndpointsValid(const TrackedTextRange::Impl &range) const
    {
        return range.owner_.get() == ownerToken_.get()
            && range.start_ != nullptr && range.end_ != nullptr
            && range.target_ != nullptr
            && ghostty_tracked_grid_ref_has_value(range.start_)
            && ghostty_tracked_grid_ref_has_value(range.end_)
            && ghostty_tracked_grid_ref_has_value(range.target_);
    }

    std::optional<TextMapData>
    currentTextRangeData(const TrackedTextRange::Impl &range) const
    {
        if (!trackedTextRangeEndpointsValid(range)) {
            return std::nullopt;
        }
        const std::optional<GhosttyTerminalScreen> screen = activeScreen();
        if (!screen.has_value() || *screen != range.screen_) {
            return std::nullopt;
        }

        GhosttyPointCoordinate start{};
        GhosttyPointCoordinate end{};
        if (ghostty_tracked_grid_ref_point(range.start_,
                                           GHOSTTY_POINT_TAG_SCREEN, &start)
                != GHOSTTY_SUCCESS
            || ghostty_tracked_grid_ref_point(range.end_,
                                              GHOSTTY_POINT_TAG_SCREEN, &end)
                != GHOSTTY_SUCCESS) {
            return std::nullopt;
        }
        return textMapBetween(ScreenCell{start.x, start.y},
                              ScreenCell{end.x, end.y}, true);
    }

    bool trackedTextRangeValid(const TrackedTextRange::Impl &range) const
    {
        if (!trackedTextRangeEndpointsValid(range)) {
            return false;
        }
        const std::optional<GhosttyTerminalScreen> screen = activeScreen();
        if (!screen.has_value()) {
            return false;
        }
        if (*screen != range.screen_) {
            return true;
        }
        const std::optional<TextMapData> current = currentTextRangeData(range);
        return current.has_value() && current->text == range.coveredText_;
    }

    bool installTextRange(const TrackedTextRange::Impl &range)
    {
        const std::optional<TextMapData> current = currentTextRangeData(range);
        if (!current.has_value() || current->text != range.coveredText_) {
            return false;
        }

        GhosttySelection selection{};
        selection.size = sizeof(selection);
        selection.start.size = sizeof(selection.start);
        selection.end.size = sizeof(selection.end);
        if (ghostty_tracked_grid_ref_snapshot(range.start_, &selection.start)
                != GHOSTTY_SUCCESS
            || ghostty_tracked_grid_ref_snapshot(range.end_, &selection.end)
                != GHOSTTY_SUCCESS) {
            return false;
        }
        selection.rectangle = false;
        return installSelection(selection);
    }

    std::optional<TextRangeMatch>
    resolveTextRange(const TrackedTextRange::Impl &range) const
    {
        const std::optional<TextMapData> current = currentTextRangeData(range);
        if (!current.has_value() || current->text != range.coveredText_) {
            return std::nullopt;
        }

        TextRangeMatch match;
        match.text = range.matchedText_;
        GhosttyPointCoordinate targetViewport{};
        if (ghostty_tracked_grid_ref_point(
                range.target_, GHOSTTY_POINT_TAG_VIEWPORT, &targetViewport)
                != GHOSTTY_SUCCESS
            || static_cast<int>(targetViewport.x) >= geometry_.columns
            || targetViewport.y >= static_cast<uint32_t>(geometry_.rows)) {
            return std::nullopt;
        }
        match.targetCell =
            QPoint(targetViewport.x, static_cast<int>(targetViewport.y));

        GhosttyGridRef targetSnapshot{};
        targetSnapshot.size = sizeof(targetSnapshot);
        if (ghostty_tracked_grid_ref_snapshot(range.target_, &targetSnapshot)
            != GHOSTTY_SUCCESS) {
            return std::nullopt;
        }
        const uint32_t noWhitespace = 0;
        GhosttyTerminalSelectLineOptions lineOptions{};
        lineOptions.size = sizeof(lineOptions);
        lineOptions.ref = targetSnapshot;
        lineOptions.whitespace = &noWhitespace;
        lineOptions.whitespace_len = 0;
        lineOptions.semantic_prompt_boundary = true;
        GhosttySelection lineSelection{};
        lineSelection.size = sizeof(lineSelection);
        GhosttySelection orderedLine{};
        orderedLine.size = sizeof(orderedLine);
        if (ghostty_terminal_select_line(terminal_, &lineOptions,
                                         &lineSelection)
                != GHOSTTY_SUCCESS
            || ghostty_terminal_selection_ordered(
                   terminal_, &lineSelection, GHOSTTY_SELECTION_ORDER_FORWARD,
                   &orderedLine)
                != GHOSTTY_SUCCESS) {
            return std::nullopt;
        }
        GhosttyPointCoordinate lineStart{};
        GhosttyPointCoordinate lineEnd{};
        if (ghostty_terminal_point_from_grid_ref(terminal_, &orderedLine.start,
                                                 GHOSTTY_POINT_TAG_SCREEN,
                                                 &lineStart)
                != GHOSTTY_SUCCESS
            || ghostty_terminal_point_from_grid_ref(terminal_, &orderedLine.end,
                                                    GHOSTTY_POINT_TAG_SCREEN,
                                                    &lineEnd)
                != GHOSTTY_SUCCESS) {
            return std::nullopt;
        }
        if (lineEnd.y < lineStart.y) {
            return std::nullopt;
        }
        const quint64 logicalRowCount =
            static_cast<quint64>(lineEnd.y) - lineStart.y + 1U;
        const quint64 logicalColumnCount =
            static_cast<quint64>(std::max(geometry_.columns, 1));
        if (logicalRowCount > maximumLogicalLineCells
            || logicalRowCount * logicalColumnCount > maximumLogicalLineCells) {
            return std::nullopt;
        }
        uint32_t lineY = lineStart.y;
        for (;;) {
            GhosttyGridRef rowReference{};
            GhosttyPointCoordinate viewportRow{};
            if (gridRefAtScreen(ScreenCell{0, lineY}, &rowReference)
                && ghostty_terminal_point_from_grid_ref(
                       terminal_, &rowReference, GHOSTTY_POINT_TAG_VIEWPORT,
                       &viewportRow)
                    == GHOSTTY_SUCCESS
                && viewportRow.y < static_cast<uint32_t>(geometry_.rows)) {
                const int visibleRow = static_cast<int>(viewportRow.y);
                if (match.logicalLineRows.isEmpty()
                    || match.logicalLineRows.constLast() != visibleRow) {
                    match.logicalLineRows.append(visibleRow);
                }
            }
            if (lineY == lineEnd.y) {
                break;
            }
            ++lineY;
        }

        QSet<quint64> visitedScreenCells;
        QSet<quint64> visitedViewportCells;
        for (const ScreenCell &cell : current->byteCells) {
            const quint64 screenKey =
                (static_cast<quint64>(cell.y) << 16U) | cell.x;
            if (visitedScreenCells.contains(screenKey)) {
                continue;
            }
            visitedScreenCells.insert(screenKey);

            GhosttyGridRef reference{};
            GhosttyPointCoordinate viewport{};
            if (!gridRefAtScreen(cell, &reference)
                || ghostty_terminal_point_from_grid_ref(
                       terminal_, &reference, GHOSTTY_POINT_TAG_VIEWPORT,
                       &viewport)
                    != GHOSTTY_SUCCESS
                || static_cast<int>(viewport.x) >= geometry_.columns
                || viewport.y >= static_cast<uint32_t>(geometry_.rows)) {
                continue;
            }

            GhosttyCell rawCell = 0;
            GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
            if (ghostty_grid_ref_cell(&reference, &rawCell) != GHOSTTY_SUCCESS
                || ghostty_cell_get(rawCell, GHOSTTY_CELL_DATA_WIDE, &wide)
                    != GHOSTTY_SUCCESS) {
                return std::nullopt;
            }

            const auto appendViewportCell = [&](int x, int y) {
                const quint64 key =
                    (static_cast<quint64>(y) << 32U) | static_cast<quint32>(x);
                if (!visitedViewportCells.contains(key)) {
                    visitedViewportCells.insert(key);
                    match.cells.append(QPoint(x, y));
                }
            };
            appendViewportCell(viewport.x, static_cast<int>(viewport.y));
            if (wide == GHOSTTY_CELL_WIDE_WIDE
                && static_cast<int>(viewport.x) + 1 < geometry_.columns) {
                appendViewportCell(static_cast<int>(viewport.x) + 1,
                                   static_cast<int>(viewport.y));
            }
        }

        // String formatting emits no byte for a right-edge spacer head, but
        // Ghostty's link hit test treats it as part of a selection whose
        // mapped endpoints straddle that cell. Preserve the queried target in
        // the visible range so the GUI can keep pointer and decoration state
        // coherent for that exact edge cell.
        if (!match.cells.contains(match.targetCell)) {
            qsizetype insertionIndex = 0;
            while (insertionIndex < match.cells.size()) {
                const QPoint &cell = match.cells.at(insertionIndex);
                if (cell.y() > match.targetCell.y()
                    || (cell.y() == match.targetCell.y()
                        && cell.x() > match.targetCell.x())) {
                    break;
                }
                ++insertionIndex;
            }
            match.cells.insert(insertionIndex, match.targetCell);
        }

        if (match.cells.isEmpty()) {
            return std::nullopt;
        }
        return match;
    }

    std::optional<HyperlinkMatch>
    hyperlinkAt(int column, int row,
                const QVector<QPoint> &candidateCells) const
    {
        const std::optional<QByteArray> uri = hyperlinkUriAt(column, row);
        if (!uri.has_value() || uri->isEmpty()) {
            return std::nullopt;
        }

        HyperlinkMatch match;
        match.uri = *uri;
        match.targetCell = QPoint(column, row);
        match.cells.reserve(candidateCells.size());
        for (const QPoint &candidate : candidateCells) {
            const std::optional<QByteArray> candidateUri =
                hyperlinkUriAt(candidate.x(), candidate.y());
            if (candidateUri.has_value() && *candidateUri == match.uri) {
                match.cells.append(candidate);
            }
        }
        if (!match.cells.contains(QPoint(column, row))) {
            match.cells.append(QPoint(column, row));
        }
        return match;
    }

    static bool
    setSelectionWordBoundaries(GhosttySelectionGestureEvent event,
                               const QVector<uint32_t> &wordBoundaryCodepoints)
    {
        if (wordBoundaryCodepoints.isEmpty()) {
            return ghostty_selection_gesture_event_set(
                       event,
                       GHOSTTY_SELECTION_GESTURE_EVENT_OPT_WORD_BOUNDARY_CODEPOINTS,
                       nullptr)
                == GHOSTTY_SUCCESS;
        }
        const GhosttyCodepoints codepoints{
            .ptr = wordBoundaryCodepoints.constData(),
            .len = static_cast<size_t>(wordBoundaryCodepoints.size()),
        };
        return ghostty_selection_gesture_event_set(
                   event,
                   GHOSTTY_SELECTION_GESTURE_EVENT_OPT_WORD_BOUNDARY_CODEPOINTS,
                   &codepoints)
            == GHOSTTY_SUCCESS;
    }

    bool setSelectionWordChars(const QVector<uint32_t> &wordBoundaryCodepoints)
    {
        if ((!wordBoundaryCodepoints.isEmpty()
             && wordBoundaryCodepoints.front() != 0)
            || !std::all_of(
                wordBoundaryCodepoints.cbegin(), wordBoundaryCodepoints.cend(),
                [](uint32_t codepoint) {
                    return codepoint <= 0x10ffffU
                        && !(codepoint >= 0xd800U && codepoint <= 0xdfffU);
                })) {
            return false;
        }
        if (selectionWordChars_ == wordBoundaryCodepoints) {
            return true;
        }

        QVector<uint32_t> replacementWordChars = wordBoundaryCodepoints;
        GhosttySelectionGestureEvent replacementPressEvent = nullptr;
        GhosttySelectionGestureEvent replacementDragEvent = nullptr;
        GhosttySelectionGestureEvent replacementAutoscrollTickEvent = nullptr;
        const auto replacementGuard = qScopeGuard([&] {
            if (replacementAutoscrollTickEvent != nullptr) {
                ghostty_selection_gesture_event_free(
                    replacementAutoscrollTickEvent);
            }
            if (replacementDragEvent != nullptr) {
                ghostty_selection_gesture_event_free(replacementDragEvent);
            }
            if (replacementPressEvent != nullptr) {
                ghostty_selection_gesture_event_free(replacementPressEvent);
            }
        });
        if (ghostty_selection_gesture_event_new(
                nullptr, &replacementPressEvent,
                GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_PRESS)
                != GHOSTTY_SUCCESS
            || ghostty_selection_gesture_event_new(
                   nullptr, &replacementDragEvent,
                   GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_DRAG)
                != GHOSTTY_SUCCESS
            || ghostty_selection_gesture_event_new(
                   nullptr, &replacementAutoscrollTickEvent,
                   GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_AUTOSCROLL_TICK)
                != GHOSTTY_SUCCESS) {
            return false;
        }
        if (!setSelectionWordBoundaries(replacementPressEvent,
                                        wordBoundaryCodepoints)
            || !setSelectionWordBoundaries(replacementDragEvent,
                                           wordBoundaryCodepoints)
            || !setSelectionWordBoundaries(replacementAutoscrollTickEvent,
                                           wordBoundaryCodepoints)) {
            return false;
        }

        std::swap(selectionPressEvent_, replacementPressEvent);
        std::swap(selectionDragEvent_, replacementDragEvent);
        std::swap(selectionAutoscrollTickEvent_,
                  replacementAutoscrollTickEvent);
        selectionWordChars_.swap(replacementWordChars);
        return true;
    }

    bool setClickRepeatIntervalMilliseconds(quint32 milliseconds)
    {
        if (milliseconds == 0) {
            return false;
        }
        clickRepeatIntervalMilliseconds_ = milliseconds;
        return true;
    }

    bool beginSelection(const TerminalSelectionPressInput &input)
    {
        if (!std::isfinite(input.surfaceX) || !std::isfinite(input.surfaceY)) {
            return false;
        }

        GhosttyGridRef reference{};
        if (!pointToGridRef(input.column, input.row, &reference)) {
            return false;
        }

        const bool hadSelection = hasSelection();
        const quint64 repeatIntervalNanoseconds =
            static_cast<quint64>(clickRepeatIntervalMilliseconds_) * 1'000'000;
        uint8_t retainedClickCount = 0;
        if (input.extendExistingSelection && hadSelection
            && input.timestampValid
            && lastSelectionPressTimestampNanoseconds_.has_value()
            && input.timestampNanoseconds
                > *lastSelectionPressTimestampNanoseconds_
            && input.timestampNanoseconds
                    - *lastSelectionPressTimestampNanoseconds_
                > repeatIntervalNanoseconds
            && ghostty_selection_gesture_get(
                   selectionGesture_, terminal_,
                   GHOSTTY_SELECTION_GESTURE_DATA_CLICK_COUNT,
                   &retainedClickCount)
                == GHOSTTY_SUCCESS
            && retainedClickCount > 0) {
            // Ghostty treats a delayed released-Shift press as motion of the
            // retained gesture, preserving its anchor, behavior, count, and
            // timestamp. A drag with no value is still consumed by this
            // branch; it must not fall through and create a new press.
            return updateSelection({
                .column = input.column,
                .row = input.row,
                .surfaceX = input.surfaceX,
                .surfaceY = input.surfaceY,
                .rectangular = input.rectangular,
            });
        }

        const GhosttySurfacePosition position{
            .x = input.surfaceX,
            .y = input.surfaceY,
        };
        const double repeatDistance =
            static_cast<double>(geometry_.cellWidthPixels);
        const GhosttySelectionGestureBehaviors behaviors{
            .single_click = GHOSTTY_SELECTION_GESTURE_BEHAVIOR_CELL,
            .double_click = GHOSTTY_SELECTION_GESTURE_BEHAVIOR_WORD,
            .triple_click = input.controlModifier
                ? GHOSTTY_SELECTION_GESTURE_BEHAVIOR_OUTPUT
                : GHOSTTY_SELECTION_GESTURE_BEHAVIOR_LINE,
        };
        const void *const timestamp = input.timestampValid
            ? static_cast<const void *>(&input.timestampNanoseconds)
            : nullptr;
        if (ghostty_selection_gesture_event_set(
                selectionPressEvent_, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF,
                &reference)
                != GHOSTTY_SUCCESS
            || ghostty_selection_gesture_event_set(
                   selectionPressEvent_,
                   GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION, &position)
                != GHOSTTY_SUCCESS
            || ghostty_selection_gesture_event_set(
                   selectionPressEvent_,
                   GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REPEAT_DISTANCE,
                   &repeatDistance)
                != GHOSTTY_SUCCESS
            || ghostty_selection_gesture_event_set(
                   selectionPressEvent_,
                   GHOSTTY_SELECTION_GESTURE_EVENT_OPT_TIME_NS, timestamp)
                != GHOSTTY_SUCCESS
            || ghostty_selection_gesture_event_set(
                   selectionPressEvent_,
                   GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REPEAT_INTERVAL_NS,
                   &repeatIntervalNanoseconds)
                != GHOSTTY_SUCCESS
            || ghostty_selection_gesture_event_set(
                   selectionPressEvent_,
                   GHOSTTY_SELECTION_GESTURE_EVENT_OPT_BEHAVIORS, &behaviors)
                != GHOSTTY_SUCCESS) {
            return false;
        }

        GhosttySelection selection{};
        selection.size = sizeof(selection);
        const GhosttyResult result = ghostty_selection_gesture_event(
            selectionGesture_, terminal_, selectionPressEvent_, &selection);
        if (result == GHOSTTY_SUCCESS || result == GHOSTTY_NO_VALUE) {
            if (input.timestampValid) {
                lastSelectionPressTimestampNanoseconds_ =
                    input.timestampNanoseconds;
            } else {
                lastSelectionPressTimestampNanoseconds_.reset();
            }
        } else {
            // The gesture may have failed after partially mutating its
            // implementation. Never compare a later Shift press with stale
            // frontend timing in that uncertain state.
            lastSelectionPressTimestampNanoseconds_.reset();
        }
        if (result == GHOSTTY_SUCCESS) {
            return installSelection(selection);
        }
        uint8_t clickCount = 0;
        if (result == GHOSTTY_NO_VALUE
            && ghostty_selection_gesture_get(
                   selectionGesture_, terminal_,
                   GHOSTTY_SELECTION_GESTURE_DATA_CLICK_COUNT, &clickCount)
                == GHOSTTY_SUCCESS
            && clickCount == 1 && hadSelection) {
            // A single press creates the drag anchor without returning an
            // installed range. Match Ghostty's surface lifecycle by clearing
            // the old range without resetting that new gesture state.
            return ghostty_terminal_set(terminal_,
                                        GHOSTTY_TERMINAL_OPT_SELECTION, nullptr)
                == GHOSTTY_SUCCESS;
        }
        return false;
    }

    bool updateSelection(const TerminalSelectionDragInput &input)
    {
        if (!std::isfinite(input.surfaceX) || !std::isfinite(input.surfaceY)) {
            return false;
        }

        GhosttyGridRef anchor{};
        if (ghostty_selection_gesture_get(selectionGesture_, terminal_,
                                          GHOSTTY_SELECTION_GESTURE_DATA_ANCHOR,
                                          &anchor)
            != GHOSTTY_SUCCESS) {
            // Ghostty preserves the installed selection when the tracked
            // press belongs to an inactive or replaced screen.
            return false;
        }

        GhosttyGridRef end{};
        if (!pointToGridRef(input.column, input.row, &end)) {
            return false;
        }

        const GhosttySurfacePosition position{
            .x = input.surfaceX,
            .y = input.surfaceY,
        };
        const GhosttySelectionGestureGeometry geometry{
            .columns = boundedU32(geometry_.columns),
            .cell_width = boundedU32(geometry_.cellWidthPixels),
            .padding_left = nonnegativeU32(geometry_.padding.left),
            .screen_height = boundedU32(geometry_.surfaceHeightPixels),
        };
        if (ghostty_selection_gesture_event_set(
                selectionDragEvent_, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF,
                &end)
                != GHOSTTY_SUCCESS
            || ghostty_selection_gesture_event_set(
                   selectionDragEvent_,
                   GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION, &position)
                != GHOSTTY_SUCCESS
            || ghostty_selection_gesture_event_set(
                   selectionDragEvent_,
                   GHOSTTY_SELECTION_GESTURE_EVENT_OPT_GEOMETRY, &geometry)
                != GHOSTTY_SUCCESS
            || ghostty_selection_gesture_event_set(
                   selectionDragEvent_,
                   GHOSTTY_SELECTION_GESTURE_EVENT_OPT_RECTANGLE,
                   &input.rectangular)
                != GHOSTTY_SUCCESS) {
            return false;
        }

        GhosttySelection selection{};
        selection.size = sizeof(selection);
        const GhosttyResult result = ghostty_selection_gesture_event(
            selectionGesture_, terminal_, selectionDragEvent_, &selection);
        if (result == GHOSTTY_SUCCESS) {
            return installSelection(selection);
        }
        if (result == GHOSTTY_NO_VALUE) {
            // A valid gesture that has not crossed its selection threshold
            // collapses the installed range. The anchor check above
            // distinguishes this from an invalid-screen no-op.
            return ghostty_terminal_set(terminal_,
                                        GHOSTTY_TERMINAL_OPT_SELECTION, nullptr)
                == GHOSTTY_SUCCESS;
        }
        return false;
    }

    SelectionAutoscrollDirection selectionAutoscrollDirection() const
    {
        GhosttySelectionGestureAutoscroll direction =
            GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_NONE;
        if (ghostty_selection_gesture_get(
                selectionGesture_, terminal_,
                GHOSTTY_SELECTION_GESTURE_DATA_AUTOSCROLL, &direction)
            != GHOSTTY_SUCCESS) {
            return SelectionAutoscrollDirection::None;
        }

        switch (direction) {
        case GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_UP:
            return SelectionAutoscrollDirection::Up;
        case GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_DOWN:
            return SelectionAutoscrollDirection::Down;
        case GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_NONE:
        default: return SelectionAutoscrollDirection::None;
        }
    }

    SelectionAutoscrollTickResult
    selectionAutoscrollTick(const TerminalSelectionDragInput &input)
    {
        if (selectionAutoscrollDirection() == SelectionAutoscrollDirection::None
            || !std::isfinite(input.surfaceX)
            || !std::isfinite(input.surfaceY)) {
            return SelectionAutoscrollTickResult::Unavailable;
        }

        const auto clampedCell = [](double surfacePosition, int padding,
                                    int cellSize, int count) {
            const double gridPosition =
                std::max(0.0, surfacePosition - static_cast<double>(padding))
                / static_cast<double>(cellSize);
            if (!std::isfinite(gridPosition)
                || gridPosition >= static_cast<double>(count)) {
                return count - 1;
            }
            return static_cast<int>(gridPosition);
        };
        const GhosttyPointCoordinate viewport{
            .x = static_cast<uint16_t>(
                clampedCell(input.surfaceX, geometry_.padding.left,
                            geometry_.cellWidthPixels, geometry_.columns)),
            .y = static_cast<uint32_t>(
                clampedCell(input.surfaceY, geometry_.padding.top,
                            geometry_.cellHeightPixels, geometry_.rows)),
        };
        const GhosttySurfacePosition position{
            .x = input.surfaceX,
            .y = input.surfaceY,
        };
        const GhosttySelectionGestureGeometry geometry{
            .columns = boundedU32(geometry_.columns),
            .cell_width = boundedU32(geometry_.cellWidthPixels),
            .padding_left = nonnegativeU32(geometry_.padding.left),
            .screen_height = boundedU32(geometry_.surfaceHeightPixels),
        };
        if (ghostty_selection_gesture_event_set(
                selectionAutoscrollTickEvent_,
                GHOSTTY_SELECTION_GESTURE_EVENT_OPT_VIEWPORT, &viewport)
                != GHOSTTY_SUCCESS
            || ghostty_selection_gesture_event_set(
                   selectionAutoscrollTickEvent_,
                   GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION, &position)
                != GHOSTTY_SUCCESS
            || ghostty_selection_gesture_event_set(
                   selectionAutoscrollTickEvent_,
                   GHOSTTY_SELECTION_GESTURE_EVENT_OPT_GEOMETRY, &geometry)
                != GHOSTTY_SUCCESS
            || ghostty_selection_gesture_event_set(
                   selectionAutoscrollTickEvent_,
                   GHOSTTY_SELECTION_GESTURE_EVENT_OPT_RECTANGLE,
                   &input.rectangular)
                != GHOSTTY_SUCCESS) {
            return SelectionAutoscrollTickResult::Unavailable;
        }

        GhosttySelection selection{};
        selection.size = sizeof(selection);
        const GhosttyResult result = ghostty_selection_gesture_event(
            selectionGesture_, terminal_, selectionAutoscrollTickEvent_,
            &selection);
        if (result == GHOSTTY_SUCCESS) {
            // The tick has already applied its viewport operation. Preserve
            // that mutation result even if installing the returned snapshot
            // unexpectedly fails so the caller still publishes the viewport.
            installSelection(selection);
            return SelectionAutoscrollTickResult::Mutated;
        }
        if (result != GHOSTTY_NO_VALUE) {
            return SelectionAutoscrollTickResult::Unavailable;
        }

        uint8_t clickCount = 0;
        if (ghostty_selection_gesture_get(
                selectionGesture_, terminal_,
                GHOSTTY_SELECTION_GESTURE_DATA_CLICK_COUNT, &clickCount)
                != GHOSTTY_SUCCESS
            || clickCount == 0) {
            // Ghostty resets an invalidated anchor without scrolling. Leave
            // the previously installed selection intact.
            return SelectionAutoscrollTickResult::Unavailable;
        }

        // A live gesture that produces no selection has still scrolled the
        // viewport. Match Ghostty's Surface.setSelection(null) behavior.
        clearSelection();
        return SelectionAutoscrollTickResult::Mutated;
    }

    void endSelection(int column, int row)
    {
        GhosttyGridRef reference{};
        const void *value =
            pointToGridRef(column, row, &reference) ? &reference : nullptr;
        if (ghostty_selection_gesture_event_set(
                selectionReleaseEvent_, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF,
                value)
            == GHOSTTY_SUCCESS) {
            ghostty_selection_gesture_event(selectionGesture_, terminal_,
                                            selectionReleaseEvent_, nullptr);
        }
    }

    bool selectionGestureDragged() const
    {
        bool dragged = false;
        return ghostty_selection_gesture_get(
                   selectionGesture_, terminal_,
                   GHOSTTY_SELECTION_GESTURE_DATA_DRAGGED, &dragged)
            == GHOSTTY_SUCCESS
            && dragged;
    }

    bool installSelection(const GhosttySelection &selection)
    {
        return ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_SELECTION,
                                    &selection)
            == GHOSTTY_SUCCESS;
    }

    bool selectionContains(int column, int row) const
    {
        if (column < 0 || column >= geometry_.columns || row < 0
            || row >= geometry_.rows) {
            return false;
        }

        GhosttySelection selection{};
        selection.size = sizeof(selection);
        if (ghostty_terminal_get(terminal_, GHOSTTY_TERMINAL_DATA_SELECTION,
                                 &selection)
            != GHOSTTY_SUCCESS) {
            return false;
        }

        GhosttyPoint point{};
        point.tag = GHOSTTY_POINT_TAG_VIEWPORT;
        point.value.coordinate.x = static_cast<uint16_t>(column);
        point.value.coordinate.y = static_cast<uint32_t>(row);
        bool contains = false;
        return ghostty_terminal_selection_contains(terminal_, &selection, point,
                                                   &contains)
            == GHOSTTY_SUCCESS
            && contains;
    }

    bool selectCell(int column, int row)
    {
        GhosttyGridRef reference{};
        if (!pointToGridRefExact(column, row, &reference)) {
            return false;
        }

        GhosttySelection selection{};
        selection.size = sizeof(selection);
        selection.start = reference;
        selection.end = reference;
        selection.rectangle = false;
        return installSelection(selection);
    }

    bool selectWord(int column, int row)
    {
        GhosttyGridRef reference{};
        if (!pointToGridRefExact(column, row, &reference)) {
            return false;
        }

        GhosttyTerminalSelectWordOptions options{};
        options.size = sizeof(options);
        options.ref = reference;
        if (!selectionWordChars_.isEmpty()) {
            options.boundary_codepoints = selectionWordChars_.constData();
            options.boundary_codepoints_len =
                static_cast<size_t>(selectionWordChars_.size());
        }

        GhosttySelection selection{};
        selection.size = sizeof(selection);
        return ghostty_terminal_select_word(terminal_, &options, &selection)
            == GHOSTTY_SUCCESS
            && installSelection(selection);
    }

    bool selectAll()
    {
        GhosttySelection selection{};
        selection.size = sizeof(selection);
        const GhosttyResult result =
            ghostty_terminal_select_all(terminal_, &selection);
        if (result != GHOSTTY_SUCCESS) {
            return false;
        }
        return installSelection(selection);
    }

    bool adjustSelection(TerminalSelectionAdjustment adjustment)
    {
        GhosttySelectionAdjust ghosttyAdjustment =
            GHOSTTY_SELECTION_ADJUST_LEFT;
        if (!toGhosttySelectionAdjustment(adjustment, &ghosttyAdjustment)) {
            return false;
        }

        GhosttySelection selection{};
        selection.size = sizeof(selection);
        if (ghostty_terminal_get(terminal_, GHOSTTY_TERMINAL_DATA_SELECTION,
                                 &selection)
                != GHOSTTY_SUCCESS
            || ghostty_terminal_selection_adjust(terminal_, &selection,
                                                 ghosttyAdjustment)
                != GHOSTTY_SUCCESS) {
            return false;
        }

        // Capture the adjusted logical endpoint before installation. Installing
        // the snapshot mutates the terminal and invalidates its untracked refs.
        GhosttyPointCoordinate endpoint{};
        if (ghostty_terminal_point_from_grid_ref(
                terminal_, &selection.end, GHOSTTY_POINT_TAG_SCREEN, &endpoint)
                != GHOSTTY_SUCCESS
            || !installSelection(selection)) {
            return false;
        }

        scrollEndpointIntoView(endpoint.y);
        return true;
    }

    bool scrollViewport(const TerminalViewportRequest &request)
    {
        GhosttyTerminalScrollViewport scroll{};
        switch (request.kind) {
        case TerminalViewportRequest::Kind::Top:
            scroll.tag = GHOSTTY_SCROLL_VIEWPORT_TOP;
            break;
        case TerminalViewportRequest::Kind::Bottom:
            scroll.tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM;
            break;
        case TerminalViewportRequest::Kind::Delta:
            if (request.delta == 0) return false;
            if (request.delta
                    < static_cast<qint64>(std::numeric_limits<intptr_t>::min())
                || request.delta > static_cast<qint64>(
                       std::numeric_limits<intptr_t>::max())) {
                return false;
            }
            scroll.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
            scroll.value.delta = static_cast<intptr_t>(request.delta);
            break;
        case TerminalViewportRequest::Kind::Row:
            scroll.tag = GHOSTTY_SCROLL_VIEWPORT_ROW;
            scroll.value.row = boundedRow(request.row);
            break;
        case TerminalViewportRequest::Kind::Selection: {
            GhosttySelection selection{};
            selection.size = sizeof(selection);
            GhosttySelection ordered{};
            ordered.size = sizeof(ordered);
            if (ghostty_terminal_get(terminal_, GHOSTTY_TERMINAL_DATA_SELECTION,
                                     &selection)
                    != GHOSTTY_SUCCESS
                || ghostty_terminal_selection_ordered(
                       terminal_, &selection, GHOSTTY_SELECTION_ORDER_FORWARD,
                       &ordered)
                    != GHOSTTY_SUCCESS) {
                return false;
            }

            GhosttyPointCoordinate topLeft{};
            if (ghostty_terminal_point_from_grid_ref(terminal_, &ordered.start,
                                                     GHOSTTY_POINT_TAG_SCREEN,
                                                     &topLeft)
                != GHOSTTY_SUCCESS) {
                return false;
            }
            scroll.tag = GHOSTTY_SCROLL_VIEWPORT_ROW;
            scroll.value.row = static_cast<size_t>(topLeft.y);
            break;
        }
        default: return false;
        }

        ghostty_terminal_scroll_viewport(terminal_, scroll);
        return true;
    }

    std::optional<SearchExtent> searchExtent() const
    {
        size_t totalRows = 0;
        uint16_t columns = 0;
        uint16_t rows = 0;
        GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
        GhosttyTerminalScrollbar scrollbar{};
        const GhosttyTerminalData keys[] = {
            GHOSTTY_TERMINAL_DATA_TOTAL_ROWS,
            GHOSTTY_TERMINAL_DATA_COLS,
            GHOSTTY_TERMINAL_DATA_ROWS,
            GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN,
            GHOSTTY_TERMINAL_DATA_SCROLLBAR,
        };
        void *values[] = {
            &totalRows, &columns, &rows, &screen, &scrollbar,
        };
        size_t written = 0;
        if (ghostty_terminal_get_multi(terminal_, std::size(keys), keys, values,
                                       &written)
                != GHOSTTY_SUCCESS
            || written != std::size(keys) || columns == 0 || rows == 0
            || totalRows > static_cast<size_t>(UINT32_MAX)) {
            return std::nullopt;
        }

        SearchScreen searchScreen = SearchScreen::Primary;
        switch (screen) {
        case GHOSTTY_TERMINAL_SCREEN_PRIMARY: break;
        case GHOSTTY_TERMINAL_SCREEN_ALTERNATE:
            searchScreen = SearchScreen::Alternate;
            break;
        default: return std::nullopt;
        }
        const quint64 viewportOffset = std::min<quint64>(
            scrollbar.offset, static_cast<quint64>(totalRows));
        return SearchExtent{
            .totalRows = static_cast<quint32>(totalRows),
            .columns = static_cast<int>(columns),
            .rows = static_cast<int>(rows),
            .viewportOffset = viewportOffset,
            .viewportLength = std::min<quint64>(scrollbar.len,
                                                static_cast<quint64>(totalRows)
                                                    - viewportOffset),
            .activeScreen = searchScreen,
        };
    }

    std::optional<SearchRowSnapshot> snapshotSearchRow(quint32 screenRow) const
    {
        const std::optional<SearchExtent> extent = searchExtent();
        if (!extent.has_value() || screenRow >= extent->totalRows
            || extent->columns <= 0
            || extent->columns > static_cast<int>(UINT16_MAX)) {
            return std::nullopt;
        }

        const ScreenCell start{.x = 0, .y = screenRow};
        const ScreenCell end{
            .x = static_cast<uint16_t>(extent->columns - 1),
            .y = screenRow,
        };
        GhosttyGridRef rowReference{};
        GhosttyRow rawRow = 0;
        bool wrapped = false;
        if (!gridRefAtScreen(start, &rowReference)
            || ghostty_grid_ref_row(&rowReference, &rawRow) != GHOSTTY_SUCCESS
            || ghostty_row_get(rawRow, GHOSTTY_ROW_DATA_WRAP, &wrapped)
                != GHOSTTY_SUCCESS) {
            return std::nullopt;
        }

        // PageFormatter carries trailing blanks across a soft-wrap boundary
        // and emits them if the continuation later contains text. Preserve
        // those cells for a wrapped row; only a hard line ending owns
        // independently trimmable trailing spaces.
        std::optional<SearchRowTextMapData> data =
            textMapSearchRow(start, end, wrapped);
        if (!data.has_value()) {
            return std::nullopt;
        }
        if (!wrapped) {
            while (!data->text.isEmpty()
                   && data->text.at(data->text.size() - 1) == ' ') {
                data->text.chop(1);
                data->byteCells.removeLast();
            }
        }
        if (data->text.size() != data->byteCells.size()) {
            return std::nullopt;
        }

        SearchRowSnapshot snapshot;
        snapshot.text = std::move(data->text);
        snapshot.byteColumns = std::move(data->byteCells);
        snapshot.screenRow = screenRow;
        snapshot.newlineColumn = snapshot.byteColumns.isEmpty()
            ? 0
            : snapshot.byteColumns.constLast();
        snapshot.wrapped = wrapped;
        return snapshot;
    }

    QVector<QPoint> visibleCellsForSearchRange(TerminalSearchRange range) const
    {
        const std::optional<SearchExtent> extent = searchExtent();
        if (!extent.has_value() || extent->columns <= 0
            || range.start.column >= extent->columns
            || range.end.column >= extent->columns
            || range.start.screenRow >= extent->totalRows
            || range.end.screenRow >= extent->totalRows) {
            return {};
        }

        if (range.end < range.start) {
            std::swap(range.start, range.end);
        }

        GhosttyTerminalScrollbar scrollbar{};
        if (ghostty_terminal_get(terminal_, GHOSTTY_TERMINAL_DATA_SCROLLBAR,
                                 &scrollbar)
                != GHOSTTY_SUCCESS
            || scrollbar.len == 0) {
            return {};
        }
        const quint64 viewportStart = scrollbar.offset;
        const quint64 viewportEnd =
            std::min(static_cast<quint64>(scrollbar.offset + scrollbar.len),
                     static_cast<quint64>(extent->totalRows));
        const quint64 firstRow = std::max(
            static_cast<quint64>(range.start.screenRow), viewportStart);
        const quint64 lastRowExclusive = std::min(
            static_cast<quint64>(range.end.screenRow) + 1U, viewportEnd);
        if (firstRow >= lastRowExclusive) {
            return {};
        }

        QVector<QPoint> result;
        for (quint64 row = firstRow; row < lastRowExclusive; ++row) {
            const int firstColumn = row == range.start.screenRow
                ? static_cast<int>(range.start.column)
                : 0;
            const int lastColumn = row == range.end.screenRow
                ? static_cast<int>(range.end.column)
                : extent->columns - 1;
            const quint64 viewportRow = row - viewportStart;
            if (viewportRow > static_cast<quint64>(INT_MAX)) {
                return {};
            }
            for (int column = firstColumn; column <= lastColumn; ++column) {
                result.append(QPoint(column, static_cast<int>(viewportRow)));
            }
        }
        return result;
    }

    bool scrollSearchRangeIntoView(TerminalSearchRange range)
    {
        const std::optional<SearchExtent> extent = searchExtent();
        if (!extent.has_value() || extent->columns <= 0
            || range.start.column >= extent->columns
            || range.end.column >= extent->columns
            || range.start.screenRow >= extent->totalRows
            || range.end.screenRow >= extent->totalRows) {
            return false;
        }

        if (range.end < range.start) {
            std::swap(range.start, range.end);
        }

        GhosttyTerminalScrollbar scrollbar{};
        if (ghostty_terminal_get(terminal_, GHOSTTY_TERMINAL_DATA_SCROLLBAR,
                                 &scrollbar)
                != GHOSTTY_SUCCESS
            || scrollbar.len == 0) {
            return false;
        }
        const quint64 viewportStart = scrollbar.offset;
        const quint64 viewportEnd = scrollbar.offset + scrollbar.len;
        const bool overlapsViewport = range.start.screenRow < viewportEnd
            && range.end.screenRow >= viewportStart;
        if (overlapsViewport) {
            return false;
        }
        // Ghostty's search thread scrolls the result's start pin directly to
        // the viewport origin. This intentionally differs from selection
        // endpoint scrolling, which places a below-viewport endpoint on the
        // final visible row.
        GhosttyTerminalScrollViewport scroll{};
        scroll.tag = GHOSTTY_SCROLL_VIEWPORT_ROW;
        scroll.value.row = boundedRow(range.start.screenRow);
        ghostty_terminal_scroll_viewport(terminal_, scroll);
        return true;
    }

    std::uint64_t compressionActivity() const
    {
        uint64_t activity = 0;
        if (ghostty_terminal_compression_activity(terminal_, &activity)
            != GHOSTTY_SUCCESS) {
            return 0;
        }
        return activity;
    }

    bool compressScrollback()
    {
        GhosttyTerminalCompressionResult result =
            GHOSTTY_TERMINAL_COMPRESSION_RESULT_COMPLETE;
        return ghostty_terminal_compress(
                   terminal_, GHOSTTY_TERMINAL_COMPRESSION_MODE_INCREMENTAL,
                   &result)
            == GHOSTTY_SUCCESS
            && result == GHOSTTY_TERMINAL_COMPRESSION_RESULT_PENDING;
    }

    std::shared_ptr<const TerminalKittyGraphicsImage>
    kittyImageAsset(const KittyImageView &image)
    {
        if (const auto cached =
                kittyImageAssets_.value(image.generation).lock();
            cached != nullptr && cached->imageId == image.imageId) {
            return cached;
        }

        std::optional<TerminalKittyPixelFormat> format;
        switch (image.format) {
        case GHOSTTY_KITTY_IMAGE_FORMAT_RGB:
            format = TerminalKittyPixelFormat::Rgb;
            break;
        case GHOSTTY_KITTY_IMAGE_FORMAT_RGBA:
            format = TerminalKittyPixelFormat::Rgba;
            break;
        case GHOSTTY_KITTY_IMAGE_FORMAT_GRAY_ALPHA:
            format = TerminalKittyPixelFormat::GrayAlpha;
            break;
        case GHOSTTY_KITTY_IMAGE_FORMAT_GRAY:
            format = TerminalKittyPixelFormat::Gray;
            break;
        default: return {};
        }

        auto materialized = terminalMaterializeKittyImage(
            QSize(static_cast<int>(image.width),
                  static_cast<int>(image.height)),
            *format,
            std::span<const std::uint8_t>(image.pixels, image.dataLength));
        if (!materialized.has_value()) return {};

        auto asset = std::make_shared<const TerminalKittyGraphicsImage>(
            TerminalKittyGraphicsImage{
                .imageId = image.imageId,
                .generation = image.generation,
                .fullyOpaque = materialized->fullyOpaque,
                .straightRgba = std::move(materialized->straightRgba),
            });
        kittyImageAssets_.insert(image.generation, asset);
        return asset;
    }

    std::optional<std::shared_ptr<const TerminalKittyGraphicsSnapshot>>
    kittyGraphicsSnapshot()
    {
        auto result = std::make_shared<TerminalKittyGraphicsSnapshot>();
        result->cellWidthPixels =
            static_cast<quint32>(geometry_.cellWidthPixels);
        result->cellHeightPixels =
            static_cast<quint32>(geometry_.cellHeightPixels);

        GhosttyKittyGraphics graphics = nullptr;
        const GhosttyResult graphicsResult = ghostty_terminal_get(
            terminal_, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS, &graphics);
        if (graphicsResult == GHOSTTY_NO_VALUE) {
            return result;
        }
        if (graphicsResult != GHOSTTY_SUCCESS || graphics == nullptr
            || ghostty_kitty_graphics_get(
                   graphics, GHOSTTY_KITTY_GRAPHICS_DATA_GENERATION,
                   &result->storageGeneration)
                != GHOSTTY_SUCCESS
            || ghostty_kitty_graphics_get(
                   graphics, GHOSTTY_KITTY_GRAPHICS_DATA_PLACEMENT_ITERATOR,
                   &kittyPlacementIterator_)
                != GHOSTTY_SUCCESS) {
            return std::nullopt;
        }

        struct PlacementCandidate final {
            KittyImageView image;
            quint32 placementId = 0;
            qint32 z = 0;
            TerminalKittyGraphicsLayer layer =
                TerminalKittyGraphicsLayer::AboveText;
            qint32 viewportColumn = 0;
            qint32 viewportRow = 0;
            quint32 xOffsetPixels = 0;
            quint32 yOffsetPixels = 0;
            quint32 destinationWidthPixels = 0;
            quint32 destinationHeightPixels = 0;
            quint32 sourceX = 0;
            quint32 sourceY = 0;
            quint32 sourceWidth = 0;
            quint32 sourceHeight = 0;
        };
        QVector<PlacementCandidate> candidates;
        while (ghostty_kitty_graphics_placement_next(kittyPlacementIterator_)) {
            quint32 imageId = 0;
            quint32 placementId = 0;
            bool isVirtual = false;
            quint32 xOffset = 0;
            quint32 yOffset = 0;
            qint32 z = 0;
            constexpr std::array keys{
                GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_IMAGE_ID,
                GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_PLACEMENT_ID,
                GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_IS_VIRTUAL,
                GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_X_OFFSET,
                GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_Y_OFFSET,
                GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_Z,
            };
            std::array<void *, keys.size()> values{
                &imageId, &placementId, &isVirtual, &xOffset, &yOffset, &z,
            };
            if (ghostty_kitty_graphics_placement_get_multi(
                    kittyPlacementIterator_, keys.size(), keys.data(),
                    values.data(), nullptr)
                != GHOSTTY_SUCCESS) {
                continue;
            }
            if (isVirtual) {
                result->containsVirtualPlacements = true;
                continue;
            }

            const GhosttyKittyGraphicsImage image =
                ghostty_kitty_graphics_image(graphics, imageId);
            if (image == nullptr) {
                continue;
            }
            GhosttyKittyGraphicsPlacementRenderInfo renderInfo{};
            renderInfo.size = sizeof(renderInfo);
            if (ghostty_kitty_graphics_placement_render_info(
                    kittyPlacementIterator_, image, terminal_, &renderInfo)
                    != GHOSTTY_SUCCESS
                || !renderInfo.viewport_visible || renderInfo.pixel_width == 0
                || renderInfo.pixel_height == 0 || renderInfo.source_width == 0
                || renderInfo.source_height == 0) {
                continue;
            }
            const auto imageView = queryKittyImage(image, imageId);
            if (!imageView.has_value()) {
                continue;
            }

            candidates.append({
                .image = *imageView,
                .placementId = placementId,
                .z = z,
                .layer = kittyLayer(z),
                .viewportColumn = renderInfo.viewport_col,
                .viewportRow = renderInfo.viewport_row,
                .xOffsetPixels = xOffset,
                .yOffsetPixels = yOffset,
                .destinationWidthPixels = renderInfo.pixel_width,
                .destinationHeightPixels = renderInfo.pixel_height,
                .sourceX = renderInfo.source_x,
                .sourceY = renderInfo.source_y,
                .sourceWidth = renderInfo.source_width,
                .sourceHeight = renderInfo.source_height,
            });
        }
        std::ranges::sort(candidates,
                          [](const PlacementCandidate &left,
                             const PlacementCandidate &right) {
                              if (left.z != right.z) return left.z < right.z;
                              if (left.image.imageId != right.image.imageId) {
                                  return left.image.imageId
                                      < right.image.imageId;
                              }
                              return left.placementId < right.placementId;
                          });

        // Preserve libghostty as the protocol-storage owner, including older
        // placements that can reappear after a Kitty delete, but avoid
        // mirroring pixels that cannot affect this rendered snapshot. The
        // deliberately exact key is conservative: it only recognizes a
        // higher, successfully materialized opaque placement at the same
        // renderer depth and physical destination.
        using OpaqueCoverKey =
            std::tuple<qint32, TerminalKittyGraphicsLayer, qint32, qint32,
                       quint32, quint32, quint32, quint32>;
        std::set<OpaqueCoverKey> opaqueCovers;
        result->placements.reserve(candidates.size());
        for (auto iterator = candidates.crbegin();
             iterator != candidates.crend(); ++iterator) {
            const OpaqueCoverKey coverKey{
                iterator->z,
                iterator->layer,
                iterator->viewportColumn,
                iterator->viewportRow,
                iterator->xOffsetPixels,
                iterator->yOffsetPixels,
                iterator->destinationWidthPixels,
                iterator->destinationHeightPixels,
            };
            if (opaqueCovers.contains(coverKey)) {
                continue;
            }

            std::shared_ptr<const TerminalKittyGraphicsImage> asset =
                kittyImageAsset(iterator->image);
            if (asset == nullptr) {
                continue;
            }
            const bool fullyOpaque = asset->fullyOpaque;
            result->placements.append({
                .image = std::move(asset),
                .placementId = iterator->placementId,
                .z = iterator->z,
                .layer = iterator->layer,
                .viewportColumn = iterator->viewportColumn,
                .viewportRow = iterator->viewportRow,
                .xOffsetPixels = iterator->xOffsetPixels,
                .yOffsetPixels = iterator->yOffsetPixels,
                .destinationWidthPixels = iterator->destinationWidthPixels,
                .destinationHeightPixels = iterator->destinationHeightPixels,
                .sourceX = iterator->sourceX,
                .sourceY = iterator->sourceY,
                .sourceWidth = iterator->sourceWidth,
                .sourceHeight = iterator->sourceHeight,
            });
            if (fullyOpaque) {
                opaqueCovers.insert(coverKey);
            }
        }
        std::ranges::reverse(result->placements);
        kittyImageAssets_.removeIf(
            [](auto iterator) { return iterator.value().expired(); });
        return std::shared_ptr<const TerminalKittyGraphicsSnapshot>(
            std::move(result));
    }

    RenderResult renderFrame(RenderSnapshot *snapshot)
    {
        if (snapshot == nullptr
            || ghostty_render_state_update(renderState_, terminal_)
                != GHOSTTY_SUCCESS) {
            return RenderResult::Unavailable;
        }

        GhosttyRenderStateDirty dirty = GHOSTTY_RENDER_STATE_DIRTY_FULL;
        uint16_t columns = 0;
        uint16_t rows = 0;
        GhosttyRenderStateCursor cursor{};
        cursor.size = sizeof(cursor);
        constexpr std::array stateFields{
            GHOSTTY_RENDER_STATE_DATA_DIRTY,
            GHOSTTY_RENDER_STATE_DATA_COLS,
            GHOSTTY_RENDER_STATE_DATA_ROWS,
            GHOSTTY_RENDER_STATE_DATA_CURSOR,
        };
        std::array<void *, stateFields.size()> stateValues{
            &dirty,
            &columns,
            &rows,
            &cursor,
        };
        if (ghostty_render_state_get_multi(renderState_, stateFields.size(),
                                           stateFields.data(),
                                           stateValues.data(), nullptr)
            != GHOSTTY_SUCCESS) {
            return RenderResult::Retry;
        }

        TerminalFrame metadata;
        metadata.columns = static_cast<int>(columns);
        metadata.rows = static_cast<int>(rows);
        const bool fullFrame = !hasPublishedFrame_
            || dirty == GHOSTTY_RENDER_STATE_DIRTY_FULL
            || publishedMetadata_.columns != metadata.columns
            || publishedMetadata_.rows != metadata.rows;

        GhosttyRenderStateColors refreshedColors{};
        refreshedColors.size = sizeof(refreshedColors);
        quint64 colorStateQueries = 0;
        // libghostty defines partial dirty state as rows-only: none of the
        // global state, including colors, changed. Keep the raw colors needed
        // for partial cell materialization with the last published full frame.
        if (fullFrame) {
            if (ghostty_render_state_get(renderState_,
                                         GHOSTTY_RENDER_STATE_DATA_COLORS,
                                         &refreshedColors)
                != GHOSTTY_SUCCESS) {
                return RenderResult::Retry;
            }
            ++colorStateQueries;
        }
        const GhosttyRenderStateColors &colors =
            fullFrame ? refreshedColors : publishedColors_;
        bool paletteChanged = false;
        if (fullFrame) {
            metadata.foreground = toQColor(colors.foreground);
            metadata.background = toQColor(colors.background);
            paletteChanged = !std::ranges::equal(
                colors.palette, std::as_const(publishedMetadata_.palette),
                [](const GhosttyColorRgb &current, const QColor &published) {
                    return published.rgb()
                        == qRgb(current.r, current.g, current.b);
                });
            if (paletteChanged) {
                metadata.palette.reserve(
                    static_cast<qsizetype>(std::size(colors.palette)));
                for (const GhosttyColorRgb &color : colors.palette) {
                    metadata.palette.append(toQColor(color));
                }
            } else {
                metadata.palette = publishedMetadata_.palette;
            }
            metadata.cursorColorExplicit = colors.cursor_has_value;
            metadata.cursorColor = colors.cursor_has_value
                ? toQColor(colors.cursor)
                : metadata.foreground;
        } else {
            metadata.foreground = publishedMetadata_.foreground;
            metadata.background = publishedMetadata_.background;
            metadata.palette = publishedMetadata_.palette;
            metadata.cursorColorExplicit =
                publishedMetadata_.cursorColorExplicit;
            metadata.cursorColor = publishedMetadata_.cursorColor;
        }

        const bool cursorInViewport = cursor.viewport_has_value;
        metadata.cursorVisible = cursor.visible && cursorInViewport;
        metadata.cursorBlinking = cursor.blinking;
        const uint16_t cursorColumn =
            cursorInViewport ? cursor.viewport_x : uint16_t{};
        const uint16_t cursorRow =
            cursorInViewport ? cursor.viewport_y : uint16_t{};
        const bool cursorOnWideTail = cursorInViewport && cursor.wide_tail;
        metadata.cursorColumn = static_cast<int>(cursorColumn);
        metadata.cursorRow = static_cast<int>(cursorRow);
        metadata.cursorStyle = static_cast<int>(cursor.visual_style);
        if (metadata.cursorVisible && cursorOnWideTail
            && metadata.cursorColumn > 0) {
            --metadata.cursorColumn;
            metadata.cursorColumnSpan = 2;
        }

        GhosttyTerminalScrollbar scrollbar{};
        if (ghostty_terminal_get(terminal_, GHOSTTY_TERMINAL_DATA_SCROLLBAR,
                                 &scrollbar)
            == GHOSTTY_SUCCESS) {
            metadata.scrollTotal = scrollbar.total;
            metadata.scrollOffset = scrollbar.offset;
            metadata.scrollLength = scrollbar.len;
        }

        const auto kittySnapshot = kittyGraphicsSnapshot();
        if (!kittySnapshot.has_value()) {
            return RenderResult::Retry;
        }

        TerminalUpdate update;
        update.columns = metadata.columns;
        update.rows = metadata.rows;
        update.fullFrame = fullFrame;
        quint64 graphemeDataQueries = 0;
        quint64 contentBackgroundDataQueries = 0;
        if (fullFrame) {
            update.dirtyRows.reserve(metadata.rows);
        }

        const bool inspectCursorCell = metadata.cursorVisible
            && !cursorOnWideTail && metadata.cursorColumn >= 0
            && metadata.cursorColumn < metadata.columns
            && metadata.cursorRow >= 0 && metadata.cursorRow < metadata.rows;
        bool cursorCellResolved = !inspectCursorCell;
        if (inspectCursorCell && hasPublishedFrame_
            && publishedMetadata_.cursorVisible
            && metadata.cursorColumn == publishedMetadata_.cursorColumn
            && metadata.cursorRow == publishedMetadata_.cursorRow) {
            metadata.cursorColumnSpan = publishedMetadata_.cursorColumnSpan;
            cursorCellResolved = true;
        }

        const bool visitRows = dirty != GHOSTTY_RENDER_STATE_DIRTY_FALSE;
        if (visitRows) {
            if (ghostty_render_state_get(renderState_,
                                         GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                         &rowIterator_)
                != GHOSTTY_SUCCESS) {
                return RenderResult::Retry;
            }

            int previousRow = -1;
            uint16_t rowY = 0;
            while (ghostty_render_state_row_iterator_next_dirty(rowIterator_,
                                                                &rowY)) {
                const int rowIndex = static_cast<int>(rowY);
                if (rowIndex <= previousRow || rowIndex >= metadata.rows) {
                    return RenderResult::Retry;
                }
                previousRow = rowIndex;
                const bool cursorRowNeedsInspection =
                    inspectCursorCell && rowIndex == metadata.cursorRow;

                GhosttyRenderStateRowSelection rowSelection{};
                rowSelection.size = sizeof(rowSelection);
                const bool hasSelection =
                    ghostty_render_state_row_get(
                        rowIterator_, GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION,
                        &rowSelection)
                    == GHOSTTY_SUCCESS;
                if (ghostty_render_state_row_get(
                        rowIterator_, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                        &rowCells_)
                    != GHOSTTY_SUCCESS) {
                    return RenderResult::Retry;
                }

                TerminalRowUpdate rowUpdate;
                rowUpdate.row = rowIndex;
                rowUpdate.cells.resize(metadata.columns);
                GhosttyRow rawRow = 0;
                GhosttyRowSemanticPrompt semanticPrompt =
                    GHOSTTY_ROW_SEMANTIC_NONE;
                if (ghostty_render_state_row_get(
                        rowIterator_, GHOSTTY_RENDER_STATE_ROW_DATA_RAW,
                        &rawRow)
                        != GHOSTTY_SUCCESS
                    || ghostty_row_get(rawRow, GHOSTTY_ROW_DATA_SEMANTIC_PROMPT,
                                       &semanticPrompt)
                        != GHOSTTY_SUCCESS) {
                    return RenderResult::Retry;
                }
                rowUpdate.presentation.paddingExtensionSafe =
                    semanticPrompt == GHOSTTY_ROW_SEMANTIC_NONE;
                int columnIndex = 0;
                while (columnIndex < metadata.columns
                       && ghostty_render_state_row_cells_next(rowCells_)) {
                    GhosttyCell rawCell = 0;
                    GhosttyStyle style{};
                    style.size = sizeof(style);
                    uint32_t graphemeCount = 0;
                    constexpr std::array fields{
                        GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW,
                        GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE,
                        GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN,
                    };
                    std::array<void *, fields.size()> values{
                        &rawCell,
                        &style,
                        &graphemeCount,
                    };
                    if (ghostty_render_state_row_cells_get_multi(
                            rowCells_, fields.size(), fields.data(),
                            values.data(), nullptr)
                        != GHOSTTY_SUCCESS) {
                        return RenderResult::Retry;
                    }

                    GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
                    GhosttyCellContentTag contentTag =
                        GHOSTTY_CELL_CONTENT_CODEPOINT;
                    uint32_t codepoint = 0;
                    bool hasHyperlink = false;
                    constexpr std::array cellFields{
                        GHOSTTY_CELL_DATA_WIDE,
                        GHOSTTY_CELL_DATA_CONTENT_TAG,
                        GHOSTTY_CELL_DATA_HAS_HYPERLINK,
                        GHOSTTY_CELL_DATA_CODEPOINT,
                    };
                    std::array<void *, cellFields.size()> cellValues{
                        &wide,
                        &contentTag,
                        &hasHyperlink,
                        &codepoint,
                    };
                    if (ghostty_cell_get_multi(rawCell, cellFields.size(),
                                               cellFields.data(),
                                               cellValues.data(), nullptr)
                        != GHOSTTY_SUCCESS) {
                        return RenderResult::Retry;
                    }

                    if (cursorRowNeedsInspection
                        && columnIndex == metadata.cursorColumn) {
                        metadata.cursorColumnSpan =
                            wide == GHOSTTY_CELL_WIDE_WIDE ? 2 : 1;
                        cursorCellResolved = true;
                    }

                    TerminalCell &cell = rowUpdate.cells[columnIndex];
                    cell.setColumnSpan(wide == GHOSTTY_CELL_WIDE_WIDE ? 2 : 1);
                    cell.setHasHyperlink(hasHyperlink);
                    cell.setMinimumContrastExemptGlyph(
                        minimumContrastExemptGlyph(codepoint));
                    const bool coveringGlyph = codepoint == 0x2588;
                    cell.setSpacer(wide == GHOSTTY_CELL_WIDE_SPACER_TAIL
                                   || wide == GHOSTTY_CELL_WIDE_SPACER_HEAD);
                    if (isPowerlinePaddingGlyph(codepoint)) {
                        rowUpdate.presentation.paddingExtensionSafe = false;
                    }

                    if (graphemeCount == 1 && !isUnicodeScalar(codepoint)) {
                        return RenderResult::Retry;
                    }
                    const bool suppressText = style.invisible || cell.spacer()
                        || codepoint == kittyUnicodePlaceholder;
                    if (graphemeCount == 1 && !suppressText) {
                        cell.text = singleCodepointText(codepoint);
                    } else if (graphemeCount > 1 && !suppressText) {
                        std::array<uint8_t, 64> graphemeStorage;
                        GhosttyBuffer graphemeBuffer{
                            .ptr = graphemeStorage.data(),
                            .cap = graphemeStorage.size(),
                            .len = 0,
                        };
                        GhosttyResult graphemeResult =
                            ghostty_render_state_row_cells_get(
                                rowCells_,
                                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8,
                                &graphemeBuffer);
                        ++graphemeDataQueries;
                        QByteArray dynamicGrapheme;
                        if (graphemeResult == GHOSTTY_OUT_OF_SPACE) {
                            dynamicGrapheme.resize(
                                static_cast<qsizetype>(graphemeBuffer.len));
                            graphemeBuffer.ptr = reinterpret_cast<uint8_t *>(
                                dynamicGrapheme.data());
                            graphemeBuffer.cap =
                                static_cast<size_t>(dynamicGrapheme.size());
                            graphemeResult = ghostty_render_state_row_cells_get(
                                rowCells_,
                                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8,
                                &graphemeBuffer);
                            ++graphemeDataQueries;
                        }
                        if (graphemeResult != GHOSTTY_SUCCESS) {
                            return RenderResult::Retry;
                        }
                        if (graphemeBuffer.len > 0) {
                            cell.text = QString::fromUtf8(
                                reinterpret_cast<const char *>(
                                    graphemeBuffer.ptr),
                                static_cast<qsizetype>(graphemeBuffer.len));
                        }
                    }
                    cell.baseCodepoint = codepoint;
                    cell.setPlainCodepoint(graphemeCount == 1);
                    cell.setExtendedGrapheme(graphemeCount > 1);

                    GhosttyColorRgb foreground = resolveStyleColor(
                        style.fg_color, colors, colors.foreground);
                    GhosttyColorRgb background = colors.background;
                    bool backgroundExplicit = true;
                    switch (contentTag) {
                    case GHOSTTY_CELL_CONTENT_BG_COLOR_PALETTE: {
                        GhosttyColorPaletteIndex paletteIndex = 0;
                        if (ghostty_cell_get(rawCell,
                                             GHOSTTY_CELL_DATA_COLOR_PALETTE,
                                             &paletteIndex)
                            != GHOSTTY_SUCCESS) {
                            return RenderResult::Retry;
                        }
                        ++contentBackgroundDataQueries;
                        background = colors.palette[paletteIndex];
                        break;
                    }
                    case GHOSTTY_CELL_CONTENT_BG_COLOR_RGB:
                        if (ghostty_cell_get(rawCell,
                                             GHOSTTY_CELL_DATA_COLOR_RGB,
                                             &background)
                            != GHOSTTY_SUCCESS) {
                            return RenderResult::Retry;
                        }
                        ++contentBackgroundDataQueries;
                        break;
                    case GHOSTTY_CELL_CONTENT_CODEPOINT:
                    case GHOSTTY_CELL_CONTENT_CODEPOINT_GRAPHEME:
                        backgroundExplicit =
                            style.bg_color.tag != GHOSTTY_STYLE_COLOR_NONE;
                        background = resolveStyleColor(style.bg_color, colors,
                                                       colors.background);
                        break;
                    default: return RenderResult::Retry;
                    }
                    if (backgroundExplicit) {
                        cell.setBackgroundExplicit(true);
                        if (sameRgb(background, colors.background)) {
                            rowUpdate.presentation.paddingExtensionSafe = false;
                        }
                    } else {
                        rowUpdate.presentation.paddingExtensionSafe = false;
                    }
                    const GhosttyColorRgb styleForeground = foreground;
                    const GhosttyColorRgb styleBackground = background;
                    foreground =
                        style.inverse ? styleBackground : styleForeground;
                    background = (style.inverse != coveringGlyph)
                        ? styleForeground
                        : styleBackground;

                    cell.foreground = TerminalCellColor::fromRgb(
                        foreground.r, foreground.g, foreground.b);
                    cell.background = TerminalCellColor::fromRgb(
                        background.r, background.g, background.b);
                    const GhosttyColorRgb underlineColor = resolveStyleColor(
                        style.underline_color, colors, foreground);
                    cell.underlineColor = TerminalCellColor::fromRgb(
                        underlineColor.r, underlineColor.g, underlineColor.b);
                    switch (style.fg_color.tag) {
                    case GHOSTTY_STYLE_COLOR_PALETTE:
                        cell.setStyleForegroundSource(
                            TerminalColorSource::Palette);
                        cell.setStyleForegroundPaletteIndex(
                            static_cast<int>(style.fg_color.value.palette));
                        break;
                    case GHOSTTY_STYLE_COLOR_RGB:
                        cell.setStyleForegroundSource(TerminalColorSource::Rgb);
                        break;
                    default:
                        cell.setStyleForegroundSource(
                            TerminalColorSource::Default);
                        break;
                    }
                    cell.setBold(style.bold);
                    cell.setItalic(style.italic);
                    cell.setFaint(style.faint);
                    cell.setTextBlink(style.blink);
                    cell.setInverse(style.inverse);
                    cell.setInvisible(style.invisible);
                    cell.setUnderlineUsesForeground(
                        style.underline_color.tag == GHOSTTY_STYLE_COLOR_NONE);
                    switch (style.underline) {
                    case GHOSTTY_SGR_UNDERLINE_SINGLE:
                        cell.setUnderlineStyle(TerminalUnderlineStyle::Single);
                        break;
                    case GHOSTTY_SGR_UNDERLINE_DOUBLE:
                        cell.setUnderlineStyle(TerminalUnderlineStyle::Double);
                        break;
                    case GHOSTTY_SGR_UNDERLINE_CURLY:
                        cell.setUnderlineStyle(TerminalUnderlineStyle::Curly);
                        break;
                    case GHOSTTY_SGR_UNDERLINE_DOTTED:
                        cell.setUnderlineStyle(TerminalUnderlineStyle::Dotted);
                        break;
                    case GHOSTTY_SGR_UNDERLINE_DASHED:
                        cell.setUnderlineStyle(TerminalUnderlineStyle::Dashed);
                        break;
                    default:
                        cell.setUnderlineStyle(TerminalUnderlineStyle::None);
                        break;
                    }
                    cell.setStrikeThrough(style.strikethrough);
                    cell.setOverline(style.overline);
                    cell.setSelected(
                        hasSelection
                        && columnIndex >= static_cast<int>(rowSelection.start_x)
                        && columnIndex <= static_cast<int>(rowSelection.end_x));
                    ++columnIndex;
                }
                if (columnIndex != metadata.columns) {
                    return RenderResult::Retry;
                }
                update.dirtyRows.append(std::move(rowUpdate));
            }
            if (fullFrame && update.dirtyRows.size() != metadata.rows) {
                return RenderResult::Retry;
            }
        }

        // Cursor shape depends on the leading cell width. Reuse the prior
        // result while the cursor stays put; if it moved without its row being
        // dirty, perform one targeted contiguous lookup as a correctness
        // fallback. Ordinary partial frames never scan clean rows.
        if (!cursorCellResolved) {
            if (ghostty_render_state_get(renderState_,
                                         GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                         &rowIterator_)
                != GHOSTTY_SUCCESS) {
                return RenderResult::Retry;
            }
            for (int rowIndex = 0; rowIndex <= metadata.cursorRow; ++rowIndex) {
                if (!ghostty_render_state_row_iterator_next(rowIterator_)) {
                    return RenderResult::Retry;
                }
                if (rowIndex != metadata.cursorRow) {
                    continue;
                }
                if (ghostty_render_state_row_get(
                        rowIterator_, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                        &rowCells_)
                    != GHOSTTY_SUCCESS) {
                    return RenderResult::Retry;
                }
                for (int columnIndex = 0; columnIndex <= metadata.cursorColumn;
                     ++columnIndex) {
                    if (!ghostty_render_state_row_cells_next(rowCells_)) {
                        return RenderResult::Retry;
                    }
                    if (columnIndex != metadata.cursorColumn) {
                        continue;
                    }
                    GhosttyCell rawCell = 0;
                    GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
                    if (ghostty_render_state_row_cells_get(
                            rowCells_, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW,
                            &rawCell)
                            != GHOSTTY_SUCCESS
                        || ghostty_cell_get(rawCell, GHOSTTY_CELL_DATA_WIDE,
                                            &wide)
                            != GHOSTTY_SUCCESS) {
                        return RenderResult::Retry;
                    }
                    metadata.cursorColumnSpan =
                        wide == GHOSTTY_CELL_WIDE_WIDE ? 2 : 1;
                    cursorCellResolved = true;
                }
            }
            if (!cursorCellResolved) {
                return RenderResult::Retry;
            }
        }

        update.colorsChanged = fullFrame || !hasPublishedFrame_
            || metadata.foreground != publishedMetadata_.foreground
            || metadata.background != publishedMetadata_.background
            || metadata.cursorColor != publishedMetadata_.cursorColor
            || paletteChanged
            || metadata.cursorColorExplicit
                != publishedMetadata_.cursorColorExplicit;
        if (update.colorsChanged) {
            update.foreground = metadata.foreground;
            update.background = metadata.background;
            update.cursorColor = metadata.cursorColor;
            update.palette = metadata.palette;
            update.cursorColorExplicit = metadata.cursorColorExplicit;
        }

        update.cursorChanged = fullFrame || !hasPublishedFrame_
            || metadata.cursorVisible != publishedMetadata_.cursorVisible
            || metadata.cursorBlinking != publishedMetadata_.cursorBlinking
            || metadata.cursorColumn != publishedMetadata_.cursorColumn
            || metadata.cursorRow != publishedMetadata_.cursorRow
            || metadata.cursorStyle != publishedMetadata_.cursorStyle
            || metadata.cursorColumnSpan != publishedMetadata_.cursorColumnSpan;
        update.cursorVisible = metadata.cursorVisible;
        update.cursorBlinking = metadata.cursorBlinking;
        update.cursorColumn = metadata.cursorColumn;
        update.cursorRow = metadata.cursorRow;
        update.cursorStyle = metadata.cursorStyle;
        update.cursorColumnSpan = metadata.cursorColumnSpan;

        update.scrollbarChanged = fullFrame || !hasPublishedFrame_
            || metadata.scrollTotal != publishedMetadata_.scrollTotal
            || metadata.scrollOffset != publishedMetadata_.scrollOffset
            || metadata.scrollLength != publishedMetadata_.scrollLength;
        update.scrollTotal = metadata.scrollTotal;
        update.scrollOffset = metadata.scrollOffset;
        update.scrollLength = metadata.scrollLength;

        update.kittyGraphicsChanged = fullFrame
            || publishedKittyGraphics_ == nullptr
            || **kittySnapshot != *publishedKittyGraphics_;
        if (update.kittyGraphicsChanged) {
            update.kittyGraphics = *kittySnapshot;
        }

        if (ghostty_render_state_clean(renderState_) != GHOSTTY_SUCCESS) {
            return RenderResult::Retry;
        }

        bool tracking = false;
        ghostty_terminal_get(terminal_, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING,
                             &tracking);
        if (fullFrame) {
            publishedColors_ = refreshedColors;
        }
        publishedMetadata_ = std::move(metadata);
        publishedKittyGraphics_ = *kittySnapshot;
        hasPublishedFrame_ = true;
        const quint64 materializedCells =
            static_cast<quint64>(update.dirtyRows.size())
            * static_cast<quint64>(update.columns);
        snapshot->update = std::move(update);
        snapshot->mouseTracking = tracking;
        snapshot->cellMaterialization = {
            .cells = materializedCells,
            .primaryDataQueries = materializedCells * 2,
            .graphemeDataQueries = graphemeDataQueries,
            .contentBackgroundDataQueries = contentBackgroundDataQueries,
        };
        snapshot->colorStateQueries = colorStateQueries;
        return RenderResult::Ready;
    }

    DeferredEffects takeDeferredEffects()
    {
        DeferredEffects effects;
        if (titleDirty_) {
            titleDirty_ = false;
            GhosttyString title{};
            if (ghostty_terminal_get(terminal_, GHOSTTY_TERMINAL_DATA_TITLE,
                                     &title)
                == GHOSTTY_SUCCESS) {
                effects.title = title.len == 0
                    ? QStringLiteral("")
                    : QString::fromUtf8(
                          reinterpret_cast<const char *>(title.ptr),
                          static_cast<qsizetype>(title.len));
            }
        }
        if (auto directory =
                std::exchange(pendingCurrentDirectory_, std::nullopt)) {
            effects.currentDirectory = std::move(*directory);
        }
        effects.bell = std::exchange(bellPending_, false);
        effects.clipboardWrites = std::move(pendingClipboardWrites_);
        pendingClipboardWrites_.clear();
        pendingClipboardBytes_ = 0;
        effects.desktopNotifications = std::move(pendingDesktopNotifications_);
        pendingDesktopNotifications_.clear();
        pendingDesktopNotificationBytes_ = 0;
        effects.progressReports = std::move(pendingProgressReports_);
        pendingProgressReports_.clear();
        return effects;
    }

private:
    static size_t boundedRow(quint64 row)
    {
        const quint64 maximum =
            static_cast<quint64>(std::numeric_limits<size_t>::max());
        return static_cast<size_t>(std::min(row, maximum));
    }

    bool scrollEndpointIntoView(uint32_t endpointRow)
    {
        GhosttyTerminalScrollbar scrollbar{};
        if (ghostty_terminal_get(terminal_, GHOSTTY_TERMINAL_DATA_SCROLLBAR,
                                 &scrollbar)
                != GHOSTTY_SUCCESS
            || scrollbar.len == 0) {
            return false;
        }

        const uint64_t row = endpointRow;
        uint64_t target = row;
        if (row >= scrollbar.offset) {
            const uint64_t viewportRow = row - scrollbar.offset;
            if (viewportRow < scrollbar.len) {
                return false;
            }
            // Put a logical endpoint below the viewport on its final row,
            // matching Ghostty's pin-up-by-(rows-1) adjustment.
            target = row - (scrollbar.len - 1);
        }

        GhosttyTerminalScrollViewport scroll{};
        scroll.tag = GHOSTTY_SCROLL_VIEWPORT_ROW;
        scroll.value.row = boundedRow(target);
        ghostty_terminal_scroll_viewport(terminal_, scroll);
        return true;
    }

    static void writePtyCallback(GhosttyTerminal, void *userdata,
                                 const uint8_t *data, size_t length)
    {
        auto *impl = static_cast<Impl *>(userdata);
        if (impl != nullptr && impl->callbacks_.writePty && data != nullptr
            && length > 0) {
            impl->callbacks_.writePty(
                QByteArrayView(reinterpret_cast<const char *>(data),
                               static_cast<qsizetype>(length)));
        }
    }

    static GhosttyString enquiryCallback(GhosttyTerminal, void *userdata)
    {
        auto *impl = static_cast<Impl *>(userdata);
        if (impl == nullptr || impl->enquiryResponse_.isEmpty()) {
            return {};
        }

        // Full Ghostty forwards the configured byte slice without the
        // standalone stream bridge's 255-byte scratch-buffer ceiling. The
        // public callback identifies the parser-normalized ENQ synchronously,
        // so publish through the same ordered public PTY sink and return an
        // empty slice to prevent a duplicate bridge write.
        if (impl->callbacks_.writePty) {
            impl->callbacks_.writePty(QByteArrayView(impl->enquiryResponse_));
        }
        return {};
    }

    static void bellCallback(GhosttyTerminal, void *userdata)
    {
        if (auto *impl = static_cast<Impl *>(userdata)) {
            impl->bellPending_ = true;
        }
    }

    static void titleCallback(GhosttyTerminal, void *userdata)
    {
        if (auto *impl = static_cast<Impl *>(userdata)) {
            impl->titleDirty_ = true;
        }
    }

    static void pwdCallback(GhosttyTerminal terminal, void *userdata)
    {
        if (auto *impl = static_cast<Impl *>(userdata)) {
            GhosttyString pwd{};
            if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_PWD, &pwd)
                != GHOSTTY_SUCCESS) {
                return;
            }
            std::string_view reported;
            if (pwd.len != 0) {
                reported = {
                    reinterpret_cast<const char *>(pwd.ptr),
                    static_cast<std::size_t>(pwd.len),
                };
            }
            if (const auto directory = validatedOsc7Directory(
                    reported, impl->callbacks_.queryMachineHostName)) {
                // Retain the newest accepted report at callback time. A later
                // invalid URI in the same VT batch must not overwrite it.
                impl->pendingCurrentDirectory_ = *directory;
            }
        }
    }

    static bool sizeCallback(GhosttyTerminal, void *userdata,
                             GhosttySizeReportSize *size)
    {
        auto *impl = static_cast<Impl *>(userdata);
        if (impl == nullptr || size == nullptr) {
            return false;
        }
        size->rows = boundedU16(impl->geometry_.rows);
        size->columns = boundedU16(impl->geometry_.columns);
        size->cell_width = boundedU32(impl->geometry_.cellWidthPixels);
        size->cell_height = boundedU32(impl->geometry_.cellHeightPixels);
        return true;
    }

    static bool colorSchemeCallback(GhosttyTerminal, void *userdata,
                                    GhosttyColorScheme *scheme)
    {
        const auto *impl = static_cast<const Impl *>(userdata);
        if (impl == nullptr || scheme == nullptr) {
            return false;
        }
        *scheme = toGhosttyColorScheme(impl->colorScheme_);
        return true;
    }

    static bool deviceAttributesCallback(GhosttyTerminal, void *userdata,
                                         GhosttyDeviceAttributes *attributes)
    {
        auto *impl = static_cast<Impl *>(userdata);
        if (impl == nullptr || attributes == nullptr) {
            return false;
        }
        *attributes = GhosttyDeviceAttributes{};
        attributes->primary.conformance_level = GHOSTTY_DA_CONFORMANCE_VT220;
        attributes->primary.features[0] = GHOSTTY_DA_FEATURE_ANSI_COLOR;
        attributes->primary.num_features = 1;
        if (impl->clipboardWriteAccess_ != TerminalClipboardAccess::Deny) {
            attributes->primary.features[1] = GHOSTTY_DA_FEATURE_CLIPBOARD;
            attributes->primary.num_features = 2;
        }
        attributes->secondary.device_type = GHOSTTY_DA_DEVICE_TYPE_VT220;
        return true;
    }

    GhosttyClipboardWriteResult
    clipboardWrite(const GhosttyClipboardWrite *write) noexcept
    {
        if (clipboardWriteAccess_ == TerminalClipboardAccess::Deny) {
            return GHOSTTY_CLIPBOARD_WRITE_RESULT_DENIED;
        }
        if (write == nullptr) {
            return GHOSTTY_CLIPBOARD_WRITE_RESULT_INVALID_DATA;
        }

        constexpr size_t minimumWriteSize =
            offsetof(GhosttyClipboardWrite, contents_len) + sizeof(size_t);
        if (write->size < minimumWriteSize) {
            return GHOSTTY_CLIPBOARD_WRITE_RESULT_INVALID_DATA;
        }
        if (write->contents_len > maximumClipboardRepresentations) {
            return GHOSTTY_CLIPBOARD_WRITE_RESULT_INVALID_DATA;
        }
        if (write->contents_len != 0 && write->contents == nullptr) {
            return GHOSTTY_CLIPBOARD_WRITE_RESULT_INVALID_DATA;
        }

        TerminalClipboardWriteRequest request;
        switch (write->location) {
        case GHOSTTY_CLIPBOARD_LOCATION_STANDARD:
            request.write.location = TerminalClipboardLocation::Standard;
            break;
        case GHOSTTY_CLIPBOARD_LOCATION_SELECTION:
            request.write.location = TerminalClipboardLocation::Selection;
            break;
        case GHOSTTY_CLIPBOARD_LOCATION_PRIMARY:
            request.write.location = TerminalClipboardLocation::Primary;
            break;
        default: return GHOSTTY_CLIPBOARD_WRITE_RESULT_UNSUPPORTED;
        }
        // A clear request carries no MIME or data bytes. Bound the request
        // count independently so a clear flood cannot create an unbounded
        // deferred-effect vector before the worker drains this VT batch.
        if (pendingClipboardWrites_.size() >= maximumPendingClipboardWrites) {
            return GHOSTTY_CLIPBOARD_WRITE_RESULT_BUSY;
        }
        request.confirmationRequired =
            clipboardWriteAccess_ == TerminalClipboardAccess::Ask;

        quint64 requestBytes = 0;
        const auto accountBytes = [&requestBytes](size_t length) {
            if (length > maximumPendingClipboardBytes
                || requestBytes > maximumPendingClipboardBytes
                        - static_cast<quint64>(length)) {
                return false;
            }
            requestBytes += static_cast<quint64>(length);
            return true;
        };

        try {
            request.write.contents.reserve(
                static_cast<qsizetype>(write->contents_len));
            for (size_t index = 0; index < write->contents_len; ++index) {
                const GhosttyClipboardContent &content = write->contents[index];
                if (content.mime.len == 0 || content.mime.ptr == nullptr
                    || content.mime.len
                        > static_cast<size_t>(QByteArray::maxSize())
                    || (content.data.len != 0 && content.data.ptr == nullptr)
                    || content.data.len
                        > static_cast<size_t>(QByteArray::maxSize())) {
                    return GHOSTTY_CLIPBOARD_WRITE_RESULT_INVALID_DATA;
                }
                if (!accountBytes(content.mime.len)
                    || !accountBytes(content.data.len)) {
                    return GHOSTTY_CLIPBOARD_WRITE_RESULT_BUSY;
                }
                if (std::memchr(content.mime.ptr, '\0', content.mime.len)
                    != nullptr) {
                    return GHOSTTY_CLIPBOARD_WRITE_RESULT_INVALID_DATA;
                }

                request.write.contents.append({
                    .mime = QByteArray(
                        reinterpret_cast<const char *>(content.mime.ptr),
                        static_cast<qsizetype>(content.mime.len)),
                    .data = content.data.len == 0
                        ? QByteArray{}
                        : QByteArray(
                              reinterpret_cast<const char *>(content.data.ptr),
                              static_cast<qsizetype>(content.data.len)),
                });
            }

            if (pendingClipboardBytes_
                > maximumPendingClipboardBytes - requestBytes) {
                return GHOSTTY_CLIPBOARD_WRITE_RESULT_BUSY;
            }
            pendingClipboardWrites_.append(std::move(request));
            pendingClipboardBytes_ += requestBytes;
        } catch (...) {
            // No C callback may propagate an allocation failure through the
            // libghostty ABI. Treat transient local memory pressure as busy.
            return GHOSTTY_CLIPBOARD_WRITE_RESULT_BUSY;
        }
        return GHOSTTY_CLIPBOARD_WRITE_RESULT_SUCCESS;
    }

    static GhosttyClipboardWriteResult
    clipboardWriteCallback(GhosttyTerminal, void *userdata,
                           const GhosttyClipboardWrite *write)
    {
        auto *impl = static_cast<Impl *>(userdata);
        return impl != nullptr ? impl->clipboardWrite(write)
                               : GHOSTTY_CLIPBOARD_WRITE_RESULT_UNSUPPORTED;
    }

    void desktopNotification(
        const GhosttyTerminalDesktopNotification *notification) noexcept
    {
        constexpr size_t minimumSize =
            offsetof(GhosttyTerminalDesktopNotification, body)
            + sizeof(GhosttyString);
        if (notification == nullptr || notification->size < minimumSize
            || pendingDesktopNotifications_.size()
                >= maximumPendingDesktopNotifications) {
            return;
        }

        const GhosttyString title = notification->title;
        const GhosttyString body = notification->body;
        const size_t maximumQtStringBytes =
            static_cast<size_t>(std::numeric_limits<qsizetype>::max());
        if ((title.len != 0 && title.ptr == nullptr)
            || (body.len != 0 && body.ptr == nullptr)
            || title.len > maximumQtStringBytes
            || body.len > maximumQtStringBytes
            || title.len > maximumPendingDesktopNotificationBytes
            || body.len > maximumPendingDesktopNotificationBytes - title.len
            || pendingDesktopNotificationBytes_
                > maximumPendingDesktopNotificationBytes - title.len
                    - body.len) {
            return;
        }

        try {
            const auto copy = [](GhosttyString value) {
                return value.len == 0
                    ? QString{}
                    : QString::fromUtf8(
                          reinterpret_cast<const char *>(value.ptr),
                          static_cast<qsizetype>(value.len));
            };
            pendingDesktopNotifications_.append({
                .title = copy(title),
                .body = copy(body),
            });
            pendingDesktopNotificationBytes_ += title.len + body.len;
        } catch (...) {
            // A libghostty callback cannot report allocation failure or let
            // an exception cross the C ABI. Dropping this advisory effect
            // keeps terminal parsing and later requests usable.
        }
    }

    static void desktopNotificationCallback(
        GhosttyTerminal, void *userdata,
        const GhosttyTerminalDesktopNotification *notification)
    {
        if (auto *impl = static_cast<Impl *>(userdata)) {
            impl->desktopNotification(notification);
        }
    }

    void progressReport(const GhosttyTerminalProgressReport *report) noexcept
    {
        constexpr size_t minimumSize =
            offsetof(GhosttyTerminalProgressReport, progress) + sizeof(int8_t);
        if (report == nullptr || report->size < minimumSize) {
            return;
        }

        TerminalProgressState state;
        switch (report->state) {
        case GHOSTTY_TERMINAL_PROGRESS_STATE_REMOVE:
            state = TerminalProgressState::Remove;
            break;
        case GHOSTTY_TERMINAL_PROGRESS_STATE_SET:
            state = TerminalProgressState::Set;
            break;
        case GHOSTTY_TERMINAL_PROGRESS_STATE_ERROR:
            state = TerminalProgressState::Error;
            break;
        case GHOSTTY_TERMINAL_PROGRESS_STATE_INDETERMINATE:
            state = TerminalProgressState::Indeterminate;
            break;
        case GHOSTTY_TERMINAL_PROGRESS_STATE_PAUSE:
            state = TerminalProgressState::Pause;
            break;
        default: return;
        }

        if (report->progress < -1 || report->progress > 100) return;
        const std::optional<quint8> progress = report->progress < 0
            ? std::nullopt
            : std::optional<quint8>(static_cast<quint8>(report->progress));
        try {
            if (pendingProgressReports_.size()
                >= maximumPendingProgressReports) {
                compactPendingProgressReports();
            }
            pendingProgressReports_.append({
                .state = state,
                .progress = progress,
            });
        } catch (...) {
            // The C callback cannot propagate allocation failure. Progress is
            // advisory, so dropping this report keeps VT parsing usable.
        }
    }

    void compactPendingProgressReports()
    {
        // A PTY read can contain an arbitrary number of tiny OSC reports. Do
        // not discard the suffix when the bounded queue fills: REMOVE and the
        // last value are the state that the user must see. Instead, replace
        // the prefix with a short sequence that has the same transformation
        // on any pre-existing pane state. This also preserves pause-without-
        // value, whose meaning depends on the preceding presentation.
        if (pendingProgressReports_.isEmpty()) return;

        std::array<TerminalProgressReport, 3> compacted;
        qsizetype compactedCount = 0;
        const auto append = [&compacted, &compactedCount](
                                TerminalProgressState state,
                                std::optional<quint8> progress = std::nullopt) {
            compacted.at(static_cast<size_t>(compactedCount++)) = {
                .state = state,
                .progress = progress,
                .activityPulses = quint8{0},
            };
        };

        bool visible = false;
        bool paused = false;
        std::optional<bool> error;
        std::optional<bool> indeterminate;
        std::optional<quint8> value;
        quint32 activityPulses = 0;
        for (const TerminalProgressReport &report :
             std::as_const(pendingProgressReports_)) {
            const bool ordinaryActivity =
                report.state == TerminalProgressState::Indeterminate
                || ((report.state == TerminalProgressState::Set
                     || report.state == TerminalProgressState::Error)
                    && !report.progress.has_value());
            activityPulses += report.activityPulses.value_or(
                ordinaryActivity ? quint8{1} : quint8{0});

            switch (report.state) {
            case TerminalProgressState::Remove:
                visible = false;
                paused = false;
                break;
            case TerminalProgressState::Set:
            case TerminalProgressState::Error:
                visible = true;
                paused = false;
                error = report.state == TerminalProgressState::Error;
                indeterminate = !report.progress.has_value();
                if (report.progress.has_value()) value = report.progress;
                break;
            case TerminalProgressState::Indeterminate:
                visible = true;
                paused = false;
                indeterminate = true;
                break;
            case TerminalProgressState::Pause:
                visible = true;
                paused = true;
                if (report.progress.has_value()) {
                    value = report.progress;
                    indeterminate = false;
                }
            }
        }

        bool canonicalPaused = false;
        if (error.has_value()) {
            const TerminalProgressState state = *error
                ? TerminalProgressState::Error
                : TerminalProgressState::Set;
            if (indeterminate.value_or(false)) {
                if (value.has_value()) append(state, value);
                append(state);
            } else {
                Q_ASSERT(value.has_value());
                append(state, value);
            }
        } else if (indeterminate.has_value()) {
            if (*indeterminate) {
                if (value.has_value()) {
                    append(TerminalProgressState::Pause, value);
                }
                append(TerminalProgressState::Indeterminate);
            } else {
                Q_ASSERT(value.has_value());
                append(TerminalProgressState::Pause, value);
                canonicalPaused = true;
            }
        }

        if (!visible) {
            append(TerminalProgressState::Remove);
        } else if (paused && !canonicalPaused) {
            append(TerminalProgressState::Pause);
        } else if (compactedCount == 0) {
            // With no internal transformation, the only report that can make
            // the final presentation visible is pause-without-value.
            Q_ASSERT(paused);
            append(TerminalProgressState::Pause);
        }

        Q_ASSERT(compactedCount > 0);
        compacted.front().activityPulses =
            static_cast<quint8>(activityPulses % 20);

        for (qsizetype index = 0; index < compactedCount; ++index) {
            pendingProgressReports_[index] =
                compacted.at(static_cast<size_t>(index));
        }
        pendingProgressReports_.resize(compactedCount);
    }

    static void
    progressReportCallback(GhosttyTerminal, void *userdata,
                           const GhosttyTerminalProgressReport *report)
    {
        if (auto *impl = static_cast<Impl *>(userdata)) {
            impl->progressReport(report);
        }
    }

    Geometry geometry_;
    Callbacks callbacks_;
    std::shared_ptr<const AdapterOwnerToken> ownerToken_;
    GhosttyTerminal terminal_ = nullptr;
    GhosttyRenderState renderState_ = nullptr;
    GhosttyRenderStateRowIterator rowIterator_ = nullptr;
    GhosttyRenderStateRowCells rowCells_ = nullptr;
    GhosttyKittyGraphicsPlacementIterator kittyPlacementIterator_ = nullptr;
    GhosttyKeyEncoder keyEncoder_ = nullptr;
    GhosttyKeyEvent keyEvent_ = nullptr;
    GhosttyMouseEncoder mouseEncoder_ = nullptr;
    GhosttyMouseEvent mouseEvent_ = nullptr;
    GhosttySelectionGesture selectionGesture_ = nullptr;
    GhosttySelectionGestureEvent selectionPressEvent_ = nullptr;
    GhosttySelectionGestureEvent selectionDragEvent_ = nullptr;
    GhosttySelectionGestureEvent selectionAutoscrollTickEvent_ = nullptr;
    GhosttySelectionGestureEvent selectionReleaseEvent_ = nullptr;
    QVector<uint32_t> selectionWordChars_;
    quint32 clickRepeatIntervalMilliseconds_ = 500;
    std::optional<quint64> lastSelectionPressTimestampNanoseconds_;
    TerminalFrame publishedMetadata_;
    GhosttyRenderStateColors publishedColors_{};
    std::shared_ptr<const TerminalKittyGraphicsSnapshot>
        publishedKittyGraphics_;
    QHash<quint64, std::weak_ptr<const TerminalKittyGraphicsImage>>
        kittyImageAssets_;
    bool hasPublishedFrame_ = false;
    bool titleDirty_ = false;
    std::optional<QByteArray> pendingCurrentDirectory_;
    bool bellPending_ = false;
    TerminalColorScheme colorScheme_ = TerminalColorScheme::Light;
    TerminalClipboardAccess clipboardWriteAccess_ =
        TerminalClipboardAccess::Allow;
    QByteArray enquiryResponse_;
    QVector<TerminalClipboardWriteRequest> pendingClipboardWrites_;
    quint64 pendingClipboardBytes_ = 0;
    QVector<TerminalDesktopNotification> pendingDesktopNotifications_;
    size_t pendingDesktopNotificationBytes_ = 0;
    QVector<TerminalProgressReport> pendingProgressReports_;
    std::optional<quintptr> lastOutputBottomNode_;
    quint16 lastOutputBottomY_ = 0;
    uint32_t mouseModeFingerprint_ = 0;
    bool mouseEncoderConfigured_ = false;
    bool normalizeKeyboardAfterCommandExit_ = false;
};

GhosttyVtAdapter::TrackedHyperlink::TrackedHyperlink(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{}

GhosttyVtAdapter::TrackedHyperlink::TrackedHyperlink(
    TrackedHyperlink &&) noexcept = default;

GhosttyVtAdapter::TrackedHyperlink &
GhosttyVtAdapter::TrackedHyperlink::operator=(TrackedHyperlink &&) noexcept =
    default;

GhosttyVtAdapter::TrackedHyperlink::~TrackedHyperlink() = default;

GhosttyVtAdapter::LogicalLineSnapshot::LogicalLineSnapshot(
    std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{}

GhosttyVtAdapter::LogicalLineSnapshot::LogicalLineSnapshot(
    LogicalLineSnapshot &&) noexcept = default;

GhosttyVtAdapter::LogicalLineSnapshot &
GhosttyVtAdapter::LogicalLineSnapshot::operator=(
    LogicalLineSnapshot &&) noexcept = default;

GhosttyVtAdapter::LogicalLineSnapshot::~LogicalLineSnapshot() = default;

const QByteArray &GhosttyVtAdapter::LogicalLineSnapshot::text() const
{
    static const QByteArray empty;
    return impl_ != nullptr ? impl_->data_.text : empty;
}

qsizetype GhosttyVtAdapter::LogicalLineSnapshot::targetByteOffset() const
{
    return impl_ != nullptr ? impl_->targetByteOffset_ : -1;
}

bool GhosttyVtAdapter::LogicalLineSnapshot::byteRangeContainsTarget(
    qsizetype beginByte, qsizetype endByte) const
{
    return impl_ != nullptr
        && impl_->byteRangeContainsTarget(beginByte, endByte);
}

GhosttyVtAdapter::TrackedTextRange::TrackedTextRange(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{}

GhosttyVtAdapter::TrackedTextRange::TrackedTextRange(
    TrackedTextRange &&) noexcept = default;

GhosttyVtAdapter::TrackedTextRange &
GhosttyVtAdapter::TrackedTextRange::operator=(TrackedTextRange &&) noexcept =
    default;

GhosttyVtAdapter::TrackedTextRange::~TrackedTextRange() = default;

std::unique_ptr<GhosttyVtAdapter>
GhosttyVtAdapter::create(const Options &options, Callbacks callbacks)
{
    auto impl = std::make_unique<Impl>(options.geometry, std::move(callbacks));
    if (!impl->initialize(options)) {
        return {};
    }
    return std::unique_ptr<GhosttyVtAdapter>(
        new GhosttyVtAdapter(std::move(impl)));
}

GhosttyVtAdapter::GhosttyVtAdapter(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{}

GhosttyVtAdapter::~GhosttyVtAdapter() = default;

bool GhosttyVtAdapter::resize(const Geometry &geometry)
{
    return impl_->resize(geometry);
}

bool GhosttyVtAdapter::setAppearance(const TerminalAppearance &appearance)
{
    return impl_->setAppearance(appearance);
}

bool GhosttyVtAdapter::setKittyImageStorageLimit(quint64 bytes)
{
    return impl_->setKittyImageStorageLimit(bytes);
}

void GhosttyVtAdapter::setColorScheme(TerminalColorScheme scheme)
{
    impl_->setColorScheme(scheme);
}

void GhosttyVtAdapter::setClipboardWriteAccess(TerminalClipboardAccess access)
{
    impl_->setClipboardWriteAccess(access);
}

void GhosttyVtAdapter::setEnquiryResponse(const QByteArray &response)
{
    impl_->setEnquiryResponse(response);
}

void GhosttyVtAdapter::writeVt(QByteArrayView data)
{
    impl_->writeVt(data);
}

bool GhosttyVtAdapter::observeOutputBottomAnchorChanged()
{
    return impl_->observeOutputBottomAnchorChanged();
}

void GhosttyVtAdapter::reset()
{
    impl_->reset();
}

void GhosttyVtAdapter::synchronizeInputModes()
{
    impl_->synchronizeInputModes();
}

void GhosttyVtAdapter::normalizeKeyboardAfterCommandExit()
{
    impl_->normalizeKeyboardAfterCommandExit();
}

GhosttyVtAdapter::EncodedKey
GhosttyVtAdapter::encodeKey(const TerminalKeyInput &input)
{
    return impl_->encodeKey(input);
}

bool GhosttyVtAdapter::keyboardActionMode() const
{
    return impl_->keyboardActionMode();
}

bool GhosttyVtAdapter::mouseTracking() const
{
    return impl_->mouseTracking();
}

QByteArray GhosttyVtAdapter::encodeMouse(const TerminalMouseInput &input)
{
    return impl_->encodeMouse(input);
}

std::optional<QByteArray>
GhosttyVtAdapter::alternateScrollSequence(qint64 rows) const
{
    return impl_->alternateScrollSequence(rows);
}

QByteArray GhosttyVtAdapter::encodeFocus(bool focused) const
{
    return impl_->encodeFocus(focused);
}

QByteArray GhosttyVtAdapter::encodePaste(const QString &text) const
{
    return impl_->encodePaste(text);
}

GhosttyVtAdapter::PreparedPaste
GhosttyVtAdapter::preparePaste(const QString &text,
                               const PastePreparationOptions &options) const
{
    return impl_->preparePaste(text, options);
}

QString GhosttyVtAdapter::selectedText(bool trim) const
{
    return impl_->selectedText(trim);
}

GhosttyVtAdapter::PlainFileSnapshot
GhosttyVtAdapter::snapshotPlainFile(TerminalFileLocation location) const
{
    return impl_->snapshotPlainFile(location);
}

bool GhosttyVtAdapter::hasSelection() const
{
    return impl_->hasSelection();
}

void GhosttyVtAdapter::clearSelection()
{
    impl_->clearSelection();
}

void GhosttyVtAdapter::resetSelectionGesture()
{
    impl_->resetSelectionGesture();
}

void GhosttyVtAdapter::clearSelectionAndResetGesture()
{
    impl_->clearSelectionAndResetGesture();
}

bool GhosttyVtAdapter::setSelectionWordChars(
    const QVector<uint32_t> &wordBoundaryCodepoints)
{
    return impl_->setSelectionWordChars(wordBoundaryCodepoints);
}

bool GhosttyVtAdapter::setClickRepeatIntervalMilliseconds(quint32 milliseconds)
{
    return impl_->setClickRepeatIntervalMilliseconds(milliseconds);
}

bool GhosttyVtAdapter::beginSelection(const TerminalSelectionPressInput &input)
{
    return impl_->beginSelection(input);
}

bool GhosttyVtAdapter::updateSelection(const TerminalSelectionDragInput &input)
{
    return impl_->updateSelection(input);
}

void GhosttyVtAdapter::endSelection(int column, int row)
{
    impl_->endSelection(column, row);
}

bool GhosttyVtAdapter::selectionGestureDragged() const
{
    return impl_->selectionGestureDragged();
}

GhosttyVtAdapter::SelectionAutoscrollDirection
GhosttyVtAdapter::selectionAutoscrollDirection() const
{
    return impl_->selectionAutoscrollDirection();
}

GhosttyVtAdapter::SelectionAutoscrollTickResult
GhosttyVtAdapter::selectionAutoscrollTick(
    const TerminalSelectionDragInput &input)
{
    return impl_->selectionAutoscrollTick(input);
}

bool GhosttyVtAdapter::selectionContains(int column, int row) const
{
    return impl_->selectionContains(column, row);
}

bool GhosttyVtAdapter::selectCell(int column, int row)
{
    return impl_->selectCell(column, row);
}

bool GhosttyVtAdapter::selectWord(int column, int row)
{
    return impl_->selectWord(column, row);
}

bool GhosttyVtAdapter::selectAll()
{
    return impl_->selectAll();
}

bool GhosttyVtAdapter::adjustSelection(TerminalSelectionAdjustment adjustment)
{
    return impl_->adjustSelection(adjustment);
}

bool GhosttyVtAdapter::scrollViewport(const TerminalViewportRequest &request)
{
    return impl_->scrollViewport(request);
}

std::optional<GhosttyVtAdapter::SearchExtent>
GhosttyVtAdapter::searchExtent() const
{
    return impl_->searchExtent();
}

std::optional<GhosttyVtAdapter::SearchRowSnapshot>
GhosttyVtAdapter::snapshotSearchRow(quint32 screenRow) const
{
    return impl_->snapshotSearchRow(screenRow);
}

QVector<QPoint> GhosttyVtAdapter::visibleCellsForSearchRange(
    const TerminalSearchRange &range) const
{
    return impl_->visibleCellsForSearchRange(range);
}

bool GhosttyVtAdapter::scrollSearchRangeIntoView(
    const TerminalSearchRange &range)
{
    return impl_->scrollSearchRangeIntoView(range);
}

std::optional<GhosttyVtAdapter::HyperlinkMatch>
GhosttyVtAdapter::hyperlinkAt(int column, int row,
                              const QVector<QPoint> &candidateCells) const
{
    return impl_->hyperlinkAt(column, row, candidateCells);
}

std::optional<GhosttyVtAdapter::TrackedHyperlink>
GhosttyVtAdapter::trackHyperlinkAt(int column, int row) const
{
    return impl_->trackHyperlinkAt(column, row);
}

bool GhosttyVtAdapter::trackedHyperlinkValid(
    const TrackedHyperlink &target) const
{
    return target.impl_ != nullptr
        && impl_->trackedHyperlinkValid(*target.impl_);
}

std::optional<GhosttyVtAdapter::HyperlinkMatch>
GhosttyVtAdapter::resolveHyperlink(const TrackedHyperlink &target,
                                   const QVector<QPoint> &candidateCells) const
{
    if (target.impl_ == nullptr) {
        return std::nullopt;
    }
    return impl_->resolveHyperlink(*target.impl_, candidateCells);
}

std::optional<GhosttyVtAdapter::LogicalLineSnapshot>
GhosttyVtAdapter::snapshotLogicalLineAt(int column, int row) const
{
    return impl_->snapshotLogicalLineAt(column, row);
}

std::optional<GhosttyVtAdapter::TrackedTextRange>
GhosttyVtAdapter::trackTextRange(const LogicalLineSnapshot &line,
                                 qsizetype beginByte, qsizetype endByte) const
{
    if (line.impl_ == nullptr) {
        return std::nullopt;
    }
    return impl_->trackTextRange(*line.impl_, beginByte, endByte);
}

bool GhosttyVtAdapter::trackedTextRangeValid(
    const TrackedTextRange &range) const
{
    return range.impl_ != nullptr && impl_->trackedTextRangeValid(*range.impl_);
}

bool GhosttyVtAdapter::installTextRange(const TrackedTextRange &range)
{
    return range.impl_ != nullptr && impl_->installTextRange(*range.impl_);
}

TerminalInspectorSnapshot GhosttyVtAdapter::inspectorSnapshot() const
{
    return impl_->inspectorSnapshot();
}

TerminalInspectorCellSnapshot
GhosttyVtAdapter::inspectorCellSnapshot(int viewportColumn,
                                        int viewportRow) const
{
    return impl_->inspectorCellSnapshot(viewportColumn, viewportRow);
}

std::optional<GhosttyVtAdapter::TextRangeMatch>
GhosttyVtAdapter::resolveTextRange(const TrackedTextRange &range) const
{
    if (range.impl_ == nullptr) {
        return std::nullopt;
    }
    return impl_->resolveTextRange(*range.impl_);
}

GhosttyVtAdapter::SemanticPromptState
GhosttyVtAdapter::semanticPromptState() const
{
    return impl_->semanticPromptState();
}

std::uint64_t GhosttyVtAdapter::compressionActivity() const
{
    return impl_->compressionActivity();
}

bool GhosttyVtAdapter::compressScrollback()
{
    return impl_->compressScrollback();
}

GhosttyVtAdapter::RenderResult
GhosttyVtAdapter::renderFrame(RenderSnapshot *snapshot)
{
    return impl_->renderFrame(snapshot);
}

GhosttyVtAdapter::DeferredEffects GhosttyVtAdapter::takeDeferredEffects()
{
    return impl_->takeDeferredEffects();
}
