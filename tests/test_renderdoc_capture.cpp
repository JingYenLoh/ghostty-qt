#include "renderdoc_capture.h"

#include <QTest>

namespace {

using Api = RenderDocCaptureDetail::ApiV100;

struct FakeRenderDoc {
    Api api;
    int requestedVersion = 0;
    QByteArray capturePath;
    bool capturing = false;
    bool beginSucceeds = true;
    bool endSucceeds = true;
    int startCount = 0;
    int endCount = 0;
    void *startDevice = reinterpret_cast<void *>(1);
    void *startWindow = reinterpret_cast<void *>(1);
    void *endDevice = reinterpret_cast<void *>(1);
    void *endWindow = reinterpret_cast<void *>(1);
};

FakeRenderDoc *fake = nullptr;
bool getterSucceeds = true;
bool getterReturnsNull = false;

void getApiVersion(int *major, int *minor, int *patch)
{
    if (major != nullptr) *major = 1;
    if (minor != nullptr) *minor = 7;
    if (patch != nullptr) *patch = 0;
}

void setCapturePath(const char *path)
{
    fake->capturePath = path;
}

void startCapture(void *device, void *window)
{
    ++fake->startCount;
    fake->startDevice = device;
    fake->startWindow = window;
    if (fake->beginSucceeds) fake->capturing = true;
}

std::uint32_t isCapturing()
{
    return fake->capturing ? 1U : 0U;
}

std::uint32_t endCapture(void *device, void *window)
{
    ++fake->endCount;
    fake->endDevice = device;
    fake->endWindow = window;
    fake->capturing = false;
    return fake->endSucceeds ? 1U : 0U;
}

int getApi(int version, void **api)
{
    fake->requestedVersion = version;
    if (!getterSucceeds) return 0;
    *api = getterReturnsNull ? nullptr : &fake->api;
    return 1;
}

Api completeApi()
{
    return {
        .GetAPIVersion = getApiVersion,
        .SetCaptureFilePathTemplate = setCapturePath,
        .StartFrameCapture = startCapture,
        .IsFrameCapturing = isCapturing,
        .EndFrameCapture = endCapture,
    };
}

} // namespace

class RenderDocCaptureTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init();
    void requestsStableApiAndConfiguresCapturePath();
    void reportsRejectedAndNullApis();
    void rejectsIncompleteApi();
    void rejectsInvalidPath();
    void capturesHeadlessFrame();
    void refusesOverlappingCapture();
    void reportsCaptureStartAndEndFailures();
    void scopeEndsCaptureOnExit();

private:
    FakeRenderDoc state_;
};

void RenderDocCaptureTest::init()
{
    state_ = {};
    state_.api = completeApi();
    fake = &state_;
    getterSucceeds = true;
    getterReturnsNull = false;
}

void RenderDocCaptureTest::requestsStableApiAndConfiguresCapturePath()
{
    RenderDocCapture capture(QByteArrayLiteral("/tmp/ghostty-qt-capture"),
                             getApi);

    QVERIFY2(capture.isAvailable(), qPrintable(capture.errorString()));
    QCOMPARE(state_.requestedVersion, RenderDocCaptureDetail::apiVersion100);
    QCOMPARE(state_.capturePath, QByteArrayLiteral("/tmp/ghostty-qt-capture"));
    QCOMPARE(capture.apiVersion(), QStringLiteral("1.7.0"));
    QVERIFY(capture.errorString().isEmpty());
}

void RenderDocCaptureTest::reportsRejectedAndNullApis()
{
    getterSucceeds = false;
    RenderDocCapture rejected({}, getApi);
    QVERIFY(!rejected.isAvailable());
    QVERIFY(
        rejected.errorString().contains(QStringLiteral("does not support")));

    getterSucceeds = true;
    getterReturnsNull = true;
    RenderDocCapture nullApi({}, getApi);
    QVERIFY(!nullApi.isAvailable());
    QVERIFY(nullApi.errorString().contains(QStringLiteral("null API")));
}

void RenderDocCaptureTest::rejectsIncompleteApi()
{
    state_.api.EndFrameCapture = nullptr;
    RenderDocCapture capture({}, getApi);

    QVERIFY(!capture.isAvailable());
    QVERIFY(capture.errorString().contains(QStringLiteral("EndFrameCapture")));
}

void RenderDocCaptureTest::rejectsInvalidPath()
{
    RenderDocCapture capture({}, getApi);
    QVERIFY(capture.isAvailable());

    QVERIFY(!capture.setCapturePathTemplate({}));
    QVERIFY(
        capture.errorString().contains(QStringLiteral("must not be empty")));

    const QByteArray embeddedNul("capture\0suffix", 14);
    QVERIFY(!capture.setCapturePathTemplate(embeddedNul));
    QVERIFY(capture.errorString().contains(QStringLiteral("NUL")));
    QVERIFY(state_.capturePath.isEmpty());
}

void RenderDocCaptureTest::capturesHeadlessFrame()
{
    RenderDocCapture capture({}, getApi);

    QVERIFY(capture.start());
    QVERIFY(capture.isCapturing());
    QCOMPARE(state_.startCount, 1);
    QCOMPARE(state_.startDevice, nullptr);
    QCOMPARE(state_.startWindow, nullptr);

    QVERIFY(capture.end());
    QVERIFY(!capture.isCapturing());
    QCOMPARE(state_.endCount, 1);
    QCOMPARE(state_.endDevice, nullptr);
    QCOMPARE(state_.endWindow, nullptr);
    QVERIFY(capture.errorString().isEmpty());
}

void RenderDocCaptureTest::refusesOverlappingCapture()
{
    state_.capturing = true;
    RenderDocCapture capture({}, getApi);

    QVERIFY(!capture.start());
    QCOMPARE(state_.startCount, 0);
    QVERIFY(
        capture.errorString().contains(QStringLiteral("already capturing")));
}

void RenderDocCaptureTest::reportsCaptureStartAndEndFailures()
{
    state_.beginSucceeds = false;
    RenderDocCapture capture({}, getApi);
    QVERIFY(!capture.start());
    QVERIFY(capture.errorString().contains(QStringLiteral("did not start")));

    state_.beginSucceeds = true;
    state_.endSucceeds = false;
    QVERIFY(capture.start());
    QVERIFY(!capture.end());
    QVERIFY(capture.errorString().contains(QStringLiteral("failed to save")));
}

void RenderDocCaptureTest::scopeEndsCaptureOnExit()
{
    RenderDocCapture capture({}, getApi);
    {
        RenderDocCaptureScope scope(capture);
        QVERIFY(scope.started());
        QVERIFY(capture.isCapturing());
    }

    QVERIFY(!capture.isCapturing());
    QCOMPARE(state_.endCount, 1);
}

QTEST_GUILESS_MAIN(RenderDocCaptureTest)

#include "test_renderdoc_capture.moc"
