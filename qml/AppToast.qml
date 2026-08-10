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
    radius: 6
    color: toastLabel.palette.toolTipBase
    border.color: toastLabel.palette.mid
    border.width: 1

    Label {
        id: toastLabel

        anchors.centerIn: parent
        width: Math.min(implicitWidth, root.width - 28)
        text: root.uiController !== null
              ? root.uiController.toastMessage
              : ""
        color: palette.toolTipText
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
            // Read the controller directly: when it is attached after QML
            // construction, this handler can run before root.visible's
            // binding has reevaluated for the same notification signal.
            if (root.uiController !== null
                    && root.uiController.toastVisible)
                expiryTimer.restart()
        }
    }

    Component.onCompleted: {
        if (root.uiController !== null
                && root.uiController.toastVisible)
            expiryTimer.start()
    }

    Accessible.name: toastLabel.text
    Accessible.role: Accessible.AlertMessage
}
