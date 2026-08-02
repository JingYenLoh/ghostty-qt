#pragma once

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QString>
#include <QVector>

#include <deque>

// A short-lived, bounded diagnostic stream owned by one open inspector. The
// source records contain only copied scalar/text summaries; terminal cells,
// renderer snapshots, clipboard contents, and libghostty handles never enter
// this model.
class TerminalInspectorEventModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int retainedCount READ retainedCount NOTIFY countChanged)
    Q_PROPERTY(int capacity READ capacity CONSTANT)
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY
                   filterChanged)
    Q_PROPERTY(int categoryFilter READ categoryFilter WRITE setCategoryFilter
                   NOTIFY filterChanged)
    Q_PROPERTY(
        quint64 evictedCount READ evictedCount NOTIFY evictedCountChanged)
    Q_PROPERTY(quint64 skippedWhilePaused READ skippedWhilePaused NOTIFY
                   skippedWhilePausedChanged)

public:
    enum class Category : quint8 {
        Input,
        Terminal,
        State,
    };
    Q_ENUM(Category)

    enum Role {
        SequenceRole = Qt::UserRole + 1,
        ElapsedTextRole,
        CategoryRole,
        KindRole,
        SummaryRole,
        DetailsRole,
    };

    static constexpr int DefaultCapacity = 256;
    static constexpr qsizetype MaximumFilterLength = 128;

    explicit TerminalInspectorEventModel(QObject *parent = nullptr,
                                         int capacity = DefaultCapacity);

    [[nodiscard]] int
    rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index,
                                int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] int count() const
    {
        return static_cast<int>(visible_.size());
    }
    [[nodiscard]] int retainedCount() const
    {
        return static_cast<int>(events_.size());
    }
    [[nodiscard]] int capacity() const { return capacity_; }
    [[nodiscard]] bool paused() const { return paused_; }
    [[nodiscard]] QString filterText() const { return filterText_; }
    [[nodiscard]] int categoryFilter() const { return categoryFilter_; }
    [[nodiscard]] quint64 evictedCount() const { return evictedCount_; }
    [[nodiscard]] quint64 skippedWhilePaused() const
    {
        return skippedWhilePaused_;
    }

    // Returns the sequence allocated to this observation. Sequence numbers
    // advance even while paused so resumed capture visibly preserves gaps.
    quint64 append(Category category, QString kind, QString summary,
                   QString details = {});
    // Records only a sequence gap and aggregate count. Callers use this to
    // stop payload projection and model notifications at the pause boundary.
    quint64 skipObservation();

    Q_INVOKABLE void clear();
    Q_INVOKABLE void setPaused(bool paused);
    Q_INVOKABLE void setFilterText(const QString &text);
    Q_INVOKABLE void setCategoryFilter(int category);

Q_SIGNALS:
    void countChanged();
    void pausedChanged();
    void filterChanged();
    void evictedCountChanged();
    void skippedWhilePausedChanged();
    void eventEvicted(quint64 sequence);
    void cleared();

private:
    struct Event {
        quint64 sequence = 0;
        qint64 elapsedMilliseconds = 0;
        Category category = Category::State;
        QString kind;
        QString summary;
        QString details;
    };

    [[nodiscard]] bool matches(const Event &event) const;
    [[nodiscard]] const Event *eventAt(int row) const;
    void rebuildVisible();
    [[nodiscard]] static QString categoryName(Category category);
    [[nodiscard]] static QString elapsedName(qint64 milliseconds);
    [[nodiscard]] static QString bounded(QString value, qsizetype maximum);
    [[nodiscard]] quint64 nextSequence();
    void appendEvent(Event event);

    const int capacity_;
    std::deque<Event> events_;
    std::deque<Event> pendingEvents_;
    QVector<const Event *> visible_;
    QElapsedTimer elapsed_;
    QString filterText_;
    int categoryFilter_ = -1;
    quint64 nextSequence_ = 1;
    quint64 evictedCount_ = 0;
    quint64 skippedWhilePaused_ = 0;
    bool skippedNotificationPending_ = false;
    bool paused_ = false;
    bool appending_ = false;
};
