import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GhosttyQt 1.0

ApplicationWindow {
    id: window
    width: 1100
    height: 720
    minimumWidth: 480
    minimumHeight: 320
    visible: true
    title: workspace.tabTitles.length > 0
           ? workspace.tabTitles[workspace.currentIndex] + " — ghostty-qt"
           : "ghostty-qt"
    color: "#1e222a"

    onClosing: function(close) {
        close.accepted = false
        workspace.requestQuit()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 38
            color: "#2e3440"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 5
                anchors.rightMargin: 5
                spacing: 4

                TabBar {
                    id: tabs
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: workspace.currentIndex
                    background: Item {}

                    Repeater {
                        model: workspace.tabTitles
                        TabButton {
                            required property int index
                            required property string modelData
                            text: modelData
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
                    ToolTip.text: "Split right (Ctrl+Shift+E)"
                    onClicked: workspace.splitRight()
                }
                ToolButton {
                    text: "↕"
                    focusPolicy: Qt.NoFocus
                    Accessible.name: "Split down"
                    ToolTip.visible: hovered
                    ToolTip.text: "Split down (Ctrl+Shift+O)"
                    onClicked: workspace.splitDown()
                }
                ToolButton {
                    text: "×"
                    focusPolicy: Qt.NoFocus
                    Accessible.name: "Close pane"
                    ToolTip.visible: hovered
                    ToolTip.text: "Close pane (Ctrl+Shift+W)"
                    onClicked: workspace.closeActivePane()
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#3b4252"

            TerminalWorkspace {
                id: workspace
                anchors.fill: parent
            }
        }
    }

    Dialog {
        id: closeDialog
        anchors.centerIn: parent
        modal: true
        title: "Confirm close"
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: workspace.confirmClose()
        onRejected: workspace.cancelClose()

        Label {
            id: closeMessage
            width: Math.min(420, window.width - 80)
            wrapMode: Text.WordWrap
        }
    }

    Dialog {
        id: pasteDialog
        anchors.centerIn: parent
        modal: true
        title: "Paste text containing a command break?"
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: workspace.confirmPaste()
        onRejected: workspace.cancelPaste()

        ColumnLayout {
            width: Math.min(520, window.width - 80)
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
        function onUnsafePasteConfirmationRequested(preview) {
            pastePreview.text = preview
            pasteDialog.open()
        }
        function onQuitApproved() {
            Qt.quit()
        }
    }
}
