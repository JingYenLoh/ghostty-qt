#include "ghostty_keybind_set.h"

#include <QTest>
#include <linux/input-event-codes.h>

#include <utility>

namespace {

using Disposition = GhosttyKeybindEntryDisposition;
using Reason = GhosttyKeybindUnsupportedReason;

GhosttyKeybindMatch requireMatch(
    std::optional<GhosttyKeybindMatch> match)
{
    if (!match.has_value()) {
        QTest::qFail("expected keybind match", __FILE__, __LINE__);
        return {};
    }
    return std::move(*match);
}

constexpr quint32 xkbKeycode(unsigned int evdevCode)
{
    return static_cast<quint32>(evdevCode + 8U);
}

GhosttyKeybindTrigger unicodeTrigger(quint32 codepoint,
                                     quint8 modifiers = 0)
{
    return {
        .kind = GhosttyKeybindKeyKind::Unicode,
        .unicodeCodepoint = codepoint,
        .modifiers = modifiers,
    };
}

GhosttyKeybindTrigger catchAllTrigger(quint8 modifiers = 0)
{
    return {
        .kind = GhosttyKeybindKeyKind::CatchAll,
        .modifiers = modifiers,
    };
}

GhosttyKeybindDefinition binding(
    QVector<GhosttyKeybindTrigger> sequence,
    QString action,
    GhosttyKeybindFlags flags = {})
{
    return {
        .sequence = std::move(sequence),
        .actions = {std::move(action)},
        .flags = flags,
    };
}

} // namespace

class GhosttyKeybindSetTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void parsesEqualsAndPlusTriggerDelimiters();
    void matchesNamedAndW3cPhysicalKeys();
    void physicalBindingsTakePrecedenceOverUnicode();
    void nativePhysicalBindingsAreLayoutIndependent();
    void distinguishesKeypadAndModifierLocations();
    void matchesShiftedUnicodeByUnshiftedCodepoint();
    void appendsAdjacentChainsInOrder();
    void preservesLocalFlags();
    void supportsSequencesAndCatchAllWhileRejectingDeferredForms();
    void advancesSharedPrefixSequences();
    void recoversInvalidSequencesAndHonorsCatchAll();
    void preservesLookupPriorityInsideSequences();
    void keepsSequenceStatePerMatcherInstance();
    void loadsStructuredDefinitionsAndRetainsFlags();
    void rejectsBroadStructuredSequences();
    void routesNamedTablesNewestToOldestAndRoot();
    void enforcesTableStackAndActivationRules();
    void handlesOneShotTablesExactly();
    void keepsTableSequenceAndCatchAllSemantics();
    void reloadClearsTablesAndSequences();
    void rejectsMalformedBindings();
    void replacesDuplicateTriggers();
    void matchesLinuxDefaultLikeBindings();
};

void GhosttyKeybindSetTest::parsesEqualsAndPlusTriggerDelimiters()
{
    GhosttyKeybindSet set;
    const GhosttyKeybindLoadReport report = set.load({
        QStringLiteral("ctrl+==increase_font_size:1"),
        QStringLiteral("ctrl++=increase_font_size:2"),
        QStringLiteral("=+alt=text:=hello"),
    });

    QCOMPARE(report.count(Disposition::Installed), 3);
    QCOMPARE(set.size(), 3);

    QCOMPARE(requireMatch(set.match(Qt::Key_Equal, Qt::ControlModifier,
                                    QStringLiteral("=")))
                 .actions,
             QStringList({QStringLiteral("increase_font_size:1")}));
    QCOMPARE(requireMatch(set.match(Qt::Key_Plus, Qt::ControlModifier,
                                    QStringLiteral("+")))
                 .actions,
             QStringList({QStringLiteral("increase_font_size:2")}));
    QVERIFY(!set.match(Qt::Key_Plus,
                       Qt::ControlModifier | Qt::ShiftModifier,
                       QStringLiteral("+")).has_value());
    QCOMPARE(requireMatch(set.match(Qt::Key_Equal, Qt::AltModifier,
                                    QStringLiteral("=")))
                 .actions,
             QStringList({QStringLiteral("text:=hello")}));
}

void GhosttyKeybindSetTest::matchesNamedAndW3cPhysicalKeys()
{
    GhosttyKeybindSet set;
    const GhosttyKeybindLoadReport report = set.load({
        QStringLiteral("ctrl+arrow_left=goto_split:left"),
        QStringLiteral("alt+F4=close_window"),
        QStringLiteral("shift+PageDown=scroll_page_down"),
        QStringLiteral("KeyA=physical_a"),
    });

    QCOMPARE(report.count(Disposition::Installed), 4);

    auto match = set.match(Qt::Key_Left, Qt::ControlModifier);
    QVERIFY(match.has_value());
    QVERIFY(match->physical);
    QCOMPARE(match->actions, QStringList({QStringLiteral("goto_split:left")}));

    match = set.match(Qt::Key_F4, Qt::AltModifier);
    QVERIFY(match.has_value());
    QVERIFY(match->physical);
    QCOMPARE(match->actions, QStringList({QStringLiteral("close_window")}));

    match = set.match(Qt::Key_PageDown, Qt::ShiftModifier);
    QVERIFY(match.has_value());
    QVERIFY(match->physical);
    QCOMPARE(match->actions,
             QStringList({QStringLiteral("scroll_page_down")}));

    match = set.match(Qt::Key_A, Qt::NoModifier, QStringLiteral("ф"));
    QVERIFY(match.has_value());
    QVERIFY(match->physical);
    QCOMPARE(match->actions, QStringList({QStringLiteral("physical_a")}));
}

void GhosttyKeybindSetTest::physicalBindingsTakePrecedenceOverUnicode()
{
    GhosttyKeybindSet set;
    const GhosttyKeybindLoadReport report = set.load({
        QStringLiteral("ctrl+a=unicode_action"),
        // Configured later is not what establishes priority: key kind does.
        QStringLiteral("ctrl+key_a=physical_action"),
    });
    QCOMPARE(report.count(Disposition::Installed), 2);

    const GhosttyKeybindMatch &match = requireMatch(
        set.match(Qt::Key_A, Qt::ControlModifier, QStringLiteral("A")));
    QVERIFY(match.physical);
    QCOMPARE(match.actions, QStringList({QStringLiteral("physical_action")}));

    // A different physical location can still select the Unicode binding.
    const GhosttyKeybindMatch &unicode = requireMatch(
        set.match(Qt::Key_B, Qt::ControlModifier, QStringLiteral("A")));
    QVERIFY(!unicode.physical);
    QCOMPARE(unicode.actions, QStringList({QStringLiteral("unicode_action")}));
}

void GhosttyKeybindSetTest::nativePhysicalBindingsAreLayoutIndependent()
{
    GhosttyKeybindSet set;
    QCOMPARE(set.load({QStringLiteral("ctrl+key_a=physical_a")})
                 .count(Disposition::Installed),
             1);

    // A layout can report a different logical Qt key and text for the physical
    // KeyA location. Its native XKB keycode remains KEY_A + 8.
    const GhosttyKeybindMatch &remapped = requireMatch(set.match(
        GhosttyKeybindEvent{
            .qtKey = Qt::Key_Q,
            .modifiers = Qt::ControlModifier,
            .text = QStringLiteral("q"),
            .nativeScanCode = xkbKeycode(KEY_A),
            .unshiftedCodepoint = 'q',
        }));
    QVERIFY(remapped.physical);
    QCOMPARE(remapped.actions,
             QStringList({QStringLiteral("physical_a")}));

    // The logical A produced by a different physical location must not match.
    QVERIFY(!set.match(GhosttyKeybindEvent{
        .qtKey = Qt::Key_A,
        .modifiers = Qt::ControlModifier,
        .text = QStringLiteral("a"),
        .nativeScanCode = xkbKeycode(KEY_Q),
        .unshiftedCodepoint = 'a',
    }).has_value());
}

void GhosttyKeybindSetTest::distinguishesKeypadAndModifierLocations()
{
    GhosttyKeybindSet set;
    const GhosttyKeybindLoadReport report = set.load({
        QStringLiteral("alt+digit_1=top_row"),
        QStringLiteral("alt+numpad_1=keypad"),
        QStringLiteral("control_left=left_control"),
        QStringLiteral("control_right=right_control"),
    });
    QCOMPARE(report.count(Disposition::Installed), 4);
    QCOMPARE(set.size(), 4);

    QCOMPARE(requireMatch(set.match(GhosttyKeybindEvent{
                 .qtKey = Qt::Key_1,
                 .modifiers = Qt::AltModifier,
                 .text = QStringLiteral("1"),
                 .nativeScanCode = xkbKeycode(KEY_1),
                 .unshiftedCodepoint = '1',
             })).actions,
             QStringList({QStringLiteral("top_row")}));
    QCOMPARE(requireMatch(set.match(GhosttyKeybindEvent{
                 .qtKey = Qt::Key_1,
                 .modifiers = Qt::AltModifier | Qt::KeypadModifier,
                 .text = QStringLiteral("1"),
                 .nativeScanCode = xkbKeycode(KEY_KP1),
                 .unshiftedCodepoint = '1',
             })).actions,
             QStringList({QStringLiteral("keypad")}));

    QCOMPARE(requireMatch(set.match(GhosttyKeybindEvent{
                 .qtKey = Qt::Key_Control,
                 .modifiers = Qt::ControlModifier,
                 .nativeScanCode = xkbKeycode(KEY_LEFTCTRL),
             })).actions,
             QStringList({QStringLiteral("left_control")}));
    QCOMPARE(requireMatch(set.match(GhosttyKeybindEvent{
                 .qtKey = Qt::Key_Control,
                 .modifiers = Qt::ControlModifier,
                 .nativeScanCode = xkbKeycode(KEY_RIGHTCTRL),
             })).actions,
             QStringList({QStringLiteral("right_control")}));

    // Synthetic events without native data retain a useful keypad fallback.
    QCOMPARE(requireMatch(set.match(Qt::Key_1, Qt::AltModifier,
                                    QStringLiteral("1"))).actions,
             QStringList({QStringLiteral("top_row")}));
    QCOMPARE(requireMatch(set.match(Qt::Key_1,
                                    Qt::AltModifier | Qt::KeypadModifier,
                                    QStringLiteral("1"))).actions,
             QStringList({QStringLiteral("keypad")}));
}

void GhosttyKeybindSetTest::matchesShiftedUnicodeByUnshiftedCodepoint()
{
    GhosttyKeybindSet set;
    QCOMPARE(set.load({QStringLiteral("ctrl+shift+,=reload_config")})
                 .count(Disposition::Installed),
             1);

    // Qt text carries '<', while Ghostty's Unicode trigger is the unshifted
    // comma codepoint at the same logical location.
    QVERIFY(!set.match(Qt::Key_Less,
                       Qt::ControlModifier | Qt::ShiftModifier,
                       QStringLiteral("<")).has_value());
    QCOMPARE(requireMatch(set.match(GhosttyKeybindEvent{
                 .qtKey = Qt::Key_Less,
                 .modifiers = Qt::ControlModifier | Qt::ShiftModifier,
                 .text = QStringLiteral("<"),
                 .nativeScanCode = xkbKeycode(KEY_COMMA),
                 .unshiftedCodepoint = ',',
             })).actions,
             QStringList({QStringLiteral("reload_config")}));

    QCOMPARE(set.load({QStringLiteral("ctrl+,=ignore")})
                 .count(Disposition::Installed),
             1);
    QVERIFY(!set.match(GhosttyKeybindEvent{
                 .qtKey = Qt::Key_Less,
                 .modifiers = Qt::ControlModifier | Qt::ShiftModifier,
                 .text = QStringLiteral("<"),
                 .nativeScanCode = xkbKeycode(KEY_COMMA),
                 .unshiftedCodepoint = ',',
             }).has_value());
}

void GhosttyKeybindSetTest::appendsAdjacentChainsInOrder()
{
    GhosttyKeybindSet set;
    const GhosttyKeybindLoadReport report = set.load({
        QStringLiteral("performable:ctrl+a=new_tab"),
        QStringLiteral("chain=goto_split:left"),
        QStringLiteral("chain=toggle_split_zoom"),
        QStringLiteral("ctrl+b=next_tab"),
        QStringLiteral("chain=close_tab:this"),
    });

    QCOMPARE(report.count(Disposition::Installed), 2);
    QCOMPARE(report.count(Disposition::Chained), 3);

    const GhosttyKeybindMatch &first = requireMatch(
        set.match(Qt::Key_A, Qt::ControlModifier, QStringLiteral("a")));
    QCOMPARE(first.actions,
             QStringList({QStringLiteral("new_tab"),
                          QStringLiteral("goto_split:left"),
                          QStringLiteral("toggle_split_zoom")}));
    QVERIFY(first.performable);

    const GhosttyKeybindMatch &second = requireMatch(
        set.match(Qt::Key_B, Qt::ControlModifier, QStringLiteral("b")));
    QCOMPARE(second.actions,
             QStringList({QStringLiteral("next_tab"),
                          QStringLiteral("close_tab:this")}));
    QVERIFY(!second.performable);
}

void GhosttyKeybindSetTest::preservesLocalFlags()
{
    GhosttyKeybindSet set;
    const GhosttyKeybindLoadReport report = set.load({
        QStringLiteral("performable:unconsumed:ctrl+shift+c=copy_to_clipboard:mixed"),
        QStringLiteral("shift+insert=paste_from_selection"),
    });

    QCOMPARE(report.count(Disposition::Installed), 2);
    const GhosttyKeybindMatch &copy = requireMatch(
        set.match(Qt::Key_C,
                  Qt::ControlModifier | Qt::ShiftModifier,
                  QStringLiteral("C")));
    QVERIFY(!copy.consumed);
    QVERIFY(copy.performable);

    const GhosttyKeybindMatch &paste = requireMatch(
        set.match(Qt::Key_Insert,
                  Qt::ShiftModifier | Qt::KeypadModifier));
    QVERIFY(paste.consumed);
    QVERIFY(!paste.performable);
    QVERIFY(paste.physical);
}

void GhosttyKeybindSetTest::supportsSequencesAndCatchAllWhileRejectingDeferredForms()
{
    GhosttyKeybindSet set;
    const GhosttyKeybindLoadReport report = set.load({
        QStringLiteral("ctrl+a>n=new_window"),
        QStringLiteral("copy-mode/ctrl+c=copy_to_clipboard"),
        QStringLiteral("copy-mode/"),
        QStringLiteral("ctrl+catch_all=ignore"),
        QStringLiteral("global:ctrl+g=toggle_quick_terminal"),
        QStringLiteral("all:ctrl+a=select_all"),
        // A chain cannot jump over an unsupported entry.
        QStringLiteral("chain=quit"),
    });

    QCOMPARE(set.size(), 2);
    QCOMPARE(report.count(Disposition::Installed), 2);
    QCOMPARE(report.count(Disposition::Unsupported), 5);
    QCOMPARE(report.records.at(0).disposition, Disposition::Installed);
    QCOMPARE(report.records.at(1).reason, Reason::KeyTable);
    QCOMPARE(report.records.at(2).reason, Reason::KeyTable);
    QCOMPARE(report.records.at(3).disposition, Disposition::Installed);
    QCOMPARE(report.records.at(4).reason, Reason::NonLocal);
    QCOMPARE(report.records.at(5).reason, Reason::NonLocal);
    QCOMPARE(report.records.at(6).reason, Reason::OrphanChain);

    GhosttyKeybindStep leader = set.advance({
        .qtKey = Qt::Key_A,
        .modifiers = Qt::ControlModifier,
        .text = QStringLiteral("a"),
    });
    QCOMPARE(leader.kind, GhosttyKeybindStepKind::Leader);
    const GhosttyKeybindStep leaf = set.advance({
        .qtKey = Qt::Key_N,
        .text = QStringLiteral("n"),
    });
    QCOMPARE(leaf.kind, GhosttyKeybindStepKind::Binding);
    QCOMPARE(leaf.match.actions, QStringList({QStringLiteral("new_window")}));

    const GhosttyKeybindStep catchAll = set.advance({
        .qtKey = Qt::Key_Z,
        .modifiers = Qt::ControlModifier,
        .text = QStringLiteral("z"),
    });
    QCOMPARE(catchAll.kind, GhosttyKeybindStepKind::Binding);
    QCOMPARE(catchAll.match.actions, QStringList({QStringLiteral("ignore")}));
}

void GhosttyKeybindSetTest::advancesSharedPrefixSequences()
{
    GhosttyKeybindSet set;
    QCOMPARE(set.load({
                 QStringLiteral("ctrl+a>b=new_tab"),
                 QStringLiteral("ctrl+a>c=next_tab"),
                 QStringLiteral("ctrl+a>d>e=previous_tab"),
             }).count(Disposition::Installed),
             3);

    GhosttyKeybindStep step = set.advance({
        .qtKey = Qt::Key_A,
        .modifiers = Qt::ControlModifier,
        .text = QStringLiteral("a"),
    });
    QCOMPARE(step.kind, GhosttyKeybindStepKind::Leader);
    QCOMPARE(step.queuedEvents.size(), 1);
    QVERIFY(set.sequenceActive());

    step = set.advance({.qtKey = Qt::Key_C, .text = QStringLiteral("c")});
    QCOMPARE(step.kind, GhosttyKeybindStepKind::Binding);
    QCOMPARE(step.match.actions, QStringList({QStringLiteral("next_tab")}));
    QCOMPARE(step.queuedEvents.size(), 1);
    QVERIFY(!set.sequenceActive());

    QCOMPARE(set.advance({
                 .qtKey = Qt::Key_A,
                 .modifiers = Qt::ControlModifier,
                 .text = QStringLiteral("a"),
             }).kind,
             GhosttyKeybindStepKind::Leader);
    step = set.advance({.qtKey = Qt::Key_D, .text = QStringLiteral("d")});
    QCOMPARE(step.kind, GhosttyKeybindStepKind::Leader);
    QCOMPARE(step.queuedEvents.size(), 2);
    step = set.advance({.qtKey = Qt::Key_E, .text = QStringLiteral("e")});
    QCOMPARE(step.kind, GhosttyKeybindStepKind::Binding);
    QCOMPARE(step.match.actions,
             QStringList({QStringLiteral("previous_tab")}));
    QCOMPARE(step.queuedEvents.size(), 2);
}

void GhosttyKeybindSetTest::recoversInvalidSequencesAndHonorsCatchAll()
{
    GhosttyKeybindSet set;
    (void) set.load({QStringLiteral("ctrl+a>b=new_tab")});
    const GhosttyKeybindEvent leader{
        .qtKey = Qt::Key_A,
        .modifiers = Qt::ControlModifier,
        .text = QStringLiteral("a"),
    };
    QCOMPARE(set.advance(leader).kind, GhosttyKeybindStepKind::Leader);

    // Modifier-only events do not cancel a pending sequence.
    QCOMPARE(set.advance({
                 .qtKey = Qt::Key_Shift,
                 .modifiers = Qt::ShiftModifier,
             }).kind,
             GhosttyKeybindStepKind::Unmatched);
    QVERIFY(set.sequenceActive());

    GhosttyKeybindStep step = set.advance({
        .qtKey = Qt::Key_Z,
        .text = QStringLiteral("z"),
    });
    QCOMPARE(step.kind, GhosttyKeybindStepKind::InvalidSequence);
    QCOMPARE(step.queuedEvents, QVector<GhosttyKeybindEvent>({leader}));
    QVERIFY(!set.sequenceActive());

    // A bare root catch_all=ignore is Ghostty's special invalid-sequence
    // guard. It drops the queued prefix and the invalid continuation.
    (void) set.load({
        QStringLiteral("ctrl+a>b=new_tab"),
        QStringLiteral("catch_all=ignore"),
    });
    QCOMPARE(set.advance(leader).kind, GhosttyKeybindStepKind::Leader);
    step = set.advance({.qtKey = Qt::Key_Z, .text = QStringLiteral("z")});
    QCOMPARE(step.kind, GhosttyKeybindStepKind::IgnoredSequence);
    QCOMPARE(step.queuedEvents, QVector<GhosttyKeybindEvent>({leader}));

    // A non-ignore root catch-all is not executed during recovery.
    (void) set.load({
        QStringLiteral("ctrl+a>b=new_tab"),
        QStringLiteral("catch_all=reload_config"),
    });
    QCOMPARE(set.advance(leader).kind, GhosttyKeybindStepKind::Leader);
    QCOMPARE(set.advance({.qtKey = Qt::Key_Z, .text = QStringLiteral("z")}).kind,
             GhosttyKeybindStepKind::InvalidSequence);

    // A catch-all inside the active sequence is an ordinary leaf.
    (void) set.load({QStringLiteral("ctrl+a>catch_all=reload_config")});
    QCOMPARE(set.advance(leader).kind, GhosttyKeybindStepKind::Leader);
    step = set.advance({.qtKey = Qt::Key_Z, .text = QStringLiteral("z")});
    QCOMPARE(step.kind, GhosttyKeybindStepKind::Binding);
    QCOMPARE(step.match.actions,
             QStringList({QStringLiteral("reload_config")}));
}

void GhosttyKeybindSetTest::preservesLookupPriorityInsideSequences()
{
    GhosttyKeybindSet set;
    (void) set.load({
        QStringLiteral("ctrl+a>b=unicode_leaf"),
        QStringLiteral("ctrl+a>key_b=physical_leaf"),
        QStringLiteral("ctrl+x>catch_all=bare_catch_all"),
        QStringLiteral("ctrl+x>ctrl+catch_all=modified_catch_all"),
    });

    QCOMPARE(set.advance({
                 .qtKey = Qt::Key_A,
                 .modifiers = Qt::ControlModifier,
                 .text = QStringLiteral("a"),
             }).kind,
             GhosttyKeybindStepKind::Leader);
    GhosttyKeybindStep step = set.advance({
        .qtKey = Qt::Key_B,
        .text = QStringLiteral("b"),
        .nativeScanCode = xkbKeycode(KEY_B),
        .unshiftedCodepoint = 'b',
    });
    QCOMPARE(step.kind, GhosttyKeybindStepKind::Binding);
    QVERIFY(step.match.physical);
    QCOMPARE(step.match.actions,
             QStringList({QStringLiteral("physical_leaf")}));

    QCOMPARE(set.advance({
                 .qtKey = Qt::Key_X,
                 .modifiers = Qt::ControlModifier,
                 .text = QStringLiteral("x"),
             }).kind,
             GhosttyKeybindStepKind::Leader);
    step = set.advance({
        .qtKey = Qt::Key_Z,
        .modifiers = Qt::ControlModifier,
        .text = QStringLiteral("z"),
    });
    QCOMPARE(step.match.actions,
             QStringList({QStringLiteral("modified_catch_all")}));

    QCOMPARE(set.advance({
                 .qtKey = Qt::Key_X,
                 .modifiers = Qt::ControlModifier,
                 .text = QStringLiteral("x"),
             }).kind,
             GhosttyKeybindStepKind::Leader);
    step = set.advance({.qtKey = Qt::Key_Z, .text = QStringLiteral("z")});
    QCOMPARE(step.match.actions,
             QStringList({QStringLiteral("bare_catch_all")}));
}

void GhosttyKeybindSetTest::keepsSequenceStatePerMatcherInstance()
{
    GhosttyKeybindSet first;
    GhosttyKeybindSet second;
    const QStringList config{QStringLiteral("ctrl+a>b=new_tab")};
    (void) first.load(config);
    (void) second.load(config);

    QCOMPARE(first.advance({
                 .qtKey = Qt::Key_A,
                 .modifiers = Qt::ControlModifier,
                 .text = QStringLiteral("a"),
             }).kind,
             GhosttyKeybindStepKind::Leader);
    QVERIFY(first.sequenceActive());
    QVERIFY(!second.sequenceActive());
    QCOMPARE(second.advance({.qtKey = Qt::Key_B,
                             .text = QStringLiteral("b")}).kind,
             GhosttyKeybindStepKind::Unmatched);
    first.resetSequence();
    QVERIFY(!first.sequenceActive());
}

void GhosttyKeybindSetTest::loadsStructuredDefinitionsAndRetainsFlags()
{
    GhosttyKeybindConfig config;
    config.root = {
        GhosttyKeybindDefinition{
            .sequence = {
                GhosttyKeybindTrigger{
                    .kind = GhosttyKeybindKeyKind::Unicode,
                    .unicodeCodepoint = 'x',
                    .modifiers = GhosttyKeybindCtrl,
                },
                GhosttyKeybindTrigger{
                    .kind = GhosttyKeybindKeyKind::Physical,
                    .physicalName = QStringLiteral("key_y"),
                },
            },
            .actions = {QStringLiteral("new_tab"),
                        QStringLiteral("goto_split:left")},
            .flags = {.consumed = false, .performable = true},
        },
        GhosttyKeybindDefinition{
            .sequence = {GhosttyKeybindTrigger{
                .kind = GhosttyKeybindKeyKind::CatchAll,
            }},
            .actions = {QStringLiteral("ignore")},
        },
        GhosttyKeybindDefinition{
            .sequence = {GhosttyKeybindTrigger{
                .kind = GhosttyKeybindKeyKind::Unicode,
                .unicodeCodepoint = 'g',
            }},
            .actions = {QStringLiteral("toggle_quick_terminal")},
            .flags = {.all = true},
        },
    };
    config.tables = {GhosttyKeybindTable{
        .name = QStringLiteral("copy-mode"),
        .bindings = {GhosttyKeybindDefinition{
            .sequence = {GhosttyKeybindTrigger{
                .kind = GhosttyKeybindKeyKind::Unicode,
                .unicodeCodepoint = 'c',
            }},
            .actions = {QStringLiteral("copy_to_clipboard:mixed")},
            .flags = {.global = true},
        }},
    }};

    GhosttyKeybindSet set;
    const GhosttyKeybindLoadReport report = set.load(config);
    QCOMPARE(report.count(Disposition::Installed), 4);
    QCOMPARE(report.count(Disposition::Unsupported), 0);
    QCOMPARE(set.size(), 4);
    QVERIFY(set.hasTable(QStringLiteral("copy-mode")));

    const GhosttyKeybindMatch all = requireMatch(
        set.match(Qt::Key_G, Qt::NoModifier, QStringLiteral("g")));
    QVERIFY(all.all);
    QVERIFY(!all.global);

    QCOMPARE(set.advance({
                 .qtKey = Qt::Key_X,
                 .modifiers = Qt::ControlModifier,
                 .text = QStringLiteral("x"),
             }).kind,
             GhosttyKeybindStepKind::Leader);
    const GhosttyKeybindStep step = set.advance({
        .qtKey = Qt::Key_Y,
        .text = QStringLiteral("y"),
        .nativeScanCode = xkbKeycode(KEY_Y),
    });
    QCOMPARE(step.kind, GhosttyKeybindStepKind::Binding);
    QCOMPARE(step.match.actions,
             QStringList({QStringLiteral("new_tab"),
                          QStringLiteral("goto_split:left")}));
    QVERIFY(!step.match.consumed);
    QVERIFY(step.match.performable);

    QVERIFY(set.activateTable(QStringLiteral("copy-mode")));
    const GhosttyKeybindStep table = set.advance({
        .qtKey = Qt::Key_C,
        .text = QStringLiteral("c"),
    });
    QCOMPARE(table.kind, GhosttyKeybindStepKind::Binding);
    QCOMPARE(table.match.actions,
             QStringList({QStringLiteral("copy_to_clipboard:mixed")}));
    QVERIFY(!table.match.all);
    QVERIFY(table.match.global);

    const QStringList actions = set.serializedActions();
    QVERIFY(actions.contains(QStringLiteral("toggle_quick_terminal")));
    QVERIFY(actions.contains(QStringLiteral("copy_to_clipboard:mixed")));
}

void GhosttyKeybindSetTest::rejectsBroadStructuredSequences()
{
    GhosttyKeybindConfig config;
    config.root = {
        binding({unicodeTrigger('a'), unicodeTrigger('b')},
                QStringLiteral("new_tab"), {.all = true}),
        binding({unicodeTrigger('g'), unicodeTrigger('h')},
                QStringLiteral("toggle_visibility"), {.global = true}),
        binding({unicodeTrigger('v')}, QStringLiteral("new_tab"),
                {.all = true}),
    };

    GhosttyKeybindSet set;
    const GhosttyKeybindLoadReport report = set.load(config);
    QCOMPARE(report.count(Disposition::Invalid), 2);
    QCOMPARE(report.count(Disposition::Installed), 1);
    QCOMPARE(set.size(), 1);

    const GhosttyKeybindMatch match = requireMatch(
        set.match(Qt::Key_V, Qt::NoModifier, QStringLiteral("v")));
    QVERIFY(match.all);
}

void GhosttyKeybindSetTest::routesNamedTablesNewestToOldestAndRoot()
{
    GhosttyKeybindConfig config;
    config.root = {
        binding({unicodeTrigger('q')}, QStringLiteral("root_q")),
        binding({unicodeTrigger('r')}, QStringLiteral("root_r")),
    };
    config.tables = {
        GhosttyKeybindTable{
            .name = QStringLiteral("A"),
            .bindings = {
                binding({unicodeTrigger('q')}, QStringLiteral("table_a_q")),
                binding({unicodeTrigger('a')}, QStringLiteral("table_a_a")),
            },
        },
        GhosttyKeybindTable{
            .name = QStringLiteral("B"),
            .bindings = {
                binding({unicodeTrigger('b')}, QStringLiteral("table_b_b")),
            },
        },
        GhosttyKeybindTable{
            .name = QStringLiteral("C"),
            .bindings = {
                binding({catchAllTrigger()}, QStringLiteral("table_c_any")),
            },
        },
    };

    GhosttyKeybindSet set;
    QCOMPARE(set.load(config).count(Disposition::Installed), 6);
    QVERIFY(set.activateTable(QStringLiteral("A")));
    QVERIFY(set.activateTable(QStringLiteral("B")));
    QCOMPARE(set.activeTableNames(),
             QStringList({QStringLiteral("A"), QStringLiteral("B")}));

    GhosttyKeybindStep step = set.advance({
        .qtKey = Qt::Key_Q,
        .text = QStringLiteral("q"),
    });
    QCOMPARE(step.match.actions,
             QStringList({QStringLiteral("table_a_q")}));

    step = set.advance({.qtKey = Qt::Key_B, .text = QStringLiteral("b")});
    QCOMPARE(step.match.actions,
             QStringList({QStringLiteral("table_b_b")}));

    step = set.advance({.qtKey = Qt::Key_R, .text = QStringLiteral("r")});
    QCOMPARE(step.match.actions, QStringList({QStringLiteral("root_r")}));

    QVERIFY(set.activateTable(QStringLiteral("C")));
    step = set.advance({.qtKey = Qt::Key_Q, .text = QStringLiteral("q")});
    QCOMPARE(step.match.actions,
             QStringList({QStringLiteral("table_c_any")}));
    QCOMPARE(set.activeTableNames(),
             QStringList({QStringLiteral("A"), QStringLiteral("B"),
                          QStringLiteral("C")}));
}

void GhosttyKeybindSetTest::enforcesTableStackAndActivationRules()
{
    GhosttyKeybindConfig config;
    config.tables = {
        GhosttyKeybindTable{.name = QStringLiteral("A")},
        GhosttyKeybindTable{.name = QStringLiteral("B")},
    };

    GhosttyKeybindSet set;
    (void) set.load(config);
    QVERIFY(set.hasTable(QStringLiteral("A")));
    QVERIFY(!set.hasTable(QStringLiteral("missing")));
    QVERIFY(!set.activateTable(QStringLiteral("missing")));
    QVERIFY(set.canActivateTable(QStringLiteral("A")));
    QVERIFY(set.activateTable(QStringLiteral("A")));
    QVERIFY(!set.canActivateTable(QStringLiteral("A")));
    QVERIFY(!set.activateTable(QStringLiteral("A")));
    QVERIFY(set.activateTable(QStringLiteral("B")));
    QVERIFY(set.activateTable(QStringLiteral("A")));
    QCOMPARE(set.activeTableNames(),
             QStringList({QStringLiteral("A"), QStringLiteral("B"),
                          QStringLiteral("A")}));
    QVERIFY(set.deactivateTable());
    QCOMPARE(set.activeTableNames(),
             QStringList({QStringLiteral("A"), QStringLiteral("B")}));
    QVERIFY(set.deactivateAllTables());
    QVERIFY(!set.deactivateAllTables());
    QVERIFY(!set.deactivateTable());

    for (qsizetype index = 0;
         index < GhosttyKeybindSet::MaximumActiveTables; ++index) {
        const QString name = index % 2 == 0
            ? QStringLiteral("A") : QStringLiteral("B");
        QVERIFY(set.activateTable(name));
    }
    QCOMPARE(set.activeTableNames().size(),
             GhosttyKeybindSet::MaximumActiveTables);
    QVERIFY(!set.canActivateTable(QStringLiteral("A")));
    QVERIFY(!set.activateTable(QStringLiteral("A")));
}

void GhosttyKeybindSetTest::handlesOneShotTablesExactly()
{
    GhosttyKeybindConfig config;
    config.root = {
        binding({unicodeTrigger('r')}, QStringLiteral("root")),
    };
    config.tables = {
        GhosttyKeybindTable{
            .name = QStringLiteral("A"),
            .bindings = {
                binding({unicodeTrigger('a')}, QStringLiteral("table_a")),
                binding({unicodeTrigger('p')}, QStringLiteral("performable"),
                        GhosttyKeybindFlags{.performable = true}),
            },
        },
        GhosttyKeybindTable{
            .name = QStringLiteral("B"),
            .bindings = {
                binding({unicodeTrigger('b')}, QStringLiteral("table_b")),
            },
        },
        GhosttyKeybindTable{
            .name = QStringLiteral("C"),
            .bindings = {
                binding({catchAllTrigger()}, QStringLiteral("table_c_any")),
            },
        },
    };

    GhosttyKeybindSet set;
    (void) set.load(config);
    QVERIFY(set.activateTable(QStringLiteral("A"), true));
    QCOMPARE(set.advance({.qtKey = Qt::Key_R, .text = QStringLiteral("r")})
                 .match.actions,
             QStringList({QStringLiteral("root")}));
    QCOMPARE(set.activeTableNames(), QStringList({QStringLiteral("A")}));
    QCOMPARE(set.advance({.qtKey = Qt::Key_A, .text = QStringLiteral("a")})
                 .match.actions,
             QStringList({QStringLiteral("table_a")}));
    QVERIFY(set.activeTableNames().isEmpty());

    QVERIFY(set.activateTable(QStringLiteral("C"), true));
    QCOMPARE(set.advance({.qtKey = Qt::Key_Q, .text = QStringLiteral("q")})
                 .match.actions,
             QStringList({QStringLiteral("table_c_any")}));
    QVERIFY(set.activeTableNames().isEmpty());

    QVERIFY(set.activateTable(QStringLiteral("A"), true));
    QVERIFY(set.activateTable(QStringLiteral("B")));
    QCOMPARE(set.advance({.qtKey = Qt::Key_A, .text = QStringLiteral("a")})
                 .match.actions,
             QStringList({QStringLiteral("table_a")}));
    QCOMPARE(set.activeTableNames(),
             QStringList({QStringLiteral("A"), QStringLiteral("B")}));
    QVERIFY(set.deactivateTable());
    QCOMPARE(set.advance({.qtKey = Qt::Key_A, .text = QStringLiteral("a")})
                 .match.actions,
             QStringList({QStringLiteral("table_a")}));
    QVERIFY(set.activeTableNames().isEmpty());

    QVERIFY(set.activateTable(QStringLiteral("A"), true));
    const GhosttyKeybindStep performable = set.advance({
        .qtKey = Qt::Key_P,
        .text = QStringLiteral("p"),
    });
    QCOMPARE(performable.kind, GhosttyKeybindStepKind::Binding);
    QVERIFY(performable.match.performable);
    QVERIFY(set.activeTableNames().isEmpty());
}

void GhosttyKeybindSetTest::keepsTableSequenceAndCatchAllSemantics()
{
    const GhosttyKeybindFlags flags{
        .consumed = false,
        .performable = true,
    };
    GhosttyKeybindConfig config;
    config.tables = {
        GhosttyKeybindTable{
            .name = QStringLiteral("persistent"),
            .bindings = {
                binding({unicodeTrigger('x'), unicodeTrigger('n')},
                        QStringLiteral("persistent_sequence"), flags),
                binding({catchAllTrigger()}, QStringLiteral("ignore")),
            },
        },
        GhosttyKeybindTable{
            .name = QStringLiteral("shadow"),
            .bindings = {
                binding({catchAllTrigger()}, QStringLiteral("reload_config")),
            },
        },
        GhosttyKeybindTable{
            .name = QStringLiteral("one-shot"),
            .bindings = {
                binding({unicodeTrigger('x'), unicodeTrigger('n')},
                        QStringLiteral("one_shot_sequence")),
                binding({catchAllTrigger()}, QStringLiteral("ignore")),
            },
        },
    };

    GhosttyKeybindSet set;
    (void) set.load(config);
    QVERIFY(set.activateTable(QStringLiteral("persistent")));
    QCOMPARE(set.advance({.qtKey = Qt::Key_X, .text = QStringLiteral("x")})
                 .kind,
             GhosttyKeybindStepKind::Leader);
    QVERIFY(set.activateTable(QStringLiteral("shadow")));
    QCOMPARE(set.advance({.qtKey = Qt::Key_Z, .text = QStringLiteral("z")})
                 .kind,
             GhosttyKeybindStepKind::InvalidSequence);
    QCOMPARE(set.activeTableNames(),
             QStringList({QStringLiteral("persistent"),
                          QStringLiteral("shadow")}));

    QVERIFY(set.deactivateTable());
    QCOMPARE(set.advance({.qtKey = Qt::Key_X, .text = QStringLiteral("x")})
                 .kind,
             GhosttyKeybindStepKind::Leader);
    QCOMPARE(set.advance({.qtKey = Qt::Key_Z, .text = QStringLiteral("z")})
                 .kind,
             GhosttyKeybindStepKind::IgnoredSequence);
    QCOMPARE(set.activeTableNames(),
             QStringList({QStringLiteral("persistent")}));

    QCOMPARE(set.advance({.qtKey = Qt::Key_X, .text = QStringLiteral("x")})
                 .kind,
             GhosttyKeybindStepKind::Leader);
    const GhosttyKeybindStep leaf = set.advance({
        .qtKey = Qt::Key_N,
        .text = QStringLiteral("n"),
    });
    QCOMPARE(leaf.kind, GhosttyKeybindStepKind::Binding);
    QVERIFY(!leaf.match.consumed);
    QVERIFY(!leaf.match.all);
    QVERIFY(!leaf.match.global);
    QVERIFY(leaf.match.performable);
    QCOMPARE(set.activeTableNames(),
             QStringList({QStringLiteral("persistent")}));

    QVERIFY(set.deactivateTable());
    QVERIFY(set.activateTable(QStringLiteral("one-shot"), true));
    QCOMPARE(set.advance({.qtKey = Qt::Key_X, .text = QStringLiteral("x")})
                 .kind,
             GhosttyKeybindStepKind::Leader);
    QVERIFY(set.activeTableNames().isEmpty());
    QVERIFY(set.sequenceActive());
    QCOMPARE(set.advance({.qtKey = Qt::Key_N, .text = QStringLiteral("n")})
                 .match.actions,
             QStringList({QStringLiteral("one_shot_sequence")}));

    QVERIFY(set.activateTable(QStringLiteral("one-shot"), true));
    QCOMPARE(set.advance({.qtKey = Qt::Key_X, .text = QStringLiteral("x")})
                 .kind,
             GhosttyKeybindStepKind::Leader);
    QVERIFY(set.activeTableNames().isEmpty());
    QCOMPARE(set.advance({.qtKey = Qt::Key_Z, .text = QStringLiteral("z")})
                 .kind,
             GhosttyKeybindStepKind::InvalidSequence);
}

void GhosttyKeybindSetTest::reloadClearsTablesAndSequences()
{
    GhosttyKeybindConfig initial;
    initial.tables = {GhosttyKeybindTable{
        .name = QStringLiteral("A"),
        .bindings = {
            binding({unicodeTrigger('x'), unicodeTrigger('n')},
                    QStringLiteral("sequence")),
        },
    }};

    GhosttyKeybindSet set;
    (void) set.load(initial);
    QVERIFY(set.activateTable(QStringLiteral("A")));
    QCOMPARE(set.advance({.qtKey = Qt::Key_X, .text = QStringLiteral("x")})
                 .kind,
             GhosttyKeybindStepKind::Leader);
    QVERIFY(set.sequenceActive());

    GhosttyKeybindConfig replacement;
    replacement.tables = {
        GhosttyKeybindTable{.name = QStringLiteral("B")},
    };
    (void) set.load(replacement);
    QVERIFY(!set.sequenceActive());
    QVERIFY(set.activeTableNames().isEmpty());
    QVERIFY(!set.hasTable(QStringLiteral("A")));
    QVERIFY(set.hasTable(QStringLiteral("B")));
}

void GhosttyKeybindSetTest::rejectsMalformedBindings()
{
    GhosttyKeybindSet set;
    const GhosttyKeybindLoadReport report = set.load({
        QStringLiteral("ctrl+ctrl+a=quit"),
        QStringLiteral("ctrl+not_a_key=quit"),
        QStringLiteral("ctrl+a"),
        QStringLiteral("ctrl+a="),
        QStringLiteral("performable:performable:ctrl+a=quit"),
        QStringLiteral("ctrl+a+b=quit"),
    });

    QCOMPARE(set.size(), 0);
    QCOMPARE(report.count(Disposition::Invalid), 6);
    for (const GhosttyKeybindParseRecord &entry : report.records) {
        QCOMPARE(entry.reason, Reason::None);
        QVERIFY(!entry.detail.isEmpty());
    }
}

void GhosttyKeybindSetTest::replacesDuplicateTriggers()
{
    GhosttyKeybindSet set;
    const GhosttyKeybindLoadReport report = set.load({
        QStringLiteral("ctrl+A=first"),
        QStringLiteral("unconsumed:ctrl+a=second"),
        QStringLiteral("chain=third"),
    });

    QCOMPARE(report.count(Disposition::Installed), 2);
    QCOMPARE(set.size(), 1);
    const GhosttyKeybindMatch &match = requireMatch(
        set.match(Qt::Key_A, Qt::ControlModifier, QStringLiteral("a")));
    QCOMPARE(match.actions,
             QStringList({QStringLiteral("second"), QStringLiteral("third")}));
    QVERIFY(!match.consumed);
}

void GhosttyKeybindSetTest::matchesLinuxDefaultLikeBindings()
{
    GhosttyKeybindSet set;
    const GhosttyKeybindLoadReport report = set.load({
        QStringLiteral("copy=copy_to_clipboard:mixed"),
        QStringLiteral("ctrl+shift+tab=previous_tab"),
        QStringLiteral("ctrl+tab=next_tab"),
        QStringLiteral("ctrl+shift+t=new_tab"),
        QStringLiteral("ctrl+alt+arrow_down=goto_split:down"),
        QStringLiteral("alt+digit_1=goto_tab:physical"),
        QStringLiteral("alt+1=goto_tab:unicode"),
        QStringLiteral("escape=end_search"),
        QStringLiteral("ctrl+enter=toggle_fullscreen"),
    });

    QCOMPARE(report.count(Disposition::Installed), 9);

    QCOMPARE(requireMatch(set.match(
                 Qt::Key_Backtab,
                 Qt::ControlModifier | Qt::ShiftModifier))
                 .actions,
             QStringList({QStringLiteral("previous_tab")}));
    QCOMPARE(requireMatch(set.match(Qt::Key_Tab, Qt::ControlModifier)).actions,
             QStringList({QStringLiteral("next_tab")}));
    QCOMPARE(requireMatch(set.match(
                 Qt::Key_T,
                 Qt::ControlModifier | Qt::ShiftModifier,
                 QString(QChar(0x14))))
                 .actions,
             QStringList({QStringLiteral("new_tab")}));
    QCOMPARE(requireMatch(set.match(
                 Qt::Key_Down,
                 Qt::ControlModifier | Qt::AltModifier))
                 .actions,
             QStringList({QStringLiteral("goto_split:down")}));

    // Both bindings exist in Ghostty's Linux defaults. Physical digit_1 wins.
    const GhosttyKeybindMatch &digit = requireMatch(
        set.match(Qt::Key_1, Qt::AltModifier, QStringLiteral("1")));
    QVERIFY(digit.physical);
    QCOMPARE(digit.actions, QStringList({QStringLiteral("goto_tab:physical")}));

    QCOMPARE(requireMatch(set.match(Qt::Key_Escape, Qt::NoModifier)).actions,
             QStringList({QStringLiteral("end_search")}));
    QCOMPARE(requireMatch(set.match(Qt::Key_Return, Qt::ControlModifier)).actions,
             QStringList({QStringLiteral("toggle_fullscreen")}));
}

QTEST_APPLESS_MAIN(GhosttyKeybindSetTest)

#include "test_ghostty_keybind_set.moc"
