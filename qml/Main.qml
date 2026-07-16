import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GhosttyQt 1.0

ApplicationWindow {
    id: window
    property bool closeApproved: false

    width: 1100
    height: 720
    minimumWidth: 480
    minimumHeight: 320
    visible: true
    title: workspace.currentTitle.length > 0
           ? workspace.currentTitle + " — ghostty-qt"
           : "ghostty-qt"
    color: "#1e222a"

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
                        text: title
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
        }
    }

    Dialog {
        id: closeDialog
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
        anchors.centerIn: parent
        width: Math.max(320, Math.min(560, window.width - 32))
        modal: true
        title: "Paste text containing a command break?"
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: workspace.confirmPaste()
        onRejected: workspace.cancelPaste()

        ColumnLayout {
            width: pasteDialog.availableWidth
            Label {
                Layout.fillWidth: true
                text: "The clipboard contains a newline or terminal control sequence. Review it before pasting."
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
        function onUnsafePasteConfirmationRequested(preview) {
            pastePreview.text = preview
            pasteDialog.open()
        }
        function onQuitApproved() {
            window.closeApproved = true
            Qt.callLater(window.close)
        }
    }
}
