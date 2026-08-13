pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    required property var uiController

    objectName: "commandPalette"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: parent ? Math.min(680, Math.max(320, parent.width - 32)) : 680
    height: parent ? Math.min(520, Math.max(240, parent.height - 48)) : 520
    modal: true
    focus: visible
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    visible: uiController !== null && uiController.commandPaletteVisible
    padding: 12

    function resetAndFocus() {
        if (!visible || uiController === null)
            return;
        searchField.text = "";
        uiController.commandPaletteModel.filter = "";
        Qt.callLater(function () {
            if (root.visible)
                searchField.forceActiveFocus();
        });
    }

    function dismiss() {
        if (uiController !== null)
            uiController.closeModal();
    }

    function activateSelected() {
        if (uiController === null)
            return;
        if (uiController.commandPaletteModel.selectedIndex < 0)
            return;
        dismiss();
        // Popup teardown and focus restoration complete before C++ resolves
        // the captured typed command.
        Qt.callLater(function () {
            if (root.uiController !== null)
                root.uiController.activateSelectedCommand();
        });
    }

    onClosed: {
        if (uiController !== null && uiController.commandPaletteVisible)
            uiController.closeModal();
    }

    contentItem: ColumnLayout {
        spacing: 8

        TextField {
            id: searchField

            Layout.fillWidth: true
            placeholderText: qsTr("Search commands")
            selectByMouse: true
            Accessible.name: qsTr("Search commands")

            onTextEdited: {
                if (root.uiController !== null)
                    root.uiController.commandPaletteModel.filter = text;
            }

            Keys.onPressed: function (event) {
                if (root.uiController === null)
                    return;
                if (event.key === Qt.Key_Down) {
                    root.uiController.commandPaletteModel.selectRelative(1);
                    event.accepted = true;
                } else if (event.key === Qt.Key_Up) {
                    root.uiController.commandPaletteModel.selectRelative(-1);
                    event.accepted = true;
                } else if (event.key === Qt.Key_PageDown) {
                    root.uiController.commandPaletteModel.selectRelative(8);
                    event.accepted = true;
                } else if (event.key === Qt.Key_PageUp) {
                    root.uiController.commandPaletteModel.selectRelative(-8);
                    event.accepted = true;
                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    root.activateSelected();
                    event.accepted = true;
                } else if (event.key === Qt.Key_Escape) {
                    root.dismiss();
                    event.accepted = true;
                }
            }
        }

        ListView {
            id: resultList

            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            focus: false
            model: root.uiController !== null ? root.uiController.commandPaletteModel : null
            currentIndex: root.uiController !== null ? root.uiController.commandPaletteModel.selectedIndex : -1

            delegate: ItemDelegate {
                id: commandDelegate

                required property int index
                required property string title
                required property string description

                width: ListView.view.width
                highlighted: ListView.isCurrentItem
                Accessible.name: title
                Accessible.description: description

                contentItem: ColumnLayout {
                    spacing: 1

                    Label {
                        Layout.fillWidth: true
                        text: commandDelegate.title
                        elide: Text.ElideRight
                        font.bold: commandDelegate.ListView.isCurrentItem
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: commandDelegate.description
                        elide: Text.ElideRight
                        opacity: 0.72
                    }
                }

                onClicked: {
                    root.uiController.commandPaletteModel.selectedIndex = index;
                    root.activateSelected();
                }
            }

            ScrollBar.vertical: ScrollBar {}

            Label {
                anchors.centerIn: parent
                visible: resultList.count === 0
                text: qsTr("No matching commands")
                opacity: 0.72
            }
        }
    }

    onVisibleChanged: {
        if (visible)
            resetAndFocus();
    }
}
