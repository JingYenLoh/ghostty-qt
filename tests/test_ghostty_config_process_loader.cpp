#include "ghostty_config_process_loader.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <limits>

#ifndef GHOSTTY_QT_FAKE_CONFIG_HELPER_PATH
#define GHOSTTY_QT_FAKE_CONFIG_HELPER_PATH ""
#endif

#ifndef GHOSTTY_QT_REAL_CONFIG_HELPER_PATH
#define GHOSTTY_QT_REAL_CONFIG_HELPER_PATH ""
#endif

namespace {

QByteArray defaultOutput()
{
    QByteArray output =
        QByteArrayLiteral("font-family = \n"
                          "font-size = 13\n"
                          "foreground = #ffffff\n"
                          "background = #282c34\n"
                          "selection-foreground = \n"
                          "selection-background = \n"
                          "search-foreground = #000000\n"
                          "search-background = #ffe082\n"
                          "search-selected-foreground = #000000\n"
                          "search-selected-background = #f2a57e\n"
                          "cursor-color = \n"
                          "cursor-opacity = 1\n"
                          "cursor-style = block\n"
                          "cursor-style-blink = \n"
                          "cursor-text = \n"
                          "bold-color = \n"
                          "faint-opacity = 0.5\n"
                          "scrollback-limit = 50000000\n"
                          "confirm-close-surface = true\n"
                          "clipboard-trim-trailing-spaces = true\n"
                          "clipboard-paste-protection = true\n"
                          "clipboard-paste-bracketed-safe = true\n"
                          "copy-on-select = true\n"
                          "selection-clear-on-typing = true\n"
                          "selection-clear-on-copy = false\n"
                          "middle-click-action = primary-paste\n"
                          "link-url = true\n"
                          "link-previews = true\n"
                          "keybind = ctrl+shift+t=new_tab\n"
                          "config-file = \n");
    for (int index = 0; index < 256; ++index) {
        output.append(QStringLiteral("palette = %1=#%2%2%2\n")
                          .arg(index)
                          .arg(index, 2, 16, QLatin1Char('0'))
                          .toLatin1());
    }
    return output;
}

struct ConfigFixture {
    QTemporaryDir temporary;
    QString xdgHome;
    QString legacyPath;
    QString preferredPath;

    ConfigFixture()
    {
        xdgHome = QDir(temporary.path()).filePath(QStringLiteral("xdg"));
        const QString ghosttyDirectory =
            QDir(xdgHome).filePath(QStringLiteral("ghostty"));
        QDir().mkpath(ghosttyDirectory);
        legacyPath = QDir(ghosttyDirectory).filePath(QStringLiteral("config"));
        preferredPath =
            QDir(ghosttyDirectory).filePath(QStringLiteral("config.ghostty"));
        writeFile(legacyPath, QByteArrayLiteral("font-size = 14\n"));
        writeFile(preferredPath, QByteArrayLiteral("font-size = 15\n"));
    }

    QStringList candidates() const { return {legacyPath, preferredPath}; }

    static void writeFile(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qFatal("could not create test fixture file");
        }
        file.write(contents);
    }
};

GhosttyConfigProcessLoaderOptions fakeOptions(const ConfigFixture &fixture,
                                               const QString &mode = {})
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_EXPECT_XDG_CONFIG_HOME"),
                       fixture.xdgHome);
    environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_DEFAULT_OUTPUT"),
                       QString::fromLatin1(defaultOutput()));
    environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_CHANGES_OUTPUT"),
                       QStringLiteral("font-size = 17.25\n"));
    environment.insert(
        QStringLiteral("GHOSTTY_QT_FAKE_KEYBIND_OUTPUT"),
        QStringLiteral(
            R"json({"version":1,"root":[{"sequence":[{"kind":"unicode","codepoint":116,"mods":3}],"actions":["new_tab"],"flags":{"consumed":true,"all":false,"global":false,"performable":false}}],"tables":[]})json"));
    if (!mode.isEmpty()) {
        environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_MODE"), mode);
    }
    return {
        .helperPath = QString::fromUtf8(GHOSTTY_QT_FAKE_CONFIG_HELPER_PATH),
        .timeoutMilliseconds = 2'000,
        .environment = environment,
    };
}

} // namespace

class GhosttyConfigProcessLoaderTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void derivesXdgHomeFromEitherCandidateOrder();
    void parsesStructuredKeybindJsonTransactionally();
    void rejectsMalformedStructuredKeybindJson();
    void mergesCanonicalOutputsIntoTypedSnapshot();
    void preservesDefaultAndAcceptsEveryLinkPreviewMode();
    void preservesDefaultsAndAcceptsEveryClipboardMode();
    void emptyRepeatableChangesResetDefaults();
    void rejectsMalformedCanonicalValues();
    void invokesValidationThenDefaultAndCurrentQueries();
    void rejectsFailedOrMalformedStructuredQuery();
    void rejectsConfigThatBecomesInvalidDuringQueries();
    void rejectsConfigThatChangesValidlyDuringQueries();
    void preservesSuccessfulHelperWarnings();
    void realHelperPreservesAppearanceAndEffectiveUnbindSemantics();
    void realHelperExportsFinalizedStructuredKeybindings();
    void realHelperCanonicalizesTerminalControlActionPayloads();
    void reportsValidationFailureDeterministically();
    void reportsTimeoutCrashAndStartFailureDeterministically();
};

void GhosttyConfigProcessLoaderTest::parsesStructuredKeybindJsonTransactionally()
{
    const QByteArray json = QByteArrayLiteral(R"json({
        "version": 1,
        "root": [{
            "sequence": [
                {"kind":"physical","key":"key_a","mods":3},
                {"kind":"unicode","codepoint":128578,"mods":4},
                {"kind":"catch_all","mods":0}
            ],
            "actions": ["new_tab", "goto_tab:2"],
            "flags": {
                "consumed": false,
                "all": true,
                "global": false,
                "performable": true
            }
        }],
        "tables": [{
            "name":"resize",
            "bindings":[{
                "sequence":[{"kind":"unicode","codepoint":104,"mods":2}],
                "actions":["resize_split:left,10"],
                "flags": {
                    "consumed":true,
                    "all":false,
                    "global":false,
                    "performable":false
                }
            }]
        }]
    })json");

    GhosttyKeybindConfig config;
    QString error;
    QVERIFY2(parseGhosttyKeybindConfigJson(json, &config, &error),
             qPrintable(error));
    QVERIFY(error.isEmpty());
    QCOMPARE(config.schemaVersion, 1);
    QCOMPARE(config.root.size(), 1);
    QCOMPARE(config.root.constFirst().sequence.size(), 3);
    QCOMPARE(config.root.constFirst().sequence.at(0).kind,
             GhosttyKeybindKeyKind::Physical);
    QCOMPARE(config.root.constFirst().sequence.at(0).physicalName,
             QStringLiteral("key_a"));
    QCOMPARE(config.root.constFirst().sequence.at(0).modifiers, quint8(3));
    QCOMPARE(config.root.constFirst().sequence.at(1).kind,
             GhosttyKeybindKeyKind::Unicode);
    QCOMPARE(config.root.constFirst().sequence.at(1).unicodeCodepoint,
             quint32(128578));
    QCOMPARE(config.root.constFirst().sequence.at(2).kind,
             GhosttyKeybindKeyKind::CatchAll);
    QCOMPARE(config.root.constFirst().actions,
             QStringList({QStringLiteral("new_tab"),
                          QStringLiteral("goto_tab:2")}));
    QVERIFY(!config.root.constFirst().flags.consumed);
    QVERIFY(config.root.constFirst().flags.all);
    QVERIFY(config.root.constFirst().flags.performable);
    QCOMPARE(config.tables.size(), 1);
    QCOMPARE(config.tables.constFirst().name, QStringLiteral("resize"));
    QCOMPARE(config.tables.constFirst().bindings.size(), 1);
}

void GhosttyConfigProcessLoaderTest::rejectsMalformedStructuredKeybindJson()
{
    GhosttyKeybindConfig config;
    config.schemaVersion = 77;
    QString error;
    QVERIFY(!parseGhosttyKeybindConfigJson(
        QByteArrayLiteral(R"json({
            "version":1,
            "root":[{
                "sequence":[{"kind":"unicode","codepoint":55296,"mods":0}],
                "actions":["ignore"],
                "flags":{"consumed":true,"all":false,"global":false,"performable":false}
            }],
            "tables":[]
        })json"),
        &config, &error));
    QVERIFY(error.contains(QStringLiteral("Unicode scalar")));
    QCOMPARE(config.schemaVersion, 77);

    QVERIFY(!parseGhosttyKeybindConfigJson(
        QByteArrayLiteral("{\"version\":2,\"root\":[],\"tables\":[]}"),
        &config, &error));
    QVERIFY(error.contains(QStringLiteral("schema version")));
    QCOMPARE(config.schemaVersion, 77);
}

void GhosttyConfigProcessLoaderTest::derivesXdgHomeFromEitherCandidateOrder()
{
    ConfigFixture fixture;
    QString error;
    QCOMPARE(ghosttyConfigXdgHome(fixture.candidates(), &error), fixture.xdgHome);
    QVERIFY(error.isEmpty());

    QCOMPARE(ghosttyConfigXdgHome(
                 {fixture.preferredPath, fixture.legacyPath}, &error),
             fixture.xdgHome);
    QVERIFY(error.isEmpty());

    QVERIFY(ghosttyConfigXdgHome({fixture.legacyPath}, &error).isEmpty());
    QCOMPARE(error,
             QStringLiteral("Ghostty config candidates must contain both config and config.ghostty"));
}

void GhosttyConfigProcessLoaderTest::mergesCanonicalOutputsIntoTypedSnapshot()
{
    ConfigFixture fixture;
    const QString includePath =
        QDir(fixture.temporary.path()).filePath(QStringLiteral("included.conf"));
    ConfigFixture::writeFile(includePath,
                             QByteArrayLiteral("foreground = #102030\n"));
    const QString missingOptional =
        QDir(fixture.temporary.path()).filePath(QStringLiteral("optional.conf"));

    const QByteArray changes = QString(
        QStringLiteral(
            "# canonical changed values\r\n"
            "font-family = JetBrains Mono\r\n"
            "font-family = Noto Color Emoji\r\n"
            "font-size = 15.5\r\n"
            "foreground = #102030\r\n"
            "palette = 1=#123456\r\n"
            "palette = 255=#fedcba\r\n"
            "selection-foreground = cell-background\r\n"
            "selection-background = #334455\r\n"
            "search-foreground = cell-background\r\n"
            "search-background = #123456\r\n"
            "search-selected-foreground = cell-foreground\r\n"
            "search-selected-background = #654321\r\n"
            "cursor-color = cell-background\r\n"
            "cursor-opacity = 0.375\r\n"
            "cursor-style = block_hollow\r\n"
            "cursor-style-blink = false\r\n"
            "cursor-text = cell-foreground\r\n"
            "bold-color = bright\r\n"
            "faint-opacity = 0.25\r\n"
            "scrollback-limit = 123456\r\n"
            "confirm-close-surface = always\r\n"
            "clipboard-trim-trailing-spaces = false\r\n"
            "clipboard-paste-protection = false\r\n"
            "clipboard-paste-bracketed-safe = true\r\n"
            "copy-on-select = clipboard\r\n"
            "selection-clear-on-typing = false\r\n"
            "selection-clear-on-copy = true\r\n"
            "middle-click-action = ignore\r\n"
            "link-url = false\r\n"
            "link-previews = osc8\r\n"
            "keybind = alt+n=new_tab\r\n"
            "keybind = chain=next_tab\r\n"
            "keybind = ctrl+x>ctrl+y=new_tab\r\n"
            "keybind = ctrl+f=toggle_fullscreen\r\n"
            "keybind = ctrl+u=open_config\r\n"
            "config-file = %1\r\n"
            "config-file = ?%2\r\n"))
                                   .arg(includePath, missingOptional)
                                   .toUtf8();

    const GhosttyConfigLoadResult result =
        parseGhosttyConfigShowOutputs(defaultOutput(), changes,
                                      fixture.candidates());
    QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
    const GhosttyConfigSnapshot &snapshot = *result.snapshot;
    QCOMPARE(snapshot.availability, GhosttyConfigAvailability::Available);
    QCOMPARE(snapshot.values.value(QStringLiteral("font-family")).toStringList(),
             QStringList({QStringLiteral("JetBrains Mono"),
                          QStringLiteral("Noto Color Emoji")}));
    QCOMPARE(snapshot.values.value(QStringLiteral("font-size")).toDouble(), 15.5);
    QCOMPARE(snapshot.values.value(QStringLiteral("foreground")).value<QColor>(),
             QColor(QStringLiteral("#102030")));
    QCOMPARE(snapshot.values.value(QStringLiteral("background")).value<QColor>(),
             QColor(QStringLiteral("#282c34")));
    const QVariantList palette =
        snapshot.values.value(QStringLiteral("palette")).toList();
    QCOMPARE(palette.size(), 256);
    QCOMPARE(palette.at(0).value<QColor>(), QColor(QStringLiteral("#000000")));
    QCOMPARE(palette.at(1).value<QColor>(), QColor(QStringLiteral("#123456")));
    QCOMPARE(palette.at(2).value<QColor>(), QColor(QStringLiteral("#020202")));
    QCOMPARE(palette.at(255).value<QColor>(), QColor(QStringLiteral("#fedcba")));
    QCOMPARE(snapshot.values.value(QStringLiteral("selection-foreground"))
                 .toString(),
             QStringLiteral("cell-background"));
    QCOMPARE(snapshot.values.value(QStringLiteral("selection-background"))
                 .value<QColor>(),
             QColor(QStringLiteral("#334455")));
    QCOMPARE(snapshot.values.value(QStringLiteral("search-foreground"))
                 .toString(),
             QStringLiteral("cell-background"));
    QCOMPARE(snapshot.values.value(QStringLiteral("search-background"))
                 .value<QColor>(),
             QColor(QStringLiteral("#123456")));
    QCOMPARE(snapshot.values.value(
                 QStringLiteral("search-selected-foreground")).toString(),
             QStringLiteral("cell-foreground"));
    QCOMPARE(snapshot.values.value(
                 QStringLiteral("search-selected-background")).value<QColor>(),
             QColor(QStringLiteral("#654321")));
    QCOMPARE(snapshot.values.value(QStringLiteral("cursor-color")).toString(),
             QStringLiteral("cell-background"));
    QCOMPARE(snapshot.values.value(QStringLiteral("cursor-opacity")).toDouble(),
             0.375);
    QCOMPARE(snapshot.values.value(QStringLiteral("cursor-style")).toString(),
             QStringLiteral("block_hollow"));
    QCOMPARE(snapshot.values.value(QStringLiteral("cursor-style-blink")).toBool(),
             false);
    QCOMPARE(snapshot.values.value(QStringLiteral("cursor-text")).toString(),
             QStringLiteral("cell-foreground"));
    QCOMPARE(snapshot.values.value(QStringLiteral("bold-color")).toString(),
             QStringLiteral("bright"));
    QCOMPARE(snapshot.values.value(QStringLiteral("faint-opacity")).toDouble(),
             0.25);
    QCOMPARE(snapshot.values.value(QStringLiteral("scrollback-limit")).toULongLong(),
             quint64(123456));
    QCOMPARE(snapshot.values.value(QStringLiteral("confirm-close-surface")).toString(),
             QStringLiteral("always"));
    QCOMPARE(snapshot.values.value(
                 QStringLiteral("clipboard-trim-trailing-spaces")).toBool(),
             false);
    QVERIFY(!snapshot.values.value(
        QStringLiteral("clipboard-paste-protection")).toBool());
    QVERIFY(snapshot.values.value(
        QStringLiteral("clipboard-paste-bracketed-safe")).toBool());
    QCOMPARE(snapshot.values.value(QStringLiteral("copy-on-select")).toString(),
             QStringLiteral("clipboard"));
    QCOMPARE(snapshot.values.value(
                 QStringLiteral("selection-clear-on-typing")).toBool(),
             false);
    QCOMPARE(snapshot.values.value(
                 QStringLiteral("selection-clear-on-copy")).toBool(),
             true);
    QCOMPARE(snapshot.values.value(QStringLiteral("middle-click-action")).toString(),
             QStringLiteral("ignore"));
    QCOMPARE(snapshot.values.value(QStringLiteral("link-url")).toBool(), false);
    QCOMPARE(snapshot.values.value(QStringLiteral("link-previews")).toString(),
             QStringLiteral("osc8"));
    QCOMPARE(snapshot.values.value(QStringLiteral("keybind")).toStringList(),
             QStringList({QStringLiteral("alt+n=new_tab"),
                          QStringLiteral("chain=next_tab"),
                          QStringLiteral("ctrl+x>ctrl+y=new_tab"),
                          QStringLiteral("ctrl+f=toggle_fullscreen"),
                          QStringLiteral("ctrl+u=open_config")}));
    QCOMPARE(snapshot.diagnostics.size(), 1);
    QVERIFY(std::any_of(
        snapshot.diagnostics.cbegin(), snapshot.diagnostics.cend(),
        [](const GhosttyConfigDiagnostic &diagnostic) {
            return diagnostic.message.contains(
                QStringLiteral("open_config"));
        }));
    QCOMPARE(snapshot.values.value(QStringLiteral("config-file")).toStringList(),
             QStringList({includePath, QStringLiteral("?") + missingOptional}));
    QCOMPARE(snapshot.sourcePaths,
             QStringList({fixture.legacyPath, fixture.preferredPath, includePath}));
}

void GhosttyConfigProcessLoaderTest::preservesDefaultAndAcceptsEveryLinkPreviewMode()
{
    ConfigFixture fixture;
    const GhosttyConfigLoadResult unchanged = parseGhosttyConfigShowOutputs(
        defaultOutput(), {}, fixture.candidates());
    QVERIFY2(unchanged.succeeded(), qPrintable(unchanged.errorMessage));
    QCOMPARE(unchanged.snapshot->values
                 .value(QStringLiteral("link-previews")).toString(),
             QStringLiteral("true"));
    QCOMPARE(unchanged.snapshot->values
                 .value(QStringLiteral("search-foreground")).value<QColor>(),
             QColor(QStringLiteral("#000000")));
    QCOMPARE(unchanged.snapshot->values
                 .value(QStringLiteral("search-background")).value<QColor>(),
             QColor(QStringLiteral("#ffe082")));
    QCOMPARE(unchanged.snapshot->values.value(
                 QStringLiteral("search-selected-foreground")).value<QColor>(),
             QColor(QStringLiteral("#000000")));
    QCOMPARE(unchanged.snapshot->values.value(
                 QStringLiteral("search-selected-background")).value<QColor>(),
             QColor(QStringLiteral("#f2a57e")));

    for (const QByteArray &mode : {QByteArrayLiteral("false"),
                                   QByteArrayLiteral("true"),
                                   QByteArrayLiteral("osc8")}) {
        const GhosttyConfigLoadResult changed = parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("link-previews = ") + mode + '\n',
            fixture.candidates());
        QVERIFY2(changed.succeeded(), qPrintable(changed.errorMessage));
        QCOMPARE(changed.snapshot->values
                     .value(QStringLiteral("link-previews")).toString(),
                 QString::fromLatin1(mode));
    }
}

void GhosttyConfigProcessLoaderTest::preservesDefaultsAndAcceptsEveryClipboardMode()
{
    ConfigFixture fixture;
    const GhosttyConfigLoadResult unchanged = parseGhosttyConfigShowOutputs(
        defaultOutput(), {}, fixture.candidates());
    QVERIFY2(unchanged.succeeded(), qPrintable(unchanged.errorMessage));
    const QVariantMap &defaults = unchanged.snapshot->values;
    QVERIFY(defaults.value(
        QStringLiteral("clipboard-trim-trailing-spaces")).toBool());
    QVERIFY(defaults.value(
        QStringLiteral("clipboard-paste-protection")).toBool());
    QVERIFY(defaults.value(
        QStringLiteral("clipboard-paste-bracketed-safe")).toBool());
    QCOMPARE(defaults.value(QStringLiteral("copy-on-select")).toString(),
             QStringLiteral("true"));
    QVERIFY(defaults.value(
        QStringLiteral("selection-clear-on-typing")).toBool());
    QVERIFY(!defaults.value(
        QStringLiteral("selection-clear-on-copy")).toBool());
    QCOMPARE(defaults.value(QStringLiteral("middle-click-action")).toString(),
             QStringLiteral("primary-paste"));

    for (const QByteArray &mode : {QByteArrayLiteral("false"),
                                   QByteArrayLiteral("true"),
                                   QByteArrayLiteral("clipboard")}) {
        const GhosttyConfigLoadResult changed = parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("copy-on-select = ") + mode + '\n',
            fixture.candidates());
        QVERIFY2(changed.succeeded(), qPrintable(changed.errorMessage));
        QCOMPARE(changed.snapshot->values
                     .value(QStringLiteral("copy-on-select")).toString(),
                 QString::fromLatin1(mode));
    }

    for (const QByteArray &action : {QByteArrayLiteral("primary-paste"),
                                     QByteArrayLiteral("ignore")}) {
        const GhosttyConfigLoadResult changed = parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("middle-click-action = ")
                + action + '\n', fixture.candidates());
        QVERIFY2(changed.succeeded(), qPrintable(changed.errorMessage));
        QCOMPARE(changed.snapshot->values
                     .value(QStringLiteral("middle-click-action")).toString(),
                 QString::fromLatin1(action));
    }
}

void GhosttyConfigProcessLoaderTest::emptyRepeatableChangesResetDefaults()
{
    ConfigFixture fixture;
    const QByteArray defaults =
        QByteArrayLiteral("font-family = Monospace\n") + defaultOutput();
    const QByteArray changes =
        QByteArrayLiteral("font-family = \nkeybind = \nconfig-file = \n");

    const GhosttyConfigLoadResult result =
        parseGhosttyConfigShowOutputs(defaults, changes, fixture.candidates());
    QVERIFY(result.succeeded());
    QVERIFY(result.snapshot->values.value(QStringLiteral("font-family"))
                .toStringList()
                .isEmpty());
    QVERIFY(result.snapshot->values.value(QStringLiteral("config-file"))
                .toStringList()
                .isEmpty());
    QVERIFY(result.snapshot->values.value(QStringLiteral("keybind"))
                .toStringList()
                .isEmpty());
}

void GhosttyConfigProcessLoaderTest::rejectsMalformedCanonicalValues()
{
    ConfigFixture fixture;
    const GhosttyConfigLoadResult malformed = parseGhosttyConfigShowOutputs(
        defaultOutput(), QByteArrayLiteral("foreground = not-a-color\n"),
        fixture.candidates());
    QVERIFY(!malformed.succeeded());
    QCOMPARE(malformed.errorMessage,
             QStringLiteral("Invalid foreground in Ghostty config output at line 1"));

    const GhosttyConfigLoadResult malformedPalette =
        parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("palette = 256=#abcdef\n"),
            fixture.candidates());
    QVERIFY(!malformedPalette.succeeded());
    QCOMPARE(malformedPalette.errorMessage,
             QStringLiteral("Invalid palette in Ghostty config output at line 1"));

    const GhosttyConfigLoadResult malformedSearch =
        parseGhosttyConfigShowOutputs(
            defaultOutput(),
            QByteArrayLiteral("search-background = not-an-alias\n"),
            fixture.candidates());
    QVERIFY(!malformedSearch.succeeded());
    QCOMPARE(malformedSearch.errorMessage,
             QStringLiteral("Invalid search-background in Ghostty config output at line 1"));

    const GhosttyConfigLoadResult emptySearch = parseGhosttyConfigShowOutputs(
        defaultOutput(), QByteArrayLiteral("search-foreground = \n"),
        fixture.candidates());
    QVERIFY(!emptySearch.succeeded());
    QCOMPARE(emptySearch.errorMessage,
             QStringLiteral("Invalid search-foreground in Ghostty config output at line 1"));

    const GhosttyConfigLoadResult malformedCursor =
        parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("cursor-style = beam\n"),
            fixture.candidates());
    QVERIFY(!malformedCursor.succeeded());
    QCOMPARE(malformedCursor.errorMessage,
             QStringLiteral("Invalid cursor-style in Ghostty config output at line 1"));

    // Ghostty accepts cursor opacity outside the nominal interval and clamps
    // it in the renderer rather than the config finalizer. Preserve that
    // canonical value here; LaunchOptions performs the renderer-side clamp.
    const GhosttyConfigLoadResult unboundedCursorOpacity =
        parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("cursor-opacity = 2\n"),
            fixture.candidates());
    QVERIFY(unboundedCursorOpacity.succeeded());
    QCOMPARE(unboundedCursorOpacity.snapshot->values
                 .value(QStringLiteral("cursor-opacity")).toDouble(),
             2.0);

    const GhosttyConfigLoadResult malformedOpacity =
        parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("faint-opacity = 1.1\n"),
            fixture.candidates());
    QVERIFY(!malformedOpacity.succeeded());
    QCOMPARE(malformedOpacity.errorMessage,
             QStringLiteral("Invalid faint-opacity in Ghostty config output at line 1"));

    const GhosttyConfigLoadResult malformedLinkUrl =
        parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("link-url = yes\n"),
            fixture.candidates());
    QVERIFY(!malformedLinkUrl.succeeded());
    QCOMPARE(malformedLinkUrl.errorMessage,
             QStringLiteral("Invalid link-url in Ghostty config output at line 1"));

    const GhosttyConfigLoadResult malformedLinkPreviews =
        parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("link-previews = yes\n"),
            fixture.candidates());
    QVERIFY(!malformedLinkPreviews.succeeded());
    QCOMPARE(malformedLinkPreviews.errorMessage,
             QStringLiteral("Invalid link-previews in Ghostty config output at line 1"));

    const struct {
        QByteArray setting;
        QString error;
    } malformedClipboard[] = {
        {QByteArrayLiteral("clipboard-trim-trailing-spaces = yes\n"),
         QStringLiteral("Invalid clipboard-trim-trailing-spaces in Ghostty config output at line 1")},
        {QByteArrayLiteral("clipboard-paste-protection = yes\n"),
         QStringLiteral("Invalid clipboard-paste-protection in Ghostty config output at line 1")},
        {QByteArrayLiteral("clipboard-paste-bracketed-safe = yes\n"),
         QStringLiteral("Invalid clipboard-paste-bracketed-safe in Ghostty config output at line 1")},
        {QByteArrayLiteral("copy-on-select = primary\n"),
         QStringLiteral("Invalid copy-on-select in Ghostty config output at line 1")},
        {QByteArrayLiteral("copy-on-select = TRUE\n"),
         QStringLiteral("Invalid copy-on-select in Ghostty config output at line 1")},
        {QByteArrayLiteral("selection-clear-on-typing = yes\n"),
         QStringLiteral("Invalid selection-clear-on-typing in Ghostty config output at line 1")},
        {QByteArrayLiteral("selection-clear-on-copy = yes\n"),
         QStringLiteral("Invalid selection-clear-on-copy in Ghostty config output at line 1")},
        {QByteArrayLiteral("middle-click-action = paste\n"),
         QStringLiteral("Invalid middle-click-action in Ghostty config output at line 1")},
    };
    for (const auto &testCase : malformedClipboard) {
        const GhosttyConfigLoadResult result = parseGhosttyConfigShowOutputs(
            defaultOutput(), testCase.setting, fixture.candidates());
        QVERIFY(!result.succeeded());
        QCOMPARE(result.errorMessage, testCase.error);
    }

    const GhosttyConfigLoadResult missing = parseGhosttyConfigShowOutputs(
        QByteArrayLiteral("font-size = 13\n"), {}, fixture.candidates());
    QVERIFY(!missing.succeeded());
    QCOMPARE(missing.errorMessage,
             QStringLiteral("Ghostty default config output is missing a required compatibility key"));

    const GhosttyConfigLoadResult maximum = parseGhosttyConfigShowOutputs(
        defaultOutput(),
        QByteArrayLiteral("scrollback-limit = 18446744073709551615\n"),
        fixture.candidates());
    QVERIFY(maximum.succeeded());
    QCOMPARE(maximum.snapshot->values.value(QStringLiteral("scrollback-limit"))
                 .toULongLong(),
             std::numeric_limits<quint64>::max());
}

void GhosttyConfigProcessLoaderTest::invokesValidationThenDefaultAndCurrentQueries()
{
    ConfigFixture fixture;
    const QString invocationLog =
        QDir(fixture.temporary.path()).filePath(QStringLiteral("invocations"));
    GhosttyConfigProcessLoaderOptions options = fakeOptions(fixture);
    QVERIFY2(!options.helperPath.isEmpty(),
             "CMake must provide GHOSTTY_QT_FAKE_CONFIG_HELPER_PATH");
    options.environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_INVOCATION_LOG"),
                               invocationLog);

    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.candidates());
    QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
    QCOMPARE(result.snapshot->values.value(QStringLiteral("font-size")).toDouble(),
             17.25);
    QVERIFY(result.snapshot->keybindConfig.has_value());
    QCOMPARE(result.snapshot->keybindConfig->root.size(), 1);
    QCOMPARE(result.snapshot->keybindConfig->root.constFirst().actions,
             QStringList({QStringLiteral("new_tab")}));

    QFile log(invocationLog);
    QVERIFY(log.open(QIODevice::ReadOnly));
    QCOMPARE(log.readAll(),
             QByteArrayLiteral("+validate-config\n"
                               "+show-config --default\n"
                               "+show-config\n"
                               "+show-keybinds-json\n"
                               "+validate-config\n"
                               "+show-config\n"
                               "+show-keybinds-json\n"));
}

void GhosttyConfigProcessLoaderTest::rejectsFailedOrMalformedStructuredQuery()
{
    ConfigFixture fixture;
    GhosttyConfigLoadResult result = makeGhosttyConfigProcessLoader(
        fakeOptions(fixture, QStringLiteral("keybinding-query-failure")))(
        fixture.candidates());
    QVERIFY(!result.succeeded());
    QCOMPARE(result.errorMessage,
             QStringLiteral(
                 "Ghostty config helper failed during keybinding query with exit code 8: "
                 "stderr: keybinding query failed"));

    result = makeGhosttyConfigProcessLoader(
        fakeOptions(fixture, QStringLiteral("keybinding-query-malformed")))(
        fixture.candidates());
    QVERIFY(!result.succeeded());
    QVERIFY(result.errorMessage.startsWith(
        QStringLiteral("Ghostty keybinding query returned malformed data:")));
}

void GhosttyConfigProcessLoaderTest::rejectsConfigThatBecomesInvalidDuringQueries()
{
    ConfigFixture fixture;
    const QString invocationLog =
        QDir(fixture.temporary.path()).filePath(QStringLiteral("invocations"));
    GhosttyConfigProcessLoaderOptions options =
        fakeOptions(fixture, QStringLiteral("post-validation-failure"));
    options.environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_INVOCATION_LOG"),
                               invocationLog);

    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.candidates());
    QVERIFY(!result.succeeded());
    QCOMPARE(result.errorMessage,
             QStringLiteral(
                 "Ghostty config helper failed during post-query validation with exit code 1: "
                 "stdout: config changed during query"));
}

void GhosttyConfigProcessLoaderTest::rejectsConfigThatChangesValidlyDuringQueries()
{
    ConfigFixture fixture;
    const QString invocationLog =
        QDir(fixture.temporary.path()).filePath(QStringLiteral("invocations"));
    GhosttyConfigProcessLoaderOptions options =
        fakeOptions(fixture, QStringLiteral("query-consistency-mismatch"));
    options.environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_INVOCATION_LOG"),
                               invocationLog);

    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.candidates());
    QVERIFY(!result.succeeded());
    QCOMPARE(result.errorMessage,
             QStringLiteral(
                 "Ghostty config changed while it was being queried; reload will retry"));
}

void GhosttyConfigProcessLoaderTest::preservesSuccessfulHelperWarnings()
{
    ConfigFixture fixture;
    GhosttyConfigProcessLoaderOptions options = fakeOptions(fixture);
    options.environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_SUCCESS_WARNING"),
                               QStringLiteral("both standard files exist\n"));

    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.candidates());
    QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
    QCOMPARE(result.snapshot->diagnostics.size(), 1);
    QCOMPARE(result.snapshot->diagnostics.constFirst().severity,
             GhosttyConfigDiagnosticSeverity::Warning);
    QCOMPARE(result.snapshot->diagnostics.constFirst().message,
             QStringLiteral(
                 "Ghostty config helper current query: both standard files exist"));
}

void GhosttyConfigProcessLoaderTest::realHelperPreservesAppearanceAndEffectiveUnbindSemantics()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral(
            "palette = 42=#123456\n"
            "selection-foreground = cell-background\n"
            "selection-background = #334455\n"
            "search-foreground = cell-background\n"
            "search-background = #123456\n"
            "search-selected-foreground = cell-foreground\n"
            "search-selected-background = #654321\n"
            "cursor-color = #abcdef\n"
            "cursor-opacity = 0.4\n"
            "cursor-style = block_hollow\n"
            "cursor-style-blink = false\n"
            "cursor-text = cell-foreground\n"
            "bold-color = bright\n"
            "faint-opacity = 0.25\n"
            "clipboard-trim-trailing-spaces = 0\n"
            "clipboard-paste-protection = 0\n"
            "clipboard-paste-bracketed-safe = t\n"
            "copy-on-select = clipboard\n"
            "selection-clear-on-typing = 0\n"
            "selection-clear-on-copy = t\n"
            "middle-click-action = ignore\n"
            "keybind = clear\n"
            "keybind = ctrl+a=ignore\n"
            "keybind = ctrl+a=unbind\n"
            "keybind = ctrl+b=new_tab\n"));

    GhosttyConfigProcessLoaderOptions options{
        .helperPath = helperPath,
        .timeoutMilliseconds = 10'000,
        .environment = QProcessEnvironment::systemEnvironment(),
    };
    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.candidates());
    QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
    const QStringList keybinds =
        result.snapshot->values.value(QStringLiteral("keybind")).toStringList();
    QCOMPARE(keybinds, QStringList({QStringLiteral("ctrl+b=new_tab")}));
    const QVariantList palette =
        result.snapshot->values.value(QStringLiteral("palette")).toList();
    QCOMPARE(palette.size(), 256);
    QCOMPARE(palette.at(42).value<QColor>(), QColor(QStringLiteral("#123456")));
    QCOMPARE(result.snapshot->values.value(
                 QStringLiteral("selection-foreground")).toString(),
             QStringLiteral("cell-background"));
    QCOMPARE(result.snapshot->values.value(
                 QStringLiteral("selection-background")).value<QColor>(),
             QColor(QStringLiteral("#334455")));
    QCOMPARE(result.snapshot->values.value(QStringLiteral("search-foreground"))
                 .toString(),
             QStringLiteral("cell-background"));
    QCOMPARE(result.snapshot->values.value(QStringLiteral("search-background"))
                 .value<QColor>(),
             QColor(QStringLiteral("#123456")));
    QCOMPARE(result.snapshot->values.value(
                 QStringLiteral("search-selected-foreground")).toString(),
             QStringLiteral("cell-foreground"));
    QCOMPARE(result.snapshot->values.value(
                 QStringLiteral("search-selected-background")).value<QColor>(),
             QColor(QStringLiteral("#654321")));
    QCOMPARE(result.snapshot->values.value(QStringLiteral("cursor-color"))
                 .value<QColor>(),
             QColor(QStringLiteral("#abcdef")));
    QCOMPARE(result.snapshot->values.value(QStringLiteral("cursor-opacity"))
                 .toDouble(),
             0.4);
    QCOMPARE(result.snapshot->values.value(QStringLiteral("cursor-style"))
                 .toString(),
             QStringLiteral("block_hollow"));
    QCOMPARE(result.snapshot->values.value(QStringLiteral("cursor-style-blink"))
                 .toBool(),
             false);
    QCOMPARE(result.snapshot->values.value(QStringLiteral("cursor-text"))
                 .toString(),
             QStringLiteral("cell-foreground"));
    QCOMPARE(result.snapshot->values.value(QStringLiteral("bold-color"))
                 .toString(),
             QStringLiteral("bright"));
    QCOMPARE(result.snapshot->values.value(QStringLiteral("faint-opacity"))
                 .toDouble(),
             0.25);
    QVERIFY(!result.snapshot->values.value(
        QStringLiteral("clipboard-trim-trailing-spaces")).toBool());
    QVERIFY(!result.snapshot->values.value(
        QStringLiteral("clipboard-paste-protection")).toBool());
    QVERIFY(result.snapshot->values.value(
        QStringLiteral("clipboard-paste-bracketed-safe")).toBool());
    QCOMPARE(result.snapshot->values.value(QStringLiteral("copy-on-select"))
                 .toString(),
             QStringLiteral("clipboard"));
    QVERIFY(!result.snapshot->values.value(
        QStringLiteral("selection-clear-on-typing")).toBool());
    QVERIFY(result.snapshot->values.value(
        QStringLiteral("selection-clear-on-copy")).toBool());
    QCOMPARE(result.snapshot->values.value(QStringLiteral("middle-click-action"))
                 .toString(),
             QStringLiteral("ignore"));
}

void GhosttyConfigProcessLoaderTest::realHelperExportsFinalizedStructuredKeybindings()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral(
            "keybind = clear\n"
            "keybind = unconsumed:performable:ctrl+x>key_y=new_tab\n"
            "keybind = chain=goto_split:left\n"
            "keybind = catch_all=ignore\n"
            "keybind = all:ctrl+g=new_tab\n"
            "keybind = global:ctrl+j=new_tab\n"
            "keybind = resize/ctrl+h=resize_split:left,10\n"));

    GhosttyConfigProcessLoaderOptions options{
        .helperPath = helperPath,
        .timeoutMilliseconds = 10'000,
        .environment = QProcessEnvironment::systemEnvironment(),
    };
    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.candidates());
    QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
    QVERIFY(result.snapshot->keybindConfig.has_value());
    const GhosttyKeybindConfig &config = *result.snapshot->keybindConfig;
    QCOMPARE(config.schemaVersion, 1);
    QCOMPARE(config.root.size(), 4);
    QCOMPARE(config.tables.size(), 1);

    const auto chained = std::find_if(
        config.root.cbegin(), config.root.cend(),
        [](const GhosttyKeybindDefinition &definition) {
            return definition.sequence.size() == 2;
        });
    QVERIFY(chained != config.root.cend());
    QCOMPARE(chained->sequence.at(0).kind,
             GhosttyKeybindKeyKind::Unicode);
    QCOMPARE(chained->sequence.at(0).unicodeCodepoint, quint32('x'));
    QCOMPARE(chained->sequence.at(0).modifiers, quint8(GhosttyKeybindCtrl));
    QCOMPARE(chained->sequence.at(1).kind,
             GhosttyKeybindKeyKind::Physical);
    QCOMPARE(chained->sequence.at(1).physicalName,
             QStringLiteral("key_y"));
    QCOMPARE(chained->actions,
             QStringList({QStringLiteral("new_tab"),
                          QStringLiteral("goto_split:left")}));
    QVERIFY(!chained->flags.consumed);
    QVERIFY(chained->flags.performable);

    const auto catchAll = std::find_if(
        config.root.cbegin(), config.root.cend(),
        [](const GhosttyKeybindDefinition &definition) {
            return definition.sequence.size() == 1
                && definition.sequence.constFirst().kind
                    == GhosttyKeybindKeyKind::CatchAll;
        });
    QVERIFY(catchAll != config.root.cend());
    QCOMPARE(catchAll->actions, QStringList({QStringLiteral("ignore")}));

    const auto all = std::find_if(
        config.root.cbegin(), config.root.cend(),
        [](const GhosttyKeybindDefinition &definition) {
            return definition.flags.all;
        });
    const auto global = std::find_if(
        config.root.cbegin(), config.root.cend(),
        [](const GhosttyKeybindDefinition &definition) {
            return definition.flags.global;
        });
    QVERIFY(all != config.root.cend());
    QVERIFY(global != config.root.cend());

    QCOMPARE(config.tables.constFirst().name, QStringLiteral("resize"));
    QCOMPARE(config.tables.constFirst().bindings.size(), 1);
    QCOMPARE(config.tables.constFirst().bindings.constFirst().actions,
             QStringList({QStringLiteral("resize_split:left,10")}));
}

void GhosttyConfigProcessLoaderTest::realHelperCanonicalizesTerminalControlActionPayloads()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    QByteArray config = QByteArrayLiteral(
        "keybind = clear\n"
        "keybind = ctrl+a=text:\\x15\n"
        "keybind = ctrl+b=text:");
    config.append(QByteArray::fromHex("f09f91bb"));
    config.append(QByteArrayLiteral(
        "\n"
        "keybind = ctrl+c=csi:"));
    config.append(QByteArray::fromHex("c3a9"));
    config.append(QByteArrayLiteral(
        "\n"
        "keybind = ctrl+d=esc:\\x7f\n"
        "keybind = ctrl+e=text:\\q\n"));
    ConfigFixture::writeFile(fixture.preferredPath, config);

    GhosttyConfigProcessLoaderOptions options{
        .helperPath = helperPath,
        .timeoutMilliseconds = 10'000,
        .environment = QProcessEnvironment::systemEnvironment(),
    };
    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.candidates());
    QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
    QVERIFY(result.snapshot->keybindConfig.has_value());
    const GhosttyKeybindConfig &keybinds = *result.snapshot->keybindConfig;
    QCOMPARE(keybinds.root.size(), 5);

    const auto actionFor = [&keybinds](quint32 codepoint) -> QStringList {
        const auto found = std::find_if(
            keybinds.root.cbegin(), keybinds.root.cend(),
            [codepoint](const GhosttyKeybindDefinition &definition) {
                return definition.sequence.size() == 1
                    && definition.sequence.constFirst().kind
                        == GhosttyKeybindKeyKind::Unicode
                    && definition.sequence.constFirst().unicodeCodepoint
                        == codepoint;
            });
        return found == keybinds.root.cend() ? QStringList{} : found->actions;
    };

    // Binding.Action.format applies std.zig.stringEscape to []const u8
    // fields. The structured JSON boundary therefore contains canonical
    // escaped bytes, not the original action payload. Execution must invert
    // this layer before applying text's separate config-string decoding.
    QCOMPARE(actionFor('a'),
             QStringList({QStringLiteral(R"(text:\\x15)")}));
    QCOMPARE(actionFor('b'),
             QStringList({QStringLiteral(R"(text:\xf0\x9f\x91\xbb)")}));
    QCOMPARE(actionFor('c'),
             QStringList({QStringLiteral(R"(csi:\xc3\xa9)")}));
    QCOMPARE(actionFor('d'),
             QStringList({QStringLiteral(R"(esc:\\x7f)")}));

    // Text escape validation is intentionally deferred until the action is
    // performed, so even a malformed source literal survives the helper as a
    // valid, canonically escaped binding payload.
    QCOMPARE(actionFor('e'),
             QStringList({QStringLiteral(R"(text:\\q)")}));
}

void GhosttyConfigProcessLoaderTest::reportsValidationFailureDeterministically()
{
    ConfigFixture fixture;
    GhosttyConfigProcessLoaderOptions options =
        fakeOptions(fixture, QStringLiteral("validation-failure"));
    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.candidates());
    QVERIFY(!result.succeeded());
    QCOMPARE(result.errorMessage,
             QStringLiteral(
                 "Ghostty config helper failed during validation with exit code 1: "
                 "stdout: config.ghostty:2:1: invalid value"));
}

void GhosttyConfigProcessLoaderTest::reportsTimeoutCrashAndStartFailureDeterministically()
{
    ConfigFixture fixture;
    GhosttyConfigProcessLoaderOptions timeout =
        fakeOptions(fixture, QStringLiteral("validation-timeout"));
    timeout.timeoutMilliseconds = 25;
    GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(timeout)(fixture.candidates());
    QVERIFY(!result.succeeded());
    QCOMPARE(result.errorMessage,
             QStringLiteral(
                 "Ghostty config helper timed out during validation after 25 ms"));

    GhosttyConfigProcessLoaderOptions crash =
        fakeOptions(fixture, QStringLiteral("validation-crash"));
    result = makeGhosttyConfigProcessLoader(crash)(fixture.candidates());
    QVERIFY(!result.succeeded());
    QCOMPARE(result.errorMessage,
             QStringLiteral("Ghostty config helper crashed during validation"));

    GhosttyConfigProcessLoaderOptions missing = fakeOptions(fixture);
    missing.helperPath = QDir(fixture.temporary.path())
                             .filePath(QStringLiteral("does-not-exist"));
    missing.timeoutMilliseconds = 100;
    result = makeGhosttyConfigProcessLoader(missing)(fixture.candidates());
    QVERIFY(!result.succeeded());
    QCOMPARE(result.errorMessage,
             QStringLiteral(
                 "Ghostty config helper could not be started during validation"));
}

QTEST_GUILESS_MAIN(GhosttyConfigProcessLoaderTest)

#include "test_ghostty_config_process_loader.moc"
