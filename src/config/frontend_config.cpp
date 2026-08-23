#include "config/frontend_config.h"
#include "support/posix_regular_file.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSet>
#include <QStringDecoder>
#include <QStringList>

#include <algorithm>
#include <cstring>
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

QByteArrayView trimFrontendWhitespace(QByteArrayView value)
{
    qsizetype begin = 0;
    while (begin < value.size()
           && (value.at(begin) == ' ' || value.at(begin) == '\t')) {
        ++begin;
    }
    qsizetype end = value.size();
    while (end > begin
           && (value.at(end - 1) == ' ' || value.at(end - 1) == '\t')) {
        --end;
    }
    return value.sliced(begin, end - begin);
}

bool isFrontendKey(QByteArrayView key)
{
    return key == QByteArrayView("single-instance")
        || key == QByteArrayView("tabs-location")
        || key == QByteArrayView("window-show-toolbar")
        || key == QByteArrayView("wide-tabs")
        || key == QByteArrayView("horizontal-tab-scroll")
        || key == QByteArrayView("quick-terminal-layer")
        || key == QByteArrayView("quick-terminal-namespace")
        || key == QByteArrayView("pane-enter-transition-shader")
        || key == QByteArrayView("pane-exit-transition-shader")
        || key == QByteArrayView("pane-enter-transition-duration")
        || key == QByteArrayView("pane-exit-transition-duration");
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

std::optional<std::chrono::milliseconds>
parseTransitionDuration(QStringView value)
{
    constexpr quint64 maximumMilliseconds = 10'000;
    if (!value.endsWith(QLatin1StringView("ms"))) return std::nullopt;
    const QStringView number = value.first(value.size() - 2);
    if (number.isEmpty() || !std::ranges::all_of(number, [](QChar character) {
            return character >= u'0' && character <= u'9';
        })) {
        return std::nullopt;
    }
    bool converted = false;
    const quint64 milliseconds = number.toULongLong(&converted);
    if (!converted || milliseconds > maximumMilliseconds) return std::nullopt;
    return std::chrono::milliseconds(milliseconds);
}

} // namespace

std::expected<FrontendConfigValues, QString>
parseFrontendConfig(QByteArrayView contents, QStringView sourceName)
{
    constexpr QByteArrayView Utf8Bom("\xEF\xBB\xBF", 3);
    if (contents.startsWith(Utf8Bom)) {
        contents = contents.sliced(Utf8Bom.size());
    }

    FrontendConfigValues values;
    QSet<QString> assignedKeys;
    QList<QByteArrayView> lines;
    qsizetype lineStart = 0;
    while (lineStart <= contents.size()) {
        const qsizetype separator = contents.indexOf('\n', lineStart);
        const qsizetype lineEnd = separator < 0 ? contents.size() : separator;
        lines.append(contents.sliced(lineStart, lineEnd - lineStart));
        if (separator < 0) break;
        lineStart = separator + 1;
    }
    for (qsizetype index = 0; index < lines.size(); ++index) {
        QByteArrayView encodedLine = lines.at(index);
        if (encodedLine.endsWith('\r')) encodedLine.chop(1);

        const QByteArrayView trimmedEncoded =
            trimFrontendWhitespace(encodedLine);
        if (trimmedEncoded.isEmpty() || trimmedEncoded.startsWith('#')) {
            continue;
        }

        const qsizetype firstSeparator = encodedLine.indexOf('=');
        QByteArrayView encodedKey = trimFrontendWhitespace(
            firstSeparator < 0 ? encodedLine
                               : encodedLine.first(firstSeparator));
        if (firstSeparator < 0) {
            const qsizetype whitespace = encodedKey.indexOf(' ');
            const qsizetype tab = encodedKey.indexOf('\t');
            const qsizetype end = whitespace < 0 ? tab
                : tab < 0                        ? whitespace
                                                 : std::min(whitespace, tab);
            if (end >= 0) encodedKey = encodedKey.first(end);
        }
        if (!isFrontendKey(encodedKey)) {
            // Ghostty owns every non-frontend line, including its byte
            // encoding, repeat/reset behavior, quoting, and diagnostics.
            continue;
        }

        const qsizetype lineNumber = index + 1;
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString line = decoder.decode(encodedLine);
        if (decoder.hasError()) {
            return std::unexpected(diagnostic(
                sourceName, lineNumber,
                QStringLiteral("frontend assignment is not valid UTF-8")));
        }
        for (const QChar character : std::as_const(line)) {
            if (character.unicode() < 0x20 && character != u'\t') {
                return std::unexpected(
                    diagnostic(sourceName, lineNumber,
                               QStringLiteral("invalid control character")));
            }
        }
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
        } else if (key == QLatin1StringView("window-show-toolbar")
                   || key == QLatin1StringView("wide-tabs")
                   || key == QLatin1StringView("horizontal-tab-scroll")) {
            const std::optional<bool> parsed = parseBoolean(value);
            if (!parsed) {
                return std::unexpected(diagnostic(
                    sourceName, lineNumber,
                    QStringLiteral(
                        "invalid %1 value '%2'; expected true or false")
                        .arg(key, value)));
            }
            if (key == QLatin1StringView("window-show-toolbar")) {
                values.windowShowToolbar = *parsed;
            } else if (key == QLatin1StringView("wide-tabs")) {
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
        } else if (key == QLatin1StringView("pane-enter-transition-shader")) {
            values.paneEnterTransitionShaderPath = value;
        } else if (key == QLatin1StringView("pane-exit-transition-shader")) {
            values.paneExitTransitionShaderPath = value;
        } else if (key == QLatin1StringView("pane-enter-transition-duration")
                   || key
                       == QLatin1StringView("pane-exit-transition-duration")) {
            const std::optional<std::chrono::milliseconds> parsed =
                parseTransitionDuration(value);
            if (!parsed) {
                return std::unexpected(diagnostic(
                    sourceName, lineNumber,
                    QStringLiteral(
                        "invalid %1 value '%2'; expected 0ms through 10000ms")
                        .arg(key, value)));
            }
            if (key == QLatin1StringView("pane-enter-transition-duration")) {
                values.paneEnterTransitionDuration = *parsed;
            } else {
                values.paneExitTransitionDuration = *parsed;
            }
        }
        assignedKeys.insert(key);
    }

    return values;
}

FrontendConfigLoadResult loadFrontendConfigFile(const QString &path)
{
    if (path.contains(QChar::Null)) {
        return std::unexpected(QStringLiteral(
            "Frontend configuration path must contain no NUL character"));
    }
    const QString absolutePath = normalizedAbsolutePath(path);
    if (absolutePath.isEmpty()) {
        return std::unexpected(
            QStringLiteral("Frontend configuration path must not be empty"));
    }

    auto contents = readBoundedPosixRegularFile(QFile::encodeName(absolutePath),
                                                MaximumFrontendConfigFileSize);
    if (!contents) {
        const PosixRegularFileError error = contents.error();
        if (error.kind == PosixRegularFileErrorKind::Open
            && error.systemError == ENOENT) {
            return FrontendConfigSnapshot{};
        }
        if (error.kind == PosixRegularFileErrorKind::NotRegular) {
            return std::unexpected(
                QStringLiteral(
                    "%1: frontend configuration is not a regular file")
                    .arg(absolutePath));
        }
        if (error.kind == PosixRegularFileErrorKind::TooLarge) {
            return std::unexpected(
                QStringLiteral(
                    "%1: frontend configuration exceeds the 1 MiB limit")
                    .arg(absolutePath));
        }
        const QString operation = error.kind == PosixRegularFileErrorKind::Read
            ? QStringLiteral("read")
            : error.kind == PosixRegularFileErrorKind::Inspect
            ? QStringLiteral("inspect")
            : QStringLiteral("open");
        return std::unexpected(
            QStringLiteral("%1: could not %2 frontend configuration: %3")
                .arg(absolutePath, operation,
                     QString::fromLocal8Bit(std::strerror(error.systemError))));
    }

    auto values = parseFrontendConfig(*contents, absolutePath);
    if (!values) return std::unexpected(std::move(values.error()));
    return FrontendConfigSnapshot{
        .values = std::move(*values),
        .sourcePath = absolutePath,
    };
}
