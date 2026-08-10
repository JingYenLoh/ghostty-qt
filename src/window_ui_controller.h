#pragma once

#include "application_shell_options.h"
#include "workspace_ids.h"

#include <QAbstractListModel>
#include <QObject>
#include <QString>
#include <QStringView>
#include <QVector>
#include <QtQmlIntegration/qqmlintegration.h>

#include <chrono>
#include <deque>
#include <functional>
#include <optional>
#include <variant>

using CommandPaletteCommand = std::variant<QString, SurfaceTarget>;

// Runtime palette rows are deliberately separate from configuration entries:
// some rows target a live surface and therefore have no serializable Ghostty
// action or action key.
struct CommandPaletteRow {
    QString title;
    QString description;
    QString actionKey;
    CommandPaletteCommand command;

    bool operator==(const CommandPaletteRow &) const = default;
};

class CommandPaletteModel final : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex WRITE setSelectedIndex
                   NOTIFY selectedIndexChanged)

public:
    enum Role {
        TitleRole = Qt::UserRole + 1,
        DescriptionRole,
        ActionKeyRole,
    };
    Q_ENUM(Role)

    explicit CommandPaletteModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(visibleRows_.size()); }
    QString filter() const { return filter_; }
    void setFilter(const QString &filter);
    int selectedIndex() const { return selectedIndex_; }
    void setSelectedIndex(int index);

    void replaceRows(QVector<CommandPaletteRow> rows);
    void replaceEntries(QVector<CommandPaletteEntry> entries);
    const CommandPaletteRow *rowAt(int visibleRow) const;
    std::optional<CommandPaletteCommand> commandAt(int visibleRow) const;
    std::optional<CommandPaletteCommand> selectedCommand() const;
    Q_INVOKABLE void selectRelative(int delta);

Q_SIGNALS:
    void countChanged();
    void filterChanged();
    void selectedIndexChanged();

private:
    struct PreparedEntry {
        CommandPaletteRow row;
        QString sortKey;
        QString foldedTitle;
        QString foldedActionKey;

        bool operator==(const PreparedEntry &) const = default;
    };

    static PreparedEntry prepare(CommandPaletteRow row);
    static bool lessThan(const PreparedEntry &left, const PreparedEntry &right);
    static bool matches(const PreparedEntry &entry, QStringView foldedFilter);
    void rebuildVisibleRows(
        const std::optional<CommandPaletteCommand> &previousSelection,
        bool preserveSelection);
    void updateSelectedIndex(int selectedIndex);

    QVector<PreparedEntry> entries_;
    QVector<int> visibleRows_;
    QString filter_;
    int selectedIndex_ = -1;
};

class WindowUiController final : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(Modal modal READ modal NOTIFY modalChanged)
    Q_PROPERTY(bool commandPaletteVisible READ commandPaletteVisible NOTIFY
                   commandPaletteVisibleChanged)
    Q_PROPERTY(bool tabOverviewVisible READ tabOverviewVisible NOTIFY
                   tabOverviewVisibleChanged)
    Q_PROPERTY(bool configurationDiagnosticsVisible READ
                   configurationDiagnosticsVisible NOTIFY
                       configurationDiagnosticsVisibleChanged)
    Q_PROPERTY(
        QString configurationDiagnosticsText READ configurationDiagnosticsText
            NOTIFY configurationDiagnosticsChanged)
    Q_PROPERTY(CommandPaletteModel *commandPaletteModel READ commandPaletteModel
                   CONSTANT)
    Q_PROPERTY(bool toastVisible READ toastVisible NOTIFY toastChanged)
    Q_PROPERTY(QString toastMessage READ toastMessage NOTIFY toastChanged)
    Q_PROPERTY(int toastDurationMilliseconds READ toastDurationMilliseconds
                   NOTIFY toastChanged)
    Q_PROPERTY(quint64 toastRevision READ toastRevision NOTIFY toastChanged)
    Q_PROPERTY(
        int toastQueueDepth READ toastQueueDepth NOTIFY toastQueueDepthChanged)

public:
    using CommandPaletteRefreshCallback = std::function<void()>;
    using CommandPaletteActivationCallback =
        std::function<void(CommandPaletteCommand)>;
    using ConfigurationRetryCallback = std::function<void()>;

    enum class Modal {
        None,
        CommandPalette,
        TabOverview,
        ConfigurationDiagnostics,
    };
    Q_ENUM(Modal)

    enum class ToastKind {
        ClipboardCopied,
        ClipboardCleared,
        ConfigurationReloaded,
    };
    Q_ENUM(ToastKind)

    static constexpr int MaximumToastQueueDepth = 8;
    // Match Kirigami's short passive-notification timeout. These messages are
    // brief confirmations without actions, so its ordinary seven-second
    // timeout would keep them visible longer than necessary.
    static constexpr std::chrono::milliseconds DefaultToastDuration{4000};

    explicit WindowUiController(QObject *parent = nullptr);

    Modal modal() const { return modal_; }
    bool commandPaletteVisible() const
    {
        return modal_ == Modal::CommandPalette;
    }
    bool tabOverviewVisible() const { return modal_ == Modal::TabOverview; }
    bool configurationDiagnosticsVisible() const
    {
        return modal_ == Modal::ConfigurationDiagnostics;
    }
    const QString &configurationDiagnosticsText() const
    {
        return configurationDiagnosticsText_;
    }
    CommandPaletteModel *commandPaletteModel() { return &commandPaletteModel_; }
    const CommandPaletteModel *commandPaletteModel() const
    {
        return &commandPaletteModel_;
    }

    void replaceCommandPaletteEntries(QVector<CommandPaletteEntry> entries);
    void replaceCommandPaletteRows(QVector<CommandPaletteRow> rows);
    void
    setCommandPaletteRefreshCallback(CommandPaletteRefreshCallback callback);
    void setCommandPaletteActivationCallback(
        CommandPaletteActivationCallback callback);
    void setConfigurationRetryCallback(ConfigurationRetryCallback callback);
    void setConfigurationDiagnostics(QString diagnostics);
    Q_INVOKABLE void showCommandPalette();
    Q_INVOKABLE void showTabOverview();
    Q_INVOKABLE void toggleCommandPalette();
    Q_INVOKABLE void toggleTabOverview();
    Q_INVOKABLE void closeModal();
    Q_INVOKABLE bool activateSelectedCommand();
    Q_INVOKABLE void ignoreConfigurationDiagnostics();
    Q_INVOKABLE bool retryConfigurationDiagnostics();

    bool toastVisible() const { return !toasts_.empty(); }
    QString toastMessage() const;
    int toastDurationMilliseconds() const;
    quint64 toastRevision() const { return toastRevision_; }
    int toastQueueDepth() const { return static_cast<int>(toasts_.size()); }

    void
    enqueueToast(ToastKind kind,
                 std::chrono::milliseconds duration = DefaultToastDuration);
    void
    enqueueToast(QString message,
                 std::chrono::milliseconds duration = DefaultToastDuration);
    void notifyClipboardCopied(bool clipboardIsEmpty);
    void notifyConfigurationReloaded();
    // The presentation layer owns scheduling. Keeping expiry explicit makes
    // queue behavior deterministic and lets QML restart its timer whenever the
    // front item changes.
    Q_INVOKABLE bool expireToast();
    void clearToasts();

Q_SIGNALS:
    void modalChanged();
    void commandPaletteVisibleChanged();
    void tabOverviewVisibleChanged();
    void configurationDiagnosticsVisibleChanged();
    void configurationDiagnosticsChanged();
    void toastChanged();
    void toastQueueDepthChanged();

private:
    struct Toast {
        QString message;
        std::chrono::milliseconds duration;
    };

    static QString toastMessage(ToastKind kind);
    bool refreshCommandPalette();
    void clearPendingPaletteCommand();
    void setModal(Modal modal);
    void advanceToastRevision();

    CommandPaletteModel commandPaletteModel_{this};
    CommandPaletteRefreshCallback commandPaletteRefreshCallback_;
    CommandPaletteActivationCallback commandPaletteActivationCallback_;
    ConfigurationRetryCallback configurationRetryCallback_;
    QString configurationDiagnosticsText_;
    std::optional<CommandPaletteCommand> pendingPaletteCommand_;
    std::deque<Toast> toasts_;
    quint64 toastRevision_ = 0;
    Modal modal_ = Modal::None;
};
