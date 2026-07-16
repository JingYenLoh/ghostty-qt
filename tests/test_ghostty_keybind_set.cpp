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
    void marksUnsupportedFormsWithoutInstallingThem();
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

void GhosttyKeybindSetTest::marksUnsupportedFormsWithoutInstallingThem()
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

    QCOMPARE(set.size(), 0);
    QCOMPARE(report.count(Disposition::Unsupported), 7);
    QCOMPARE(report.records.at(0).reason, Reason::Sequence);
    QCOMPARE(report.records.at(1).reason, Reason::KeyTable);
    QCOMPARE(report.records.at(2).reason, Reason::KeyTable);
    QCOMPARE(report.records.at(3).reason, Reason::CatchAll);
    QCOMPARE(report.records.at(4).reason, Reason::NonLocal);
    QCOMPARE(report.records.at(5).reason, Reason::NonLocal);
    QCOMPARE(report.records.at(6).reason, Reason::OrphanChain);
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
