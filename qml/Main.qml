import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GhosttyQt 1.0

ApplicationWindow {
    id: window
    property bool closeApproved: false
    // These are installed by ApplicationController before presentation. The
    // quick-terminal role remains immutable for the lifetime of this root,
    // while its window-local UI controller survives modal/toast QML objects.
    property bool quickTerminal: false
    property var uiController: null
    property int visibilityBeforeFullscreen: Window.Windowed
    property int previousVisibility: Window.Hidden
    property size initialNormalSize: Qt.size(0, 0)
    property bool initialNormalSizePending: false
    readonly property bool terminalHeaderVisible: !window.quickTerminal
                                                  || workspace.tabBarVisible
    // ApplicationController sizes the client area in terminal cells before
    // the first pane is constructed. Keep the frontend chrome contract typed
    // and explicit instead of guessing it from a partially laid-out item tree.
    readonly property real terminalChromeWidth: 0
    readonly property real terminalChromeHeight: terminalHeaderVisible
                                                  ? windowHeader.implicitHeight
                                                  : 0

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
    title: workspace.currentTitle
    palette.window: workspace.chromeBackground
    palette.windowText: workspace.chromeForeground
    palette.button: workspace.chromeBackground
    palette.buttonText: workspace.chromeForeground
    palette.base: workspace.chromeBackground
    palette.text: workspace.chromeForeground
    palette.highlight: workspace.chromeForeground
    palette.highlightedText: workspace.chromeBackground
    // Terminal panes paint their own effective background. Keep the window's
    // clear color fully transparent so those pane-local alpha values reach the
    // compositor instead of blending onto an opaque QML backing color first.
    color: "transparent"

    Component {
        id: terminalSearchOverlayFactory
        SearchOverlay {}
    }

    Component {
        id: terminalKeyStateOverlayFactory
        KeyStateOverlay {}
    }

    Component {
        id: terminalAbnormalExitOverlayFactory
        AbnormalExitOverlay {}
    }

    Component {
        id: terminalReadOnlyOverlayFactory
        ReadOnlyOverlay {}
    }

    Component {
        id: terminalResizeOverlayFactory
        ResizeOverlay {}
    }

    Component {
        id: terminalScrollBarFactory
        TerminalScrollBar {}
    }

    Component {
        id: terminalBellBorderFactory
        BellBorderOverlay {}
    }

    Component {
        id: wideTabButtonFactory
        TabButton {
            objectName: "windowTabButton"
            required property int index
            required property string title
            required property bool attention
            text: title
            font.bold: attention
            focusPolicy: Qt.NoFocus
            onClicked: workspace.setCurrentIndex(index)
        }
    }

    Component {
        id: compactTabButtonFactory
        TabButton {
            objectName: "windowTabButton"
            required property int index
            required property string title
            required property bool attention
            text: title
            font.bold: attention
            width: Math.min(Math.max(130, implicitWidth), 240)
            focusPolicy: Qt.NoFocus
            onClicked: workspace.setCurrentIndex(index)
        }
    }

    onClosing: function(close) {
        if (closeApproved) {
            close.accepted = true
            return
        }
        close.accepted = false
        workspace.requestWindowClose()
    }

    header: Rectangle {
        id: topToolbarSlot
        objectName: "topToolbarSlot"
        implicitHeight: terminalHeaderVisible
                        ? windowHeader.implicitHeight
                        : 0
        visible: terminalHeaderVisible && !workspace.tabBarAtBottom
        // The terminal viewport may be translucent, but application chrome is
        // intentionally opaque.
        color: workspace.chromeBackground
    }

    footer: Rectangle {
        id: bottomToolbarSlot
        objectName: "bottomToolbarSlot"
        implicitHeight: terminalHeaderVisible
                        ? windowHeader.implicitHeight
                        : 0
        visible: terminalHeaderVisible && workspace.tabBarAtBottom
        color: workspace.chromeBackground
    }

    ToolBar {
        id: windowHeader
        objectName: "windowToolbar"
        // Pinned Linux quick terminals hide their ordinary header bar but
        // retain the tab strip when multiple tabs make it visible.
        visible: terminalHeaderVisible
        parent: workspace.tabBarAtBottom
                ? bottomToolbarSlot
                : topToolbarSlot
        anchors.fill: parent
        position: workspace.tabBarAtBottom
                  ? ToolBar.Footer
                  : ToolBar.Header
        font.family: workspace.titleFontFamily
        background: Rectangle {
            color: workspace.chromeBackground
        }

        RowLayout {
            anchors.fill: parent
            spacing: 4

            TabBar {
                id: tabs
                objectName: "windowTabBar"
                Layout.fillWidth: true
                Layout.fillHeight: true
                // Keep layout negotiation independent from delegate widths.
                // Otherwise an equal-fill width binding feeds the TabBar's
                // implicit content width back into this RowLayout.
                Layout.minimumWidth: 0
                Layout.preferredWidth: 0
                visible: workspace.tabBarVisible
                currentIndex: workspace.currentIndex

                Repeater {
                    model: workspace.tabModel
                    delegate: workspace.wideTabs
                              ? wideTabButtonFactory
                              : compactTabButtonFactory
                }
            }

            Label {
                objectName: "windowSubtitle"
                Layout.maximumWidth: 320
                visible: !window.quickTerminal
                         && workspace.currentSubtitle.length > 0
                text: workspace.currentSubtitle
                elide: Text.ElideMiddle
                color: workspace.chromeForeground
            }

            ToolButton {
                text: "+"
                visible: !window.quickTerminal
                focusPolicy: Qt.NoFocus
                Accessible.name: "New tab"
                ToolTip.visible: hovered
                ToolTip.text: "New tab (Ctrl+Shift+T)"
                onClicked: workspace.newTab()
            }
            ToolButton {
                text: "↔"
                visible: !window.quickTerminal
                focusPolicy: Qt.NoFocus
                Accessible.name: "Split right"
                ToolTip.visible: hovered
                ToolTip.text: "Split right (Ctrl+Shift+O)"
                onClicked: workspace.splitRight()
            }
            ToolButton {
                text: "↕"
                visible: !window.quickTerminal
                focusPolicy: Qt.NoFocus
                Accessible.name: "Split down"
                ToolTip.visible: hovered
                ToolTip.text: "Split down (Ctrl+Shift+E)"
                onClicked: workspace.splitDown()
            }
            ToolButton {
                text: "×"
                visible: !window.quickTerminal
                focusPolicy: Qt.NoFocus
                Accessible.name: "Close pane"
                ToolTip.visible: hovered
                ToolTip.text: "Close pane"
                onClicked: workspace.closeActivePane()
            }
        }
    }

    TerminalWorkspace {
        id: workspace
        anchors.fill: parent
        searchOverlayComponent: terminalSearchOverlayFactory
        keyStateOverlayComponent: terminalKeyStateOverlayFactory
        abnormalExitOverlayComponent: terminalAbnormalExitOverlayFactory
        readOnlyOverlayComponent: terminalReadOnlyOverlayFactory
        resizeOverlayComponent: terminalResizeOverlayFactory
        scrollbarComponent: terminalScrollBarFactory
        bellBorderComponent: terminalBellBorderFactory
        onWindowAttentionRequested: window.alert(0)
    }

    CommandPalette {
        id: commandPalette
        uiController: window.uiController
    }

    TabOverview {
        id: tabOverview
        uiController: window.uiController
        tabModel: workspace.tabModel
        currentIndex: workspace.currentIndex
        onTabActivated: function(tabId) {
            workspace.activateTabByStableId(tabId)
        }
    }

    ConfigDiagnosticsDialog {
        id: configurationDiagnosticsDialog
        uiController: window.uiController
    }

    AppToast {
        id: applicationToast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        uiController: window.uiController
    }

    Menu {
        id: terminalContextMenu
        property var requestId: 0
        property var closingRequestId: 0
        property point requestPosition: Qt.point(0, 0)
        property bool selectionAvailable: false
        property string pendingAction: ""
        objectName: "terminalContextMenu"
        popupType: Popup.Item

        function scheduleOpen(requestId) {
            Qt.callLater(function() {
                if (terminalContextMenu.requestId !== requestId
                        || terminalContextMenu.visible
                        || terminalContextMenu.closingRequestId !== 0)
                    return
                const popupPosition = window.contentItem.mapFromItem(
                                                null,
                                                terminalContextMenu.requestPosition)
                terminalContextMenu.popup(window.contentItem,
                                          popupPosition.x,
                                          popupPosition.y)
            })
        }

        function showFor(requestId, position, selectionAvailable) {
            terminalContextMenu.requestId = requestId
            terminalContextMenu.requestPosition = position
            terminalContextMenu.selectionAvailable = selectionAvailable
            terminalContextMenu.pendingAction = ""
            if (!terminalContextMenu.visible
                    && terminalContextMenu.closingRequestId === 0)
                terminalContextMenu.scheduleOpen(requestId)
        }

        function cancel(requestId) {
            if (terminalContextMenu.requestId !== requestId)
                return
            terminalContextMenu.requestId = 0
            terminalContextMenu.pendingAction = ""
            if (terminalContextMenu.visible) {
                if (terminalContextMenu.closingRequestId === 0)
                    terminalContextMenu.closingRequestId = requestId
                terminalContextMenu.close()
            }
        }

        Action {
            objectName: "terminalContextMenuCopy"
            text: qsTr("Copy")
            enabled: terminalContextMenu.selectionAvailable
            onTriggered: terminalContextMenu.pendingAction =
                         "copy_to_clipboard:mixed"
        }

        Action {
            objectName: "terminalContextMenuPaste"
            text: qsTr("Paste")
            onTriggered: terminalContextMenu.pendingAction =
                         "paste_from_clipboard"
        }

        MenuSeparator {
        }

        Action {
            objectName: "terminalContextMenuReset"
            text: qsTr("Reset")
            onTriggered: terminalContextMenu.pendingAction = "reset"
        }

        MenuSeparator {
        }

        Menu {
            objectName: "terminalContextMenuSplit"
            title: qsTr("Split")

            Action {
                objectName: "terminalContextMenuChangeTitle"
                text: qsTr("Change Title…")
                onTriggered: terminalContextMenu.pendingAction =
                             "prompt_surface_title"
            }

            Action {
                objectName: "terminalContextMenuSplitUp"
                text: qsTr("Split Up")
                onTriggered: terminalContextMenu.pendingAction =
                             "new_split:up"
            }

            Action {
                objectName: "terminalContextMenuSplitDown"
                text: qsTr("Split Down")
                onTriggered: terminalContextMenu.pendingAction =
                             "new_split:down"
            }

            Action {
                objectName: "terminalContextMenuSplitLeft"
                text: qsTr("Split Left")
                onTriggered: terminalContextMenu.pendingAction =
                             "new_split:left"
            }

            Action {
                objectName: "terminalContextMenuSplitRight"
                text: qsTr("Split Right")
                onTriggered: terminalContextMenu.pendingAction =
                             "new_split:right"
            }

            Action {
                objectName: "terminalContextMenuCloseSplit"
                text: qsTr("Close Split")
                onTriggered: terminalContextMenu.pendingAction =
                             "close_surface"
            }
        }

        Menu {
            objectName: "terminalContextMenuTab"
            title: qsTr("Tab")

            Action {
                objectName: "terminalContextMenuChangeTabTitle"
                text: qsTr("Change Tab Title…")
                onTriggered: terminalContextMenu.pendingAction =
                             "prompt_tab_title"
            }

            Action {
                objectName: "terminalContextMenuNewTab"
                text: qsTr("New Tab")
                onTriggered: terminalContextMenu.pendingAction = "new_tab"
            }

            Action {
                objectName: "terminalContextMenuCloseTab"
                text: qsTr("Close Tab")
                onTriggered: terminalContextMenu.pendingAction =
                             "close_tab:this"
            }
        }

        Menu {
            objectName: "terminalContextMenuWindow"
            title: qsTr("Window")

            Action {
                objectName: "terminalContextMenuNewWindow"
                text: qsTr("New Window")
                onTriggered: terminalContextMenu.pendingAction = "new_window"
            }

            Action {
                objectName: "terminalContextMenuCloseWindow"
                text: qsTr("Close Window")
                onTriggered: terminalContextMenu.pendingAction =
                             "close_window"
            }
        }

        MenuSeparator {
        }

        Menu {
            objectName: "terminalContextMenuConfig"
            title: qsTr("Config")

            Action {
                objectName: "terminalContextMenuOpenConfig"
                text: qsTr("Open Configuration")
                onTriggered: terminalContextMenu.pendingAction =
                             "open_config"
            }

            Action {
                objectName: "terminalContextMenuReloadConfig"
                text: qsTr("Reload Configuration")
                onTriggered: terminalContextMenu.pendingAction =
                             "reload_config"
            }
        }

        onClosed: {
            const cancelledClose = closingRequestId !== 0
            const finishedRequestId = cancelledClose
                                      ? closingRequestId
                                      : requestId
            const selectedAction = cancelledClose ? "" : pendingAction
            closingRequestId = 0
            if (!cancelledClose) {
                requestId = 0
                pendingAction = ""
            }
            if (finishedRequestId !== 0) {
                Qt.callLater(function() {
                    if (selectedAction.length !== 0)
                        workspace.executeContextMenuAction(finishedRequestId,
                                                           selectedAction)
                    else
                        workspace.finishContextMenu(finishedRequestId)
                })
            }
            if (requestId !== 0)
                terminalContextMenu.scheduleOpen(requestId)
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
        id: clipboardWriteDialog
        property var confirmationId: 0
        objectName: "clipboardWriteDialog"
        anchors.centerIn: parent
        width: Math.max(340, Math.min(600, window.width - 32))
        modal: true
        title: "Authorize Clipboard Access"
        onAccepted: {
            const acceptedId = confirmationId
            confirmationId = 0
            workspace.confirmClipboardWrite(acceptedId,
                                            rememberClipboardChoice.checked)
        }
        onRejected: {
            const rejectedId = confirmationId
            confirmationId = 0
            workspace.cancelClipboardWrite(rejectedId,
                                           rememberClipboardChoice.checked)
        }

        footer: DialogButtonBox {
            Button {
                text: "Deny"
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
            Button {
                text: "Allow"
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
        }

        ColumnLayout {
            width: clipboardWriteDialog.availableWidth

            Label {
                Layout.fillWidth: true
                text: "An application is attempting to write to the clipboard. The content to write is shown below."
                wrapMode: Text.WordWrap
            }

            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 170

                TextArea {
                    id: clipboardWritePreview
                    readOnly: true
                    wrapMode: TextEdit.WrapAnywhere
                    font.family: "monospace"
                }
            }

            CheckBox {
                id: rememberClipboardChoice
                objectName: "rememberClipboardChoice"
                text: "Remember choice for this split"
            }

            Label {
                Layout.fillWidth: true
                text: "Reload configuration to show this prompt again."
                wrapMode: Text.WordWrap
                opacity: 0.75
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
        function onTerminalClipboardWriteConfirmationRequested(
                confirmationId, preview) {
            clipboardWriteDialog.confirmationId = confirmationId
            clipboardWritePreview.text = preview
            rememberClipboardChoice.checked = false
            clipboardWriteDialog.open()
        }
        function onTerminalClipboardWriteConfirmationResolved(confirmationId) {
            if (clipboardWriteDialog.confirmationId !== confirmationId)
                return
            clipboardWriteDialog.confirmationId = 0
            clipboardWriteDialog.close()
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
        function onContextMenuRequested(requestId, windowPosition,
                                        selectionAvailable) {
            terminalContextMenu.showFor(requestId, windowPosition,
                                        selectionAvailable)
        }
        function onContextMenuCancelled(requestId) {
            terminalContextMenu.cancel(requestId)
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
