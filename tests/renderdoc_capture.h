#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>

namespace RenderDocCaptureDetail {

// This is the API 1.0.0 prefix from RenderDoc's MIT-licensed
// renderdoc_app.h:
// https://github.com/baldurk/renderdoc/blob/v1.x/renderdoc/api/app/renderdoc_app.h
// Later API versions append function pointers, so requesting 1.0.0 keeps this
// prefix ABI-stable while still working with newer RenderDoc releases.
enum CaptureOption : int {
};
enum InputButton : int {
};

using DevicePointer = void *;
using WindowHandle = void *;

using GetApiVersionFn = void (*)(int *, int *, int *);
using SetCaptureOptionU32Fn = int (*)(CaptureOption, std::uint32_t);
using SetCaptureOptionF32Fn = int (*)(CaptureOption, float);
using GetCaptureOptionU32Fn = std::uint32_t (*)(CaptureOption);
using GetCaptureOptionF32Fn = float (*)(CaptureOption);
using SetFocusToggleKeysFn = void (*)(InputButton *, int);
using SetCaptureKeysFn = void (*)(InputButton *, int);
using GetOverlayBitsFn = std::uint32_t (*)();
using MaskOverlayBitsFn = void (*)(std::uint32_t, std::uint32_t);
using RemoveHooksFn = void (*)();
using UnloadCrashHandlerFn = void (*)();
using SetCaptureFilePathTemplateFn = void (*)(const char *);
using GetCaptureFilePathTemplateFn = const char *(*)();
using GetNumCapturesFn = std::uint32_t (*)();
using GetCaptureFn = std::uint32_t (*)(std::uint32_t, char *, std::uint32_t *,
                                       std::uint64_t *);
using TriggerCaptureFn = void (*)();
using IsTargetControlConnectedFn = std::uint32_t (*)();
using LaunchReplayUiFn = std::uint32_t (*)(std::uint32_t, const char *);
using SetActiveWindowFn = void (*)(DevicePointer, WindowHandle);
using StartFrameCaptureFn = void (*)(DevicePointer, WindowHandle);
using IsFrameCapturingFn = std::uint32_t (*)();
using EndFrameCaptureFn = std::uint32_t (*)(DevicePointer, WindowHandle);

struct ApiV100 {
    GetApiVersionFn GetAPIVersion = nullptr;
    SetCaptureOptionU32Fn SetCaptureOptionU32 = nullptr;
    SetCaptureOptionF32Fn SetCaptureOptionF32 = nullptr;
    GetCaptureOptionU32Fn GetCaptureOptionU32 = nullptr;
    GetCaptureOptionF32Fn GetCaptureOptionF32 = nullptr;
    SetFocusToggleKeysFn SetFocusToggleKeys = nullptr;
    SetCaptureKeysFn SetCaptureKeys = nullptr;
    GetOverlayBitsFn GetOverlayBits = nullptr;
    MaskOverlayBitsFn MaskOverlayBits = nullptr;
    RemoveHooksFn RemoveHooks = nullptr;
    UnloadCrashHandlerFn UnloadCrashHandler = nullptr;
    SetCaptureFilePathTemplateFn SetCaptureFilePathTemplate = nullptr;
    GetCaptureFilePathTemplateFn GetCaptureFilePathTemplate = nullptr;
    GetNumCapturesFn GetNumCaptures = nullptr;
    GetCaptureFn GetCapture = nullptr;
    TriggerCaptureFn TriggerCapture = nullptr;
    IsTargetControlConnectedFn IsTargetControlConnected = nullptr;
    LaunchReplayUiFn LaunchReplayUI = nullptr;
    SetActiveWindowFn SetActiveWindow = nullptr;
    StartFrameCaptureFn StartFrameCapture = nullptr;
    IsFrameCapturingFn IsFrameCapturing = nullptr;
    EndFrameCaptureFn EndFrameCapture = nullptr;
};

inline constexpr int apiVersion100 = 10'000;
using GetApi = int (*)(int version, void **api);

} // namespace RenderDocCaptureDetail

class RenderDocCapture {
public:
    // Passing no resolver looks up RENDERDOC_GetAPI in the process-wide symbol
    // table. RenderDoc must already have been injected before process startup.
    explicit RenderDocCapture(
        QByteArray capturePathTemplate = {},
        RenderDocCaptureDetail::GetApi resolver = nullptr);
    ~RenderDocCapture();

    RenderDocCapture(const RenderDocCapture &) = delete;
    RenderDocCapture &operator=(const RenderDocCapture &) = delete;
    RenderDocCapture(RenderDocCapture &&) = delete;
    RenderDocCapture &operator=(RenderDocCapture &&) = delete;

    [[nodiscard]] bool isAvailable() const noexcept;
    [[nodiscard]] bool isCapturing() const noexcept;
    [[nodiscard]] QString apiVersion() const;
    [[nodiscard]] const QString &errorString() const noexcept;

    bool setCapturePathTemplate(const QByteArray &capturePathTemplate);
    bool start();
    bool end();

private:
    void initialize(RenderDocCaptureDetail::GetApi resolver);
    void setError(QString error);

    RenderDocCaptureDetail::ApiV100 *api_ = nullptr;
    QString apiVersion_;
    QString error_;
    bool ownsCapture_ = false;
};

class RenderDocCaptureScope {
public:
    explicit RenderDocCaptureScope(RenderDocCapture &capture);
    ~RenderDocCaptureScope();

    RenderDocCaptureScope(const RenderDocCaptureScope &) = delete;
    RenderDocCaptureScope &operator=(const RenderDocCaptureScope &) = delete;
    RenderDocCaptureScope(RenderDocCaptureScope &&) = delete;
    RenderDocCaptureScope &operator=(RenderDocCaptureScope &&) = delete;

    [[nodiscard]] bool started() const noexcept;
    bool finish();

private:
    RenderDocCapture &capture_;
    bool active_ = false;
};
