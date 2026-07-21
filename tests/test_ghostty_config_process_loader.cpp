#include "ghostty_config_process_loader.h"
#include "ghostty_config_export.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QMetaType>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <expected>
#include <limits>

#ifndef GHOSTTY_QT_FAKE_CONFIG_HELPER_PATH
#define GHOSTTY_QT_FAKE_CONFIG_HELPER_PATH ""
#endif

#ifndef GHOSTTY_QT_REAL_CONFIG_HELPER_PATH
#define GHOSTTY_QT_REAL_CONFIG_HELPER_PATH ""
#endif

namespace {

template<typename Value>
QString errorMessage(const std::expected<Value, QString> &result)
{
    return result ? QString{} : result.error();
}

QByteArray defaultOutput()
{
    QByteArray output =
        QByteArrayLiteral("working-directory = \n"
                          "font-family = \n"
                          "font-size = 13\n"
                          "foreground = #ffffff\n"
                          "background = #282c34\n"
                          "unfocused-split-opacity = 0.7\n"
                          "unfocused-split-fill = \n"
                          "split-divider-color = \n"
                          "split-inherit-working-directory = true\n"
                          "split-preserve-zoom = no-navigation\n"
                          "tab-inherit-working-directory = true\n"
                          "window-inherit-working-directory = true\n"
                          "window-inherit-font-size = true\n"
                          "window-new-tab-position = current\n"
                          "window-show-tab-bar = auto\n"
                          "maximize = false\n"
                          "fullscreen = false\n"
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
                          "mouse-reporting = true\n"
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
        : temporary(temporaryTemplate())
    {
        if (!temporary.isValid()) {
            qFatal("could not create repository-local config fixture");
        }
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

    static QString temporaryTemplate()
    {
        const QString directory =
            QDir::current().filePath(QStringLiteral("tmp"));
        if (!QDir().mkpath(directory)) {
            qFatal("could not create repository-local tmp directory");
        }
        return QDir(directory).filePath(
            QStringLiteral("config-loader-XXXXXX"));
    }

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
        QStringLiteral("GHOSTTY_QT_FAKE_CONFIG_JSON"),
        QStringLiteral(
            R"json({"version":3,"application":{"quit-after-last-window-closed":true,"quit-after-last-window-closed-delay-ms":null,"initial-window":true,"gtk-single-instance":"detect"},"keybindings":{"root":[{"sequence":[{"kind":"unicode","codepoint":116,"mods":3}],"actions":["new_tab"],"flags":{"consumed":true,"all":false,"global":false,"performable":false}}],"tables":[]}})json"));
    if (!mode.isEmpty()) {
        environment.insert(QStringLiteral("GHOSTTY_QT_FAKE_MODE"), mode);
    }
    return {
        .helperPath = QString::fromUtf8(GHOSTTY_QT_FAKE_CONFIG_HELPER_PATH),
        .timeoutMilliseconds = 2'000,
        .environment = environment,
    };
}

std::expected<GhosttyConfigExport, QString> queryRealConfigExport(
    const QString &helperPath, const ConfigFixture &fixture)
{
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), fixture.xdgHome);
    process.setProcessEnvironment(environment);
    process.setProgram(helperPath);
    process.setArguments({QStringLiteral("+show-config-json")});
    process.start(QIODevice::ReadOnly);
    if (!process.waitForStarted(10'000)) {
        return std::unexpected(QStringLiteral("structured helper did not start"));
    }
    if (!process.waitForFinished(10'000)) {
        process.kill();
        process.waitForFinished(1'000);
        return std::unexpected(QStringLiteral("structured helper timed out"));
    }
    if (process.exitStatus() != QProcess::NormalExit
        || process.exitCode() != 0) {
        return std::unexpected(
            QStringLiteral("structured helper failed: %1")
                .arg(QString::fromUtf8(process.readAllStandardError()).trimmed()));
    }
    return parseGhosttyConfigExportJson(process.readAllStandardOutput());
}

} // namespace

class GhosttyConfigProcessLoaderTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void derivesXdgHomeFromEitherCandidateOrder();
    void parsesStructuredConfigJson();
    void parsesEmptyStructuredConfigJson();
    void rejectsMalformedStructuredConfigJson();
    void mergesCanonicalOutputsIntoTypedSnapshot();
    void preservesDefaultAndAcceptsEveryWindowShowTabBarMode();
    void preservesDefaultAndAcceptsEveryFullscreenMode();
    void preservesDefaultAndAcceptsEveryLinkPreviewMode();
    void preservesDefaultsAndAcceptsEveryClipboardMode();
    void emptyRepeatableChangesResetDefaults();
    void emptyNullableChangesOverrideDefaults();
    void rejectsMalformedCanonicalValues();
    void invokesValidationThenDefaultAndCurrentQueries();
    void loadsStructuredApplicationLifetimeValues();
    void rejectsFailedOrMalformedStructuredQuery();
    void rejectsConfigThatBecomesInvalidDuringQueries();
    void rejectsConfigThatChangesValidlyDuringQueries();
    void preservesSuccessfulHelperWarnings();
    void realHelperFinalizesSurfaceInheritance();
    void realHelperFinalizesUnfocusedSplitAppearance_data();
    void realHelperFinalizesUnfocusedSplitAppearance();
    void realHelperPreservesAppearanceAndEffectiveUnbindSemantics();
    void realHelperExportsApplicationLifetime();
    void realHelperExportsFinalizedStructuredKeybindings();
    void realHelperCanonicalizesTerminalControlActionPayloads();
    void reportsValidationFailureDeterministically();
    void reportsTimeoutCrashAndStartFailureDeterministically();
};

void GhosttyConfigProcessLoaderTest::parsesStructuredConfigJson()
{
    const QByteArray json = QByteArrayLiteral(R"json({
        "version": 3,
        "application": {
            "quit-after-last-window-closed": false,
            "quit-after-last-window-closed-delay-ms": 1500,
            "initial-window": false,
            "gtk-single-instance": "false"
        },
        "keybindings": {
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
        }
    })json");

    const auto parsed = parseGhosttyConfigExportJson(json);
    QVERIFY2(parsed.has_value(), qPrintable(errorMessage(parsed)));
    QVERIFY(!parsed->quitAfterLastWindowClosed);
    QCOMPARE(parsed->quitAfterLastWindowClosedDelayMilliseconds,
             std::optional<quint32>(1500));
    QVERIFY(!parsed->initialWindow);
    QCOMPARE(parsed->singleInstanceMode, QStringLiteral("false"));
    const GhosttyKeybindConfig &config = parsed->keybindings;
    QCOMPARE(config.schemaVersion,
             GhosttyKeybindConfig::CurrentSchemaVersion);
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

void GhosttyConfigProcessLoaderTest::parsesEmptyStructuredConfigJson()
{
    const auto parsed = parseGhosttyConfigExportJson(
        QByteArrayLiteral(R"json({"version":3,"application":{"quit-after-last-window-closed":true,"quit-after-last-window-closed-delay-ms":null,"initial-window":true,"gtk-single-instance":"detect"},"keybindings":{"root":[],"tables":[]}})json"));
    QVERIFY2(parsed.has_value(), qPrintable(errorMessage(parsed)));
    QCOMPARE(parsed->schemaVersion,
             GhosttyConfigExport::CurrentSchemaVersion);
    QVERIFY(parsed->quitAfterLastWindowClosed);
    QVERIFY(!parsed->quitAfterLastWindowClosedDelayMilliseconds.has_value());
    QVERIFY(parsed->initialWindow);
    QCOMPARE(parsed->singleInstanceMode, QStringLiteral("detect"));
    QVERIFY(parsed->keybindings.root.isEmpty());
    QVERIFY(parsed->keybindings.tables.isEmpty());
}

void GhosttyConfigProcessLoaderTest::rejectsMalformedStructuredConfigJson()
{
    auto parsed = parseGhosttyConfigExportJson(
        QByteArrayLiteral("{not-json"));
    QVERIFY(!parsed.has_value());
    QVERIFY(parsed.error().startsWith(
        QStringLiteral("Invalid Ghostty structured config JSON at offset ")));

    parsed = parseGhosttyConfigExportJson(
        QByteArrayLiteral(R"json({
            "version":3,
            "application":{"quit-after-last-window-closed":true,"quit-after-last-window-closed-delay-ms":null,"initial-window":true,"gtk-single-instance":"detect"},
            "keybindings":{"root":[{
                    "sequence":[{"kind":"unicode","codepoint":55296,"mods":0}],
                    "actions":["ignore"],
                    "flags":{"consumed":true,"all":false,"global":false,"performable":false}
                }],"tables":[]}
        })json"));
    QVERIFY(!parsed.has_value());
    QVERIFY(parsed.error().contains(QStringLiteral("Unicode scalar")));

    parsed = parseGhosttyConfigExportJson(
        QByteArrayLiteral(R"json({"version":4,"application":{"quit-after-last-window-closed":true,"quit-after-last-window-closed-delay-ms":null,"initial-window":true,"gtk-single-instance":"detect"},"keybindings":{"root":[],"tables":[]}})json"));
    QVERIFY(!parsed.has_value());
    QCOMPARE(parsed.error(),
             QStringLiteral(
                 "Unsupported Ghostty structured config JSON schema version"));

    parsed = parseGhosttyConfigExportJson(
        QByteArrayLiteral(R"json({"version":2,"application":{"quit-after-last-window-closed":true,"quit-after-last-window-closed-delay-ms":null,"initial-window":true,"gtk-single-instance":"detect"},"keybindings":{"root":[],"tables":[]}})json"));
    QVERIFY(!parsed.has_value());
    QCOMPARE(parsed.error(),
             QStringLiteral(
                 "Unsupported Ghostty structured config JSON schema version"));

    parsed = parseGhosttyConfigExportJson(
        QByteArrayLiteral(R"json({
            "version":3,
            "application":{"quit-after-last-window-closed":true,"quit-after-last-window-closed-delay-ms":null,"initial-window":true,"gtk-single-instance":"detect"},
            "keybindings":{"root":[],"tables":[
                    {"name":"modal","bindings":[]},
                    {"name":"modal","bindings":[]}
                ]}
        })json"));
    QVERIFY(!parsed.has_value());
    QCOMPARE(parsed.error(),
             QStringLiteral("Duplicate Ghostty keybinding table 'modal'"));

    parsed = parseGhosttyConfigExportJson(QByteArrayLiteral(R"json({
        "version":3,
        "application":{"quit-after-last-window-closed":"true","quit-after-last-window-closed-delay-ms":null,"initial-window":true,"gtk-single-instance":"detect"},
        "keybindings":{"root":[],"tables":[]}
    })json"));
    QVERIFY(!parsed.has_value());
    QCOMPARE(parsed.error(), QStringLiteral(
        "application.quit-after-last-window-closed must be a boolean"));

    parsed = parseGhosttyConfigExportJson(QByteArrayLiteral(R"json({
        "version":3,
        "application":{"quit-after-last-window-closed":true,"quit-after-last-window-closed-delay-ms":4294967296,"initial-window":true,"gtk-single-instance":"detect"},
        "keybindings":{"root":[],"tables":[]}
    })json"));
    QVERIFY(!parsed.has_value());
    QCOMPARE(parsed.error(), QStringLiteral(
        "application.quit-after-last-window-closed-delay-ms must be null or a uint32 integer"));

    parsed = parseGhosttyConfigExportJson(QByteArrayLiteral(R"json({
        "version":3,
        "application":{"quit-after-last-window-closed":true,"quit-after-last-window-closed-delay-ms":null,"initial-window":true,"gtk-single-instance":true},
        "keybindings":{"root":[],"tables":[]}
    })json"));
    QVERIFY(!parsed.has_value());
    QCOMPARE(parsed.error(), QStringLiteral(
        "application.gtk-single-instance must be false, true, or detect"));

    parsed = parseGhosttyConfigExportJson(QByteArrayLiteral(R"json({
        "version":3,
        "application":{"quit-after-last-window-closed":true,"quit-after-last-window-closed-delay-ms":null,"initial-window":true,"gtk-single-instance":"desktop"},
        "keybindings":{"root":[],"tables":[]}
    })json"));
    QVERIFY(!parsed.has_value());
    QCOMPARE(parsed.error(), QStringLiteral(
        "application.gtk-single-instance must be false, true, or detect"));

    parsed = parseGhosttyConfigExportJson(QByteArrayLiteral(R"json({
        "version":3,
        "application":{"quit-after-last-window-closed":true,"quit-after-last-window-closed-delay-ms":null,"initial-window":true},
        "keybindings":{"root":[],"tables":[]}
    })json"));
    QVERIFY(!parsed.has_value());
    QCOMPARE(parsed.error(), QStringLiteral(
        "application is missing field 'gtk-single-instance'"));

    for (const QByteArray &invalidDelay : {
             QByteArrayLiteral("-1"), QByteArrayLiteral("1.5"),
         }) {
        const QByteArray json = QByteArrayLiteral(
            R"json({"version":3,"application":{"quit-after-last-window-closed":true,"quit-after-last-window-closed-delay-ms":)json")
            + invalidDelay
            + QByteArrayLiteral(
                R"json(,"initial-window":true,"gtk-single-instance":"detect"},"keybindings":{"root":[],"tables":[]}})json");
        parsed = parseGhosttyConfigExportJson(json);
        QVERIFY(!parsed.has_value());
        QCOMPARE(parsed.error(), QStringLiteral(
            "application.quit-after-last-window-closed-delay-ms must be null or a uint32 integer"));
    }

    parsed = parseGhosttyConfigExportJson(QByteArrayLiteral(R"json({
        "version":3,
        "application":{"quit-after-last-window-closed":true,"initial-window":true,"gtk-single-instance":"detect"},
        "keybindings":{"root":[],"tables":[]}
    })json"));
    QVERIFY(!parsed.has_value());
    QCOMPARE(parsed.error(), QStringLiteral(
        "application is missing field 'quit-after-last-window-closed-delay-ms'"));

    parsed = parseGhosttyConfigExportJson(QByteArrayLiteral(R"json({
        "version":3,
        "application":{"quit-after-last-window-closed":true,"quit-after-last-window-closed-delay-ms":null,"initial-window":true,"gtk-single-instance":"detect","extra":false},
        "keybindings":{"root":[],"tables":[]}
    })json"));
    QVERIFY(!parsed.has_value());
    QCOMPARE(parsed.error(),
             QStringLiteral("application has unexpected field 'extra'"));

    parsed = parseGhosttyConfigExportJson(QByteArrayLiteral(R"json({
        "version":3,
        "application":{"quit-after-last-window-closed":true,"quit-after-last-window-closed-delay-ms":null,"initial-window":"true","gtk-single-instance":"detect"},
        "keybindings":{"root":[],"tables":[]}
    })json"));
    QVERIFY(!parsed.has_value());
    QCOMPARE(parsed.error(), QStringLiteral(
        "application.initial-window must be a boolean"));

    parsed = parseGhosttyConfigExportJson(QByteArrayLiteral(R"json({
        "version":3,
        "application":{"quit-after-last-window-closed":true,"quit-after-last-window-closed-delay-ms":null,"gtk-single-instance":"detect"},
        "keybindings":{"root":[],"tables":[]}
    })json"));
    QVERIFY(!parsed.has_value());
    QCOMPARE(parsed.error(), QStringLiteral(
        "application is missing field 'initial-window'"));
}

void GhosttyConfigProcessLoaderTest::derivesXdgHomeFromEitherCandidateOrder()
{
    ConfigFixture fixture;
    auto result = ghosttyConfigXdgHome(fixture.candidates());
    QVERIFY(result.has_value());
    QCOMPARE(*result, fixture.xdgHome);

    result = ghosttyConfigXdgHome(
        {fixture.preferredPath, fixture.legacyPath});
    QVERIFY(result.has_value());
    QCOMPARE(*result, fixture.xdgHome);

    result = ghosttyConfigXdgHome({fixture.legacyPath});
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(),
             QStringLiteral("Ghostty config candidates must contain both config and config.ghostty"));
}

void GhosttyConfigProcessLoaderTest::mergesCanonicalOutputsIntoTypedSnapshot()
{
    ConfigFixture fixture;
    const GhosttyConfigLoadResult canonicalDefaults =
        parseGhosttyConfigShowOutputs(defaultOutput(), {},
                                      fixture.candidates());
    QVERIFY2(canonicalDefaults.has_value(),
             qPrintable(errorMessage(canonicalDefaults)));
    QCOMPARE(canonicalDefaults->values.value(
                 QStringLiteral("working-directory")),
             QVariant(QString{}));
    QVERIFY(canonicalDefaults->values.value(
                QStringLiteral("split-inherit-working-directory")).toBool());
    QVERIFY(!canonicalDefaults->values.value(
                 QStringLiteral("split-preserve-zoom")).toBool());
    QVERIFY(canonicalDefaults->values.value(
                QStringLiteral("tab-inherit-working-directory")).toBool());
    QVERIFY(canonicalDefaults->values.value(
                QStringLiteral("window-inherit-working-directory")).toBool());
    QVERIFY(canonicalDefaults->values.value(
                QStringLiteral("window-inherit-font-size")).toBool());
    QCOMPARE(canonicalDefaults->values.value(
                 QStringLiteral("window-new-tab-position")).toString(),
             QStringLiteral("current"));
    QCOMPARE(canonicalDefaults->values.value(
                 QStringLiteral("window-show-tab-bar")).toString(),
             QStringLiteral("auto"));
    QVERIFY(!canonicalDefaults->values.value(
                 QStringLiteral("maximize")).toBool());
    QCOMPARE(canonicalDefaults->values.value(
                 QStringLiteral("fullscreen")).toString(),
             QStringLiteral("false"));
    QCOMPARE(canonicalDefaults->values.value(
                 QStringLiteral("unfocused-split-opacity")).toDouble(),
             0.7);
    QCOMPARE(canonicalDefaults->values.value(
                 QStringLiteral("unfocused-split-fill")),
             QVariant(QString{}));

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
            "working-directory = %3\r\n"
            "foreground = #102030\r\n"
            "unfocused-split-opacity = 0.42\r\n"
            "unfocused-split-fill = #778899\r\n"
            "split-divider-color = #a1b2c3\r\n"
            "split-inherit-working-directory = false\r\n"
            "split-preserve-zoom = navigation\r\n"
            "tab-inherit-working-directory = false\r\n"
            "window-inherit-working-directory = false\r\n"
            "window-inherit-font-size = false\r\n"
            "window-new-tab-position = end\r\n"
            "window-show-tab-bar = never\r\n"
            "maximize = true\r\n"
            "fullscreen = non-native-visible-menu\r\n"
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
            "mouse-reporting = false\r\n"
            "link-url = false\r\n"
            "link-previews = osc8\r\n"
            "keybind = alt+n=new_tab\r\n"
            "keybind = chain=next_tab\r\n"
            "keybind = ctrl+x>ctrl+y=new_tab\r\n"
            "keybind = ctrl+f=toggle_fullscreen\r\n"
            "keybind = ctrl+u=open_config\r\n"
            "config-file = %1\r\n"
            "config-file = ?%2\r\n"))
                                   .arg(includePath, missingOptional,
                                        fixture.temporary.path())
                                   .toUtf8();

    const GhosttyConfigLoadResult result =
        parseGhosttyConfigShowOutputs(defaultOutput(), changes,
                                      fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    const GhosttyConfigSnapshot &snapshot = *result;
    QCOMPARE(snapshot.availability, GhosttyConfigAvailability::Available);
    QCOMPARE(snapshot.values.value(QStringLiteral("font-family")).toStringList(),
             QStringList({QStringLiteral("JetBrains Mono"),
                          QStringLiteral("Noto Color Emoji")}));
    QCOMPARE(snapshot.values.value(QStringLiteral("font-size")).toDouble(), 15.5);
    QCOMPARE(snapshot.values.value(QStringLiteral("foreground")).value<QColor>(),
             QColor(QStringLiteral("#102030")));
    QCOMPARE(snapshot.values.value(QStringLiteral("background")).value<QColor>(),
             QColor(QStringLiteral("#282c34")));
    QCOMPARE(snapshot.values.value(QStringLiteral("working-directory")).toString(),
             fixture.temporary.path());
    QVERIFY(!snapshot.values.value(
                 QStringLiteral("split-inherit-working-directory")).toBool());
    QVERIFY(snapshot.values.value(
                QStringLiteral("split-preserve-zoom")).toBool());
    QVERIFY(!snapshot.values.value(
                 QStringLiteral("tab-inherit-working-directory")).toBool());
    QVERIFY(!snapshot.values.value(
                 QStringLiteral("window-inherit-working-directory")).toBool());
    QVERIFY(!snapshot.values.value(
                 QStringLiteral("window-inherit-font-size")).toBool());
    QCOMPARE(snapshot.values.value(
                 QStringLiteral("window-new-tab-position")).toString(),
             QStringLiteral("end"));
    QCOMPARE(snapshot.values.value(
                 QStringLiteral("window-show-tab-bar")).toString(),
             QStringLiteral("never"));
    QVERIFY(snapshot.values.value(QStringLiteral("maximize")).toBool());
    QCOMPARE(snapshot.values.value(QStringLiteral("fullscreen")).toString(),
             QStringLiteral("non-native-visible-menu"));
    QCOMPARE(snapshot.values.value(
                 QStringLiteral("unfocused-split-opacity")).toDouble(),
             0.42);
    QCOMPARE(snapshot.values.value(QStringLiteral("unfocused-split-fill"))
                 .value<QColor>(),
             QColor(QStringLiteral("#778899")));
    QCOMPARE(snapshot.values.value(QStringLiteral("split-divider-color"))
                 .value<QColor>(),
             QColor(QStringLiteral("#a1b2c3")));
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
    QVERIFY(!snapshot.values.value(
        QStringLiteral("mouse-reporting")).toBool());
    QCOMPARE(snapshot.values.value(QStringLiteral("link-url")).toBool(), false);
    QCOMPARE(snapshot.values.value(QStringLiteral("link-previews")).toString(),
             QStringLiteral("osc8"));
    QCOMPARE(snapshot.values.value(QStringLiteral("keybind")).toStringList(),
             QStringList({QStringLiteral("alt+n=new_tab"),
                          QStringLiteral("chain=next_tab"),
                          QStringLiteral("ctrl+x>ctrl+y=new_tab"),
                          QStringLiteral("ctrl+f=toggle_fullscreen"),
                          QStringLiteral("ctrl+u=open_config")}));
    QVERIFY(snapshot.diagnostics.isEmpty());
    QCOMPARE(snapshot.values.value(QStringLiteral("config-file")).toStringList(),
             QStringList({includePath, QStringLiteral("?") + missingOptional}));
    QCOMPARE(snapshot.sourcePaths,
             QStringList({fixture.legacyPath, fixture.preferredPath, includePath}));
}

void GhosttyConfigProcessLoaderTest::preservesDefaultAndAcceptsEveryWindowShowTabBarMode()
{
    ConfigFixture fixture;
    const GhosttyConfigLoadResult unchanged = parseGhosttyConfigShowOutputs(
        defaultOutput(), {}, fixture.candidates());
    QVERIFY2(unchanged.has_value(), qPrintable(errorMessage(unchanged)));
    QCOMPARE(unchanged->values
                 .value(QStringLiteral("window-show-tab-bar")).toString(),
             QStringLiteral("auto"));

    for (const QByteArray &mode : {
             QByteArrayLiteral("always"), QByteArrayLiteral("auto"),
             QByteArrayLiteral("never"),
         }) {
        const GhosttyConfigLoadResult changed = parseGhosttyConfigShowOutputs(
            defaultOutput(),
            QByteArrayLiteral("window-show-tab-bar = ") + mode + '\n',
            fixture.candidates());
        QVERIFY2(changed.has_value(), qPrintable(errorMessage(changed)));
        QCOMPARE(changed->values
                     .value(QStringLiteral("window-show-tab-bar")).toString(),
                 QString::fromLatin1(mode));
    }
}

void GhosttyConfigProcessLoaderTest::preservesDefaultAndAcceptsEveryFullscreenMode()
{
    ConfigFixture fixture;
    const GhosttyConfigLoadResult unchanged = parseGhosttyConfigShowOutputs(
        defaultOutput(), {}, fixture.candidates());
    QVERIFY2(unchanged.has_value(), qPrintable(errorMessage(unchanged)));
    QVERIFY(!unchanged->values.value(QStringLiteral("maximize")).toBool());
    QCOMPARE(unchanged->values.value(QStringLiteral("fullscreen")).toString(),
             QStringLiteral("false"));

    for (const QByteArray &mode : {
             QByteArrayLiteral("false"),
             QByteArrayLiteral("true"),
             QByteArrayLiteral("non-native"),
             QByteArrayLiteral("non-native-visible-menu"),
             QByteArrayLiteral("non-native-padded-notch"),
         }) {
        const GhosttyConfigLoadResult changed = parseGhosttyConfigShowOutputs(
            defaultOutput(),
            QByteArrayLiteral("maximize = true\nfullscreen = ") + mode + '\n',
            fixture.candidates());
        QVERIFY2(changed.has_value(), qPrintable(errorMessage(changed)));
        QVERIFY(changed->values.value(QStringLiteral("maximize")).toBool());
        QCOMPARE(changed->values.value(QStringLiteral("fullscreen")).toString(),
                 QString::fromLatin1(mode));
    }
}

void GhosttyConfigProcessLoaderTest::preservesDefaultAndAcceptsEveryLinkPreviewMode()
{
    ConfigFixture fixture;
    const GhosttyConfigLoadResult unchanged = parseGhosttyConfigShowOutputs(
        defaultOutput(), {}, fixture.candidates());
    QVERIFY2(unchanged.has_value(), qPrintable(errorMessage(unchanged)));
    QCOMPARE(unchanged->values
                 .value(QStringLiteral("link-previews")).toString(),
             QStringLiteral("true"));
    QCOMPARE(unchanged->values
                 .value(QStringLiteral("search-foreground")).value<QColor>(),
             QColor(QStringLiteral("#000000")));
    QCOMPARE(unchanged->values
                 .value(QStringLiteral("search-background")).value<QColor>(),
             QColor(QStringLiteral("#ffe082")));
    QCOMPARE(unchanged->values.value(
                 QStringLiteral("search-selected-foreground")).value<QColor>(),
             QColor(QStringLiteral("#000000")));
    QCOMPARE(unchanged->values.value(
                 QStringLiteral("search-selected-background")).value<QColor>(),
             QColor(QStringLiteral("#f2a57e")));

    for (const QByteArray &mode : {QByteArrayLiteral("false"),
                                   QByteArrayLiteral("true"),
                                   QByteArrayLiteral("osc8")}) {
        const GhosttyConfigLoadResult changed = parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("link-previews = ") + mode + '\n',
            fixture.candidates());
        QVERIFY2(changed.has_value(), qPrintable(errorMessage(changed)));
        QCOMPARE(changed->values
                     .value(QStringLiteral("link-previews")).toString(),
                 QString::fromLatin1(mode));
    }
}

void GhosttyConfigProcessLoaderTest::preservesDefaultsAndAcceptsEveryClipboardMode()
{
    ConfigFixture fixture;
    const GhosttyConfigLoadResult unchanged = parseGhosttyConfigShowOutputs(
        defaultOutput(), {}, fixture.candidates());
    QVERIFY2(unchanged.has_value(), qPrintable(errorMessage(unchanged)));
    const QVariantMap &defaults = unchanged->values;
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
    QVERIFY(defaults.value(QStringLiteral("mouse-reporting")).toBool());

    for (const QByteArray &mode : {QByteArrayLiteral("false"),
                                   QByteArrayLiteral("true"),
                                   QByteArrayLiteral("clipboard")}) {
        const GhosttyConfigLoadResult changed = parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("copy-on-select = ") + mode + '\n',
            fixture.candidates());
        QVERIFY2(changed.has_value(), qPrintable(errorMessage(changed)));
        QCOMPARE(changed->values
                     .value(QStringLiteral("copy-on-select")).toString(),
                 QString::fromLatin1(mode));
    }

    for (const QByteArray &action : {QByteArrayLiteral("primary-paste"),
                                     QByteArrayLiteral("ignore")}) {
        const GhosttyConfigLoadResult changed = parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("middle-click-action = ")
                + action + '\n', fixture.candidates());
        QVERIFY2(changed.has_value(), qPrintable(errorMessage(changed)));
        QCOMPARE(changed->values
                     .value(QStringLiteral("middle-click-action")).toString(),
                 QString::fromLatin1(action));
    }

    for (const QByteArray &enabled : {QByteArrayLiteral("false"),
                                      QByteArrayLiteral("true")}) {
        const GhosttyConfigLoadResult changed = parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("mouse-reporting = ")
                + enabled + '\n', fixture.candidates());
        QVERIFY2(changed.has_value(), qPrintable(errorMessage(changed)));
        QCOMPARE(changed->values
                     .value(QStringLiteral("mouse-reporting")).toBool(),
                 enabled == QByteArrayLiteral("true"));
    }
}

void GhosttyConfigProcessLoaderTest::emptyRepeatableChangesResetDefaults()
{
    ConfigFixture fixture;
    const QString includePath = QDir(fixture.temporary.path())
                                    .filePath(QStringLiteral("default.conf"));
    ConfigFixture::writeFile(includePath, {});
    const QByteArray defaults = defaultOutput()
        + QStringLiteral("font-family = Monospace\nconfig-file = %1\n")
              .arg(includePath)
              .toUtf8();

    const GhosttyConfigLoadResult unchanged =
        parseGhosttyConfigShowOutputs(defaults, {}, fixture.candidates());
    QVERIFY2(unchanged.has_value(), qPrintable(errorMessage(unchanged)));
    QCOMPARE(unchanged->values.value(QStringLiteral("font-family"))
                 .toStringList(),
             QStringList({QStringLiteral("Monospace")}));
    QCOMPARE(unchanged->values.value(QStringLiteral("config-file"))
                 .toStringList(),
             QStringList({includePath}));
    QCOMPARE(unchanged->values.value(QStringLiteral("keybind"))
                 .toStringList(),
             QStringList({QStringLiteral("ctrl+shift+t=new_tab")}));

    const QByteArray changes =
        QByteArrayLiteral("font-family = \nkeybind = \nconfig-file = \n");

    const GhosttyConfigLoadResult result =
        parseGhosttyConfigShowOutputs(defaults, changes, fixture.candidates());
    QVERIFY(result.has_value());
    QVERIFY(result->values.value(QStringLiteral("font-family"))
                .toStringList()
                .isEmpty());
    QVERIFY(result->values.value(QStringLiteral("config-file"))
                .toStringList()
                .isEmpty());
    QVERIFY(result->values.value(QStringLiteral("keybind"))
                .toStringList()
                .isEmpty());
}

void GhosttyConfigProcessLoaderTest::emptyNullableChangesOverrideDefaults()
{
    ConfigFixture fixture;
    const QByteArray defaults = defaultOutput()
        + QByteArrayLiteral(
            "working-directory = /configured/default\n"
            "unfocused-split-fill = #080706\n"
            "split-divider-color = #090807\n"
            "selection-foreground = #101010\n"
            "selection-background = #202020\n"
            "cursor-color = #303030\n"
            "cursor-style-blink = true\n"
            "cursor-text = #404040\n"
            "bold-color = #505050\n");
    const QByteArray changes = QByteArrayLiteral(
        "working-directory = \n"
        "unfocused-split-fill = \n"
        "split-divider-color = \n"
        "selection-foreground = \n"
        "selection-background = \n"
        "cursor-color = \n"
        "cursor-style-blink = \n"
        "cursor-text = \n"
        "bold-color = \n");

    const GhosttyConfigLoadResult result =
        parseGhosttyConfigShowOutputs(defaults, changes,
                                      fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    const QVariant emptyString = QString{};
    for (const QString &key : {
             QStringLiteral("split-divider-color"),
             QStringLiteral("selection-foreground"),
             QStringLiteral("selection-background"),
             QStringLiteral("cursor-color"),
             QStringLiteral("cursor-style-blink"),
             QStringLiteral("cursor-text"),
             QStringLiteral("bold-color"),
         }) {
        QCOMPARE(result->values.value(key), emptyString);
    }

    QCOMPARE(result->values.value(QStringLiteral("unfocused-split-fill")),
             emptyString);
    QCOMPARE(result->values.value(QStringLiteral("working-directory")),
             emptyString);

    QByteArray missingNullableDefault = defaultOutput();
    const QByteArray nullableLine = QByteArrayLiteral("cursor-color = \n");
    const qsizetype nullableLineOffset =
        missingNullableDefault.indexOf(nullableLine);
    QVERIFY(nullableLineOffset >= 0);
    missingNullableDefault.remove(nullableLineOffset, nullableLine.size());
    const GhosttyConfigLoadResult missing = parseGhosttyConfigShowOutputs(
        missingNullableDefault, {}, fixture.candidates());
    QVERIFY(!missing.has_value());
    QCOMPARE(missing.error(),
             QStringLiteral("Ghostty default config output is missing a required compatibility key"));

    QByteArray missingDividerDefault = defaultOutput();
    const QByteArray dividerLine =
        QByteArrayLiteral("split-divider-color = \n");
    const qsizetype dividerLineOffset =
        missingDividerDefault.indexOf(dividerLine);
    QVERIFY(dividerLineOffset >= 0);
    missingDividerDefault.remove(dividerLineOffset, dividerLine.size());
    const GhosttyConfigLoadResult missingDivider =
        parseGhosttyConfigShowOutputs(
            missingDividerDefault, {}, fixture.candidates());
    QVERIFY(!missingDivider.has_value());
    QCOMPARE(missingDivider.error(),
             QStringLiteral("Ghostty default config output is missing a required compatibility key"));

    for (const QByteArray &requiredLine : {
             QByteArrayLiteral("working-directory = \n"),
             QByteArrayLiteral("split-inherit-working-directory = true\n"),
             QByteArrayLiteral("split-preserve-zoom = no-navigation\n"),
             QByteArrayLiteral("tab-inherit-working-directory = true\n"),
             QByteArrayLiteral("window-inherit-working-directory = true\n"),
             QByteArrayLiteral("window-inherit-font-size = true\n"),
             QByteArrayLiteral("window-new-tab-position = current\n"),
             QByteArrayLiteral("window-show-tab-bar = auto\n"),
             QByteArrayLiteral("maximize = false\n"),
             QByteArrayLiteral("fullscreen = false\n"),
             QByteArrayLiteral("mouse-reporting = true\n"),
             QByteArrayLiteral("unfocused-split-opacity = 0.7\n"),
             QByteArrayLiteral("unfocused-split-fill = \n"),
         }) {
        QByteArray missingDefault = defaultOutput();
        const qsizetype offset = missingDefault.indexOf(requiredLine);
        QVERIFY(offset >= 0);
        missingDefault.remove(offset, requiredLine.size());
        const GhosttyConfigLoadResult missingUnfocused =
            parseGhosttyConfigShowOutputs(
                missingDefault, {}, fixture.candidates());
        QVERIFY(!missingUnfocused.has_value());
        QCOMPARE(missingUnfocused.error(),
                 QStringLiteral("Ghostty default config output is missing a required compatibility key"));
    }
}

void GhosttyConfigProcessLoaderTest::rejectsMalformedCanonicalValues()
{
    ConfigFixture fixture;
    const GhosttyConfigLoadResult malformed = parseGhosttyConfigShowOutputs(
        defaultOutput(), QByteArrayLiteral("foreground = not-a-color\n"),
        fixture.candidates());
    QVERIFY(!malformed.has_value());
    QCOMPARE(malformed.error(),
             QStringLiteral("Invalid foreground in Ghostty config output at line 1"));

    const GhosttyConfigLoadResult malformedDivider =
        parseGhosttyConfigShowOutputs(
            defaultOutput(),
            QByteArrayLiteral("split-divider-color = not-a-color\n"),
            fixture.candidates());
    QVERIFY(!malformedDivider.has_value());
    QCOMPARE(malformedDivider.error(),
             QStringLiteral("Invalid split-divider-color in Ghostty config output at line 1"));

    for (const QByteArray &key : {
             QByteArrayLiteral("split-inherit-working-directory"),
             QByteArrayLiteral("tab-inherit-working-directory"),
             QByteArrayLiteral("window-inherit-working-directory"),
             QByteArrayLiteral("window-inherit-font-size"),
             QByteArrayLiteral("maximize"),
         }) {
        for (const QByteArray &value : {
                 QByteArrayLiteral(""), QByteArrayLiteral("1"),
                 QByteArrayLiteral("yes"), QByteArrayLiteral("TRUE"),
             }) {
            const GhosttyConfigLoadResult malformedInheritance =
                parseGhosttyConfigShowOutputs(
                    defaultOutput(), key + QByteArrayLiteral(" = ")
                        + value + QByteArrayLiteral("\n"),
                    fixture.candidates());
            QVERIFY(!malformedInheritance.has_value());
            QCOMPARE(malformedInheritance.error(),
                     QStringLiteral("Invalid %1 in Ghostty config output at line 1")
                         .arg(QString::fromLatin1(key)));
        }
    }

    for (const QByteArray &value : {
             QByteArrayLiteral(""), QByteArrayLiteral("TRUE"),
             QByteArrayLiteral("native"),
             QByteArrayLiteral("non-native-visible"),
         }) {
        const GhosttyConfigLoadResult malformedFullscreen =
            parseGhosttyConfigShowOutputs(
                defaultOutput(),
                QByteArrayLiteral("fullscreen = ") + value
                    + QByteArrayLiteral("\n"),
                fixture.candidates());
        QVERIFY(!malformedFullscreen.has_value());
        QCOMPARE(malformedFullscreen.error(),
                 QStringLiteral("Invalid fullscreen in Ghostty config output at line 1"));
    }

    for (const QByteArray &value : {
             QByteArrayLiteral(""), QByteArrayLiteral("after"),
             QByteArrayLiteral("END"), QByteArrayLiteral("current-tab"),
         }) {
        const GhosttyConfigLoadResult malformedPosition =
            parseGhosttyConfigShowOutputs(
                defaultOutput(),
                QByteArrayLiteral("window-new-tab-position = ")
                    + value + QByteArrayLiteral("\n"),
                fixture.candidates());
        QVERIFY(!malformedPosition.has_value());
        QCOMPARE(malformedPosition.error(),
                 QStringLiteral("Invalid window-new-tab-position in Ghostty config output at line 1"));
    }

    for (const QByteArray &value : {
             QByteArrayLiteral(""), QByteArrayLiteral("true"),
             QByteArrayLiteral("AUTO"), QByteArrayLiteral("hidden"),
         }) {
        const GhosttyConfigLoadResult malformedVisibility =
            parseGhosttyConfigShowOutputs(
                defaultOutput(),
                QByteArrayLiteral("window-show-tab-bar = ")
                    + value + QByteArrayLiteral("\n"),
                fixture.candidates());
        QVERIFY(!malformedVisibility.has_value());
        QCOMPARE(malformedVisibility.error(),
                 QStringLiteral("Invalid window-show-tab-bar in Ghostty config output at line 1"));
    }

    for (const QByteArray &value : {
             QByteArrayLiteral(""), QByteArrayLiteral("true"),
             QByteArrayLiteral("false"),
             QByteArrayLiteral("Navigation"),
             QByteArrayLiteral("navigation,no-navigation"),
             QByteArrayLiteral("layout"),
         }) {
        const GhosttyConfigLoadResult malformedPreserveZoom =
            parseGhosttyConfigShowOutputs(
                defaultOutput(),
                QByteArrayLiteral("split-preserve-zoom = ")
                    + value + QByteArrayLiteral("\n"),
                fixture.candidates());
        QVERIFY(!malformedPreserveZoom.has_value());
        QCOMPARE(malformedPreserveZoom.error(),
                 QStringLiteral("Invalid split-preserve-zoom in Ghostty config output at line 1"));
    }

    const GhosttyConfigLoadResult malformedUnfocusedFill =
        parseGhosttyConfigShowOutputs(
            defaultOutput(),
            QByteArrayLiteral("unfocused-split-fill = not-a-color\n"),
            fixture.candidates());
    QVERIFY(!malformedUnfocusedFill.has_value());
    QCOMPARE(malformedUnfocusedFill.error(),
             QStringLiteral("Invalid unfocused-split-fill in Ghostty config output at line 1"));

    for (const QByteArray &value : {
             QByteArrayLiteral("not-a-number"), QByteArrayLiteral("nan"),
             QByteArrayLiteral("inf"), QByteArrayLiteral("-inf"),
             QByteArrayLiteral("0.149"), QByteArrayLiteral("1.001"),
         }) {
        const GhosttyConfigLoadResult malformedUnfocusedOpacity =
            parseGhosttyConfigShowOutputs(
                defaultOutput(),
                QByteArrayLiteral("unfocused-split-opacity = ")
                    + value + QByteArrayLiteral("\n"),
                fixture.candidates());
        QVERIFY(!malformedUnfocusedOpacity.has_value());
        QCOMPARE(malformedUnfocusedOpacity.error(),
                 QStringLiteral("Invalid unfocused-split-opacity in Ghostty config output at line 1"));
    }

    const GhosttyConfigLoadResult malformedPalette =
        parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("palette = 256=#abcdef\n"),
            fixture.candidates());
    QVERIFY(!malformedPalette.has_value());
    QCOMPARE(malformedPalette.error(),
             QStringLiteral("Invalid palette in Ghostty config output at line 1"));

    const GhosttyConfigLoadResult malformedSearch =
        parseGhosttyConfigShowOutputs(
            defaultOutput(),
            QByteArrayLiteral("search-background = not-an-alias\n"),
            fixture.candidates());
    QVERIFY(!malformedSearch.has_value());
    QCOMPARE(malformedSearch.error(),
             QStringLiteral("Invalid search-background in Ghostty config output at line 1"));

    const GhosttyConfigLoadResult emptySearch = parseGhosttyConfigShowOutputs(
        defaultOutput(), QByteArrayLiteral("search-foreground = \n"),
        fixture.candidates());
    QVERIFY(!emptySearch.has_value());
    QCOMPARE(emptySearch.error(),
             QStringLiteral("Invalid search-foreground in Ghostty config output at line 1"));

    const GhosttyConfigLoadResult malformedCursor =
        parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("cursor-style = beam\n"),
            fixture.candidates());
    QVERIFY(!malformedCursor.has_value());
    QCOMPARE(malformedCursor.error(),
             QStringLiteral("Invalid cursor-style in Ghostty config output at line 1"));

    // Ghostty accepts cursor opacity outside the nominal interval and clamps
    // it in the renderer rather than the config finalizer. Preserve that
    // canonical value here; LaunchOptions performs the renderer-side clamp.
    const GhosttyConfigLoadResult unboundedCursorOpacity =
        parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("cursor-opacity = 2\n"),
            fixture.candidates());
    QVERIFY(unboundedCursorOpacity.has_value());
    QCOMPARE(unboundedCursorOpacity->values
                 .value(QStringLiteral("cursor-opacity")).toDouble(),
             2.0);

    const GhosttyConfigLoadResult malformedOpacity =
        parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("faint-opacity = 1.1\n"),
            fixture.candidates());
    QVERIFY(!malformedOpacity.has_value());
    QCOMPARE(malformedOpacity.error(),
             QStringLiteral("Invalid faint-opacity in Ghostty config output at line 1"));

    const GhosttyConfigLoadResult malformedLinkUrl =
        parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("link-url = yes\n"),
            fixture.candidates());
    QVERIFY(!malformedLinkUrl.has_value());
    QCOMPARE(malformedLinkUrl.error(),
             QStringLiteral("Invalid link-url in Ghostty config output at line 1"));

    const GhosttyConfigLoadResult malformedLinkPreviews =
        parseGhosttyConfigShowOutputs(
            defaultOutput(), QByteArrayLiteral("link-previews = yes\n"),
            fixture.candidates());
    QVERIFY(!malformedLinkPreviews.has_value());
    QCOMPARE(malformedLinkPreviews.error(),
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
        {QByteArrayLiteral("mouse-reporting = yes\n"),
         QStringLiteral("Invalid mouse-reporting in Ghostty config output at line 1")},
    };
    for (const auto &testCase : malformedClipboard) {
        const GhosttyConfigLoadResult result = parseGhosttyConfigShowOutputs(
            defaultOutput(), testCase.setting, fixture.candidates());
        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), testCase.error);
    }

    const GhosttyConfigLoadResult missing = parseGhosttyConfigShowOutputs(
        QByteArrayLiteral("font-size = 13\n"), {}, fixture.candidates());
    QVERIFY(!missing.has_value());
    QCOMPARE(missing.error(),
             QStringLiteral("Ghostty default config output is missing a required compatibility key"));

    const GhosttyConfigLoadResult maximum = parseGhosttyConfigShowOutputs(
        defaultOutput(),
        QByteArrayLiteral("scrollback-limit = 18446744073709551615\n"),
        fixture.candidates());
    QVERIFY(maximum.has_value());
    QCOMPARE(maximum->values.value(QStringLiteral("scrollback-limit"))
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
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.value(QStringLiteral("font-size")).toDouble(),
             17.25);
    QVERIFY(result->keybindConfig.has_value());
    QCOMPARE(result->keybindConfig->root.size(), 1);
    QCOMPARE(result->keybindConfig->root.constFirst().actions,
             QStringList({QStringLiteral("new_tab")}));
    QVERIFY(result->values.value(
                QStringLiteral("quit-after-last-window-closed")).toBool());
    QVERIFY(result->values.contains(
        QStringLiteral("quit-after-last-window-closed-delay")));
    QVERIFY(!result->values.value(
                 QStringLiteral("quit-after-last-window-closed-delay"))
                 .isValid());
    QVERIFY(result->values.value(QStringLiteral("initial-window")).toBool());

    QFile log(invocationLog);
    QVERIFY(log.open(QIODevice::ReadOnly));
    QCOMPARE(log.readAll(),
             QByteArrayLiteral("+validate-config\n"
                               "+show-config --default\n"
                               "+show-config\n"
                               "+show-config-json\n"
                               "+validate-config\n"
                               "+show-config\n"
                               "+show-config-json\n"));
}

void GhosttyConfigProcessLoaderTest::loadsStructuredApplicationLifetimeValues()
{
    ConfigFixture fixture;
    GhosttyConfigProcessLoaderOptions options = fakeOptions(fixture);
    options.environment.insert(
        QStringLiteral("GHOSTTY_QT_FAKE_CONFIG_JSON"),
        QStringLiteral(
            R"json({"version":3,"application":{"quit-after-last-window-closed":false,"quit-after-last-window-closed-delay-ms":1500,"initial-window":false,"gtk-single-instance":"false"},"keybindings":{"root":[],"tables":[]}})json"));

    GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(!result->values.value(
                 QStringLiteral("quit-after-last-window-closed")).toBool());
    const QVariant delay = result->values.value(
        QStringLiteral("quit-after-last-window-closed-delay"));
    QCOMPARE(delay.metaType(), QMetaType::fromType<quint32>());
    QCOMPARE(delay.value<quint32>(), quint32(1'500));
    QVERIFY(!result->values.value(QStringLiteral("initial-window")).toBool());
    QCOMPARE(result->values.value(
                 QStringLiteral("gtk-single-instance")).toString(),
             QStringLiteral("false"));

    options.environment.insert(
        QStringLiteral("GHOSTTY_QT_FAKE_CONFIG_JSON"),
        QStringLiteral(
            R"json({"version":3,"application":{"quit-after-last-window-closed":true,"quit-after-last-window-closed-delay-ms":null,"initial-window":true,"gtk-single-instance":"true"},"keybindings":{"root":[],"tables":[]}})json"));
    result = makeGhosttyConfigProcessLoader(options)(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->values.value(
                QStringLiteral("quit-after-last-window-closed")).toBool());
    QVERIFY(result->values.contains(
        QStringLiteral("quit-after-last-window-closed-delay")));
    QVERIFY(!result->values.value(
                 QStringLiteral("quit-after-last-window-closed-delay"))
                 .isValid());
    QVERIFY(result->values.value(QStringLiteral("initial-window")).toBool());
    QCOMPARE(result->values.value(
                 QStringLiteral("gtk-single-instance")).toString(),
             QStringLiteral("true"));
}

void GhosttyConfigProcessLoaderTest::rejectsFailedOrMalformedStructuredQuery()
{
    ConfigFixture fixture;
    GhosttyConfigLoadResult result = makeGhosttyConfigProcessLoader(
        fakeOptions(fixture, QStringLiteral("structured-query-failure")))(
        fixture.candidates());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(),
             QStringLiteral(
                 "Ghostty config helper failed during structured config query with exit code 8: "
                 "stderr: structured config query failed"));

    result = makeGhosttyConfigProcessLoader(
        fakeOptions(fixture, QStringLiteral("structured-query-malformed")))(
        fixture.candidates());
    QVERIFY(!result.has_value());
    QVERIFY(result.error().startsWith(
        QStringLiteral(
            "Ghostty structured config query returned malformed data: "
            "Invalid Ghostty structured config JSON at offset ")));
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
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(),
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
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(),
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
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->diagnostics.size(), 1);
    QCOMPARE(result->diagnostics.constFirst().severity,
             GhosttyConfigDiagnosticSeverity::Warning);
    QCOMPARE(result->diagnostics.constFirst().message,
             QStringLiteral(
                 "Ghostty config helper current query: both standard files exist"));
}

void GhosttyConfigProcessLoaderTest::realHelperFinalizesSurfaceInheritance()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }

    ConfigFixture fixture;
    const QString configuredDirectory = QDir(fixture.temporary.path())
        .filePath(QStringLiteral("working directory"));
    QVERIFY(QDir().mkpath(configuredDirectory));
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QStringLiteral("working-directory = %1\n"
                       "split-inherit-working-directory = false\n"
                       "split-preserve-zoom = navigation\n"
                       "tab-inherit-working-directory = false\n"
                       "window-inherit-working-directory = false\n"
                       "window-inherit-font-size = false\n"
                       "window-new-tab-position = end\n"
                       "window-show-tab-bar = never\n"
                       "maximize = true\n"
                       "fullscreen = non-native-visible-menu\n")
            .arg(configuredDirectory)
            .toUtf8());

    const auto load = makeGhosttyConfigProcessLoader({
        .helperPath = helperPath,
        .timeoutMilliseconds = 10'000,
        .environment = QProcessEnvironment::systemEnvironment(),
    });
    GhosttyConfigLoadResult result = load(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.value(QStringLiteral("working-directory"))
                 .toString(),
             configuredDirectory);
    QVERIFY(!result->values.value(
                 QStringLiteral("split-inherit-working-directory")).toBool());
    QVERIFY(result->values.value(
                QStringLiteral("split-preserve-zoom")).toBool());
    QVERIFY(!result->values.value(
                 QStringLiteral("tab-inherit-working-directory")).toBool());
    QVERIFY(!result->values.value(
                 QStringLiteral("window-inherit-working-directory")).toBool());
    QVERIFY(!result->values.value(
                 QStringLiteral("window-inherit-font-size")).toBool());
    QCOMPARE(result->values.value(
                 QStringLiteral("window-new-tab-position")).toString(),
             QStringLiteral("end"));
    QCOMPARE(result->values.value(
                 QStringLiteral("window-show-tab-bar")).toString(),
             QStringLiteral("never"));
    QVERIFY(result->values.value(QStringLiteral("maximize")).toBool());
    QCOMPARE(result->values.value(QStringLiteral("fullscreen")).toString(),
             QStringLiteral("non-native-visible-menu"));

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("working-directory = inherit\n"
                          "split-inherit-working-directory = true\n"
                          "split-preserve-zoom = no-navigation\n"
                          "tab-inherit-working-directory = true\n"
                          "window-inherit-working-directory = true\n"
                          "window-inherit-font-size = true\n"
                          "window-new-tab-position = current\n"
                          "window-show-tab-bar = always\n"
                          "maximize = false\n"
                          "fullscreen = false\n"));
    result = load(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.value(QStringLiteral("working-directory"))
                 .toString(),
             QStringLiteral("inherit"));
    QVERIFY(result->values.value(
                QStringLiteral("split-inherit-working-directory")).toBool());
    QVERIFY(!result->values.value(
                 QStringLiteral("split-preserve-zoom")).toBool());
    QVERIFY(result->values.value(
                QStringLiteral("tab-inherit-working-directory")).toBool());
    QVERIFY(result->values.value(
                QStringLiteral("window-inherit-working-directory")).toBool());
    QVERIFY(result->values.value(
                QStringLiteral("window-inherit-font-size")).toBool());
    QCOMPARE(result->values.value(
                 QStringLiteral("window-new-tab-position")).toString(),
             QStringLiteral("current"));
    QCOMPARE(result->values.value(
                 QStringLiteral("window-show-tab-bar")).toString(),
             QStringLiteral("always"));
    QVERIFY(!result->values.value(QStringLiteral("maximize")).toBool());
    QCOMPARE(result->values.value(QStringLiteral("fullscreen")).toString(),
             QStringLiteral("false"));

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("working-directory = home\n"));
    result = load(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.value(QStringLiteral("working-directory"))
                 .toString(),
             QDir::homePath());

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral("working-directory = ~/ghostty-qt-test\n"));
    result = load(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.value(QStringLiteral("working-directory"))
                 .toString(),
             QDir(QDir::homePath())
                 .filePath(QStringLiteral("ghostty-qt-test")));
}

void GhosttyConfigProcessLoaderTest::realHelperFinalizesUnfocusedSplitAppearance_data()
{
    QTest::addColumn<QString>("sourceFill");
    QTest::addColumn<double>("sourceOpacity");
    QTest::addColumn<double>("expectedOpacity");
    QTest::addColumn<QColor>("expectedFill");

    QTest::newRow("hash-rgb-and-low-clamp")
        << QStringLiteral("#123456") << -1.0 << 0.15
        << QColor(QStringLiteral("#123456"));
    QTest::newRow("bare-rgb-and-high-clamp")
        << QStringLiteral("123456") << 2.0 << 1.0
        << QColor(QStringLiteral("#123456"));
    QTest::newRow("x11-and-in-range")
        << QStringLiteral("AliceBlue") << 0.42 << 0.42
        << QColor(QStringLiteral("#f0f8ff"));
    QTest::newRow("nullable-default")
        << QString{} << 0.7 << 0.7 << QColor{};
}

void GhosttyConfigProcessLoaderTest::realHelperFinalizesUnfocusedSplitAppearance()
{
    const QString helperPath =
        QString::fromUtf8(GHOSTTY_QT_REAL_CONFIG_HELPER_PATH);
    if (helperPath.isEmpty()) {
        QSKIP("The pinned Ghostty config helper is disabled");
    }
    QFETCH(QString, sourceFill);
    QFETCH(double, sourceOpacity);
    QFETCH(double, expectedOpacity);
    QFETCH(QColor, expectedFill);

    ConfigFixture fixture;
    ConfigFixture::writeFile(fixture.legacyPath, {});
    ConfigFixture::writeFile(
        fixture.preferredPath,
        QStringLiteral("unfocused-split-opacity = %1\n"
                       "unfocused-split-fill = %2\n")
            .arg(sourceOpacity, 0, 'g', 17)
            .arg(sourceFill)
            .toUtf8());

    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader({
            .helperPath = helperPath,
            .timeoutMilliseconds = 10'000,
            .environment = QProcessEnvironment::systemEnvironment(),
        })(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QCOMPARE(result->values.value(
                 QStringLiteral("unfocused-split-opacity")).toDouble(),
             expectedOpacity);
    const QVariant fill = result->values.value(
        QStringLiteral("unfocused-split-fill"));
    if (expectedFill.isValid()) {
        QCOMPARE(fill.value<QColor>(), expectedFill);
    } else {
        QCOMPARE(fill, QVariant(QString{}));
    }
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
            "split-divider-color = AliceBlue\n"
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
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    const QStringList keybinds =
        result->values.value(QStringLiteral("keybind")).toStringList();
    QCOMPARE(keybinds, QStringList({QStringLiteral("ctrl+b=new_tab")}));
    const QVariantList palette =
        result->values.value(QStringLiteral("palette")).toList();
    QCOMPARE(palette.size(), 256);
    QCOMPARE(palette.at(42).value<QColor>(), QColor(QStringLiteral("#123456")));
    QCOMPARE(result->values.value(QStringLiteral("split-divider-color"))
                 .value<QColor>(),
             QColor(QStringLiteral("#f0f8ff")));
    QCOMPARE(result->values.value(
                 QStringLiteral("selection-foreground")).toString(),
             QStringLiteral("cell-background"));
    QCOMPARE(result->values.value(
                 QStringLiteral("selection-background")).value<QColor>(),
             QColor(QStringLiteral("#334455")));
    QCOMPARE(result->values.value(QStringLiteral("search-foreground"))
                 .toString(),
             QStringLiteral("cell-background"));
    QCOMPARE(result->values.value(QStringLiteral("search-background"))
                 .value<QColor>(),
             QColor(QStringLiteral("#123456")));
    QCOMPARE(result->values.value(
                 QStringLiteral("search-selected-foreground")).toString(),
             QStringLiteral("cell-foreground"));
    QCOMPARE(result->values.value(
                 QStringLiteral("search-selected-background")).value<QColor>(),
             QColor(QStringLiteral("#654321")));
    QCOMPARE(result->values.value(QStringLiteral("cursor-color"))
                 .value<QColor>(),
             QColor(QStringLiteral("#abcdef")));
    QCOMPARE(result->values.value(QStringLiteral("cursor-opacity"))
                 .toDouble(),
             0.4);
    QCOMPARE(result->values.value(QStringLiteral("cursor-style"))
                 .toString(),
             QStringLiteral("block_hollow"));
    QCOMPARE(result->values.value(QStringLiteral("cursor-style-blink"))
                 .toBool(),
             false);
    QCOMPARE(result->values.value(QStringLiteral("cursor-text"))
                 .toString(),
             QStringLiteral("cell-foreground"));
    QCOMPARE(result->values.value(QStringLiteral("bold-color"))
                 .toString(),
             QStringLiteral("bright"));
    QCOMPARE(result->values.value(QStringLiteral("faint-opacity"))
                 .toDouble(),
             0.25);
    QVERIFY(!result->values.value(
        QStringLiteral("clipboard-trim-trailing-spaces")).toBool());
    QVERIFY(!result->values.value(
        QStringLiteral("clipboard-paste-protection")).toBool());
    QVERIFY(result->values.value(
        QStringLiteral("clipboard-paste-bracketed-safe")).toBool());
    QCOMPARE(result->values.value(QStringLiteral("copy-on-select"))
                 .toString(),
             QStringLiteral("clipboard"));
    QVERIFY(!result->values.value(
        QStringLiteral("selection-clear-on-typing")).toBool());
    QVERIFY(result->values.value(
        QStringLiteral("selection-clear-on-copy")).toBool());
    QCOMPARE(result->values.value(QStringLiteral("middle-click-action"))
                 .toString(),
             QStringLiteral("ignore"));
}

void GhosttyConfigProcessLoaderTest::realHelperExportsApplicationLifetime()
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
            "gtk-single-instance = false\n"
            "initial-window = false\n"
            "quit-after-last-window-closed = false\n"
            "quit-after-last-window-closed-delay = 1s 250ms 999us\n"));
    auto result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    GhosttyConfigExport exported = *result;
    QVERIFY(!exported.quitAfterLastWindowClosed);
    QCOMPARE(exported.quitAfterLastWindowClosedDelayMilliseconds,
             std::optional<quint32>(1'250));
    QVERIFY(!exported.initialWindow);
    QCOMPARE(exported.singleInstanceMode, QStringLiteral("false"));

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral(
            "gtk-single-instance = true\n"
            "initial-window = true\n"
            "quit-after-last-window-closed-delay = 0\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    exported = *result;
    QVERIFY(exported.quitAfterLastWindowClosed);
    QCOMPARE(exported.quitAfterLastWindowClosedDelayMilliseconds,
             std::optional<quint32>(0));
    QVERIFY(exported.initialWindow);
    QCOMPARE(exported.singleInstanceMode, QStringLiteral("true"));

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral(
            "quit-after-last-window-closed-delay = 999us\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    exported = *result;
    QCOMPARE(exported.quitAfterLastWindowClosedDelayMilliseconds,
             std::optional<quint32>(0));

    ConfigFixture::writeFile(
        fixture.preferredPath,
        QByteArrayLiteral(
            "quit-after-last-window-closed-delay = 584y 49w 23h 34m 33s 709ms 551us 615ns\n"));
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    exported = *result;
    QCOMPARE(exported.quitAfterLastWindowClosedDelayMilliseconds,
             std::optional<quint32>(std::numeric_limits<quint32>::max()));

    ConfigFixture::writeFile(fixture.preferredPath, {});
    result = queryRealConfigExport(helperPath, fixture);
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    exported = *result;
    QVERIFY(exported.quitAfterLastWindowClosed);
    QVERIFY(!exported.quitAfterLastWindowClosedDelayMilliseconds.has_value());
    QVERIFY(exported.initialWindow);
    QCOMPARE(exported.singleInstanceMode, QStringLiteral("detect"));
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
        QStringLiteral(
            "keybind = clear\n"
            "keybind = unconsumed:performable:ctrl+x>key_y=new_tab\n"
            "keybind = chain=goto_split:left\n"
            "keybind = catch_all=ignore\n"
            "keybind = all:ctrl+g=new_tab\n"
            "keybind = global:ctrl+j=new_tab\n"
            "keybind = ctrl+m=activate_key_table:modeé\n"
            "keybind = ctrl+p=prompt_tab_title\n"
            "keybind = ctrl+u=prompt_surface_title\n"
            "keybind = ctrl+i=copy_title_to_clipboard\n"
            "keybind = ctrl+s=set_surface_title:🌐 surface:detail\n"
            "keybind = ctrl+t=set_tab_title:👻 main:detail\n"
            "keybind = ctrl+v=close_tab:other\n"
            "keybind = ctrl+w=close_tab:right\n"
            "keybind = resize/ctrl+h=resize_split:left,10\n"
            "keybind = modeé/ctrl+h=resize_split:right,10\n").toUtf8());

    GhosttyConfigProcessLoaderOptions options{
        .helperPath = helperPath,
        .timeoutMilliseconds = 10'000,
        .environment = QProcessEnvironment::systemEnvironment(),
    };
    const GhosttyConfigLoadResult result =
        makeGhosttyConfigProcessLoader(options)(fixture.candidates());
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->keybindConfig.has_value());
    const GhosttyKeybindConfig &config = *result->keybindConfig;
    QCOMPARE(config.schemaVersion,
             GhosttyKeybindConfig::CurrentSchemaVersion);
    QCOMPARE(config.root.size(), 12);
    QCOMPARE(config.tables.size(), 2);

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

    const auto activation = std::ranges::find(
        config.root,
        QStringList({QStringLiteral(
            R"(activate_key_table:mode\xc3\xa9)")}),
        &GhosttyKeybindDefinition::actions);
    QVERIFY(activation != config.root.cend());

    const auto surfaceTitle = std::ranges::find(
        config.root,
        QStringList({QStringLiteral(
            R"(set_surface_title:\xf0\x9f\x8c\x90 surface:detail)")}),
        &GhosttyKeybindDefinition::actions);
    QVERIFY(surfaceTitle != config.root.cend());

    const auto surfaceTitlePrompt = std::ranges::find(
        config.root,
        QStringList({QStringLiteral("prompt_surface_title")}),
        &GhosttyKeybindDefinition::actions);
    QVERIFY(surfaceTitlePrompt != config.root.cend());

    const auto titleCopy = std::ranges::find(
        config.root,
        QStringList({QStringLiteral("copy_title_to_clipboard")}),
        &GhosttyKeybindDefinition::actions);
    QVERIFY(titleCopy != config.root.cend());

    const auto tabTitle = std::ranges::find(
        config.root,
        QStringList({QStringLiteral(
            R"(set_tab_title:\xf0\x9f\x91\xbb main:detail)")}),
        &GhosttyKeybindDefinition::actions);
    QVERIFY(tabTitle != config.root.cend());

    const auto tabTitlePrompt = std::ranges::find(
        config.root,
        QStringList({QStringLiteral("prompt_tab_title")}),
        &GhosttyKeybindDefinition::actions);
    QVERIFY(tabTitlePrompt != config.root.cend());

    const auto closeOtherTabs = std::ranges::find(
        config.root,
        QStringList({QStringLiteral("close_tab:other")}),
        &GhosttyKeybindDefinition::actions);
    const auto closeTabsRight = std::ranges::find(
        config.root,
        QStringList({QStringLiteral("close_tab:right")}),
        &GhosttyKeybindDefinition::actions);
    QVERIFY(closeOtherTabs != config.root.cend());
    QVERIFY(closeTabsRight != config.root.cend());
    const auto falseCloseDiagnostic = std::ranges::find_if(
        result->diagnostics,
        [](const GhosttyConfigDiagnostic &diagnostic) {
            return diagnostic.message.contains(
                       QStringLiteral("close_tab:other"))
                || diagnostic.message.contains(
                       QStringLiteral("close_tab:right"));
        });
    QVERIFY2(falseCloseDiagnostic == result->diagnostics.cend(),
             falseCloseDiagnostic == result->diagnostics.cend()
                 ? ""
                 : qPrintable(falseCloseDiagnostic->message));

    const auto resize = std::ranges::find(
        config.tables, QStringLiteral("resize"), &GhosttyKeybindTable::name);
    QVERIFY(resize != config.tables.cend());
    QCOMPARE(resize->bindings.size(), 1);
    QCOMPARE(resize->bindings.constFirst().actions,
             QStringList({QStringLiteral("resize_split:left,10")}));

    const auto unicode = std::ranges::find(
        config.tables, QStringLiteral("modeé"), &GhosttyKeybindTable::name);
    QVERIFY(unicode != config.tables.cend());
    QCOMPARE(unicode->bindings.size(), 1);
    QCOMPARE(unicode->bindings.constFirst().actions,
             QStringList({QStringLiteral("resize_split:right,10")}));
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
    QVERIFY2(result.has_value(), qPrintable(errorMessage(result)));
    QVERIFY(result->keybindConfig.has_value());
    const GhosttyKeybindConfig &keybinds = *result->keybindConfig;
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
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(),
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
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(),
             QStringLiteral(
                 "Ghostty config helper timed out during validation after 25 ms"));

    GhosttyConfigProcessLoaderOptions crash =
        fakeOptions(fixture, QStringLiteral("validation-crash"));
    result = makeGhosttyConfigProcessLoader(crash)(fixture.candidates());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(),
             QStringLiteral("Ghostty config helper crashed during validation"));

    GhosttyConfigProcessLoaderOptions missing = fakeOptions(fixture);
    missing.helperPath = QDir(fixture.temporary.path())
                             .filePath(QStringLiteral("does-not-exist"));
    missing.timeoutMilliseconds = 100;
    result = makeGhosttyConfigProcessLoader(missing)(fixture.candidates());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(),
             QStringLiteral(
                 "Ghostty config helper could not be started during validation"));
}

QTEST_GUILESS_MAIN(GhosttyConfigProcessLoaderTest)

#include "test_ghostty_config_process_loader.moc"
