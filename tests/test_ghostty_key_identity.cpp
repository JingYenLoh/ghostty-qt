#include "input/ghostty_key_identity.h"

#include <QTest>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

constexpr quint32 xkbKeycode(unsigned int evdevCode)
{
    return static_cast<quint32>(evdevCode + 8U);
}

} // namespace

class GhosttyKeyIdentityTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void mirrorsPinnedRemappableWritingBoundary();
    void selectsEitherFunctionalRemapEndpoint();
    void retainsWritingAndUnknownKeysymIdentity();
    void preservesSidedKeypadAndExtendedFunctionIdentity();
};

void GhosttyKeyIdentityTest::mirrorsPinnedRemappableWritingBoundary()
{
    QVERIFY(!ghosttyKeyShouldBeRemappable(GHOSTTY_KEY_BACKQUOTE));
    QVERIFY(!ghosttyKeyShouldBeRemappable(GHOSTTY_KEY_DIGIT_0));
    QVERIFY(!ghosttyKeyShouldBeRemappable(GHOSTTY_KEY_A));
    QVERIFY(!ghosttyKeyShouldBeRemappable(GHOSTTY_KEY_Z));
    QVERIFY(!ghosttyKeyShouldBeRemappable(GHOSTTY_KEY_SLASH));

    QVERIFY(ghosttyKeyShouldBeRemappable(GHOSTTY_KEY_UNIDENTIFIED));
    QVERIFY(ghosttyKeyShouldBeRemappable(GHOSTTY_KEY_SPACE));
    QVERIFY(ghosttyKeyShouldBeRemappable(GHOSTTY_KEY_CAPS_LOCK));
    QVERIFY(ghosttyKeyShouldBeRemappable(GHOSTTY_KEY_ESCAPE));
    QVERIFY(ghosttyKeyShouldBeRemappable(GHOSTTY_KEY_CONTROL_RIGHT));
    QVERIFY(ghosttyKeyShouldBeRemappable(GHOSTTY_KEY_NUMPAD_1));
}

void GhosttyKeyIdentityTest::selectsEitherFunctionalRemapEndpoint()
{
    QCOMPARE(ghosttyEffectiveKey(xkbKeycode(KEY_CAPSLOCK), XKB_KEY_Escape,
                                 Qt::Key_Escape),
             GHOSTTY_KEY_ESCAPE);
    QCOMPARE(ghosttyEffectiveKey(xkbKeycode(KEY_ESC), XKB_KEY_Caps_Lock,
                                 Qt::Key_CapsLock),
             GHOSTTY_KEY_CAPS_LOCK);
    QCOMPARE(
        ghosttyEffectiveKey(xkbKeycode(KEY_A), XKB_KEY_Escape, Qt::Key_Escape),
        GHOSTTY_KEY_ESCAPE);
    QCOMPARE(ghosttyEffectiveKey(xkbKeycode(KEY_ESC), XKB_KEY_a, Qt::Key_A),
             GHOSTTY_KEY_A);
}

void GhosttyKeyIdentityTest::retainsWritingAndUnknownKeysymIdentity()
{
    QCOMPARE(ghosttyEffectiveKey(xkbKeycode(KEY_Y), XKB_KEY_z, Qt::Key_Z),
             GHOSTTY_KEY_Y);
    QCOMPARE(ghosttyEffectiveKey(xkbKeycode(KEY_ESC), XKB_KEY_Cyrillic_tse,
                                 Qt::Key_unknown),
             GHOSTTY_KEY_ESCAPE);
    QCOMPARE(ghosttyEffectiveKey(xkbKeycode(KEY_HELP), 0, Qt::Key_Help),
             GHOSTTY_KEY_UNIDENTIFIED);
    QCOMPARE(ghosttyEffectiveKey(0, 0, Qt::Key_Help), GHOSTTY_KEY_HELP);

    // Pinned GTK maps only lower-case writing keysyms for remap selection.
    QCOMPARE(ghosttyKeyFromXkbKeysym(XKB_KEY_A), GHOSTTY_KEY_UNIDENTIFIED);
}

void GhosttyKeyIdentityTest::preservesSidedKeypadAndExtendedFunctionIdentity()
{
    QCOMPARE(ghosttyEffectiveKey(xkbKeycode(KEY_CAPSLOCK), XKB_KEY_Control_R,
                                 Qt::Key_Control),
             GHOSTTY_KEY_CONTROL_RIGHT);
    QCOMPARE(ghosttyEffectiveKey(xkbKeycode(KEY_KP1), XKB_KEY_KP_End,
                                 Qt::Key_End, Qt::KeypadModifier),
             GHOSTTY_KEY_NUMPAD_END);
    QCOMPARE(ghosttyEffectiveKey(0, 0, Qt::Key_Clear, Qt::KeypadModifier),
             GHOSTTY_KEY_NUMPAD_BEGIN);
    QCOMPARE(ghosttyEffectiveKey(xkbKeycode(KEY_F1), XKB_KEY_F25, Qt::Key_F25),
             GHOSTTY_KEY_F25);
}

QTEST_GUILESS_MAIN(GhosttyKeyIdentityTest)

#include "test_ghostty_key_identity.moc"
