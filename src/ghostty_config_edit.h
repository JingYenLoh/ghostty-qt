#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

#include <expected>
#include <functional>

using GhosttyConfigUrlOpener = std::function<bool(const QUrl &)>;

// Prepare the Ghostty configuration path used by the GUI open_config action.
// Candidate paths must be supplied in edit priority order: config.ghostty,
// then the legacy config path on Linux. The first non-empty file wins; if all
// existing candidates are empty, the first existing path wins; if none exist,
// the first candidate is created without truncation.
[[nodiscard]] std::expected<QString, QString>
prepareGhosttyConfigForEditing(const QStringList &editCandidatePaths);

// Prepare and launch the selected file through the desktop. An empty opener
// uses QDesktopServices; injection keeps external launches out of tests.
[[nodiscard]] std::expected<QString, QString>
openGhosttyConfigForEditing(
    const QStringList &editCandidatePaths,
    GhosttyConfigUrlOpener opener = {});
