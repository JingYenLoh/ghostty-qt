import QtQuick
import QtQuick.Controls
import GhosttyQt 1.0

ScrollBar {
    id: root

    required property TerminalPane terminalPane

    objectName: "terminalScrollBar"
    z: 700
    anchors.top: parent ? parent.top : undefined
    anchors.right: parent ? parent.right : undefined
    anchors.bottom: parent ? parent.bottom : undefined
    orientation: Qt.Vertical
    // Let Qt own automatic presentation while the pane remains authoritative
    // about whether this control is allowed and has a scrollable range.
    policy: terminalPane !== null && terminalPane.scrollbarVisible
            ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
    interactive: true
    focusPolicy: Qt.NoFocus
    position: terminalPane !== null ? terminalPane.scrollbarPosition : 0
    size: terminalPane !== null ? terminalPane.scrollbarSize : 1
    Accessible.name: "Terminal scrollback"

    onPositionChanged: {
        // C++ rejects authoritative and pending no-ops, which also makes
        // non-pointer changes from accessibility APIs safe to forward.
        if (terminalPane !== null)
            terminalPane.scrollbarMoveTo(position)
    }
}
