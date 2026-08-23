#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringView>

#include <optional>

enum class RendererGraphicsBackend {
    OpenGL,
    Vulkan,
};

enum class RendererGraphicsLibraryRole {
    Driver,
    VendorDispatch,
    ApiLoader,
    Layer,
    Compiler,
};

enum class RendererGraphicsLibraryManifestStatus {
    Complete,
    Partial,
    Unavailable,
};

struct RendererGraphicsLibraryMapping {
    quint64 start = 0;
    quint64 end = 0;
    quint64 deviceMajor = 0;
    quint64 deviceMinor = 0;
    quint64 inode = 0;
    QByteArray permissions;
    QString path;
};

struct RendererGraphicsLibraryEntry {
    RendererGraphicsLibraryRole role = RendererGraphicsLibraryRole::Driver;
    QString name;
    QString pathKind;
    quint64 size = 0;
    QByteArray sha256;
};

struct RendererGraphicsLibraryManifest {
    RendererGraphicsBackend backend = RendererGraphicsBackend::OpenGL;
    RendererGraphicsLibraryManifestStatus status =
        RendererGraphicsLibraryManifestStatus::Unavailable;
    QList<RendererGraphicsLibraryEntry> libraries;
    QByteArray aggregateSha256;
    QByteArray compactJson;
    QString diagnostic;
};

[[nodiscard]] QStringView rendererGraphicsLibraryManifestStatusName(
    RendererGraphicsLibraryManifestStatus status);

[[nodiscard]] QString rendererGraphicsLibraryPathKind(QStringView path);

[[nodiscard]] QList<RendererGraphicsLibraryMapping>
parseRendererGraphicsLibraryMaps(QByteArrayView contents,
                                 QString *diagnostic = nullptr);

[[nodiscard]] std::optional<RendererGraphicsLibraryRole>
classifyRendererGraphicsLibrary(RendererGraphicsBackend backend,
                                QStringView basename);

[[nodiscard]] QByteArray rendererGraphicsLibraryAggregate(
    const QList<RendererGraphicsLibraryEntry> &libraries);

[[nodiscard]] RendererGraphicsLibraryManifest
collectRendererGraphicsLibraryManifest(
    RendererGraphicsBackend backend,
    const QString &procSelfRoot = QStringLiteral("/proc/self"));
