import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GhosttyQt 1.0

Pane {
    id: root

    required property TerminalPane terminalPane
    property bool activatingPane: false

    objectName: "terminalSearchOverlay"
    z: 1000
    visible: terminalPane !== null && terminalPane.searchUiActive
    width: parent ? Math.min(implicitWidth, Math.max(0, parent.width - 16))
                  : implicitWidth
    x: parent ? Math.max(0, parent.width - width - 8) : 0
    y: 8
    padding: 6

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

        TextField {
            id: searchField

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
