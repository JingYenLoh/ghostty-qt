#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringView>

#include <expected>
#include <optional>

// Startup-only Qt mapping for Ghostty's `class` setting. The effective
// application ID is shared by Qt's desktop-file identity and the
// org.freedesktop.Application service. It must therefore satisfy
// GApplication's stricter application-ID grammar before any surface or
// single-instance endpoint is created.
//
// Ghostty's `language` setting intentionally has no corresponding mapping
// here: without an application translation catalog, changing Qt's locale
// would not implement that setting.
struct ApplicationIdentityResolution {
    QString applicationId;
    std::optional<QString> diagnostic;

    [[nodiscard]] const QString &serviceId() const noexcept
    {
        return applicationId;
    }

    bool operator==(const ApplicationIdentityResolution &) const = default;
};

[[nodiscard]] bool isValidApplicationId(QByteArrayView applicationId) noexcept;

[[nodiscard]] std::expected<ApplicationIdentityResolution, QString>
resolveApplicationIdentity(const std::optional<QByteArray> &configuredClass,
                           QStringView buildFallback);

[[nodiscard]] std::expected<ApplicationIdentityResolution, QString>
resolveApplicationIdentity(const std::optional<QString> &configuredClass,
                           QStringView buildFallback);
