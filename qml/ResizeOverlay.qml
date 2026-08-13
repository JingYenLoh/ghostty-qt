import QtQuick
import GhosttyQt 1.0

Rectangle {
    id: root

    required property TerminalPane terminalPane

    objectName: "terminalResizeOverlay"
    z: 800
    enabled: false
    visible: terminalPane !== null && terminalPane.resizeOverlayVisible
    x: terminalPane !== null ? terminalPane.resizeOverlayRect.x : 0
    y: terminalPane !== null ? terminalPane.resizeOverlayRect.y : 0
    width: terminalPane !== null ? terminalPane.resizeOverlayRect.width : 0
    height: terminalPane !== null ? terminalPane.resizeOverlayRect.height : 0
    radius: 6
    color: "#e62b303b"
    border.color: "#555555"
    border.width: 1

    Text {
        anchors.centerIn: parent
        text: root.terminalPane !== null ? root.terminalPane.resizeOverlayText : ""
        color: "#f2f2f2"
        font.bold: true
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    Accessible.name: terminalPane !== null ? terminalPane.resizeOverlayText : ""
    Accessible.role: Accessible.StaticText
}
