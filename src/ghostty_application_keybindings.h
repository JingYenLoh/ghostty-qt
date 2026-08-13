#pragma once

#include "application_action.h"
#include "ghostty_keybind_set.h"
#include "key_event_snapshot.h"
#include "keyboard_layout.h"
#include "launch_options.h"
#include "modifier_remap.h"
#include "terminal_action_result.h"
#include "workspace_ids.h"

#include <QInputMethodEvent>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QVector>

#include <deque>
#include <memory>
#include <variant>

class ApplicationController;
class GhosttyGlobalShortcutPortal;
class TerminalPane;
class TerminalWorkspace;

// Process-level keybinding coordination. Root application actions are checked
// before a focused pane's table stack, while all:/global: actions fan out over
// every registered surface. Per-pane table/sequence state deliberately remains
// in TerminalPane.
class GhosttyApplicationKeybindings final : public QObject {
    Q_OBJECT

public:
    explicit GhosttyApplicationKeybindings(
        const LaunchOptions &options, bool enableGlobalShortcutsPortal = true,
        QObject *parent = nullptr);
    ~GhosttyApplicationKeybindings() override;

    void registerWorkspace(TerminalWorkspace *workspace);
    [[nodiscard]] GhosttyKeybindProgram
    applyLaunchOptions(const LaunchOptions &options);
    [[nodiscard]] GhosttyKeybindProgram keybindProgram() const noexcept
    {
        return rootState_.program();
    }

    // Shared by focused all:/global: matches and XDG portal activations.
    void dispatchBroadActions(const QStringList &actions);

Q_SIGNALS:
    // Process actions deliberately have no workspace dependency. The
    // application controller can therefore execute them while resident with
    // zero windows.
    void applicationActionRequested(ApplicationAction action);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    friend class ApplicationController;

    struct DeferredKeyEvent {
        QPointer<QObject> target;
        KeyEventSnapshot event;
        KeyboardLayoutTranslation layout;
        bool composing = false;
    };
    struct DeferredInputMethodEvent {
        QPointer<QObject> target;
        QString preedit;
        QList<QInputMethodEvent::Attribute> attributes;
        QString commit;
        int replacementStart = 0;
        int replacementLength = 0;
        bool composingAfter = false;
    };
    using DeferredInput =
        std::variant<DeferredKeyEvent, DeferredInputMethodEvent,
                     GhosttyCompiledActionChain>;
    struct SurfaceTarget {
        // Keep the raw workspace address solely as a removal key after its
        // QPointer has already observed QObject destruction.
        TerminalWorkspace *workspaceIdentity = nullptr;
        QPointer<TerminalWorkspace> workspace;
        PaneId paneId;
        QPointer<TerminalPane> pane;
    };
    struct BroadExecution;

    void beginConfigurationUpdate() noexcept;
    void endConfigurationUpdate();
    [[nodiscard]] bool
    deferredCompositionState(const TerminalPane &pane) const noexcept;
    void dispatchOrDeferBroadActions(GhosttyCompiledActionChain actions);
    void drainDeferredInputs();
    void appendSurface(TerminalWorkspace *workspace, PaneId paneId,
                       TerminalPane *pane);
    void removeSurface(TerminalWorkspace *workspace, PaneId paneId);
    void transferSurface(TerminalWorkspace *workspace, PaneId paneId,
                         TerminalPane *pane, TerminalWorkspace *destination);
    void removeWorkspaceSurfaces(TerminalWorkspace *workspace);
    void swapRemoveSurface(qsizetype index);
    [[nodiscard]] QVector<SurfaceTarget> surfaceSnapshot() const;
    [[nodiscard]] bool surfaceTargetIsLive(const SurfaceTarget &target) const;
    bool executeApplicationActions(const GhosttyCompiledActionChain &actions);
    void
    dispatchCompiledBroadActions(const GhosttyCompiledActionChain &actions);
    void continueBroadExecution();
    void resolveBroadTarget(quint64 generation, qsizetype targetIndex,
                            TerminalActionExecutionResult result,
                            bool deferContinuation = false);
    void deferBroadExecutionContinuation(quint64 generation);
    void resumeReadyBroadExecution();
    [[nodiscard]] static bool
    broadExecutionHasLostTarget(const BroadExecution &execution);

    GhosttyKeybindState rootState_;
    ModifierRemapTracker modifierRemaps_;
    QSet<TerminalWorkspace *> workspaces_;
    QVector<SurfaceTarget> surfaces_;
    QSet<quint64> consumedKeys_;
    std::deque<DeferredInput> deferredInputs_;
    int configurationUpdateDepth_ = 0;
    int keyEventDispatchDepth_ = 0;
    bool drainingDeferredInputs_ = false;
    const QKeyEvent *replayingDeferredKeyEvent_ = nullptr;
    const QInputMethodEvent *replayingDeferredInputMethodEvent_ = nullptr;
    quint64 nextBroadExecutionGeneration_ = 0;
    std::shared_ptr<BroadExecution> broadExecution_;
    std::unique_ptr<GhosttyGlobalShortcutPortal> portal_;
};
