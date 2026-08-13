pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GhosttyQt 1.0

// TerminalWorkspace owns this zero-sized host alongside its TerminalPane. The
// actual inspector is a separate, non-modal window so inspecting a surface
// never steals space from the terminal grid. Keeping both objects in one QML
// component also makes pane teardown destroy its inspector window atomically.
Item {
    id: host

    required property TerminalPane terminalPane

    readonly property var inspectorModel: terminalPane !== null ? terminalPane.inspectorModel : null
    readonly property var snapshot: inspectorModel !== null && inspectorModel.snapshot !== undefined ? inspectorModel.snapshot : ({})
    readonly property var eventModel: inspectorModel !== null && inspectorModel.eventModel !== undefined ? inspectorModel.eventModel : null
    readonly property var surface: snapshot.surface || ({})
    readonly property var terminal: snapshot.terminal || ({})
    readonly property var keyboard: snapshot.keyboard || ({})
    readonly property var renderer: snapshot.renderer || ({})
    readonly property var cell: snapshot.cell || ({})

    objectName: "terminalInspectorHost"
    width: 0
    height: 0
    visible: false

    function display(value, emptyText) {
        const fallback = emptyText === undefined ? "\u2014" : emptyText;
        if (value === undefined || value === null || value === "")
            return fallback;
        if (typeof value === "boolean")
            return value ? qsTr("Yes") : qsTr("No");
        if (value.join !== undefined)
            return value.length > 0 ? value.join(", ") : fallback;
        return String(value);
    }

    function dimensions(width, height, unit) {
        if (width === undefined || height === undefined)
            return "\u2014";
        return width + " \u00d7 " + height + (unit ? " " + unit : "");
    }

    function position(column, row) {
        if (column === undefined || row === undefined || column < 0 || row < 0)
            return qsTr("Outside grid");
        return column + ", " + row;
    }

    function bytes(value) {
        if (value === undefined || value === null || value < 0)
            return "\u2014";
        if (value < 1024)
            return value + " B";
        if (value < 1024 * 1024)
            return (value / 1024).toFixed(1) + " KiB";
        if (value < 1024 * 1024 * 1024)
            return (value / (1024 * 1024)).toFixed(1) + " MiB";
        return (value / (1024 * 1024 * 1024)).toFixed(1) + " GiB";
    }

    function modeCode(mode) {
        if (mode === undefined || mode === null)
            return "";
        return (mode.ansi ? "" : "?") + mode.number;
    }

    function hexByte(value) {
        if (value === undefined || value === null)
            return "\u2014";
        return "0x" + Number(value).toString(16).padStart(2, "0");
    }

    function authoritativeDisplay(value, emptyText) {
        if (!terminal.authoritativeAvailable)
            return display(terminal.authoritativeStatus);
        return display(value, emptyText);
    }

    function cellDisplay(value, emptyText) {
        if (cell.available !== true)
            return "\u2014";
        return display(value, emptyText);
    }

    function refresh() {
        if (inspectorModel !== null)
            inspectorModel.refresh();
    }

    function closeInspector() {
        if (inspectorModel !== null)
            inspectorModel.close();
    }

    function toggleCellPick() {
        if (inspectorModel === null)
            return;
        if (cell.picking)
            inspectorModel.cancelCellPick();
        else
            inspectorModel.beginCellPick();
    }

    component InspectorRow: RowLayout {
        required property string name
        property string value: "\u2014"
        property bool monospace: false

        Layout.fillWidth: true
        spacing: 12

        Label {
            Layout.preferredWidth: 190
            Layout.alignment: Qt.AlignTop
            text: parent.name
            opacity: 0.72
        }

        Label {
            Layout.fillWidth: true
            text: parent.value
            wrapMode: Text.WrapAnywhere
            font.family: parent.monospace ? "monospace" : inspectorWindow.font.family
        }
    }

    component SectionHeading: Label {
        Layout.fillWidth: true
        Layout.topMargin: 8
        font.bold: true
        font.pointSize: 11
        Accessible.role: Accessible.Heading
    }

    component InspectorPage: ScrollView {
        id: page
        default property alias contents: body.data

        clip: true
        contentWidth: availableWidth
        contentHeight: body.implicitHeight + 26
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        ColumnLayout {
            id: body
            x: 16
            y: 10
            width: Math.max(0, page.availableWidth - 32)
            spacing: 5
        }
    }

    ApplicationWindow {
        id: inspectorWindow

        objectName: "terminalInspectorWindow"
        transientParent: host.Window.window
        modality: Qt.NonModal
        width: 720
        height: 600
        minimumWidth: 520
        minimumHeight: 360
        visible: host.terminalPane !== null && host.terminalPane.inspectorVisible
        title: {
            const paneTitle = host.display(host.snapshot.title, "");
            return paneTitle.length > 0 ? qsTr("Terminal Inspector \u2014 %1").arg(paneTitle) : qsTr("Terminal Inspector");
        }

        onVisibleChanged: {
            if (visible)
                host.refresh();
        }

        // The C++ model is authoritative for visibility. Rejecting the native
        // close first preserves that binding; close() then hides the window by
        // changing TerminalPane::inspectorVisible.
        onClosing: function (close) {
            close.accepted = false;
            host.closeInspector();
        }

        header: ToolBar {
            RowLayout {
                anchors.fill: parent
                spacing: 6

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 10
                    spacing: 0

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Terminal Inspector")
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    Label {
                        Layout.fillWidth: true
                        text: host.display(host.surface.currentDirectory, qsTr("No working directory"))
                        opacity: 0.68
                        elide: Text.ElideMiddle
                    }
                }

                ChromeToolButton {
                    objectName: "terminalInspectorPickCell"
                    icon.name: "crosshairs"
                    icon.source: Qt.resolvedUrl("icons/cell-pick.svg")
                    highlighted: host.cell.picking === true
                    accessibleName: host.cell.picking ? qsTr("Cancel Cell Picking") : qsTr("Pick Terminal Cell")
                    onClicked: {
                        inspectorTabs.currentIndex = 1;
                        host.toggleCellPick();
                    }
                }

                ChromeToolButton {
                    objectName: "terminalInspectorRefresh"
                    icon.name: "view-refresh"
                    icon.source: Qt.resolvedUrl("icons/view-refresh.svg")
                    accessibleName: qsTr("Refresh Inspector")
                    onClicked: host.refresh()
                }

                ChromeToolButton {
                    objectName: "terminalInspectorClose"
                    icon.name: "window-close"
                    icon.source: Qt.resolvedUrl("icons/window-close.svg")
                    accessibleName: qsTr("Close Inspector")
                    onClicked: host.closeInspector()
                }
            }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            TabBar {
                id: inspectorTabs

                objectName: "terminalInspectorTabs"
                Layout.fillWidth: true

                TabButton {
                    text: qsTr("Surface")
                }
                TabButton {
                    text: qsTr("Cell")
                }
                TabButton {
                    text: qsTr("Terminal")
                }
                TabButton {
                    text: qsTr("Keyboard")
                }
                TabButton {
                    objectName: "terminalInspectorEventsTab"
                    text: qsTr("Events")
                }
                TabButton {
                    text: qsTr("Renderer")
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: inspectorTabs.currentIndex

                InspectorPage {
                    SectionHeading {
                        text: qsTr("Geometry")
                    }
                    InspectorRow {
                        name: qsTr("Pane size")
                        value: host.dimensions(host.surface.width, host.surface.height, qsTr("logical px"))
                    }
                    InspectorRow {
                        name: qsTr("Device pixel ratio")
                        value: host.display(host.surface.devicePixelRatio)
                    }
                    InspectorRow {
                        name: qsTr("Terminal grid")
                        value: host.dimensions(host.surface.gridColumns, host.surface.gridRows, qsTr("cells"))
                    }
                    InspectorRow {
                        name: qsTr("Cell size")
                        value: host.dimensions(host.surface.cellWidth, host.surface.cellHeight, qsTr("logical px"))
                    }
                    InspectorRow {
                        name: qsTr("Pointer cell")
                        value: host.position(host.surface.hoverColumn, host.surface.hoverRow)
                    }

                    SectionHeading {
                        text: qsTr("Font")
                    }
                    InspectorRow {
                        name: qsTr("Family")
                        value: host.display(host.surface.fontFamily)
                    }
                    InspectorRow {
                        name: qsTr("Style")
                        value: host.display(host.surface.fontStyle)
                    }
                    InspectorRow {
                        name: qsTr("Point size")
                        value: host.display(host.surface.fontPointSize)
                    }

                    SectionHeading {
                        text: qsTr("Surface state")
                    }
                    InspectorRow {
                        name: qsTr("Focused")
                        value: host.display(host.surface.focused)
                    }
                    InspectorRow {
                        name: qsTr("Visible")
                        value: host.display(host.surface.visible)
                    }
                    InspectorRow {
                        name: qsTr("Working directory")
                        value: host.display(host.surface.currentDirectory)
                        monospace: true
                    }
                }

                InspectorPage {
                    SectionHeading {
                        text: qsTr("Cell picker")
                    }
                    InspectorRow {
                        name: qsTr("Status")
                        value: host.display(host.cell.status)
                    }
                    InspectorRow {
                        name: qsTr("Viewport position")
                        value: host.position(host.cell.viewportColumn, host.cell.viewportRow)
                    }
                    InspectorRow {
                        name: qsTr("Content revision")
                        value: host.display(host.cell.contentRevision)
                    }
                    InspectorRow {
                        name: qsTr("Active screen")
                        value: host.display(host.cell.activeScreen)
                    }

                    Button {
                        text: host.cell.picking ? qsTr("Cancel picking") : qsTr("Pick cell")
                        icon.name: host.cell.picking ? "dialog-cancel" : "crosshairs"
                        onClicked: host.toggleCellPick()
                    }

                    Label {
                        Layout.fillWidth: true
                        text: host.cell.picking ? qsTr("Click a cell in the terminal. Right-click or press Escape to cancel.") : qsTr("Cell queries are one-shot and use the exact displayed viewport revision.")
                        wrapMode: Text.WordWrap
                        opacity: 0.68
                    }

                    SectionHeading {
                        text: qsTr("Content")
                    }
                    InspectorRow {
                        name: qsTr("Text")
                        value: host.cell.available ? (host.cell.hasText ? "\u201c" + host.cell.text + "\u201d" : qsTr("Empty")) : "\u2014"
                        monospace: true
                    }
                    InspectorRow {
                        name: qsTr("Codepoints")
                        value: host.cellDisplay(host.cell.codepoints, qsTr("None"))
                        monospace: true
                    }
                    InspectorRow {
                        name: qsTr("Content kind")
                        value: host.cellDisplay(host.cell.contentKind)
                    }
                    InspectorRow {
                        name: qsTr("Width role")
                        value: host.cellDisplay(host.cell.widthRole)
                    }
                    InspectorRow {
                        name: qsTr("Semantic content")
                        value: host.cellDisplay(host.cell.semantic)
                    }
                    InspectorRow {
                        name: qsTr("Protected")
                        value: host.cellDisplay(host.cell.protectedCell)
                    }
                    InspectorRow {
                        name: qsTr("Hyperlink")
                        value: host.cellDisplay(host.cell.hyperlinkUri, qsTr("None"))
                        monospace: true
                    }
                    InspectorRow {
                        name: qsTr("Content background")
                        value: host.cellDisplay(host.cell.contentBackground)
                        monospace: true
                    }

                    SectionHeading {
                        text: qsTr("Raw style")
                    }
                    InspectorRow {
                        name: qsTr("Style ID")
                        value: host.cellDisplay(host.cell.styleId)
                    }
                    InspectorRow {
                        name: qsTr("Attributes")
                        value: host.cellDisplay(host.cell.styleAttributes, qsTr("None"))
                    }
                    InspectorRow {
                        name: qsTr("Foreground source")
                        value: host.cellDisplay(host.cell.styleForeground)
                        monospace: true
                    }
                    InspectorRow {
                        name: qsTr("Background source")
                        value: host.cellDisplay(host.cell.styleBackground)
                        monospace: true
                    }
                    InspectorRow {
                        name: qsTr("Underline")
                        value: host.cellDisplay(host.cell.underline)
                    }
                    InspectorRow {
                        name: qsTr("Underline color source")
                        value: host.cellDisplay(host.cell.styleUnderlineColor)
                        monospace: true
                    }

                    SectionHeading {
                        text: qsTr("Row")
                    }
                    InspectorRow {
                        name: qsTr("Soft wrapped")
                        value: host.cellDisplay(host.cell.rowWrapped)
                    }
                    InspectorRow {
                        name: qsTr("Wrap continuation")
                        value: host.cellDisplay(host.cell.rowWrapContinuation)
                    }
                    InspectorRow {
                        name: qsTr("Semantic prompt")
                        value: host.cellDisplay(host.cell.rowSemantic)
                    }
                }

                InspectorPage {
                    SectionHeading {
                        text: qsTr("Process")
                    }
                    InspectorRow {
                        name: qsTr("Running")
                        value: host.display(host.terminal.running)
                    }
                    InspectorRow {
                        name: qsTr("Session started")
                        value: host.display(host.terminal.sessionStarted)
                    }
                    InspectorRow {
                        name: qsTr("Active process")
                        value: host.display(host.terminal.activeProcess)
                        monospace: true
                    }
                    InspectorRow {
                        name: qsTr("Selection")
                        value: host.display(host.terminal.selectionAvailable)
                    }
                    InspectorRow {
                        name: qsTr("Read only")
                        value: host.display(host.terminal.readOnly)
                    }

                    SectionHeading {
                        text: qsTr("Authoritative VT state")
                    }
                    InspectorRow {
                        name: qsTr("Snapshot")
                        value: host.display(host.terminal.authoritativeStatus)
                    }
                    InspectorRow {
                        name: qsTr("Active screen")
                        value: host.display(host.terminal.activeScreen)
                    }
                    InspectorRow {
                        name: qsTr("Terminal grid")
                        value: host.dimensions(host.terminal.vtColumns, host.terminal.vtRows, qsTr("cells"))
                    }
                    InspectorRow {
                        name: qsTr("Terminal pixels")
                        value: host.dimensions(host.terminal.vtWidthPixels, host.terminal.vtHeightPixels, qsTr("px"))
                    }
                    InspectorRow {
                        name: qsTr("Worker content revision")
                        value: host.display(host.terminal.workerContentRevision)
                    }

                    SectionHeading {
                        text: qsTr("Cursor")
                    }
                    InspectorRow {
                        name: qsTr("Rendered visible")
                        value: host.display(host.terminal.cursorVisible)
                    }
                    InspectorRow {
                        name: qsTr("DEC mode visible")
                        value: host.display(host.terminal.decCursorVisible)
                    }
                    InspectorRow {
                        name: qsTr("Blinking")
                        value: host.display(host.terminal.cursorBlinking)
                    }
                    InspectorRow {
                        name: qsTr("Rendered position")
                        value: host.position(host.terminal.cursorColumn, host.terminal.cursorRow)
                    }
                    InspectorRow {
                        name: qsTr("VT position")
                        value: host.position(host.terminal.vtCursorColumn, host.terminal.vtCursorRow)
                    }
                    InspectorRow {
                        name: qsTr("Pending wrap")
                        value: host.display(host.terminal.cursorPendingWrap)
                    }
                    InspectorRow {
                        name: qsTr("Visual shape")
                        value: host.display(host.terminal.cursorStyle)
                    }
                    InspectorRow {
                        name: qsTr("Column span")
                        value: host.display(host.terminal.cursorColumnSpan)
                    }

                    SectionHeading {
                        text: qsTr("Viewport")
                    }
                    InspectorRow {
                        name: qsTr("Pinned to active area")
                        value: host.display(host.terminal.viewportActive)
                    }
                    InspectorRow {
                        name: qsTr("Total rows")
                        value: host.display(host.terminal.totalRows)
                    }
                    InspectorRow {
                        name: qsTr("Scrollback rows")
                        value: host.display(host.terminal.scrollbackRows)
                    }
                    InspectorRow {
                        name: qsTr("Scrollbar total")
                        value: host.display(host.terminal.scrollTotal)
                    }
                    InspectorRow {
                        name: qsTr("Viewport offset")
                        value: host.display(host.terminal.scrollOffset)
                    }
                    InspectorRow {
                        name: qsTr("Viewport length")
                        value: host.display(host.terminal.scrollLength)
                    }
                    InspectorRow {
                        name: qsTr("Content revision")
                        value: host.display(host.terminal.contentRevision)
                    }

                    SectionHeading {
                        text: qsTr("Mouse modes")
                    }
                    InspectorRow {
                        name: qsTr("Tracking")
                        value: host.display(host.terminal.mouseTracking)
                    }
                    InspectorRow {
                        name: qsTr("Terminal tracking")
                        value: host.display(host.terminal.terminalMouseTracking)
                    }
                    InspectorRow {
                        name: qsTr("Reporting enabled")
                        value: host.display(host.terminal.mouseReportingEnabled)
                    }

                    SectionHeading {
                        text: qsTr("ANSI and DEC modes")
                    }
                    Repeater {
                        model: host.terminal.modes || []

                        delegate: InspectorRow {
                            required property var modelData

                            name: host.modeCode(modelData) + "  " + modelData.name
                            value: modelData.enabled ? qsTr("Set") : qsTr("Reset")
                            monospace: true
                        }
                    }
                }

                InspectorPage {
                    SectionHeading {
                        text: qsTr("Keyboard protocol")
                    }
                    InspectorRow {
                        name: qsTr("Action mode")
                        value: host.display(host.terminal.keyboardActionMode)
                    }
                    InspectorRow {
                        name: qsTr("Input suppressed")
                        value: host.display(host.terminal.keyboardInputSuppressed)
                    }
                    InspectorRow {
                        name: qsTr("Kitty flags")
                        value: host.terminal.authoritativeAvailable ? host.hexByte(host.keyboard.kittyFlagsValue) + "  " + host.display(host.keyboard.kittyFlags, qsTr("Disabled")) : host.authoritativeDisplay(undefined)
                        monospace: true
                    }
                    InspectorRow {
                        name: qsTr("Held modifiers")
                        value: host.display(host.keyboard.modifiers, qsTr("None"))
                    }
                    InspectorRow {
                        name: qsTr("Preedit")
                        value: host.display(host.keyboard.preedit, qsTr("None"))
                    }
                    InspectorRow {
                        name: qsTr("Deferred input events")
                        value: host.display(host.keyboard.deferredInputCount)
                    }

                    SectionHeading {
                        text: qsTr("Keybindings")
                    }
                    InspectorRow {
                        name: qsTr("Active tables")
                        value: host.display(host.keyboard.activeTables, qsTr("None"))
                    }
                    InspectorRow {
                        name: qsTr("Pending sequence")
                        value: host.display(host.keyboard.pendingSequence, qsTr("None"))
                        monospace: true
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.topMargin: 14
                        text: qsTr("The Events tab records root and pane keybinding decisions. Pane input that reaches the key encoder reuses its trace ID for bounded per-key outcomes; sequence tokens group staged keys. Root-consumed key events skip worker key encoding, although their actions may invoke other worker operations. Exact raw VT actions and inactive-screen internals still require upstream diagnostic APIs.")
                        wrapMode: Text.WordWrap
                        opacity: 0.68
                    }
                }

                Item {
                    id: eventsPage

                    property bool eventSelected: false
                    property string selectedSequence: ""
                    property string selectedTrace: ""
                    property string selectedSummary: ""
                    property string selectedDetails: ""

                    objectName: "terminalInspectorEventsPage"

                    function clearEventSelection() {
                        eventSelected = false;
                        selectedSequence = "";
                        selectedTrace = "";
                        selectedSummary = "";
                        selectedDetails = "";
                        eventList.currentIndex = -1;
                    }

                    function emptyMessage() {
                        if (host.eventModel === null)
                            return qsTr("Event capture is unavailable");
                        if (host.eventModel.retainedCount === 0) {
                            return host.eventModel.paused ? qsTr("Capture is paused") : qsTr("Waiting for events…");
                        }
                        return qsTr("No matching events");
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            ComboBox {
                                id: eventCategory

                                objectName: "terminalInspectorEventCategory"
                                Layout.preferredWidth: 130
                                enabled: host.eventModel !== null
                                textRole: "text"
                                valueRole: "value"
                                model: [
                                    {
                                        "text": qsTr("All events"),
                                        "value": -1
                                    },
                                    {
                                        "text": qsTr("Input"),
                                        "value": 0
                                    },
                                    {
                                        "text": qsTr("Terminal"),
                                        "value": 1
                                    },
                                    {
                                        "text": qsTr("State"),
                                        "value": 2
                                    }
                                ]
                                currentIndex: host.eventModel !== null ? host.eventModel.categoryFilter + 1 : 0
                                Accessible.name: qsTr("Event category")
                                onActivated: {
                                    if (host.eventModel !== null)
                                        host.eventModel.setCategoryFilter(currentValue);
                                }
                            }

                            TextField {
                                id: eventSearch

                                objectName: "terminalInspectorEventSearch"
                                Layout.fillWidth: true
                                enabled: host.eventModel !== null
                                text: host.eventModel !== null ? host.eventModel.filterText : ""
                                placeholderText: qsTr("Filter retained events")
                                maximumLength: 128
                                selectByMouse: true
                                Accessible.name: placeholderText
                                onTextEdited: {
                                    if (host.eventModel !== null)
                                        host.eventModel.setFilterText(text);
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Button {
                                id: eventPause

                                objectName: "terminalInspectorEventPause"
                                enabled: host.eventModel !== null
                                checkable: true
                                checked: host.eventModel !== null && host.eventModel.paused
                                text: checked ? qsTr("Resume capture") : qsTr("Pause capture")
                                Accessible.name: text
                                onToggled: {
                                    if (host.eventModel !== null)
                                        host.eventModel.setPaused(checked);
                                }
                            }

                            Button {
                                objectName: "terminalInspectorEventClear"
                                enabled: host.eventModel !== null && host.eventModel.retainedCount > 0
                                text: qsTr("Clear")
                                Accessible.name: qsTr("Clear retained events")
                                onClicked: host.eventModel.clear()
                            }

                            Label {
                                objectName: "terminalInspectorEventStatus"
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignRight
                                elide: Text.ElideLeft
                                opacity: 0.72
                                text: {
                                    if (host.eventModel === null)
                                        return qsTr("Unavailable");
                                    let status = qsTr("%1 shown · %2/%3 retained").arg(host.eventModel.count).arg(host.eventModel.retainedCount).arg(host.eventModel.capacity);
                                    if (host.eventModel.evictedCount > 0) {
                                        status += qsTr(" · %1 evicted").arg(host.eventModel.evictedCount);
                                    }
                                    if (host.eventModel.skippedWhilePaused > 0) {
                                        status += qsTr(" · %1 skipped").arg(host.eventModel.skippedWhilePaused);
                                    }
                                    if (host.eventModel.paused)
                                        status += qsTr(" · Paused");
                                    return status;
                                }
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("This stream contains keybinding decisions, pane input and worker outcomes, coalesced terminal frames, and pane state changes. K# identifies one captured key event; sequence tokens group staged keys. Within one capture generation, successfully staged keys receive per-key final rows; when an allowed sequence is flushed, its combined PTY write remains atomic. Input previews may contain typed secrets; worker rows retain the byte count attributed to the key operation—zero when no bytes were encoded—and at most a 64-byte prefix. Pausing immediately stops new keyboard traces and event payload retention; an ordered worker update clears staged diagnostic metadata, while other skipped observations may still increase the skipped count. Paste contents are omitted. Raw VT actions and private terminal-page internals are not available through public libghostty-vt.")
                            wrapMode: Text.WordWrap
                            opacity: 0.68
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumHeight: 80
                            color: inspectorWindow.palette.base
                            border.color: inspectorWindow.palette.mid
                            border.width: 1
                            radius: 3
                            clip: true

                            ListView {
                                id: eventList

                                objectName: "terminalInspectorEventList"
                                anchors.fill: parent
                                anchors.margins: 1
                                model: host.eventModel
                                clip: true
                                reuseItems: true
                                cacheBuffer: 96
                                boundsBehavior: Flickable.StopAtBounds
                                currentIndex: -1
                                focus: false

                                delegate: ItemDelegate {
                                    id: eventDelegate

                                    required property int index
                                    required property var sequence
                                    required property var traceId
                                    required property string elapsedText
                                    required property string category
                                    required property string kind
                                    required property string summary
                                    required property string details

                                    width: ListView.view.width
                                    height: 52
                                    highlighted: ListView.isCurrentItem
                                    Accessible.name: summary
                                    Accessible.description: details
                                    onClicked: {
                                        eventList.currentIndex = index;
                                        eventsPage.eventSelected = true;
                                        eventsPage.selectedSequence = String(sequence);
                                        eventsPage.selectedTrace = traceId !== 0 ? String(traceId) : "";
                                        eventsPage.selectedSummary = summary;
                                        eventsPage.selectedDetails = details;
                                    }

                                    contentItem: ColumnLayout {
                                        spacing: 1

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 7

                                            Label {
                                                text: "#" + eventDelegate.sequence
                                                font.family: "monospace"
                                                opacity: 0.68
                                            }
                                            Label {
                                                visible: eventDelegate.traceId !== 0
                                                text: "K#" + eventDelegate.traceId
                                                font.family: "monospace"
                                                opacity: 0.68
                                            }
                                            Label {
                                                text: eventDelegate.category
                                                font.bold: true
                                            }
                                            Label {
                                                Layout.fillWidth: true
                                                text: eventDelegate.kind
                                                elide: Text.ElideRight
                                                opacity: 0.78
                                            }
                                            Label {
                                                text: eventDelegate.elapsedText
                                                font.family: "monospace"
                                                opacity: 0.68
                                            }
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: eventDelegate.summary
                                            elide: Text.ElideRight
                                            font.family: "monospace"
                                        }
                                    }
                                }

                                ScrollBar.vertical: ScrollBar {}
                            }

                            Label {
                                anchors.centerIn: parent
                                visible: eventList.count === 0
                                text: eventsPage.emptyMessage()
                                opacity: 0.68
                            }
                        }

                        Frame {
                            objectName: "terminalInspectorEventDetails"
                            Layout.fillWidth: true
                            Layout.preferredHeight: visible ? 112 : 0
                            visible: eventsPage.eventSelected
                            padding: 8

                            contentItem: ColumnLayout {
                                spacing: 4

                                Label {
                                    Layout.fillWidth: true
                                    text: {
                                        if (!eventsPage.eventSelected)
                                            return "";
                                        let heading = qsTr("Event #%1 details").arg(eventsPage.selectedSequence);
                                        if (eventsPage.selectedTrace.length > 0)
                                            heading += qsTr(" · key trace #%1").arg(eventsPage.selectedTrace);
                                        return heading;
                                    }
                                    font.bold: true
                                    elide: Text.ElideRight
                                }

                                ScrollView {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                                    TextArea {
                                        objectName: "terminalInspectorEventDetailsText"
                                        readOnly: true
                                        selectByMouse: true
                                        wrapMode: TextEdit.WrapAnywhere
                                        text: {
                                            if (!eventsPage.eventSelected)
                                                return "";
                                            if (eventsPage.selectedDetails.length === 0)
                                                return eventsPage.selectedSummary;
                                            return eventsPage.selectedSummary + "\n\n" + eventsPage.selectedDetails;
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Connections {
                        target: host.eventModel

                        function onModelReset() {
                            eventsPage.clearEventSelection();
                        }

                        function onCleared() {
                            eventsPage.clearEventSelection();
                        }

                        function onEventEvicted(sequence) {
                            if (eventsPage.selectedSequence === String(sequence))
                                eventsPage.clearEventSelection();
                        }
                    }

                    Connections {
                        target: host

                        function onEventModelChanged() {
                            eventsPage.clearEventSelection();
                        }
                    }
                }

                InspectorPage {
                    SectionHeading {
                        text: qsTr("Scene graph")
                    }
                    InspectorRow {
                        name: qsTr("Graphics API")
                        value: host.display(host.renderer.graphicsApi)
                    }
                    InspectorRow {
                        name: qsTr("Custom shader stages")
                        value: host.display(host.renderer.customShaderStages)
                    }
                    InspectorRow {
                        name: qsTr("Shader diagnostic")
                        value: host.display(host.renderer.customShaderDiagnostic, qsTr("None"))
                        monospace: true
                    }

                    SectionHeading {
                        text: qsTr("Kitty graphics")
                    }
                    InspectorRow {
                        name: qsTr("Placements")
                        value: host.display(host.renderer.kittyPlacements)
                    }
                    InspectorRow {
                        name: qsTr("Virtual placements")
                        value: host.display(host.renderer.kittyVirtualPlacements)
                    }
                    InspectorRow {
                        name: qsTr("Images")
                        value: host.display(host.renderer.kittyImages)
                    }
                    InspectorRow {
                        name: qsTr("Image storage")
                        value: host.bytes(host.renderer.kittyBytes)
                    }
                    InspectorRow {
                        name: qsTr("Storage generation")
                        value: host.display(host.renderer.kittyStorageGeneration)
                    }
                    InspectorRow {
                        name: qsTr("Protocol available")
                        value: host.display(host.renderer.kittyProtocolAvailable)
                    }
                    InspectorRow {
                        name: qsTr("Configured storage limit")
                        value: host.bytes(host.renderer.kittyStorageLimit)
                    }
                    InspectorRow {
                        name: qsTr("File medium")
                        value: host.display(host.renderer.kittyFileMedium)
                    }
                    InspectorRow {
                        name: qsTr("Temporary-file medium")
                        value: host.display(host.renderer.kittyTemporaryFileMedium)
                    }
                    InspectorRow {
                        name: qsTr("Shared-memory medium")
                        value: host.display(host.renderer.kittySharedMemoryMedium)
                    }

                    SectionHeading {
                        text: qsTr("Terminal colors")
                    }
                    InspectorRow {
                        name: qsTr("Rendered foreground")
                        value: host.display(host.terminal.foreground)
                        monospace: true
                    }
                    InspectorRow {
                        name: qsTr("Rendered background")
                        value: host.display(host.terminal.background)
                        monospace: true
                    }
                    InspectorRow {
                        name: qsTr("Rendered cursor")
                        value: host.display(host.terminal.cursorColor)
                        monospace: true
                    }
                    InspectorRow {
                        name: qsTr("Effective foreground")
                        value: host.authoritativeDisplay(host.terminal.effectiveForeground, qsTr("Unset"))
                        monospace: true
                    }
                    InspectorRow {
                        name: qsTr("Default foreground")
                        value: host.authoritativeDisplay(host.terminal.defaultForeground, qsTr("Unset"))
                        monospace: true
                    }
                    InspectorRow {
                        name: qsTr("Effective background")
                        value: host.authoritativeDisplay(host.terminal.effectiveBackground, qsTr("Unset"))
                        monospace: true
                    }
                    InspectorRow {
                        name: qsTr("Default background")
                        value: host.authoritativeDisplay(host.terminal.defaultBackground, qsTr("Unset"))
                        monospace: true
                    }
                    InspectorRow {
                        name: qsTr("Effective cursor")
                        value: host.authoritativeDisplay(host.terminal.effectiveCursor, qsTr("Unset"))
                        monospace: true
                    }
                    InspectorRow {
                        name: qsTr("Default cursor")
                        value: host.authoritativeDisplay(host.terminal.defaultCursor, qsTr("Unset"))
                        monospace: true
                    }
                    InspectorRow {
                        name: qsTr("Palette entries differing from default")
                        value: host.authoritativeDisplay(host.terminal.paletteDifferences, qsTr("None"))
                        monospace: true
                    }

                    GridLayout {
                        readonly property var colors: host.terminal.palette || []

                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        columns: 8
                        columnSpacing: 8
                        rowSpacing: 8

                        Repeater {
                            model: parent.colors

                            delegate: ColumnLayout {
                                id: paletteDelegate

                                required property int index
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 2

                                Rectangle {
                                    Layout.alignment: Qt.AlignHCenter
                                    implicitWidth: 32
                                    implicitHeight: 24
                                    radius: 3
                                    color: paletteDelegate.modelData
                                    border.width: 1
                                    border.color: inspectorWindow.palette.mid
                                    Accessible.name: qsTr("Palette color %1: %2").arg(paletteDelegate.index).arg(paletteDelegate.modelData)
                                }

                                Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: paletteDelegate.index
                                    opacity: 0.68
                                    font.pixelSize: 10
                                }
                            }
                        }

                        Label {
                            Layout.columnSpan: 8
                            Layout.fillWidth: true
                            visible: parent.colors.length === 0
                            text: qsTr("Palette unavailable")
                            opacity: 0.68
                        }
                    }
                }
            }
        }

        Shortcut {
            sequences: [StandardKey.Cancel]
            enabled: inspectorWindow.visible
            onActivated: {
                if (host.cell.picking)
                    host.toggleCellPick();
                else
                    host.closeInspector();
            }
        }
    }
}
