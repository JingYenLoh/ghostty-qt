import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GhosttyQt 1.0

ApplicationWindow {
    id: window

    SystemPalette {
        id: platformPalette
        colorGroup: SystemPalette.Active
    }

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
    palette.button: workspace.platformChromePalette
                    ? platformPalette.button
                    : workspace.chromeBackground
    palette.buttonText: workspace.platformChromePalette
                        ? platformPalette.buttonText
                        : workspace.chromeForeground
    palette.base: workspace.platformChromePalette
                  ? platformPalette.base
                  : workspace.chromeBackground
    palette.alternateBase: workspace.platformChromePalette
                           ? platformPalette.alternateBase
                           : workspace.chromeBackground
    palette.text: workspace.platformChromePalette
                  ? platformPalette.text
                  : workspace.chromeForeground
    // Platform mode preserves KDE's distinct Window, Button, and Base roles.
    // Forced and Ghostty themes replace only the neutral chrome roles; the
    // platform still owns accent, selection, and other semantic colors.
    // Terminal panes paint their own effective background. Keep the window's
    // clear color fully transparent so those pane-local alpha values reach the
    // compositor instead of blending onto an opaque QML backing color first.
    color: "transparent"

    Action {
        id: newTabAction
        text: qsTr("New Tab")
        icon.name: "tab-new"
        icon.source: Qt.resolvedUrl("icons/tab-new.svg")
        enabled: !window.quickTerminal
        onTriggered: workspace.newTab()
    }

    Action {
        id: splitRightAction
        text: qsTr("Split Right")
        icon.name: "view-split-left-right"
        icon.source: Qt.resolvedUrl("icons/split-right.svg")
        enabled: !window.quickTerminal && workspace.tabCount > 0
        onTriggered: workspace.splitRight()
    }

    Action {
        id: splitDownAction
        text: qsTr("Split Down")
        icon.name: "view-split-top-bottom"
        icon.source: Qt.resolvedUrl("icons/split-down.svg")
        enabled: !window.quickTerminal && workspace.tabCount > 0
        onTriggered: workspace.splitDown()
    }

    Action {
        id: closePaneAction
        text: qsTr("Close Pane")
        icon.name: "window-close"
        icon.source: Qt.resolvedUrl("icons/window-close.svg")
        enabled: !window.quickTerminal && workspace.tabCount > 0
        onTriggered: workspace.closeActivePane()
    }

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
        id: terminalProgressOverlayFactory
        ProgressOverlay {}
    }

    Component {
        id: terminalScrollBarFactory
        TerminalScrollBar {}
    }

    Component {
        id: terminalCustomShaderStageFactory
        TerminalShaderStage {}
    }

    Component {
        id: terminalBellBorderFactory
        BellBorderOverlay {}
    }

    Component {
        id: terminalInspectorFactory
        TerminalInspector {}
    }

    Component {
        id: wideTabButtonFactory
        TabButton {
            objectName: "windowTabButton"
            required property int index
            required property string title
            required property string currentDirectory
            required property bool running
            required property bool attention
            required property int progress
            required property bool readOnly
            readonly property string statusText: {
                const states = []
                if (running)
                    states.push(qsTr("Running"))
                if (readOnly)
                    states.push(qsTr("Read only"))
                if (progress >= 0)
                    states.push(qsTr("%1% complete").arg(progress))
                return states.join(" · ")
            }
            text: title
            font.bold: attention
            focusPolicy: Qt.NoFocus
            Accessible.description: statusText
            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: {
                const lines = [title]
                if (currentDirectory.length > 0)
                    lines.push(currentDirectory)
                if (statusText.length > 0)
                    lines.push(statusText)
                return lines.join("\n")
            }
            onClicked: workspace.setCurrentIndex(index)
        }
    }

    Component {
        id: compactTabButtonFactory
        TabButton {
            objectName: "windowTabButton"
            required property int index
            required property string title
            required property string currentDirectory
            required property bool running
            required property bool attention
            required property int progress
            required property bool readOnly
            readonly property string statusText: {
                const states = []
                if (running)
                    states.push(qsTr("Running"))
                if (readOnly)
                    states.push(qsTr("Read only"))
                if (progress >= 0)
                    states.push(qsTr("%1% complete").arg(progress))
                return states.join(" · ")
            }
            text: title
            font.bold: attention
            width: Math.min(Math.max(130, implicitWidth), 240)
            focusPolicy: Qt.NoFocus
            Accessible.description: statusText
            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: {
                const lines = [title]
                if (currentDirectory.length > 0)
                    lines.push(currentDirectory)
                if (statusText.length > 0)
                    lines.push(statusText)
                return lines.join("\n")
            }
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
        implicitHeight: window.terminalHeaderVisible
                        ? windowHeader.implicitHeight
                        : 0
        visible: window.terminalHeaderVisible && !workspace.tabBarAtBottom
        // The terminal viewport may be translucent, but application chrome is
        // intentionally opaque.
        color: workspace.chromeBackground
    }

    footer: Rectangle {
        id: bottomToolbarSlot
        objectName: "bottomToolbarSlot"
        implicitHeight: window.terminalHeaderVisible
                        ? windowHeader.implicitHeight
                        : 0
        visible: window.terminalHeaderVisible && workspace.tabBarAtBottom
        color: workspace.chromeBackground
    }

    ToolBar {
        id: windowHeader
        objectName: "windowToolbar"
        // Pinned Linux quick terminals hide their ordinary header bar but
        // retain the tab strip when multiple tabs make it visible.
        visible: window.terminalHeaderVisible
        parent: workspace.tabBarAtBottom
                ? bottomToolbarSlot
                : topToolbarSlot
        anchors.fill: parent
        position: workspace.tabBarAtBottom
                  ? ToolBar.Footer
                  : ToolBar.Header
        font.family: workspace.titleFontFamily
        contentItem: RowLayout {
            id: toolbarLayout
            objectName: "windowToolbarLayout"
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

            Item {
                // An invisible TabBar is removed from layout negotiation.
                // Keep one and only one expanding item so the action cluster
                // remains naturally sized at the trailing edge.
                visible: !tabs.visible
                Layout.fillWidth: true
                Layout.minimumWidth: 0
            }

            Label {
                id: windowSubtitle
                objectName: "windowSubtitle"
                Layout.maximumWidth: 320
                visible: !window.quickTerminal
                         && workspace.currentSubtitle.length > 0
                text: workspace.currentSubtitle
                elide: Text.ElideMiddle
                color: workspace.chromeForeground
            }

            ToolSeparator {
                visible: !window.quickTerminal
                         && (tabs.visible || windowSubtitle.visible)
                orientation: Qt.Vertical
                Layout.fillHeight: true
            }

            RowLayout {
                id: toolbarActions
                objectName: "windowToolbarActions"
                visible: !window.quickTerminal
                spacing: 0
                Layout.fillWidth: false
                Layout.fillHeight: false
                Layout.alignment: Qt.AlignVCenter

                ChromeToolButton {
                    objectName: "windowNewTabButton"
                    action: newTabAction
                    accessibleName: newTabAction.text
                    toolTipText: qsTr("New Tab")
                }

                ChromeToolButton {
                    objectName: "windowSplitRightButton"
                    action: splitRightAction
                    accessibleName: splitRightAction.text
                    toolTipText: qsTr("Split Right")
                }

                ChromeToolButton {
                    objectName: "windowSplitDownButton"
                    action: splitDownAction
                    accessibleName: splitDownAction.text
                    toolTipText: qsTr("Split Down")
                }

                ChromeToolButton {
                    objectName: "windowClosePaneButton"
                    action: closePaneAction
                    accessibleName: closePaneAction.text
                    toolTipText: qsTr("Close Pane")
                }
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
        progressOverlayComponent: terminalProgressOverlayFactory
        scrollbarComponent: terminalScrollBarFactory
        bellBorderComponent: terminalBellBorderFactory
        inspectorComponent: terminalInspectorFactory
        customShaderStageComponent: terminalCustomShaderStageFactory
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
            icon.name: "edit-copy"
            enabled: terminalContextMenu.selectionAvailable
            onTriggered: terminalContextMenu.pendingAction =
                         "copy_to_clipboard:mixed"
        }

        Action {
            objectName: "terminalContextMenuPaste"
            text: qsTr("Paste")
            icon.name: "edit-paste"
            onTriggered: terminalContextMenu.pendingAction =
                         "paste_from_clipboard"
        }

        MenuSeparator {
        }

        Action {
            objectName: "terminalContextMenuReset"
            text: qsTr("Reset Terminal")
            icon.name: "view-refresh"
            onTriggered: terminalContextMenu.pendingAction = "reset"
        }

        MenuSeparator {
        }

        Menu {
            objectName: "terminalContextMenuSplit"
            title: qsTr("Pane")
            icon.name: "view-split-left-right"

            Action {
                objectName: "terminalContextMenuChangeTitle"
                text: qsTr("Change Title…")
                icon.name: "edit-rename"
                onTriggered: terminalContextMenu.pendingAction =
                             "prompt_surface_title"
            }

            Action {
                objectName: "terminalContextMenuSplitUp"
                text: qsTr("Split Up")
                icon.name: "view-split-top-bottom"
                onTriggered: terminalContextMenu.pendingAction =
                             "new_split:up"
            }

            Action {
                objectName: "terminalContextMenuSplitDown"
                text: qsTr("Split Down")
                icon.name: "view-split-top-bottom"
                onTriggered: terminalContextMenu.pendingAction =
                             "new_split:down"
            }

            Action {
                objectName: "terminalContextMenuSplitLeft"
                text: qsTr("Split Left")
                icon.name: "view-split-left-right"
                onTriggered: terminalContextMenu.pendingAction =
                             "new_split:left"
            }

            Action {
                objectName: "terminalContextMenuSplitRight"
                text: qsTr("Split Right")
                icon.name: "view-split-left-right"
                onTriggered: terminalContextMenu.pendingAction =
                             "new_split:right"
            }

            MenuSeparator {
            }

            Action {
                objectName: "terminalContextMenuCloseSplit"
                text: qsTr("Close Pane")
                icon.name: "window-close"
                onTriggered: terminalContextMenu.pendingAction =
                             "close_surface"
            }
        }

        Menu {
            objectName: "terminalContextMenuTab"
            title: qsTr("Tab")
            icon.name: "tab-new"

            Action {
                objectName: "terminalContextMenuNewTab"
                text: qsTr("New Tab")
                icon.name: "tab-new"
                onTriggered: terminalContextMenu.pendingAction = "new_tab"
            }

            Action {
                objectName: "terminalContextMenuChangeTabTitle"
                text: qsTr("Change Tab Title…")
                icon.name: "edit-rename"
                onTriggered: terminalContextMenu.pendingAction =
                             "prompt_tab_title"
            }

            MenuSeparator {
            }

            Action {
                objectName: "terminalContextMenuCloseTab"
                text: qsTr("Close Tab")
                icon.name: "window-close"
                onTriggered: terminalContextMenu.pendingAction =
                             "close_tab:this"
            }
        }

        Menu {
            objectName: "terminalContextMenuWindow"
            title: qsTr("Window")
            icon.name: "window-new"

            Action {
                objectName: "terminalContextMenuNewWindow"
                text: qsTr("New Window")
                icon.name: "window-new"
                onTriggered: terminalContextMenu.pendingAction = "new_window"
            }

            Action {
                objectName: "terminalContextMenuCloseWindow"
                text: qsTr("Close Window")
                icon.name: "window-close"
                onTriggered: terminalContextMenu.pendingAction =
                             "close_window"
            }
        }

        MenuSeparator {
        }

        Menu {
            objectName: "terminalContextMenuConfig"
            title: qsTr("Configuration")
            icon.name: "configure"

            Action {
                objectName: "terminalContextMenuOpenConfig"
                text: qsTr("Open Configuration")
                icon.name: "document-open"
                onTriggered: terminalContextMenu.pendingAction =
                             "open_config"
            }

            Action {
                objectName: "terminalContextMenuReloadConfig"
                text: qsTr("Reload Configuration")
                icon.name: "view-refresh"
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
        title: qsTr("Confirm Close")
        onAccepted: workspace.confirmClose(confirmationId)
        onRejected: workspace.cancelClose(confirmationId)

        footer: DialogButtonBox {
            standardButtons: DialogButtonBox.Cancel | DialogButtonBox.Ok
            Component.onCompleted: {
                const closeButton = standardButton(DialogButtonBox.Ok)
                if (closeButton !== null)
                    closeButton.text = qsTr("Close")
            }
        }

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
        title: qsTr("Confirm Paste")
        onAccepted: workspace.confirmPaste(confirmationId)
        onRejected: workspace.cancelPaste(confirmationId)

        footer: DialogButtonBox {
            standardButtons: DialogButtonBox.Cancel | DialogButtonBox.Ok
            Component.onCompleted: {
                const pasteButton = standardButton(DialogButtonBox.Ok)
                if (pasteButton !== null)
                    pasteButton.text = qsTr("Paste")
            }
        }

        ColumnLayout {
            width: pasteDialog.availableWidth
            Label {
                Layout.fillWidth: true
                text: qsTr("The clipboard contains a newline or bracketed-paste terminator. Review it before pasting.")
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
        title: qsTr("Authorize Clipboard Access")
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
            standardButtons: DialogButtonBox.Cancel | DialogButtonBox.Ok
            Component.onCompleted: {
                const denyButton = standardButton(DialogButtonBox.Cancel)
                if (denyButton !== null)
                    denyButton.text = qsTr("Deny")
                const allowButton = standardButton(DialogButtonBox.Ok)
                if (allowButton !== null)
                    allowButton.text = qsTr("Allow")
            }
        }

        ColumnLayout {
            width: clipboardWriteDialog.availableWidth

            Label {
                Layout.fillWidth: true
                text: qsTr("An application is attempting to write to the clipboard. The content to write is shown below.")
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
                text: qsTr("Remember choice for this pane")
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Reload the configuration to show this prompt again.")
                wrapMode: Text.WordWrap
                opacity: 0.75
            }
        }
    }

    Dialog {
        id: titleDialog
        objectName: "titleDialog"
        property var promptId: 0
        property var focusReturnTarget: null
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
        onClosed: {
            const target = focusReturnTarget
            focusReturnTarget = null
            if (target)
                target.forceActiveFocus()
        }

        ColumnLayout {
            width: titleDialog.availableWidth

            Label {
                Layout.fillWidth: true
                text: qsTr("Leave blank to restore the default title.")
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
            // Popup focus restoration differs between Qt versions. Preserve
            // the exact pre-dialog item so broad title actions never infer a
            // pane target or disturb the active tab/split on close.
            titleDialog.focusReturnTarget = window.activeFocusItem
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
