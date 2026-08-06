#include "desktop_quick_controls_style.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibraryInfo>

namespace {

constexpr auto QuickControlsStyleEnvironment = "QT_QUICK_CONTROLS_STYLE";
constexpr auto KdeDesktopModulePath = "org/kde/desktop/qmldir";

void appendImportRoot(QStringList &roots, const QString &root)
{
    const QString normalized = QDir::cleanPath(root.trimmed());
    if (!normalized.isEmpty() && !roots.contains(normalized)) {
        roots.append(normalized);
    }
}

void appendImportPathList(QStringList &roots, const QString &pathList)
{
    const QStringList paths =
        pathList.split(QDir::listSeparator(), Qt::SkipEmptyParts);
    for (const QString &path : paths) appendImportRoot(roots, path);
}

bool isKdeDesktopName(QString name)
{
    name = name.trimmed().toLower();
    if (name.endsWith(QStringLiteral(".desktop"))) name.chop(8);

    return name == QStringLiteral("kde") || name == QStringLiteral("plasma")
        || name == QStringLiteral("plasma5")
        || name == QStringLiteral("plasma6")
        || name == QStringLiteral("plasma-x11")
        || name == QStringLiteral("plasmax11")
        || name == QStringLiteral("plasma-wayland")
        || name == QStringLiteral("plasmawayland")
        || name == QStringLiteral("plasma-mobile")
        || name == QStringLiteral("plasmamobile");
}

bool containsKdeDesktopName(const QString &desktopNames)
{
    const QStringList names = desktopNames.split(u':', Qt::SkipEmptyParts);
    for (const QString &name : names) {
        if (isKdeDesktopName(name)) return true;
    }
    return false;
}

bool environmentIdentifiesKde(const QProcessEnvironment &environment)
{
    if (containsKdeDesktopName(
            environment.value(QStringLiteral("XDG_CURRENT_DESKTOP")))) {
        return true;
    }
    if (containsKdeDesktopName(
            environment.value(QStringLiteral("XDG_SESSION_DESKTOP")))) {
        return true;
    }

    const QString session =
        environment.value(QStringLiteral("DESKTOP_SESSION"));
    if (!session.isEmpty()
        && isKdeDesktopName(QFileInfo(session).completeBaseName())) {
        return true;
    }

    const QString fullSession =
        environment.value(QStringLiteral("KDE_FULL_SESSION"))
            .trimmed()
            .toLower();
    return fullSession == QStringLiteral("true")
        || fullSession == QStringLiteral("1")
        || fullSession == QStringLiteral("yes");
}

bool hasKdeDesktopModule(const QStringList &importRoots)
{
    for (const QString &root : importRoots) {
        if (root.trimmed().isEmpty()) continue;
        const QFileInfo qmldir(
            QDir(root).filePath(QString::fromLatin1(KdeDesktopModulePath)));
        if (qmldir.isFile()) return true;
    }
    return false;
}

} // namespace

QStringList
desktopQuickControlsImportRoots(const QProcessEnvironment &environment)
{
    QStringList roots;
    appendImportPathList(roots,
                         environment.value(QStringLiteral("QML2_IMPORT_PATH")));
    appendImportPathList(roots,
                         environment.value(QStringLiteral("QML_IMPORT_PATH")));

    // Match QQmlEngine's built-in roots closely enough that a module it can
    // import is not rejected by startup policy. In particular, paths() keeps
    // every QmlImports entry from an application-local qt.conf; path() would
    // silently retain only the first one.
    appendImportRoot(roots, QCoreApplication::applicationDirPath());
    appendImportRoot(roots, QStringLiteral(":/qt/qml"));
    appendImportRoot(roots, QStringLiteral(":/qt-project.org/imports"));
    for (const QString &path :
         QLibraryInfo::paths(QLibraryInfo::QmlImportsPath)) {
        appendImportRoot(roots, path);
    }
    return roots;
}

bool shouldSelectKdeDesktopQuickControlsStyle(
    const QProcessEnvironment &environment, const QStringList &importRoots)
{
    return !environment.contains(
               QString::fromLatin1(QuickControlsStyleEnvironment))
        && environmentIdentifiesKde(environment)
        && hasKdeDesktopModule(importRoots);
}

bool shouldSelectKdeDesktopQuickControlsStyle(
    const QProcessEnvironment &environment)
{
    return shouldSelectKdeDesktopQuickControlsStyle(
        environment, desktopQuickControlsImportRoots(environment));
}
