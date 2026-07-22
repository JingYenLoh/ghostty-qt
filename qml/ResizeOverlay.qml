import QtQuick

Rectangle {
    id: root

    required property var terminalPane

    objectName: "terminalResizeOverlay"
    z: 800
    enabled: false
    visible: terminalPane.resizeOverlayVisible
    x: terminalPane.resizeOverlayRect.x
    y: terminalPane.resizeOverlayRect.y
    width: terminalPane.resizeOverlayRect.width
    height: terminalPane.resizeOverlayRect.height
    radius: 6
    color: "#e62b303b"
    border.color: "#555555"
    border.width: 1

    Text {
        anchors.centerIn: parent
        text: root.terminalPane.resizeOverlayText
        color: "#f2f2f2"
        font.bold: true
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    Accessible.name: terminalPane.resizeOverlayText
    Accessible.role: Accessible.StaticText
}
