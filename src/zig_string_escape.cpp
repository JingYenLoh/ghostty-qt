#include "zig_string_escape.h"

namespace {

enum class HexEscapeEncoding {
    RawByte,
    Utf8Codepoint,
};

constexpr int hexDigitValue(char value) noexcept
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool appendUtf8(QByteArray &output, quint32 codepoint)
{
    if (codepoint <= 0x7fU) {
        output.append(static_cast<char>(codepoint));
        return true;
    }
    if (codepoint <= 0x7ffU) {
        output.append(static_cast<char>(0xc0U | (codepoint >> 6U)));
        output.append(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        return true;
    }
    if (codepoint >= 0xd800U && codepoint <= 0xdfffU) {
        return false;
    }
    if (codepoint <= 0xffffU) {
        output.append(static_cast<char>(0xe0U | (codepoint >> 12U)));
        output.append(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.append(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        return true;
    }
    if (codepoint <= 0x10ffffU) {
        output.append(static_cast<char>(0xf0U | (codepoint >> 18U)));
        output.append(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
        output.append(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.append(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        return true;
    }
    return false;
}

std::optional<QByteArray> decodeZigEscapes(QByteArrayView serialized,
                                           HexEscapeEncoding hexEscapeEncoding)
{
    QByteArray decoded;
    decoded.reserve(serialized.size());
    qsizetype index = 0;
    while (index < serialized.size()) {
        const char value = serialized.at(index++);
        if (value != '\\') {
            decoded.append(value);
            continue;
        }
        if (index >= serialized.size()) {
            return std::nullopt;
        }

        const char escaped = serialized.at(index++);
        quint32 codepoint = 0;
        switch (escaped) {
        case 'n': codepoint = '\n'; break;
        case 'r': codepoint = '\r'; break;
        case 't': codepoint = '\t'; break;
        case '\\': codepoint = '\\'; break;
        case '\'': codepoint = '\''; break;
        case '"': codepoint = '"'; break;
        case 'x': {
            if (index + 2 > serialized.size()) {
                return std::nullopt;
            }
            const int high = hexDigitValue(serialized.at(index));
            const int low = hexDigitValue(serialized.at(index + 1));
            if (high < 0 || low < 0) {
                return std::nullopt;
            }
            codepoint = static_cast<quint32>((high << 4) | low);
            index += 2;
            if (hexEscapeEncoding == HexEscapeEncoding::RawByte) {
                decoded.append(static_cast<char>(codepoint));
                continue;
            }
            break;
        }
        case 'u': {
            if (index >= serialized.size() || serialized.at(index++) != '{'
                || index >= serialized.size() || serialized.at(index) == '}') {
                return std::nullopt;
            }
            bool closed = false;
            while (index < serialized.size()) {
                const char digit = serialized.at(index++);
                if (digit == '}') {
                    closed = true;
                    break;
                }
                const int nibble = hexDigitValue(digit);
                if (nibble < 0) {
                    return std::nullopt;
                }
                codepoint = codepoint * 16U + static_cast<quint32>(nibble);
                if (codepoint > 0x10ffffU) {
                    return std::nullopt;
                }
            }
            if (!closed) {
                return std::nullopt;
            }
            break;
        }
        default: return std::nullopt;
        }

        if (!appendUtf8(decoded, codepoint)) {
            return std::nullopt;
        }
    }
    return decoded;
}

} // namespace

std::optional<QByteArray> decodeGhosttyActionString(QByteArrayView serialized)
{
    return decodeZigEscapes(serialized, HexEscapeEncoding::RawByte);
}

std::optional<QByteArray> decodeGhosttyConfigString(QByteArrayView serialized)
{
    return decodeZigEscapes(serialized, HexEscapeEncoding::Utf8Codepoint);
}
