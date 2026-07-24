import QtQuick
import GhosttyQt 1.0

Rectangle {
    id: root

    required property TerminalPane terminalPane

    objectName: "terminalBellBorder"
    z: 600
    anchors.fill: parent
    enabled: false
    visible: opacity > 0
    opacity: terminalPane !== null && terminalPane.bellBorderVisible ? 1 : 0
    color: "transparent"
    border.color: "#803a944a"
    border.width: 3

    Behavior on opacity {
        NumberAnimation {
            duration: 500
        }
    }
}
