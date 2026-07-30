import QtQuick
import GhosttyQt 1.0

Item {
    id: root

    required property string fragmentShaderFileName
    required property var fragmentShaderData
    required property var uniformProvider
    required property int stageIndex
    required property real sourceDevicePixelRatio

    layer.enabled: true
    layer.live: true
    layer.smooth: true
    layer.textureMirroring: ShaderEffectSource.NoMirroring
    layer.textureSize: Qt.size(
        Math.max(1, Math.round(width * sourceDevicePixelRatio)),
        Math.max(1, Math.round(height * sourceDevicePixelRatio)))
    layer.effect: Component {
        TerminalCustomShaderEffect {
            fragmentShaderFileName: root.fragmentShaderFileName
            fragmentShaderData: root.fragmentShaderData
            uniformProvider: root.uniformProvider
            stageIndex: root.stageIndex
        }
    }
}
