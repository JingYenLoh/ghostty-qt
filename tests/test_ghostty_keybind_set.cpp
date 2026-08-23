#include "input/ghostty_keybind_set.h"

#include <QTest>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <array>
#include <initializer_list>
#include <type_traits>
#include <utility>

namespace {

using Disposition = GhosttyKeybindEntryDisposition;
using Reason = GhosttyKeybindUnsupportedReason;

static_assert(!std::is_copy_constructible_v<GhosttyKeybindState>);
static_assert(!std::is_copy_assignable_v<GhosttyKeybindState>);
static_assert(!std::is_move_constructible_v<GhosttyKeybindState>);
static_assert(!std::is_move_assignable_v<GhosttyKeybindState>);

template <typename Source>
GhosttyKeybindLoadReport installProgram(GhosttyKeybindState &state,
                                        const Source &source)
{
    GhosttyKeybindCompilation compilation =
        GhosttyKeybindProgram::compile(source);
    (void)state.replaceProgram(std::move(compilation.program));
    return std::move(compilation.report);
}

GhosttyKeybindLoadReport installProgram(GhosttyKeybindState &state,
                                        std::initializer_list<QString> values)
{
    return installProgram(state, QStringList(values));
}

GhosttyKeybindMatch requireMatch(std::optional<GhosttyKeybindMatch> match)
{
    if (!match.has_value()) {
        QTest::qFail("expected keybind match", __FILE__, __LINE__);
        return {};
    }
    return std::move(*match);
}

QStringList serializedActions(const GhosttyKeybindMatch &match)
{
    return match.actionChain.serializedActions();
}

QStringList serializedActions(const GhosttyKeybindStep &step)
{
    return serializedActions(step.match);
}

constexpr quint32 xkbKeycode(unsigned int evdevCode)
{
    return static_cast<quint32>(evdevCode + 8U);
}

GhosttyKeybindTrigger unicodeTrigger(quint32 codepoint, quint8 modifiers = 0)
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

GhosttyKeybindTrigger physicalTrigger(QString name, quint8 modifiers = 0)
{
    return {
        .kind = GhosttyKeybindKeyKind::Physical,
        .physicalName = std::move(name),
        .modifiers = modifiers,
    };
}

GhosttyKeybindDefinition binding(QVector<GhosttyKeybindTrigger> sequence,
                                 QString action, GhosttyKeybindFlags flags = {})
{
    return {
        .sequence = std::move(sequence),
        .actions = {std::move(action)},
        .flags = flags,
    };
}

} // namespace

class GhosttyKeybindStateTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void distinguishesUnavailableAndAvailableEmptyPrograms();
    void parsesEqualsAndPlusTriggerDelimiters();
    void matchesNamedAndW3cPhysicalKeys();
    void physicalBindingsTakePrecedenceOverUnicode();
    void nativePhysicalBindingsAreLayoutIndependent();
    void honorsFunctionalXkbRemapsForPhysicalBindings();
    void distinguishesKeypadAndModifierLocations();
    void matchesSemanticNumLockKeypadBindings();
    void matchesShiftedUnicodeByUnshiftedCodepoint();
    void matchesFullUnicodeCaseFoldsAndScalars();
    void ordersAndFiltersUnicodeCandidates();
    void unicodeBindingsSurviveSharedProgramAndReload();
    void loadsTaggedSources();
    void appendsAdjacentChainsInOrder();
    void compiledMatchesOwnPositionalMetadata();
    void preservesLocalFlags();
    void supportsSequencesAndCatchAllWhileRejectingDeferredForms();
    void advancesSharedPrefixSequences();
    void retainsConfiguredLabelsForActiveSequence();
    void recoversInvalidSequencesAndHonorsCatchAll();
    void preservesLookupPriorityInsideSequences();
    void sharesProgramWithoutSharingMutableState();
    void loadsStructuredDefinitionsAndRetainsFlags();
    void walksDeepSequencesIteratively();
    void rejectsBroadStructuredSequences();
    void routesNamedTablesNewestToOldestAndRoot();
    void enforcesTableStackAndActivationRules();
    void handlesOneShotTablesExactly();
    void keepsTableSequenceAndCatchAllSemantics();
    void replacingProgramResetsOnlyOwningState();
    void rejectsMalformedBindings();
    void replacesDuplicateTriggers();
    void matchesLinuxDefaultLikeBindings();
};

void GhosttyKeybindStateTest::
    distinguishesUnavailableAndAvailableEmptyPrograms()
{
    const GhosttyKeybindProgram unavailable;
    const GhosttyKeybindProgram anotherUnavailable;
    QVERIFY(!unavailable.isAvailable());
    QVERIFY(unavailable.isEmpty());
    QCOMPARE(unavailable.size(), 0);
    QVERIFY(unavailable.serializedActions().isEmpty());
    QVERIFY(unavailable.isSameGeneration(anotherUnavailable));

    const GhosttyKeybindProgram unavailableGeneration =
        GhosttyKeybindProgram::compile(GhosttyKeybindSource{}).program;
    const GhosttyKeybindProgram nextUnavailableGeneration =
        GhosttyKeybindProgram::compile(GhosttyKeybindSource{}).program;
    QVERIFY(!unavailableGeneration.isAvailable());
    QVERIFY(unavailableGeneration.isEmpty());
    QVERIFY(!unavailableGeneration.isSameGeneration(unavailable));
    QVERIFY(!unavailableGeneration.isSameGeneration(nextUnavailableGeneration));

    const GhosttyKeybindCompilation emptyCompilation =
        GhosttyKeybindProgram::compile(QStringList{});
    QVERIFY(emptyCompilation.report.records.isEmpty());
    QVERIFY(emptyCompilation.program.isAvailable());
    QVERIFY(emptyCompilation.program.isEmpty());
    QCOMPARE(emptyCompilation.program.size(), 0);
    QVERIFY(emptyCompilation.program.serializedActions().isEmpty());
    QVERIFY(!emptyCompilation.program.isSameGeneration(unavailable));

    const GhosttyKeybindState defaultState;
    const GhosttyKeybindState availableEmptyState(emptyCompilation.program);
    QVERIFY(!defaultState.program().isAvailable());
    QVERIFY(availableEmptyState.program().isAvailable());
    QVERIFY(defaultState.isEmpty());
    QVERIFY(availableEmptyState.isEmpty());
}

void GhosttyKeybindStateTest::walksDeepSequencesIteratively()
{
    // Configuration depth is user-controlled. Counting and diagnostic
    // serialization must use heap-backed traversal instead of the C++ call
    // stack even for a deliberately pathological sequence.
    constexpr qsizetype depth = 32'768;
    QVector<GhosttyKeybindTrigger> sequence;
    sequence.fill(unicodeTrigger('x'), depth);

    GhosttyKeybindConfig config;
    config.root = {
        binding(std::move(sequence), QStringLiteral("deep_action")),
    };
    const GhosttyKeybindCompilation compilation =
        GhosttyKeybindProgram::compile(config);

    QCOMPARE(compilation.report.count(Disposition::Installed), 1);
    QCOMPARE(compilation.program.size(), 1);
    QCOMPARE(compilation.program.serializedActions(),
             QStringList({QStringLiteral("deep_action")}));
}

void GhosttyKeybindStateTest::parsesEqualsAndPlusTriggerDelimiters()
{
    GhosttyKeybindState set;
    const GhosttyKeybindLoadReport report =
        installProgram(set,
                       {
                           QStringLiteral("ctrl+==increase_font_size:1"),
                           QStringLiteral("ctrl++=increase_font_size:2"),
                           QStringLiteral("=+alt=text:=hello"),
                       });

    QCOMPARE(report.count(Disposition::Installed), 3);
    QCOMPARE(set.size(), 3);

    QCOMPARE(serializedActions(requireMatch(set.match(
                 Qt::Key_Equal, Qt::ControlModifier, QStringLiteral("=")))),
             QStringList({QStringLiteral("increase_font_size:1")}));
    QCOMPARE(serializedActions(requireMatch(set.match(
                 Qt::Key_Plus, Qt::ControlModifier, QStringLiteral("+")))),
             QStringList({QStringLiteral("increase_font_size:2")}));
    QVERIFY(!set.match(Qt::Key_Plus, Qt::ControlModifier | Qt::ShiftModifier,
                       QStringLiteral("+"))
                 .has_value());
    QCOMPARE(serializedActions(requireMatch(set.match(
                 Qt::Key_Equal, Qt::AltModifier, QStringLiteral("=")))),
             QStringList({QStringLiteral("text:=hello")}));
}

void GhosttyKeybindStateTest::loadsTaggedSources()
{
    GhosttyKeybindState set;

    const GhosttyKeybindSource text = GhosttyKeybindSource::text({
        QStringLiteral("alt+t=new_tab"),
    });
    QVERIFY(text.isAvailable());
    QVERIFY(text.text() != nullptr);
    QVERIFY(text.structured() == nullptr);
    const GhosttyKeybindLoadReport textReport = installProgram(set, text);
    QCOMPARE(textReport.count(Disposition::Installed), 1);
    QCOMPARE(serializedActions(requireMatch(
                 set.match(Qt::Key_T, Qt::AltModifier, QStringLiteral("t")))),
             QStringList({QStringLiteral("new_tab")}));

    const GhosttyKeybindSource emptyText = GhosttyKeybindSource::text({});
    QVERIFY(emptyText.isAvailable());
    QVERIFY(emptyText.text() != nullptr);
    QVERIFY(emptyText.text()->isEmpty());
    QVERIFY(emptyText.structured() == nullptr);
    QVERIFY(installProgram(set, emptyText).records.isEmpty());
    QVERIFY(set.isEmpty());

    (void)installProgram(set, text);
    QCOMPARE(set.size(), 1);

    const GhosttyKeybindSource unavailable;
    QVERIFY(!unavailable.isAvailable());
    QVERIFY(unavailable.text() == nullptr);
    QVERIFY(unavailable.structured() == nullptr);
    QVERIFY(installProgram(set, unavailable).records.isEmpty());
    QVERIFY(set.isEmpty());

    const GhosttyKeybindSource structuredEmpty =
        GhosttyKeybindSource::structured({});
    QVERIFY(structuredEmpty.isAvailable());
    QVERIFY(structuredEmpty.text() == nullptr);
    QVERIFY(structuredEmpty.structured() != nullptr);
    QVERIFY(installProgram(set, structuredEmpty).records.isEmpty());
    QVERIFY(set.isEmpty());

    GhosttyKeybindConfig config;
    config.root = {binding({unicodeTrigger('n', GhosttyKeybindAlt)},
                           QStringLiteral("new_window"))};
    const GhosttyKeybindSource structured =
        GhosttyKeybindSource::structured(std::move(config));
    const GhosttyKeybindLoadReport structuredReport =
        installProgram(set, structured);
    QCOMPARE(structuredReport.count(Disposition::Installed), 1);
    QCOMPARE(serializedActions(requireMatch(
                 set.match(Qt::Key_N, Qt::AltModifier, QStringLiteral("n")))),
             QStringList({QStringLiteral("new_window")}));
}

void GhosttyKeybindStateTest::matchesNamedAndW3cPhysicalKeys()
{
    GhosttyKeybindState set;
    const GhosttyKeybindLoadReport report =
        installProgram(set,
                       {
                           QStringLiteral("ctrl+arrow_left=goto_split:left"),
                           QStringLiteral("alt+F4=close_window"),
                           QStringLiteral("shift+PageDown=scroll_page_down"),
                           QStringLiteral("KeyA=physical_a"),
                       });

    QCOMPARE(report.count(Disposition::Installed), 4);

    auto match = set.match(Qt::Key_Left, Qt::ControlModifier);
    QVERIFY(match.has_value());
    QVERIFY(match->physical);
    QCOMPARE(serializedActions(*match),
             QStringList({QStringLiteral("goto_split:left")}));

    match = set.match(Qt::Key_F4, Qt::AltModifier);
    QVERIFY(match.has_value());
    QVERIFY(match->physical);
    QCOMPARE(serializedActions(*match),
             QStringList({QStringLiteral("close_window")}));

    match = set.match(Qt::Key_PageDown, Qt::ShiftModifier);
    QVERIFY(match.has_value());
    QVERIFY(match->physical);
    QCOMPARE(serializedActions(*match),
             QStringList({QStringLiteral("scroll_page_down")}));

    match = set.match(Qt::Key_A, Qt::NoModifier, QStringLiteral("ф"));
    QVERIFY(match.has_value());
    QVERIFY(match->physical);
    QCOMPARE(serializedActions(*match),
             QStringList({QStringLiteral("physical_a")}));
}

void GhosttyKeybindStateTest::physicalBindingsTakePrecedenceOverUnicode()
{
    GhosttyKeybindState set;
    const GhosttyKeybindLoadReport report = installProgram(
        set,
        {
            QStringLiteral("ctrl+a=unicode_action"),
            // Configured later is not what establishes priority: key kind does.
            QStringLiteral("ctrl+key_a=physical_action"),
        });
    QCOMPARE(report.count(Disposition::Installed), 2);

    const GhosttyKeybindMatch &match = requireMatch(
        set.match(Qt::Key_A, Qt::ControlModifier, QStringLiteral("A")));
    QVERIFY(match.physical);
    QCOMPARE(serializedActions(match),
             QStringList({QStringLiteral("physical_action")}));

    // A different physical location can still select the Unicode binding.
    const GhosttyKeybindMatch &unicode = requireMatch(
        set.match(Qt::Key_B, Qt::ControlModifier, QStringLiteral("A")));
    QVERIFY(!unicode.physical);
    QCOMPARE(serializedActions(unicode),
             QStringList({QStringLiteral("unicode_action")}));
}

void GhosttyKeybindStateTest::nativePhysicalBindingsAreLayoutIndependent()
{
    GhosttyKeybindState set;
    QCOMPARE(installProgram(set, {QStringLiteral("ctrl+key_a=physical_a")})
                 .count(Disposition::Installed),
             1);

    // A layout can report a different logical Qt key and text for the physical
    // KeyA location. Its native XKB keycode remains KEY_A + 8.
    const GhosttyKeybindMatch &remapped =
        requireMatch(set.match(GhosttyKeybindEvent{
            .qtKey = Qt::Key_Q,
            .modifiers = Qt::ControlModifier,
            .text = QStringLiteral("q"),
            .nativeScanCode = xkbKeycode(KEY_A),
            .unshiftedCodepoint = 'q',
        }));
    QVERIFY(remapped.physical);
    QCOMPARE(serializedActions(remapped),
             QStringList({QStringLiteral("physical_a")}));

    // The logical A produced by a different physical location must not match.
    QVERIFY(!set.match(GhosttyKeybindEvent{
                           .qtKey = Qt::Key_A,
                           .modifiers = Qt::ControlModifier,
                           .text = QStringLiteral("a"),
                           .nativeScanCode = xkbKeycode(KEY_Q),
                           .unshiftedCodepoint = 'a',
                       })
                 .has_value());
}

void GhosttyKeybindStateTest::honorsFunctionalXkbRemapsForPhysicalBindings()
{
    GhosttyKeybindState set;
    QCOMPARE(installProgram(set,
                            {
                                QStringLiteral("escape=escape_action"),
                                QStringLiteral("caps_lock=caps_action"),
                                QStringLiteral("key_a=a_action"),
                                QStringLiteral("key_y=y_action"),
                            })
                 .count(Disposition::Installed),
             4);

    const auto actionFor = [&set](int qtKey, quint32 nativeScanCode,
                                  quint32 resolvedKeysym) {
        return serializedActions(requireMatch(set.match(GhosttyKeybindEvent{
            .qtKey = qtKey,
            .nativeScanCode = nativeScanCode,
            .resolvedKeysym = resolvedKeysym,
        })));
    };

    QCOMPARE(
        actionFor(Qt::Key_Escape, xkbKeycode(KEY_CAPSLOCK), XKB_KEY_Escape),
        QStringList({QStringLiteral("escape_action")}));
    QCOMPARE(
        actionFor(Qt::Key_CapsLock, xkbKeycode(KEY_ESC), XKB_KEY_Caps_Lock),
        QStringList({QStringLiteral("caps_action")}));

    // Either non-writing endpoint enables the remap.
    QCOMPARE(actionFor(Qt::Key_Escape, xkbKeycode(KEY_A), XKB_KEY_Escape),
             QStringList({QStringLiteral("escape_action")}));
    QCOMPARE(actionFor(Qt::Key_A, xkbKeycode(KEY_ESC), XKB_KEY_a),
             QStringList({QStringLiteral("a_action")}));

    // Writing-to-writing layout changes remain tied to the physical key.
    QCOMPARE(actionFor(Qt::Key_Z, xkbKeycode(KEY_Y), XKB_KEY_z),
             QStringList({QStringLiteral("y_action")}));

    // A keysym outside the pinned GTK mapping cannot destroy raw identity.
    QCOMPARE(
        actionFor(Qt::Key_unknown, xkbKeycode(KEY_ESC), XKB_KEY_Cyrillic_tse),
        QStringList({QStringLiteral("escape_action")}));
}

void GhosttyKeybindStateTest::distinguishesKeypadAndModifierLocations()
{
    GhosttyKeybindState set;
    const GhosttyKeybindLoadReport report =
        installProgram(set,
                       {
                           QStringLiteral("alt+digit_1=top_row"),
                           QStringLiteral("alt+numpad_1=keypad"),
                           QStringLiteral("control_left=left_control"),
                           QStringLiteral("control_right=right_control"),
                           QStringLiteral("shift+semicolon=punctuation"),
                       });
    QCOMPARE(report.count(Disposition::Installed), 5);
    QCOMPARE(set.size(), 5);

    QCOMPARE(serializedActions(requireMatch(set.match(GhosttyKeybindEvent{
                 .qtKey = Qt::Key_1,
                 .modifiers = Qt::AltModifier,
                 .text = QStringLiteral("1"),
                 .nativeScanCode = xkbKeycode(KEY_1),
                 .unshiftedCodepoint = '1',
             }))),
             QStringList({QStringLiteral("top_row")}));
    QCOMPARE(serializedActions(requireMatch(set.match(GhosttyKeybindEvent{
                 .qtKey = Qt::Key_1,
                 .modifiers = Qt::AltModifier | Qt::KeypadModifier,
                 .text = QStringLiteral("1"),
                 .nativeScanCode = xkbKeycode(KEY_KP1),
                 .unshiftedCodepoint = '1',
             }))),
             QStringList({QStringLiteral("keypad")}));

    QCOMPARE(serializedActions(requireMatch(set.match(GhosttyKeybindEvent{
                 .qtKey = Qt::Key_Control,
                 .modifiers = Qt::ControlModifier,
                 .nativeScanCode = xkbKeycode(KEY_LEFTCTRL),
             }))),
             QStringList({QStringLiteral("left_control")}));
    QCOMPARE(serializedActions(requireMatch(set.match(GhosttyKeybindEvent{
                 .qtKey = Qt::Key_Control,
                 .modifiers = Qt::ControlModifier,
                 .nativeScanCode = xkbKeycode(KEY_RIGHTCTRL),
             }))),
             QStringList({QStringLiteral("right_control")}));

    // Synthetic events without native data retain a useful keypad fallback.
    QCOMPARE(serializedActions(requireMatch(
                 set.match(Qt::Key_1, Qt::AltModifier, QStringLiteral("1")))),
             QStringList({QStringLiteral("top_row")}));
    QCOMPARE(serializedActions(requireMatch(
                 set.match(Qt::Key_1, Qt::AltModifier | Qt::KeypadModifier,
                           QStringLiteral("1")))),
             QStringList({QStringLiteral("keypad")}));
    QCOMPARE(serializedActions(requireMatch(set.match(
                 Qt::Key_Colon, Qt::ShiftModifier, QStringLiteral(":")))),
             QStringList({QStringLiteral("punctuation")}));
}

void GhosttyKeybindStateTest::matchesSemanticNumLockKeypadBindings()
{
    struct SemanticCase {
        const char *name;
        int qtKey;
        unsigned int evdevCode;
        quint32 keysym;
    };
    static constexpr std::array<SemanticCase, 12> semanticCases = {{
        {"numpad_home", Qt::Key_Home, KEY_KP7, XKB_KEY_KP_Home},
        {"numpad_up", Qt::Key_Up, KEY_KP8, XKB_KEY_KP_Up},
        {"numpad_page_up", Qt::Key_PageUp, KEY_KP9, XKB_KEY_KP_Page_Up},
        {"numpad_left", Qt::Key_Left, KEY_KP4, XKB_KEY_KP_Left},
        {"numpad_begin", Qt::Key_Clear, KEY_KP5, XKB_KEY_KP_Begin},
        {"numpad_right", Qt::Key_Right, KEY_KP6, XKB_KEY_KP_Right},
        {"numpad_end", Qt::Key_End, KEY_KP1, XKB_KEY_KP_End},
        {"numpad_down", Qt::Key_Down, KEY_KP2, XKB_KEY_KP_Down},
        {"numpad_page_down", Qt::Key_PageDown, KEY_KP3, XKB_KEY_KP_Page_Down},
        {"numpad_insert", Qt::Key_Insert, KEY_KP0, XKB_KEY_KP_Insert},
        {"numpad_delete", Qt::Key_Delete, KEY_KPDOT, XKB_KEY_KP_Delete},
        {"numpad_separator", Qt::Key_Comma, KEY_KPCOMMA, XKB_KEY_KP_Separator},
    }};

    const auto keypadEvent = [](int qtKey, unsigned int evdevCode,
                                quint32 keysym) {
        return GhosttyKeybindEvent{
            .qtKey = qtKey,
            .modifiers = Qt::KeypadModifier,
            .nativeScanCode = xkbKeycode(evdevCode),
            .resolvedKeysym = keysym,
        };
    };

    GhosttyKeybindConfig config;
    for (const SemanticCase &sample : semanticCases) {
        const QString name = QString::fromLatin1(sample.name);
        config.root.append(
            binding({physicalTrigger(name)}, QStringLiteral("text:") + name));
    }
    config.root.append(binding({physicalTrigger(QStringLiteral("numpad_1"))},
                               QStringLiteral("text:numpad_1")));
    config.root.append(binding({physicalTrigger(QStringLiteral("numpad_5"))},
                               QStringLiteral("text:numpad_5")));
    config.root.append(
        binding({physicalTrigger(QStringLiteral("numpad_decimal"))},
                QStringLiteral("text:numpad_decimal")));

    GhosttyKeybindState set;
    QCOMPARE(installProgram(set, config).count(Disposition::Installed),
             static_cast<int>(semanticCases.size()) + 3);
    for (const SemanticCase &sample : semanticCases) {
        const QString name = QString::fromLatin1(sample.name);
        QCOMPARE(serializedActions(requireMatch(set.match(keypadEvent(
                     sample.qtKey, sample.evdevCode, sample.keysym)))),
                 QStringList({QStringLiteral("text:") + name}));
    }

    // The same hardware positions select their numeric identities with
    // NumLock on rather than colliding with the semantic navigation entries.
    QCOMPARE(serializedActions(requireMatch(
                 set.match(keypadEvent(Qt::Key_1, KEY_KP1, XKB_KEY_KP_1)))),
             QStringList({QStringLiteral("text:numpad_1")}));
    QCOMPARE(serializedActions(requireMatch(
                 set.match(keypadEvent(Qt::Key_5, KEY_KP5, XKB_KEY_KP_5)))),
             QStringList({QStringLiteral("text:numpad_5")}));
    QCOMPARE(serializedActions(requireMatch(set.match(
                 keypadEvent(Qt::Key_Period, KEY_KPDOT, XKB_KEY_KP_Decimal)))),
             QStringList({QStringLiteral("text:numpad_decimal")}));

    GhosttyKeybindState aliases;
    const GhosttyKeybindLoadReport aliasReport =
        installProgram(aliases,
                       {
                           QStringLiteral("kp_end=text:legacy-end"),
                           QStringLiteral("NumpadHome=text:camel-home"),
                           QStringLiteral("kp_separator=text:legacy-separator"),
                       });
    QCOMPARE(aliasReport.count(Disposition::Installed), 3);
    QCOMPARE(serializedActions(requireMatch(aliases.match(
                 keypadEvent(Qt::Key_End, KEY_KP1, XKB_KEY_KP_End)))),
             QStringList({QStringLiteral("text:legacy-end")}));
    QCOMPARE(serializedActions(requireMatch(aliases.match(
                 keypadEvent(Qt::Key_Home, KEY_KP7, XKB_KEY_KP_Home)))),
             QStringList({QStringLiteral("text:camel-home")}));
    QCOMPARE(serializedActions(requireMatch(aliases.match(keypadEvent(
                 Qt::Key_Comma, KEY_KPCOMMA, XKB_KEY_KP_Separator)))),
             QStringList({QStringLiteral("text:legacy-separator")}));

    GhosttyKeybindState invalidAliases;
    const GhosttyKeybindLoadReport invalidAliasReport =
        installProgram(invalidAliases,
                       {
                           QStringLiteral("KP_End=text:invalid-uppercase"),
                           QStringLiteral("kpEnd=text:invalid-camel-alias"),
                           QStringLiteral("KEY_A=text:invalid-underscore-case"),
                       });
    QCOMPARE(invalidAliasReport.count(Disposition::Invalid), 3);
    QVERIFY(invalidAliases.isEmpty());

    GhosttyKeybindState sequence;
    QCOMPARE(installProgram(
                 sequence,
                 {QStringLiteral("kp_begin>kp_page_up>kp_page_down>kp_end="
                                 "text:keypad-sequence")})
                 .count(Disposition::Installed),
             1);
    QCOMPARE(
        sequence.advance(keypadEvent(Qt::Key_Clear, KEY_KP5, XKB_KEY_KP_Begin))
            .kind,
        GhosttyKeybindStepKind::Leader);
    QCOMPARE(sequence.activeSequenceLabels(),
             QStringList({QStringLiteral("KP_Begin")}));
    QCOMPARE(
        sequence
            .advance(keypadEvent(Qt::Key_PageUp, KEY_KP9, XKB_KEY_KP_Page_Up))
            .kind,
        GhosttyKeybindStepKind::Leader);
    QCOMPARE(
        sequence.activeSequenceLabels(),
        QStringList({QStringLiteral("KP_Begin"), QStringLiteral("KP_Prior")}));
    QCOMPARE(sequence
                 .advance(keypadEvent(Qt::Key_PageDown, KEY_KP3,
                                      XKB_KEY_KP_Page_Down))
                 .kind,
             GhosttyKeybindStepKind::Leader);
    QCOMPARE(
        sequence.activeSequenceLabels(),
        QStringList({QStringLiteral("KP_Begin"), QStringLiteral("KP_Prior"),
                     QStringLiteral("KP_Next")}));
    const GhosttyKeybindStep completed =
        sequence.advance(keypadEvent(Qt::Key_End, KEY_KP1, XKB_KEY_KP_End));
    QCOMPARE(completed.kind, GhosttyKeybindStepKind::Binding);
    QCOMPARE(serializedActions(completed),
             QStringList({QStringLiteral("text:keypad-sequence")}));
}

void GhosttyKeybindStateTest::matchesShiftedUnicodeByUnshiftedCodepoint()
{
    GhosttyKeybindState set;
    QCOMPARE(installProgram(set, {QStringLiteral("ctrl+shift+,=reload_config")})
                 .count(Disposition::Installed),
             1);

    // Qt text carries '<', while Ghostty's Unicode trigger is the unshifted
    // comma codepoint at the same logical location.
    QVERIFY(!set.match(Qt::Key_Less, Qt::ControlModifier | Qt::ShiftModifier,
                       QStringLiteral("<"))
                 .has_value());
    QCOMPARE(serializedActions(requireMatch(set.match(GhosttyKeybindEvent{
                 .qtKey = Qt::Key_Less,
                 .modifiers = Qt::ControlModifier | Qt::ShiftModifier,
                 .text = QStringLiteral("<"),
                 .nativeScanCode = xkbKeycode(KEY_COMMA),
                 .unshiftedCodepoint = ',',
             }))),
             QStringList({QStringLiteral("reload_config")}));

    QCOMPARE(installProgram(set, {QStringLiteral("ctrl+,=ignore")})
                 .count(Disposition::Installed),
             1);
    QVERIFY(!set.match(GhosttyKeybindEvent{
                           .qtKey = Qt::Key_Less,
                           .modifiers = Qt::ControlModifier | Qt::ShiftModifier,
                           .text = QStringLiteral("<"),
                           .nativeScanCode = xkbKeycode(KEY_COMMA),
                           .unshiftedCodepoint = ',',
                       })
                 .has_value());
}

void GhosttyKeybindStateTest::matchesFullUnicodeCaseFoldsAndScalars()
{
    GhosttyKeybindState set;
    const GhosttyKeybindLoadReport report =
        installProgram(set,
                       {
                           QStringLiteral("ctrl+ß=first"),
                           QStringLiteral("ctrl+ẞ=second"),
                           QStringLiteral("alt+Σ=sigma"),
                       });

    QCOMPARE(report.count(Disposition::Installed), 3);
    QCOMPARE(set.size(), 2);
    QCOMPARE(serializedActions(requireMatch(set.match(GhosttyKeybindEvent{
                 .qtKey = Qt::Key_unknown,
                 .modifiers = Qt::ControlModifier,
                 .text = QStringLiteral("ß"),
             }))),
             QStringList({QStringLiteral("second")}));
    QCOMPARE(serializedActions(requireMatch(set.match(GhosttyKeybindEvent{
                 .qtKey = Qt::Key_unknown,
                 .modifiers = Qt::ControlModifier,
                 .text = QStringLiteral("ẞ"),
             }))),
             QStringList({QStringLiteral("second")}));
    QCOMPARE(serializedActions(requireMatch(set.match(GhosttyKeybindEvent{
                 .qtKey = Qt::Key_unknown,
                 .modifiers = Qt::AltModifier,
                 .text = QStringLiteral("ς"),
             }))),
             QStringList({QStringLiteral("sigma")}));

    QCOMPARE(installProgram(set, {QStringLiteral("😀=emoji")})
                 .count(Disposition::Installed),
             1);
    const char32_t emoji = 0x1f600U;
    QCOMPARE(serializedActions(requireMatch(set.match(GhosttyKeybindEvent{
                 .qtKey = Qt::Key_unknown,
                 .text = QString::fromUcs4(&emoji, 1),
             }))),
             QStringList({QStringLiteral("emoji")}));
}

void GhosttyKeybindStateTest::ordersAndFiltersUnicodeCandidates()
{
    GhosttyKeybindState set;
    const GhosttyKeybindEvent shifted{
        .qtKey = Qt::Key_Less,
        .modifiers = Qt::ControlModifier | Qt::ShiftModifier,
        .text = QStringLiteral("<"),
        .nativeScanCode = xkbKeycode(KEY_COMMA),
        .unshiftedCodepoint = ',',
    };

    QCOMPARE(installProgram(set,
                            {
                                QStringLiteral("ctrl+shift+,=unshifted"),
                                QStringLiteral("ctrl+shift+<=shifted"),
                            })
                 .count(Disposition::Installed),
             2);
    QCOMPARE(serializedActions(requireMatch(set.match(shifted))),
             QStringList({QStringLiteral("shifted")}));

    QCOMPARE(installProgram(set, {QStringLiteral("ctrl+shift+,=unshifted")})
                 .count(Disposition::Installed),
             1);
    QCOMPARE(serializedActions(requireMatch(set.match(shifted))),
             QStringList({QStringLiteral("unshifted")}));

    QCOMPARE(installProgram(set,
                            {
                                QStringLiteral("ctrl+ß=sharp_s"),
                                QStringLiteral("ctrl+é=precomposed"),
                                QStringLiteral("ctrl+a=letter"),
                            })
                 .count(Disposition::Installed),
             3);
    QVERIFY(!set.match(GhosttyKeybindEvent{
                           .qtKey = Qt::Key_unknown,
                           .modifiers = Qt::ControlModifier,
                           .text = QStringLiteral("ss"),
                       })
                 .has_value());
    QVERIFY(!set.match(GhosttyKeybindEvent{
                           .qtKey = Qt::Key_unknown,
                           .modifiers = Qt::ControlModifier,
                           .text = QStringLiteral("e\u0301"),
                       })
                 .has_value());
    QCOMPARE(serializedActions(requireMatch(set.match(GhosttyKeybindEvent{
                 .qtKey = Qt::Key_A,
                 .modifiers = Qt::ControlModifier,
                 .text = QStringLiteral("ab"),
             }))),
             QStringList({QStringLiteral("letter")}));
    QVERIFY(!set.match(GhosttyKeybindEvent{
                           .qtKey = Qt::Key_unknown,
                           .modifiers = Qt::ControlModifier,
                           .text = QStringLiteral("ab"),
                       })
                 .has_value());

    QCOMPARE(serializedActions(requireMatch(set.match(GhosttyKeybindEvent{
                 .qtKey = Qt::Key_A,
                 .modifiers = Qt::ControlModifier | Qt::KeypadModifier
                     | Qt::GroupSwitchModifier,
                 .text = QStringLiteral("a"),
             }))),
             QStringList({QStringLiteral("letter")}));
    QVERIFY(!set.match(GhosttyKeybindEvent{
                           .qtKey = Qt::Key_A,
                           .modifiers = Qt::ControlModifier | Qt::AltModifier
                               | Qt::KeypadModifier | Qt::GroupSwitchModifier,
                           .text = QStringLiteral("a"),
                       })
                 .has_value());
}

void GhosttyKeybindStateTest::unicodeBindingsSurviveSharedProgramAndReload()
{
    GhosttyKeybindState original;
    QCOMPARE(installProgram(original,
                            {
                                QStringLiteral("ctrl+ß=sharp_s"),
                                QStringLiteral("alt+Σ=sigma"),
                            })
                 .count(Disposition::Installed),
             2);
    GhosttyKeybindState copy(original.program());
    QVERIFY(original.program().isSameGeneration(copy.program()));

    QCOMPARE(installProgram(original, {QStringLiteral("ctrl+x=replacement")})
                 .count(Disposition::Installed),
             1);
    QCOMPARE(serializedActions(requireMatch(copy.match(GhosttyKeybindEvent{
                 .qtKey = Qt::Key_unknown,
                 .modifiers = Qt::ControlModifier,
                 .text = QStringLiteral("ẞ"),
             }))),
             QStringList({QStringLiteral("sharp_s")}));
    QCOMPARE(serializedActions(requireMatch(copy.match(GhosttyKeybindEvent{
                 .qtKey = Qt::Key_unknown,
                 .modifiers = Qt::AltModifier,
                 .text = QStringLiteral("ς"),
             }))),
             QStringList({QStringLiteral("sigma")}));
    QVERIFY(!original
                 .match(GhosttyKeybindEvent{
                     .qtKey = Qt::Key_unknown,
                     .modifiers = Qt::ControlModifier,
                     .text = QStringLiteral("ẞ"),
                 })
                 .has_value());
    QCOMPARE(serializedActions(requireMatch(original.match(
                 Qt::Key_X, Qt::ControlModifier, QStringLiteral("x")))),
             QStringList({QStringLiteral("replacement")}));
}

void GhosttyKeybindStateTest::appendsAdjacentChainsInOrder()
{
    GhosttyKeybindState set;
    const GhosttyKeybindLoadReport report =
        installProgram(set,
                       {
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
    QCOMPARE(serializedActions(first),
             QStringList({QStringLiteral("new_tab"),
                          QStringLiteral("goto_split:left"),
                          QStringLiteral("toggle_split_zoom")}));
    QVERIFY(first.performable);

    const GhosttyKeybindMatch &second = requireMatch(
        set.match(Qt::Key_B, Qt::ControlModifier, QStringLiteral("b")));
    QCOMPARE(serializedActions(second),
             QStringList({QStringLiteral("next_tab"),
                          QStringLiteral("close_tab:this")}));
    QVERIFY(!second.performable);
}

void GhosttyKeybindStateTest::compiledMatchesOwnPositionalMetadata()
{
    GhosttyKeybindState flattenedSet;
    QStringList flattenedSource{
        QStringLiteral("ctrl+f=search:flattened:needle"),
        QStringLiteral("chain=toggle_visibility"),
        QStringLiteral("chain=reload_config:bogus"),
        QStringLiteral("chain=unknown_surface"),
        QStringLiteral("chain=close_window"),
        QStringLiteral("chain=ignore"),
    };
    QCOMPARE(installProgram(flattenedSet, flattenedSource)
                 .count(Disposition::Chained),
             5);
    flattenedSource.clear();

    const GhosttyKeybindMatch flattened = requireMatch(flattenedSet.match(
        Qt::Key_F, Qt::ControlModifier, QStringLiteral("f")));
    QCOMPARE(serializedActions(flattened),
             QStringList({QStringLiteral("search:flattened:needle"),
                          QStringLiteral("toggle_visibility"),
                          QStringLiteral("reload_config:bogus"),
                          QStringLiteral("unknown_surface"),
                          QStringLiteral("close_window"),
                          QStringLiteral("ignore")}));
    QCOMPARE(flattened.actionChain.entries.size(), 6);
    QCOMPARE(flattened.actionChain.entries[0].scope,
             GhosttyActionScope::Surface);
    QVERIFY(flattened.actionChain.entries[0].action.has_value());
    QCOMPARE(flattened.actionChain.entries[1].scope,
             GhosttyActionScope::Application);
    QVERIFY(!flattened.actionChain.entries[1].action.has_value());
    QCOMPARE(flattened.actionChain.entries[2].scope,
             GhosttyActionScope::Application);
    QVERIFY(!flattened.actionChain.entries[2].action.has_value());
    QCOMPARE(flattened.actionChain.entries[3].scope,
             GhosttyActionScope::Surface);
    QVERIFY(!flattened.actionChain.entries[3].action.has_value());
    QCOMPARE(flattened.actionChain.inputEffect,
             GhosttyActionInputEffect::ClosingAction);
    QVERIFY(!flattened.actionChain.applicationOnly);

    GhosttyKeybindState structuredSet;
    GhosttyKeybindMatch rootSnapshot;
    GhosttyKeybindMatch namedSnapshot;
    {
        GhosttyKeybindConfig config;
        config.root = {GhosttyKeybindDefinition{
            .sequence = {unicodeTrigger('r')},
            .actions = {QStringLiteral("search:root:needle"),
                        QStringLiteral("toggle_visibility"),
                        QStringLiteral("new_tab")},
        }};
        config.tables = {GhosttyKeybindTable{
            .name = QStringLiteral("owned"),
            .bindings = {GhosttyKeybindDefinition{
                .sequence = {unicodeTrigger('n')},
                .actions = {QStringLiteral("text:table:payload"),
                            QStringLiteral("reload_config:bogus"),
                            QStringLiteral("reload_config")},
            }},
        }};
        const GhosttyKeybindSource source =
            GhosttyKeybindSource::structured(std::move(config));
        QCOMPARE(
            installProgram(structuredSet, source).count(Disposition::Installed),
            2);
        rootSnapshot = requireMatch(structuredSet.match(
            Qt::Key_R, Qt::NoModifier, QStringLiteral("r")));
        QVERIFY(structuredSet.activateTable(QStringLiteral("owned")));
        namedSnapshot = structuredSet
                            .advance({
                                .qtKey = Qt::Key_N,
                                .text = QStringLiteral("n"),
                            })
                            .match;
    }

    const GhosttyKeybindCompilation replacement =
        GhosttyKeybindProgram::compile({QStringLiteral("ctrl+x=reset")});
    QVERIFY(structuredSet.replaceProgram(replacement.program));
    QVERIFY(structuredSet.program().isSameGeneration(replacement.program));
    QVERIFY(!structuredSet.hasTable(QStringLiteral("owned")));

    QCOMPARE(serializedActions(rootSnapshot),
             QStringList({QStringLiteral("search:root:needle"),
                          QStringLiteral("toggle_visibility"),
                          QStringLiteral("new_tab")}));
    QCOMPARE(rootSnapshot.actionChain.entries[0].scope,
             GhosttyActionScope::Surface);
    QCOMPARE(rootSnapshot.actionChain.entries[1].scope,
             GhosttyActionScope::Application);
    QVERIFY(!rootSnapshot.actionChain.entries[1].action.has_value());
    QCOMPARE(rootSnapshot.actionChain.entries[2].scope,
             GhosttyActionScope::Surface);
    const GhosttyPaneAction *rootPane =
        rootSnapshot.actionChain.entries[0].getIf<GhosttyPaneAction>();
    QVERIFY(rootPane != nullptr);
    const auto *search = std::get_if<GhosttyPaneActions::Search>(rootPane);
    QVERIFY(search != nullptr);
    QCOMPARE(search->serializedNeedle, QByteArray("root:needle"));

    QCOMPARE(serializedActions(namedSnapshot),
             QStringList({QStringLiteral("text:table:payload"),
                          QStringLiteral("reload_config:bogus"),
                          QStringLiteral("reload_config")}));
    QCOMPARE(namedSnapshot.actionChain.entries[0].scope,
             GhosttyActionScope::Surface);
    QCOMPARE(namedSnapshot.actionChain.entries[1].scope,
             GhosttyActionScope::Application);
    QVERIFY(!namedSnapshot.actionChain.entries[1].action.has_value());
    QCOMPARE(namedSnapshot.actionChain.entries[2].scope,
             GhosttyActionScope::Application);
    const GhosttyPaneAction *namedPane =
        namedSnapshot.actionChain.entries[0].getIf<GhosttyPaneAction>();
    QVERIFY(namedPane != nullptr);
    const auto *text = std::get_if<GhosttyPaneActions::SendText>(namedPane);
    QVERIFY(text != nullptr);
    QCOMPARE(text->serializedBytes, QByteArray("table:payload"));
}

void GhosttyKeybindStateTest::preservesLocalFlags()
{
    GhosttyKeybindState set;
    const GhosttyKeybindLoadReport report = installProgram(
        set,
        {
            QStringLiteral(
                "performable:unconsumed:ctrl+shift+c=copy_to_clipboard:mixed"),
            QStringLiteral("shift+insert=paste_from_selection"),
        });

    QCOMPARE(report.count(Disposition::Installed), 2);
    const GhosttyKeybindMatch &copy = requireMatch(
        set.match(Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier,
                  QStringLiteral("C")));
    QVERIFY(!copy.consumed);
    QVERIFY(copy.performable);

    const GhosttyKeybindMatch &paste =
        requireMatch(set.match(Qt::Key_Insert, Qt::ShiftModifier));
    QVERIFY(paste.consumed);
    QVERIFY(!paste.performable);
    QVERIFY(paste.physical);
    QVERIFY(!set.match(Qt::Key_Insert, Qt::ShiftModifier | Qt::KeypadModifier)
                 .has_value());
}

void GhosttyKeybindStateTest::
    supportsSequencesAndCatchAllWhileRejectingDeferredForms()
{
    GhosttyKeybindState set;
    const GhosttyKeybindLoadReport report = installProgram(
        set,
        {
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
    QVERIFY(!leaf.activeTablesChanged);
    QCOMPARE(serializedActions(leaf),
             QStringList({QStringLiteral("new_window")}));

    const GhosttyKeybindStep catchAll = set.advance({
        .qtKey = Qt::Key_Z,
        .modifiers = Qt::ControlModifier,
        .text = QStringLiteral("z"),
    });
    QCOMPARE(catchAll.kind, GhosttyKeybindStepKind::Binding);
    QCOMPARE(serializedActions(catchAll),
             QStringList({QStringLiteral("ignore")}));
}

void GhosttyKeybindStateTest::advancesSharedPrefixSequences()
{
    GhosttyKeybindState set;
    QCOMPARE(installProgram(set,
                            {
                                QStringLiteral("ctrl+a>b=new_tab"),
                                QStringLiteral("ctrl+a>c=next_tab"),
                                QStringLiteral("ctrl+a>d>e=previous_tab"),
                            })
                 .count(Disposition::Installed),
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
    QCOMPARE(serializedActions(step),
             QStringList({QStringLiteral("next_tab")}));
    QCOMPARE(step.queuedEvents.size(), 1);
    QVERIFY(!set.sequenceActive());

    QCOMPARE(set.advance({
                             .qtKey = Qt::Key_A,
                             .modifiers = Qt::ControlModifier,
                             .text = QStringLiteral("a"),
                         })
                 .kind,
             GhosttyKeybindStepKind::Leader);
    step = set.advance({.qtKey = Qt::Key_D, .text = QStringLiteral("d")});
    QCOMPARE(step.kind, GhosttyKeybindStepKind::Leader);
    QCOMPARE(step.queuedEvents.size(), 2);
    step = set.advance({.qtKey = Qt::Key_E, .text = QStringLiteral("e")});
    QCOMPARE(step.kind, GhosttyKeybindStepKind::Binding);
    QCOMPARE(serializedActions(step),
             QStringList({QStringLiteral("previous_tab")}));
    QCOMPARE(step.queuedEvents.size(), 2);
}

void GhosttyKeybindStateTest::retainsConfiguredLabelsForActiveSequence()
{
    GhosttyKeybindConfig config;
    config.root = {
        binding(
            {
                GhosttyKeybindTrigger{
                    .kind = GhosttyKeybindKeyKind::Physical,
                    .physicalName = QStringLiteral("backslash"),
                    .modifiers = GhosttyKeybindSuper | GhosttyKeybindCtrl
                        | GhosttyKeybindAlt | GhosttyKeybindShift,
                },
                GhosttyKeybindTrigger{
                    .kind = GhosttyKeybindKeyKind::Physical,
                    .physicalName = QStringLiteral("key_a"),
                    .modifiers = GhosttyKeybindAlt,
                },
                unicodeTrigger('z'),
            },
            QStringLiteral("new_tab")),
    };

    GhosttyKeybindState set;
    (void)installProgram(set, config);
    QCOMPARE(
        set.advance({
                        .qtKey = Qt::Key_Backslash,
                        .modifiers = Qt::ShiftModifier | Qt::ControlModifier
                            | Qt::AltModifier | Qt::MetaModifier,
                        .nativeScanCode = xkbKeycode(KEY_BACKSLASH),
                    })
            .kind,
        GhosttyKeybindStepKind::Leader);
    QCOMPARE(set.activeSequenceLabels(),
             QStringList({QStringLiteral("Super+Ctrl+Alt+Shift+Backslash")}));

    QCOMPARE(set.advance({
                             .qtKey = Qt::Key_A,
                             .modifiers = Qt::AltModifier,
                             .text = QStringLiteral("a"),
                             .nativeScanCode = xkbKeycode(KEY_A),
                         })
                 .kind,
             GhosttyKeybindStepKind::Leader);
    QCOMPARE(set.activeSequenceLabels(),
             QStringList({QStringLiteral("Super+Ctrl+Alt+Shift+Backslash"),
                          QStringLiteral("Alt+A")}));

    // The labels belong to the configured trie edges, not the event text
    // which happened to match their physical identity.
    QCOMPARE(set.advance({
                             .qtKey = Qt::Key_Z,
                             .text = QStringLiteral("Z"),
                         })
                 .kind,
             GhosttyKeybindStepKind::Binding);
    QVERIFY(set.activeSequenceLabels().isEmpty());

    QCOMPARE(
        set.advance({
                        .qtKey = Qt::Key_Backslash,
                        .modifiers = Qt::ShiftModifier | Qt::ControlModifier
                            | Qt::AltModifier | Qt::MetaModifier,
                        .nativeScanCode = xkbKeycode(KEY_BACKSLASH),
                    })
            .kind,
        GhosttyKeybindStepKind::Leader);
    set.resetSequence();
    QVERIFY(set.activeSequenceLabels().isEmpty());
}

void GhosttyKeybindStateTest::recoversInvalidSequencesAndHonorsCatchAll()
{
    GhosttyKeybindState set;
    (void)installProgram(set, {QStringLiteral("ctrl+a>b=new_tab")});
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
                         })
                 .kind,
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
    (void)installProgram(set,
                         {
                             QStringLiteral("ctrl+a>b=new_tab"),
                             QStringLiteral("catch_all=ignore"),
                         });
    QCOMPARE(set.advance(leader).kind, GhosttyKeybindStepKind::Leader);
    step = set.advance({.qtKey = Qt::Key_Z, .text = QStringLiteral("z")});
    QCOMPARE(step.kind, GhosttyKeybindStepKind::IgnoredSequence);
    QCOMPARE(step.queuedEvents, QVector<GhosttyKeybindEvent>({leader}));

    // Recovery still keys off the presence of ignore in a chain, while the
    // compiled input effect retains closing-before-ignore precedence.
    (void)installProgram(set,
                         {
                             QStringLiteral("ctrl+a>b=new_tab"),
                             QStringLiteral("catch_all=close_window"),
                             QStringLiteral("chain=ignore"),
                         });
    const GhosttyKeybindMatch closeAndIgnore =
        requireMatch(set.match(Qt::Key_Z, Qt::NoModifier, QStringLiteral("z")));
    QCOMPARE(serializedActions(closeAndIgnore),
             QStringList(
                 {QStringLiteral("close_window"), QStringLiteral("ignore")}));
    QCOMPARE(closeAndIgnore.actionChain.inputEffect,
             GhosttyActionInputEffect::ClosingAction);
    QCOMPARE(set.advance(leader).kind, GhosttyKeybindStepKind::Leader);
    QCOMPARE(
        set.advance({.qtKey = Qt::Key_Z, .text = QStringLiteral("z")}).kind,
        GhosttyKeybindStepKind::IgnoredSequence);

    // A non-ignore root catch-all is not executed during recovery.
    (void)installProgram(set,
                         {
                             QStringLiteral("ctrl+a>b=new_tab"),
                             QStringLiteral("catch_all=reload_config"),
                         });
    QCOMPARE(set.advance(leader).kind, GhosttyKeybindStepKind::Leader);
    QCOMPARE(
        set.advance({.qtKey = Qt::Key_Z, .text = QStringLiteral("z")}).kind,
        GhosttyKeybindStepKind::InvalidSequence);

    // Parameterized ignore is invalid Ghostty action grammar and must not
    // inherit ignore's special invalid-sequence behavior.
    (void)installProgram(set,
                         {
                             QStringLiteral("ctrl+a>b=new_tab"),
                             QStringLiteral("catch_all=ignore:bogus"),
                         });
    QCOMPARE(set.advance(leader).kind, GhosttyKeybindStepKind::Leader);
    QCOMPARE(
        set.advance({.qtKey = Qt::Key_Z, .text = QStringLiteral("z")}).kind,
        GhosttyKeybindStepKind::InvalidSequence);

    // A catch-all inside the active sequence is an ordinary leaf.
    (void)installProgram(set,
                         {QStringLiteral("ctrl+a>catch_all=reload_config")});
    QCOMPARE(set.advance(leader).kind, GhosttyKeybindStepKind::Leader);
    step = set.advance({.qtKey = Qt::Key_Z, .text = QStringLiteral("z")});
    QCOMPARE(step.kind, GhosttyKeybindStepKind::Binding);
    QCOMPARE(serializedActions(step),
             QStringList({QStringLiteral("reload_config")}));
}

void GhosttyKeybindStateTest::preservesLookupPriorityInsideSequences()
{
    GhosttyKeybindState set;
    (void)installProgram(
        set,
        {
            QStringLiteral("ctrl+a>b=unicode_leaf"),
            QStringLiteral("ctrl+a>key_b=physical_leaf"),
            QStringLiteral("ctrl+x>catch_all=bare_catch_all"),
            QStringLiteral("ctrl+x>ctrl+catch_all=modified_catch_all"),
        });

    QCOMPARE(set.advance({
                             .qtKey = Qt::Key_A,
                             .modifiers = Qt::ControlModifier,
                             .text = QStringLiteral("a"),
                         })
                 .kind,
             GhosttyKeybindStepKind::Leader);
    GhosttyKeybindStep step = set.advance({
        .qtKey = Qt::Key_B,
        .text = QStringLiteral("b"),
        .nativeScanCode = xkbKeycode(KEY_B),
        .unshiftedCodepoint = 'b',
    });
    QCOMPARE(step.kind, GhosttyKeybindStepKind::Binding);
    QVERIFY(step.match.physical);
    QCOMPARE(serializedActions(step),
             QStringList({QStringLiteral("physical_leaf")}));

    QCOMPARE(set.advance({
                             .qtKey = Qt::Key_X,
                             .modifiers = Qt::ControlModifier,
                             .text = QStringLiteral("x"),
                         })
                 .kind,
             GhosttyKeybindStepKind::Leader);
    step = set.advance({
        .qtKey = Qt::Key_Z,
        .modifiers = Qt::ControlModifier,
        .text = QStringLiteral("z"),
    });
    QCOMPARE(serializedActions(step),
             QStringList({QStringLiteral("modified_catch_all")}));

    QCOMPARE(set.advance({
                             .qtKey = Qt::Key_X,
                             .modifiers = Qt::ControlModifier,
                             .text = QStringLiteral("x"),
                         })
                 .kind,
             GhosttyKeybindStepKind::Leader);
    step = set.advance({.qtKey = Qt::Key_Z, .text = QStringLiteral("z")});
    QCOMPARE(serializedActions(step),
             QStringList({QStringLiteral("bare_catch_all")}));
}

void GhosttyKeybindStateTest::sharesProgramWithoutSharingMutableState()
{
    GhosttyKeybindConfig config;
    config.root = {
        binding({unicodeTrigger('a', GhosttyKeybindCtrl), unicodeTrigger('b')},
                QStringLiteral("root_sequence")),
    };
    config.tables = {
        GhosttyKeybindTable{
            .name = QStringLiteral("named"),
            .bindings =
                {
                    binding({unicodeTrigger('n')},
                            QStringLiteral("named_binding")),
                },
        },
    };
    const GhosttyKeybindCompilation compilation =
        GhosttyKeybindProgram::compile(config);
    QCOMPARE(compilation.report.count(Disposition::Installed), 2);
    QVERIFY(compilation.program.isAvailable());
    QCOMPARE(compilation.program.size(), 2);
    GhosttyKeybindState first(compilation.program);
    GhosttyKeybindState second(compilation.program);
    QVERIFY(compilation.program.isSameGeneration(first.program()));
    QVERIFY(first.program().isSameGeneration(second.program()));

    const GhosttyKeybindEvent rootLeader{
        .qtKey = Qt::Key_A,
        .modifiers = Qt::ControlModifier,
        .text = QStringLiteral("a"),
    };
    const GhosttyKeybindEvent rootLeaf{
        .qtKey = Qt::Key_B,
        .text = QStringLiteral("b"),
    };
    QCOMPARE(first.advance(rootLeader).kind, GhosttyKeybindStepKind::Leader);
    QVERIFY(first.sequenceActive());
    QVERIFY(!second.sequenceActive());
    QCOMPARE(second.advance(rootLeaf).kind, GhosttyKeybindStepKind::Unmatched);
    QCOMPARE(second.advance(rootLeader).kind, GhosttyKeybindStepKind::Leader);
    QCOMPARE(serializedActions(first.advance(rootLeaf)),
             QStringList({QStringLiteral("root_sequence")}));
    QVERIFY(!first.sequenceActive());
    QVERIFY(second.sequenceActive());
    QCOMPARE(serializedActions(second.advance(rootLeaf)),
             QStringList({QStringLiteral("root_sequence")}));

    QVERIFY(first.activateTable(QStringLiteral("named")));
    QCOMPARE(serializedActions(first.advance({
                 .qtKey = Qt::Key_N,
                 .text = QStringLiteral("n"),
             })),
             QStringList({QStringLiteral("named_binding")}));
    QCOMPARE(second
                 .advance({
                     .qtKey = Qt::Key_N,
                     .text = QStringLiteral("n"),
                 })
                 .kind,
             GhosttyKeybindStepKind::Unmatched);
    QCOMPARE(first.activeTableNames(), QStringList({QStringLiteral("named")}));
    QVERIFY(second.activeTableNames().isEmpty());
    QVERIFY(first.deactivateTable());
}

void GhosttyKeybindStateTest::loadsStructuredDefinitionsAndRetainsFlags()
{
    GhosttyKeybindConfig config;
    config.root = {
        GhosttyKeybindDefinition{
            .sequence =
                {
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

    GhosttyKeybindState set;
    const GhosttyKeybindLoadReport report = installProgram(set, config);
    QCOMPARE(report.count(Disposition::Installed), 4);
    QCOMPARE(report.count(Disposition::Unsupported), 0);
    QCOMPARE(set.size(), 4);
    QVERIFY(set.hasTable(QStringLiteral("copy-mode")));

    const GhosttyKeybindMatch all =
        requireMatch(set.match(Qt::Key_G, Qt::NoModifier, QStringLiteral("g")));
    QVERIFY(all.all);
    QVERIFY(!all.global);

    QCOMPARE(set.advance({
                             .qtKey = Qt::Key_X,
                             .modifiers = Qt::ControlModifier,
                             .text = QStringLiteral("x"),
                         })
                 .kind,
             GhosttyKeybindStepKind::Leader);
    const GhosttyKeybindStep step = set.advance({
        .qtKey = Qt::Key_Y,
        .text = QStringLiteral("y"),
        .nativeScanCode = xkbKeycode(KEY_Y),
    });
    QCOMPARE(step.kind, GhosttyKeybindStepKind::Binding);
    QCOMPARE(serializedActions(step),
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
    QCOMPARE(serializedActions(table),
             QStringList({QStringLiteral("copy_to_clipboard:mixed")}));
    QVERIFY(!table.match.all);
    QVERIFY(table.match.global);

    const QStringList actions = set.serializedActions();
    QVERIFY(actions.contains(QStringLiteral("toggle_quick_terminal")));
    QVERIFY(actions.contains(QStringLiteral("copy_to_clipboard:mixed")));
}

void GhosttyKeybindStateTest::rejectsBroadStructuredSequences()
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

    GhosttyKeybindState set;
    const GhosttyKeybindLoadReport report = installProgram(set, config);
    QCOMPARE(report.count(Disposition::Invalid), 2);
    QCOMPARE(report.count(Disposition::Installed), 1);
    QCOMPARE(set.size(), 1);

    const GhosttyKeybindMatch match =
        requireMatch(set.match(Qt::Key_V, Qt::NoModifier, QStringLiteral("v")));
    QVERIFY(match.all);
}

void GhosttyKeybindStateTest::routesNamedTablesNewestToOldestAndRoot()
{
    GhosttyKeybindConfig config;
    config.root = {
        binding({unicodeTrigger('q')}, QStringLiteral("root_q")),
        binding({unicodeTrigger('r')}, QStringLiteral("root_r")),
    };
    config.tables = {
        GhosttyKeybindTable{
            .name = QStringLiteral("A"),
            .bindings =
                {
                    binding({unicodeTrigger('q')}, QStringLiteral("table_a_q")),
                    binding({unicodeTrigger('a')}, QStringLiteral("table_a_a")),
                },
        },
        GhosttyKeybindTable{
            .name = QStringLiteral("B"),
            .bindings =
                {
                    binding({unicodeTrigger('b')}, QStringLiteral("table_b_b")),
                },
        },
        GhosttyKeybindTable{
            .name = QStringLiteral("C"),
            .bindings =
                {
                    binding({catchAllTrigger()}, QStringLiteral("table_c_any")),
                },
        },
    };

    GhosttyKeybindState set;
    QCOMPARE(installProgram(set, config).count(Disposition::Installed), 6);
    QVERIFY(set.activateTable(QStringLiteral("A")));
    QVERIFY(set.activateTable(QStringLiteral("B")));
    QCOMPARE(set.activeTableNames(),
             QStringList({QStringLiteral("A"), QStringLiteral("B")}));

    GhosttyKeybindStep step = set.advance({
        .qtKey = Qt::Key_Q,
        .text = QStringLiteral("q"),
    });
    QCOMPARE(serializedActions(step),
             QStringList({QStringLiteral("table_a_q")}));

    step = set.advance({.qtKey = Qt::Key_B, .text = QStringLiteral("b")});
    QCOMPARE(serializedActions(step),
             QStringList({QStringLiteral("table_b_b")}));

    step = set.advance({.qtKey = Qt::Key_R, .text = QStringLiteral("r")});
    QCOMPARE(serializedActions(step), QStringList({QStringLiteral("root_r")}));

    QVERIFY(set.activateTable(QStringLiteral("C")));
    step = set.advance({.qtKey = Qt::Key_Q, .text = QStringLiteral("q")});
    QCOMPARE(serializedActions(step),
             QStringList({QStringLiteral("table_c_any")}));
    QCOMPARE(set.activeTableNames(),
             QStringList({QStringLiteral("A"), QStringLiteral("B"),
                          QStringLiteral("C")}));
}

void GhosttyKeybindStateTest::enforcesTableStackAndActivationRules()
{
    GhosttyKeybindConfig config;
    config.tables = {
        GhosttyKeybindTable{.name = QStringLiteral("A")},
        GhosttyKeybindTable{.name = QStringLiteral("B")},
    };

    GhosttyKeybindState set;
    (void)installProgram(set, config);
    QVERIFY(!set.hasActiveTables());
    QVERIFY(set.hasTable(QStringLiteral("A")));
    QVERIFY(!set.hasTable(QStringLiteral("missing")));
    QVERIFY(!set.activateTable(QStringLiteral("missing")));
    QVERIFY(set.canActivateTable(QStringLiteral("A")));
    QVERIFY(set.activateTable(QStringLiteral("A")));
    QVERIFY(set.hasActiveTables());
    QVERIFY(!set.canActivateTable(QStringLiteral("A")));
    QVERIFY(!set.activateTable(QStringLiteral("A")));
    QVERIFY(set.activateTable(QStringLiteral("B")));
    QVERIFY(set.activateTable(QStringLiteral("A")));
    QCOMPARE(set.activeTableNames(),
             QStringList({QStringLiteral("A"), QStringLiteral("B"),
                          QStringLiteral("A")}));
    QVERIFY(set.deactivateTable());
    QVERIFY(set.hasActiveTables());
    QCOMPARE(set.activeTableNames(),
             QStringList({QStringLiteral("A"), QStringLiteral("B")}));
    QVERIFY(set.deactivateAllTables());
    QVERIFY(!set.hasActiveTables());
    QVERIFY(!set.deactivateAllTables());
    QVERIFY(!set.deactivateTable());

    for (qsizetype index = 0; index < GhosttyKeybindState::MaximumActiveTables;
         ++index) {
        const QString name =
            index % 2 == 0 ? QStringLiteral("A") : QStringLiteral("B");
        QVERIFY(set.activateTable(name));
    }
    QCOMPARE(set.activeTableNames().size(),
             GhosttyKeybindState::MaximumActiveTables);
    QVERIFY(!set.canActivateTable(QStringLiteral("A")));
    QVERIFY(!set.activateTable(QStringLiteral("A")));
}

void GhosttyKeybindStateTest::handlesOneShotTablesExactly()
{
    GhosttyKeybindConfig config;
    config.root = {
        binding({unicodeTrigger('r')}, QStringLiteral("root")),
    };
    config.tables = {
        GhosttyKeybindTable{
            .name = QStringLiteral("A"),
            .bindings =
                {
                    binding({unicodeTrigger('a')}, QStringLiteral("table_a")),
                    binding({unicodeTrigger('p')},
                            QStringLiteral("performable"),
                            GhosttyKeybindFlags{.performable = true}),
                },
        },
        GhosttyKeybindTable{
            .name = QStringLiteral("B"),
            .bindings =
                {
                    binding({unicodeTrigger('b')}, QStringLiteral("table_b")),
                },
        },
        GhosttyKeybindTable{
            .name = QStringLiteral("C"),
            .bindings =
                {
                    binding({catchAllTrigger()}, QStringLiteral("table_c_any")),
                },
        },
    };

    GhosttyKeybindState set;
    (void)installProgram(set, config);
    QVERIFY(set.activateTable(QStringLiteral("A"), true));
    GhosttyKeybindStep step = set.advance({
        .qtKey = Qt::Key_R,
        .text = QStringLiteral("r"),
    });
    QCOMPARE(serializedActions(step), QStringList({QStringLiteral("root")}));
    QVERIFY(!step.activeTablesChanged);
    QCOMPARE(set.activeTableNames(), QStringList({QStringLiteral("A")}));
    step = set.advance({.qtKey = Qt::Key_A, .text = QStringLiteral("a")});
    QCOMPARE(serializedActions(step), QStringList({QStringLiteral("table_a")}));
    QVERIFY(step.activeTablesChanged);
    QVERIFY(set.activeTableNames().isEmpty());

    QVERIFY(set.activateTable(QStringLiteral("C"), true));
    step = set.advance({.qtKey = Qt::Key_Q, .text = QStringLiteral("q")});
    QCOMPARE(serializedActions(step),
             QStringList({QStringLiteral("table_c_any")}));
    QVERIFY(step.activeTablesChanged);
    QVERIFY(set.activeTableNames().isEmpty());

    QVERIFY(set.activateTable(QStringLiteral("A"), true));
    QVERIFY(set.activateTable(QStringLiteral("B")));
    step = set.advance({.qtKey = Qt::Key_A, .text = QStringLiteral("a")});
    QCOMPARE(serializedActions(step), QStringList({QStringLiteral("table_a")}));
    QVERIFY(!step.activeTablesChanged);
    QCOMPARE(set.activeTableNames(),
             QStringList({QStringLiteral("A"), QStringLiteral("B")}));
    QVERIFY(set.deactivateTable());
    step = set.advance({.qtKey = Qt::Key_A, .text = QStringLiteral("a")});
    QCOMPARE(serializedActions(step), QStringList({QStringLiteral("table_a")}));
    QVERIFY(step.activeTablesChanged);
    QVERIFY(set.activeTableNames().isEmpty());

    QVERIFY(set.activateTable(QStringLiteral("A"), true));
    const GhosttyKeybindStep performable = set.advance({
        .qtKey = Qt::Key_P,
        .text = QStringLiteral("p"),
    });
    QCOMPARE(performable.kind, GhosttyKeybindStepKind::Binding);
    QVERIFY(performable.match.performable);
    QVERIFY(performable.activeTablesChanged);
    QVERIFY(set.activeTableNames().isEmpty());
}

void GhosttyKeybindStateTest::keepsTableSequenceAndCatchAllSemantics()
{
    const GhosttyKeybindFlags flags{
        .consumed = false,
        .performable = true,
    };
    GhosttyKeybindConfig config;
    config.tables = {
        GhosttyKeybindTable{
            .name = QStringLiteral("persistent"),
            .bindings =
                {
                    binding({unicodeTrigger('x'), unicodeTrigger('n')},
                            QStringLiteral("persistent_sequence"), flags),
                    binding({catchAllTrigger()}, QStringLiteral("ignore")),
                },
        },
        GhosttyKeybindTable{
            .name = QStringLiteral("shadow"),
            .bindings =
                {
                    binding({catchAllTrigger()},
                            QStringLiteral("reload_config")),
                },
        },
        GhosttyKeybindTable{
            .name = QStringLiteral("one-shot"),
            .bindings =
                {
                    binding({unicodeTrigger('x'), unicodeTrigger('n')},
                            QStringLiteral("one_shot_sequence")),
                    binding({catchAllTrigger()}, QStringLiteral("ignore")),
                },
        },
    };

    GhosttyKeybindState set;
    (void)installProgram(set, config);
    QVERIFY(set.activateTable(QStringLiteral("persistent")));
    QCOMPARE(
        set.advance({.qtKey = Qt::Key_X, .text = QStringLiteral("x")}).kind,
        GhosttyKeybindStepKind::Leader);
    QVERIFY(set.activateTable(QStringLiteral("shadow")));
    QCOMPARE(
        set.advance({.qtKey = Qt::Key_Z, .text = QStringLiteral("z")}).kind,
        GhosttyKeybindStepKind::InvalidSequence);
    QCOMPARE(
        set.activeTableNames(),
        QStringList({QStringLiteral("persistent"), QStringLiteral("shadow")}));

    QVERIFY(set.deactivateTable());
    QCOMPARE(
        set.advance({.qtKey = Qt::Key_X, .text = QStringLiteral("x")}).kind,
        GhosttyKeybindStepKind::Leader);
    QCOMPARE(
        set.advance({.qtKey = Qt::Key_Z, .text = QStringLiteral("z")}).kind,
        GhosttyKeybindStepKind::IgnoredSequence);
    QCOMPARE(set.activeTableNames(),
             QStringList({QStringLiteral("persistent")}));

    QCOMPARE(
        set.advance({.qtKey = Qt::Key_X, .text = QStringLiteral("x")}).kind,
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
    const GhosttyKeybindStep oneShotLeader = set.advance({
        .qtKey = Qt::Key_X,
        .text = QStringLiteral("x"),
    });
    QCOMPARE(oneShotLeader.kind, GhosttyKeybindStepKind::Leader);
    QVERIFY(oneShotLeader.activeTablesChanged);
    QVERIFY(set.activeTableNames().isEmpty());
    QVERIFY(set.sequenceActive());
    const GhosttyKeybindStep oneShotLeaf = set.advance({
        .qtKey = Qt::Key_N,
        .text = QStringLiteral("n"),
    });
    QCOMPARE(serializedActions(oneShotLeaf),
             QStringList({QStringLiteral("one_shot_sequence")}));
    QVERIFY(!oneShotLeaf.activeTablesChanged);

    QVERIFY(set.activateTable(QStringLiteral("one-shot"), true));
    const GhosttyKeybindStep secondOneShotLeader = set.advance({
        .qtKey = Qt::Key_X,
        .text = QStringLiteral("x"),
    });
    QCOMPARE(secondOneShotLeader.kind, GhosttyKeybindStepKind::Leader);
    QVERIFY(secondOneShotLeader.activeTablesChanged);
    QVERIFY(set.activeTableNames().isEmpty());
    QCOMPARE(
        set.advance({.qtKey = Qt::Key_Z, .text = QStringLiteral("z")}).kind,
        GhosttyKeybindStepKind::InvalidSequence);
}

void GhosttyKeybindStateTest::replacingProgramResetsOnlyOwningState()
{
    GhosttyKeybindConfig initial;
    initial.root = {
        binding({unicodeTrigger('a', GhosttyKeybindCtrl), unicodeTrigger('b')},
                QStringLiteral("initial_sequence")),
    };
    initial.tables = {
        GhosttyKeybindTable{
            .name = QStringLiteral("initial-table"),
            .bindings =
                {
                    binding({unicodeTrigger('i')},
                            QStringLiteral("initial_table_binding")),
                },
        },
    };

    GhosttyKeybindConfig replacement;
    replacement.root = {
        binding({unicodeTrigger('x', GhosttyKeybindCtrl), unicodeTrigger('y')},
                QStringLiteral("replacement_sequence")),
    };
    replacement.tables = {
        GhosttyKeybindTable{
            .name = QStringLiteral("replacement-table"),
            .bindings =
                {
                    binding({unicodeTrigger('r')},
                            QStringLiteral("replacement_table_binding")),
                },
        },
    };

    const GhosttyKeybindCompilation initialCompilation =
        GhosttyKeybindProgram::compile(initial);
    const GhosttyKeybindCompilation replacementCompilation =
        GhosttyKeybindProgram::compile(replacement);
    GhosttyKeybindState replaced(initialCompilation.program);
    GhosttyKeybindState survivor(initialCompilation.program);
    QVERIFY(replaced.program().isSameGeneration(survivor.program()));
    QVERIFY(replaced.activateTable(QStringLiteral("initial-table")));
    QVERIFY(survivor.activateTable(QStringLiteral("initial-table")));

    const GhosttyKeybindEvent initialLeader{
        .qtKey = Qt::Key_A,
        .modifiers = Qt::ControlModifier,
        .text = QStringLiteral("a"),
    };
    QCOMPARE(replaced.advance(initialLeader).kind,
             GhosttyKeybindStepKind::Leader);
    QCOMPARE(survivor.advance(initialLeader).kind,
             GhosttyKeybindStepKind::Leader);
    QVERIFY(replaced.sequenceActive());
    QVERIFY(survivor.sequenceActive());

    QVERIFY(replaced.replaceProgram(replacementCompilation.program));
    QVERIFY(!replaced.sequenceActive());
    QVERIFY(replaced.activeTableNames().isEmpty());
    QVERIFY(!replaced.hasTable(QStringLiteral("initial-table")));
    QVERIFY(replaced.hasTable(QStringLiteral("replacement-table")));
    QVERIFY(!replaced.program().isSameGeneration(survivor.program()));

    QVERIFY(survivor.sequenceActive());
    QCOMPARE(survivor.activeTableNames(),
             QStringList({QStringLiteral("initial-table")}));
    QCOMPARE(serializedActions(survivor.advance({
                 .qtKey = Qt::Key_B,
                 .text = QStringLiteral("b"),
             })),
             QStringList({QStringLiteral("initial_sequence")}));
    QCOMPARE(serializedActions(survivor.advance({
                 .qtKey = Qt::Key_I,
                 .text = QStringLiteral("i"),
             })),
             QStringList({QStringLiteral("initial_table_binding")}));

    QVERIFY(replaced.activateTable(QStringLiteral("replacement-table")));
    QCOMPARE(replaced
                 .advance({
                     .qtKey = Qt::Key_X,
                     .modifiers = Qt::ControlModifier,
                     .text = QStringLiteral("x"),
                 })
                 .kind,
             GhosttyKeybindStepKind::Leader);
    QVERIFY(!replaced.replaceProgram(replacementCompilation.program));
    QVERIFY(replaced.sequenceActive());
    QCOMPARE(replaced.activeTableNames(),
             QStringList({QStringLiteral("replacement-table")}));
    QCOMPARE(serializedActions(replaced.advance({
                 .qtKey = Qt::Key_Y,
                 .text = QStringLiteral("y"),
             })),
             QStringList({QStringLiteral("replacement_sequence")}));
    QCOMPARE(serializedActions(replaced.advance({
                 .qtKey = Qt::Key_R,
                 .text = QStringLiteral("r"),
             })),
             QStringList({QStringLiteral("replacement_table_binding")}));
}

void GhosttyKeybindStateTest::rejectsMalformedBindings()
{
    GhosttyKeybindState set;
    const GhosttyKeybindLoadReport report = installProgram(
        set,
        {
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

void GhosttyKeybindStateTest::replacesDuplicateTriggers()
{
    GhosttyKeybindState set;
    const GhosttyKeybindLoadReport report =
        installProgram(set,
                       {
                           QStringLiteral("ctrl+A=first"),
                           QStringLiteral("unconsumed:ctrl+a=second"),
                           QStringLiteral("chain=third"),
                       });

    QCOMPARE(report.count(Disposition::Installed), 2);
    QCOMPARE(set.size(), 1);
    const GhosttyKeybindMatch &match = requireMatch(
        set.match(Qt::Key_A, Qt::ControlModifier, QStringLiteral("a")));
    QCOMPARE(serializedActions(match),
             QStringList({QStringLiteral("second"), QStringLiteral("third")}));
    QVERIFY(!match.consumed);
}

void GhosttyKeybindStateTest::matchesLinuxDefaultLikeBindings()
{
    GhosttyKeybindState set;
    const GhosttyKeybindLoadReport report = installProgram(
        set,
        {
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

    QCOMPARE(serializedActions(requireMatch(set.match(
                 Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier))),
             QStringList({QStringLiteral("previous_tab")}));
    QCOMPARE(serializedActions(
                 requireMatch(set.match(Qt::Key_Tab, Qt::ControlModifier))),
             QStringList({QStringLiteral("next_tab")}));
    QCOMPARE(serializedActions(requireMatch(
                 set.match(Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier,
                           QString(QChar(0x14))))),
             QStringList({QStringLiteral("new_tab")}));
    QCOMPARE(serializedActions(requireMatch(set.match(
                 Qt::Key_Down, Qt::ControlModifier | Qt::AltModifier))),
             QStringList({QStringLiteral("goto_split:down")}));

    // Both bindings exist in Ghostty's Linux defaults. Physical digit_1 wins.
    const GhosttyKeybindMatch &digit = requireMatch(
        set.match(Qt::Key_1, Qt::AltModifier, QStringLiteral("1")));
    QVERIFY(digit.physical);
    QCOMPARE(serializedActions(digit),
             QStringList({QStringLiteral("goto_tab:physical")}));

    QCOMPARE(serializedActions(
                 requireMatch(set.match(Qt::Key_Escape, Qt::NoModifier))),
             QStringList({QStringLiteral("end_search")}));
    QCOMPARE(serializedActions(
                 requireMatch(set.match(Qt::Key_Return, Qt::ControlModifier))),
             QStringList({QStringLiteral("toggle_fullscreen")}));
}

QTEST_APPLESS_MAIN(GhosttyKeybindStateTest)

#include "test_ghostty_keybind_set.moc"
