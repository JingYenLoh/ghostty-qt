import QtQuick
import QtQuick.Controls

Rectangle {
    id: root

    required property var uiController

    objectName: "applicationToast"
    z: 1200
    // No handlers are installed and the disabled visual item cannot become an
    // input target. Terminal mouse and keyboard input pass through unchanged.
    enabled: false
    focus: false
    visible: uiController !== null && uiController.toastVisible
    width: Math.min(implicitWidth, parent ? Math.max(0, parent.width - 32)
                                         : implicitWidth)
    height: implicitHeight
    implicitWidth: toastLabel.implicitWidth + 28
    implicitHeight: toastLabel.implicitHeight + 18
    radius: 8
    color: "#f51e222a"
    border.color: "#5d6470"
    border.width: 1

    Label {
        id: toastLabel

        anchors.centerIn: parent
        width: Math.min(implicitWidth, root.width - 28)
        text: root.uiController !== null
              ? root.uiController.toastMessage
              : ""
        color: "#f2f2f2"
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
    }

    Timer {
        id: expiryTimer

        interval: root.uiController !== null
                  ? Math.max(1,
                      root.uiController.toastDurationMilliseconds)
                  : 1
        repeat: false
        onTriggered: {
            if (root.uiController !== null)
                root.uiController.expireToast()
        }
    }

    Connections {
        target: root.uiController
        ignoreUnknownSignals: true

        function onToastChanged() {
            expiryTimer.stop()
            if (root.visible)
                expiryTimer.restart()
        }
    }

    Component.onCompleted: {
        if (visible)
            expiryTimer.start()
    }

    Accessible.name: toastLabel.text
    Accessible.role: Accessible.AlertMessage
}
