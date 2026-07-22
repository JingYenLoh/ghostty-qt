import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GhosttyQt 1.0

ApplicationWindow {
    id: window
    property bool closeApproved: false
    property int visibilityBeforeFullscreen: Window.Windowed
    property int previousVisibility: Window.Hidden
    property size initialNormalSize: Qt.size(0, 0)
    property bool initialNormalSizePending: false
    // ApplicationController sizes the client area in terminal cells before
    // the first pane is constructed. Keep the frontend chrome contract typed
    // and explicit instead of guessing it from a partially laid-out item tree.
    readonly property real terminalChromeWidth: 0
    readonly property real terminalChromeHeight: windowHeader.implicitHeight

    function toggleFullscreen() {
        if (visibility === Window.FullScreen) {
            visibility = visibilityBeforeFullscreen
        } else {
            visibilityBeforeFullscreen = visibility
            visibility = Window.FullScreen
        }
    }

    function toggleMaximize() {
        if (visibility === Window.FullScreen) {
            visibilityBeforeFullscreen =
                    visibilityBeforeFullscreen === Window.Maximized
                    ? Window.Windowed
                    : Window.Maximized
        } else {
            visibility = visibility === Window.Maximized
                       ? Window.Windowed
                       : Window.Maximized
        }
    }

    onVisibilityChanged: {
        const prior = previousVisibility
        previousVisibility = window.visibility
        if (prior === Window.FullScreen
                && window.visibility === Window.Windowed
                && visibilityBeforeFullscreen !== Window.Windowed) {
            // A compositor-side fullscreen exit bypasses toggleFullscreen().
            // Restore the state captured before fullscreen just as the
            // application action does.
            window.visibility = visibilityBeforeFullscreen
            return
        }
        if (window.visibility === Window.Windowed
                && initialNormalSizePending) {
            // A root first mapped maximized/fullscreen has no compositor-owned
            // normal geometry yet. Restore the hidden pre-map size once; all
            // later normal geometry belongs to Qt and the compositor.
            initialNormalSizePending = false
            window.width = initialNormalSize.width
            window.height = initialNormalSize.height
        }
    }

    width: 1100
    height: 720
    // The process controller presents the window only after its workspace,
    // lifetime tracking, process actions, and retirement wiring are complete.
    visible: false
    title: workspace.currentTitle.length > 0
           ? workspace.currentTitle + " — ghostty-qt"
           : "ghostty-qt"
    color: "#1e222a"

    Component {
        id: terminalSearchOverlayFactory
        SearchOverlay {}
    }

    Component {
        id: terminalReadOnlyOverlayFactory
        ReadOnlyOverlay {}
    }

    onClosing: function(close) {
        if (closeApproved) {
            close.accepted = true
            return
        }
        close.accepted = false
        workspace.requestWindowClose()
    }

    header: ToolBar {
        id: windowHeader
        objectName: "windowToolbar"

        RowLayout {
            anchors.fill: parent
            spacing: 4

            TabBar {
                id: tabs
                objectName: "windowTabBar"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: workspace.tabBarVisible
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
            readOnlyOverlayComponent: terminalReadOnlyOverlayFactory
        }
    }

    Dialog {
        id: closeDialog
        property var confirmationId: 0
        objectName: "closeDialog"
        anchors.centerIn: parent
        width: Math.max(280, Math.min(460, window.width - 32))
        modal: true
        title: "Confirm close"
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: workspace.confirmClose(confirmationId)
        onRejected: workspace.cancelClose(confirmationId)

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

    Dialog {
        id: titleDialog
        objectName: "titleDialog"
        property var promptId: 0
        anchors.centerIn: parent
        width: Math.max(320, Math.min(520, window.width - 32))
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: {
            titleField.forceActiveFocus()
            titleField.cursorPosition = titleField.length
        }
        onAccepted: {
            const acceptedPromptId = promptId
            promptId = 0
            workspace.confirmTitlePrompt(acceptedPromptId, titleField.text)
        }
        onRejected: {
            const rejectedPromptId = promptId
            promptId = 0
            workspace.cancelTitlePrompt(rejectedPromptId)
        }

        ColumnLayout {
            width: titleDialog.availableWidth

            Label {
                Layout.fillWidth: true
                text: "Leave blank to restore the default title."
                wrapMode: Text.WordWrap
            }

            TextField {
                id: titleField
                objectName: "titleField"
                Layout.fillWidth: true
                selectByMouse: true
                onAccepted: titleDialog.accept()
            }
        }
    }

    Connections {
        target: workspace
        function onCloseConfirmationRequested(confirmationId, message) {
            closeDialog.confirmationId = confirmationId
            closeMessage.text = message
            closeDialog.open()
        }
        function onCloseConfirmationResolved(confirmationId) {
            if (closeDialog.confirmationId !== confirmationId)
                return
            closeDialog.confirmationId = 0
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
        function onTitlePromptRequested(promptId, heading, initialTitle) {
            titleDialog.promptId = promptId
            titleDialog.title = heading
            titleField.text = initialTitle
            titleDialog.open()
        }
        function onTitlePromptResolved(promptId) {
            if (titleDialog.promptId !== promptId)
                return
            titleDialog.promptId = 0
            titleDialog.close()
        }
        function onToggleFullscreenRequested() {
            window.toggleFullscreen()
        }
        function onToggleMaximizeRequested() {
            window.toggleMaximize()
        }
        function onWindowCloseApproved() {
            window.closeApproved = true
        }
    }
}
