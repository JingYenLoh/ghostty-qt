#include "input/ghostty_application_keybindings.h"
#include "input/modifier_remap.h"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QSignalSpy>
#include <QTest>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <cstdint>
#include <span>
#include <utility>

namespace {

constexpr quint32 xkbKeycode(std::uint32_t evdevCode)
{
    return static_cast<quint32>(evdevCode + 8U);
}

ModifierRemap mapping(ModifierKey fromKey, ModifierSide fromSide,
                      ModifierKey toKey, ModifierSide toSide)
{
    return {
        .from =
            {
                .key = fromKey,
                .side = fromSide,
            },
        .to =
            {
                .key = toKey,
                .side = toSide,
            },
    };
}

KeyEventSnapshot snapshot(bool pressed, int key,
                          Qt::KeyboardModifiers modifiers,
                          quint32 nativeScanCode = 0, bool autoRepeat = false)
{
    return {
        .pressed = pressed,
        .key = key,
        .modifiers = modifiers,
        .nativeScanCode = nativeScanCode,
        .nativeVirtualKey = 0,
        .nativeModifiers = 0,
        .text = {},
        .autoRepeat = autoRepeat,
        .count = 1,
    };
}

GhosttyKeybindDefinition rootBinding(quint32 codepoint, quint8 modifiers,
                                     QString action)
{
    return {
        .sequence = {GhosttyKeybindTrigger{
            .kind = GhosttyKeybindKeyKind::Unicode,
            .unicodeCodepoint = codepoint,
            .modifiers = modifiers,
        }},
        .actions = {std::move(action)},
    };
}

class KeyReceiver final : public QObject {
public:
    int presses = 0;
    int releases = 0;

protected:
    bool event(QEvent *event) override
    {
        if (event != nullptr && event->type() == QEvent::KeyPress) {
            ++presses;
        } else if (event != nullptr && event->type() == QEvent::KeyRelease) {
            ++releases;
        }
        return QObject::event(event);
    }
};

} // namespace

class ModifierRemapTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void appliesFinalizedSidedAndUnsidedInputs();
    void changesOnlyTheFirstSimultaneousSource();
    void doesNotApplyMappingsTransitively();
    void preservesAnAlreadyHeldDestinationSide();
    void handlesModifierPressRepeatAndRelease();
    void preservesLayoutTextNativeIdentityAndUnrelatedFlags();
    void replacementTakesEffectWithoutRetainedState();
    void trackerRetainsSideAcrossChordAndPrunesMissedRelease();
    void usesXkbRemappedModifierIdentityBeforeConfigRemaps();
    void trackerReplacementClearsObservedSide();
    void rootMatchingUsesTheSameAtomicRemapGeneration();
};

void ModifierRemapTest::appliesFinalizedSidedAndUnsidedInputs()
{
    // Ghostty expands an unsided source into right and left entries, then
    // finalization orders entries with a right source before left sources.
    const QVector mappings{
        mapping(ModifierKey::Ctrl, ModifierSide::Right, ModifierKey::Super,
                ModifierSide::Left),
        mapping(ModifierKey::Ctrl, ModifierSide::Left, ModifierKey::Super,
                ModifierSide::Left),
    };
    const ModifierRemapEngine engine(mappings);

    const auto ordinary =
        engine.remapModifiers(snapshot(true, Qt::Key_A, Qt::ControlModifier));
    QVERIFY(!ordinary.contains(ModifierKey::Ctrl));
    QVERIFY(ordinary.contains(ModifierKey::Super));
    QCOMPARE(ordinary.side(ModifierKey::Super), ModifierSide::Left);

    const auto right = engine.remapModifiers(snapshot(
        true, Qt::Key_Control, Qt::NoModifier, xkbKeycode(KEY_RIGHTCTRL)));
    QVERIFY(!right.contains(ModifierKey::Ctrl));
    QVERIFY(right.contains(ModifierKey::Super));
    QCOMPARE(right.side(ModifierKey::Super), ModifierSide::Left);

    const ModifierRemapEngine leftOnly(QVector{mappings.constLast()});
    const auto unmatchedRight = leftOnly.remapModifiers(snapshot(
        true, Qt::Key_Control, Qt::NoModifier, xkbKeycode(KEY_RIGHTCTRL)));
    QVERIFY(unmatchedRight.contains(ModifierKey::Ctrl));
    QVERIFY(!unmatchedRight.contains(ModifierKey::Super));
    QCOMPARE(unmatchedRight.side(ModifierKey::Ctrl), ModifierSide::Right);
}

void ModifierRemapTest::changesOnlyTheFirstSimultaneousSource()
{
    const ModifierRemapEngine engine(QVector{
        mapping(ModifierKey::Ctrl, ModifierSide::Left, ModifierKey::Super,
                ModifierSide::Left),
        mapping(ModifierKey::Alt, ModifierSide::Left, ModifierKey::Shift,
                ModifierSide::Left),
    });
    const auto result = engine.remap(ModifierRemapState{
        .modifiers = Qt::ControlModifier | Qt::AltModifier,
        .rightSides = 0,
    });

    QVERIFY(!result.contains(ModifierKey::Ctrl));
    QVERIFY(result.contains(ModifierKey::Super));
    QVERIFY(result.contains(ModifierKey::Alt));
    QVERIFY(!result.contains(ModifierKey::Shift));
}

void ModifierRemapTest::doesNotApplyMappingsTransitively()
{
    const ModifierRemapEngine engine(QVector{
        mapping(ModifierKey::Alt, ModifierSide::Left, ModifierKey::Ctrl,
                ModifierSide::Left),
        mapping(ModifierKey::Ctrl, ModifierSide::Left, ModifierKey::Super,
                ModifierSide::Left),
    });
    const auto result = engine.remap(ModifierRemapState{
        .modifiers = Qt::AltModifier,
        .rightSides = 0,
    });

    QVERIFY(!result.contains(ModifierKey::Alt));
    QVERIFY(result.contains(ModifierKey::Ctrl));
    QVERIFY(!result.contains(ModifierKey::Super));
}

void ModifierRemapTest::preservesAnAlreadyHeldDestinationSide()
{
    const ModifierRemapEngine engine(QVector{
        mapping(ModifierKey::Ctrl, ModifierSide::Left, ModifierKey::Alt,
                ModifierSide::Left),
    });
    const auto result = engine.remap(ModifierRemapState{
        .modifiers = Qt::ControlModifier | Qt::AltModifier,
        .rightSides = 1U << 2U,
    });

    QVERIFY(!result.contains(ModifierKey::Ctrl));
    QVERIFY(result.contains(ModifierKey::Alt));
    // Ghostty ORs the destination value after clearing only the source.
    // A left target therefore cannot erase an already-held right target.
    QCOMPARE(result.side(ModifierKey::Alt), ModifierSide::Right);
}

void ModifierRemapTest::handlesModifierPressRepeatAndRelease()
{
    const ModifierRemapEngine engine(QVector{
        mapping(ModifierKey::Ctrl, ModifierSide::Right, ModifierKey::Alt,
                ModifierSide::Right),
    });
    const quint32 rightControl = xkbKeycode(KEY_RIGHTCTRL);

    const auto press = engine.remapModifiers(
        snapshot(true, Qt::Key_Control, Qt::NoModifier, rightControl));
    QVERIFY(!press.contains(ModifierKey::Ctrl));
    QVERIFY(press.contains(ModifierKey::Alt));
    QCOMPARE(press.side(ModifierKey::Alt), ModifierSide::Right);

    const auto repeat = engine.remapModifiers(snapshot(
        true, Qt::Key_Control, Qt::ControlModifier, rightControl, true));
    QCOMPARE(repeat, press);

    const auto release = engine.remapModifiers(
        snapshot(false, Qt::Key_Control, Qt::ControlModifier, rightControl));
    QVERIFY(!release.contains(ModifierKey::Ctrl));
    QVERIFY(!release.contains(ModifierKey::Alt));
}

void ModifierRemapTest::preservesLayoutTextNativeIdentityAndUnrelatedFlags()
{
    const ModifierRemapEngine engine(QVector{
        mapping(ModifierKey::Alt, ModifierSide::Left, ModifierKey::Ctrl,
                ModifierSide::Right),
    });
    KeyEventSnapshot input =
        snapshot(true, Qt::Key_A, Qt::AltModifier | Qt::KeypadModifier, 38);
    input.text = QString::fromUtf8("å");
    input.nativeVirtualKey = 0x1234;
    input.nativeModifiers = 0x5678;
    input.count = 3;

    const KeyEventSnapshot output = engine.remap(input);
    QCOMPARE(output.modifiers,
             Qt::KeyboardModifiers(Qt::ControlModifier | Qt::KeypadModifier));
    QCOMPARE(output.text, input.text);
    QCOMPARE(output.key, input.key);
    QCOMPARE(output.nativeScanCode, input.nativeScanCode);
    QCOMPARE(output.nativeVirtualKey, input.nativeVirtualKey);
    QCOMPARE(output.nativeModifiers, input.nativeModifiers);
    QCOMPARE(output.count, input.count);
}

void ModifierRemapTest::replacementTakesEffectWithoutRetainedState()
{
    ModifierRemapEngine engine(QVector{
        mapping(ModifierKey::Ctrl, ModifierSide::Left, ModifierKey::Alt,
                ModifierSide::Left),
    });
    const KeyEventSnapshot input =
        snapshot(true, Qt::Key_A, Qt::ControlModifier);
    QVERIFY(engine.remapModifiers(input).contains(ModifierKey::Alt));

    const QVector replacement{
        mapping(ModifierKey::Ctrl, ModifierSide::Left, ModifierKey::Super,
                ModifierSide::Right),
    };
    engine.replaceMappings(replacement);
    const auto replaced = engine.remapModifiers(input);
    QVERIFY(!replaced.contains(ModifierKey::Alt));
    QVERIFY(replaced.contains(ModifierKey::Super));
    QCOMPARE(replaced.side(ModifierKey::Super), ModifierSide::Right);

    engine.replaceMappings(std::span<const ModifierRemap>{});
    const auto cleared = engine.remapModifiers(input);
    QVERIFY(cleared.contains(ModifierKey::Ctrl));
    QVERIFY(!cleared.contains(ModifierKey::Super));
}

void ModifierRemapTest::trackerRetainsSideAcrossChordAndPrunesMissedRelease()
{
    ModifierRemapTracker tracker(QVector{
        mapping(ModifierKey::Ctrl, ModifierSide::Right, ModifierKey::Alt,
                ModifierSide::Left),
    });

    const KeyEventSnapshot rightPress = tracker.remapEvent(snapshot(
        true, Qt::Key_Control, Qt::NoModifier, xkbKeycode(KEY_RIGHTCTRL)));
    QVERIFY(rightPress.modifiers.testFlag(Qt::AltModifier));
    QVERIFY(!rightPress.modifiers.testFlag(Qt::ControlModifier));

    // Ordinary Qt key events retain only the generic Ctrl bit. The tracker
    // carries the observed physical side through the rest of the chord.
    const KeyEventSnapshot chord =
        tracker.remapEvent(snapshot(true, Qt::Key_R, Qt::ControlModifier));
    QVERIFY(chord.modifiers.testFlag(Qt::AltModifier));
    QVERIFY(!chord.modifiers.testFlag(Qt::ControlModifier));

    // A focus transition can drop the physical release. The next generic Qt
    // mask is authoritative and clears the stale right-side classification.
    const KeyEventSnapshot afterMissedRelease =
        tracker.remapEvent(snapshot(true, Qt::Key_A, Qt::NoModifier));
    QCOMPARE(afterMissedRelease.modifiers, Qt::NoModifier);

    (void)tracker.remapEvent(snapshot(true, Qt::Key_Control, Qt::NoModifier,
                                      xkbKeycode(KEY_RIGHTCTRL)));
    const KeyEventSnapshot release =
        tracker.remapEvent(snapshot(false, Qt::Key_Control, Qt::ControlModifier,
                                    xkbKeycode(KEY_RIGHTCTRL)));
    QCOMPARE(release.modifiers, Qt::NoModifier);

    const KeyEventSnapshot leftPress = tracker.remapEvent(snapshot(
        true, Qt::Key_Control, Qt::NoModifier, xkbKeycode(KEY_LEFTCTRL)));
    QVERIFY(leftPress.modifiers.testFlag(Qt::ControlModifier));
    QVERIFY(!leftPress.modifiers.testFlag(Qt::AltModifier));
    const KeyEventSnapshot leftChord =
        tracker.remapEvent(snapshot(true, Qt::Key_R, Qt::ControlModifier));
    QVERIFY(leftChord.modifiers.testFlag(Qt::ControlModifier));
    QVERIFY(!leftChord.modifiers.testFlag(Qt::AltModifier));
}

void ModifierRemapTest::usesXkbRemappedModifierIdentityBeforeConfigRemaps()
{
    ModifierRemapTracker tracker(QVector{
        mapping(ModifierKey::Ctrl, ModifierSide::Left, ModifierKey::Alt,
                ModifierSide::Right),
    });

    // XKB turns the physical Caps key into left Control. That selected key is
    // established before Ghostty's separate key-remap transforms Ctrl to Alt.
    const KeyEventSnapshot press =
        tracker.remapEvent(snapshot(true, Qt::Key_Control, Qt::NoModifier,
                                    xkbKeycode(KEY_CAPSLOCK)),
                           XKB_KEY_Control_L);
    QVERIFY(!press.modifiers.testFlag(Qt::ControlModifier));
    QVERIFY(press.modifiers.testFlag(Qt::AltModifier));

    const KeyEventSnapshot chord = tracker.remapEvent(
        snapshot(true, Qt::Key_R, Qt::ControlModifier), XKB_KEY_r);
    QVERIFY(!chord.modifiers.testFlag(Qt::ControlModifier));
    QVERIFY(chord.modifiers.testFlag(Qt::AltModifier));

    const KeyEventSnapshot release =
        tracker.remapEvent(snapshot(false, Qt::Key_Control, Qt::ControlModifier,
                                    xkbKeycode(KEY_CAPSLOCK)),
                           XKB_KEY_Control_L);
    QCOMPARE(release.modifiers, Qt::NoModifier);

    // The inverse mapping must not resurrect Ctrl merely because its raw
    // evdev location is a control key.
    ModifierRemapTracker inverse;
    const KeyEventSnapshot remappedCaps =
        inverse.remapEvent(snapshot(true, Qt::Key_CapsLock, Qt::NoModifier,
                                    xkbKeycode(KEY_RIGHTCTRL)),
                           XKB_KEY_Caps_Lock);
    QCOMPARE(remappedCaps.modifiers, Qt::NoModifier);
}

void ModifierRemapTest::trackerReplacementClearsObservedSide()
{
    ModifierRemapTracker tracker(QVector{
        mapping(ModifierKey::Ctrl, ModifierSide::Right, ModifierKey::Alt,
                ModifierSide::Left),
    });
    (void)tracker.remapEvent(snapshot(true, Qt::Key_Control, Qt::NoModifier,
                                      xkbKeycode(KEY_RIGHTCTRL)));

    tracker.replaceMappings(QVector{
        mapping(ModifierKey::Ctrl, ModifierSide::Left, ModifierKey::Super,
                ModifierSide::Right),
    });
    const KeyEventSnapshot replaced =
        tracker.remapEvent(snapshot(true, Qt::Key_R, Qt::ControlModifier));
    QVERIFY(replaced.modifiers.testFlag(Qt::MetaModifier));
    QVERIFY(!replaced.modifiers.testFlag(Qt::AltModifier));
    QVERIFY(!replaced.modifiers.testFlag(Qt::ControlModifier));

    tracker.replaceMappings(std::span<const ModifierRemap>{});
    const KeyEventSnapshot cleared =
        tracker.remapEvent(snapshot(true, Qt::Key_R, Qt::ControlModifier));
    QVERIFY(cleared.modifiers.testFlag(Qt::ControlModifier));
    QVERIFY(!cleared.modifiers.testFlag(Qt::MetaModifier));
}

void ModifierRemapTest::rootMatchingUsesTheSameAtomicRemapGeneration()
{
    LaunchOptions options;
    GhosttyKeybindConfig config;
    config.root = {
        rootBinding('r', GhosttyKeybindAlt, QStringLiteral("reload_config")),
    };
    options.keybindSource = GhosttyKeybindSource::structured(std::move(config));

    GhosttyApplicationKeybindings bindings(options, false);
    const QVector remaps{
        mapping(ModifierKey::Ctrl, ModifierSide::Left, ModifierKey::Alt,
                ModifierSide::Left),
    };
    options.modifierRemaps = remaps;
    const GhosttyKeybindProgram generation =
        bindings.applyLaunchOptions(options);
    QVERIFY(bindings.keybindProgram().isSameGeneration(generation));

    QSignalSpy requested(
        &bindings, &GhosttyApplicationKeybindings::applicationActionRequested);
    KeyReceiver target;
    QKeyEvent press(QEvent::KeyPress, Qt::Key_R, Qt::ControlModifier,
                    QString(QChar(0x12)));
    QCoreApplication::sendEvent(&target, &press);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_R, Qt::ControlModifier);
    QCoreApplication::sendEvent(&target, &release);

    QCOMPARE(requested.count(), 1);
    QCOMPARE(
        qvariant_cast<ApplicationAction>(requested.constFirst().constFirst()),
        ApplicationAction::ReloadConfig);
    QCOMPARE(target.presses, 0);
    QCOMPARE(target.releases, 0);

    // Reinstalling a generation without the remap updates both root inputs in
    // one transaction. The raw Ctrl event now reaches the target, while Alt
    // activates the same binding.
    options.modifierRemaps.clear();
    (void)bindings.applyLaunchOptions(options);
    QKeyEvent rawControl(QEvent::KeyPress, Qt::Key_R, Qt::ControlModifier,
                         QString(QChar(0x12)));
    QCoreApplication::sendEvent(&target, &rawControl);
    QCOMPARE(requested.count(), 1);
    QCOMPARE(target.presses, 1);

    QKeyEvent rawAlt(QEvent::KeyPress, Qt::Key_R, Qt::AltModifier,
                     QStringLiteral("r"));
    QCoreApplication::sendEvent(&target, &rawAlt);
    QCOMPARE(requested.count(), 2);
    QCOMPARE(target.presses, 1);
}

QTEST_GUILESS_MAIN(ModifierRemapTest)

#include "test_modifier_remap.moc"
