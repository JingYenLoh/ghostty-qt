import QtQuick
import QtQuick.Controls
import GhosttyQt 1.0

Rectangle {
    id: root

    required property TerminalPane terminalPane

    objectName: "terminalReadOnlyOverlay"
    z: 900
    enabled: false
    visible: terminalPane !== null && terminalPane.readOnly
    width: label.implicitWidth + 16
    height: label.implicitHeight + 10
    x: parent ? Math.max(0, parent.width - width - 8) : 0
    y: 8
    radius: 4
    color: "#cc3b2f1b"
    border.color: "#f0a43c"
    border.width: 1

    Accessible.name: "Read-only terminal"
    Accessible.role: Accessible.StaticText

    Label {
        id: label
        anchors.centerIn: parent
        text: "⊘  Read-only"
        color: "#ffd18a"
        font.bold: true
    }
}
