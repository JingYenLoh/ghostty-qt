import QtQuick
import GhosttyQt 1.0

Item {
    id: root

    required property TerminalPane terminalPane

    readonly property bool determinate: terminalPane !== null
                                        && !terminalPane.progressIndeterminate
    readonly property real value: determinate ? terminalPane.progressValue : 0
    readonly property real minimumValue: 0
    readonly property real maximumValue: 100
    readonly property real stepSize: 1
    readonly property color progressColor: terminalPane !== null
                                           && terminalPane.progressError
                                           ? "#e05252"
                                           : "#4b9cff"
    readonly property string accessibleDescription: {
        if (terminalPane === null)
            return ""
        if (terminalPane.progressError)
            return determinate
                ? "Operation failed at " + value + " percent"
                : "Operation failed"
        if (terminalPane.progressPaused)
            return determinate
                ? "Operation paused at " + value + " percent"
                : "Operation paused"
        if (!determinate)
            return "Operation in progress"
        return value + " percent complete"
    }

    objectName: "terminalProgressOverlay"
    z: 750
    anchors.top: parent ? parent.top : undefined
    anchors.left: parent ? parent.left : undefined
    anchors.right: parent ? parent.right : undefined
    height: 2
    clip: true
    visible: terminalPane !== null && terminalPane.progressVisible

    Rectangle {
        anchors.fill: parent
        color: root.progressColor
        opacity: 0.3
    }

    Rectangle {
        id: indicator

        x: root.terminalPane !== null
           && root.terminalPane.progressIndeterminate
           ? Math.max(0, root.width - width)
             * root.terminalPane.progressActivityPosition
           : 0
        width: root.terminalPane !== null
               && root.terminalPane.progressIndeterminate
               ? root.width * 0.25
               : root.width * Math.max(0, Math.min(
                   100, root.terminalPane !== null
                        ? root.terminalPane.progressValue : 0)) / 100
        height: root.height
        color: root.progressColor

        Behavior on x {
            enabled: root.visible && root.terminalPane !== null
                     && root.terminalPane.progressIndeterminate
                     && !root.terminalPane.progressPaused
            NumberAnimation {
                duration: 200
                easing.type: Easing.InOutQuad
            }
        }

        Behavior on width {
            enabled: root.terminalPane !== null
                     && !root.terminalPane.progressIndeterminate
            NumberAnimation {
                duration: 200
                easing.type: Easing.InOutQuad
            }
        }
    }

    Accessible.role: Accessible.ProgressBar
    Accessible.readOnly: true
    Accessible.name: terminalPane !== null && terminalPane.progressError
                     ? "Terminal progress - Error"
                     : terminalPane !== null && terminalPane.progressPaused
                       ? "Terminal progress - Paused"
                       : terminalPane !== null
                         && terminalPane.progressIndeterminate
                         ? "Terminal progress - In progress"
                         : "Terminal progress"
    Accessible.description: accessibleDescription
}
