#pragma once

#include <QByteArray>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QRectF>
#include <QSGGeometryNode>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

class QSGGeometry;
class QSGTexture;
class QShader;
class TerminalCustomShaderEffect;
class TerminalCustomShaderPipelineEffect;
class TerminalCustomShaderQsgMaterial;
struct QSGMaterialType;
struct TerminalCustomShaderStage;

struct alignas(16) TerminalCustomShaderVec4 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 0.0F;

    bool operator==(const TerminalCustomShaderVec4 &) const = default;
};

// CPU mirror of the std140 buffer shared by the fixed Qt Quick vertex shader
// and every generated custom fragment shader. Qt's scene graph owns the first
// 80 bytes: its matrix starts at 0, inherited opacity at 64, and std140
// alignment pads the first Ghostty field to 80. The remainder matches the
// ShaderToy-compatible Ghostty block.
struct alignas(16) TerminalCustomShaderUniforms {
    std::array<float, 16> qtMatrix{};
    float qtOpacity = 1.0F;
    std::array<float, 3> qtPadding{};

    std::array<float, 3> resolution{};
    float time = 0.0F;
    float timeDelta = 0.0F;
    float frameRate = 60.0F;
    std::int32_t frame = 0;
    float framePadding = 0.0F;
    std::array<TerminalCustomShaderVec4, 4> channelTime{};
    std::array<TerminalCustomShaderVec4, 4> channelResolution{};
    TerminalCustomShaderVec4 mouse{};
    TerminalCustomShaderVec4 date{};
    float sampleRate = 0.0F;
    std::array<float, 3> sampleRatePadding{};
    TerminalCustomShaderVec4 currentCursor{};
    TerminalCustomShaderVec4 previousCursor{};
    TerminalCustomShaderVec4 currentCursorColor{};
    TerminalCustomShaderVec4 previousCursorColor{};
    std::int32_t currentCursorStyle = 0;
    std::int32_t previousCursorStyle = 0;
    std::int32_t cursorVisible = 0;
    float timeCursorChange = 0.0F;
    float timeFocus = 0.0F;
    std::int32_t focus = 0;
    std::array<std::int32_t, 2> statePadding{};
    std::array<TerminalCustomShaderVec4, 256> palette{};
    TerminalCustomShaderVec4 backgroundColor{};
    TerminalCustomShaderVec4 foregroundColor{};
    TerminalCustomShaderVec4 cursorColor{};
    TerminalCustomShaderVec4 cursorText{};
    TerminalCustomShaderVec4 selectionForegroundColor{};
    TerminalCustomShaderVec4 selectionBackgroundColor{};

    bool operator==(const TerminalCustomShaderUniforms &) const = default;
};

namespace TerminalCustomShaderUniformLayout {

inline constexpr std::size_t qtMatrix = 0;
inline constexpr std::size_t qtOpacity = 64;
inline constexpr std::size_t ghostty = 80;
inline constexpr std::size_t resolution = 80;
inline constexpr std::size_t time = 92;
inline constexpr std::size_t timeDelta = 96;
inline constexpr std::size_t frameRate = 100;
inline constexpr std::size_t frame = 104;
inline constexpr std::size_t channelTime = 112;
inline constexpr std::size_t channelResolution = 176;
inline constexpr std::size_t mouse = 240;
inline constexpr std::size_t date = 256;
inline constexpr std::size_t sampleRate = 272;
inline constexpr std::size_t currentCursor = 288;
inline constexpr std::size_t previousCursor = 304;
inline constexpr std::size_t currentCursorColor = 320;
inline constexpr std::size_t previousCursorColor = 336;
inline constexpr std::size_t currentCursorStyle = 352;
inline constexpr std::size_t previousCursorStyle = 356;
inline constexpr std::size_t cursorVisible = 360;
inline constexpr std::size_t timeCursorChange = 364;
inline constexpr std::size_t timeFocus = 368;
inline constexpr std::size_t focus = 372;
inline constexpr std::size_t palette = 384;
inline constexpr std::size_t backgroundColor = 4480;
inline constexpr std::size_t foregroundColor = 4496;
inline constexpr std::size_t cursorColor = 4512;
inline constexpr std::size_t cursorText = 4528;
inline constexpr std::size_t selectionForegroundColor = 4544;
inline constexpr std::size_t selectionBackgroundColor = 4560;
inline constexpr std::size_t trailingPadding = 4572;
inline constexpr std::size_t size = 4576;

} // namespace TerminalCustomShaderUniformLayout

static_assert(std::is_standard_layout_v<TerminalCustomShaderUniforms>);
static_assert(std::is_trivially_copyable_v<TerminalCustomShaderUniforms>);
static_assert(alignof(TerminalCustomShaderUniforms) == 16);
static_assert(sizeof(TerminalCustomShaderUniforms)
              == TerminalCustomShaderUniformLayout::size);
static_assert(offsetof(TerminalCustomShaderUniforms, resolution)
              == TerminalCustomShaderUniformLayout::resolution);
static_assert(offsetof(TerminalCustomShaderUniforms, time)
              == TerminalCustomShaderUniformLayout::time);
static_assert(offsetof(TerminalCustomShaderUniforms, channelTime)
              == TerminalCustomShaderUniformLayout::channelTime);
static_assert(offsetof(TerminalCustomShaderUniforms, currentCursor)
              == TerminalCustomShaderUniformLayout::currentCursor);
static_assert(offsetof(TerminalCustomShaderUniforms, palette)
              == TerminalCustomShaderUniformLayout::palette);
static_assert(offsetof(TerminalCustomShaderUniforms, selectionForegroundColor)
              == TerminalCustomShaderUniformLayout::selectionForegroundColor);
static_assert(offsetof(TerminalCustomShaderUniforms, selectionBackgroundColor)
              == TerminalCustomShaderUniformLayout::selectionBackgroundColor);
static_assert(offsetof(TerminalCustomShaderUniforms, selectionBackgroundColor)
                  + offsetof(TerminalCustomShaderVec4, w)
              == TerminalCustomShaderUniformLayout::trailingPadding);

using TerminalCustomShaderUniformSnapshot =
    std::shared_ptr<const TerminalCustomShaderUniforms>;

// Implemented by the pane-side object named by the QML effect's
// uniformProvider property. updatePaintNode calls uniform snapshot while the
// GUI thread is blocked; implementations must only copy immutable state and
// must not create or mutate QObjects or QSG resources there.
//
// The attach hooks let the provider retain guarded effect pointers and drive
// effect->update() for animation. Updating an effect directly re-runs only the
// post-processing item; it does not dirty or repaint the terminal source.
class TerminalCustomShaderUniformProvider {
public:
    virtual ~TerminalCustomShaderUniformProvider() = default;

    [[nodiscard]] virtual TerminalCustomShaderUniformSnapshot
    terminalCustomShaderUniformSnapshot(int stageIndex) const = 0;

    virtual void
    terminalCustomShaderEffectAttached(TerminalCustomShaderEffect *, int)
    {}
    virtual void
    terminalCustomShaderEffectDetached(TerminalCustomShaderEffect *, int)
    {}
    virtual void
    terminalCustomShaderPipelineAttached(TerminalCustomShaderPipelineEffect *)
    {}
    virtual void
    terminalCustomShaderPipelineDetached(TerminalCustomShaderPipelineEffect *)
    {}
};

#define TerminalCustomShaderUniformProvider_iid                                \
    "io.github.JingYenLoh.ghostty_qt.TerminalCustomShaderUniformProvider"
Q_DECLARE_INTERFACE(TerminalCustomShaderUniformProvider,
                    TerminalCustomShaderUniformProvider_iid)

// Immutable runtime shader identity. qsbPath must name a serialized fragment
// QShader and remain content-stable for cacheKey's lifetime.
class TerminalCustomShaderProgram final {
public:
    explicit TerminalCustomShaderProgram(QString qsbPath,
                                         QByteArray cacheKey = {},
                                         QByteArray serializedShader = {});

    [[nodiscard]] const QString &qsbPath() const noexcept;
    [[nodiscard]] const QByteArray &cacheKey() const noexcept;
    [[nodiscard]] const QShader &shader() const noexcept;
    [[nodiscard]] bool isValid() const noexcept;

private:
    struct Data;

    QString qsbPath_;
    QByteArray cacheKey_;
    std::shared_ptr<const Data> data_;
    QSGMaterialType *materialType_ = nullptr;

    friend class TerminalCustomShaderQsgMaterial;
};

// Render-thread-only retained node. The source texture is borrowed from a
// texture provider and must not be atlas-backed: ShaderToy programs can sample
// arbitrary normalized iChannel0 coordinates, so an atlas sub-rectangle cannot
// be made transparent to them.
class TerminalCustomShaderQsgNode final : public QSGGeometryNode {
public:
    TerminalCustomShaderQsgNode();
    ~TerminalCustomShaderQsgNode() override;

    [[nodiscard]] bool
    update(QSGTexture *source, const QRectF &viewport,
           std::shared_ptr<const TerminalCustomShaderProgram> program,
           TerminalCustomShaderUniformSnapshot uniforms);
    void clear();

    [[nodiscard]] bool isDrawable() const noexcept;
    void preprocess() override;

private:
    QSGGeometry *geometry_ = nullptr;
    TerminalCustomShaderQsgMaterial *material_ = nullptr;
    QRectF viewport_;
    QRectF textureCoordinates_;
};

// A layer-effect item compatible with Item.layer.effect. The layer machinery
// injects its texture provider into `source`; QML supplies the
// content-addressed fragment qsb, the pane-side uniform provider, and the
// ordered stage index.
// Use Item.layer.textureMirroring=NoMirroring. The layer texture's inverted
// normalized sub-rectangle then gives the output top edge a texture Y of 1,
// so both fragCoord and arbitrary iChannel0 samples use Ghostty's
// bottom-left coordinate convention.
class TerminalCustomShaderEffect : public QQuickItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(TerminalCustomShaderEffect)
    Q_PROPERTY(
        QQuickItem *source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(
        QString fragmentShaderFileName READ fragmentShaderFileName WRITE
            setFragmentShaderFileName NOTIFY fragmentShaderFileNameChanged)
    Q_PROPERTY(QByteArray fragmentShaderData READ fragmentShaderData WRITE
                   setFragmentShaderData NOTIFY fragmentShaderDataChanged)
    Q_PROPERTY(QObject *uniformProvider READ uniformProvider WRITE
                   setUniformProvider NOTIFY uniformProviderChanged)
    Q_PROPERTY(int stageIndex READ stageIndex WRITE setStageIndex NOTIFY
                   stageIndexChanged)
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)

public:
    explicit TerminalCustomShaderEffect(QQuickItem *parent = nullptr);
    ~TerminalCustomShaderEffect() override;

    [[nodiscard]] QQuickItem *source() const noexcept;
    void setSource(QQuickItem *source);

    [[nodiscard]] QString fragmentShaderFileName() const;
    void setFragmentShaderFileName(const QString &path);
    [[nodiscard]] QByteArray fragmentShaderData() const;
    void setFragmentShaderData(const QByteArray &data);
    void setStage(const TerminalCustomShaderStage &stage);

    [[nodiscard]] QObject *uniformProvider() const noexcept;
    void setUniformProvider(QObject *provider);

    [[nodiscard]] int stageIndex() const noexcept;
    void setStageIndex(int stageIndex);

    [[nodiscard]] bool isActive() const noexcept;

Q_SIGNALS:
    void sourceChanged();
    void fragmentShaderFileNameChanged();
    void fragmentShaderDataChanged();
    void uniformProviderChanged();
    void stageIndexChanged();
    void activeChanged();

protected:
    QSGNode *updatePaintNode(
        QSGNode *oldNode,
        QQuickItem::UpdatePaintNodeData *updatePaintNodeData) override;
    void geometryChange(const QRectF &newGeometry,
                        const QRectF &oldGeometry) override;

private:
    [[nodiscard]] TerminalCustomShaderUniformProvider *
    providerInterface() const noexcept;
    void attachToProvider();
    void detachFromProvider();
    void updateActive(bool wasActive);
    void rebuildProgram();

    QPointer<QQuickItem> source_;
    QString fragmentShaderFileName_;
    QByteArray fragmentShaderData_;
    std::shared_ptr<const TerminalCustomShaderProgram> program_;
    QPointer<QObject> uniformProvider_;
    int stageIndex_ = 0;
    QMetaObject::Connection sourceDestroyedConnection_;
    QMetaObject::Connection providerDestroyedConnection_;
};
