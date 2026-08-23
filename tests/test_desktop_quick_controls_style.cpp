#include "desktop/desktop_quick_controls_style.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLibraryInfo>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

namespace {

bool createKdeDesktopModule(const QString &root)
{
    const QString module =
        QDir(root).filePath(QStringLiteral("org/kde/desktop"));
    if (!QDir().mkpath(module)) return false;

    QFile qmldir(QDir(module).filePath(QStringLiteral("qmldir")));
    if (!qmldir.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return qmldir.write("module org.kde.desktop\n") > 0;
}

} // namespace

class DesktopQuickControlsStyleTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void recognizesKdeDesktopIdentities_data();
    void recognizesKdeDesktopIdentities();
    void rejectsNonKdeDesktops_data();
    void rejectsNonKdeDesktops();
    void respectsExplicitStyleOverride_data();
    void respectsExplicitStyleOverride();
    void requiresInstalledStyleModule();
    void searchesEveryProvidedImportRoot();
    void derivesDefaultRootsFromInjectedEnvironment();
    void includesQtDefaultImportRoots();
};

void DesktopQuickControlsStyleTest::recognizesKdeDesktopIdentities_data()
{
    QTest::addColumn<QString>("variable");
    QTest::addColumn<QString>("value");

    QTest::newRow("xdg-kde")
        << QStringLiteral("XDG_CURRENT_DESKTOP") << QStringLiteral("KDE");
    QTest::newRow("xdg-colon-list-kde")
        << QStringLiteral("XDG_CURRENT_DESKTOP") << QStringLiteral("Unity:KDE");
    QTest::newRow("xdg-colon-list-plasma")
        << QStringLiteral("XDG_CURRENT_DESKTOP")
        << QStringLiteral("vendor:Plasma");
    QTest::newRow("session-desktop")
        << QStringLiteral("XDG_SESSION_DESKTOP") << QStringLiteral("plasma6");
    QTest::newRow("plasma-wayland-session")
        << QStringLiteral("DESKTOP_SESSION") << QStringLiteral("plasmawayland");
    QTest::newRow("plasma-x11-session")
        << QStringLiteral("DESKTOP_SESSION") << QStringLiteral("plasmax11");
    QTest::newRow("session-file-path")
        << QStringLiteral("DESKTOP_SESSION")
        << QStringLiteral("/usr/share/wayland-sessions/plasma.desktop");
    QTest::newRow("kde-full-session")
        << QStringLiteral("KDE_FULL_SESSION") << QStringLiteral("true");
}

void DesktopQuickControlsStyleTest::recognizesKdeDesktopIdentities()
{
    QFETCH(QString, variable);
    QFETCH(QString, value);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(createKdeDesktopModule(root.path()));

    QProcessEnvironment environment;
    environment.insert(variable, value);
    QVERIFY(
        shouldSelectKdeDesktopQuickControlsStyle(environment, {root.path()}));
}

void DesktopQuickControlsStyleTest::rejectsNonKdeDesktops_data()
{
    QTest::addColumn<QString>("variable");
    QTest::addColumn<QString>("value");

    QTest::newRow("gnome") << QStringLiteral("XDG_CURRENT_DESKTOP")
                           << QStringLiteral("GNOME");
    QTest::newRow("colon-list") << QStringLiteral("XDG_CURRENT_DESKTOP")
                                << QStringLiteral("ubuntu:GNOME");
    QTest::newRow("unrelated-session")
        << QStringLiteral("DESKTOP_SESSION") << QStringLiteral("xfce");
    QTest::newRow("similar-prefix")
        << QStringLiteral("DESKTOP_SESSION") << QStringLiteral("plasmatic");
    QTest::newRow("disabled-kde-session")
        << QStringLiteral("KDE_FULL_SESSION") << QStringLiteral("false");
}

void DesktopQuickControlsStyleTest::rejectsNonKdeDesktops()
{
    QFETCH(QString, variable);
    QFETCH(QString, value);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(createKdeDesktopModule(root.path()));

    QProcessEnvironment environment;
    environment.insert(variable, value);
    QVERIFY(
        !shouldSelectKdeDesktopQuickControlsStyle(environment, {root.path()}));
}

void DesktopQuickControlsStyleTest::respectsExplicitStyleOverride_data()
{
    QTest::addColumn<QString>("style");
    QTest::newRow("named-style") << QStringLiteral("Fusion");
    QTest::newRow("explicit-kde-style") << QStringLiteral("org.kde.desktop");
    QTest::newRow("explicit-empty-style") << QString();
}

void DesktopQuickControlsStyleTest::respectsExplicitStyleOverride()
{
    QFETCH(QString, style);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(createKdeDesktopModule(root.path()));

    QProcessEnvironment environment;
    environment.insert(QStringLiteral("XDG_CURRENT_DESKTOP"),
                       QStringLiteral("KDE"));
    environment.insert(QStringLiteral("QT_QUICK_CONTROLS_STYLE"), style);
    QVERIFY(
        !shouldSelectKdeDesktopQuickControlsStyle(environment, {root.path()}));
}

void DesktopQuickControlsStyleTest::requiresInstalledStyleModule()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    QProcessEnvironment environment;
    environment.insert(QStringLiteral("XDG_CURRENT_DESKTOP"),
                       QStringLiteral("KDE"));
    QVERIFY(
        !shouldSelectKdeDesktopQuickControlsStyle(environment, {root.path()}));
}

void DesktopQuickControlsStyleTest::searchesEveryProvidedImportRoot()
{
    QTemporaryDir firstRoot;
    QTemporaryDir secondRoot;
    QVERIFY(firstRoot.isValid());
    QVERIFY(secondRoot.isValid());
    QVERIFY(createKdeDesktopModule(secondRoot.path()));

    QProcessEnvironment environment;
    environment.insert(QStringLiteral("XDG_CURRENT_DESKTOP"),
                       QStringLiteral("KDE"));
    QVERIFY(shouldSelectKdeDesktopQuickControlsStyle(
        environment, {firstRoot.path(), secondRoot.path()}));
}

void DesktopQuickControlsStyleTest::derivesDefaultRootsFromInjectedEnvironment()
{
    QTemporaryDir firstRoot;
    QTemporaryDir secondRoot;
    QTemporaryDir legacyRoot;
    QVERIFY(firstRoot.isValid());
    QVERIFY(secondRoot.isValid());
    QVERIFY(legacyRoot.isValid());
    QVERIFY(createKdeDesktopModule(legacyRoot.path()));

    QProcessEnvironment environment;
    environment.insert(QStringLiteral("XDG_CURRENT_DESKTOP"),
                       QStringLiteral("KDE"));
    environment.insert(QStringLiteral("QML2_IMPORT_PATH"),
                       firstRoot.path() + QDir::listSeparator()
                           + secondRoot.path());
    environment.insert(QStringLiteral("QML_IMPORT_PATH"), legacyRoot.path());

    const QStringList roots = desktopQuickControlsImportRoots(environment);
    QCOMPARE(roots.first(), QDir::cleanPath(firstRoot.path()));
    QCOMPARE(roots.at(1), QDir::cleanPath(secondRoot.path()));
    QCOMPARE(roots.at(2), QDir::cleanPath(legacyRoot.path()));
    QVERIFY(shouldSelectKdeDesktopQuickControlsStyle(environment));
}

void DesktopQuickControlsStyleTest::includesQtDefaultImportRoots()
{
    const QStringList roots =
        desktopQuickControlsImportRoots(QProcessEnvironment{});

    QVERIFY(roots.contains(
        QDir::cleanPath(QCoreApplication::applicationDirPath())));
    QVERIFY(roots.contains(QStringLiteral(":/qt/qml")));
    QVERIFY(roots.contains(QStringLiteral(":/qt-project.org/imports")));
    for (const QString &path :
         QLibraryInfo::paths(QLibraryInfo::QmlImportsPath)) {
        QVERIFY(roots.contains(QDir::cleanPath(path)));
    }
}

QTEST_GUILESS_MAIN(DesktopQuickControlsStyleTest)

#include "test_desktop_quick_controls_style.moc"
