#pragma once

#include <QColor>
#include <QRectF>
#include <QSGNode>
#include <QVector>
#include <QtGlobal>

#include <optional>

class QSGGeometry;
class QSGGeometryNode;
class QSGSimpleRectNode;

struct TerminalColoredRect {
    QRectF rect;
    QColor color;

    bool operator==(const TerminalColoredRect &) const = default;
};

// A retained scene-graph layer for colored rectangles. Call beginUpdate(),
// append the complete next frame, and then commit(). The two scratch vectors
// exchange storage instead of copying, hardware geometry grows only when its
// previous capacity is insufficient, and the software renderer reuses a pool
// of QSGSimpleRectNodes.
class TerminalRectBatch final : public QSGNode {
public:
    TerminalRectBatch();
    ~TerminalRectBatch() override;

    [[nodiscard]] QVector<TerminalColoredRect> &beginUpdate() noexcept;
    void commit(bool softwareRenderer);

    [[nodiscard]] qsizetype size() const noexcept;
    [[nodiscard]] quint64 allocationGeneration() const noexcept;
    [[nodiscard]] quint64 commitGeneration() const noexcept;

private:
    void commitHardware();
    void commitSoftware();
    void hideHardware();
    void hideSoftware();

    QSGGeometryNode *hardwareNode_ = nullptr;
    QSGGeometry *hardwareGeometry_ = nullptr;
    QVector<QSGSimpleRectNode *> softwareNodes_;
    QVector<TerminalColoredRect> pending_;
    QVector<TerminalColoredRect> committed_;
    qsizetype vertexCapacity_ = 0;
    quint64 allocationGeneration_ = 0;
    quint64 commitGeneration_ = 0;
    std::optional<bool> committedSoftwareRenderer_;
};
