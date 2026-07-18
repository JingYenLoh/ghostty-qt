import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GhosttyQt 1.0

ApplicationWindow {
    id: window
    property bool closeApproved: false
    property int visibilityBeforeFullscreen: Window.Windowed

    function toggleFullscreen() {
        if (visibility === Window.FullScreen) {
            visibility = visibilityBeforeFullscreen
        } else {
            visibilityBeforeFullscreen = visibility
            visibility = Window.FullScreen
        }
    }

    width: 1100
    height: 720
    minimumWidth: 480
    minimumHeight: 320
    visible: true
    title: workspace.currentTitle.length > 0
           ? workspace.currentTitle + " — ghostty-qt"
           : "ghostty-qt"
    color: "#1e222a"

    Component {
        id: terminalSearchOverlayFactory
        SearchOverlay {}
    }

    onClosing: function(close) {
        if (closeApproved) {
            close.accepted = true
            return
        }
        close.accepted = false
        workspace.requestQuit()
    }

    header: ToolBar {
        id: windowHeader

        RowLayout {
            anchors.fill: parent
            spacing: 4

            TabBar {
                id: tabs
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: workspace.currentIndex

                Repeater {
                    model: workspace.tabModel
                    TabButton {
                        required property int index
                        required property string title
                        required property bool zoomed
                        text: (zoomed ? "🔍 " : "") + title
                        width: Math.min(Math.max(130, implicitWidth), 240)
                        focusPolicy: Qt.NoFocus
                        onClicked: workspace.setCurrentIndex(index)
                    }
                }
            }

            ToolButton {
                text: "+"
                focusPolicy: Qt.NoFocus
                Accessible.name: "New tab"
                ToolTip.visible: hovered
                ToolTip.text: "New tab (Ctrl+Shift+T)"
                onClicked: workspace.newTab()
            }
            ToolButton {
                text: "↔"
                focusPolicy: Qt.NoFocus
                Accessible.name: "Split right"
                ToolTip.visible: hovered
                ToolTip.text: "Split right (Ctrl+Shift+O)"
                onClicked: workspace.splitRight()
            }
            ToolButton {
                text: "↕"
                focusPolicy: Qt.NoFocus
                Accessible.name: "Split down"
                ToolTip.visible: hovered
                ToolTip.text: "Split down (Ctrl+Shift+E)"
                onClicked: workspace.splitDown()
            }
            ToolButton {
                text: "×"
                focusPolicy: Qt.NoFocus
                Accessible.name: "Close pane"
                ToolTip.visible: hovered
                ToolTip.text: "Close pane"
                onClicked: workspace.closeActivePane()
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#3b4252"

        TerminalWorkspace {
            id: workspace
            anchors.fill: parent
            searchOverlayComponent: terminalSearchOverlayFactory
        }
    }

    Dialog {
        id: closeDialog
        objectName: "closeDialog"
        anchors.centerIn: parent
        width: Math.max(280, Math.min(460, window.width - 32))
        modal: true
        title: "Confirm close"
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: workspace.confirmClose()
        onRejected: workspace.cancelClose()

        Label {
            id: closeMessage
            width: closeDialog.availableWidth
            wrapMode: Text.WordWrap
        }
    }

    Dialog {
        id: pasteDialog
        property var confirmationId: 0
        anchors.centerIn: parent
        width: Math.max(320, Math.min(560, window.width - 32))
        modal: true
        title: "Paste text containing a command break?"
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: workspace.confirmPaste(confirmationId)
        onRejected: workspace.cancelPaste(confirmationId)

        ColumnLayout {
            width: pasteDialog.availableWidth
            Label {
                Layout.fillWidth: true
                text: "The clipboard contains a newline or bracketed-paste terminator. Review it before pasting."
                wrapMode: Text.WordWrap
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 150
                TextArea {
                    id: pastePreview
                    readOnly: true
                    wrapMode: TextEdit.WrapAnywhere
                    font.family: "monospace"
                }
            }
        }
    }

    Connections {
        target: workspace
        function onCloseConfirmationRequested(message) {
            closeMessage.text = message
            closeDialog.open()
        }
        function onCloseConfirmationResolved() {
            closeDialog.close()
        }
        function onUnsafePasteConfirmationRequested(confirmationId, preview) {
            pasteDialog.confirmationId = confirmationId
            pastePreview.text = preview
            pasteDialog.open()
        }
        function onUnsafePasteConfirmationResolved(confirmationId) {
            if (pasteDialog.confirmationId !== confirmationId)
                return
            pasteDialog.confirmationId = 0
            pasteDialog.close()
        }
        function onToggleFullscreenRequested() {
            window.toggleFullscreen()
        }
        function onQuitApproved() {
            window.closeApproved = true
            Qt.callLater(window.close)
        }
    }
}
