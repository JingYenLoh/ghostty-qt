import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root

    required property var uiController

    objectName: "configurationDiagnosticsDialog"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: parent ? Math.min(680, Math.max(340, parent.width - 32)) : 680
    height: parent ? Math.min(460, Math.max(260, parent.height - 48)) : 460
    modal: true
    focus: visible
    closePolicy: Popup.NoAutoClose
    visible: uiController !== null
             && uiController.configurationDiagnosticsVisible
    title: "Configuration errors"

    function ignore() {
        if (uiController !== null)
            uiController.ignoreConfigurationDiagnostics()
    }

    contentItem: ColumnLayout {
        spacing: 10

        Label {
            Layout.fillWidth: true
            text: "The last valid configuration remains active. Fix the errors below, then retry."
            wrapMode: Text.WordWrap
        }

        ScrollView {
            objectName: "configurationDiagnosticsScrollView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            TextArea {
                id: diagnosticsText

                objectName: "configurationDiagnosticsText"
                text: root.uiController !== null
                      ? root.uiController.configurationDiagnosticsText : ""
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                font.family: "monospace"
                Accessible.name: "Configuration errors"
            }
        }
    }

    footer: DialogButtonBox {
        Button {
            objectName: "configurationDiagnosticsIgnore"
            text: "Ignore"
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: root.ignore()
        }

        Button {
            objectName: "configurationDiagnosticsRetry"
            text: "Retry"
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: {
                if (root.uiController !== null)
                    root.uiController.retryConfigurationDiagnostics()
            }
        }
    }

    Shortcut {
        sequences: [StandardKey.Cancel]
        enabled: root.visible
        onActivated: root.ignore()
    }
}
