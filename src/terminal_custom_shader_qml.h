#pragma once

#include "terminal_custom_shader_pipeline.h"
#include "terminal_custom_shader_qsg.h"

#include <QtQmlIntegration/qqmlintegration.h>

// The renderer is a reusable static library rather than its own QML module.
// Register its effect item as a foreign type in the application module so
// qmlcachegen and the generated type registrar share one authoritative name.
struct TerminalCustomShaderEffectForeign {
    Q_GADGET
    QML_FOREIGN(TerminalCustomShaderEffect)
    QML_NAMED_ELEMENT(TerminalCustomShaderEffect)
    QML_ADDED_IN_VERSION(1, 0)
};

struct TerminalCustomShaderPipelineEffectForeign {
    Q_GADGET
    QML_FOREIGN(TerminalCustomShaderPipelineEffect)
    QML_NAMED_ELEMENT(TerminalCustomShaderPipelineEffect)
    QML_ADDED_IN_VERSION(1, 0)
};
