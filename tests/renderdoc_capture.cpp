#include "renderdoc_capture.h"

#if !defined(__linux__)
#error "The RenderDoc benchmark capture bridge is Linux-only"
#endif

#include <dlfcn.h>

#include <bit>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace {

using Api = RenderDocCaptureDetail::ApiV100;
using GetApi = RenderDocCaptureDetail::GetApi;

static_assert(sizeof(void *) == sizeof(GetApi));
static_assert(std::is_standard_layout_v<Api>);
static_assert(offsetof(Api, StartFrameCapture)
              == 19 * sizeof(RenderDocCaptureDetail::GetApiVersionFn));
static_assert(offsetof(Api, EndFrameCapture)
              == 21 * sizeof(RenderDocCaptureDetail::GetApiVersionFn));
static_assert(sizeof(Api)
              == 22 * sizeof(RenderDocCaptureDetail::GetApiVersionFn));

GetApi findInjectedRenderDoc(QString *error)
{
    // Clear any stale loader error before asking the global symbol table. Do
    // not dlopen librenderdoc here: loading after graphics initialization is
    // too late for RenderDoc to install its API hooks.
    (void)::dlerror();
    void *symbol = ::dlsym(RTLD_DEFAULT, "RENDERDOC_GetAPI");
    const char *loaderError = ::dlerror();
    if (loaderError != nullptr || symbol == nullptr) {
        *error = QStringLiteral(
            "RenderDoc is not injected; launch the benchmark through "
            "renderdoccmd capture");
        if (loaderError != nullptr) {
            *error += QStringLiteral(" (%1)").arg(
                QString::fromLocal8Bit(loaderError));
        }
        return nullptr;
    }

    return std::bit_cast<GetApi>(symbol);
}

QString missingFunctionError(const Api &api)
{
    if (api.GetAPIVersion == nullptr) {
        return QStringLiteral("RenderDoc API 1.0.0 omitted GetAPIVersion");
    }
    if (api.SetCaptureFilePathTemplate == nullptr) {
        return QStringLiteral(
            "RenderDoc API 1.0.0 omitted SetCaptureFilePathTemplate");
    }
    if (api.StartFrameCapture == nullptr) {
        return QStringLiteral("RenderDoc API 1.0.0 omitted StartFrameCapture");
    }
    if (api.IsFrameCapturing == nullptr) {
        return QStringLiteral("RenderDoc API 1.0.0 omitted IsFrameCapturing");
    }
    if (api.EndFrameCapture == nullptr) {
        return QStringLiteral("RenderDoc API 1.0.0 omitted EndFrameCapture");
    }
    return {};
}

} // namespace

RenderDocCapture::RenderDocCapture(QByteArray capturePathTemplate,
                                   RenderDocCaptureDetail::GetApi resolver)
{
    initialize(resolver);
    if (api_ != nullptr && !capturePathTemplate.isEmpty()) {
        (void)setCapturePathTemplate(capturePathTemplate);
    }
}

RenderDocCapture::~RenderDocCapture()
{
    if (ownsCapture_ && api_ != nullptr) {
        (void)api_->EndFrameCapture(nullptr, nullptr);
    }
}

bool RenderDocCapture::isAvailable() const noexcept
{
    return api_ != nullptr;
}

bool RenderDocCapture::isCapturing() const noexcept
{
    return api_ != nullptr && api_->IsFrameCapturing() != 0;
}

QString RenderDocCapture::apiVersion() const
{
    return apiVersion_;
}

const QString &RenderDocCapture::errorString() const noexcept
{
    return error_;
}

bool RenderDocCapture::setCapturePathTemplate(
    const QByteArray &capturePathTemplate)
{
    error_.clear();
    if (api_ == nullptr) {
        setError(QStringLiteral("RenderDoc API is unavailable"));
        return false;
    }
    if (capturePathTemplate.isEmpty()) {
        setError(QStringLiteral(
            "RenderDoc capture path template must not be empty"));
        return false;
    }
    if (capturePathTemplate.contains('\0')) {
        setError(QStringLiteral(
            "RenderDoc capture path template contains an embedded NUL byte"));
        return false;
    }

    api_->SetCaptureFilePathTemplate(capturePathTemplate.constData());
    return true;
}

bool RenderDocCapture::start()
{
    error_.clear();
    if (api_ == nullptr) {
        setError(QStringLiteral("RenderDoc API is unavailable"));
        return false;
    }
    if (ownsCapture_) {
        setError(
            QStringLiteral("this RenderDoc capture session is already active"));
        return false;
    }
    if (api_->IsFrameCapturing() != 0) {
        setError(QStringLiteral(
            "RenderDoc is already capturing; overlapping captures are "
            "unsupported"));
        return false;
    }

    // Null device/window is RenderDoc's documented wildcard for a process with
    // one graphics device, and specifically supports headless rendering.
    api_->StartFrameCapture(nullptr, nullptr);
    if (api_->IsFrameCapturing() == 0) {
        setError(QStringLiteral(
            "RenderDoc did not start capturing; no supported graphics device "
            "matched the headless wildcard"));
        return false;
    }

    ownsCapture_ = true;
    return true;
}

bool RenderDocCapture::end()
{
    error_.clear();
    if (api_ == nullptr) {
        setError(QStringLiteral("RenderDoc API is unavailable"));
        return false;
    }
    if (!ownsCapture_) {
        setError(QStringLiteral(
            "this RenderDoc capture session has not been started"));
        return false;
    }

    ownsCapture_ = false;
    if (api_->EndFrameCapture(nullptr, nullptr) == 0) {
        setError(QStringLiteral("RenderDoc failed to save the frame capture"));
        return false;
    }
    return true;
}

void RenderDocCapture::initialize(RenderDocCaptureDetail::GetApi resolver)
{
    if (resolver == nullptr) {
        resolver = findInjectedRenderDoc(&error_);
        if (resolver == nullptr) return;
    }

    void *rawApi = nullptr;
    if (resolver(RenderDocCaptureDetail::apiVersion100, &rawApi) != 1) {
        setError(QStringLiteral(
            "injected RenderDoc does not support API version 1.0.0"));
        return;
    }
    if (rawApi == nullptr) {
        setError(QStringLiteral(
            "RenderDoc accepted API version 1.0.0 but returned a null API"));
        return;
    }

    auto *api = static_cast<Api *>(rawApi);
    const QString missing = missingFunctionError(*api);
    if (!missing.isEmpty()) {
        setError(missing);
        return;
    }

    int major = 0;
    int minor = 0;
    int patch = 0;
    api->GetAPIVersion(&major, &minor, &patch);
    if (major != 1) {
        setError(QStringLiteral(
                     "RenderDoc returned incompatible API version %1.%2.%3")
                     .arg(major)
                     .arg(minor)
                     .arg(patch));
        return;
    }

    api_ = api;
    apiVersion_ = QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
    error_.clear();
}

void RenderDocCapture::setError(QString error)
{
    error_ = std::move(error);
}

RenderDocCaptureScope::RenderDocCaptureScope(RenderDocCapture &capture)
    : capture_(capture)
    , active_(capture_.start())
{}

RenderDocCaptureScope::~RenderDocCaptureScope()
{
    if (active_) (void)capture_.end();
}

bool RenderDocCaptureScope::started() const noexcept
{
    return active_;
}

bool RenderDocCaptureScope::finish()
{
    if (!active_) return false;
    active_ = false;
    return capture_.end();
}
