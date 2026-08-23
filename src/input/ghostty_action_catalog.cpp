#include "input/ghostty_action_catalog.h"
#include "input/zig_string_escape.h"

#include <QChar>
#include <QLatin1StringView>
#include <QStringDecoder>
#include <QtCore/qnamespace.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <locale.h>
#include <memory>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using Error = GhosttyActionTranslationError;
using OptionalView = std::optional<QStringView>;
namespace PaneAction = GhosttyPaneActions;
namespace FrontendAction = WorkspaceFrontendActions;

locale_t cNumericLocale()
{
    using Locale = std::remove_pointer_t<locale_t>;
    static const std::unique_ptr<Locale, decltype(&freelocale)> locale{
        newlocale(LC_NUMERIC_MASK, "C", nullptr),
        &freelocale,
    };
    return locale.get();
}

bool equals(QStringView value, QLatin1StringView expected)
{
    return value == expected;
}

std::optional<quint64> parseUnsignedInteger(QStringView value)
{
    // Binding.Action.parse ultimately delegates integer fields to
    // std.fmt.parseInt with base 10. Match its optional sign and interior
    // underscore handling rather than QString's more permissive conversion.
    if (value.isEmpty()) return std::nullopt;

    bool negative = false;
    if (value.front() == u'+' || value.front() == u'-') {
        negative = value.front() == u'-';
        value = value.sliced(1);
    }
    if (value.isEmpty() || value.front() == u'_' || value.back() == u'_') {
        return std::nullopt;
    }

    constexpr quint64 limit =
        static_cast<quint64>(std::numeric_limits<quintptr>::max());
    quint64 result = 0;
    for (const QChar character : value) {
        if (character == u'_') continue;
        if (character < u'0' || character > u'9') return std::nullopt;

        const quint64 digit = character.unicode() - u'0';
        if (result > (limit - digit) / 10) {
            return std::nullopt;
        }
        result = result * 10 + digit;
    }

    // Zig accepts -0 for unsigned parseInt fields, but no other negative
    // value can be represented by usize/u16.
    if (negative && result != 0) return std::nullopt;
    return result;
}

std::optional<qint64> parseSignedInteger(QStringView value)
{
    if (value.isEmpty()) return std::nullopt;

    bool negative = false;
    if (value.front() == u'+' || value.front() == u'-') {
        negative = value.front() == u'-';
        value = value.sliced(1);
    }
    if (value.isEmpty() || value.front() == u'_' || value.back() == u'_') {
        return std::nullopt;
    }

    constexpr quint64 positiveLimit =
        static_cast<quint64>(std::numeric_limits<qintptr>::max());
    constexpr quint64 negativeLimit = positiveLimit + 1;
    const quint64 limit = negative ? negativeLimit : positiveLimit;
    quint64 magnitude = 0;
    for (const QChar character : value) {
        if (character == u'_') continue;
        if (character < u'0' || character > u'9') return std::nullopt;

        const quint64 digit = character.unicode() - u'0';
        if (magnitude > (limit - digit) / 10) return std::nullopt;
        magnitude = magnitude * 10 + digit;
    }

    if (!negative) return static_cast<qint64>(magnitude);
    constexpr quint64 qint64NegativeLimit =
        static_cast<quint64>(std::numeric_limits<qint64>::max()) + 1;
    if (magnitude == qint64NegativeLimit) {
        return std::numeric_limits<qint64>::min();
    }
    return -static_cast<qint64>(magnitude);
}

bool isDigitForBase(QChar character, int base)
{
    if (character >= u'0' && character <= u'9') {
        return character.unicode() - u'0' < base;
    }
    if (base == 16) {
        const QChar lower = character.toLower();
        return lower >= u'a' && lower <= u'f';
    }
    return false;
}

bool consumeFloatDigits(QStringView value, qsizetype *index, int base,
                        std::string *normalized, bool *consumedAny)
{
    if (index == nullptr || normalized == nullptr || consumedAny == nullptr) {
        return false;
    }

    while (*index < value.size()) {
        const QChar character = value.at(*index);
        if (isDigitForBase(character, base)) {
            normalized->push_back(static_cast<char>(character.unicode()));
            *consumedAny = true;
            ++*index;
            continue;
        }
        if (character != u'_') break;

        // Zig's float parser permits underscores only when both neighbors are
        // digits in the active mantissa/exponent base. In particular, unlike
        // parseInt, consecutive underscores are invalid.
        if (*index == 0 || *index + 1 >= value.size()
            || !isDigitForBase(value.at(*index - 1), base)
            || !isDigitForBase(value.at(*index + 1), base)) {
            return false;
        }
        ++*index;
    }
    return true;
}

std::optional<float> parseFloat32(QStringView value)
{
    if (value.isEmpty()) return std::nullopt;

    qsizetype index = 0;
    bool negative = false;
    if (value.front() == u'+' || value.front() == u'-') {
        negative = value.front() == u'-';
        ++index;
    }
    if (index >= value.size()) return std::nullopt;

    const QStringView unsignedValue = value.sliced(index);
    if (unsignedValue.compare(QLatin1StringView("nan"), Qt::CaseInsensitive)
        == 0) {
        // Zig discards the sign when parsing NaN.
        return std::numeric_limits<float>::quiet_NaN();
    }
    if (unsignedValue.compare(QLatin1StringView("inf"), Qt::CaseInsensitive)
            == 0
        || unsignedValue.compare(QLatin1StringView("infinity"),
                                 Qt::CaseInsensitive)
            == 0) {
        const float infinity = std::numeric_limits<float>::infinity();
        return negative ? -infinity : infinity;
    }

    const bool hexadecimal = index + 1 < value.size() && value.at(index) == u'0'
        && (value.at(index + 1) == u'x' || value.at(index + 1) == u'X');
    const int mantissaBase = hexadecimal ? 16 : 10;

    std::string normalized;
    normalized.reserve(static_cast<size_t>(value.size()));
    if (negative) normalized.push_back('-');
    if (hexadecimal) {
        normalized.append("0x");
        index += 2;
    }

    bool hasMantissaDigit = false;
    if (!consumeFloatDigits(value, &index, mantissaBase, &normalized,
                            &hasMantissaDigit)) {
        return std::nullopt;
    }
    if (index < value.size() && value.at(index) == u'.') {
        normalized.push_back('.');
        ++index;
        if (!consumeFloatDigits(value, &index, mantissaBase, &normalized,
                                &hasMantissaDigit)) {
            return std::nullopt;
        }
    }
    if (!hasMantissaDigit) return std::nullopt;

    const QChar exponentMarker = hexadecimal ? u'p' : u'e';
    if (index < value.size() && value.at(index).toLower() == exponentMarker) {
        normalized.push_back(static_cast<char>(value.at(index).unicode()));
        ++index;
        if (index < value.size()
            && (value.at(index) == u'+' || value.at(index) == u'-')) {
            normalized.push_back(static_cast<char>(value.at(index).unicode()));
            ++index;
        }
        bool hasExponentDigit = false;
        if (!consumeFloatDigits(value, &index, 10, &normalized,
                                &hasExponentDigit)
            || !hasExponentDigit) {
            return std::nullopt;
        }
    }
    if (index != value.size()) return std::nullopt;

    // The project is Linux-only, so use the POSIX locale-specific conversion
    // to obtain the same binary32 rounding independent of the process locale.
    // A strict lexer above prevents strtof_l's whitespace and NaN-payload
    // extensions from widening the accepted action grammar.
    const locale_t numericLocale = cNumericLocale();
    if (numericLocale == nullptr) return std::nullopt;
    char *end = nullptr;
    const float result = strtof_l(normalized.c_str(), &end, numericLocale);
    if (end != normalized.data() + normalized.size()) {
        return std::nullopt;
    }
    return result;
}

std::optional<float> parseFiniteFloat32(QStringView value)
{
    const std::optional<float> result = parseFloat32(value);
    if (!result.has_value() || !std::isfinite(*result)) {
        return std::nullopt;
    }
    return result;
}

bool fitsSignedPointer(float value)
{
    if (!std::isfinite(value)) return false;
    value = std::trunc(value);

    // Converting intptr max to f32 rounds up to the next power of two on the
    // supported 32/64-bit targets. Step down once to obtain the largest f32
    // value whose integer conversion is representable.
    const float minimum =
        static_cast<float>(std::numeric_limits<qintptr>::min());
    const float maximum = std::nextafter(
        static_cast<float>(std::numeric_limits<qintptr>::max()), 0.0F);
    return value >= minimum && value <= maximum;
}

GhosttyActionTranslation reject(Error error, QStringView actionName,
                                OptionalView parameter)
{
    GhosttyActionTranslation result;
    result.error = error;
    result.actionName = actionName.toString();
    if (parameter.has_value()) {
        result.parameter = parameter->toString();
    }
    return result;
}

GhosttyActionTranslation accept(WorkspaceAction action,
                                WorkspaceActionContext context,
                                QStringView actionName, OptionalView parameter,
                                QString payload = {})
{
    GhosttyActionTranslation result;
    result.request =
        WorkspaceActionRequest{action, context, std::move(payload)};
    result.error = Error::None;
    result.actionName = actionName.toString();
    if (parameter.has_value()) {
        result.parameter = parameter->toString();
    }
    return result;
}

std::optional<QString> decodeActionUtf8String(QStringView value)
{
    const std::optional<QByteArray> decoded =
        decodeGhosttyActionString(value.toUtf8());
    if (!decoded.has_value()) return std::nullopt;

    QStringDecoder utf8(QStringDecoder::Utf8,
                        QStringDecoder::Flag::Stateless
                            | QStringDecoder::Flag::ConvertInitialBom);
    QString result = utf8(*decoded);
    if (utf8.hasError()) return std::nullopt;
    return result;
}

struct WorkspaceVoidActionSpec {
    QLatin1StringView name;
    WorkspaceAction action;
    std::optional<qint64> contextValue;
};

constexpr std::array<WorkspaceVoidActionSpec, 14> kWorkspaceVoidActions{{
    {QLatin1StringView("new_tab"), WorkspaceAction::NewTab, std::nullopt},
    {QLatin1StringView("close_surface"), WorkspaceAction::ClosePane,
     std::nullopt},
    {QLatin1StringView("previous_tab"), WorkspaceAction::ChangeTabRelative, -1},
    {QLatin1StringView("next_tab"), WorkspaceAction::ChangeTabRelative, 1},
    {QLatin1StringView("last_tab"), WorkspaceAction::ActivateLastTab,
     std::nullopt},
    {QLatin1StringView("move_tab_to_new_window"),
     WorkspaceAction::MoveTabToNewWindow, std::nullopt},
    {QLatin1StringView("toggle_split_zoom"), WorkspaceAction::ToggleSplitZoom,
     std::nullopt},
    {QLatin1StringView("toggle_fullscreen"), WorkspaceAction::ToggleFullscreen,
     std::nullopt},
    {QLatin1StringView("toggle_maximize"), WorkspaceAction::ToggleMaximize,
     std::nullopt},
    {QLatin1StringView("toggle_window_decorations"),
     WorkspaceAction::ToggleWindowDecorations, std::nullopt},
    {QLatin1StringView("equalize_splits"), WorkspaceAction::EqualizeSplits,
     std::nullopt},
    {QLatin1StringView("prompt_surface_title"),
     WorkspaceAction::PromptSurfaceTitle, std::nullopt},
    {QLatin1StringView("prompt_tab_title"), WorkspaceAction::PromptTabTitle,
     std::nullopt},
    {QLatin1StringView("prompt_window_title"),
     WorkspaceAction::PromptWindowTitle, std::nullopt},
}};

constexpr std::array<QLatin1StringView, 9> kParameterizedWorkspaceActions{{
    QLatin1StringView("close_tab"),
    QLatin1StringView("new_split"),
    QLatin1StringView("goto_split"),
    QLatin1StringView("goto_tab"),
    QLatin1StringView("move_tab"),
    QLatin1StringView("set_surface_title"),
    QLatin1StringView("set_tab_title"),
    QLatin1StringView("set_window_title"),
    QLatin1StringView("resize_split"),
}};

struct ApplicationActionSpec {
    QLatin1StringView name;
    std::optional<ApplicationAction> parsedAction;
    bool executable = false;
};

// Binding.Action.scope() classifies all of these as application actions even
// when the current frontend cannot execute them. `unbind` is a
// configuration-finalization directive and therefore deliberately has no
// runtime ApplicationAction. Parsing and execution remain separate so
// platform-specific actions are admitted only when their frontend route is
// complete.
constexpr std::array<ApplicationActionSpec, 13> kApplicationActions{{
    {QLatin1StringView("ignore"), ApplicationAction::Ignore, true},
    {QLatin1StringView("unbind"), std::nullopt},
    {QLatin1StringView("open_config"), ApplicationAction::OpenConfig, true},
    {QLatin1StringView("reload_config"), ApplicationAction::ReloadConfig, true},
    {QLatin1StringView("close_all_windows"),
     ApplicationAction::DeprecatedCloseAllWindows, true},
    {QLatin1StringView("quit"), ApplicationAction::Quit, true},
    {QLatin1StringView("toggle_quick_terminal"),
     ApplicationAction::ToggleQuickTerminal, true},
    {QLatin1StringView("toggle_visibility"), std::nullopt},
    {QLatin1StringView("check_for_updates"), std::nullopt},
    {QLatin1StringView("show_gtk_inspector"), std::nullopt},
    {QLatin1StringView("new_window"), ApplicationAction::NewWindow, true},
    {QLatin1StringView("undo"), std::nullopt},
    {QLatin1StringView("redo"), std::nullopt},
}};

template <typename Spec, std::size_t Size>
const Spec *findActionSpec(QStringView name,
                           const std::array<Spec, Size> &specs)
{
    const auto match = std::ranges::find_if(
        specs, [name](const Spec &spec) { return name == spec.name; });
    return match == specs.end() ? nullptr : std::addressof(*match);
}

template <std::size_t Size>
bool containsActionName(QStringView name,
                        const std::array<QLatin1StringView, Size> &names)
{
    return std::ranges::any_of(names, [name](QLatin1StringView candidate) {
        return name == candidate;
    });
}

GhosttyActionScope scopeForActionName(QStringView name)
{
    return findActionSpec(name, kApplicationActions) != nullptr
        ? GhosttyActionScope::Application
        : GhosttyActionScope::Surface;
}

} // namespace

GhosttySerializedActionView
GhosttyActionCatalog::parseSerializedAction(QStringView serializedAction)
{
    const qsizetype colon = serializedAction.indexOf(u':');
    GhosttySerializedActionView result;
    result.name = colon < 0 ? serializedAction : serializedAction.first(colon);
    if (colon >= 0) {
        result.parameter = serializedAction.sliced(colon + 1);
    }
    return result;
}

namespace {

GhosttyActionTranslation
translateWorkspaceAction(GhosttySerializedActionView parsed,
                         WorkspaceActionContext context)
{
    const QStringView actionName = parsed.name;
    const OptionalView parameter = parsed.parameter;

    // Binding.Action.parse reports an empty action name as InvalidFormat.
    if (actionName.isEmpty()) {
        return reject(Error::InvalidFormat, actionName, parameter);
    }

    const WorkspaceVoidActionSpec *const voidAction =
        findActionSpec(actionName, kWorkspaceVoidActions);
    if (voidAction == nullptr
        && !containsActionName(actionName, kParameterizedWorkspaceActions)) {
        return reject(Error::UnsupportedAction, actionName, parameter);
    }

    // Ghostty's void actions reject the presence of a colon, including a
    // trailing colon with an empty value.
    if (voidAction != nullptr) {
        if (parameter.has_value()) {
            return reject(Error::InvalidFormat, actionName, parameter);
        }
        if (voidAction->contextValue.has_value()) {
            context.value = *voidAction->contextValue;
        }
        return accept(voidAction->action, context, actionName, parameter);
    }
    if (equals(actionName, QLatin1StringView("goto_tab"))) {
        if (!parameter.has_value()) {
            return reject(Error::InvalidFormat, actionName, parameter);
        }
        const std::optional<quint64> index = parseUnsignedInteger(*parameter);
        if (!index.has_value()) {
            return reject(Error::InvalidFormat, actionName, parameter);
        }
        // Preserve successful usize parsing across qint64 storage. The
        // execution layer rejects values above Ghostty's c_int boundary, so
        // qint64::max is an unambiguous rejection sentinel for the upper half
        // of Linux's usize range.
        context.value =
            *index > static_cast<quint64>(std::numeric_limits<qint64>::max())
            ? std::numeric_limits<qint64>::max()
            : static_cast<qint64>(*index);
        return accept(WorkspaceAction::ActivateTabByIndex, context, actionName,
                      parameter);
    }

    if (equals(actionName, QLatin1StringView("move_tab"))) {
        if (!parameter.has_value()) {
            return reject(Error::InvalidFormat, actionName, parameter);
        }
        const std::optional<qint64> offset = parseSignedInteger(*parameter);
        if (!offset.has_value()) {
            return reject(Error::InvalidFormat, actionName, parameter);
        }
        context.value = *offset;
        return accept(WorkspaceAction::MoveTab, context, actionName, parameter);
    }

    const bool setsSurfaceTitle =
        equals(actionName, QLatin1StringView("set_surface_title"));
    const bool setsTabTitle =
        equals(actionName, QLatin1StringView("set_tab_title"));
    if (setsSurfaceTitle || setsTabTitle
        || equals(actionName, QLatin1StringView("set_window_title"))) {
        // The pinned structured helper serializes []const u8 action values
        // with std.zig.stringEscape. Invert that byte boundary and reject
        // text Qt cannot represent losslessly. An empty decoded title remains
        // an explicit value for a surface and clears a tab override.
        if (!parameter.has_value()) {
            return reject(Error::InvalidFormat, actionName, parameter);
        }
        std::optional<QString> title = decodeActionUtf8String(*parameter);
        if (!title.has_value()) {
            return reject(Error::InvalidFormat, actionName, parameter);
        }
        return accept(setsSurfaceTitle   ? WorkspaceAction::SetSurfaceTitle
                          : setsTabTitle ? WorkspaceAction::SetTabTitle
                                         : WorkspaceAction::SetWindowTitle,
                      context, actionName, parameter, std::move(*title));
    }

    if (equals(actionName, QLatin1StringView("close_tab"))) {
        // CloseTabMode.default is .this in Binding.zig.
        if (!parameter.has_value()
            || equals(*parameter, QLatin1StringView("this"))) {
            context.closeTabMode = CloseTabMode::This;
        } else if (equals(*parameter, QLatin1StringView("other"))) {
            context.closeTabMode = CloseTabMode::Other;
        } else if (equals(*parameter, QLatin1StringView("right"))) {
            context.closeTabMode = CloseTabMode::Right;
        } else {
            return reject(Error::InvalidFormat, actionName, parameter);
        }
        return accept(WorkspaceAction::CloseTab, context, actionName,
                      parameter);
    }

    if (equals(actionName, QLatin1StringView("new_split"))) {
        if (!parameter.has_value()) {
            return accept(WorkspaceAction::SplitAuto, context, actionName,
                          parameter);
        }
        if (equals(*parameter, QLatin1StringView("left"))) {
            return accept(WorkspaceAction::SplitLeft, context, actionName,
                          parameter);
        }
        if (equals(*parameter, QLatin1StringView("right"))) {
            return accept(WorkspaceAction::SplitRight, context, actionName,
                          parameter);
        }
        if (equals(*parameter, QLatin1StringView("down"))) {
            return accept(WorkspaceAction::SplitDown, context, actionName,
                          parameter);
        }
        if (equals(*parameter, QLatin1StringView("up"))) {
            return accept(WorkspaceAction::SplitUp, context, actionName,
                          parameter);
        }
        if (equals(*parameter, QLatin1StringView("auto"))) {
            return accept(WorkspaceAction::SplitAuto, context, actionName,
                          parameter);
        }
        return reject(Error::InvalidFormat, actionName, parameter);
    }

    if (equals(actionName, QLatin1StringView("resize_split"))) {
        if (!parameter.has_value()) {
            return reject(Error::InvalidFormat, actionName, parameter);
        }
        const qsizetype comma = parameter->indexOf(u',');
        if (comma < 0 || parameter->indexOf(u',', comma + 1) >= 0) {
            return reject(Error::InvalidFormat, actionName, parameter);
        }

        const QStringView direction = parameter->first(comma);
        if (equals(direction, QLatin1StringView("left"))) {
            context.value = Qt::Key_Left;
        } else if (equals(direction, QLatin1StringView("right"))) {
            context.value = Qt::Key_Right;
        } else if (equals(direction, QLatin1StringView("up"))) {
            context.value = Qt::Key_Up;
        } else if (equals(direction, QLatin1StringView("down"))) {
            context.value = Qt::Key_Down;
        } else {
            return reject(Error::InvalidFormat, actionName, parameter);
        }

        const std::optional<quint64> amount =
            parseUnsignedInteger(parameter->sliced(comma + 1));
        if (!amount.has_value()
            || *amount > std::numeric_limits<quint16>::max()) {
            return reject(Error::InvalidFormat, actionName, parameter);
        }
        context.amount = static_cast<int>(*amount);
        return accept(WorkspaceAction::ResizeSplit, context, actionName,
                      parameter);
    }

    // goto_split has no default and uses a custom parser in Binding.zig.
    // Ghostty's backwards-compatible top/bottom spellings remain accepted.
    if (!parameter.has_value()) {
        return reject(Error::InvalidFormat, actionName, parameter);
    }

    if (equals(*parameter, QLatin1StringView("left"))) {
        context.value = Qt::Key_Left;
    } else if (equals(*parameter, QLatin1StringView("right"))) {
        context.value = Qt::Key_Right;
    } else if (equals(*parameter, QLatin1StringView("up"))
               || equals(*parameter, QLatin1StringView("top"))) {
        context.value = Qt::Key_Up;
    } else if (equals(*parameter, QLatin1StringView("down"))
               || equals(*parameter, QLatin1StringView("bottom"))) {
        context.value = Qt::Key_Down;
    } else if (equals(*parameter, QLatin1StringView("previous"))) {
        context.value = -1;
        return accept(WorkspaceAction::NavigatePaneRelative, context,
                      actionName, parameter);
    } else if (equals(*parameter, QLatin1StringView("next"))) {
        context.value = 1;
        return accept(WorkspaceAction::NavigatePaneRelative, context,
                      actionName, parameter);
    } else {
        return reject(Error::InvalidFormat, actionName, parameter);
    }

    return accept(WorkspaceAction::NavigatePane, context, actionName,
                  parameter);
}

GhosttyDirectSurfaceActionParseResult
parseDirectSurfaceActionView(GhosttySerializedActionView parsed)
{
    if (parsed.name.isEmpty()) {
        return std::unexpected(Error::InvalidFormat);
    }

    std::optional<TerminalFileLocation> fileLocation;
    if (parsed.name == QLatin1StringView("write_screen_file")) {
        fileLocation = TerminalFileLocation::Screen;
    } else if (parsed.name == QLatin1StringView("write_scrollback_file")) {
        fileLocation = TerminalFileLocation::Scrollback;
    } else if (parsed.name == QLatin1StringView("write_selection_file")) {
        fileLocation = TerminalFileLocation::Selection;
    }
    if (fileLocation.has_value()) {
        if (!parsed.parameter.has_value() || parsed.parameter->isEmpty()) {
            return std::unexpected(Error::InvalidFormat);
        }

        const qsizetype comma = parsed.parameter->indexOf(u',');
        const QStringView dispositionName =
            comma < 0 ? *parsed.parameter : parsed.parameter->first(comma);
        TerminalFileDisposition disposition;
        if (dispositionName == QLatin1StringView("copy")) {
            disposition = TerminalFileDisposition::Copy;
        } else if (dispositionName == QLatin1StringView("paste")) {
            disposition = TerminalFileDisposition::Paste;
        } else if (dispositionName == QLatin1StringView("open")) {
            disposition = TerminalFileDisposition::Open;
        } else {
            return std::unexpected(Error::InvalidFormat);
        }

        if (comma >= 0) {
            const QStringView formatName = parsed.parameter->sliced(comma + 1);
            if (formatName == QLatin1StringView("vt")
                || formatName == QLatin1StringView("html")) {
                return std::unexpected(Error::UnsupportedParameter);
            }
            if (formatName != QLatin1StringView("plain")) {
                return std::unexpected(Error::InvalidFormat);
            }
        }
        return GhosttyPaneAction{TerminalWriteFileAction{
            .location = *fileLocation,
            .disposition = disposition,
            .format = TerminalFileFormat::Plain,
        }};
    }

    if (parsed.name == QLatin1StringView("copy_to_clipboard")) {
        if (!parsed.parameter.has_value()
            || *parsed.parameter == QLatin1StringView("mixed")) {
            return GhosttyPaneAction{PaneAction::CopyToClipboard{
                .format = PaneAction::CopyFormat::Mixed,
            }};
        }
        if (*parsed.parameter == QLatin1StringView("plain")) {
            return GhosttyPaneAction{PaneAction::CopyToClipboard{
                .format = PaneAction::CopyFormat::Plain,
            }};
        }
        if (*parsed.parameter == QLatin1StringView("vt")
            || *parsed.parameter == QLatin1StringView("html")) {
            return std::unexpected(Error::UnsupportedParameter);
        }
        return std::unexpected(Error::InvalidFormat);
    }

    std::optional<GhosttyPaneAction> action;
    if (parsed.name == QLatin1StringView("paste_from_clipboard")) {
        action = PaneAction::Paste{
            .source = PaneAction::PasteSource::Clipboard,
        };
    } else if (parsed.name == QLatin1StringView("paste_from_selection")) {
        action = PaneAction::Paste{
            .source = PaneAction::PasteSource::Selection,
        };
    } else if (parsed.name == QLatin1StringView("copy_url_to_clipboard")) {
        action = PaneAction::CopyUrlToClipboard{};
    } else if (parsed.name == QLatin1StringView("copy_title_to_clipboard")) {
        action = PaneAction::CopyTitleToClipboard{};
    } else if (parsed.name == QLatin1StringView("end_key_sequence")) {
        action = PaneAction::EndKeySequence{};
    } else if (parsed.name == QLatin1StringView("close_window")) {
        action = PaneAction::CloseWindow{};
    } else {
        return std::unexpected(Error::UnsupportedAction);
    }
    if (parsed.parameter.has_value()) {
        return std::unexpected(Error::InvalidFormat);
    }
    return std::move(*action);
}

std::optional<WorkspaceFrontendActionRequest>
parseFrontendActionView(GhosttySerializedActionView parsed,
                        WorkspaceActionContext context)
{
    const auto request = [&context](WorkspaceFrontendAction action) {
        return WorkspaceFrontendActionRequest{
            .action = std::move(action),
            .context = context,
        };
    };

    if (parsed.name == QLatin1StringView("toggle_command_palette")
        || parsed.name == QLatin1StringView("toggle_tab_overview")
        || parsed.name == QLatin1StringView("show_on_screen_keyboard")) {
        // These are void Binding.Action fields: the presence of any colon is
        // invalid, including an explicitly empty parameter.
        if (parsed.parameter.has_value()) return std::nullopt;
        if (parsed.name == QLatin1StringView("toggle_command_palette")) {
            return request(FrontendAction::ToggleCommandPalette{});
        }
        if (parsed.name == QLatin1StringView("toggle_tab_overview")) {
            return request(FrontendAction::ToggleTabOverview{});
        }
        return request(FrontendAction::ShowOnScreenKeyboard{});
    }

    if (parsed.name == QLatin1StringView("inspector")) {
        if (!parsed.parameter.has_value()) return std::nullopt;

        FrontendAction::InspectorMode mode;
        if (*parsed.parameter == QLatin1StringView("toggle")) {
            mode = FrontendAction::InspectorMode::Toggle;
        } else if (*parsed.parameter == QLatin1StringView("show")) {
            mode = FrontendAction::InspectorMode::Show;
        } else if (*parsed.parameter == QLatin1StringView("hide")) {
            mode = FrontendAction::InspectorMode::Hide;
        } else {
            return std::nullopt;
        }
        return request(FrontendAction::Inspector{.mode = mode});
    }

    if (parsed.name == QLatin1StringView("crash")) {
        if (!parsed.parameter.has_value()) return std::nullopt;

        FrontendAction::CrashTarget target;
        if (*parsed.parameter == QLatin1StringView("main")) {
            target = FrontendAction::CrashTarget::Main;
        } else if (*parsed.parameter == QLatin1StringView("io")) {
            target = FrontendAction::CrashTarget::Io;
        } else if (*parsed.parameter == QLatin1StringView("render")) {
            target = FrontendAction::CrashTarget::Render;
        } else {
            return std::nullopt;
        }
        return request(FrontendAction::Crash{.target = target});
    }

    return std::nullopt;
}

bool isExecutableFrontendAction(
    const WorkspaceFrontendActionRequest &request) noexcept
{
    return std::holds_alternative<FrontendAction::ToggleCommandPalette>(
               request.action)
        || std::holds_alternative<FrontendAction::ToggleTabOverview>(
               request.action)
        || std::holds_alternative<FrontendAction::ShowOnScreenKeyboard>(
               request.action)
        || std::holds_alternative<FrontendAction::Inspector>(request.action)
        || std::holds_alternative<FrontendAction::Crash>(request.action);
}

std::optional<ApplicationAction>
parseApplicationActionView(GhosttySerializedActionView parsed)
{
    if (parsed.name == QLatin1StringView("open_config")) {
        if (!parsed.parameter.has_value()
            || *parsed.parameter == QLatin1StringView("os_open")) {
            return ApplicationAction::OpenConfig;
        }
        if (*parsed.parameter == QLatin1StringView("new_window")) {
            return ApplicationAction::OpenConfigNewWindow;
        }
        return std::nullopt;
    }
    if (parsed.parameter.has_value()) return std::nullopt;

    const ApplicationActionSpec *const spec =
        findActionSpec(parsed.name, kApplicationActions);
    return spec != nullptr ? spec->parsedAction : std::nullopt;
}

std::optional<WindowNavigationAction>
parseWindowNavigationActionView(GhosttySerializedActionView parsed)
{
    if (parsed.name != QLatin1StringView("goto_window")
        || !parsed.parameter.has_value()) {
        return std::nullopt;
    }
    if (*parsed.parameter == QLatin1StringView("previous")) {
        return WindowNavigationAction::Previous;
    }
    if (*parsed.parameter == QLatin1StringView("next")) {
        return WindowNavigationAction::Next;
    }
    return std::nullopt;
}

std::optional<GhosttyPaneAction>
parsePaneActionView(GhosttySerializedActionView parsed)
{
    const QStringView name = parsed.name;
    const OptionalView parameter = parsed.parameter;

    if (GhosttyDirectSurfaceActionParseResult direct =
            parseDirectSurfaceActionView(parsed);
        direct.has_value()) {
        return std::move(*direct);
    }

    if (name == QLatin1StringView("scroll_to_top")
        || name == QLatin1StringView("scroll_to_bottom")
        || name == QLatin1StringView("scroll_to_selection")) {
        if (parameter.has_value()) return std::nullopt;
        if (name == QLatin1StringView("scroll_to_top")) {
            return GhosttyPaneAction{PaneAction::ScrollToTop{}};
        }
        if (name == QLatin1StringView("scroll_to_bottom")) {
            return GhosttyPaneAction{PaneAction::ScrollToBottom{}};
        }
        return GhosttyPaneAction{PaneAction::ScrollToSelection{}};
    }

    if (name == QLatin1StringView("scroll_to_row")) {
        if (!parameter.has_value()) return std::nullopt;
        const std::optional<quint64> row = parseUnsignedInteger(*parameter);
        if (!row.has_value()) return std::nullopt;
        return GhosttyPaneAction{PaneAction::ScrollToRow{.row = *row}};
    }

    if (name == QLatin1StringView("scroll_page_up")
        || name == QLatin1StringView("scroll_page_down")) {
        if (parameter.has_value()) return std::nullopt;
        return name == QLatin1StringView("scroll_page_up")
            ? GhosttyPaneAction{PaneAction::ScrollPageUp{}}
            : GhosttyPaneAction{PaneAction::ScrollPageDown{}};
    }

    if (name == QLatin1StringView("scroll_page_fractional")) {
        if (!parameter.has_value()) return std::nullopt;
        const std::optional<float> fraction = parseFiniteFloat32(*parameter);
        // The terminal always has at least one row. A fraction that cannot be
        // converted safely even at that minimum can never be executable.
        if (!fraction.has_value() || !fitsSignedPointer(*fraction)) {
            return std::nullopt;
        }
        return GhosttyPaneAction{
            PaneAction::ScrollPageFractional{.fraction = *fraction}};
    }

    if (name == QLatin1StringView("scroll_page_lines")) {
        if (!parameter.has_value()) return std::nullopt;
        const std::optional<qint64> lines = parseSignedInteger(*parameter);
        if (!lines.has_value() || *lines < std::numeric_limits<qint16>::min()
            || *lines > std::numeric_limits<qint16>::max()) {
            return std::nullopt;
        }
        return GhosttyPaneAction{PaneAction::ScrollPageLines{
            .lines = static_cast<qint16>(*lines),
        }};
    }

    if (name == QLatin1StringView("increase_font_size")
        || name == QLatin1StringView("decrease_font_size")
        || name == QLatin1StringView("set_font_size")) {
        // All three fields are f32 in Binding.Action. The parameter is
        // required, and parseFloat intentionally accepts non-finite values;
        // Surface.performBindingAction gives those defined clamp semantics.
        if (!parameter.has_value()) return std::nullopt;
        const std::optional<float> points = parseFloat32(*parameter);
        if (!points.has_value()) return std::nullopt;

        if (name == QLatin1StringView("increase_font_size")) {
            return GhosttyPaneAction{
                PaneAction::IncreaseFontSize{.points = *points}};
        }
        if (name == QLatin1StringView("decrease_font_size")) {
            return GhosttyPaneAction{
                PaneAction::DecreaseFontSize{.points = *points}};
        }
        return GhosttyPaneAction{PaneAction::SetFontSize{.points = *points}};
    }

    if (name == QLatin1StringView("reset_font_size")) {
        if (parameter.has_value()) return std::nullopt;
        return GhosttyPaneAction{PaneAction::ResetFontSize{}};
    }

    if (name == QLatin1StringView("activate_key_table")
        || name == QLatin1StringView("activate_key_table_once")) {
        // These fields are []const u8 in Binding.Action. The colon is
        // required, but an empty or colon-containing table name still parses.
        // The structured config boundary uses Action.format, so invert its
        // canonical byte escapes before comparing against decoded table names.
        if (!parameter.has_value()) return std::nullopt;
        const std::optional<QString> tableName =
            decodeActionUtf8String(*parameter);
        if (!tableName.has_value()) return std::nullopt;
        if (name == QLatin1StringView("activate_key_table")) {
            return GhosttyPaneAction{PaneAction::ActivateKeyTable{
                .name = *tableName,
            }};
        }
        return GhosttyPaneAction{PaneAction::ActivateKeyTableOnce{
            .name = *tableName,
        }};
    }

    if (name == QLatin1StringView("deactivate_key_table")
        || name == QLatin1StringView("deactivate_all_key_tables")) {
        // Both deactivation fields are void, so even an empty parameter is
        // invalid rather than an alternate spelling of the action.
        if (parameter.has_value()) return std::nullopt;
        return name == QLatin1StringView("deactivate_key_table")
            ? GhosttyPaneAction{PaneAction::DeactivateKeyTable{}}
            : GhosttyPaneAction{PaneAction::DeactivateAllKeyTables{}};
    }

    if (name == QLatin1StringView("select_all")) {
        if (parameter.has_value()) return std::nullopt;
        return GhosttyPaneAction{PaneAction::SelectAll{}};
    }

    if (name == QLatin1StringView("start_search")
        || name == QLatin1StringView("end_search")
        || name == QLatin1StringView("search_selection")) {
        // These are void Binding.Action fields, so even an explicitly empty
        // parameter is invalid.
        if (parameter.has_value()) return std::nullopt;
        if (name == QLatin1StringView("start_search")) {
            return GhosttyPaneAction{PaneAction::StartSearch{}};
        }
        return name == QLatin1StringView("end_search")
            ? GhosttyPaneAction{PaneAction::EndSearch{}}
            : GhosttyPaneAction{PaneAction::SearchSelection{}};
    }

    if (name == QLatin1StringView("search")) {
        // Search is a []const u8 field. Its colon is required, its payload may
        // be empty, and only the first colon separates the action name.
        if (!parameter.has_value()) return std::nullopt;
        return GhosttyPaneAction{PaneAction::Search{
            .serializedNeedle = parameter->toUtf8(),
        }};
    }

    if (name == QLatin1StringView("navigate_search")) {
        if (!parameter.has_value()) return std::nullopt;
        TerminalSearchDirection direction;
        if (*parameter == QLatin1StringView("previous")) {
            direction = TerminalSearchDirection::Previous;
        } else if (*parameter == QLatin1StringView("next")) {
            direction = TerminalSearchDirection::Next;
        } else {
            return std::nullopt;
        }
        return GhosttyPaneAction{
            PaneAction::NavigateSearch{.direction = direction}};
    }

    if (name == QLatin1StringView("csi") || name == QLatin1StringView("esc")
        || name == QLatin1StringView("text")) {
        // All three fields are []const u8 in Binding.Action, so the colon is
        // required but the byte string after it may be empty. Split only at
        // the first colon; later colons are part of the payload.
        if (!parameter.has_value()) return std::nullopt;
        if (name == QLatin1StringView("csi")) {
            return GhosttyPaneAction{PaneAction::SendCsi{
                .serializedBytes = parameter->toUtf8(),
            }};
        }
        return name == QLatin1StringView("esc")
            ? GhosttyPaneAction{PaneAction::SendEscape{
                  .serializedBytes = parameter->toUtf8(),
              }}
            : GhosttyPaneAction{PaneAction::SendText{
                  .serializedBytes = parameter->toUtf8(),
              }};
    }

    if (name == QLatin1StringView("reset")) {
        // Reset is a void Binding.Action field, therefore even `reset:` is
        // invalid rather than an empty-parameter spelling of reset.
        if (parameter.has_value()) return std::nullopt;
        return GhosttyPaneAction{PaneAction::ResetTerminal{}};
    }

    if (name == QLatin1StringView("toggle_readonly")) {
        // Read-only is a per-surface void action. An explicit colon, even
        // with an empty value, is not an alternate spelling.
        if (parameter.has_value()) return std::nullopt;
        return GhosttyPaneAction{PaneAction::ToggleReadOnly{}};
    }

    if (name == QLatin1StringView("toggle_mouse_reporting")) {
        // Like the configuration it mutates, this is a per-surface void
        // action. It never changes the terminal's requested DEC mouse mode.
        if (parameter.has_value()) return std::nullopt;
        return GhosttyPaneAction{PaneAction::ToggleMouseReporting{}};
    }

    if (name != QLatin1StringView("adjust_selection")) {
        return std::nullopt;
    }
    if (!parameter.has_value()) return std::nullopt;

    TerminalSelectionAdjustment adjustment;
    if (*parameter == QLatin1StringView("left")) {
        adjustment = TerminalSelectionAdjustment::Left;
    } else if (*parameter == QLatin1StringView("right")) {
        adjustment = TerminalSelectionAdjustment::Right;
    } else if (*parameter == QLatin1StringView("up")) {
        adjustment = TerminalSelectionAdjustment::Up;
    } else if (*parameter == QLatin1StringView("down")) {
        adjustment = TerminalSelectionAdjustment::Down;
    } else if (*parameter == QLatin1StringView("page_up")) {
        adjustment = TerminalSelectionAdjustment::PageUp;
    } else if (*parameter == QLatin1StringView("page_down")) {
        adjustment = TerminalSelectionAdjustment::PageDown;
    } else if (*parameter == QLatin1StringView("home")) {
        adjustment = TerminalSelectionAdjustment::Home;
    } else if (*parameter == QLatin1StringView("end")) {
        adjustment = TerminalSelectionAdjustment::End;
    } else if (*parameter == QLatin1StringView("beginning_of_line")) {
        adjustment = TerminalSelectionAdjustment::BeginningOfLine;
    } else if (*parameter == QLatin1StringView("end_of_line")) {
        adjustment = TerminalSelectionAdjustment::EndOfLine;
    } else {
        return std::nullopt;
    }
    return GhosttyPaneAction{
        PaneAction::AdjustSelection{.adjustment = adjustment}};
}

std::optional<GhosttyConfiguredAction>
parseConfiguredActionView(GhosttySerializedActionView parsed,
                          WorkspaceActionContext context)
{
    // Application names retain their upstream scope even when this frontend
    // does not implement them. Do not reinterpret an unsupported application
    // action through a surface parser.
    if (const ApplicationActionSpec *const spec =
            findActionSpec(parsed.name, kApplicationActions)) {
        if (spec->executable) {
            if (auto action = parseApplicationActionView(parsed);
                action.has_value()) {
                return GhosttyConfiguredAction{*action};
            }
        }
        return std::nullopt;
    }

    if (std::optional<WorkspaceFrontendActionRequest> action =
            parseFrontendActionView(parsed, context);
        action.has_value() && isExecutableFrontendAction(*action)) {
        return GhosttyConfiguredAction{std::move(*action)};
    }

    if (std::optional<WindowNavigationAction> action =
            parseWindowNavigationActionView(parsed)) {
        return GhosttyConfiguredAction{*action};
    }

    if (std::optional<GhosttyPaneAction> action = parsePaneActionView(parsed)) {
        return GhosttyConfiguredAction{std::move(*action)};
    }

    GhosttyActionTranslation translated =
        translateWorkspaceAction(parsed, context);
    if (translated.accepted()) {
        return GhosttyConfiguredAction{std::move(*translated.request)};
    }
    return std::nullopt;
}

} // namespace

GhosttyActionTranslation
GhosttyActionCatalog::translate(QStringView serializedAction,
                                WorkspaceActionContext context)
{
    return translateWorkspaceAction(parseSerializedAction(serializedAction),
                                    context);
}

std::optional<GhosttyConfiguredAction>
GhosttyActionCatalog::parseConfiguredAction(QStringView serializedAction,
                                            WorkspaceActionContext context)
{
    return parseConfiguredActionView(parseSerializedAction(serializedAction),
                                     context);
}

bool GhosttyActionCatalog::isImplemented(QStringView serializedAction)
{
    return parseConfiguredAction(serializedAction).has_value();
}

std::optional<GhosttyPaneAction>
GhosttyActionCatalog::parsePaneAction(QStringView serializedAction)
{
    return parsePaneActionView(parseSerializedAction(serializedAction));
}

GhosttyDirectSurfaceActionParseResult
GhosttyActionCatalog::parseDirectSurfaceAction(QStringView serializedAction)
{
    return parseDirectSurfaceActionView(
        parseSerializedAction(serializedAction));
}

std::optional<ApplicationAction>
GhosttyActionCatalog::parseApplicationAction(QStringView serializedAction)
{
    return parseApplicationActionView(parseSerializedAction(serializedAction));
}

std::optional<WorkspaceFrontendActionRequest>
GhosttyActionCatalog::parseFrontendAction(QStringView serializedAction,
                                          WorkspaceActionContext context)
{
    return parseFrontendActionView(parseSerializedAction(serializedAction),
                                   context);
}

std::optional<WindowNavigationAction>
GhosttyActionCatalog::parseWindowNavigationAction(QStringView serializedAction)
{
    return parseWindowNavigationActionView(
        parseSerializedAction(serializedAction));
}

GhosttyActionScope GhosttyActionCatalog::scope(QStringView serializedAction)
{
    return scopeForActionName(parseSerializedAction(serializedAction).name);
}

GhosttyActionInputEffect GhosttyActionCatalog::inputEffect(
    const GhosttyConfiguredAction &action) noexcept
{
    if (const auto *application = std::get_if<ApplicationAction>(&action)) {
        return *application == ApplicationAction::Ignore
            ? GhosttyActionInputEffect::Ignore
            : GhosttyActionInputEffect::None;
    }
    if (std::holds_alternative<WindowNavigationAction>(action)) {
        return GhosttyActionInputEffect::None;
    }
    if (std::holds_alternative<WorkspaceFrontendActionRequest>(action)) {
        return GhosttyActionInputEffect::None;
    }
    if (const auto *pane = std::get_if<GhosttyPaneAction>(&action)) {
        return std::holds_alternative<PaneAction::CloseWindow>(*pane)
            ? GhosttyActionInputEffect::ClosingAction
            : GhosttyActionInputEffect::None;
    }
    const auto &workspace = std::get<WorkspaceActionRequest>(action);
    return workspace.action == WorkspaceAction::ClosePane
            || workspace.action == WorkspaceAction::CloseTab
        ? GhosttyActionInputEffect::ClosingAction
        : GhosttyActionInputEffect::None;
}

QStringList GhosttyCompiledActionChain::serializedActions() const
{
    QStringList result;
    result.reserve(entries.size());
    for (const GhosttyCompiledAction &entry : entries) {
        result.append(entry.serialized);
    }
    return result;
}

GhosttyCompiledActionChain
GhosttyActionCatalog::compileActionChain(const QStringList &actions)
{
    GhosttyCompiledActionChain result;
    result.entries.reserve(actions.size());

    bool ignored = false;
    bool closing = false;
    for (const QString &serialized : actions) {
        const GhosttySerializedActionView parsed =
            parseSerializedAction(serialized);
        const GhosttyActionScope actionScope = scopeForActionName(parsed.name);
        std::optional<GhosttyConfiguredAction> action =
            parseConfiguredActionView(parsed, {});
        result.applicationOnly &=
            actionScope == GhosttyActionScope::Application;

        if (action.has_value()) {
            switch (inputEffect(*action)) {
            case GhosttyActionInputEffect::None: break;
            case GhosttyActionInputEffect::Ignore: ignored = true; break;
            case GhosttyActionInputEffect::ClosingAction: closing = true; break;
            }
        }
        result.entries.append(GhosttyCompiledAction{
            .serialized = serialized,
            .scope = actionScope,
            .action = std::move(action),
        });
    }

    result.inputEffect = closing ? GhosttyActionInputEffect::ClosingAction
        : ignored                ? GhosttyActionInputEffect::Ignore
                                 : GhosttyActionInputEffect::None;
    return result;
}

GhosttyActionInputEffect
GhosttyActionCatalog::combinedInputEffect(const QStringList &actions)
{
    return compileActionChain(actions).inputEffect;
}

bool GhosttyActionCatalog::shouldCoalesceBroadClose(
    const GhosttyConfiguredAction &action) noexcept
{
    if (const auto *pane = std::get_if<GhosttyPaneAction>(&action)) {
        return std::holds_alternative<PaneAction::CloseWindow>(*pane);
    }
    const auto *workspace = std::get_if<WorkspaceActionRequest>(&action);
    return workspace != nullptr
        && (workspace->action == WorkspaceAction::ClosePane
            || (workspace->action == WorkspaceAction::CloseTab
                && workspace->context.closeTabMode == CloseTabMode::This));
}
