#pragma once

#include "ghostty_keybind_set.h"
#include "launch_options.h"

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QVector>

#include <memory>

class GhosttyGlobalShortcutPortal;
class TerminalWorkspace;

// Process-level keybinding coordination. Root application actions are checked
// before a focused pane's table stack, while all:/global: actions fan out over
// every registered surface. Per-pane table/sequence state deliberately remains
// in TerminalPane.
class GhosttyApplicationKeybindings final : public QObject {
    Q_OBJECT

public:
    explicit GhosttyApplicationKeybindings(
        const LaunchOptions &options,
        bool enableGlobalShortcutsPortal = true,
        QObject *parent = nullptr);
    ~GhosttyApplicationKeybindings() override;

    void registerWorkspace(TerminalWorkspace *workspace);
    void applyLaunchOptions(const LaunchOptions &options);

    // Shared by focused all:/global: matches and XDG portal activations.
    void dispatchBroadActions(const QStringList &actions);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    TerminalWorkspace *activeWorkspace() const;
    QVector<QPointer<TerminalWorkspace>> workspaceSnapshot() const;
    bool executeApplicationActions(const QStringList &actions);

    GhosttyKeybindSet rootBindings_;
    QVector<QPointer<TerminalWorkspace>> workspaces_;
    QSet<quint64> consumedKeys_;
    std::unique_ptr<GhosttyGlobalShortcutPortal> portal_;
};
