pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    required property var uiController
    required property var tabModel
    required property int currentIndex
    signal tabActivated(var tabId)

    objectName: "tabOverview"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: parent ? Math.min(760, Math.max(320, parent.width - 32)) : 760
    height: parent ? Math.min(560, Math.max(240, parent.height - 48)) : 560
    modal: true
    focus: visible
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    visible: uiController !== null && uiController.tabOverviewVisible
    padding: 12

    function dismiss() {
        if (uiController !== null)
            uiController.closeModal()
    }

    function activate(tabId) {
        dismiss()
        Qt.callLater(function() {
            root.tabActivated(tabId)
        })
    }

    function activateCurrent() {
        const item = tabs.currentItem
        if (item !== null)
            activate(item.stableTabId)
    }

    onClosed: {
        if (uiController !== null && uiController.tabOverviewVisible)
            uiController.closeModal()
    }

    contentItem: ColumnLayout {
        spacing: 8

        Label {
            Layout.fillWidth: true
            text: qsTr("Tabs")
            font.bold: true
            font.pointSize: 14
            Accessible.role: Accessible.Heading
        }

        GridView {
            id: tabs

            Layout.fillWidth: true
            Layout.fillHeight: true
            model: root.tabModel
            clip: true
            focus: root.visible
            currentIndex: root.currentIndex
            cellWidth: Math.max(180, width / Math.max(1,
                Math.floor(width / 220)))
            cellHeight: 112
            keyNavigationEnabled: true

            delegate: ItemDelegate {
                id: tabDelegate

                required property int index
                required property var tabId
                required property string title
                required property string currentDirectory
                required property bool running
                required property bool attention
                required property int progress
                required property bool readOnly
                readonly property var stableTabId: tabId
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
                readonly property string accessibleDescription: {
                    const parts = []
                    if (currentDirectory.length > 0)
                        parts.push(currentDirectory)
                    if (statusText.length > 0)
                        parts.push(statusText)
                    return parts.join(". ")
                }

                width: GridView.view.cellWidth - 8
                height: GridView.view.cellHeight - 8
                highlighted: GridView.isCurrentItem
                Accessible.name: title
                Accessible.description: accessibleDescription

                contentItem: ColumnLayout {
                    spacing: 3

                    Label {
                        Layout.fillWidth: true
                        text: tabDelegate.title
                        font.bold: tabDelegate.attention
                                   || tabDelegate.GridView.isCurrentItem
                        elide: Text.ElideRight
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: tabDelegate.currentDirectory
                        elide: Text.ElideMiddle
                        opacity: 0.72
                    }

                    Label {
                        Layout.fillWidth: true
                        text: tabDelegate.statusText
                        visible: text.length > 0
                        elide: Text.ElideRight
                        opacity: 0.72
                    }
                }

                onClicked: root.activate(stableTabId)
            }

            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Return
                        || event.key === Qt.Key_Enter) {
                    root.activateCurrent()
                    event.accepted = true
                } else if (event.key === Qt.Key_Escape) {
                    root.dismiss()
                    event.accepted = true
                }
            }

            ScrollBar.vertical: ScrollBar {}

            Label {
                anchors.centerIn: parent
                visible: tabs.count === 0
                text: qsTr("No tabs")
                opacity: 0.72
            }
        }
    }

    onVisibleChanged: {
        if (visible) {
            tabs.currentIndex = currentIndex
            Qt.callLater(function() {
                if (root.visible)
                    tabs.forceActiveFocus()
            })
        }
    }
}
