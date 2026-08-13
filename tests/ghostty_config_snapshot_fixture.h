#pragma once

#include "ghostty_config_export.h"
#include "ghostty_config_snapshot.h"

#include "ghostty_config_export_fixture.h"

#include <QtGlobal>

#include <type_traits>
#include <utility>

static_assert(!std::is_default_constructible_v<GhosttyConfigExport>);
static_assert(!std::is_default_constructible_v<GhosttyConfigSnapshot>);

namespace GhosttyConfigSnapshotFixture {

inline GhosttyConfigSnapshot snapshot()
{
    auto exported =
        parseGhosttyConfigExportJson(GhosttyConfigExportFixture::json());
    if (!exported) {
        qFatal("Could not parse the complete config test fixture: %s",
               qPrintable(exported.error()));
    }
    return GhosttyConfigSnapshot(std::move(*exported));
}

} // namespace GhosttyConfigSnapshotFixture
