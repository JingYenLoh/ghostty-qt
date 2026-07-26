#include "ghostty_vt_adapter.h"

#include <ghostty/vt.h>

#include <QScopeGuard>
#include <QSet>

#include <algorithm>
#include <array>
#include <climits>
#include <compare>
#include <expected>
#include <limits>
#include <linux/input-event-codes.h>
#include <optional>
#include <string_view>
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

struct TextMapData final {
    QByteArray text;
    QVector<ScreenCell> byteCells;
};

constexpr quint64 maximumLogicalLineCells = 131'072;
constexpr qsizetype maximumLogicalLineBytes = 4 * 1024 * 1024;

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

std::optional<QString>
validatedOsc7Directory(std::string_view reported,
                       const std::function<QByteArray()> &queryMachineHostName)
{
    if (reported.empty()) {
        return QStringLiteral("");
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
        const QByteArray path = percentDecode(uri->path);
        return path.isEmpty() ? QStringLiteral("") : QString::fromUtf8(path);
    }

    // Ghostty's kitty-shell-cwd variant intentionally treats the URI path as
    // raw text. Preserve literal percent sequences instead of URL-decoding
    // them a second time.
    const std::string_view path = reported.substr(uri->pathStart);
    return path.empty()
        ? QStringLiteral("")
        : QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size()));
}

QColor toQColor(GhosttyColorRgb color)
{
    return QColor::fromRgb(color.r, color.g, color.b);
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

GhosttyVtAdapter::Geometry
normalizedGeometry(GhosttyVtAdapter::Geometry geometry)
{
    geometry.columns = boundedU16(geometry.columns);
    geometry.rows = boundedU16(geometry.rows);
    geometry.cellWidthPixels =
        static_cast<int>(boundedU32(geometry.cellWidthPixels));
    geometry.cellHeightPixels =
        static_cast<int>(boundedU32(geometry.cellHeightPixels));
    geometry.surfaceWidthPixels =
        static_cast<int>(boundedU32(geometry.surfaceWidthPixels));
    geometry.surfaceHeightPixels =
        static_cast<int>(boundedU32(geometry.surfaceHeightPixels));
    return geometry;
}

bool containsControlText(const QByteArray &text)
{
    return std::any_of(text.cbegin(), text.cend(), [](char value) {
        const auto byte = static_cast<unsigned char>(value);
        return byte < 0x20U || byte == 0x7fU;
    });
}

GhosttyKey mapQtKey(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return static_cast<GhosttyKey>(GHOSTTY_KEY_A + key - Qt::Key_A);
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return static_cast<GhosttyKey>(GHOSTTY_KEY_DIGIT_0 + key - Qt::Key_0);
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        return static_cast<GhosttyKey>(GHOSTTY_KEY_F1 + key - Qt::Key_F1);
    }

    switch (key) {
    case Qt::Key_QuoteLeft:
    case Qt::Key_AsciiTilde: return GHOSTTY_KEY_BACKQUOTE;
    case Qt::Key_Backslash:
    case Qt::Key_Bar: return GHOSTTY_KEY_BACKSLASH;
    case Qt::Key_BracketLeft:
    case Qt::Key_BraceLeft: return GHOSTTY_KEY_BRACKET_LEFT;
    case Qt::Key_BracketRight:
    case Qt::Key_BraceRight: return GHOSTTY_KEY_BRACKET_RIGHT;
    case Qt::Key_Comma:
    case Qt::Key_Less: return GHOSTTY_KEY_COMMA;
    case Qt::Key_Equal:
    case Qt::Key_Plus: return GHOSTTY_KEY_EQUAL;
    case Qt::Key_Minus:
    case Qt::Key_Underscore: return GHOSTTY_KEY_MINUS;
    case Qt::Key_Period:
    case Qt::Key_Greater: return GHOSTTY_KEY_PERIOD;
    case Qt::Key_Apostrophe:
    case Qt::Key_QuoteDbl: return GHOSTTY_KEY_QUOTE;
    case Qt::Key_Semicolon:
    case Qt::Key_Colon: return GHOSTTY_KEY_SEMICOLON;
    case Qt::Key_Slash:
    case Qt::Key_Question: return GHOSTTY_KEY_SLASH;
    case Qt::Key_Backspace: return GHOSTTY_KEY_BACKSPACE;
    case Qt::Key_Return:
    case Qt::Key_Enter: return GHOSTTY_KEY_ENTER;
    case Qt::Key_Space: return GHOSTTY_KEY_SPACE;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return GHOSTTY_KEY_TAB;
    case Qt::Key_Delete: return GHOSTTY_KEY_DELETE;
    case Qt::Key_End: return GHOSTTY_KEY_END;
    case Qt::Key_Home: return GHOSTTY_KEY_HOME;
    case Qt::Key_Insert: return GHOSTTY_KEY_INSERT;
    case Qt::Key_PageDown: return GHOSTTY_KEY_PAGE_DOWN;
    case Qt::Key_PageUp: return GHOSTTY_KEY_PAGE_UP;
    case Qt::Key_Down: return GHOSTTY_KEY_ARROW_DOWN;
    case Qt::Key_Left: return GHOSTTY_KEY_ARROW_LEFT;
    case Qt::Key_Right: return GHOSTTY_KEY_ARROW_RIGHT;
    case Qt::Key_Up: return GHOSTTY_KEY_ARROW_UP;
    case Qt::Key_Escape: return GHOSTTY_KEY_ESCAPE;
    case Qt::Key_Pause: return GHOSTTY_KEY_PAUSE;
    case Qt::Key_Print: return GHOSTTY_KEY_PRINT_SCREEN;
    case Qt::Key_ScrollLock: return GHOSTTY_KEY_SCROLL_LOCK;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
}

bool isModifierKey(GhosttyKey key)
{
    switch (key) {
    case GHOSTTY_KEY_SHIFT_LEFT:
    case GHOSTTY_KEY_SHIFT_RIGHT:
    case GHOSTTY_KEY_CONTROL_LEFT:
    case GHOSTTY_KEY_CONTROL_RIGHT:
    case GHOSTTY_KEY_ALT_LEFT:
    case GHOSTTY_KEY_ALT_RIGHT:
    case GHOSTTY_KEY_META_LEFT:
    case GHOSTTY_KEY_META_RIGHT: return true;
    default: return false;
    }
}

GhosttyKey mapNativeScanCode(quint32 nativeScanCode)
{
    if (nativeScanCode < 8U) {
        return GHOSTTY_KEY_UNIDENTIFIED;
    }
    const unsigned int code = nativeScanCode - 8U;
    switch (code) {
    case KEY_GRAVE: return GHOSTTY_KEY_BACKQUOTE;
    case KEY_BACKSLASH: return GHOSTTY_KEY_BACKSLASH;
    case KEY_LEFTBRACE: return GHOSTTY_KEY_BRACKET_LEFT;
    case KEY_RIGHTBRACE: return GHOSTTY_KEY_BRACKET_RIGHT;
    case KEY_COMMA: return GHOSTTY_KEY_COMMA;
    case KEY_0: return GHOSTTY_KEY_DIGIT_0;
    case KEY_1: return GHOSTTY_KEY_DIGIT_1;
    case KEY_2: return GHOSTTY_KEY_DIGIT_2;
    case KEY_3: return GHOSTTY_KEY_DIGIT_3;
    case KEY_4: return GHOSTTY_KEY_DIGIT_4;
    case KEY_5: return GHOSTTY_KEY_DIGIT_5;
    case KEY_6: return GHOSTTY_KEY_DIGIT_6;
    case KEY_7: return GHOSTTY_KEY_DIGIT_7;
    case KEY_8: return GHOSTTY_KEY_DIGIT_8;
    case KEY_9: return GHOSTTY_KEY_DIGIT_9;
    case KEY_EQUAL: return GHOSTTY_KEY_EQUAL;
    case KEY_102ND: return GHOSTTY_KEY_INTL_BACKSLASH;
    case KEY_RO: return GHOSTTY_KEY_INTL_RO;
    case KEY_YEN: return GHOSTTY_KEY_INTL_YEN;
    case KEY_A: return GHOSTTY_KEY_A;
    case KEY_B: return GHOSTTY_KEY_B;
    case KEY_C: return GHOSTTY_KEY_C;
    case KEY_D: return GHOSTTY_KEY_D;
    case KEY_E: return GHOSTTY_KEY_E;
    case KEY_F: return GHOSTTY_KEY_F;
    case KEY_G: return GHOSTTY_KEY_G;
    case KEY_H: return GHOSTTY_KEY_H;
    case KEY_I: return GHOSTTY_KEY_I;
    case KEY_J: return GHOSTTY_KEY_J;
    case KEY_K: return GHOSTTY_KEY_K;
    case KEY_L: return GHOSTTY_KEY_L;
    case KEY_M: return GHOSTTY_KEY_M;
    case KEY_N: return GHOSTTY_KEY_N;
    case KEY_O: return GHOSTTY_KEY_O;
    case KEY_P: return GHOSTTY_KEY_P;
    case KEY_Q: return GHOSTTY_KEY_Q;
    case KEY_R: return GHOSTTY_KEY_R;
    case KEY_S: return GHOSTTY_KEY_S;
    case KEY_T: return GHOSTTY_KEY_T;
    case KEY_U: return GHOSTTY_KEY_U;
    case KEY_V: return GHOSTTY_KEY_V;
    case KEY_W: return GHOSTTY_KEY_W;
    case KEY_X: return GHOSTTY_KEY_X;
    case KEY_Y: return GHOSTTY_KEY_Y;
    case KEY_Z: return GHOSTTY_KEY_Z;
    case KEY_MINUS: return GHOSTTY_KEY_MINUS;
    case KEY_DOT: return GHOSTTY_KEY_PERIOD;
    case KEY_APOSTROPHE: return GHOSTTY_KEY_QUOTE;
    case KEY_SEMICOLON: return GHOSTTY_KEY_SEMICOLON;
    case KEY_SLASH: return GHOSTTY_KEY_SLASH;
    case KEY_LEFTALT: return GHOSTTY_KEY_ALT_LEFT;
    case KEY_RIGHTALT: return GHOSTTY_KEY_ALT_RIGHT;
    case KEY_BACKSPACE: return GHOSTTY_KEY_BACKSPACE;
    case KEY_CAPSLOCK: return GHOSTTY_KEY_CAPS_LOCK;
    case KEY_MENU: return GHOSTTY_KEY_CONTEXT_MENU;
    case KEY_LEFTCTRL: return GHOSTTY_KEY_CONTROL_LEFT;
    case KEY_RIGHTCTRL: return GHOSTTY_KEY_CONTROL_RIGHT;
    case KEY_ENTER: return GHOSTTY_KEY_ENTER;
    case KEY_LEFTMETA: return GHOSTTY_KEY_META_LEFT;
    case KEY_RIGHTMETA: return GHOSTTY_KEY_META_RIGHT;
    case KEY_LEFTSHIFT: return GHOSTTY_KEY_SHIFT_LEFT;
    case KEY_RIGHTSHIFT: return GHOSTTY_KEY_SHIFT_RIGHT;
    case KEY_SPACE: return GHOSTTY_KEY_SPACE;
    case KEY_TAB: return GHOSTTY_KEY_TAB;
    case KEY_DELETE: return GHOSTTY_KEY_DELETE;
    case KEY_END: return GHOSTTY_KEY_END;
    case KEY_HELP: return GHOSTTY_KEY_HELP;
    case KEY_HOME: return GHOSTTY_KEY_HOME;
    case KEY_INSERT: return GHOSTTY_KEY_INSERT;
    case KEY_PAGEDOWN: return GHOSTTY_KEY_PAGE_DOWN;
    case KEY_PAGEUP: return GHOSTTY_KEY_PAGE_UP;
    case KEY_DOWN: return GHOSTTY_KEY_ARROW_DOWN;
    case KEY_LEFT: return GHOSTTY_KEY_ARROW_LEFT;
    case KEY_RIGHT: return GHOSTTY_KEY_ARROW_RIGHT;
    case KEY_UP: return GHOSTTY_KEY_ARROW_UP;
    case KEY_NUMLOCK: return GHOSTTY_KEY_NUM_LOCK;
    case KEY_KP0: return GHOSTTY_KEY_NUMPAD_0;
    case KEY_KP1: return GHOSTTY_KEY_NUMPAD_1;
    case KEY_KP2: return GHOSTTY_KEY_NUMPAD_2;
    case KEY_KP3: return GHOSTTY_KEY_NUMPAD_3;
    case KEY_KP4: return GHOSTTY_KEY_NUMPAD_4;
    case KEY_KP5: return GHOSTTY_KEY_NUMPAD_5;
    case KEY_KP6: return GHOSTTY_KEY_NUMPAD_6;
    case KEY_KP7: return GHOSTTY_KEY_NUMPAD_7;
    case KEY_KP8: return GHOSTTY_KEY_NUMPAD_8;
    case KEY_KP9: return GHOSTTY_KEY_NUMPAD_9;
    case KEY_KPPLUS: return GHOSTTY_KEY_NUMPAD_ADD;
    case KEY_KPCOMMA: return GHOSTTY_KEY_NUMPAD_COMMA;
    case KEY_KPDOT: return GHOSTTY_KEY_NUMPAD_DECIMAL;
    case KEY_KPSLASH: return GHOSTTY_KEY_NUMPAD_DIVIDE;
    case KEY_KPENTER: return GHOSTTY_KEY_NUMPAD_ENTER;
    case KEY_KPEQUAL: return GHOSTTY_KEY_NUMPAD_EQUAL;
    case KEY_KPASTERISK: return GHOSTTY_KEY_NUMPAD_MULTIPLY;
    case KEY_KPMINUS: return GHOSTTY_KEY_NUMPAD_SUBTRACT;
    case KEY_ESC: return GHOSTTY_KEY_ESCAPE;
    case KEY_PAUSE: return GHOSTTY_KEY_PAUSE;
    case KEY_SYSRQ: return GHOSTTY_KEY_PRINT_SCREEN;
    case KEY_SCROLLLOCK: return GHOSTTY_KEY_SCROLL_LOCK;
    case KEY_COPY: return GHOSTTY_KEY_COPY;
    case KEY_CUT: return GHOSTTY_KEY_CUT;
    case KEY_PASTE: return GHOSTTY_KEY_PASTE;
    case KEY_F1: return GHOSTTY_KEY_F1;
    case KEY_F2: return GHOSTTY_KEY_F2;
    case KEY_F3: return GHOSTTY_KEY_F3;
    case KEY_F4: return GHOSTTY_KEY_F4;
    case KEY_F5: return GHOSTTY_KEY_F5;
    case KEY_F6: return GHOSTTY_KEY_F6;
    case KEY_F7: return GHOSTTY_KEY_F7;
    case KEY_F8: return GHOSTTY_KEY_F8;
    case KEY_F9: return GHOSTTY_KEY_F9;
    case KEY_F10: return GHOSTTY_KEY_F10;
    case KEY_F11: return GHOSTTY_KEY_F11;
    case KEY_F12: return GHOSTTY_KEY_F12;
    case KEY_F13: return GHOSTTY_KEY_F13;
    case KEY_F14: return GHOSTTY_KEY_F14;
    case KEY_F15: return GHOSTTY_KEY_F15;
    case KEY_F16: return GHOSTTY_KEY_F16;
    case KEY_F17: return GHOSTTY_KEY_F17;
    case KEY_F18: return GHOSTTY_KEY_F18;
    case KEY_F19: return GHOSTTY_KEY_F19;
    case KEY_F20: return GHOSTTY_KEY_F20;
    case KEY_F21: return GHOSTTY_KEY_F21;
    case KEY_F22: return GHOSTTY_KEY_F22;
    case KEY_F23: return GHOSTTY_KEY_F23;
    case KEY_F24: return GHOSTTY_KEY_F24;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
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
        const quint64 maximum =
            static_cast<quint64>(std::numeric_limits<size_t>::max());
        const GhosttyTerminalOptions options{
            .cols = boundedU16(geometry_.columns),
            .rows = boundedU16(geometry_.rows),
            .max_scrollback = static_cast<size_t>(
                std::min(adapterOptions.scrollbackBytes, maximum)),
        };
        if (ghostty_terminal_new(nullptr, &terminal_, options)
            != GHOSTTY_SUCCESS) {
            return false;
        }

        if (!setAppearance(adapterOptions.appearance)) {
            return false;
        }

        ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_USERDATA, this);
        ghostty_terminal_set(
            terminal_, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
            reinterpret_cast<const void *>(&Impl::writePtyCallback));
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

        if (ghostty_render_state_new(nullptr, &renderState_) != GHOSTTY_SUCCESS
            || ghostty_render_state_row_iterator_new(nullptr, &rowIterator_)
                != GHOSTTY_SUCCESS
            || ghostty_render_state_row_cells_new(nullptr, &rowCells_)
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

    void destroy()
    {
        if (selectionReleaseEvent_ != nullptr) {
            ghostty_selection_gesture_event_free(selectionReleaseEvent_);
            selectionReleaseEvent_ = nullptr;
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
        pendingCurrentDirectory_ = QStringLiteral("");

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
        (void)ghostty_terminal_mode_set(terminal_, GHOSTTY_MODE_KAM, false);
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
        uint32_t fingerprint = 0;
        for (size_t index = 0; index < modes.size(); ++index) {
            bool enabled = false;
            if (ghostty_terminal_mode_get(terminal_, modes[index], &enabled)
                    == GHOSTTY_SUCCESS
                && enabled) {
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

        GhosttyKey key = mapNativeScanCode(input.nativeScanCode);
        if (key == GHOSTTY_KEY_UNIDENTIFIED) {
            key = mapQtKey(input.key);
        }
        const auto qtModifiers =
            static_cast<Qt::KeyboardModifiers>(input.modifiers);
        if (qtModifiers.testFlag(Qt::KeypadModifier)) {
            if (input.key >= Qt::Key_0 && input.key <= Qt::Key_9) {
                key = static_cast<GhosttyKey>(GHOSTTY_KEY_NUMPAD_0 + input.key
                                              - Qt::Key_0);
            } else if (input.key == Qt::Key_Enter
                       || input.key == Qt::Key_Return) {
                key = GHOSTTY_KEY_NUMPAD_ENTER;
            } else if (input.key == Qt::Key_Plus) {
                key = GHOSTTY_KEY_NUMPAD_ADD;
            } else if (input.key == Qt::Key_Minus) {
                key = GHOSTTY_KEY_NUMPAD_SUBTRACT;
            } else if (input.key == Qt::Key_Asterisk) {
                key = GHOSTTY_KEY_NUMPAD_MULTIPLY;
            } else if (input.key == Qt::Key_Slash) {
                key = GHOSTTY_KEY_NUMPAD_DIVIDE;
            } else if (input.key == Qt::Key_Period) {
                key = GHOSTTY_KEY_NUMPAD_DECIMAL;
            } else if (input.key == Qt::Key_Comma) {
                key = GHOSTTY_KEY_NUMPAD_COMMA;
            } else if (input.key == Qt::Key_Equal) {
                key = GHOSTTY_KEY_NUMPAD_EQUAL;
            }
        }

        ghostty_key_event_set_key(keyEvent_, key);
        ghostty_key_event_set_mods(keyEvent_, mapQtModifiers(input.modifiers));
        ghostty_key_event_set_consumed_mods(keyEvent_, 0);
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
            .modifier = isModifierKey(key),
            .escape = key == GHOSTTY_KEY_ESCAPE,
        };
    }

    QByteArray encodeMouse(const TerminalMouseInput &input)
    {
        GhosttyMouseEncoderSize size{};
        size.size = sizeof(size);
        size.screen_width = boundedU32(geometry_.surfaceWidthPixels);
        size.screen_height = boundedU32(geometry_.surfaceHeightPixels);
        size.cell_width = boundedU32(geometry_.cellWidthPixels);
        size.cell_height = boundedU32(geometry_.cellHeightPixels);
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

    QByteArray encodeFocus(bool focused) const
    {
        bool reportFocus = false;
        if (ghostty_terminal_mode_get(terminal_, GHOSTTY_MODE_FOCUS_EVENT,
                                      &reportFocus)
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
        (void)ghostty_terminal_mode_get(terminal_, GHOSTTY_MODE_BRACKETED_PASTE,
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

        size_t required = 0;
        GhosttyResult result =
            ghostty_formatter_format_buf(formatter, nullptr, 0, &required);
        if ((result != GHOSTTY_OUT_OF_SPACE && result != GHOSTTY_SUCCESS)
            || required
                > static_cast<size_t>(std::numeric_limits<qsizetype>::max())) {
            return withoutBytes(PlainFileSnapshotStatus::Failed);
        }
        if (required == 0) {
            return withoutBytes(PlainFileSnapshotStatus::Ready);
        }

        QByteArray output(static_cast<qsizetype>(required), Qt::Uninitialized);
        size_t written = 0;
        result = ghostty_formatter_format_buf(
            formatter, reinterpret_cast<uint8_t *>(output.data()),
            static_cast<size_t>(output.size()), &written);
        if (result != GHOSTTY_SUCCESS
            || written > static_cast<size_t>(output.size())) {
            return withoutBytes(PlainFileSnapshotStatus::Failed);
        }
        output.resize(static_cast<qsizetype>(written));
        return {
            .status = PlainFileSnapshotStatus::Ready,
            .bytes = std::move(output),
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

    void clearSelectionAndResetGesture()
    {
        clearSelection();
        ghostty_selection_gesture_reset(selectionGesture_, terminal_);
        lastSelectionPressTimestampNanoseconds_.reset();
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

    std::optional<TextMapData>
    textMapBetween(ScreenCell start, ScreenCell end,
                   bool includeTrailingEmptyStorage = false) const
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

        TextMapData data;
        QVector<ScreenCell> pendingBlanks;
        const auto appendMapped = [&data](QByteArrayView bytes,
                                          const ScreenCell &cell) {
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
            for (const ScreenCell &blank : std::as_const(pendingBlanks)) {
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
                    pendingBlanks.append(cell);
                    continue;
                }

                if (!flushBlanks() || !appendMapped(*grapheme, cell)) {
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
        const auto replacementGuard = qScopeGuard([&] {
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
                != GHOSTTY_SUCCESS) {
            return false;
        }
        if (!setSelectionWordBoundaries(replacementPressEvent,
                                        wordBoundaryCodepoints)
            || !setSelectionWordBoundaries(replacementDragEvent,
                                           wordBoundaryCodepoints)) {
            return false;
        }

        std::swap(selectionPressEvent_, replacementPressEvent);
        std::swap(selectionDragEvent_, replacementDragEvent);
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
            .padding_left = 0,
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
        std::optional<TextMapData> data = textMapBetween(start, end, wrapped);
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
        snapshot.screenRow = screenRow;
        snapshot.text = std::move(data->text);
        snapshot.byteCells.reserve(data->byteCells.size());
        for (const ScreenCell &cell : std::as_const(data->byteCells)) {
            snapshot.byteCells.append(TerminalSearchCell{
                .column = cell.x,
                .screenRow = cell.y,
            });
        }
        snapshot.wrapped = wrapped;
        snapshot.newlineCell = snapshot.byteCells.isEmpty()
            ? TerminalSearchCell{.column = 0, .screenRow = screenRow}
            : snapshot.byteCells.constLast();
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
        if (ghostty_render_state_get(renderState_,
                                     GHOSTTY_RENDER_STATE_DATA_DIRTY, &dirty)
                != GHOSTTY_SUCCESS
            || ghostty_render_state_get(
                   renderState_, GHOSTTY_RENDER_STATE_DATA_COLS, &columns)
                != GHOSTTY_SUCCESS
            || ghostty_render_state_get(renderState_,
                                        GHOSTTY_RENDER_STATE_DATA_ROWS, &rows)
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

        GhosttyRenderStateColors colors{};
        colors.size = sizeof(colors);
        if (ghostty_render_state_colors_get(renderState_, &colors)
            != GHOSTTY_SUCCESS) {
            return RenderResult::Retry;
        }
        metadata.foreground = toQColor(colors.foreground);
        metadata.background = toQColor(colors.background);
        const bool paletteChanged = !std::ranges::equal(
            colors.palette, std::as_const(publishedMetadata_.palette),
            [](const GhosttyColorRgb &current, const QColor &published) {
                return published.rgb() == qRgb(current.r, current.g, current.b);
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
        metadata.cursorColor = colors.cursor_has_value ? toQColor(colors.cursor)
                                                       : metadata.foreground;

        if (ghostty_render_state_get(renderState_,
                                     GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE,
                                     &metadata.cursorVisible)
                != GHOSTTY_SUCCESS
            || ghostty_render_state_get(
                   renderState_, GHOSTTY_RENDER_STATE_DATA_CURSOR_BLINKING,
                   &metadata.cursorBlinking)
                != GHOSTTY_SUCCESS) {
            return RenderResult::Retry;
        }
        bool cursorInViewport = false;
        if (ghostty_render_state_get(
                renderState_,
                GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE,
                &cursorInViewport)
            != GHOSTTY_SUCCESS) {
            return RenderResult::Retry;
        }
        metadata.cursorVisible = metadata.cursorVisible && cursorInViewport;
        uint16_t cursorColumn = 0;
        uint16_t cursorRow = 0;
        bool cursorOnWideTail = false;
        GhosttyRenderStateCursorVisualStyle cursorStyle =
            GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK;
        if (cursorInViewport) {
            if (ghostty_render_state_get(
                    renderState_, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X,
                    &cursorColumn)
                    != GHOSTTY_SUCCESS
                || ghostty_render_state_get(
                       renderState_,
                       GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cursorRow)
                    != GHOSTTY_SUCCESS
                || ghostty_render_state_get(
                       renderState_,
                       GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_WIDE_TAIL,
                       &cursorOnWideTail)
                    != GHOSTTY_SUCCESS) {
                return RenderResult::Retry;
            }
        }
        if (ghostty_render_state_get(
                renderState_, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISUAL_STYLE,
                &cursorStyle)
            != GHOSTTY_SUCCESS) {
            return RenderResult::Retry;
        }
        metadata.cursorColumn = static_cast<int>(cursorColumn);
        metadata.cursorRow = static_cast<int>(cursorRow);
        metadata.cursorStyle = static_cast<int>(cursorStyle);
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

        TerminalUpdate update;
        update.columns = metadata.columns;
        update.rows = metadata.rows;
        update.fullFrame = fullFrame;
        if (fullFrame) {
            update.dirtyRows.reserve(metadata.rows);
        }

        const bool inspectCursorCell = metadata.cursorVisible
            && !cursorOnWideTail && metadata.cursorColumn >= 0
            && metadata.cursorColumn < metadata.columns
            && metadata.cursorRow >= 0 && metadata.cursorRow < metadata.rows;
        const bool visitRows = fullFrame
            || dirty == GHOSTTY_RENDER_STATE_DIRTY_PARTIAL || inspectCursorCell;
        if (visitRows) {
            if (ghostty_render_state_get(renderState_,
                                         GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                         &rowIterator_)
                != GHOSTTY_SUCCESS) {
                return RenderResult::Retry;
            }

            int rowIndex = 0;
            while (rowIndex < metadata.rows
                   && ghostty_render_state_row_iterator_next(rowIterator_)) {
                bool rowDirty = false;
                if (ghostty_render_state_row_get(
                        rowIterator_, GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY,
                        &rowDirty)
                    != GHOSTTY_SUCCESS) {
                    return RenderResult::Retry;
                }
                const bool copyRow = fullFrame || rowDirty;
                const bool cursorRowNeedsInspection =
                    inspectCursorCell && rowIndex == metadata.cursorRow;
                if (!copyRow && !cursorRowNeedsInspection) {
                    ++rowIndex;
                    continue;
                }

                GhosttyRenderStateRowSelection rowSelection{};
                rowSelection.size = sizeof(rowSelection);
                const bool hasSelection = copyRow
                    && ghostty_render_state_row_get(
                           rowIterator_,
                           GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION,
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
                if (copyRow) {
                    rowUpdate.cells.resize(metadata.columns);
                }
                int columnIndex = 0;
                while (columnIndex < metadata.columns
                       && ghostty_render_state_row_cells_next(rowCells_)) {
                    GhosttyCell rawCell = 0;
                    GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
                    bool hasHyperlink = false;
                    if (ghostty_render_state_row_cells_get(
                            rowCells_, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW,
                            &rawCell)
                            != GHOSTTY_SUCCESS
                        || ghostty_cell_get(rawCell, GHOSTTY_CELL_DATA_WIDE,
                                            &wide)
                            != GHOSTTY_SUCCESS
                        || ghostty_cell_get(rawCell,
                                            GHOSTTY_CELL_DATA_HAS_HYPERLINK,
                                            &hasHyperlink)
                            != GHOSTTY_SUCCESS) {
                        return RenderResult::Retry;
                    }

                    if (cursorRowNeedsInspection
                        && columnIndex == metadata.cursorColumn) {
                        metadata.cursorColumnSpan =
                            wide == GHOSTTY_CELL_WIDE_WIDE ? 2 : 1;
                    }
                    if (!copyRow) {
                        ++columnIndex;
                        continue;
                    }

                    TerminalCell &cell = rowUpdate.cells[columnIndex];
                    cell.columnSpan = wide == GHOSTTY_CELL_WIDE_WIDE ? 2 : 1;
                    cell.hasHyperlink = hasHyperlink;
                    cell.spacer = wide == GHOSTTY_CELL_WIDE_SPACER_TAIL
                        || wide == GHOSTTY_CELL_WIDE_SPACER_HEAD;

                    std::array<uint8_t, 64> graphemeStorage{};
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
                    QByteArray dynamicGrapheme;
                    if (graphemeResult == GHOSTTY_OUT_OF_SPACE) {
                        dynamicGrapheme.resize(
                            static_cast<qsizetype>(graphemeBuffer.len));
                        graphemeBuffer.ptr =
                            reinterpret_cast<uint8_t *>(dynamicGrapheme.data());
                        graphemeBuffer.cap =
                            static_cast<size_t>(dynamicGrapheme.size());
                        graphemeResult = ghostty_render_state_row_cells_get(
                            rowCells_,
                            GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8,
                            &graphemeBuffer);
                    }
                    if (graphemeResult != GHOSTTY_SUCCESS) {
                        return RenderResult::Retry;
                    }
                    if (graphemeBuffer.len > 0) {
                        cell.text = QString::fromUtf8(
                            reinterpret_cast<const char *>(graphemeBuffer.ptr),
                            static_cast<qsizetype>(graphemeBuffer.len));
                    }

                    GhosttyStyle style{};
                    style.size = sizeof(style);
                    if (ghostty_render_state_row_cells_get(
                            rowCells_,
                            GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style)
                        != GHOSTTY_SUCCESS) {
                        return RenderResult::Retry;
                    }
                    GhosttyColorRgb foreground = colors.foreground;
                    GhosttyColorRgb background = colors.background;
                    GhosttyColorRgb explicitColor{};
                    if (ghostty_render_state_row_cells_get(
                            rowCells_,
                            GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR,
                            &explicitColor)
                        == GHOSTTY_SUCCESS) {
                        foreground = explicitColor;
                    }
                    if (ghostty_render_state_row_cells_get(
                            rowCells_,
                            GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR,
                            &explicitColor)
                        == GHOSTTY_SUCCESS) {
                        background = explicitColor;
                    }
                    if (style.inverse) {
                        std::swap(foreground, background);
                    }

                    cell.foreground = toQColor(foreground);
                    cell.background = toQColor(background);
                    cell.underlineColor = toQColor(resolveStyleColor(
                        style.underline_color, colors, foreground));
                    switch (style.fg_color.tag) {
                    case GHOSTTY_STYLE_COLOR_PALETTE:
                        cell.styleForegroundSource =
                            TerminalColorSource::Palette;
                        cell.styleForegroundPaletteIndex =
                            static_cast<int>(style.fg_color.value.palette);
                        break;
                    case GHOSTTY_STYLE_COLOR_RGB:
                        cell.styleForegroundSource = TerminalColorSource::Rgb;
                        break;
                    default:
                        cell.styleForegroundSource =
                            TerminalColorSource::Default;
                        break;
                    }
                    cell.bold = style.bold;
                    cell.italic = style.italic;
                    cell.faint = style.faint;
                    cell.textBlink = style.blink;
                    cell.inverse = style.inverse;
                    cell.invisible = style.invisible;
                    cell.underlineUsesForeground =
                        style.underline_color.tag == GHOSTTY_STYLE_COLOR_NONE;
                    switch (style.underline) {
                    case GHOSTTY_SGR_UNDERLINE_SINGLE:
                        cell.underlineStyle = TerminalUnderlineStyle::Single;
                        break;
                    case GHOSTTY_SGR_UNDERLINE_DOUBLE:
                        cell.underlineStyle = TerminalUnderlineStyle::Double;
                        break;
                    case GHOSTTY_SGR_UNDERLINE_CURLY:
                        cell.underlineStyle = TerminalUnderlineStyle::Curly;
                        break;
                    case GHOSTTY_SGR_UNDERLINE_DOTTED:
                        cell.underlineStyle = TerminalUnderlineStyle::Dotted;
                        break;
                    case GHOSTTY_SGR_UNDERLINE_DASHED:
                        cell.underlineStyle = TerminalUnderlineStyle::Dashed;
                        break;
                    default:
                        cell.underlineStyle = TerminalUnderlineStyle::None;
                        break;
                    }
                    cell.strikeThrough = style.strikethrough;
                    cell.overline = style.overline;
                    cell.selected = hasSelection
                        && columnIndex >= static_cast<int>(rowSelection.start_x)
                        && columnIndex <= static_cast<int>(rowSelection.end_x);
                    if (style.invisible || cell.spacer) {
                        cell.text.clear();
                    }
                    ++columnIndex;
                }
                if (columnIndex != metadata.columns) {
                    return RenderResult::Retry;
                }
                if (copyRow) {
                    update.dirtyRows.append(std::move(rowUpdate));
                }
                ++rowIndex;
            }
            if (rowIndex != metadata.rows) {
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

        const auto setRowsDirty =
            [this](const QVector<TerminalRowUpdate> &rowUpdates, bool value) {
                if (rowUpdates.isEmpty()) {
                    return true;
                }
                if (ghostty_render_state_get(
                        renderState_, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                        &rowIterator_)
                    != GHOSTTY_SUCCESS) {
                    return false;
                }
                qsizetype target = 0;
                int rowIndex = 0;
                while (
                    target < rowUpdates.size()
                    && ghostty_render_state_row_iterator_next(rowIterator_)) {
                    if (rowIndex == rowUpdates.at(target).row) {
                        if (ghostty_render_state_row_set(
                                rowIterator_,
                                GHOSTTY_RENDER_STATE_ROW_OPTION_DIRTY, &value)
                            != GHOSTTY_SUCCESS) {
                            return false;
                        }
                        ++target;
                    }
                    ++rowIndex;
                }
                return target == rowUpdates.size();
            };

        const bool clean = false;
        if (!setRowsDirty(update.dirtyRows, clean)) {
            const bool dirtyRow = true;
            setRowsDirty(update.dirtyRows, dirtyRow);
            return RenderResult::Retry;
        }
        const GhosttyRenderStateDirty cleanState =
            GHOSTTY_RENDER_STATE_DIRTY_FALSE;
        if (ghostty_render_state_set(
                renderState_, GHOSTTY_RENDER_STATE_OPTION_DIRTY, &cleanState)
            != GHOSTTY_SUCCESS) {
            const bool dirtyRow = true;
            setRowsDirty(update.dirtyRows, dirtyRow);
            return RenderResult::Retry;
        }

        bool tracking = false;
        ghostty_terminal_get(terminal_, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING,
                             &tracking);
        publishedMetadata_ = std::move(metadata);
        hasPublishedFrame_ = true;
        snapshot->update = std::move(update);
        snapshot->mouseTracking = tracking;
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
                QByteArray(reinterpret_cast<const char *>(data),
                           static_cast<qsizetype>(length)));
        }
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

    static bool colorSchemeCallback(GhosttyTerminal, void *,
                                    GhosttyColorScheme *scheme)
    {
        if (scheme == nullptr) {
            return false;
        }
        *scheme = GHOSTTY_COLOR_SCHEME_DARK;
        return true;
    }

    static bool deviceAttributesCallback(GhosttyTerminal, void *,
                                         GhosttyDeviceAttributes *attributes)
    {
        if (attributes == nullptr) {
            return false;
        }
        *attributes = GhosttyDeviceAttributes{};
        attributes->primary.conformance_level = GHOSTTY_DA_CONFORMANCE_VT220;
        attributes->primary.features[0] = GHOSTTY_DA_FEATURE_ANSI_COLOR;
        attributes->primary.num_features = 1;
        attributes->secondary.device_type = GHOSTTY_DA_DEVICE_TYPE_VT220;
        return true;
    }

    static GhosttyClipboardWriteResult
    clipboardWriteCallback(GhosttyTerminal, void *,
                           const GhosttyClipboardWrite *)
    {
        return GHOSTTY_CLIPBOARD_WRITE_RESULT_DENIED;
    }

    Geometry geometry_;
    Callbacks callbacks_;
    std::shared_ptr<const AdapterOwnerToken> ownerToken_;
    GhosttyTerminal terminal_ = nullptr;
    GhosttyRenderState renderState_ = nullptr;
    GhosttyRenderStateRowIterator rowIterator_ = nullptr;
    GhosttyRenderStateRowCells rowCells_ = nullptr;
    GhosttyKeyEncoder keyEncoder_ = nullptr;
    GhosttyKeyEvent keyEvent_ = nullptr;
    GhosttyMouseEncoder mouseEncoder_ = nullptr;
    GhosttyMouseEvent mouseEvent_ = nullptr;
    GhosttySelectionGesture selectionGesture_ = nullptr;
    GhosttySelectionGestureEvent selectionPressEvent_ = nullptr;
    GhosttySelectionGestureEvent selectionDragEvent_ = nullptr;
    GhosttySelectionGestureEvent selectionReleaseEvent_ = nullptr;
    QVector<uint32_t> selectionWordChars_;
    quint32 clickRepeatIntervalMilliseconds_ = 500;
    std::optional<quint64> lastSelectionPressTimestampNanoseconds_;
    TerminalFrame publishedMetadata_;
    bool hasPublishedFrame_ = false;
    bool titleDirty_ = false;
    std::optional<QString> pendingCurrentDirectory_;
    bool bellPending_ = false;
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

void GhosttyVtAdapter::writeVt(QByteArrayView data)
{
    impl_->writeVt(data);
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

bool GhosttyVtAdapter::mouseTracking() const
{
    return impl_->mouseTracking();
}

QByteArray GhosttyVtAdapter::encodeMouse(const TerminalMouseInput &input)
{
    return impl_->encodeMouse(input);
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

std::optional<GhosttyVtAdapter::TextRangeMatch>
GhosttyVtAdapter::resolveTextRange(const TrackedTextRange &range) const
{
    if (range.impl_ == nullptr) {
        return std::nullopt;
    }
    return impl_->resolveTextRange(*range.impl_);
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
