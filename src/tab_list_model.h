#pragma once

#include "workspace_ids.h"

#include <QAbstractListModel>
#include <QString>
#include <QVector>

class TerminalWorkspace;

struct TabListEntry {
    TabId id;
    PaneId activePaneId;
    QString title = QStringLiteral("Terminal");
    QString titleOverride;
    QString currentDirectory;
    bool running = false;
    bool zoomed = false;
    // The active surface's title bell is presentation-only; `title` and
    // `titleOverride` remain raw application-visible values.
    bool bell = false;
    bool attention = false;
    int progress = -1;
    bool readOnly = false;

    friend bool operator==(const TabListEntry &,
                           const TabListEntry &) = default;
};

class TabListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        TabIdRole = Qt::UserRole + 1,
        TitleRole,
        TitleOverrideRole,
        CurrentDirectoryRole,
        RunningRole,
        ZoomedRole,
        AttentionRole,
        ProgressRole,
        ReadOnlyRole,
        ActivePaneIdRole,
    };
    Q_ENUM(Role)

    explicit TabListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(entries_.size()); }
    const TabListEntry *entryAt(int index) const;
    TabId idAt(int index) const;
    int indexOf(TabId id) const;

Q_SIGNALS:
    void countChanged();

private:
    friend class TerminalWorkspace;

    void append(TabListEntry entry);
    bool insert(int index, TabListEntry entry);
    bool replace(TabId id, TabListEntry entry);
    bool move(TabId id, int destination);
    bool removeAt(int index);

    static QString displayTitle(const TabListEntry &entry);

    QVector<TabListEntry> entries_;
};
