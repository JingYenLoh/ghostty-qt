#include "terminal/inspector/terminal_inspector_event_model.h"

#include <QByteArray>
#include <QVariant>

#include <algorithm>

namespace {

constexpr qsizetype maximumSummaryLength = 256;
constexpr qsizetype maximumDetailsLength = 1024;

} // namespace

TerminalInspectorEventModel::TerminalInspectorEventModel(QObject *parent,
                                                         int capacity)
    : QAbstractListModel(parent)
    , capacity_(std::max(1, capacity))
{
    elapsed_.start();
}

int TerminalInspectorEventModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(visible_.size());
}

QVariant TerminalInspectorEventModel::data(const QModelIndex &index,
                                           int role) const
{
    if (!index.isValid() || index.column() != 0) return {};
    const Event *const event = eventAt(index.row());
    if (event == nullptr) return {};

    switch (role) {
    case SequenceRole: return QVariant::fromValue(event->sequence);
    case TraceIdRole: return QVariant::fromValue(event->traceId);
    case ElapsedTextRole: return elapsedName(event->elapsedMilliseconds);
    case CategoryRole: return categoryName(event->category);
    case KindRole: return event->kind;
    case SummaryRole: return event->summary;
    case DetailsRole: return event->details;
    default: return {};
    }
}

QHash<int, QByteArray> TerminalInspectorEventModel::roleNames() const
{
    return {
        {SequenceRole, QByteArrayLiteral("sequence")},
        {TraceIdRole, QByteArrayLiteral("traceId")},
        {ElapsedTextRole, QByteArrayLiteral("elapsedText")},
        {CategoryRole, QByteArrayLiteral("category")},
        {KindRole, QByteArrayLiteral("kind")},
        {SummaryRole, QByteArrayLiteral("summary")},
        {DetailsRole, QByteArrayLiteral("details")},
    };
}

quint64 TerminalInspectorEventModel::append(Category category, QString kind,
                                            QString summary, QString details,
                                            quint64 traceId)
{
    if (paused_) return skipObservation();
    const quint64 sequence = nextSequence();

    Event event{
        .sequence = sequence,
        .traceId = traceId,
        .elapsedMilliseconds = elapsed_.elapsed(),
        .category = category,
        .kind = bounded(std::move(kind), maximumSummaryLength),
        .summary = bounded(std::move(summary), maximumSummaryLength),
        .details = bounded(std::move(details), maximumDetailsLength),
    };

    if (appending_) {
        pendingEvents_.push_back(std::move(event));
        return sequence;
    }

    const quint64 clearEpoch = clearEpoch_;
    appending_ = true;
    appendEvent(std::move(event));
    while (clearEpoch_ == clearEpoch && !pendingEvents_.empty()) {
        Event pending = std::move(pendingEvents_.front());
        pendingEvents_.pop_front();
        appendEvent(std::move(pending));
    }
    if (clearEpoch_ != clearEpoch) pendingEvents_.clear();
    appending_ = false;
    return sequence;
}

void TerminalInspectorEventModel::appendEvent(Event event)
{
    Q_ASSERT(appending_);

    const int previousRetainedCount = retainedCount();
    const int previousVisibleCount = count();
    quint64 evictedSequence = 0;
    if (retainedCount() == capacity_) {
        const Event *const oldest = &events_.front();
        evictedSequence = oldest->sequence;
        if (matches(*oldest)) {
            Q_ASSERT(!visible_.isEmpty());
            Q_ASSERT(visible_.constLast() == oldest);
            const int row = static_cast<int>(visible_.size()) - 1;
            beginRemoveRows(QModelIndex(), row, row);
            visible_.removeLast();
            events_.pop_front();
            endRemoveRows();
        } else {
            events_.pop_front();
        }
        ++evictedCount_;
    }

    const bool visible = matches(event);
    if (visible) beginInsertRows(QModelIndex(), 0, 0);
    events_.push_back(std::move(event));
    if (visible) {
        visible_.prepend(&events_.back());
        endInsertRows();
    }
    if (previousRetainedCount != retainedCount()
        || previousVisibleCount != count()) {
        Q_EMIT countChanged();
    }
    if (evictedSequence != 0) {
        Q_EMIT evictedCountChanged();
        Q_EMIT eventEvicted(evictedSequence);
    }
}

quint64 TerminalInspectorEventModel::skipObservation()
{
    Q_ASSERT(paused_);
    const quint64 sequence = nextSequence();
    ++skippedWhilePaused_;
    skippedNotificationPending_ = true;
    return sequence;
}

void TerminalInspectorEventModel::clear()
{
    ++clearEpoch_;
    pendingEvents_.clear();
    if (!events_.empty()) {
        beginResetModel();
        events_.clear();
        visible_.clear();
        endResetModel();
        Q_EMIT countChanged();
    }
    Q_EMIT cleared();
}

void TerminalInspectorEventModel::setPaused(bool paused)
{
    if (paused_ == paused) return;
    paused_ = paused;
    if (!paused_ && skippedNotificationPending_) {
        skippedNotificationPending_ = false;
        Q_EMIT skippedWhilePausedChanged();
    }
    Q_EMIT pausedChanged();
}

void TerminalInspectorEventModel::setFilterText(const QString &text)
{
    const QString boundedText = text.left(MaximumFilterLength);
    if (filterText_ == boundedText) return;
    filterText_ = boundedText;
    beginResetModel();
    rebuildVisible();
    endResetModel();
    Q_EMIT filterChanged();
    Q_EMIT countChanged();
}

void TerminalInspectorEventModel::setCategoryFilter(int category)
{
    const int normalized = category >= static_cast<int>(Category::Input)
            && category <= static_cast<int>(Category::State)
        ? category
        : -1;
    if (categoryFilter_ == normalized) return;
    categoryFilter_ = normalized;
    beginResetModel();
    rebuildVisible();
    endResetModel();
    Q_EMIT filterChanged();
    Q_EMIT countChanged();
}

bool TerminalInspectorEventModel::matches(const Event &event) const
{
    if (categoryFilter_ >= 0
        && categoryFilter_ != static_cast<int>(event.category)) {
        return false;
    }
    if (filterText_.isEmpty()) return true;
    const auto containsFilter = [this](QStringView value) {
        return value.contains(filterText_, Qt::CaseInsensitive);
    };
    return (event.traceId != 0
            && QStringLiteral("K#%1")
                   .arg(event.traceId)
                   .contains(filterText_, Qt::CaseInsensitive))
        || containsFilter(categoryName(event.category))
        || containsFilter(event.kind) || containsFilter(event.summary)
        || containsFilter(event.details);
}

const TerminalInspectorEventModel::Event *
TerminalInspectorEventModel::eventAt(int row) const
{
    return row >= 0 && row < visible_.size() ? visible_.at(row) : nullptr;
}

void TerminalInspectorEventModel::rebuildVisible()
{
    visible_.clear();
    visible_.reserve(static_cast<qsizetype>(events_.size()));
    for (auto iterator = events_.crbegin(); iterator != events_.crend();
         ++iterator) {
        if (matches(*iterator)) visible_.append(&*iterator);
    }
}

QString TerminalInspectorEventModel::categoryName(Category category)
{
    switch (category) {
    case Category::Input: return QStringLiteral("Input");
    case Category::Terminal: return QStringLiteral("Terminal");
    case Category::State: return QStringLiteral("State");
    }
    return QStringLiteral("Unknown");
}

QString TerminalInspectorEventModel::elapsedName(qint64 milliseconds)
{
    if (milliseconds < 1000) {
        return QStringLiteral("+%1 ms").arg(milliseconds);
    }
    return QStringLiteral("+%1 s").arg(
        static_cast<double>(milliseconds) / 1000.0, 0, 'f', 3);
}

QString TerminalInspectorEventModel::bounded(QString value, qsizetype maximum)
{
    if (value.size() <= maximum) return value;
    value.truncate(std::max<qsizetype>(qsizetype{0}, maximum - 1));
    value.append(QChar(0x2026));
    return value;
}

quint64 TerminalInspectorEventModel::nextSequence()
{
    quint64 sequence = 0;
    do {
        sequence = nextSequence_++;
    } while (sequence == 0);
    return sequence;
}
