#include "ghostty_action_catalog.h"
#include "zig_string_escape.h"

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
    if (value.isEmpty() || value.front() == u'_'
        || value.back() == u'_') {
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
    if (value.isEmpty() || value.front() == u'_'
        || value.back() == u'_') {
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
    if (unsignedValue.compare(QLatin1StringView("nan"),
                              Qt::CaseInsensitive) == 0) {
        // Zig discards the sign when parsing NaN.
        return std::numeric_limits<float>::quiet_NaN();
    }
    if (unsignedValue.compare(QLatin1StringView("inf"),
                              Qt::CaseInsensitive) == 0
        || unsignedValue.compare(QLatin1StringView("infinity"),
                                 Qt::CaseInsensitive) == 0) {
        const float infinity = std::numeric_limits<float>::infinity();
        return negative ? -infinity : infinity;
    }

    const bool hexadecimal = index + 1 < value.size()
        && value.at(index) == u'0'
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
    if (!consumeFloatDigits(value, &index, mantissaBase,
                            &normalized, &hasMantissaDigit)) {
        return std::nullopt;
    }
    if (index < value.size() && value.at(index) == u'.') {
        normalized.push_back('.');
        ++index;
        if (!consumeFloatDigits(value, &index, mantissaBase,
                                &normalized, &hasMantissaDigit)) {
            return std::nullopt;
        }
    }
    if (!hasMantissaDigit) return std::nullopt;

    const QChar exponentMarker = hexadecimal ? u'p' : u'e';
    if (index < value.size()
        && value.at(index).toLower() == exponentMarker) {
        normalized.push_back(static_cast<char>(value.at(index).unicode()));
        ++index;
        if (index < value.size()
            && (value.at(index) == u'+' || value.at(index) == u'-')) {
            normalized.push_back(static_cast<char>(value.at(index).unicode()));
            ++index;
        }
        bool hasExponentDigit = false;
        if (!consumeFloatDigits(value, &index, 10,
                                &normalized, &hasExponentDigit)
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

GhosttyActionTranslation reject(Error error,
                                 QStringView actionName,
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
                                 QStringView actionName,
                                 OptionalView parameter,
                                 QString payload = {})
{
    GhosttyActionTranslation result;
    result.request = WorkspaceActionRequest{
        action, context, std::move(payload)};
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

    QStringDecoder utf8(
        QStringDecoder::Utf8,
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

constexpr std::array<WorkspaceVoidActionSpec, 11> kWorkspaceVoidActions{{
    {QLatin1StringView("new_tab"), WorkspaceAction::NewTab, std::nullopt},
    {QLatin1StringView("close_surface"), WorkspaceAction::ClosePane,
     std::nullopt},
    {QLatin1StringView("previous_tab"), WorkspaceAction::ChangeTabRelative,
     -1},
    {QLatin1StringView("next_tab"), WorkspaceAction::ChangeTabRelative, 1},
    {QLatin1StringView("last_tab"), WorkspaceAction::ActivateLastTab,
     std::nullopt},
    {QLatin1StringView("toggle_split_zoom"), WorkspaceAction::ToggleSplitZoom,
     std::nullopt},
    {QLatin1StringView("toggle_fullscreen"), WorkspaceAction::ToggleFullscreen,
     std::nullopt},
    {QLatin1StringView("toggle_maximize"), WorkspaceAction::ToggleMaximize,
     std::nullopt},
    {QLatin1StringView("equalize_splits"), WorkspaceAction::EqualizeSplits,
     std::nullopt},
    {QLatin1StringView("prompt_surface_title"),
     WorkspaceAction::PromptSurfaceTitle, std::nullopt},
    {QLatin1StringView("prompt_tab_title"), WorkspaceAction::PromptTabTitle,
     std::nullopt},
}};

constexpr std::array<QLatin1StringView, 8> kParameterizedWorkspaceActions{{
    QLatin1StringView("close_tab"),
    QLatin1StringView("new_split"),
    QLatin1StringView("goto_split"),
    QLatin1StringView("goto_tab"),
    QLatin1StringView("move_tab"),
    QLatin1StringView("set_surface_title"),
    QLatin1StringView("set_tab_title"),
    QLatin1StringView("resize_split"),
}};

struct ApplicationActionSpec {
    QLatin1StringView name;
    std::optional<ApplicationAction> implementedAction;
};

// Binding.Action.scope() classifies all of these as application actions even
// when GTK does not implement them. `unbind` is a configuration-finalization
// directive and therefore deliberately has no runtime ApplicationAction.
constexpr std::array<ApplicationActionSpec, 13> kApplicationActions{{
    {QLatin1StringView("ignore"), ApplicationAction::Ignore},
    {QLatin1StringView("unbind"), std::nullopt},
    {QLatin1StringView("open_config"), ApplicationAction::OpenConfig},
    {QLatin1StringView("reload_config"), ApplicationAction::ReloadConfig},
    {QLatin1StringView("close_all_windows"), std::nullopt},
    {QLatin1StringView("quit"), ApplicationAction::Quit},
    {QLatin1StringView("toggle_quick_terminal"), std::nullopt},
    {QLatin1StringView("toggle_visibility"), std::nullopt},
    {QLatin1StringView("check_for_updates"), std::nullopt},
    {QLatin1StringView("show_gtk_inspector"), std::nullopt},
    {QLatin1StringView("new_window"), ApplicationAction::NewWindow},
    {QLatin1StringView("undo"), std::nullopt},
    {QLatin1StringView("redo"), std::nullopt},
}};

enum class DirectSurfaceParameter : quint8 {
    Void,
    CopyFormat,
};

struct DirectSurfaceActionSpec {
    QLatin1StringView name;
    GhosttyPaneActionKind kind;
    DirectSurfaceParameter parameter;
};

constexpr std::array<DirectSurfaceActionSpec, 7> kDirectSurfaceActions{{
    {QLatin1StringView("copy_to_clipboard"),
     GhosttyPaneActionKind::CopyToClipboardMixed,
     DirectSurfaceParameter::CopyFormat},
    {QLatin1StringView("paste_from_clipboard"),
     GhosttyPaneActionKind::PasteFromClipboard,
     DirectSurfaceParameter::Void},
    {QLatin1StringView("paste_from_selection"),
     GhosttyPaneActionKind::PasteFromSelection,
     DirectSurfaceParameter::Void},
    {QLatin1StringView("copy_url_to_clipboard"),
     GhosttyPaneActionKind::CopyUrlToClipboard,
     DirectSurfaceParameter::Void},
    {QLatin1StringView("copy_title_to_clipboard"),
     GhosttyPaneActionKind::CopyTitleToClipboard,
     DirectSurfaceParameter::Void},
    {QLatin1StringView("end_key_sequence"),
     GhosttyPaneActionKind::EndKeySequence,
     DirectSurfaceParameter::Void},
    {QLatin1StringView("close_window"),
     GhosttyPaneActionKind::CloseWindow,
     DirectSurfaceParameter::Void},
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
    return std::ranges::any_of(
        names, [name](QLatin1StringView candidate) {
            return name == candidate;
        });
}

} // namespace

GhosttySerializedActionView GhosttyActionCatalog::parseSerializedAction(
    QStringView serializedAction)
{
    const qsizetype colon = serializedAction.indexOf(u':');
    GhosttySerializedActionView result;
    result.name = colon < 0
        ? serializedAction
        : serializedAction.first(colon);
    if (colon >= 0) {
        result.parameter = serializedAction.sliced(colon + 1);
    }
    return result;
}

namespace {

GhosttyActionTranslation translateWorkspaceAction(
    GhosttySerializedActionView parsed,
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
        && !containsActionName(actionName,
                               kParameterizedWorkspaceActions)) {
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
        const std::optional<quint64> index =
            parseUnsignedInteger(*parameter);
        if (!index.has_value()) {
            return reject(Error::InvalidFormat, actionName, parameter);
        }
        // Preserve successful usize parsing across qint64 storage. The
        // execution layer rejects values above Ghostty's c_int boundary, so
        // qint64::max is an unambiguous rejection sentinel for the upper half
        // of Linux's usize range.
        context.value = *index > static_cast<quint64>(
                                      std::numeric_limits<qint64>::max())
            ? std::numeric_limits<qint64>::max()
            : static_cast<qint64>(*index);
        return accept(WorkspaceAction::ActivateTabByIndex,
                      context,
                      actionName,
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
        return accept(WorkspaceAction::MoveTab,
                      context,
                      actionName,
                      parameter);
    }

    const bool setsSurfaceTitle =
        equals(actionName, QLatin1StringView("set_surface_title"));
    if (setsSurfaceTitle
        || equals(actionName, QLatin1StringView("set_tab_title"))) {
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
        return accept(setsSurfaceTitle
                          ? WorkspaceAction::SetSurfaceTitle
                          : WorkspaceAction::SetTabTitle,
                      context,
                      actionName,
                      parameter,
                      std::move(*title));
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
        return accept(WorkspaceAction::CloseTab,
                      context,
                      actionName,
                      parameter);
    }

    if (equals(actionName, QLatin1StringView("new_split"))) {
        if (!parameter.has_value()) {
            return accept(WorkspaceAction::SplitAuto,
                          context,
                          actionName,
                          parameter);
        }
        if (equals(*parameter, QLatin1StringView("left"))) {
            return accept(WorkspaceAction::SplitLeft,
                          context,
                          actionName,
                          parameter);
        }
        if (equals(*parameter, QLatin1StringView("right"))) {
            return accept(WorkspaceAction::SplitRight,
                          context,
                          actionName,
                          parameter);
        }
        if (equals(*parameter, QLatin1StringView("down"))) {
            return accept(WorkspaceAction::SplitDown,
                          context,
                          actionName,
                          parameter);
        }
        if (equals(*parameter, QLatin1StringView("up"))) {
            return accept(WorkspaceAction::SplitUp,
                          context,
                          actionName,
                          parameter);
        }
        if (equals(*parameter, QLatin1StringView("auto"))) {
            return accept(WorkspaceAction::SplitAuto,
                          context,
                          actionName,
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
        return accept(WorkspaceAction::ResizeSplit,
                      context,
                      actionName,
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
        return accept(WorkspaceAction::NavigatePaneRelative,
                      context,
                      actionName,
                      parameter);
    } else if (equals(*parameter, QLatin1StringView("next"))) {
        context.value = 1;
        return accept(WorkspaceAction::NavigatePaneRelative,
                      context,
                      actionName,
                      parameter);
    } else {
        return reject(Error::InvalidFormat, actionName, parameter);
    }

    return accept(WorkspaceAction::NavigatePane,
                  context,
                  actionName,
                  parameter);
}

GhosttyDirectSurfaceActionParseResult parseDirectSurfaceActionView(
    GhosttySerializedActionView parsed)
{
    if (parsed.name.isEmpty()) {
        return std::unexpected(Error::InvalidFormat);
    }

    const DirectSurfaceActionSpec *const spec =
        findActionSpec(parsed.name, kDirectSurfaceActions);
    if (spec == nullptr) {
        return std::unexpected(Error::UnsupportedAction);
    }

    switch (spec->parameter) {
    case DirectSurfaceParameter::Void: {
        if (parsed.parameter.has_value()) {
            return std::unexpected(Error::InvalidFormat);
        }
        GhosttyPaneAction action;
        action.kind = spec->kind;
        return action;
    }
    case DirectSurfaceParameter::CopyFormat: {
        GhosttyPaneAction action;
        if (!parsed.parameter.has_value()
            || *parsed.parameter == QLatin1StringView("mixed")) {
            action.kind = GhosttyPaneActionKind::CopyToClipboardMixed;
            return action;
        }
        if (*parsed.parameter == QLatin1StringView("plain")) {
            action.kind = GhosttyPaneActionKind::CopyToClipboardPlain;
            return action;
        }
        if (*parsed.parameter == QLatin1StringView("vt")
            || *parsed.parameter == QLatin1StringView("html")) {
            return std::unexpected(Error::UnsupportedParameter);
        }
        return std::unexpected(Error::InvalidFormat);
    }
    }
    std::unreachable();
}

std::optional<ApplicationAction> parseApplicationActionView(
    GhosttySerializedActionView parsed)
{
    if (parsed.parameter.has_value()) return std::nullopt;

    const ApplicationActionSpec *const spec =
        findActionSpec(parsed.name, kApplicationActions);
    return spec != nullptr ? spec->implementedAction : std::nullopt;
}

std::optional<GhosttyPaneAction> parsePaneActionView(
    GhosttySerializedActionView parsed)
{
    const QStringView name = parsed.name;
    const OptionalView parameter = parsed.parameter;

    if (GhosttyDirectSurfaceActionParseResult direct =
            parseDirectSurfaceActionView(parsed);
        direct.has_value()) {
        return std::move(*direct);
    }

    const auto viewportAction = [](TerminalViewportRequest::Kind kind) {
        GhosttyPaneAction action;
        action.kind = GhosttyPaneActionKind::ScrollViewport;
        action.viewport.kind = kind;
        return action;
    };

    if (name == QLatin1StringView("scroll_to_top")
        || name == QLatin1StringView("scroll_to_bottom")
        || name == QLatin1StringView("scroll_to_selection")) {
        if (parameter.has_value()) return std::nullopt;
        if (name == QLatin1StringView("scroll_to_top")) {
            return viewportAction(TerminalViewportRequest::Kind::Top);
        }
        if (name == QLatin1StringView("scroll_to_bottom")) {
            return viewportAction(TerminalViewportRequest::Kind::Bottom);
        }
        return viewportAction(TerminalViewportRequest::Kind::Selection);
    }

    if (name == QLatin1StringView("scroll_to_row")) {
        if (!parameter.has_value()) return std::nullopt;
        const std::optional<quint64> row = parseUnsignedInteger(*parameter);
        if (!row.has_value()) return std::nullopt;
        GhosttyPaneAction action =
            viewportAction(TerminalViewportRequest::Kind::Row);
        action.viewport.row = *row;
        return action;
    }

    if (name == QLatin1StringView("scroll_page_up")
        || name == QLatin1StringView("scroll_page_down")) {
        if (parameter.has_value()) return std::nullopt;
        GhosttyPaneAction action;
        action.kind = name == QLatin1StringView("scroll_page_up")
            ? GhosttyPaneActionKind::ScrollPageUp
            : GhosttyPaneActionKind::ScrollPageDown;
        return action;
    }

    if (name == QLatin1StringView("scroll_page_fractional")) {
        if (!parameter.has_value()) return std::nullopt;
        const std::optional<float> fraction = parseFiniteFloat32(*parameter);
        // The terminal always has at least one row. A fraction that cannot be
        // converted safely even at that minimum can never be executable.
        if (!fraction.has_value() || !fitsSignedPointer(*fraction)) {
            return std::nullopt;
        }
        GhosttyPaneAction action;
        action.kind = GhosttyPaneActionKind::ScrollPageFractional;
        action.pageFraction = *fraction;
        return action;
    }

    if (name == QLatin1StringView("scroll_page_lines")) {
        if (!parameter.has_value()) return std::nullopt;
        const std::optional<qint64> lines = parseSignedInteger(*parameter);
        if (!lines.has_value()
            || *lines < std::numeric_limits<qint16>::min()
            || *lines > std::numeric_limits<qint16>::max()) {
            return std::nullopt;
        }
        GhosttyPaneAction action =
            viewportAction(TerminalViewportRequest::Kind::Delta);
        action.viewport.delta = *lines;
        return action;
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

        GhosttyPaneAction action;
        action.kind = GhosttyPaneActionKind::FontSize;
        if (name == QLatin1StringView("increase_font_size")) {
            action.fontSize.kind = TerminalFontSizeRequest::Kind::Increase;
        } else if (name == QLatin1StringView("decrease_font_size")) {
            action.fontSize.kind = TerminalFontSizeRequest::Kind::Decrease;
        } else {
            action.fontSize.kind = TerminalFontSizeRequest::Kind::Set;
        }
        action.fontSize.points = *points;
        return action;
    }

    if (name == QLatin1StringView("reset_font_size")) {
        if (parameter.has_value()) return std::nullopt;
        GhosttyPaneAction action;
        action.kind = GhosttyPaneActionKind::FontSize;
        action.fontSize.kind = TerminalFontSizeRequest::Kind::Reset;
        return action;
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
        GhosttyPaneAction action;
        action.kind = GhosttyPaneActionKind::KeyTable;
        action.keyTable.kind =
            name == QLatin1StringView("activate_key_table")
            ? TerminalKeyTableRequest::Kind::Activate
            : TerminalKeyTableRequest::Kind::ActivateOnce;
        action.keyTable.name = *tableName;
        return action;
    }

    if (name == QLatin1StringView("deactivate_key_table")
        || name == QLatin1StringView("deactivate_all_key_tables")) {
        // Both deactivation fields are void, so even an empty parameter is
        // invalid rather than an alternate spelling of the action.
        if (parameter.has_value()) return std::nullopt;
        GhosttyPaneAction action;
        action.kind = GhosttyPaneActionKind::KeyTable;
        action.keyTable.kind =
            name == QLatin1StringView("deactivate_key_table")
            ? TerminalKeyTableRequest::Kind::Deactivate
            : TerminalKeyTableRequest::Kind::DeactivateAll;
        return action;
    }

    if (name == QLatin1StringView("select_all")) {
        if (parameter.has_value()) return std::nullopt;
        GhosttyPaneAction action;
        action.kind = GhosttyPaneActionKind::SelectAll;
        return action;
    }

    if (name == QLatin1StringView("start_search")
        || name == QLatin1StringView("end_search")
        || name == QLatin1StringView("search_selection")) {
        // These are void Binding.Action fields, so even an explicitly empty
        // parameter is invalid.
        if (parameter.has_value()) return std::nullopt;
        GhosttyPaneAction action;
        if (name == QLatin1StringView("start_search")) {
            action.kind = GhosttyPaneActionKind::StartSearch;
        } else if (name == QLatin1StringView("end_search")) {
            action.kind = GhosttyPaneActionKind::EndSearch;
        } else {
            action.kind = GhosttyPaneActionKind::SearchSelection;
        }
        return action;
    }

    if (name == QLatin1StringView("search")) {
        // Search is a []const u8 field. Its colon is required, its payload may
        // be empty, and only the first colon separates the action name.
        if (!parameter.has_value()) return std::nullopt;
        GhosttyPaneAction action;
        action.kind = GhosttyPaneActionKind::Search;
        action.payload = parameter->toString();
        return action;
    }

    if (name == QLatin1StringView("navigate_search")) {
        if (!parameter.has_value()) return std::nullopt;
        GhosttyPaneAction action;
        action.kind = GhosttyPaneActionKind::NavigateSearch;
        if (*parameter == QLatin1StringView("previous")) {
            action.searchDirection = TerminalSearchDirection::Previous;
        } else if (*parameter == QLatin1StringView("next")) {
            action.searchDirection = TerminalSearchDirection::Next;
        } else {
            return std::nullopt;
        }
        return action;
    }

    if (name == QLatin1StringView("csi")
        || name == QLatin1StringView("esc")
        || name == QLatin1StringView("text")) {
        // All three fields are []const u8 in Binding.Action, so the colon is
        // required but the byte string after it may be empty. Split only at
        // the first colon; later colons are part of the payload.
        if (!parameter.has_value()) return std::nullopt;
        GhosttyPaneAction action;
        if (name == QLatin1StringView("csi")) {
            action.kind = GhosttyPaneActionKind::Csi;
        } else if (name == QLatin1StringView("esc")) {
            action.kind = GhosttyPaneActionKind::Esc;
        } else {
            action.kind = GhosttyPaneActionKind::Text;
        }
        action.payload = parameter->toString();
        return action;
    }

    if (name == QLatin1StringView("reset")) {
        // Reset is a void Binding.Action field, therefore even `reset:` is
        // invalid rather than an empty-parameter spelling of reset.
        if (parameter.has_value()) return std::nullopt;
        GhosttyPaneAction action;
        action.kind = GhosttyPaneActionKind::Reset;
        return action;
    }

    if (name == QLatin1StringView("toggle_readonly")) {
        // Read-only is a per-surface void action. An explicit colon, even
        // with an empty value, is not an alternate spelling.
        if (parameter.has_value()) return std::nullopt;
        GhosttyPaneAction action;
        action.kind = GhosttyPaneActionKind::ToggleReadOnly;
        return action;
    }

    if (name == QLatin1StringView("toggle_mouse_reporting")) {
        // Like the configuration it mutates, this is a per-surface void
        // action. It never changes the terminal's requested DEC mouse mode.
        if (parameter.has_value()) return std::nullopt;
        GhosttyPaneAction action;
        action.kind = GhosttyPaneActionKind::ToggleMouseReporting;
        return action;
    }

    if (name != QLatin1StringView("adjust_selection")) {
        return std::nullopt;
    }
    if (!parameter.has_value()) return std::nullopt;

    GhosttyPaneAction action;
    action.kind = GhosttyPaneActionKind::AdjustSelection;
    if (*parameter == QLatin1StringView("left")) {
        action.selectionAdjustment = TerminalSelectionAdjustment::Left;
    } else if (*parameter == QLatin1StringView("right")) {
        action.selectionAdjustment = TerminalSelectionAdjustment::Right;
    } else if (*parameter == QLatin1StringView("up")) {
        action.selectionAdjustment = TerminalSelectionAdjustment::Up;
    } else if (*parameter == QLatin1StringView("down")) {
        action.selectionAdjustment = TerminalSelectionAdjustment::Down;
    } else if (*parameter == QLatin1StringView("page_up")) {
        action.selectionAdjustment = TerminalSelectionAdjustment::PageUp;
    } else if (*parameter == QLatin1StringView("page_down")) {
        action.selectionAdjustment = TerminalSelectionAdjustment::PageDown;
    } else if (*parameter == QLatin1StringView("home")) {
        action.selectionAdjustment = TerminalSelectionAdjustment::Home;
    } else if (*parameter == QLatin1StringView("end")) {
        action.selectionAdjustment = TerminalSelectionAdjustment::End;
    } else if (*parameter == QLatin1StringView("beginning_of_line")) {
        action.selectionAdjustment =
            TerminalSelectionAdjustment::BeginningOfLine;
    } else if (*parameter == QLatin1StringView("end_of_line")) {
        action.selectionAdjustment = TerminalSelectionAdjustment::EndOfLine;
    } else {
        return std::nullopt;
    }
    return action;
}

std::optional<GhosttyConfiguredAction> parseConfiguredActionView(
    GhosttySerializedActionView parsed,
    WorkspaceActionContext context)
{
    // Application names retain their upstream scope even when this frontend
    // does not implement them. Do not reinterpret an unsupported application
    // action through a surface parser.
    if (const ApplicationActionSpec *const spec =
            findActionSpec(parsed.name, kApplicationActions)) {
        if (!parsed.parameter.has_value()
            && spec->implementedAction.has_value()) {
            return GhosttyConfiguredAction{*spec->implementedAction};
        }
        return std::nullopt;
    }

    if (std::optional<GhosttyPaneAction> action =
            parsePaneActionView(parsed)) {
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

GhosttyActionTranslation GhosttyActionCatalog::translate(
    QStringView serializedAction,
    WorkspaceActionContext context)
{
    return translateWorkspaceAction(parseSerializedAction(serializedAction),
                                    context);
}

std::optional<GhosttyConfiguredAction>
GhosttyActionCatalog::parseConfiguredAction(
    QStringView serializedAction,
    WorkspaceActionContext context)
{
    return parseConfiguredActionView(parseSerializedAction(serializedAction),
                                     context);
}

bool GhosttyActionCatalog::isImplemented(QStringView serializedAction)
{
    return parseConfiguredAction(serializedAction).has_value();
}

std::optional<GhosttyPaneAction> GhosttyActionCatalog::parsePaneAction(
    QStringView serializedAction)
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
    return parseApplicationActionView(
        parseSerializedAction(serializedAction));
}

GhosttyActionScope GhosttyActionCatalog::scope(
    QStringView serializedAction)
{
    return findActionSpec(parseSerializedAction(serializedAction).name,
                          kApplicationActions) != nullptr
        ? GhosttyActionScope::Application
        : GhosttyActionScope::Surface;
}

GhosttyActionInputEffect GhosttyActionCatalog::inputEffect(
    const GhosttyConfiguredAction &action) noexcept
{
    if (const auto *application = std::get_if<ApplicationAction>(&action)) {
        return *application == ApplicationAction::Ignore
            ? GhosttyActionInputEffect::Ignore
            : GhosttyActionInputEffect::None;
    }
    if (const auto *pane = std::get_if<GhosttyPaneAction>(&action)) {
        return pane->kind == GhosttyPaneActionKind::CloseWindow
            ? GhosttyActionInputEffect::ClosingAction
            : GhosttyActionInputEffect::None;
    }
    const auto &workspace = std::get<WorkspaceActionRequest>(action);
    return workspace.action == WorkspaceAction::ClosePane
            || workspace.action == WorkspaceAction::CloseTab
        ? GhosttyActionInputEffect::ClosingAction
        : GhosttyActionInputEffect::None;
}

GhosttyActionInputEffect GhosttyActionCatalog::combinedInputEffect(
    const QStringList &actions)
{
    bool ignored = false;
    for (const QString &serialized : actions) {
        const std::optional<GhosttyConfiguredAction> action =
            parseConfiguredAction(serialized);
        if (!action.has_value()) continue;
        switch (inputEffect(*action)) {
        case GhosttyActionInputEffect::None:
            break;
        case GhosttyActionInputEffect::Ignore:
            ignored = true;
            break;
        case GhosttyActionInputEffect::ClosingAction:
            return GhosttyActionInputEffect::ClosingAction;
        }
    }
    return ignored ? GhosttyActionInputEffect::Ignore
                   : GhosttyActionInputEffect::None;
}

bool GhosttyActionCatalog::shouldCoalesceBroadClose(
    const GhosttyConfiguredAction &action) noexcept
{
    if (const auto *pane = std::get_if<GhosttyPaneAction>(&action)) {
        return pane->kind == GhosttyPaneActionKind::CloseWindow;
    }
    const auto *workspace = std::get_if<WorkspaceActionRequest>(&action);
    return workspace != nullptr
        && (workspace->action == WorkspaceAction::ClosePane
            || (workspace->action == WorkspaceAction::CloseTab
                && workspace->context.closeTabMode == CloseTabMode::This));
}
