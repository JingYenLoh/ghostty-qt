import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GhosttyQt 1.0

Pane {
    id: root

    required property TerminalPane terminalPane
    property bool activatingPane: false
    property bool alignLeft: false
    property bool alignTop: true
    property bool dragging: false
    property real dragX: 0
    property real dragY: 0

    objectName: "terminalSearchOverlay"
    z: 1000
    visible: terminalPane !== null && terminalPane.searchUiActive
    width: parent ? Math.min(implicitWidth, Math.max(0, parent.width - 16))
                  : implicitWidth
    x: dragging ? boundedX(dragX) : restingX()
    y: dragging ? boundedY(dragY) : restingY()
    padding: 6

    function boundedX(value) {
        if (parent === null)
            return 0
        return Math.max(0, Math.min(value, parent.width - width))
    }

    function boundedY(value) {
        if (parent === null)
            return 0
        return Math.max(0, Math.min(value, parent.height - height))
    }

    function restingX() {
        if (parent === null)
            return 0
        return boundedX(alignLeft ? 8 : parent.width - width - 8)
    }

    function restingY() {
        if (parent === null)
            return 0
        return boundedY(alignTop ? 8 : parent.height - height - 8)
    }

    function synchronizeText() {
        if (terminalPane === null)
            return
        if (searchField.text !== terminalPane.searchUiText)
            searchField.text = terminalPane.searchUiText
    }

    function submitText() {
        searchDebounce.stop()
        if (terminalPane !== null)
            terminalPane.setSearchUiText(searchField.text)
    }

    function focusAndSelect() {
        Qt.callLater(function() {
            if (!root.visible)
                return
            searchField.forceActiveFocus()
            searchField.selectAll()
        })
    }

    function closeSearch() {
        searchDebounce.stop()
        if (terminalPane !== null)
            terminalPane.endSearchUi()
    }

    contentItem: RowLayout {
        spacing: 4

        Item {
            id: dragHandle

            objectName: "terminalSearchDragHandle"
            Layout.preferredWidth: 18
            Layout.fillHeight: true

            Label {
                anchors.fill: parent
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: "⠿"
                Accessible.ignored: true
            }

            MouseArea {
                id: dragArea

                property point previousParentPoint

                objectName: "terminalSearchDragArea"
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                preventStealing: true
                cursorShape: pressed ? Qt.ClosedHandCursor
                                     : Qt.OpenHandCursor
                Accessible.name: "Move search"

                function parentPoint(mouse) {
                    if (root.parent === null)
                        return Qt.point(0, 0)
                    return mapToItem(root.parent, mouse.x, mouse.y)
                }

                onPressed: function(mouse) {
                    root.dragX = root.x
                    root.dragY = root.y
                    previousParentPoint = parentPoint(mouse)
                    root.dragging = true
                    mouse.accepted = true
                }

                onPositionChanged: function(mouse) {
                    if (!pressed)
                        return
                    const nextParentPoint = parentPoint(mouse)
                    root.dragX = root.boundedX(
                                root.dragX + nextParentPoint.x
                                - previousParentPoint.x)
                    root.dragY = root.boundedY(
                                root.dragY + nextParentPoint.y
                                - previousParentPoint.y)
                    previousParentPoint = nextParentPoint
                }

                onReleased: function(mouse) {
                    root.alignLeft = root.dragX + root.width / 2
                            <= root.parent.width / 2
                    root.alignTop = root.dragY + root.height / 2
                            <= root.parent.height / 2
                    root.dragging = false
                    mouse.accepted = true
                }

                onCanceled: root.dragging = false
            }
        }

        TextField {
            id: searchField

            objectName: "terminalSearchField"
            Layout.preferredWidth: 220
            Layout.minimumWidth: 80
            selectByMouse: true
            placeholderText: "Find"
            Accessible.name: "Search terminal"

            onTextEdited: searchDebounce.restart()
            onActiveFocusChanged: {
                if (activeFocus && !root.activatingPane
                        && root.terminalPane !== null) {
                    // Activating a workspace pane normally returns focus to
                    // the terminal. Reclaim it once without emitting again.
                    root.activatingPane = true
                    root.terminalPane.activated(root.terminalPane)
                    forceActiveFocus()
                    root.activatingPane = false
                }
            }
            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    root.submitText()
                    root.terminalPane.navigateSearch(
                                event.modifiers & Qt.ShiftModifier ? -1 : 1)
                    event.accepted = true
                } else if (event.key === Qt.Key_Escape) {
                    root.closeSearch()
                    event.accepted = true
                }
            }
        }

        Label {
            Layout.minimumWidth: 44
            horizontalAlignment: Text.AlignRight
            text: root.terminalPane !== null
                  ? root.terminalPane.searchMatchLabel : ""
            Accessible.name: "Search match count"
        }

        ToolButton {
            text: "↑"
            focusPolicy: Qt.NoFocus
            Accessible.name: "Previous match"
            ToolTip.visible: hovered
            ToolTip.text: "Previous match (Shift+Enter)"
            onClicked: {
                root.submitText()
                root.terminalPane.navigateSearch(-1)
            }
        }

        ToolButton {
            text: "↓"
            focusPolicy: Qt.NoFocus
            Accessible.name: "Next match"
            ToolTip.visible: hovered
            ToolTip.text: "Next match (Enter)"
            onClicked: {
                root.submitText()
                root.terminalPane.navigateSearch(1)
            }
        }

        ToolButton {
            text: "×"
            focusPolicy: Qt.NoFocus
            Accessible.name: "Close search"
            ToolTip.visible: hovered
            ToolTip.text: "Close search (Escape)"
            onClicked: root.closeSearch()
        }
    }

    Timer {
        id: searchDebounce
        interval: 150
        onTriggered: {
            if (root.terminalPane !== null)
                root.terminalPane.setSearchUiText(searchField.text)
        }
    }

    Connections {
        target: root.terminalPane
        ignoreUnknownSignals: true

        function onSearchUiTextChanged() {
            root.synchronizeText()
        }

        function onSearchUiActiveChanged() {
            root.synchronizeText()
            if (root.terminalPane !== null
                    && root.terminalPane.searchUiActive)
                root.focusAndSelect()
        }

        function onSearchUiFocusRequested() {
            root.synchronizeText()
            root.focusAndSelect()
        }
    }

    Component.onCompleted: root.synchronizeText()
}
