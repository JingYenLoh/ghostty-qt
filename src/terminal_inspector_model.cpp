#include "terminal_inspector_model.h"

#include "terminal_controller.h"
#include "terminal_kitty_graphics.h"
#include "terminal_pane.h"

#include <QColor>
#include <QFont>
#include <QKeySequence>
#include <QMutexLocker>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <algorithm>

namespace {

QString graphicsApiName(const QQuickWindow *window)
{
    const QSGRendererInterface *const renderer =
        window != nullptr ? window->rendererInterface() : nullptr;
    if (renderer == nullptr) return QStringLiteral("unknown");

    switch (renderer->graphicsApi()) {
    case QSGRendererInterface::Software: return QStringLiteral("software");
    case QSGRendererInterface::OpenGL: return QStringLiteral("OpenGL");
    case QSGRendererInterface::Vulkan: return QStringLiteral("Vulkan");
    case QSGRendererInterface::OpenVG: return QStringLiteral("OpenVG");
    case QSGRendererInterface::Direct3D11: return QStringLiteral("Direct3D 11");
    case QSGRendererInterface::Direct3D12: return QStringLiteral("Direct3D 12");
    case QSGRendererInterface::Metal: return QStringLiteral("Metal");
    case QSGRendererInterface::Null: return QStringLiteral("null");
    case QSGRendererInterface::Unknown: return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

QString modifierNames(Qt::KeyboardModifiers modifiers)
{
    QStringList names;
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        names.append(QStringLiteral("Shift"));
    }
    if (modifiers.testFlag(Qt::ControlModifier)) {
        names.append(QStringLiteral("Ctrl"));
    }
    if (modifiers.testFlag(Qt::AltModifier)) {
        names.append(QStringLiteral("Alt"));
    }
    if (modifiers.testFlag(Qt::MetaModifier)) {
        names.append(QStringLiteral("Meta"));
    }
    if (modifiers.testFlag(Qt::KeypadModifier)) {
        names.append(QStringLiteral("Keypad"));
    }
    if (modifiers.testFlag(Qt::GroupSwitchModifier)) {
        names.append(QStringLiteral("Group switch"));
    }
    return names.isEmpty() ? QStringLiteral("none") : names.join(u'+');
}

QString colorName(const QColor &color)
{
    return color.isValid() ? color.name(QColor::HexArgb)
                           : QStringLiteral("invalid");
}

QString optionalColorName(const QColor &color)
{
    return color.isValid() ? color.name(QColor::HexArgb) : QString{};
}

QString terminalStatusName(TerminalInspectorStatus status, bool requestPending)
{
    if (requestPending && status == TerminalInspectorStatus::Unavailable) {
        return QStringLiteral("Requesting");
    }
    switch (status) {
    case TerminalInspectorStatus::Unavailable:
        return QStringLiteral("Unavailable");
    case TerminalInspectorStatus::Ready: return QStringLiteral("Ready");
    case TerminalInspectorStatus::Failed: return QStringLiteral("Failed");
    }
    return QStringLiteral("Unavailable");
}

QString cellStatusName(TerminalInspectorCellStatus status)
{
    switch (status) {
    case TerminalInspectorCellStatus::Unavailable:
        return QStringLiteral("Unavailable");
    case TerminalInspectorCellStatus::Ready: return QStringLiteral("Ready");
    case TerminalInspectorCellStatus::Stale:
        return QStringLiteral("Stale — pick again");
    case TerminalInspectorCellStatus::OutOfBounds:
        return QStringLiteral("Outside current viewport");
    case TerminalInspectorCellStatus::Failed: return QStringLiteral("Failed");
    }
    return QStringLiteral("Unavailable");
}

QString cellContentName(TerminalInspectorCellContentKind kind)
{
    switch (kind) {
    case TerminalInspectorCellContentKind::Codepoint:
        return QStringLiteral("Codepoint");
    case TerminalInspectorCellContentKind::Grapheme:
        return QStringLiteral("Grapheme cluster");
    case TerminalInspectorCellContentKind::BackgroundPalette:
        return QStringLiteral("Palette background only");
    case TerminalInspectorCellContentKind::BackgroundRgb:
        return QStringLiteral("RGB background only");
    }
    return QStringLiteral("Unknown");
}

QString cellWidthName(TerminalInspectorCellWidthRole role)
{
    switch (role) {
    case TerminalInspectorCellWidthRole::Narrow:
        return QStringLiteral("Narrow");
    case TerminalInspectorCellWidthRole::Wide: return QStringLiteral("Wide");
    case TerminalInspectorCellWidthRole::SpacerTail:
        return QStringLiteral("Wide spacer tail");
    case TerminalInspectorCellWidthRole::SpacerHead:
        return QStringLiteral("Wrap spacer head");
    }
    return QStringLiteral("Unknown");
}

QString cellSemanticName(TerminalInspectorCellSemantic semantic)
{
    switch (semantic) {
    case TerminalInspectorCellSemantic::Output: return QStringLiteral("Output");
    case TerminalInspectorCellSemantic::Input: return QStringLiteral("Input");
    case TerminalInspectorCellSemantic::Prompt: return QStringLiteral("Prompt");
    }
    return QStringLiteral("Unknown");
}

QString rowSemanticName(TerminalInspectorRowSemantic semantic)
{
    switch (semantic) {
    case TerminalInspectorRowSemantic::None: return QStringLiteral("None");
    case TerminalInspectorRowSemantic::Prompt: return QStringLiteral("Prompt");
    case TerminalInspectorRowSemantic::PromptContinuation:
        return QStringLiteral("Prompt continuation");
    }
    return QStringLiteral("Unknown");
}

QString underlineName(TerminalInspectorUnderlineStyle underline)
{
    switch (underline) {
    case TerminalInspectorUnderlineStyle::None: return QStringLiteral("None");
    case TerminalInspectorUnderlineStyle::Single:
        return QStringLiteral("Single");
    case TerminalInspectorUnderlineStyle::Double:
        return QStringLiteral("Double");
    case TerminalInspectorUnderlineStyle::Curly: return QStringLiteral("Curly");
    case TerminalInspectorUnderlineStyle::Dotted:
        return QStringLiteral("Dotted");
    case TerminalInspectorUnderlineStyle::Dashed:
        return QStringLiteral("Dashed");
    }
    return QStringLiteral("Unknown");
}

QString styleColorName(const TerminalInspectorStyleColor &color,
                       QStringView noneName = u"Default")
{
    switch (color.kind) {
    case TerminalInspectorStyleColorKind::None: return noneName.toString();
    case TerminalInspectorStyleColorKind::Palette:
        return QStringLiteral("Palette %1").arg(color.paletteIndex);
    case TerminalInspectorStyleColorKind::Rgb:
        return optionalColorName(color.rgb);
    }
    return QStringLiteral("Unknown");
}

QString diagnosticByteString(const QByteArray &bytes)
{
    const QString decoded = QString::fromUtf8(bytes);
    if (decoded.toUtf8() == bytes) return decoded;
    return QStringLiteral("hex: %1").arg(QString::fromLatin1(bytes.toHex(' ')));
}

QString escapedTextPreview(QStringView text, qsizetype maximumCodepoints = 64)
{
    QString result;
    result.reserve(std::min(text.size(), maximumCodepoints) + 8);
    qsizetype offset = 0;
    qsizetype displayed = 0;
    while (offset < text.size() && displayed < maximumCodepoints) {
        uint codepoint = text.at(offset).unicode();
        qsizetype width = 1;
        if (QChar::isHighSurrogate(codepoint) && offset + 1 < text.size()
            && text.at(offset + 1).isLowSurrogate()) {
            codepoint =
                QChar::surrogateToUcs4(text.at(offset), text.at(offset + 1));
            width = 2;
        }

        switch (codepoint) {
        case '\\': result.append(QStringLiteral("\\\\")); break;
        case '\n': result.append(QStringLiteral("\\n")); break;
        case '\r': result.append(QStringLiteral("\\r")); break;
        case '\t': result.append(QStringLiteral("\\t")); break;
        default:
            if (codepoint < 0x20U || (codepoint >= 0x7fU && codepoint < 0xa0U)
                || (codepoint >= 0xd800U && codepoint <= 0xdfffU)
                || codepoint == 0x061cU || codepoint == 0x200eU
                || codepoint == 0x200fU || codepoint == 0x2028U
                || codepoint == 0x2029U
                || (codepoint >= 0x202aU && codepoint <= 0x202eU)
                || (codepoint >= 0x2066U && codepoint <= 0x2069U)) {
                result.append(QStringLiteral("\\u{%1}").arg(codepoint, 0, 16));
            } else if (codepoint <= 0xffffU) {
                result.append(QChar(static_cast<ushort>(codepoint)));
            } else {
                result.append(QChar::highSurrogate(codepoint));
                result.append(QChar::lowSurrogate(codepoint));
            }
            break;
        }
        offset += width;
        ++displayed;
    }
    if (offset < text.size()) result.append(QChar(0x2026));
    return result;
}

QString keyActionName(const TerminalKeyInput &input)
{
    if (!input.pressed) return QStringLiteral("Release");
    return input.autoRepeat ? QStringLiteral("Repeat")
                            : QStringLiteral("Press");
}

QString keyName(const TerminalKeyInput &input)
{
    const QString name =
        QKeySequence(input.key).toString(QKeySequence::PortableText);
    return name.isEmpty() ? QStringLiteral("Key %1").arg(input.key) : name;
}

QString keySummary(const TerminalKeyInput &input)
{
    QStringList fields{
        keyActionName(input),
        keyName(input),
        modifierNames(Qt::KeyboardModifiers(input.modifiers)),
    };
    if (!input.text.isEmpty()) {
        fields.append(
            QStringLiteral("text “%1”").arg(escapedTextPreview(input.text)));
    }
    return fields.join(QStringLiteral(" · "));
}

QString keyDetails(const TerminalKeyInput &input)
{
    QStringList fields{
        QStringLiteral("Qt key %1").arg(input.key),
        QStringLiteral("scan code 0x%1").arg(input.nativeScanCode, 0, 16),
        QStringLiteral("consumed %1")
            .arg(modifierNames(Qt::KeyboardModifiers(input.consumedModifiers))),
    };
    if (input.unshiftedCodepoint != 0) {
        fields.append(
            QStringLiteral("unshifted U+%1")
                .arg(input.unshiftedCodepoint, 4, 16, QLatin1Char('0'))
                .toUpper());
    }
    if (input.composing) fields.append(QStringLiteral("composing"));
    return fields.join(QStringLiteral(" · "));
}

QString sequenceResolutionName(TerminalSequenceResolution resolution)
{
    switch (resolution) {
    case TerminalSequenceResolution::Drop: return QStringLiteral("Drop");
    case TerminalSequenceResolution::Flush: return QStringLiteral("Flush");
    case TerminalSequenceResolution::FlushAndSendCurrent:
        return QStringLiteral("Flush and send current");
    }
    return QStringLiteral("Unknown");
}

QString keyboardDecisionName(TerminalKeyboardTraceDecisionKind kind)
{
    switch (kind) {
    case TerminalKeyboardTraceDecisionKind::RootApplicationBinding:
        return QStringLiteral("Root application binding");
    case TerminalKeyboardTraceDecisionKind::RootGlobalBinding:
        return QStringLiteral("Root global binding");
    case TerminalKeyboardTraceDecisionKind::RootConsumedRelease:
        return QStringLiteral("Root consumed release");
    case TerminalKeyboardTraceDecisionKind::PaneUnmatched:
        return QStringLiteral("Pane pass-through");
    case TerminalKeyboardTraceDecisionKind::PaneLeader:
        return QStringLiteral("Pane sequence leader");
    case TerminalKeyboardTraceDecisionKind::PaneInvalidSequence:
        return QStringLiteral("Pane invalid sequence");
    case TerminalKeyboardTraceDecisionKind::PaneIgnoredSequence:
        return QStringLiteral("Pane ignored sequence");
    case TerminalKeyboardTraceDecisionKind::PaneLocalBinding:
        return QStringLiteral("Pane local binding");
    case TerminalKeyboardTraceDecisionKind::PaneBroadBinding:
        return QStringLiteral("Pane broad binding");
    case TerminalKeyboardTraceDecisionKind::PaneFallbackConsumed:
        return QStringLiteral("Pane fallback consumed");
    case TerminalKeyboardTraceDecisionKind::PaneFallbackPassed:
        return QStringLiteral("Pane fallback pass-through");
    case TerminalKeyboardTraceDecisionKind::PaneConsumedRelease:
        return QStringLiteral("Pane consumed release");
    case TerminalKeyboardTraceDecisionKind::PaneInspectorCancel:
        return QStringLiteral("Pane inspector cancellation");
    }
    return QStringLiteral("Unknown decision");
}

QString keyboardTraceOperationName(TerminalKeyboardTraceOperation operation)
{
    switch (operation) {
    case TerminalKeyboardTraceOperation::Key: return QStringLiteral("Key");
    case TerminalKeyboardTraceOperation::SequenceStage:
        return QStringLiteral("Sequence stage");
    case TerminalKeyboardTraceOperation::SequenceResolution:
        return QStringLiteral("Sequence resolution");
    }
    return QStringLiteral("Unknown operation");
}

QString
keyboardTraceDispositionName(TerminalKeyboardTraceDisposition disposition)
{
    switch (disposition) {
    case TerminalKeyboardTraceDisposition::Queued:
        return QStringLiteral("Queued for PTY");
    case TerminalKeyboardTraceDisposition::EncoderFailed:
        return QStringLiteral("Encoder failed");
    case TerminalKeyboardTraceDisposition::EncoderEmpty:
        return QStringLiteral("Encoder produced no bytes");
    case TerminalKeyboardTraceDisposition::KeyboardActionMode:
        return QStringLiteral("Suppressed by KAM");
    case TerminalKeyboardTraceDisposition::ReadOnly:
        return QStringLiteral("Discarded by read-only mode");
    case TerminalKeyboardTraceDisposition::TerminalUnavailable:
        return QStringLiteral("Terminal unavailable");
    case TerminalKeyboardTraceDisposition::SessionUnavailable:
        return QStringLiteral("PTY unavailable");
    case TerminalKeyboardTraceDisposition::ExitWaitConsumed:
        return QStringLiteral("Consumed by exit wait");
    case TerminalKeyboardTraceDisposition::Staged:
        return QStringLiteral("Encoded and staged");
    case TerminalKeyboardTraceDisposition::Dropped:
        return QStringLiteral("Sequence dropped");
    case TerminalKeyboardTraceDisposition::Superseded:
        return QStringLiteral("Sequence superseded");
    case TerminalKeyboardTraceDisposition::StaleSequence:
        return QStringLiteral("Stale sequence token");
    }
    return QStringLiteral("Unknown disposition");
}

QString keyboardDecisionDetails(const TerminalKeyboardTraceDecision &decision)
{
    QStringList fields;
    if (decision.sequenceToken != 0) {
        fields.append(
            QStringLiteral("sequence token %1").arg(decision.sequenceToken));
    }
    if (!decision.actions.isEmpty()) {
        fields.append(QStringLiteral("actions %1")
                          .arg(decision.actions.join(QStringLiteral(" → "))));
    }
    if (!decision.activeTables.isEmpty()) {
        fields.append(
            QStringLiteral("tables %1")
                .arg(decision.activeTables.join(QStringLiteral(", "))));
    }
    if (!decision.pendingSequence.isEmpty()) {
        fields.append(QStringLiteral("pending %1")
                          .arg(decision.pendingSequence.join(u' ')));
    }
    QStringList flags;
    if (decision.consumed) flags.append(QStringLiteral("consumed"));
    if (decision.performable) flags.append(QStringLiteral("performable"));
    if (decision.all) flags.append(QStringLiteral("all"));
    if (decision.global) flags.append(QStringLiteral("global"));
    if (decision.physical) flags.append(QStringLiteral("physical"));
    if (!flags.isEmpty()) {
        fields.append(QStringLiteral("flags %1").arg(flags.join(u',')));
    }
    fields.append(keyDetails(decision.input));
    return fields.join(QStringLiteral(" · "));
}

QString keyboardTraceResultDetails(const TerminalKeyboardTraceResult &result)
{
    QStringList fields;
    if (result.sequenceToken != 0) {
        fields.append(
            QStringLiteral("sequence token %1").arg(result.sequenceToken));
    }
    if (!result.encodedPrefix.isEmpty()) {
        QString preview = QString::fromLatin1(result.encodedPrefix.toHex(' '));
        if (result.prefixTruncated) preview.append(QStringLiteral(" …"));
        fields.append(QStringLiteral("hex %1").arg(preview));
    }
    return fields.join(QStringLiteral(" · "));
}

QString mouseActionName(TerminalMouseInput::Action action)
{
    switch (action) {
    case TerminalMouseInput::Press: return QStringLiteral("Press");
    case TerminalMouseInput::Release: return QStringLiteral("Release");
    case TerminalMouseInput::Motion: return QStringLiteral("Motion");
    }
    return QStringLiteral("Unknown");
}

QString enabledName(bool enabled)
{
    return enabled ? QStringLiteral("Enabled") : QStringLiteral("Disabled");
}

} // namespace

TerminalInspectorModel::TerminalInspectorModel(TerminalPane *pane)
    : QObject(pane)
    , pane_(pane)
    , eventModel_(new TerminalInspectorEventModel(this))
    , refreshTimer_(new QTimer(this))
    , terminalEventTimer_(new QTimer(this))
{
    refreshTimer_->setInterval(250);
    refreshTimer_->setTimerType(Qt::CoarseTimer);
    connect(refreshTimer_, &QTimer::timeout, this,
            &TerminalInspectorModel::refresh);
    terminalEventTimer_->setInterval(50);
    terminalEventTimer_->setSingleShot(true);
    terminalEventTimer_->setTimerType(Qt::CoarseTimer);
    connect(terminalEventTimer_, &QTimer::timeout, this,
            &TerminalInspectorModel::flushPendingTerminalEvent);
    connect(
        eventModel_, &TerminalInspectorEventModel::pausedChanged, this, [this] {
            if (!active_) return;
            const QPointer<TerminalInspectorModel> guard(this);
            if (pane_ != nullptr) {
                pane_->setInspectorKeyboardTraceCapture(!eventModel_->paused());
            }
            if (guard == nullptr || !guard->active_) return;
            if (!eventModel_->paused()) return;
            const bool hadPendingFrame = pendingTerminalEvent_.updates != 0;
            clearPendingTerminalEvent();
            if (hadPendingFrame) {
                eventModel_->skipObservation();
            }
        });
    connect(eventModel_, &TerminalInspectorEventModel::cleared, this,
            &TerminalInspectorModel::clearPendingTerminalEvent);
    if (pane != nullptr && pane->controller_ != nullptr) {
        TerminalController *const controller = pane->controller_;
        connect(controller, &TerminalController::terminalUpdated, this,
                &TerminalInspectorModel::recordTerminalUpdate);
        connect(controller, &TerminalController::keyRequested, this,
                [this](const TerminalKeyInput &input) {
                    if (!acceptsKeyboardTraceInput(input)) return;
                    if (skipEventWhilePaused()) return;
                    appendEvent(TerminalInspectorEventModel::Category::Input,
                                QStringLiteral("Forwarded key request"),
                                keySummary(input), keyDetails(input),
                                input.inspectorTraceId);
                });
        connect(controller, &TerminalController::sequenceKeyStagingRequested,
                this, [this](quint64 token, const TerminalKeyInput &input) {
                    if (!acceptsKeyboardTraceInput(input)) return;
                    if (skipEventWhilePaused()) return;
                    appendEvent(TerminalInspectorEventModel::Category::Input,
                                QStringLiteral("Sequence key staged"),
                                QStringLiteral("Token %1 · %2")
                                    .arg(token)
                                    .arg(keySummary(input)),
                                keyDetails(input), input.inspectorTraceId);
                });
        connect(controller, &TerminalController::sequenceResolutionRequested,
                this,
                [this](quint64 token, TerminalSequenceResolution resolution,
                       bool hasCurrent, const TerminalKeyInput &current) {
                    if (!acceptsKeyboardTraceInput(current)) return;
                    if (skipEventWhilePaused()) return;
                    QString details;
                    if (hasCurrent) {
                        details = keySummary(current) + QStringLiteral(" · ")
                            + keyDetails(current);
                    }
                    appendEvent(TerminalInspectorEventModel::Category::Input,
                                QStringLiteral("Sequence resolved"),
                                QStringLiteral("Token %1 · %2")
                                    .arg(token)
                                    .arg(sequenceResolutionName(resolution)),
                                details, current.inspectorTraceId);
                });
        connect(
            controller, &TerminalController::keyboardTraceResult, this,
            [this](const TerminalKeyboardTraceResult &result) {
                if (skipEventWhilePaused()) return;
                appendEvent(
                    TerminalInspectorEventModel::Category::Input,
                    QStringLiteral("Worker key encoding"),
                    QStringLiteral("%1 · %2 · %3 encoded byte%4")
                        .arg(keyboardTraceOperationName(result.operation),
                             keyboardTraceDispositionName(result.disposition))
                        .arg(result.encodedByteCount)
                        .arg(result.encodedByteCount == 1
                                 ? QString{}
                                 : QStringLiteral("s")),
                    keyboardTraceResultDetails(result), result.traceId);
            });
        connect(
            controller, &TerminalController::inputMethodRequested, this,
            [this](const TerminalInputMethodInput &input) {
                if (skipEventWhilePaused()) return;
                QStringList fields;
                fields.append(QStringLiteral("%1 UTF-16 units committed")
                                  .arg(input.commitText.size()));
                if (input.preeditTransition) {
                    fields.append(QStringLiteral("preedit transition"));
                }
                const QString preview = escapedTextPreview(input.commitText);
                appendEvent(TerminalInspectorEventModel::Category::Input,
                            QStringLiteral("Input method request"),
                            fields.join(QStringLiteral(" · ")),
                            preview.isEmpty()
                                ? QString{}
                                : QStringLiteral("commit “%1”").arg(preview));
            });
        connect(controller, &TerminalController::csiRequested, this,
                [this](const QByteArray &payload) {
                    if (skipEventWhilePaused()) return;
                    appendEvent(
                        TerminalInspectorEventModel::Category::Input,
                        QStringLiteral("CSI request"),
                        QStringLiteral("%1 payload bytes").arg(payload.size()));
                });
        connect(controller, &TerminalController::escapeRequested, this,
                [this](const QByteArray &payload) {
                    if (skipEventWhilePaused()) return;
                    appendEvent(
                        TerminalInspectorEventModel::Category::Input,
                        QStringLiteral("Escape request"),
                        QStringLiteral("%1 payload bytes").arg(payload.size()));
                });
        connect(
            controller, &TerminalController::rawTextRequested, this,
            [this](const QByteArray &payload) {
                if (skipEventWhilePaused()) return;
                appendEvent(
                    TerminalInspectorEventModel::Category::Input,
                    QStringLiteral("Raw text request"),
                    QStringLiteral("%1 serialized bytes").arg(payload.size()));
            });
        connect(controller, &TerminalController::resetTerminalRequested, this,
                [this] {
                    if (skipEventWhilePaused()) return;
                    appendEvent(
                        TerminalInspectorEventModel::Category::Input,
                        QStringLiteral("Reset terminal request"),
                        QStringLiteral("Forwarded to the session worker"));
                });
        connect(controller, &TerminalController::mouseRequested, this,
                [this](const TerminalMouseInput &input) {
                    if (input.action == TerminalMouseInput::Motion) return;
                    if (skipEventWhilePaused()) return;
                    appendEvent(TerminalInspectorEventModel::Category::Input,
                                QStringLiteral("Mouse request"),
                                QStringLiteral("%1 button %2 at %3, %4")
                                    .arg(mouseActionName(input.action))
                                    .arg(input.button)
                                    .arg(input.x, 0, 'f', 1)
                                    .arg(input.y, 0, 'f', 1),
                                QStringLiteral("%1 · buttons held %2")
                                    .arg(modifierNames(
                                        Qt::KeyboardModifiers(input.modifiers)))
                                    .arg(input.anyButtonPressed
                                             ? QStringLiteral("yes")
                                             : QStringLiteral("no")));
                });
        connect(controller, &TerminalController::wheelRequested, this,
                [this](const TerminalWheelInput &input) {
                    if (skipEventWhilePaused()) return;
                    appendEvent(
                        TerminalInspectorEventModel::Category::Input,
                        QStringLiteral("Wheel request"),
                        QStringLiteral("%1 rows · %2 columns at %3, %4")
                            .arg(input.rows)
                            .arg(input.columns)
                            .arg(input.x, 0, 'f', 1)
                            .arg(input.y, 0, 'f', 1),
                        QStringLiteral("%1 · mouse reporting %2")
                            .arg(modifierNames(
                                Qt::KeyboardModifiers(input.modifiers)))
                            .arg(enabledName(input.mouseReportingEnabled)));
                });
        connect(controller, &TerminalController::focusRequested, this,
                [this](bool focused) {
                    if (skipEventWhilePaused()) return;
                    appendEvent(TerminalInspectorEventModel::Category::Input,
                                QStringLiteral("Focus request"),
                                focused ? QStringLiteral("Focused")
                                        : QStringLiteral("Unfocused"));
                });
        connect(controller, &TerminalController::pasteRequested, this,
                [this](const QString &text) {
                    if (skipEventWhilePaused()) return;
                    appendEvent(
                        TerminalInspectorEventModel::Category::Input,
                        QStringLiteral("Paste request"),
                        QStringLiteral("%1 UTF-16 units (content omitted)")
                            .arg(text.size()));
                });

        connect(controller, &TerminalController::titleChanged, this,
                [this](const QString &title) {
                    if (skipEventWhilePaused()) return;
                    appendEvent(TerminalInspectorEventModel::Category::State,
                                QStringLiteral("Title changed"),
                                escapedTextPreview(title));
                });
        connect(controller, &TerminalController::currentDirectoryChanged, this,
                [this](const QString &directory) {
                    if (skipEventWhilePaused()) return;
                    appendEvent(TerminalInspectorEventModel::Category::State,
                                QStringLiteral("Working directory changed"),
                                escapedTextPreview(directory));
                });
        connect(controller, &TerminalController::terminalMouseTrackingChanged,
                this, [this](bool enabled) {
                    if (skipEventWhilePaused()) return;
                    appendEvent(TerminalInspectorEventModel::Category::State,
                                QStringLiteral("Terminal mouse tracking"),
                                enabledName(enabled));
                });
        connect(controller, &TerminalController::keyboardActionModeChanged,
                this, [this](bool enabled) {
                    if (skipEventWhilePaused()) return;
                    appendEvent(TerminalInspectorEventModel::Category::State,
                                QStringLiteral("Keyboard action mode"),
                                enabledName(enabled));
                });
        connect(controller, &TerminalController::runningChanged, this,
                [this](bool running) {
                    if (skipEventWhilePaused()) return;
                    appendEvent(TerminalInspectorEventModel::Category::State,
                                QStringLiteral("Session running"),
                                running ? QStringLiteral("Running")
                                        : QStringLiteral("Stopped"));
                });
        connect(controller, &TerminalController::activeProcessChanged, this,
                [this](bool active) {
                    if (skipEventWhilePaused()) return;
                    appendEvent(TerminalInspectorEventModel::Category::State,
                                QStringLiteral("Active process"),
                                enabledName(active));
                });
        connect(controller, &TerminalController::selectionAvailableChanged,
                this, [this](bool available) {
                    if (skipEventWhilePaused()) return;
                    appendEvent(TerminalInspectorEventModel::Category::State,
                                QStringLiteral("Selection availability"),
                                available ? QStringLiteral("Available")
                                          : QStringLiteral("Empty"));
                });
        connect(controller, &TerminalController::readOnlyChanged, this,
                [this](bool readOnly) {
                    if (skipEventWhilePaused()) return;
                    appendEvent(TerminalInspectorEventModel::Category::State,
                                QStringLiteral("Read-only mode"),
                                enabledName(readOnly));
                });
        connect(controller, &TerminalController::sessionExited, this,
                [this](int exitCode, int signalNumber, bool hold,
                       bool waitForKey, quint64 runtimeMilliseconds,
                       bool abnormal) {
                    if (skipEventWhilePaused()) return;
                    const QString outcome = signalNumber != 0
                        ? QStringLiteral("Signal %1").arg(signalNumber)
                        : QStringLiteral("Exit code %1").arg(exitCode);
                    appendEvent(
                        TerminalInspectorEventModel::Category::State,
                        QStringLiteral("Session exited"), outcome,
                        QStringLiteral(
                            "runtime %1 ms · hold %2 · wait %3 · abnormal %4")
                            .arg(runtimeMilliseconds)
                            .arg(hold ? QStringLiteral("yes")
                                      : QStringLiteral("no"))
                            .arg(waitForKey ? QStringLiteral("yes")
                                            : QStringLiteral("no"))
                            .arg(abnormal ? QStringLiteral("yes")
                                          : QStringLiteral("no")));
                });
        connect(controller, &TerminalController::exitKeyDismissed, this,
                [this] {
                    if (skipEventWhilePaused()) return;
                    appendEvent(
                        TerminalInspectorEventModel::Category::State,
                        QStringLiteral("Exit wait dismissed"),
                        QStringLiteral("Key consumed by session lifecycle"));
                });
        connect(controller, &TerminalController::errorOccurred, this,
                [this](const QString &message) {
                    if (skipEventWhilePaused()) return;
                    appendEvent(TerminalInspectorEventModel::Category::State,
                                QStringLiteral("Session error"),
                                escapedTextPreview(message));
                });
        connect(controller, &TerminalController::bell, this, [this] {
            if (skipEventWhilePaused()) return;
            appendEvent(TerminalInspectorEventModel::Category::State,
                        QStringLiteral("Bell"),
                        QStringLiteral("Terminal bell received"));
        });

        connect(pane, &TerminalPane::activeKeyTablesChanged, this, [this] {
            if (skipEventWhilePaused()) return;
            const QStringList tables =
                pane_ != nullptr ? pane_->activeKeyTables() : QStringList{};
            appendEvent(TerminalInspectorEventModel::Category::State,
                        QStringLiteral("Active key tables"),
                        tables.isEmpty() ? QStringLiteral("None")
                                         : tables.join(QStringLiteral(", ")));
        });
        connect(pane, &TerminalPane::keyboardTraceDecision, this,
                [this](const TerminalKeyboardTraceDecision &decision) {
                    if (skipEventWhilePaused()) return;
                    appendEvent(TerminalInspectorEventModel::Category::Input,
                                QStringLiteral("Keybinding decision"),
                                QStringLiteral("%1 · %2").arg(
                                    keyboardDecisionName(decision.kind),
                                    keySummary(decision.input)),
                                keyboardDecisionDetails(decision),
                                decision.input.inspectorTraceId);
                });
        connect(pane, &TerminalPane::pendingKeySequenceChanged, this, [this] {
            if (skipEventWhilePaused()) return;
            const QStringList sequence =
                pane_ != nullptr ? pane_->pendingKeySequence() : QStringList{};
            appendEvent(TerminalInspectorEventModel::Category::State,
                        QStringLiteral("Pending key sequence"),
                        sequence.isEmpty()
                            ? QStringLiteral("None")
                            : sequence.join(QStringLiteral(" ")));
        });
        connect(pane, &TerminalPane::customShaderDiagnosticChanged, this,
                [this](const QString &diagnostic) {
                    if (skipEventWhilePaused()) return;
                    appendEvent(TerminalInspectorEventModel::Category::State,
                                QStringLiteral("Custom shader diagnostic"),
                                diagnostic.isEmpty()
                                    ? QStringLiteral("Cleared")
                                    : escapedTextPreview(diagnostic));
                });

        connect(controller, &TerminalController::terminalInspectorSnapshotReady,
                this,
                [this](quint64 requestId,
                       const TerminalInspectorSnapshot &snapshot) {
                    if (!active_ || requestId == 0
                        || requestId != pendingTerminalRequestId_) {
                        return;
                    }
                    pendingTerminalRequestId_ = 0;
                    terminalSnapshot_ = snapshot;
                    rebuildSnapshot();
                });
        connect(controller, &TerminalController::terminalInspectorCellReady,
                this,
                [this](quint64 requestId,
                       const TerminalInspectorCellSnapshot &snapshot) {
                    if (!active_ || requestId == 0
                        || requestId != pendingCellRequestId_) {
                        return;
                    }
                    pendingCellRequestId_ = 0;
                    cellSnapshot_ = snapshot;
                    cellHasResult_ = true;
                    rebuildSnapshot();
                });
        connect(pane, &TerminalPane::inspectorCellPickingChanged, this,
                &TerminalInspectorModel::rebuildSnapshot);
        connect(pane, &TerminalPane::inspectorCellPicked, this,
                [this](int viewportColumn, int viewportRow,
                       quint64 contentRevision) {
                    if (!active_) return;
                    TerminalPane *const targetPane = pane_.data();
                    if (targetPane == nullptr
                        || targetPane->controller_ == nullptr) {
                        return;
                    }
                    cellSnapshot_ = {};
                    cellSnapshot_.contentRevision = contentRevision;
                    cellSnapshot_.viewportColumn = viewportColumn;
                    cellSnapshot_.viewportRow = viewportRow;
                    cellHasResult_ = false;
                    const QPointer<TerminalInspectorModel> guard(this);
                    const quint64 requestId =
                        targetPane->controller_->requestTerminalInspectorCell(
                            contentRevision, viewportColumn, viewportRow);
                    if (guard == nullptr || !guard->active_) return;
                    pendingCellRequestId_ = requestId;
                    if (pendingCellRequestId_ == 0) {
                        cellHasResult_ = true;
                    }
                    rebuildSnapshot();
                });
    }
    // Populate the value-only GUI projection before publication, but defer
    // the first outward worker request until construction is complete.
    rebuildSnapshot();
    QTimer::singleShot(0, this, [this] { refresh(); });
    refreshTimer_->start();
}

void TerminalInspectorModel::appendEvent(
    TerminalInspectorEventModel::Category category, QString kind,
    QString summary, QString details, quint64 traceId)
{
    if (!active_) return;
    const QPointer<TerminalInspectorModel> guard(this);
    flushPendingTerminalEvent();
    if (guard == nullptr || !guard->active_) return;
    guard->eventModel_->append(category, std::move(kind), std::move(summary),
                               std::move(details), traceId);
}

bool TerminalInspectorModel::skipEventWhilePaused()
{
    if (!active_) return true;
    if (!eventModel_->paused()) return false;
    eventModel_->skipObservation();
    return true;
}

bool TerminalInspectorModel::acceptsKeyboardTraceInput(
    const TerminalKeyInput &input) const
{
    return input.inspectorTraceGeneration != 0 && input.inspectorTraceId != 0
        && pane_ != nullptr
        && input.inspectorTraceGeneration
        == pane_->inspectorKeyboardTraceGeneration_;
}

void TerminalInspectorModel::recordTerminalUpdate(const TerminalUpdate &update)
{
    if (!active_) return;
    if (eventModel_->paused()) {
        // Preserve an observable sequence gap without retaining or formatting
        // any payload while capture is paused.
        eventModel_->skipObservation();
        return;
    }

    PendingTerminalEvent &pending = pendingTerminalEvent_;
    if (pending.updates == 0) {
        pending.firstRevision = update.contentRevision;
    }
    ++pending.updates;
    pending.columns = update.columns;
    pending.rows = update.rows;
    pending.dirtyRows += update.dirtyRows.size();
    if (update.fullFrame || update.cursorChanged) {
        pending.cursorColumn = update.cursorColumn;
        pending.cursorRow = update.cursorRow;
    }
    pending.lastRevision = update.contentRevision;
    pending.fullFrame = pending.fullFrame || update.fullFrame;
    pending.colorsChanged = pending.colorsChanged || update.colorsChanged;
    pending.cursorChanged =
        pending.cursorChanged || update.fullFrame || update.cursorChanged;
    pending.scrollbarChanged =
        pending.scrollbarChanged || update.scrollbarChanged;
    pending.kittyGraphicsChanged =
        pending.kittyGraphicsChanged || update.kittyGraphicsChanged;
    pending.resetCursorBlink =
        pending.resetCursorBlink || update.resetCursorBlink;
    if (!terminalEventTimer_->isActive()) terminalEventTimer_->start();
}

void TerminalInspectorModel::flushPendingTerminalEvent()
{
    if (!active_ || pendingTerminalEvent_.updates == 0) return;
    terminalEventTimer_->stop();
    const PendingTerminalEvent pending = pendingTerminalEvent_;
    pendingTerminalEvent_ = {};

    QStringList summary{
        QStringLiteral("%1 update%2")
            .arg(pending.updates)
            .arg(pending.updates == 1 ? QString{} : QStringLiteral("s")),
        QStringLiteral("%1 × %2").arg(pending.columns).arg(pending.rows),
        QStringLiteral("%1 dirty row delta%2")
            .arg(pending.dirtyRows)
            .arg(pending.dirtyRows == 1 ? QString{} : QStringLiteral("s")),
    };
    if (pending.firstRevision == pending.lastRevision) {
        summary.append(QStringLiteral("revision %1").arg(pending.lastRevision));
    } else {
        summary.append(QStringLiteral("revisions %1–%2")
                           .arg(pending.firstRevision)
                           .arg(pending.lastRevision));
    }

    QStringList changes;
    if (pending.fullFrame) changes.append(QStringLiteral("full frame"));
    if (pending.colorsChanged) changes.append(QStringLiteral("colors"));
    if (pending.cursorChanged) {
        changes.append(QStringLiteral("cursor at %1, %2")
                           .arg(pending.cursorColumn)
                           .arg(pending.cursorRow));
    }
    if (pending.scrollbarChanged) {
        changes.append(QStringLiteral("scrollbar"));
    }
    if (pending.kittyGraphicsChanged) {
        changes.append(QStringLiteral("Kitty graphics"));
    }
    if (pending.resetCursorBlink) {
        changes.append(QStringLiteral("cursor blink reset"));
    }
    eventModel_->append(
        TerminalInspectorEventModel::Category::Terminal,
        QStringLiteral("Frame update"), summary.join(QStringLiteral(" · ")),
        changes.isEmpty() ? QStringLiteral("No visual flags")
                          : changes.join(QStringLiteral(" · ")));
}

void TerminalInspectorModel::clearPendingTerminalEvent()
{
    terminalEventTimer_->stop();
    pendingTerminalEvent_ = {};
}

void TerminalInspectorModel::refresh()
{
    if (!active_) return;
    const QPointer<TerminalInspectorModel> guard(this);
    rebuildSnapshot();
    if (guard == nullptr || !guard->active_
        || guard->pendingTerminalRequestId_ != 0) {
        return;
    }

    TerminalPane *const pane = guard->pane_.data();
    if (pane == nullptr || pane->controller_ == nullptr) return;
    const quint64 requestId =
        pane->controller_->requestTerminalInspectorSnapshot();
    if (guard == nullptr || !guard->active_) return;
    guard->pendingTerminalRequestId_ = requestId;
    if (guard->pendingTerminalRequestId_ != 0
        && guard->terminalSnapshot_.status
            == TerminalInspectorStatus::Unavailable) {
        guard->rebuildSnapshot();
    }
}

void TerminalInspectorModel::beginCellPick()
{
    if (!active_) return;
    pendingCellRequestId_ = 0;
    if (pane_ != nullptr) pane_->setInspectorCellPicking(true);
}

void TerminalInspectorModel::cancelCellPick()
{
    if (!active_) return;
    if (pane_ != nullptr) pane_->setInspectorCellPicking(false);
}

void TerminalInspectorModel::rebuildSnapshot()
{
    if (!active_) return;
    TerminalPane *const pane = pane_.data();
    if (pane == nullptr) return;

    int columns = 0;
    int rows = 0;
    int cursorColumn = 0;
    int cursorRow = 0;
    int cursorStyle = 0;
    int cursorColumnSpan = 1;
    quint64 scrollTotal = 0;
    quint64 scrollOffset = 0;
    quint64 scrollLength = 0;
    quint64 contentRevision = 0;
    bool cursorVisible = false;
    bool cursorBlinking = false;
    qreal cellWidth = 0.0;
    qreal cellHeight = 0.0;
    QFont font;
    QColor foreground;
    QColor background;
    QColor cursorColor;
    QVector<QColor> palette;
    std::shared_ptr<const TerminalKittyGraphicsSnapshot> kittyGraphics;
    {
        QMutexLocker locker(&pane->renderMutex_);
        columns = pane->frame_.columns;
        rows = pane->frame_.rows;
        cursorColumn = pane->frame_.cursorColumn;
        cursorRow = pane->frame_.cursorRow;
        cursorStyle = pane->frame_.cursorStyle;
        cursorColumnSpan = pane->frame_.cursorColumnSpan;
        scrollTotal = pane->frame_.scrollTotal;
        scrollOffset = pane->frame_.scrollOffset;
        scrollLength = pane->frame_.scrollLength;
        contentRevision = pane->frame_.contentRevision;
        cursorVisible = pane->frame_.cursorVisible;
        cursorBlinking = pane->frame_.cursorBlinking;
        cellWidth = pane->metrics_.cellWidth;
        cellHeight = pane->metrics_.cellHeight;
        font = pane->metrics_.font(TerminalFontRole::Regular);
        foreground = pane->frame_.foreground;
        background = pane->frame_.background;
        cursorColor = pane->frame_.cursorColor;
        palette = pane->frame_.palette;
        kittyGraphics = pane->frame_.kittyGraphics;
    }

    QVariantList paletteNames;
    paletteNames.reserve(palette.size());
    for (const QColor &color : palette) {
        paletteNames.append(colorName(color));
    }

    int kittyPlacements = 0;
    int kittyImages = 0;
    quint64 kittyBytes = 0;
    quint64 kittyStorageGeneration = 0;
    bool kittyVirtualPlacements = false;
    if (kittyGraphics != nullptr) {
        kittyPlacements = static_cast<int>(kittyGraphics->placements.size());
        kittyStorageGeneration = kittyGraphics->storageGeneration;
        kittyVirtualPlacements = kittyGraphics->containsVirtualPlacements;
        QSet<const TerminalKittyGraphicsImage *> images;
        for (const TerminalKittyGraphicsPlacement &placement :
             kittyGraphics->placements) {
            const TerminalKittyGraphicsImage *const image =
                placement.image.get();
            if (image == nullptr || images.contains(image)) continue;
            images.insert(image);
            kittyBytes += static_cast<quint64>(
                std::max<qsizetype>(0, image->straightRgba.sizeInBytes()));
        }
        kittyImages = static_cast<int>(images.size());
    }

    TerminalController *const controller = pane->controller_;
    const QQuickWindow *const window = pane->window();
    const qreal devicePixelRatio =
        window != nullptr ? window->devicePixelRatio() : 1.0;
    const std::optional<TerminalViewportLayout> layout =
        pane->currentViewportLayout();

    QVariantMap surface{
        {QStringLiteral("width"), pane->width()},
        {QStringLiteral("height"), pane->height()},
        {QStringLiteral("devicePixelRatio"), devicePixelRatio},
        {QStringLiteral("gridColumns"), columns},
        {QStringLiteral("gridRows"), rows},
        {QStringLiteral("cellWidth"), cellWidth},
        {QStringLiteral("cellHeight"), cellHeight},
        {QStringLiteral("fontFamily"), font.family()},
        {QStringLiteral("fontStyle"), font.styleName()},
        {QStringLiteral("fontPointSize"), font.pointSizeF()},
        {QStringLiteral("focused"), pane->hasActiveFocus()},
        {QStringLiteral("visible"), pane->isVisible()},
        {QStringLiteral("currentDirectory"), pane->currentDirectory()},
        {QStringLiteral("hoverColumn"), pane->hoverCell_.x()},
        {QStringLiteral("hoverRow"), pane->hoverCell_.y()},
        {QStringLiteral("hoverInside"), pane->hoverInside_},
    };
    if (layout.has_value()) {
        surface.insert(QStringLiteral("gridX"), layout->gridRect.x());
        surface.insert(QStringLiteral("gridY"), layout->gridRect.y());
        surface.insert(QStringLiteral("gridWidth"), layout->gridRect.width());
        surface.insert(QStringLiteral("gridHeight"), layout->gridRect.height());
    }

    QVariantMap terminal{
        {QStringLiteral("running"),
         controller != nullptr && controller->running()},
        {QStringLiteral("activeProcess"),
         controller != nullptr && controller->activeProcess()},
        {QStringLiteral("sessionStarted"),
         controller != nullptr && controller->sessionStarted()},
        {QStringLiteral("selectionAvailable"),
         controller != nullptr && controller->selectionAvailable()},
        {QStringLiteral("readOnly"),
         controller != nullptr && controller->readOnly()},
        {QStringLiteral("mouseTracking"),
         controller != nullptr && controller->mouseTracking()},
        {QStringLiteral("terminalMouseTracking"),
         controller != nullptr && controller->terminalMouseTracking()},
        {QStringLiteral("mouseReportingEnabled"),
         controller != nullptr && controller->mouseReportingEnabled()},
        {QStringLiteral("keyboardActionMode"),
         controller != nullptr && controller->keyboardActionMode()},
        {QStringLiteral("keyboardInputSuppressed"),
         controller != nullptr && controller->keyboardInputSuppressed()},
        {QStringLiteral("cursorVisible"), cursorVisible},
        {QStringLiteral("cursorBlinking"), cursorBlinking},
        {QStringLiteral("cursorColumn"), cursorColumn},
        {QStringLiteral("cursorRow"), cursorRow},
        {QStringLiteral("cursorStyle"), cursorStyle},
        {QStringLiteral("cursorColumnSpan"), cursorColumnSpan},
        {QStringLiteral("scrollTotal"), QVariant::fromValue(scrollTotal)},
        {QStringLiteral("scrollOffset"), QVariant::fromValue(scrollOffset)},
        {QStringLiteral("scrollLength"), QVariant::fromValue(scrollLength)},
        {QStringLiteral("contentRevision"),
         QVariant::fromValue(contentRevision)},
        {QStringLiteral("foreground"), colorName(foreground)},
        {QStringLiteral("background"), colorName(background)},
        {QStringLiteral("cursorColor"), colorName(cursorColor)},
        {QStringLiteral("palette"), paletteNames},
    };

    QVariantMap keyboard{
        {QStringLiteral("activeTables"), pane->activeKeyTables()},
        {QStringLiteral("pendingSequence"), pane->pendingKeySequence()},
        {QStringLiteral("modifiers"), modifierNames(pane->keyboardModifiers_)},
        {QStringLiteral("preedit"), pane->preedit_},
        {QStringLiteral("deferredInputCount"),
         static_cast<qlonglong>(pane->deferredInputs_.size())},
    };

    QVariantMap renderer{
        {QStringLiteral("graphicsApi"), graphicsApiName(window)},
        {QStringLiteral("customShaderStages"),
         pane->userCustomShaderStages_.size()},
        {QStringLiteral("customShaderDiagnostic"),
         pane->customShaderDiagnostic_},
        {QStringLiteral("kittyPlacements"), kittyPlacements},
        {QStringLiteral("kittyImages"), kittyImages},
        {QStringLiteral("kittyBytes"), QVariant::fromValue(kittyBytes)},
        {QStringLiteral("kittyStorageGeneration"),
         QVariant::fromValue(kittyStorageGeneration)},
        {QStringLiteral("kittyVirtualPlacements"), kittyVirtualPlacements},
    };

    terminal.insert(QStringLiteral("authoritativeStatus"),
                    terminalStatusName(terminalSnapshot_.status,
                                       pendingTerminalRequestId_ != 0));
    terminal.insert(QStringLiteral("authoritativeAvailable"),
                    terminalSnapshot_.status == TerminalInspectorStatus::Ready);
    if (terminalSnapshot_.status == TerminalInspectorStatus::Ready) {
        const TerminalInspectorSnapshot &vt = terminalSnapshot_;
        terminal.insert(QStringLiteral("workerContentRevision"),
                        QVariant::fromValue(vt.contentRevision));
        terminal.insert(QStringLiteral("activeScreen"),
                        vt.activeScreen == TerminalInspectorScreen::Alternate
                            ? QStringLiteral("Alternate")
                            : QStringLiteral("Primary"));
        terminal.insert(QStringLiteral("vtColumns"), vt.columns);
        terminal.insert(QStringLiteral("vtRows"), vt.rows);
        terminal.insert(QStringLiteral("vtWidthPixels"), vt.widthPixels);
        terminal.insert(QStringLiteral("vtHeightPixels"), vt.heightPixels);
        terminal.insert(QStringLiteral("vtCursorColumn"), vt.cursorColumn);
        terminal.insert(QStringLiteral("vtCursorRow"), vt.cursorRow);
        terminal.insert(QStringLiteral("cursorPendingWrap"),
                        vt.cursorPendingWrap);
        terminal.insert(QStringLiteral("decCursorVisible"), vt.cursorVisible);
        terminal.insert(QStringLiteral("viewportActive"), vt.viewportActive);
        terminal.insert(QStringLiteral("totalRows"),
                        QVariant::fromValue(vt.totalRows));
        terminal.insert(QStringLiteral("scrollbackRows"),
                        QVariant::fromValue(vt.scrollbackRows));
        terminal.insert(QStringLiteral("scrollTotal"),
                        QVariant::fromValue(vt.scrollTotal));
        terminal.insert(QStringLiteral("scrollOffset"),
                        QVariant::fromValue(vt.scrollOffset));
        terminal.insert(QStringLiteral("scrollLength"),
                        QVariant::fromValue(vt.scrollLength));
        terminal.insert(QStringLiteral("terminalMouseTracking"),
                        vt.terminalMouseTracking);
        terminal.insert(QStringLiteral("effectiveForeground"),
                        optionalColorName(vt.effectiveForeground));
        terminal.insert(QStringLiteral("effectiveBackground"),
                        optionalColorName(vt.effectiveBackground));
        terminal.insert(QStringLiteral("effectiveCursor"),
                        optionalColorName(vt.effectiveCursor));
        terminal.insert(QStringLiteral("defaultForeground"),
                        optionalColorName(vt.defaultForeground));
        terminal.insert(QStringLiteral("defaultBackground"),
                        optionalColorName(vt.defaultBackground));
        terminal.insert(QStringLiteral("defaultCursor"),
                        optionalColorName(vt.defaultCursor));

        QVariantList effectivePalette;
        effectivePalette.reserve(vt.effectivePalette.size());
        for (const QColor &color : vt.effectivePalette) {
            effectivePalette.append(colorName(color));
        }
        terminal.insert(QStringLiteral("palette"), effectivePalette);

        QVariantList paletteDifferences;
        if (vt.effectivePalette.size() == vt.defaultPalette.size()) {
            for (qsizetype index = 0; index < vt.effectivePalette.size();
                 ++index) {
                if (vt.effectivePalette.at(index)
                    != vt.defaultPalette.at(index)) {
                    paletteDifferences.append(index);
                }
            }
        }
        terminal.insert(QStringLiteral("paletteDifferences"),
                        paletteDifferences);

        QVariantList modes;
        modes.reserve(vt.modes.size());
        for (const TerminalInspectorModeState &mode : vt.modes) {
            modes.append(QVariantMap{
                {QStringLiteral("name"), mode.name},
                {QStringLiteral("number"), mode.number},
                {QStringLiteral("ansi"), mode.ansi},
                {QStringLiteral("enabled"), mode.enabled},
            });
        }
        terminal.insert(QStringLiteral("modes"), modes);

        QStringList kittyFlags;
        if ((vt.kittyKeyboardFlags & quint8{1U << 0U}) != 0) {
            kittyFlags.append(QStringLiteral("Disambiguate"));
        }
        if ((vt.kittyKeyboardFlags & quint8{1U << 1U}) != 0) {
            kittyFlags.append(QStringLiteral("Report events"));
        }
        if ((vt.kittyKeyboardFlags & quint8{1U << 2U}) != 0) {
            kittyFlags.append(QStringLiteral("Report alternates"));
        }
        if ((vt.kittyKeyboardFlags & quint8{1U << 3U}) != 0) {
            kittyFlags.append(QStringLiteral("Report all"));
        }
        if ((vt.kittyKeyboardFlags & quint8{1U << 4U}) != 0) {
            kittyFlags.append(QStringLiteral("Report associated text"));
        }
        keyboard.insert(QStringLiteral("kittyFlagsValue"),
                        vt.kittyKeyboardFlags);
        keyboard.insert(QStringLiteral("kittyFlags"), kittyFlags);

        renderer.insert(QStringLiteral("kittyProtocolAvailable"),
                        vt.kittyGraphicsAvailable);
        if (vt.kittyGraphicsAvailable) {
            renderer.insert(
                QStringLiteral("kittyStorageLimit"),
                QVariant::fromValue(vt.kittyImageStorageLimitBytes));
            renderer.insert(QStringLiteral("kittyFileMedium"),
                            vt.kittyFileMedium);
            renderer.insert(QStringLiteral("kittyTemporaryFileMedium"),
                            vt.kittyTemporaryFileMedium);
            renderer.insert(QStringLiteral("kittySharedMemoryMedium"),
                            vt.kittySharedMemoryMedium);
        }
    }

    const bool cellPicking = pane->inspectorCellPicking();
    QString cellStatus;
    if (cellPicking) {
        cellStatus = QStringLiteral("Pick a cell in the terminal");
    } else if (pendingCellRequestId_ != 0) {
        cellStatus = QStringLiteral("Requesting");
    } else if (!cellHasResult_) {
        cellStatus = QStringLiteral("Not selected");
    } else {
        cellStatus = cellStatusName(cellSnapshot_.status);
    }
    QVariantMap cell{
        {QStringLiteral("status"), cellStatus},
        {QStringLiteral("picking"), cellPicking},
        {QStringLiteral("requestPending"), pendingCellRequestId_ != 0},
        {QStringLiteral("available"),
         cellHasResult_
             && cellSnapshot_.status == TerminalInspectorCellStatus::Ready},
    };
    if (cellSnapshot_.viewportColumn >= 0 && cellSnapshot_.viewportRow >= 0) {
        cell.insert(QStringLiteral("viewportColumn"),
                    cellSnapshot_.viewportColumn);
        cell.insert(QStringLiteral("viewportRow"), cellSnapshot_.viewportRow);
        cell.insert(QStringLiteral("contentRevision"),
                    QVariant::fromValue(cellSnapshot_.contentRevision));
    }
    if (cellHasResult_
        && cellSnapshot_.status == TerminalInspectorCellStatus::Ready) {
        const TerminalInspectorCellSnapshot &raw = cellSnapshot_;
        cell.insert(QStringLiteral("activeScreen"),
                    raw.activeScreen == TerminalInspectorScreen::Alternate
                        ? QStringLiteral("Alternate")
                        : QStringLiteral("Primary"));
        cell.insert(QStringLiteral("text"), raw.text);
        QStringList codepoints;
        codepoints.reserve(raw.codepoints.size());
        for (const quint32 codepoint : raw.codepoints) {
            const int width = codepoint <= 0xffffU ? 4 : 6;
            codepoints.append(QStringLiteral("U+%1")
                                  .arg(codepoint, width, 16, QLatin1Char('0'))
                                  .toUpper());
        }
        cell.insert(QStringLiteral("codepoints"), codepoints);
        cell.insert(QStringLiteral("contentKind"),
                    cellContentName(raw.contentKind));
        cell.insert(QStringLiteral("widthRole"), cellWidthName(raw.widthRole));
        cell.insert(QStringLiteral("hasText"), raw.hasText);
        cell.insert(QStringLiteral("hasStyling"), raw.hasStyling);
        cell.insert(QStringLiteral("styleId"), raw.styleId);
        cell.insert(QStringLiteral("hasHyperlink"), raw.hasHyperlink);
        cell.insert(QStringLiteral("protectedCell"), raw.protectedCell);
        cell.insert(QStringLiteral("semantic"), cellSemanticName(raw.semantic));
        cell.insert(QStringLiteral("hyperlinkUri"),
                    diagnosticByteString(raw.hyperlinkUri));
        cell.insert(QStringLiteral("contentBackground"),
                    styleColorName(raw.contentBackground, u"None"));
        cell.insert(QStringLiteral("styleForeground"),
                    styleColorName(raw.style.foreground));
        cell.insert(QStringLiteral("styleBackground"),
                    styleColorName(raw.style.background));
        cell.insert(QStringLiteral("styleUnderlineColor"),
                    styleColorName(raw.style.underlineColor));
        cell.insert(QStringLiteral("underline"),
                    underlineName(raw.style.underline));
        QStringList attributes;
        if (raw.style.bold) attributes.append(QStringLiteral("Bold"));
        if (raw.style.italic) attributes.append(QStringLiteral("Italic"));
        if (raw.style.faint) attributes.append(QStringLiteral("Faint"));
        if (raw.style.blink) attributes.append(QStringLiteral("Blink"));
        if (raw.style.inverse) attributes.append(QStringLiteral("Inverse"));
        if (raw.style.invisible) {
            attributes.append(QStringLiteral("Invisible"));
        }
        if (raw.style.strikethrough) {
            attributes.append(QStringLiteral("Strikethrough"));
        }
        if (raw.style.overline) {
            attributes.append(QStringLiteral("Overline"));
        }
        cell.insert(QStringLiteral("styleAttributes"), attributes);
        cell.insert(QStringLiteral("rowWrapped"), raw.rowWrapped);
        cell.insert(QStringLiteral("rowWrapContinuation"),
                    raw.rowWrapContinuation);
        cell.insert(QStringLiteral("rowSemantic"),
                    rowSemanticName(raw.rowSemantic));
    }

    QVariantMap next{
        {QStringLiteral("title"), pane->title()},
        {QStringLiteral("surface"), surface},
        {QStringLiteral("terminal"), terminal},
        {QStringLiteral("keyboard"), keyboard},
        {QStringLiteral("renderer"), renderer},
        {QStringLiteral("cell"), cell},
    };
    if (next == snapshot_) return;
    snapshot_ = std::move(next);
    Q_EMIT snapshotChanged();
}

void TerminalInspectorModel::close()
{
    if (active_ && pane_ != nullptr) pane_->closeInspector();
}

void TerminalInspectorModel::deactivate()
{
    if (!active_) return;
    active_ = false;
    refreshTimer_->stop();
    clearPendingTerminalEvent();
    pendingTerminalRequestId_ = 0;
    pendingCellRequestId_ = 0;
    const QPointer<TerminalInspectorModel> guard(this);
    if (pane_ != nullptr) {
        pane_->setInspectorKeyboardTraceCapture(false);
        if (guard == nullptr) return;
        pane_->setInspectorCellPicking(false);
    }
    if (guard != nullptr) guard->eventModel_->clear();
}
