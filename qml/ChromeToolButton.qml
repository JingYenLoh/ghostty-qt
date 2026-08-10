import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A style-owned toolbar button with a deliberately non-expanding layout
// contract. Qt 6.8 and newer may derive an expanding size policy from the
// active Quick Controls style, which is useful for ordinary forms but makes a
// trailing toolbar action consume all available width.
ToolButton {
    id: root

    required property string accessibleName
    property string toolTipText: accessibleName

    display: AbstractButton.IconOnly
    // Keep action labels out of style-owned mnemonic generation. Some desktop
    // styles derive an implicit Alt+letter shortcut from ToolButton.text even
    // when the button is icon-only. The accessible name and tooltip retain the
    // human-readable label.
    text: ""
    focusPolicy: Qt.NoFocus
    hoverEnabled: true

    icon.width: 16
    icon.height: 16
    // Flat tool buttons are painted directly over the surrounding Window
    // surface. Keep monochrome theme icons and bundled fallbacks on that
    // surface's foreground role; ButtonText may legitimately target a
    // contrasting, non-flat Button background instead.
    icon.color: root.flat ? palette.windowText : palette.buttonText

    Layout.fillWidth: false
    Layout.fillHeight: false
    Layout.alignment: Qt.AlignVCenter
    Layout.preferredWidth: Math.max(implicitWidth, implicitHeight)
    Layout.preferredHeight: implicitHeight
    Layout.maximumWidth: Layout.preferredWidth
    Layout.maximumHeight: Layout.preferredHeight

    Accessible.name: accessibleName
    ToolTip.delay: 500
    ToolTip.visible: hovered && enabled && toolTipText.length > 0
    ToolTip.text: toolTipText
}
