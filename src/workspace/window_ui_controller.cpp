#include "workspace/window_ui_controller.h"

#include <QPointer>
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
    const CommandPaletteRow *row = rowAt(index.row());
    if (row == nullptr || index.column() != 0) {
        return {};
    }

    switch (role) {
    case TitleRole: return row->title;
    case DescriptionRole: return row->description;
    case ActionKeyRole: return row->actionKey;
    default: return {};
    }
}

QHash<int, QByteArray> CommandPaletteModel::roleNames() const
{
    return {
        {TitleRole, QByteArrayLiteral("title")},
        {DescriptionRole, QByteArrayLiteral("description")},
        {ActionKeyRole, QByteArrayLiteral("actionKey")},
    };
}

void CommandPaletteModel::setFilter(const QString &filter)
{
    if (filter_ == filter) {
        return;
    }
    const auto previousSelection = selectedCommand();
    filter_ = filter;
    Q_EMIT filterChanged();
    rebuildVisibleRows(previousSelection, false);
}

void CommandPaletteModel::setSelectedIndex(int index)
{
    const int clamped = count() == 0 ? -1 : std::clamp(index, 0, count() - 1);
    updateSelectedIndex(clamped);
}

void CommandPaletteModel::replaceRows(QVector<CommandPaletteRow> rows)
{
    QVector<PreparedEntry> prepared;
    prepared.reserve(rows.size());
    std::ranges::transform(
        rows, std::back_inserter(prepared),
        [](CommandPaletteRow &row) { return prepare(std::move(row)); });
    std::ranges::sort(prepared, lessThan);
    if (entries_ == prepared) {
        return;
    }
    const auto preferredSelection = selectedCommand();
    entries_ = std::move(prepared);
    rebuildVisibleRows(preferredSelection, true);
}

void CommandPaletteModel::replaceEntries(QVector<CommandPaletteEntry> entries)
{
    QVector<CommandPaletteRow> rows;
    rows.reserve(entries.size());
    std::ranges::transform(entries, std::back_inserter(rows),
                           [](CommandPaletteEntry &entry) {
                               return CommandPaletteRow{
                                   .title = std::move(entry.title),
                                   .description = std::move(entry.description),
                                   .actionKey = std::move(entry.actionKey),
                                   .command = std::move(entry.action),
                               };
                           });
    replaceRows(std::move(rows));
}

const CommandPaletteRow *CommandPaletteModel::rowAt(int visibleRow) const
{
    if (visibleRow < 0 || visibleRow >= visibleRows_.size()) {
        return nullptr;
    }
    return &entries_.at(visibleRows_.at(visibleRow)).row;
}

std::optional<CommandPaletteCommand>
CommandPaletteModel::commandAt(int visibleRow) const
{
    const CommandPaletteRow *row = rowAt(visibleRow);
    if (row == nullptr) {
        return std::nullopt;
    }
    return row->command;
}

std::optional<CommandPaletteCommand>
CommandPaletteModel::selectedCommand() const
{
    return commandAt(selectedIndex_);
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
CommandPaletteModel::prepare(CommandPaletteRow row)
{
    if (std::holds_alternative<SurfaceTarget>(row.command)) {
        row.actionKey.clear();
    }

    QString sortKey = commandSortKey(row.title);
    QString foldedTitle = row.title.toCaseFolded();
    QString foldedActionKey = row.actionKey.toCaseFolded();
    return {
        .row = std::move(row),
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

    if (const int order = compareStrings(left.row.title, right.row.title);
        order != 0) {
        return order < 0;
    }

    if (left.row.command.index() != right.row.command.index()) {
        return left.row.command.index() < right.row.command.index();
    }

    if (const auto *leftAction = std::get_if<QString>(&left.row.command)) {
        const auto &rightAction = std::get<QString>(right.row.command);
        // Ghostty's regular-command comparison treats case-insensitive title
        // ties as equivalent. Complete that ordering so config replacement is
        // deterministic regardless of parser/container iteration order.
        if (const int order = compareStrings(
                left.row.actionKey, right.row.actionKey, Qt::CaseInsensitive);
            order != 0) {
            return order < 0;
        }
        if (const int order =
                compareStrings(left.row.actionKey, right.row.actionKey);
            order != 0) {
            return order < 0;
        }
        if (const int order = compareStrings(*leftAction, rightAction);
            order != 0) {
            return order < 0;
        }
    } else {
        const auto &leftTarget = std::get<SurfaceTarget>(left.row.command);
        const auto &rightTarget = std::get<SurfaceTarget>(right.row.command);
        if (leftTarget != rightTarget) {
            return leftTarget < rightTarget;
        }
    }

    return compareStrings(left.row.description, right.row.description) < 0;
}

bool CommandPaletteModel::matches(const PreparedEntry &entry,
                                  QStringView foldedFilter)
{
    return foldedFilter.isEmpty() || entry.foldedTitle.contains(foldedFilter)
        || entry.foldedActionKey.contains(foldedFilter);
}

void CommandPaletteModel::rebuildVisibleRows(
    const std::optional<CommandPaletteCommand> &previousSelection,
    bool preserveSelection)
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
    if (preserveSelection && previousSelection.has_value()) {
        const auto selected =
            std::ranges::find_if(std::as_const(visibleRows_), [&](int row) {
                return entries_.at(row).row.command == *previousSelection;
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
}

void CommandPaletteModel::updateSelectedIndex(int selectedIndex)
{
    if (selectedIndex_ == selectedIndex) {
        return;
    }
    selectedIndex_ = selectedIndex;
    Q_EMIT selectedIndexChanged();
}

WindowUiController::WindowUiController(QObject *parent)
    : QObject(parent)
{}

void WindowUiController::replaceCommandPaletteEntries(
    QVector<CommandPaletteEntry> entries)
{
    commandPaletteModel_.replaceEntries(std::move(entries));
}

void WindowUiController::replaceCommandPaletteRows(
    QVector<CommandPaletteRow> rows)
{
    commandPaletteModel_.replaceRows(std::move(rows));
}

void WindowUiController::setCommandPaletteRefreshCallback(
    CommandPaletteRefreshCallback callback)
{
    commandPaletteRefreshCallback_ = std::move(callback);
}

void WindowUiController::setCommandPaletteActivationCallback(
    CommandPaletteActivationCallback callback)
{
    commandPaletteActivationCallback_ = std::move(callback);
}

void WindowUiController::setConfigurationRetryCallback(
    ConfigurationRetryCallback callback)
{
    configurationRetryCallback_ = std::move(callback);
}

void WindowUiController::setConfigurationDiagnostics(QString diagnostics)
{
    if (configurationDiagnosticsText_ == diagnostics) {
        return;
    }

    configurationDiagnosticsText_ = std::move(diagnostics);
    const QPointer<WindowUiController> guard(this);
    Q_EMIT configurationDiagnosticsChanged();
    if (guard == nullptr) return;

    if (configurationDiagnosticsText_.isEmpty()) {
        if (configurationDiagnosticsVisible()) {
            setModal(Modal::None);
        }
        return;
    }
    setModal(Modal::ConfigurationDiagnostics);
}

void WindowUiController::showCommandPalette()
{
    if (commandPaletteVisible()) {
        return;
    }
    clearPendingPaletteCommand();
    if (!refreshCommandPalette()) {
        return;
    }
    setModal(Modal::CommandPalette);
}

void WindowUiController::showTabOverview()
{
    setModal(Modal::TabOverview);
}

void WindowUiController::toggleCommandPalette()
{
    if (commandPaletteVisible()) {
        clearPendingPaletteCommand();
        setModal(Modal::None);
        return;
    }
    showCommandPalette();
}

void WindowUiController::toggleTabOverview()
{
    setModal(tabOverviewVisible() ? Modal::None : Modal::TabOverview);
}

void WindowUiController::closeModal()
{
    if (commandPaletteVisible()) {
        pendingPaletteCommand_ = commandPaletteModel_.selectedCommand();
    }
    setModal(Modal::None);
}

bool WindowUiController::activateSelectedCommand()
{
    auto command = std::exchange(pendingPaletteCommand_, std::nullopt);
    if (!command.has_value() && commandPaletteVisible()) {
        command = commandPaletteModel_.selectedCommand();
        setModal(Modal::None);
    }
    if (!command.has_value() || !commandPaletteActivationCallback_) {
        return false;
    }

    // Copy the callback before invoking it: activation may synchronously close
    // the window and destroy this controller.
    auto callback = commandPaletteActivationCallback_;
    callback(std::move(*command));
    return true;
}

void WindowUiController::ignoreConfigurationDiagnostics()
{
    if (configurationDiagnosticsVisible()) {
        setModal(Modal::None);
    }
}

bool WindowUiController::retryConfigurationDiagnostics()
{
    if (!configurationDiagnosticsVisible()
        || configurationDiagnosticsText_.isEmpty()
        || !configurationRetryCallback_) {
        return false;
    }

    // A retry is only a request. Keep the current errors visible until the
    // corresponding successful service publications clear them.
    const QPointer<WindowUiController> guard(this);
    auto callback = configurationRetryCallback_;
    callback();
    return guard != nullptr;
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

    // A single reload_config action publishes the shared and frontend
    // configuration domains independently. Collapse consecutive identical
    // publications so their indistinguishable presentations do not look like
    // one toast whose timeout never expires. The same rule also bounds bursts
    // of repeated clipboard confirmations without reordering distinct events.
    if (!toasts_.empty() && toasts_.back().message == message
        && toasts_.back().duration == duration) {
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

bool WindowUiController::refreshCommandPalette()
{
    if (!commandPaletteRefreshCallback_) {
        return true;
    }

    QPointer guard(this);
    auto callback = commandPaletteRefreshCallback_;
    callback();
    return !guard.isNull();
}

void WindowUiController::clearPendingPaletteCommand()
{
    pendingPaletteCommand_.reset();
}

void WindowUiController::setModal(Modal modal)
{
    if (modal_ == modal) {
        return;
    }

    const bool paletteWasVisible = commandPaletteVisible();
    const bool overviewWasVisible = tabOverviewVisible();
    const bool diagnosticsWereVisible = configurationDiagnosticsVisible();
    modal_ = modal;
    Q_EMIT modalChanged();
    if (paletteWasVisible != commandPaletteVisible()) {
        Q_EMIT commandPaletteVisibleChanged();
    }
    if (overviewWasVisible != tabOverviewVisible()) {
        Q_EMIT tabOverviewVisibleChanged();
    }
    if (diagnosticsWereVisible != configurationDiagnosticsVisible()) {
        Q_EMIT configurationDiagnosticsVisibleChanged();
    }
}

void WindowUiController::advanceToastRevision()
{
    ++toastRevision_;
    Q_EMIT toastChanged();
}
