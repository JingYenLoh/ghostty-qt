#include "terminal_inspector_model.h"

#include "terminal_controller.h"
#include "terminal_kitty_graphics.h"
#include "terminal_pane.h"

#include <QColor>
#include <QFont>
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

} // namespace

TerminalInspectorModel::TerminalInspectorModel(TerminalPane *pane)
    : QObject(pane)
    , pane_(pane)
    , refreshTimer_(new QTimer(this))
{
    refreshTimer_->setInterval(250);
    refreshTimer_->setTimerType(Qt::CoarseTimer);
    connect(refreshTimer_, &QTimer::timeout, this,
            &TerminalInspectorModel::refresh);
    if (pane != nullptr && pane->controller_ != nullptr) {
        connect(pane->controller_,
                &TerminalController::terminalInspectorSnapshotReady, this,
                [this](quint64 requestId,
                       const TerminalInspectorSnapshot &snapshot) {
                    if (requestId == 0
                        || requestId != pendingTerminalRequestId_) {
                        return;
                    }
                    pendingTerminalRequestId_ = 0;
                    terminalSnapshot_ = snapshot;
                    rebuildSnapshot();
                });
    }
    refresh();
    refreshTimer_->start();
}

void TerminalInspectorModel::refresh()
{
    rebuildSnapshot();
    if (pendingTerminalRequestId_ != 0) return;

    TerminalPane *const pane = pane_.data();
    if (pane == nullptr || pane->controller_ == nullptr) return;
    pendingTerminalRequestId_ =
        pane->controller_->requestTerminalInspectorSnapshot();
    if (pendingTerminalRequestId_ != 0
        && terminalSnapshot_.status == TerminalInspectorStatus::Unavailable) {
        rebuildSnapshot();
    }
}

void TerminalInspectorModel::rebuildSnapshot()
{
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
         pane->customShaderStages_.size()},
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

    QVariantMap next{
        {QStringLiteral("title"), pane->title()},
        {QStringLiteral("surface"), surface},
        {QStringLiteral("terminal"), terminal},
        {QStringLiteral("keyboard"), keyboard},
        {QStringLiteral("renderer"), renderer},
    };
    if (next == snapshot_) return;
    snapshot_ = std::move(next);
    Q_EMIT snapshotChanged();
}

void TerminalInspectorModel::close()
{
    if (pane_ != nullptr) pane_->closeInspector();
}

void TerminalInspectorModel::deactivate()
{
    refreshTimer_->stop();
    pendingTerminalRequestId_ = 0;
}
