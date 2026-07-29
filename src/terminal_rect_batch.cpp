#include "terminal_rect_batch.h"

#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGSimpleRectNode>
#include <QSGVertexColorMaterial>

#include <algorithm>
#include <limits>
#include <utility>

namespace {

constexpr qsizetype verticesPerRectangle = 6;

[[nodiscard]] qsizetype grownCapacity(qsizetype current,
                                      qsizetype required) noexcept
{
    if (current >= required) return current;
    const qsizetype maximum = std::numeric_limits<int>::max();
    const qsizetype growth =
        current > maximum - current / 2 ? maximum : current + current / 2;
    return std::max(required, std::max<qsizetype>(growth, 24));
}

void setRectangleVertices(QSGGeometry::ColoredPoint2D *vertices,
                          const TerminalColoredRect &coloredRect)
{
    const QRectF rect = coloredRect.rect.normalized();
    const QColor color = coloredRect.color.toRgb();
    const int alpha = color.alpha();
    const auto premultiply = [alpha](int component) {
        return static_cast<uchar>((component * alpha + 127) / 255);
    };
    const uchar red = premultiply(color.red());
    const uchar green = premultiply(color.green());
    const uchar blue = premultiply(color.blue());
    const uchar opacity = static_cast<uchar>(alpha);
    const float left = static_cast<float>(rect.left());
    const float top = static_cast<float>(rect.top());
    const float right = static_cast<float>(rect.right());
    const float bottom = static_cast<float>(rect.bottom());

    vertices[0].set(left, top, red, green, blue, opacity);
    vertices[1].set(right, top, red, green, blue, opacity);
    vertices[2].set(left, bottom, red, green, blue, opacity);
    vertices[3].set(left, bottom, red, green, blue, opacity);
    vertices[4].set(right, top, red, green, blue, opacity);
    vertices[5].set(right, bottom, red, green, blue, opacity);
}

} // namespace

TerminalRectBatch::TerminalRectBatch()
    : hardwareNode_(new QSGGeometryNode)
    , hardwareGeometry_(
          new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0))
{
    hardwareGeometry_->setDrawingMode(QSGGeometry::DrawTriangles);
    hardwareGeometry_->setVertexDataPattern(QSGGeometry::DynamicPattern);
    auto *const material = new QSGVertexColorMaterial;
    material->setFlag(QSGMaterial::Blending);
    hardwareNode_->setGeometry(hardwareGeometry_);
    hardwareNode_->setMaterial(material);
    hardwareNode_->setFlag(QSGNode::OwnsGeometry);
    hardwareNode_->setFlag(QSGNode::OwnsMaterial);
    appendChildNode(hardwareNode_);
}

TerminalRectBatch::~TerminalRectBatch() = default;

QVector<TerminalColoredRect> &TerminalRectBatch::beginUpdate() noexcept
{
    pending_.clear();
    return pending_;
}

void TerminalRectBatch::commit(bool softwareRenderer)
{
    const bool modeChanged =
        committedSoftwareRenderer_ != softwareRenderer;
    if (!modeChanged && pending_ == committed_) {
        return;
    }

    committedSoftwareRenderer_ = softwareRenderer;
    if (softwareRenderer) {
        commitSoftware();
        hideHardware();
    } else {
        commitHardware();
        hideSoftware();
    }
    committed_.swap(pending_);
    ++commitGeneration_;
}

qsizetype TerminalRectBatch::size() const noexcept
{
    return committed_.size();
}

quint64 TerminalRectBatch::allocationGeneration() const noexcept
{
    return allocationGeneration_;
}

quint64 TerminalRectBatch::commitGeneration() const noexcept
{
    return commitGeneration_;
}

void TerminalRectBatch::commitHardware()
{
    if (pending_.size()
        > std::numeric_limits<int>::max() / verticesPerRectangle) {
        hideHardware();
        return;
    }
    const qsizetype required = pending_.size() * verticesPerRectangle;
    if (required > vertexCapacity_) {
        vertexCapacity_ = grownCapacity(vertexCapacity_, required);
        hardwareGeometry_->allocate(static_cast<int>(vertexCapacity_));
        ++allocationGeneration_;
    }
    hardwareGeometry_->setVertexCount(static_cast<int>(required));
    QSGGeometry::ColoredPoint2D *vertices =
        hardwareGeometry_->vertexDataAsColoredPoint2D();
    for (const TerminalColoredRect &rect : pending_) {
        setRectangleVertices(vertices, rect);
        vertices += verticesPerRectangle;
    }
    hardwareNode_->markDirty(QSGNode::DirtyGeometry);
}

void TerminalRectBatch::commitSoftware()
{
    while (softwareNodes_.size() < pending_.size()) {
        auto *const node = new QSGSimpleRectNode;
        softwareNodes_.append(node);
        appendChildNode(node);
        ++allocationGeneration_;
    }

    qsizetype index = 0;
    for (; index < pending_.size(); ++index) {
        QSGSimpleRectNode *const node = softwareNodes_.at(index);
        const TerminalColoredRect &next = pending_.at(index);
        const QRectF rect = next.rect.normalized();
        if (node->rect() != rect) node->setRect(rect);
        if (node->color() != next.color) node->setColor(next.color);
    }
    for (; index < softwareNodes_.size(); ++index) {
        QSGSimpleRectNode *const node = softwareNodes_.at(index);
        if (!node->rect().isEmpty()) node->setRect({});
    }
}

void TerminalRectBatch::hideHardware()
{
    if (hardwareGeometry_->vertexCount() == 0) return;
    hardwareGeometry_->setVertexCount(0);
    hardwareNode_->markDirty(QSGNode::DirtyGeometry);
}

void TerminalRectBatch::hideSoftware()
{
    for (QSGSimpleRectNode *node : softwareNodes_) {
        if (!node->rect().isEmpty()) node->setRect({});
    }
}
