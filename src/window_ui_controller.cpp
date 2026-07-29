#include "window_ui_controller.h"

#include <QVariant>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <ranges>
#include <utility>

namespace {

int compareStrings(const QString &left, const QString &right,
                   Qt::CaseSensitivity sensitivity = Qt::CaseSensitive)
{
    return QString::compare(left, right, sensitivity);
}

QString commandSortKey(const QString &title)
{
    QString key = title;
    key.replace(u':', u'\t');
    return key.toCaseFolded();
}

} // namespace

CommandPaletteModel::CommandPaletteModel(QObject *parent)
    : QAbstractListModel(parent)
{}

int CommandPaletteModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant CommandPaletteModel::data(const QModelIndex &index, int role) const
{
    const CommandPaletteEntry *entry = entryAt(index.row());
    if (entry == nullptr || index.column() != 0) {
        return {};
    }

    switch (role) {
    case TitleRole: return entry->title;
    case DescriptionRole: return entry->description;
    case ActionKeyRole: return entry->actionKey;
    case ActionRole: return entry->action;
    default: return {};
    }
}

QHash<int, QByteArray> CommandPaletteModel::roleNames() const
{
    return {
        {TitleRole, QByteArrayLiteral("title")},
        {DescriptionRole, QByteArrayLiteral("description")},
        {ActionKeyRole, QByteArrayLiteral("actionKey")},
        {ActionRole, QByteArrayLiteral("action")},
    };
}

void CommandPaletteModel::setFilter(const QString &filter)
{
    if (filter_ == filter) {
        return;
    }
    const QString previousSelectedAction = selectedAction();
    filter_ = filter;
    Q_EMIT filterChanged();
    rebuildVisibleRows(previousSelectedAction, false);
}

void CommandPaletteModel::setSelectedIndex(int index)
{
    const int clamped = count() == 0 ? -1 : std::clamp(index, 0, count() - 1);
    updateSelectedIndex(clamped);
}

QString CommandPaletteModel::selectedAction() const
{
    return actionAt(selectedIndex_);
}

void CommandPaletteModel::replaceEntries(QVector<CommandPaletteEntry> entries)
{
    QVector<PreparedEntry> prepared;
    prepared.reserve(entries.size());
    std::ranges::transform(
        entries, std::back_inserter(prepared),
        [](CommandPaletteEntry &entry) { return prepare(std::move(entry)); });
    std::ranges::sort(prepared, lessThan);
    if (entries_ == prepared) {
        return;
    }
    const QString preferredAction = selectedAction();
    entries_ = std::move(prepared);
    rebuildVisibleRows(preferredAction, true);
}

const CommandPaletteEntry *CommandPaletteModel::entryAt(int visibleRow) const
{
    if (visibleRow < 0 || visibleRow >= visibleRows_.size()) {
        return nullptr;
    }
    return &entries_.at(visibleRows_.at(visibleRow)).entry;
}

QString CommandPaletteModel::actionAt(int visibleRow) const
{
    const CommandPaletteEntry *entry = entryAt(visibleRow);
    return entry != nullptr ? entry->action : QString{};
}

void CommandPaletteModel::selectRelative(int delta)
{
    if (count() == 0 || delta == 0) {
        return;
    }

    const int origin = selectedIndex_ < 0 ? 0 : selectedIndex_;
    const qint64 candidate =
        static_cast<qint64>(origin) + static_cast<qint64>(delta);
    const qint64 modulus = static_cast<qint64>(count());
    const int wrapped =
        static_cast<int>(((candidate % modulus) + modulus) % modulus);
    updateSelectedIndex(wrapped);
}

CommandPaletteModel::PreparedEntry
CommandPaletteModel::prepare(CommandPaletteEntry entry)
{
    QString sortKey = commandSortKey(entry.title);
    QString foldedTitle = entry.title.toCaseFolded();
    QString foldedActionKey = entry.actionKey.toCaseFolded();
    return {
        .entry = std::move(entry),
        .sortKey = std::move(sortKey),
        .foldedTitle = std::move(foldedTitle),
        .foldedActionKey = std::move(foldedActionKey),
    };
}

bool CommandPaletteModel::lessThan(const PreparedEntry &left,
                                   const PreparedEntry &right)
{
    if (const int order = compareStrings(left.sortKey, right.sortKey);
        order != 0) {
        return order < 0;
    }

    // Ghostty's regular-command comparison treats case-insensitive title ties
    // as equivalent. Complete that ordering so config replacement produces a
    // deterministic model independent of parser/container iteration order.
    if (const int order = compareStrings(left.entry.title, right.entry.title);
        order != 0) {
        return order < 0;
    }
    if (const int order = compareStrings(
            left.entry.actionKey, right.entry.actionKey, Qt::CaseInsensitive);
        order != 0) {
        return order < 0;
    }
    if (const int order =
            compareStrings(left.entry.actionKey, right.entry.actionKey);
        order != 0) {
        return order < 0;
    }
    if (const int order = compareStrings(left.entry.action, right.entry.action);
        order != 0) {
        return order < 0;
    }
    return compareStrings(left.entry.description, right.entry.description) < 0;
}

bool CommandPaletteModel::matches(const PreparedEntry &entry,
                                  QStringView foldedFilter)
{
    return foldedFilter.isEmpty() || entry.foldedTitle.contains(foldedFilter)
        || entry.foldedActionKey.contains(foldedFilter);
}

void CommandPaletteModel::rebuildVisibleRows(
    const QString &previousSelectedAction, bool preserveSelectedAction)
{
    const int previousCount = count();
    const int previousSelectedIndex = selectedIndex_;

    beginResetModel();
    visibleRows_.clear();
    visibleRows_.reserve(entries_.size());
    const QString foldedFilter = filter_.toCaseFolded();
    for (const int row :
         std::views::iota(0, static_cast<int>(entries_.size()))) {
        if (matches(entries_.at(row), foldedFilter)) {
            visibleRows_.append(row);
        }
    }
    selectedIndex_ = visibleRows_.isEmpty() ? -1 : 0;
    if (preserveSelectedAction && !previousSelectedAction.isEmpty()) {
        const auto selected =
            std::ranges::find_if(std::as_const(visibleRows_), [&](int row) {
                return entries_.at(row).entry.action == previousSelectedAction;
            });
        if (selected != visibleRows_.cend()) {
            selectedIndex_ = static_cast<int>(
                std::distance(visibleRows_.cbegin(), selected));
        }
    }
    endResetModel();

    if (previousCount != count()) {
        Q_EMIT countChanged();
    }
    if (previousSelectedIndex != selectedIndex_) {
        Q_EMIT selectedIndexChanged();
    }
    if (previousSelectedAction != selectedAction()) {
        Q_EMIT selectedActionChanged();
    }
}

void CommandPaletteModel::updateSelectedIndex(int selectedIndex)
{
    if (selectedIndex_ == selectedIndex) {
        return;
    }
    selectedIndex_ = selectedIndex;
    Q_EMIT selectedIndexChanged();
    Q_EMIT selectedActionChanged();
}

WindowUiController::WindowUiController(QObject *parent)
    : QObject(parent)
{}

void WindowUiController::replaceCommandPaletteEntries(
    QVector<CommandPaletteEntry> entries)
{
    commandPaletteModel_.replaceEntries(std::move(entries));
}

void WindowUiController::showCommandPalette()
{
    setModal(Modal::CommandPalette);
}

void WindowUiController::showTabOverview()
{
    setModal(Modal::TabOverview);
}

void WindowUiController::toggleCommandPalette()
{
    setModal(commandPaletteVisible() ? Modal::None : Modal::CommandPalette);
}

void WindowUiController::toggleTabOverview()
{
    setModal(tabOverviewVisible() ? Modal::None : Modal::TabOverview);
}

void WindowUiController::closeModal()
{
    setModal(Modal::None);
}

QString WindowUiController::toastMessage() const
{
    return toasts_.empty() ? QString{} : toasts_.front().message;
}

int WindowUiController::toastDurationMilliseconds() const
{
    if (toasts_.empty()) {
        return 0;
    }
    constexpr qint64 maximum = std::numeric_limits<int>::max();
    return static_cast<int>(
        std::min<qint64>(toasts_.front().duration.count(), maximum));
}

void WindowUiController::enqueueToast(ToastKind kind,
                                      std::chrono::milliseconds duration)
{
    enqueueToast(toastMessage(kind), duration);
}

void WindowUiController::enqueueToast(QString message,
                                      std::chrono::milliseconds duration)
{
    if (message.isEmpty() || duration <= std::chrono::milliseconds::zero()) {
        return;
    }

    const bool wasEmpty = toasts_.empty();
    const std::size_t previousDepth = toasts_.size();
    if (toasts_.size() == static_cast<std::size_t>(MaximumToastQueueDepth)) {
        // Never interrupt the toast already being presented. Evict the oldest
        // pending item so a notification storm remains bounded while recent
        // state changes are still eventually visible.
        toasts_.erase(std::next(toasts_.begin()));
    }
    toasts_.push_back(Toast{
        .message = std::move(message),
        .duration = duration,
    });
    if (previousDepth != toasts_.size()) {
        Q_EMIT toastQueueDepthChanged();
    }

    if (wasEmpty) {
        advanceToastRevision();
    }
}

void WindowUiController::notifyClipboardCopied(bool clipboardIsEmpty)
{
    enqueueToast(clipboardIsEmpty ? ToastKind::ClipboardCleared
                                  : ToastKind::ClipboardCopied);
}

void WindowUiController::notifyConfigurationReloaded()
{
    enqueueToast(ToastKind::ConfigurationReloaded);
}

bool WindowUiController::expireToast()
{
    if (toasts_.empty()) {
        return false;
    }

    toasts_.pop_front();
    Q_EMIT toastQueueDepthChanged();
    advanceToastRevision();
    return true;
}

void WindowUiController::clearToasts()
{
    if (toasts_.empty()) {
        return;
    }

    toasts_.clear();
    Q_EMIT toastQueueDepthChanged();
    advanceToastRevision();
}

QString WindowUiController::toastMessage(ToastKind kind)
{
    switch (kind) {
    case ToastKind::ClipboardCopied:
        return QStringLiteral("Copied to clipboard");
    case ToastKind::ClipboardCleared:
        return QStringLiteral("Cleared clipboard");
    case ToastKind::ConfigurationReloaded:
        return QStringLiteral("Reloaded the configuration");
    }
    Q_UNREACHABLE_RETURN({});
}

void WindowUiController::setModal(Modal modal)
{
    if (modal_ == modal) {
        return;
    }

    const bool paletteWasVisible = commandPaletteVisible();
    const bool overviewWasVisible = tabOverviewVisible();
    modal_ = modal;
    Q_EMIT modalChanged();
    if (paletteWasVisible != commandPaletteVisible()) {
        Q_EMIT commandPaletteVisibleChanged();
    }
    if (overviewWasVisible != tabOverviewVisible()) {
        Q_EMIT tabOverviewVisibleChanged();
    }
}

void WindowUiController::advanceToastRevision()
{
    ++toastRevision_;
    Q_EMIT toastChanged();
}
