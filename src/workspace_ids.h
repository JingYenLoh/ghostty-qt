#pragma once

#include <QMetaType>
#include <QtGlobal>

#include <cstddef>
#include <functional>

template<typename Tag>
class WorkspaceId {
public:
    using Value = quint64;

    constexpr WorkspaceId() = default;
    explicit constexpr WorkspaceId(Value value)
        : value_(value)
    {
    }

    [[nodiscard]] constexpr bool isValid() const { return value_ != 0; }
    [[nodiscard]] constexpr Value value() const { return value_; }

    friend constexpr bool operator==(WorkspaceId, WorkspaceId) = default;

private:
    Value value_ = 0;
};

struct TabIdTag;
struct PaneIdTag;

using TabId = WorkspaceId<TabIdTag>;
using PaneId = WorkspaceId<PaneIdTag>;

template<typename Tag>
constexpr size_t qHash(WorkspaceId<Tag> id, size_t seed = 0) noexcept
{
    return qHash(id.value(), seed);
}

Q_DECLARE_METATYPE(TabId)
Q_DECLARE_METATYPE(PaneId)
