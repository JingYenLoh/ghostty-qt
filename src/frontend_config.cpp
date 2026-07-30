#include "frontend_config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStringDecoder>
#include <QStringList>

#include <optional>
#include <utility>

namespace {

QString diagnostic(QStringView sourceName, qsizetype line, QStringView message)
{
    const QString source = sourceName.isEmpty()
        ? QStringLiteral("<frontend-config>")
        : sourceName.toString();
    return QStringLiteral("%1:%2: %3").arg(source).arg(line).arg(message);
}

QString normalizedAbsolutePath(const QString &path)
{
    if (path.isEmpty()) return {};
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

std::optional<SingleInstanceMode> parseSingleInstance(QStringView value)
{
    if (value == QLatin1StringView("false")) {
        return SingleInstanceMode::Disabled;
    }
    if (value == QLatin1StringView("true")) {
        return SingleInstanceMode::Enabled;
    }
    if (value == QLatin1StringView("detect")) {
        return SingleInstanceMode::Detect;
    }
    return std::nullopt;
}

std::optional<TabsLocation> parseTabsLocation(QStringView value)
{
    if (value == QLatin1StringView("top")) return TabsLocation::Top;
    if (value == QLatin1StringView("bottom")) return TabsLocation::Bottom;
    return std::nullopt;
}

std::optional<bool> parseBoolean(QStringView value)
{
    if (value == QLatin1StringView("true")) return true;
    if (value == QLatin1StringView("false")) return false;
    return std::nullopt;
}

std::optional<QuickTerminalLayer> parseQuickTerminalLayer(QStringView value)
{
    if (value == QLatin1StringView("background")) {
        return QuickTerminalLayer::Background;
    }
    if (value == QLatin1StringView("bottom")) {
        return QuickTerminalLayer::Bottom;
    }
    if (value == QLatin1StringView("top")) return QuickTerminalLayer::Top;
    if (value == QLatin1StringView("overlay")) {
        return QuickTerminalLayer::Overlay;
    }
    return std::nullopt;
}

} // namespace

std::expected<FrontendConfigValues, QString>
parseFrontendConfig(QByteArrayView contents, QStringView sourceName)
{
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString text = decoder.decode(contents);
    if (decoder.hasError()) {
        const QString source = sourceName.isEmpty()
            ? QStringLiteral("<frontend-config>")
            : sourceName.toString();
        return std::unexpected(
            QStringLiteral("%1: configuration is not valid UTF-8").arg(source));
    }
    if (!text.isEmpty() && text.front() == u'\uFEFF') {
        text.remove(0, 1);
    }

    FrontendConfigValues values;
    QSet<QString> assignedKeys;
    const QStringList lines = text.split(u'\n');
    for (qsizetype index = 0; index < lines.size(); ++index) {
        QString line = lines.at(index);
        if (line.endsWith(u'\r')) line.chop(1);

        const qsizetype lineNumber = index + 1;
        for (const QChar character : std::as_const(line)) {
            if (character.unicode() < 0x20 && character != u'\t') {
                return std::unexpected(
                    diagnostic(sourceName, lineNumber,
                               QStringLiteral("invalid control character")));
            }
        }

        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(u'#')) continue;

        if (line.count(u'=') != 1) {
            return std::unexpected(diagnostic(
                sourceName, lineNumber,
                QStringLiteral("expected exactly one key = value assignment")));
        }

        const qsizetype separator = line.indexOf(u'=');
        const QString key = line.first(separator).trimmed();
        const QString value = line.sliced(separator + 1).trimmed();
        if (key.isEmpty()) {
            return std::unexpected(diagnostic(
                sourceName, lineNumber,
                QStringLiteral("configuration key must not be empty")));
        }
        if (value.isEmpty()) {
            return std::unexpected(diagnostic(
                sourceName, lineNumber,
                QStringLiteral("value for '%1' must not be empty").arg(key)));
        }
        if (assignedKeys.contains(key)) {
            return std::unexpected(
                diagnostic(sourceName, lineNumber,
                           QStringLiteral("duplicate key '%1'").arg(key)));
        }

        if (key == QLatin1StringView("single-instance")) {
            const std::optional<SingleInstanceMode> parsed =
                parseSingleInstance(value);
            if (!parsed) {
                return std::unexpected(diagnostic(
                    sourceName, lineNumber,
                    QStringLiteral(
                        "invalid single-instance value '%1'; expected false, true, or detect")
                        .arg(value)));
            }
            values.singleInstanceMode = *parsed;
        } else if (key == QLatin1StringView("tabs-location")) {
            const std::optional<TabsLocation> parsed = parseTabsLocation(value);
            if (!parsed) {
                return std::unexpected(diagnostic(
                    sourceName, lineNumber,
                    QStringLiteral(
                        "invalid tabs-location value '%1'; expected top or bottom")
                        .arg(value)));
            }
            values.tabsLocation = *parsed;
        } else if (key == QLatin1StringView("wide-tabs")
                   || key == QLatin1StringView("horizontal-tab-scroll")) {
            const std::optional<bool> parsed = parseBoolean(value);
            if (!parsed) {
                return std::unexpected(diagnostic(
                    sourceName, lineNumber,
                    QStringLiteral(
                        "invalid %1 value '%2'; expected true or false")
                        .arg(key, value)));
            }
            if (key == QLatin1StringView("wide-tabs")) {
                values.wideTabs = *parsed;
            } else {
                values.horizontalTabScroll = *parsed;
            }
        } else if (key == QLatin1StringView("quick-terminal-layer")) {
            const std::optional<QuickTerminalLayer> parsed =
                parseQuickTerminalLayer(value);
            if (!parsed) {
                return std::unexpected(diagnostic(
                    sourceName, lineNumber,
                    QStringLiteral(
                        "invalid quick-terminal-layer value '%1'; expected background, bottom, top, or overlay")
                        .arg(value)));
            }
            values.quickTerminalLayerShell.layer = *parsed;
        } else if (key == QLatin1StringView("quick-terminal-namespace")) {
            values.quickTerminalLayerShell.layerNamespace = value;
        } else {
            return std::unexpected(
                diagnostic(sourceName, lineNumber,
                           QStringLiteral("unknown key '%1'").arg(key)));
        }
        assignedKeys.insert(key);
    }

    return values;
}

FrontendConfigLoadResult loadFrontendConfigFile(const QString &path)
{
    const QString absolutePath = normalizedAbsolutePath(path);
    if (absolutePath.isEmpty()) {
        return std::unexpected(
            QStringLiteral("Frontend configuration path must not be empty"));
    }

    const QFileInfo info(absolutePath);
    if (!info.exists()) {
        return FrontendConfigSnapshot{};
    }
    if (!info.isFile()) {
        return std::unexpected(
            QStringLiteral("%1: frontend configuration is not a regular file")
                .arg(absolutePath));
    }

    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(
            QStringLiteral("%1: could not open frontend configuration: %2")
                .arg(absolutePath, file.errorString()));
    }
    const QByteArray contents = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return std::unexpected(
            QStringLiteral("%1: could not read frontend configuration: %2")
                .arg(absolutePath, file.errorString()));
    }

    auto values = parseFrontendConfig(contents, absolutePath);
    if (!values) return std::unexpected(std::move(values.error()));
    return FrontendConfigSnapshot{
        .values = std::move(*values),
        .sourcePath = absolutePath,
    };
}
