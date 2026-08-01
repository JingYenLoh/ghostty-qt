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

    readonly property var inspectorModel: terminalPane !== null
                                          ? terminalPane.inspectorModel : null
    readonly property var snapshot: inspectorModel !== null
                                    && inspectorModel.snapshot !== undefined
                                    ? inspectorModel.snapshot : ({})
    readonly property var surface: snapshot.surface || ({})
    readonly property var terminal: snapshot.terminal || ({})
    readonly property var keyboard: snapshot.keyboard || ({})
    readonly property var renderer: snapshot.renderer || ({})

    objectName: "terminalInspectorHost"
    width: 0
    height: 0
    visible: false

    function display(value, emptyText) {
        const fallback = emptyText === undefined ? "\u2014" : emptyText
        if (value === undefined || value === null || value === "")
            return fallback
        if (typeof value === "boolean")
            return value ? qsTr("Yes") : qsTr("No")
        if (value.join !== undefined)
            return value.length > 0 ? value.join(", ") : fallback
        return String(value)
    }

    function dimensions(width, height, unit) {
        if (width === undefined || height === undefined)
            return "\u2014"
        return width + " \u00d7 " + height + (unit ? " " + unit : "")
    }

    function position(column, row) {
        if (column === undefined || row === undefined
                || column < 0 || row < 0)
            return qsTr("Outside grid")
        return column + ", " + row
    }

    function bytes(value) {
        if (value === undefined || value === null || value < 0)
            return "\u2014"
        if (value < 1024)
            return value + " B"
        if (value < 1024 * 1024)
            return (value / 1024).toFixed(1) + " KiB"
        if (value < 1024 * 1024 * 1024)
            return (value / (1024 * 1024)).toFixed(1) + " MiB"
        return (value / (1024 * 1024 * 1024)).toFixed(1) + " GiB"
    }

    function refresh() {
        if (inspectorModel !== null)
            inspectorModel.refresh()
    }

    function closeInspector() {
        if (inspectorModel !== null)
            inspectorModel.close()
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
            font.family: parent.monospace ? "monospace"
                                          : inspectorWindow.font.family
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
        visible: host.terminalPane !== null
                 && host.terminalPane.inspectorVisible
        title: {
            const paneTitle = host.display(host.snapshot.title, "")
            return paneTitle.length > 0
                    ? qsTr("Terminal Inspector \u2014 %1").arg(paneTitle)
                    : qsTr("Terminal Inspector")
        }

        onVisibleChanged: {
            if (visible)
                host.refresh()
        }

        // The C++ model is authoritative for visibility. Rejecting the native
        // close first preserves that binding; close() then hides the window by
        // changing TerminalPane::inspectorVisible.
        onClosing: function(close) {
            close.accepted = false
            host.closeInspector()
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
                        text: host.display(host.surface.currentDirectory,
                                           qsTr("No working directory"))
                        opacity: 0.68
                        elide: Text.ElideMiddle
                    }
                }

                ToolButton {
                    objectName: "terminalInspectorRefresh"
                    text: "\u21bb"
                    focusPolicy: Qt.NoFocus
                    Accessible.name: qsTr("Refresh inspector")
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Refresh")
                    onClicked: host.refresh()
                }

                ToolButton {
                    objectName: "terminalInspectorClose"
                    text: "\u00d7"
                    focusPolicy: Qt.NoFocus
                    Accessible.name: qsTr("Close inspector")
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Close")
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

                TabButton { text: qsTr("Surface") }
                TabButton { text: qsTr("Terminal") }
                TabButton { text: qsTr("Keyboard") }
                TabButton { text: qsTr("Renderer") }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: inspectorTabs.currentIndex

                InspectorPage {
                    SectionHeading { text: qsTr("Geometry") }
                    InspectorRow {
                        name: qsTr("Pane size")
                        value: host.dimensions(host.surface.width,
                                               host.surface.height,
                                               qsTr("logical px"))
                    }
                    InspectorRow {
                        name: qsTr("Device pixel ratio")
                        value: host.display(host.surface.devicePixelRatio)
                    }
                    InspectorRow {
                        name: qsTr("Terminal grid")
                        value: host.dimensions(host.surface.gridColumns,
                                               host.surface.gridRows,
                                               qsTr("cells"))
                    }
                    InspectorRow {
                        name: qsTr("Cell size")
                        value: host.dimensions(host.surface.cellWidth,
                                               host.surface.cellHeight,
                                               qsTr("logical px"))
                    }
                    InspectorRow {
                        name: qsTr("Pointer cell")
                        value: host.position(host.surface.hoverColumn,
                                             host.surface.hoverRow)
                    }

                    SectionHeading { text: qsTr("Font") }
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

                    SectionHeading { text: qsTr("Surface state") }
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
                    SectionHeading { text: qsTr("Process") }
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

                    SectionHeading { text: qsTr("Cursor") }
                    InspectorRow {
                        name: qsTr("Visible")
                        value: host.display(host.terminal.cursorVisible)
                    }
                    InspectorRow {
                        name: qsTr("Blinking")
                        value: host.display(host.terminal.cursorBlinking)
                    }
                    InspectorRow {
                        name: qsTr("Position")
                        value: host.position(host.terminal.cursorColumn,
                                             host.terminal.cursorRow)
                    }
                    InspectorRow {
                        name: qsTr("Style")
                        value: host.display(host.terminal.cursorStyle)
                    }
                    InspectorRow {
                        name: qsTr("Column span")
                        value: host.display(host.terminal.cursorColumnSpan)
                    }

                    SectionHeading { text: qsTr("Viewport") }
                    InspectorRow {
                        name: qsTr("Scrollback rows")
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

                    SectionHeading { text: qsTr("Mouse modes") }
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
                }

                InspectorPage {
                    SectionHeading { text: qsTr("Keyboard protocol") }
                    InspectorRow {
                        name: qsTr("Action mode")
                        value: host.display(host.terminal.keyboardActionMode)
                    }
                    InspectorRow {
                        name: qsTr("Input suppressed")
                        value: host.display(host.terminal.keyboardInputSuppressed)
                    }
                    InspectorRow {
                        name: qsTr("Held modifiers")
                        value: host.display(host.keyboard.modifiers,
                                            qsTr("None"))
                    }
                    InspectorRow {
                        name: qsTr("Preedit")
                        value: host.display(host.keyboard.preedit,
                                            qsTr("None"))
                    }
                    InspectorRow {
                        name: qsTr("Deferred input events")
                        value: host.display(host.keyboard.deferredInputCount)
                    }

                    SectionHeading { text: qsTr("Keybindings") }
                    InspectorRow {
                        name: qsTr("Active tables")
                        value: host.display(host.keyboard.activeTables,
                                            qsTr("None"))
                    }
                    InspectorRow {
                        name: qsTr("Pending sequence")
                        value: host.display(host.keyboard.pendingSequence,
                                            qsTr("None"))
                        monospace: true
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.topMargin: 14
                        text: qsTr("Raw VT parser and encoded keyboard event streams are not exposed by libghostty-vt.")
                        wrapMode: Text.WordWrap
                        opacity: 0.68
                    }
                }

                InspectorPage {
                    SectionHeading { text: qsTr("Scene graph") }
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
                        value: host.display(host.renderer.customShaderDiagnostic,
                                            qsTr("None"))
                        monospace: true
                    }

                    SectionHeading { text: qsTr("Kitty graphics") }
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

                    SectionHeading { text: qsTr("Terminal colors") }
                    InspectorRow {
                        name: qsTr("Foreground")
                        value: host.display(host.terminal.foreground)
                        monospace: true
                    }
                    InspectorRow {
                        name: qsTr("Background")
                        value: host.display(host.terminal.background)
                        monospace: true
                    }
                    InspectorRow {
                        name: qsTr("Cursor")
                        value: host.display(host.terminal.cursorColor)
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
                                    Accessible.name: qsTr("Palette color %1: %2")
                                                     .arg(paletteDelegate.index)
                                                     .arg(paletteDelegate.modelData)
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
            onActivated: host.closeInspector()
        }
    }

}
