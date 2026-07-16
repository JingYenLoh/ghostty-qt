#include "tab_list_model.h"

#include <QList>
#include <QVariant>

#include <algorithm>
#include <utility>

TabListModel::TabListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int TabListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

QVariant TabListModel::data(const QModelIndex &index, int role) const
{
    const TabListEntry *entry = entryAt(index.row());
    if (entry == nullptr || index.column() != 0) {
        return {};
    }

    switch (role) {
    case TabIdRole:
        return QVariant::fromValue(entry->id.value());
    case TitleRole:
        return displayTitle(*entry);
    case TitleOverrideRole:
        return entry->titleOverride;
    case CurrentDirectoryRole:
        return entry->currentDirectory;
    case RunningRole:
        return entry->running;
    case ZoomedRole:
        return entry->zoomed;
    case AttentionRole:
        return entry->attention;
    case ProgressRole:
        return entry->progress;
    case ReadOnlyRole:
        return entry->readOnly;
    case ActivePaneIdRole:
        return QVariant::fromValue(entry->activePaneId.value());
    default:
        return {};
    }
}

QHash<int, QByteArray> TabListModel::roleNames() const
{
    return {
        {TabIdRole, QByteArrayLiteral("tabId")},
        {TitleRole, QByteArrayLiteral("title")},
        {TitleOverrideRole, QByteArrayLiteral("titleOverride")},
        {CurrentDirectoryRole, QByteArrayLiteral("currentDirectory")},
        {RunningRole, QByteArrayLiteral("running")},
        {ZoomedRole, QByteArrayLiteral("zoomed")},
        {AttentionRole, QByteArrayLiteral("attention")},
        {ProgressRole, QByteArrayLiteral("progress")},
        {ReadOnlyRole, QByteArrayLiteral("readOnly")},
        {ActivePaneIdRole, QByteArrayLiteral("activePaneId")},
    };
}

const TabListEntry *TabListModel::entryAt(int index) const
{
    if (index < 0 || index >= entries_.size()) {
        return nullptr;
    }
    return &entries_.at(index);
}

TabId TabListModel::idAt(int index) const
{
    const TabListEntry *entry = entryAt(index);
    return entry != nullptr ? entry->id : TabId{};
}

int TabListModel::indexOf(TabId id) const
{
    const auto entry = std::find_if(entries_.cbegin(), entries_.cend(),
                                    [id](const TabListEntry &candidate) {
                                        return candidate.id == id;
                                    });
    return entry == entries_.cend()
        ? -1
        : static_cast<int>(std::distance(entries_.cbegin(), entry));
}

void TabListModel::append(TabListEntry entry)
{
    const int index = static_cast<int>(entries_.size());
    beginInsertRows(QModelIndex(), index, index);
    entries_.append(std::move(entry));
    endInsertRows();
    Q_EMIT countChanged();
}

bool TabListModel::replace(TabId id, TabListEntry entry)
{
    const int row = indexOf(id);
    if (row < 0 || entry.id != id) {
        return false;
    }

    const TabListEntry previous = entries_.at(row);
    if (previous == entry) {
        return true;
    }
    entries_[row] = std::move(entry);

    QList<int> roles;
    if (previous.title != entries_[row].title
        || previous.titleOverride != entries_[row].titleOverride) {
        roles.append(TitleRole);
    }
    if (previous.titleOverride != entries_[row].titleOverride) {
        roles.append(TitleOverrideRole);
    }
    if (previous.currentDirectory != entries_[row].currentDirectory) {
        roles.append(CurrentDirectoryRole);
    }
    if (previous.running != entries_[row].running) {
        roles.append(RunningRole);
    }
    if (previous.zoomed != entries_[row].zoomed) {
        roles.append(ZoomedRole);
    }
    if (previous.attention != entries_[row].attention) {
        roles.append(AttentionRole);
    }
    if (previous.progress != entries_[row].progress) {
        roles.append(ProgressRole);
    }
    if (previous.readOnly != entries_[row].readOnly) {
        roles.append(ReadOnlyRole);
    }
    if (previous.activePaneId != entries_[row].activePaneId) {
        roles.append(ActivePaneIdRole);
    }
    Q_EMIT dataChanged(index(row, 0), index(row, 0), roles);
    return true;
}

bool TabListModel::move(TabId id, int destination)
{
    const int source = indexOf(id);
    if (source < 0 || destination < 0 || destination >= entries_.size()) {
        return false;
    }
    if (source == destination) {
        return true;
    }

    // beginMoveRows uses the insertion point before removal, whereas
    // QVector::move uses the final row index.
    const int destinationChild = source < destination
        ? destination + 1
        : destination;
    if (!beginMoveRows(QModelIndex(), source, source,
                       QModelIndex(), destinationChild)) {
        return false;
    }
    entries_.move(source, destination);
    endMoveRows();
    return true;
}

bool TabListModel::remove(TabId id)
{
    const int row = indexOf(id);
    if (row < 0) {
        return false;
    }
    beginRemoveRows(QModelIndex(), row, row);
    entries_.removeAt(row);
    endRemoveRows();
    Q_EMIT countChanged();
    return true;
}

QString TabListModel::displayTitle(const TabListEntry &entry)
{
    return entry.titleOverride.isEmpty() ? entry.title : entry.titleOverride;
}
