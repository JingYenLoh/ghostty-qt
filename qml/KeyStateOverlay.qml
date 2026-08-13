import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GhosttyQt 1.0

Rectangle {
    id: root

    required property TerminalPane terminalPane
    property bool alignTop: false
    property bool dragging: false
    property real dragY: 0

    readonly property bool hasTables: terminalPane !== null && terminalPane.activeKeyTables.length > 0
    readonly property bool hasSequence: terminalPane !== null && terminalPane.pendingKeySequence.length > 0

    function boundedY(value) {
        if (parent === null)
            return 0;
        return Math.max(0, Math.min(value, parent.height - height));
    }

    function restingY() {
        if (parent === null)
            return 0;
        return boundedY(alignTop ? 8 : parent.height - height - 8);
    }

    objectName: "terminalKeyStateOverlay"
    z: 950
    visible: hasTables || hasSequence
    width: content.implicitWidth + 20
    height: content.implicitHeight + 12
    x: parent ? Math.max(0, (parent.width - width) / 2) : 0
    y: dragging ? boundedY(dragY) : restingY()
    radius: 8
    color: "#e62b303b"
    border.color: "#555555"
    border.width: 1

    Accessible.name: {
        const parts = [];
        if (hasTables)
            parts.push("Key tables " + terminalPane.activeKeyTables.join(" > "));
        if (hasSequence)
            parts.push("Pending key sequence " + terminalPane.pendingKeySequence.join(" "));
        return parts.join(", ");
    }
    Accessible.role: Accessible.StaticText

    RowLayout {
        id: content

        anchors.centerIn: parent
        spacing: 6

        Label {
            text: "⌨"
            font.pixelSize: 16
            color: "#f2f2f2"
            Accessible.ignored: true
        }

        Label {
            id: tablesLabel

            objectName: "terminalKeyStateTables"
            visible: root.hasTables
            text: root.hasTables ? root.terminalPane.activeKeyTables.join(" > ") : ""
            color: "#f2f2f2"
        }

        Label {
            objectName: "terminalKeyStateChevron"
            visible: root.hasTables && root.hasSequence
            text: "›"
            color: "#a6a6a6"
            Accessible.ignored: true
        }

        Label {
            id: sequenceLabel

            objectName: "terminalKeyStateSequence"
            visible: root.hasSequence
            text: root.hasSequence ? root.terminalPane.pendingKeySequence.join(" ") : ""
            color: "#f2f2f2"
        }

        BusyIndicator {
            objectName: "terminalKeyStatePending"
            visible: root.hasSequence
            running: visible
            implicitWidth: 16
            implicitHeight: 16
            Accessible.ignored: true
        }
    }

    MouseArea {
        id: dragArea

        property real previousParentY: 0

        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor

        function parentY(mouse) {
            if (root.parent === null)
                return 0;
            return mapToItem(root.parent, mouse.x, mouse.y).y;
        }

        onPressed: function (mouse) {
            root.dragY = root.y;
            previousParentY = parentY(mouse);
            root.dragging = true;
            mouse.accepted = true;
        }

        onPositionChanged: function (mouse) {
            if (!pressed)
                return;
            const nextParentY = parentY(mouse);
            root.dragY = root.boundedY(root.dragY + nextParentY - previousParentY);
            previousParentY = nextParentY;
        }

        onReleased: function (mouse) {
            root.alignTop = root.dragY + root.height / 2 <= root.parent.height / 2;
            root.dragging = false;
            mouse.accepted = true;
        }

        onCanceled: root.dragging = false
    }
}
