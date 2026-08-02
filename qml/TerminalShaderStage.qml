import QtQuick
import GhosttyQt 1.0

Item {
    id: root

    required property bool retainedPipeline
    required property var uniformProvider
    required property real sourceDevicePixelRatio
    required property bool linearBlending
    property string fragmentShaderFileName
    property var fragmentShaderData
    property int stageIndex: 0
    property var shaderStages: []

    layer.enabled: true
    layer.live: true
    layer.smooth: true
    layer.textureMirroring: ShaderEffectSource.NoMirroring
    layer.format: linearBlending ? ShaderEffectSource.RGBA16F
                                 : ShaderEffectSource.RGBA8
    layer.textureSize: Qt.size(
        Math.max(1, Math.round(width * sourceDevicePixelRatio)),
        Math.max(1, Math.round(height * sourceDevicePixelRatio)))
    layer.effect: retainedPipeline ? retainedPipelineComponent
                                   : legacyStageComponent

    Component {
        id: retainedPipelineComponent

        TerminalCustomShaderPipelineEffect {
            shaderStages: root.shaderStages
            uniformProvider: root.uniformProvider
            linearBlending: root.linearBlending
        }
    }

    Component {
        id: legacyStageComponent

        TerminalCustomShaderEffect {
            fragmentShaderFileName: root.fragmentShaderFileName
            fragmentShaderData: root.fragmentShaderData
            uniformProvider: root.uniformProvider
            stageIndex: root.stageIndex
        }
    }
}
