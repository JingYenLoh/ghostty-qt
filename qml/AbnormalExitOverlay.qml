import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GhosttyQt 1.0

Pane {
    id: root

    required property TerminalPane terminalPane

    objectName: "terminalAbnormalExitOverlay"
    z: 1100
    visible: terminalPane !== null && terminalPane.abnormalExitVisible
    width: parent ? parent.width : implicitWidth
    x: 0
    y: parent ? Math.max(0, parent.height - height) : 0
    padding: 8

    Accessible.name: terminalPane !== null ? terminalPane.abnormalExitText : ""
    Accessible.role: Accessible.AlertMessage

    background: Rectangle {
        color: "#c42b1c"
        border.color: "#ffb4ab"
        border.width: 1
    }

    contentItem: RowLayout {
        spacing: 8

        Label {
            objectName: "terminalAbnormalExitMessage"
            Layout.fillWidth: true
            text: root.terminalPane !== null ? root.terminalPane.abnormalExitText : ""
            color: "#ffffff"
            font.bold: true
            wrapMode: Text.Wrap
        }

        Button {
            objectName: "terminalAbnormalExitCloseButton"
            text: qsTr("Close")
            focusPolicy: Qt.TabFocus
            Accessible.name: qsTr("Close failed terminal")
            onClicked: {
                if (root.terminalPane !== null)
                    root.terminalPane.dismissAbnormalExit();
            }
        }
    }
}
