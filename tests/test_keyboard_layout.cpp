#include "keyboard_layout.h"

#include <QByteArray>
#include <QTest>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-names.h>
#include <xkbcommon/xkbcommon.h>

#include <cstdlib>
#include <limits>

namespace {

constexpr quint32 xkbKeycode(unsigned int evdevCode)
{
    return static_cast<quint32>(evdevCode + 8U);
}

class TestKeymap final {
public:
    explicit TestKeymap(const char *layout, const char *variant = nullptr)
        : context_(xkb_context_new(XKB_CONTEXT_NO_ENVIRONMENT_NAMES))
    {
        if (context_ == nullptr) return;

        const xkb_rule_names names{
            .rules = "evdev",
            .model = "pc105",
            .layout = layout,
            .variant = variant,
            .options = "",
        };
        keymap_ = xkb_keymap_new_from_names(context_, &names,
                                            XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (keymap_ == nullptr) return;

        char *const text =
            xkb_keymap_get_as_string(keymap_, XKB_KEYMAP_FORMAT_TEXT_V1);
        if (text == nullptr) return;
        serialized_ = QByteArray(text);
        std::free(text);
    }

    ~TestKeymap()
    {
        if (keymap_ != nullptr) xkb_keymap_unref(keymap_);
        if (context_ != nullptr) xkb_context_unref(context_);
    }

    TestKeymap(const TestKeymap &) = delete;
    TestKeymap &operator=(const TestKeymap &) = delete;

    [[nodiscard]] bool isValid() const noexcept
    {
        return keymap_ != nullptr && !serialized_.isEmpty();
    }

    [[nodiscard]] const QByteArray &serialized() const noexcept
    {
        return serialized_;
    }

    [[nodiscard]] quint32 modifierMask(const char *name) const
    {
        if (keymap_ == nullptr) return 0;
        const xkb_mod_index_t index = xkb_keymap_mod_get_index(keymap_, name);
        if (index == XKB_MOD_INVALID
            || index >= std::numeric_limits<xkb_mod_mask_t>::digits) {
            return 0;
        }
        return quint32{1} << index;
    }

private:
    xkb_context *context_ = nullptr;
    xkb_keymap *keymap_ = nullptr;
    QByteArray serialized_;
};

void verifyTranslation(const KeyboardLayoutTranslation &translation,
                       uint32_t unshiftedCodepoint,
                       Qt::KeyboardModifiers consumedModifiers)
{
    QVERIFY(translation.authoritative);
    QCOMPARE(translation.unshiftedCodepoint, unshiftedCodepoint);
    QCOMPARE(translation.consumedModifiers, consumedModifiers);
}

} // namespace

class KeyboardLayoutTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void translatesUsShiftAndCapsLock();
    void followsGermanLetterAndLevelThreeMappings();
    void followsFrenchShiftedDigitMapping();
    void switchesActiveLayoutGroups();
    void treatsDeadKeysAsAuthoritativeWithoutInventingText();
    void keepsValidMapAfterRejectedReplacement();
    void rejectsTranslationWithoutUsableNativeIdentity();
    void preservesCapturedTranslationDuringReplay();
};

void KeyboardLayoutTest::translatesUsShiftAndCapsLock()
{
    const TestKeymap keymap("us");
    QVERIFY(keymap.isValid());
    const quint32 shift = keymap.modifierMask(XKB_MOD_NAME_SHIFT);
    const quint32 caps = keymap.modifierMask(XKB_MOD_NAME_CAPS);
    QVERIFY(shift != 0);
    QVERIFY(caps != 0);

    XkbKeyboardLayout layout;
    QVERIFY(layout.installKeymap(keymap.serialized()));

    verifyTranslation(layout.translate(xkbKeycode(KEY_SEMICOLON)), ';',
                      Qt::NoModifier);

    layout.updateModifiers(shift, 0, 0, 0);
    verifyTranslation(layout.translate(xkbKeycode(KEY_SEMICOLON)), ';',
                      Qt::ShiftModifier);

    layout.resetModifiers();
    verifyTranslation(layout.translate(xkbKeycode(KEY_SEMICOLON), shift), ';',
                      Qt::ShiftModifier);

    layout.updateModifiers(0, 0, caps, 0);
    const KeyboardLayoutTranslation capsTranslation =
        layout.translate(xkbKeycode(KEY_A));
    verifyTranslation(capsTranslation, 'a', Qt::NoModifier);
    QVERIFY(capsTranslation.capsLock);
    QVERIFY(!capsTranslation.numLock);
    QVERIFY(capsTranslation.consumedCapsLock);

    layout.resetModifiers();
    const KeyboardLayoutTranslation effectiveCapsTranslation =
        layout.translate(xkbKeycode(KEY_A), caps);
    QVERIFY(effectiveCapsTranslation.capsLock);
    QVERIFY(effectiveCapsTranslation.consumedCapsLock);

    layout.updateModifiers(shift, 0, caps, 0);
    const KeyboardLayoutTranslation shiftedCapsTranslation =
        layout.translate(xkbKeycode(KEY_A));
    verifyTranslation(shiftedCapsTranslation, 'a', Qt::ShiftModifier);
    QVERIFY(shiftedCapsTranslation.capsLock);
    QVERIFY(shiftedCapsTranslation.consumedCapsLock);

    const quint32 num = keymap.modifierMask(XKB_MOD_NAME_NUM);
    QVERIFY(num != 0);
    layout.updateModifiers(0, 0, num, 0);
    const KeyboardLayoutTranslation numTranslation =
        layout.translate(xkbKeycode(KEY_KP1));
    QVERIFY(numTranslation.authoritative);
    QVERIFY(!numTranslation.capsLock);
    QVERIFY(numTranslation.numLock);

    layout.resetModifiers();
    QVERIFY(layout.translate(xkbKeycode(KEY_KP1), num).numLock);
}

void KeyboardLayoutTest::followsGermanLetterAndLevelThreeMappings()
{
    const TestKeymap keymap("de");
    QVERIFY(keymap.isValid());
    const quint32 levelThree = keymap.modifierMask(XKB_MOD_NAME_MOD5);
    QVERIFY(levelThree != 0);

    XkbKeyboardLayout layout;
    QVERIFY(layout.installKeymap(keymap.serialized()));

    verifyTranslation(layout.translate(xkbKeycode(KEY_Y)), 'z', Qt::NoModifier);
    verifyTranslation(layout.translate(xkbKeycode(KEY_Z)), 'y', Qt::NoModifier);

    layout.updateModifiers(levelThree, 0, 0, 0);
    verifyTranslation(layout.translate(xkbKeycode(KEY_Q)), 'q',
                      Qt::GroupSwitchModifier);

    layout.resetModifiers();
    verifyTranslation(layout.translate(xkbKeycode(KEY_Q)), 'q', Qt::NoModifier);
    verifyTranslation(layout.translate(xkbKeycode(KEY_Q), levelThree), 'q',
                      Qt::GroupSwitchModifier);
}

void KeyboardLayoutTest::followsFrenchShiftedDigitMapping()
{
    const TestKeymap keymap("fr");
    QVERIFY(keymap.isValid());
    const quint32 shift = keymap.modifierMask(XKB_MOD_NAME_SHIFT);
    QVERIFY(shift != 0);

    XkbKeyboardLayout layout;
    QVERIFY(layout.installKeymap(keymap.serialized()));

    verifyTranslation(layout.translate(xkbKeycode(KEY_Q)), 'a', Qt::NoModifier);
    verifyTranslation(layout.translate(xkbKeycode(KEY_1)), '&', Qt::NoModifier);

    layout.updateModifiers(shift, 0, 0, 0);
    verifyTranslation(layout.translate(xkbKeycode(KEY_1)), '&',
                      Qt::ShiftModifier);
}

void KeyboardLayoutTest::switchesActiveLayoutGroups()
{
    const TestKeymap keymap("us,de");
    QVERIFY(keymap.isValid());

    XkbKeyboardLayout layout;
    QVERIFY(layout.installKeymap(keymap.serialized()));

    layout.updateModifiers(0, 0, 0, 0);
    verifyTranslation(layout.translate(xkbKeycode(KEY_Y)), 'y', Qt::NoModifier);

    layout.updateModifiers(0, 0, 0, 1);
    verifyTranslation(layout.translate(xkbKeycode(KEY_Y)), 'z', Qt::NoModifier);

    layout.resetModifiers();
    verifyTranslation(layout.translate(xkbKeycode(KEY_Y)), 'y', Qt::NoModifier);
    verifyTranslation(layout.translate(xkbKeycode(KEY_Y), 0, XKB_KEY_z), 'z',
                      Qt::NoModifier);
    QVERIFY(!layout.translate(xkbKeycode(KEY_Y), 0, XKB_KEY_F1).authoritative);
}

void KeyboardLayoutTest::treatsDeadKeysAsAuthoritativeWithoutInventingText()
{
    const TestKeymap keymap("us", "intl");
    QVERIFY(keymap.isValid());

    XkbKeyboardLayout layout;
    QVERIFY(layout.installKeymap(keymap.serialized()));

    verifyTranslation(layout.translate(xkbKeycode(KEY_APOSTROPHE)), 0,
                      Qt::NoModifier);
}

void KeyboardLayoutTest::keepsValidMapAfterRejectedReplacement()
{
    const TestKeymap keymap("de");
    QVERIFY(keymap.isValid());

    XkbKeyboardLayout layout;
    QVERIFY(layout.installKeymap(keymap.serialized()));
    verifyTranslation(layout.translate(xkbKeycode(KEY_Y)), 'z', Qt::NoModifier);

    QVERIFY(!layout.installKeymap(QByteArrayLiteral("not an XKB keymap")));
    verifyTranslation(layout.translate(xkbKeycode(KEY_Y)), 'z', Qt::NoModifier);
}

void KeyboardLayoutTest::rejectsTranslationWithoutUsableNativeIdentity()
{
    XkbKeyboardLayout layout;
    QVERIFY(!layout.translate(xkbKeycode(KEY_A)).authoritative);

    const TestKeymap keymap("us");
    QVERIFY(keymap.isValid());
    QVERIFY(layout.installKeymap(keymap.serialized()));

    QVERIFY(!layout.translate(0).authoritative);
    QVERIFY(
        !layout.translate(std::numeric_limits<quint32>::max()).authoritative);
}

void KeyboardLayoutTest::preservesCapturedTranslationDuringReplay()
{
    QKeyEvent replay(QEvent::KeyPress, Qt::Key_Y, Qt::NoModifier,
                     QStringLiteral("z"));
    QCOMPARE(translateKeyboardLayout(replay).unshiftedCodepoint,
             std::uint32_t{'y'});

    const KeyboardLayoutTranslation captured{
        .unshiftedCodepoint = 'z',
        .consumedModifiers = Qt::GroupSwitchModifier,
        .capsLock = true,
        .numLock = true,
        .consumedCapsLock = true,
        .authoritative = true,
    };
    {
        const ScopedKeyboardLayoutTranslation scope(replay, captured);
        const KeyboardLayoutTranslation translated =
            translateKeyboardLayout(replay);
        QCOMPARE(translated.unshiftedCodepoint, std::uint32_t{'z'});
        QCOMPARE(translated.consumedModifiers, Qt::GroupSwitchModifier);
        QVERIFY(translated.capsLock);
        QVERIFY(translated.numLock);
        QVERIFY(translated.consumedCapsLock);
        QVERIFY(translated.authoritative);
    }

    QCOMPARE(translateKeyboardLayout(replay).unshiftedCodepoint,
             std::uint32_t{'y'});
}

QTEST_GUILESS_MAIN(KeyboardLayoutTest)

#include "test_keyboard_layout.moc"
