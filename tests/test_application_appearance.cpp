#include "app/application_appearance.h"

#include <QTest>

class ApplicationAppearanceTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void normalizesQtScheme();
    void resolvesEveryWindowTheme();
    void usesGhosttyLuminanceBoundary();
    void reportsOnlyEffectiveChanges();
};

void ApplicationAppearanceTest::normalizesQtScheme()
{
    QCOMPARE(ApplicationAppearance::fromQtColorScheme(Qt::ColorScheme::Unknown),
             TerminalColorScheme::Light);
    QCOMPARE(ApplicationAppearance::fromQtColorScheme(Qt::ColorScheme::Light),
             TerminalColorScheme::Light);
    QCOMPARE(ApplicationAppearance::fromQtColorScheme(Qt::ColorScheme::Dark),
             TerminalColorScheme::Dark);
}

void ApplicationAppearanceTest::resolvesEveryWindowTheme()
{
    const QColor black(QStringLiteral("#000000"));
    const QColor white(QStringLiteral("#ffffff"));

    QCOMPARE(ApplicationAppearance::resolveColorScheme(
                 WindowTheme::System, black, TerminalColorScheme::Light),
             TerminalColorScheme::Light);
    QCOMPARE(ApplicationAppearance::resolveColorScheme(
                 WindowTheme::System, white, TerminalColorScheme::Dark),
             TerminalColorScheme::Dark);
    QCOMPARE(ApplicationAppearance::resolveColorScheme(
                 WindowTheme::Light, black, TerminalColorScheme::Dark),
             TerminalColorScheme::Light);
    QCOMPARE(ApplicationAppearance::resolveColorScheme(
                 WindowTheme::Dark, white, TerminalColorScheme::Light),
             TerminalColorScheme::Dark);
    QCOMPARE(ApplicationAppearance::resolveColorScheme(
                 WindowTheme::Auto, black, TerminalColorScheme::Light),
             TerminalColorScheme::Dark);
    QCOMPARE(ApplicationAppearance::resolveColorScheme(
                 WindowTheme::Auto, white, TerminalColorScheme::Dark),
             TerminalColorScheme::Light);
    QCOMPARE(ApplicationAppearance::resolveColorScheme(
                 WindowTheme::Ghostty, black, TerminalColorScheme::Light),
             TerminalColorScheme::Dark);
    QCOMPARE(ApplicationAppearance::resolveColorScheme(
                 WindowTheme::Ghostty, white, TerminalColorScheme::Dark),
             TerminalColorScheme::Light);
}

void ApplicationAppearanceTest::usesGhosttyLuminanceBoundary()
{
    QCOMPARE(ApplicationAppearance::resolveColorScheme(
                 WindowTheme::Auto, QColor(QStringLiteral("#7f7f7f")),
                 TerminalColorScheme::Light),
             TerminalColorScheme::Dark);
    QCOMPARE(ApplicationAppearance::resolveColorScheme(
                 WindowTheme::Auto, QColor(QStringLiteral("#808080")),
                 TerminalColorScheme::Dark),
             TerminalColorScheme::Light);
    QCOMPARE(ApplicationAppearance::resolveColorScheme(
                 WindowTheme::Ghostty, QColor{}, TerminalColorScheme::Light),
             TerminalColorScheme::Dark);
}

void ApplicationAppearanceTest::reportsOnlyEffectiveChanges()
{
    ApplicationAppearance appearance(TerminalColorScheme::Light);
    WindowAppearanceOptions options;
    options.theme = WindowTheme::System;

    QVERIFY(!appearance.apply(options, QColor(QStringLiteral("#101010"))));
    QVERIFY(!appearance.setSystemColorScheme(TerminalColorScheme::Light));
    QVERIFY(appearance.setSystemColorScheme(TerminalColorScheme::Dark));
    QCOMPARE(appearance.colorScheme(), TerminalColorScheme::Dark);

    options.theme = WindowTheme::Light;
    QVERIFY(appearance.apply(options, QColor(QStringLiteral("#101010"))));
    QCOMPARE(appearance.colorScheme(), TerminalColorScheme::Light);
    QVERIFY(!appearance.setSystemColorScheme(TerminalColorScheme::Light));
    QVERIFY(!appearance.setSystemColorScheme(TerminalColorScheme::Dark));
    QCOMPARE(appearance.colorScheme(), TerminalColorScheme::Light);

    options.theme = WindowTheme::Auto;
    QVERIFY(appearance.apply(options, QColor(QStringLiteral("#101010"))));
    QCOMPARE(appearance.colorScheme(), TerminalColorScheme::Dark);
    QVERIFY(!appearance.apply(options, QColor(QStringLiteral("#202020"))));
    QVERIFY(appearance.apply(options, QColor(QStringLiteral("#f0f0f0"))));
    QCOMPARE(appearance.colorScheme(), TerminalColorScheme::Light);
    QVERIFY(!appearance.apply(options, QColor(QStringLiteral("#f0f0f0"))));
}

QTEST_GUILESS_MAIN(ApplicationAppearanceTest)

#include "test_application_appearance.moc"
