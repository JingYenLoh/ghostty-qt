#include "ghostty_config_export.h"

#include "ghostty_config_export_fixture.h"

#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>
#include <QTest>

#include <expected>
#include <limits>

namespace {

using namespace GhosttyConfigExportFixture;

template<typename Value>
QString errorMessage(const std::expected<Value, QString> &result)
{
    return result ? QString{} : result.error();
}

QJsonObject withValue(QJsonObject root,
                      const QString &name,
                      const QJsonValue &value)
{
    QJsonObject configValues = root.value(QStringLiteral("values")).toObject();
    configValues.insert(name, value);
    root.insert(QStringLiteral("values"), configValues);
    return root;
}

QJsonObject withoutValue(QJsonObject root, const QString &name)
{
    QJsonObject configValues = root.value(QStringLiteral("values")).toObject();
    configValues.remove(name);
    root.insert(QStringLiteral("values"), configValues);
    return root;
}

} // namespace

class GhosttyConfigExportTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void parsesEveryValueWithExactTypes();
    void parsesStructuredBindingSets();
    void rejectsMalformedEnvelope();
    void rejectsInvalidValues_data();
    void rejectsInvalidValues();
    void rejectsInvalidBindings();
};

void GhosttyConfigExportTest::parsesEveryValueWithExactTypes()
{
    const auto parsed = parseGhosttyConfigExportJson(json());
    QVERIFY2(parsed.has_value(), qPrintable(errorMessage(parsed)));
    QCOMPARE(parsed->values.size(), 53);

    const auto requireType = [&parsed]<typename Value>(const QString &key) {
        const auto found = parsed->values.constFind(key);
        QVERIFY2(found != parsed->values.cend(), qPrintable(key));
        QCOMPARE(found->metaType(), QMetaType::fromType<Value>());
    };

    for (const QString &key : {
             QStringLiteral("working-directory"),
             QStringLiteral("window-new-tab-position"),
             QStringLiteral("window-show-tab-bar"),
             QStringLiteral("fullscreen"),
             QStringLiteral("cursor-style"),
             QStringLiteral("confirm-close-surface"),
             QStringLiteral("copy-on-select"),
             QStringLiteral("middle-click-action"),
             QStringLiteral("link-previews"),
             QStringLiteral("resize-overlay"),
             QStringLiteral("resize-overlay-position"),
             QStringLiteral("gtk-single-instance"),
         }) {
        requireType.template operator()<QString>(key);
    }
    for (const QString &key : {
             QStringLiteral("font-family"),
             QStringLiteral("config-file"),
         }) {
        requireType.template operator()<QStringList>(key);
    }
    for (const QString &key : {
             QStringLiteral("font-size"),
             QStringLiteral("unfocused-split-opacity"),
             QStringLiteral("cursor-opacity"),
             QStringLiteral("faint-opacity"),
         }) {
        requireType.template operator()<double>(key);
    }
    for (const QString &key : {
             QStringLiteral("window-width"),
             QStringLiteral("window-height"),
             QStringLiteral("resize-overlay-duration"),
         }) {
        requireType.template operator()<quint32>(key);
    }
    for (const QString &key : {
             QStringLiteral("foreground"),
             QStringLiteral("background"),
             QStringLiteral("split-divider-color"),
             QStringLiteral("search-foreground"),
             QStringLiteral("search-selected-background"),
             QStringLiteral("cursor-color"),
         }) {
        requireType.template operator()<QColor>(key);
    }
    for (const QString &key : {
             QStringLiteral("split-inherit-working-directory"),
             QStringLiteral("split-preserve-zoom"),
             QStringLiteral("tab-inherit-working-directory"),
             QStringLiteral("window-inherit-working-directory"),
             QStringLiteral("window-inherit-font-size"),
             QStringLiteral("maximize"),
             QStringLiteral("clipboard-trim-trailing-spaces"),
             QStringLiteral("clipboard-paste-protection"),
             QStringLiteral("clipboard-paste-bracketed-safe"),
             QStringLiteral("selection-clear-on-typing"),
             QStringLiteral("selection-clear-on-copy"),
             QStringLiteral("mouse-reporting"),
             QStringLiteral("link-url"),
             QStringLiteral("quit-after-last-window-closed"),
             QStringLiteral("initial-window"),
         }) {
        requireType.template operator()<bool>(key);
    }

    requireType.template operator()<QVariantList>(QStringLiteral("palette"));
    requireType.template operator()<quint64>(QStringLiteral("scrollback-limit"));
    QCOMPARE(parsed->values.value(QStringLiteral("scrollback-limit"))
                 .value<quint64>(),
             std::numeric_limits<quint64>::max());
    const QVariantList palette =
        parsed->values.value(QStringLiteral("palette")).toList();
    QCOMPARE(palette.size(), 256);
    QCOMPARE(palette.constFirst().value<QColor>(), QColor(0, 0, 0));
    QCOMPARE(palette.constLast().value<QColor>(), QColor(255, 255, 255));
    QCOMPARE(parsed->values.value(QStringLiteral("font-family")).toStringList(),
             QStringList({QStringLiteral("Mono One"),
                          QStringLiteral("Emoji")}));
    QCOMPARE(parsed->values.value(QStringLiteral("selection-background"))
                 .toString(),
             QStringLiteral("cell-foreground"));
    QCOMPARE(parsed->values.value(QStringLiteral("bold-color")).toString(),
             QStringLiteral("bright"));
    QCOMPARE(parsed->values.value(QStringLiteral("unfocused-split-fill"))
                 .metaType(),
             QMetaType::fromType<QString>());
    QVERIFY(parsed->values.value(QStringLiteral("unfocused-split-fill"))
                .toString()
                .isEmpty());
    QVERIFY(parsed->values.contains(
        QStringLiteral("quit-after-last-window-closed-delay")));
    QVERIFY(!parsed->values.value(
        QStringLiteral("quit-after-last-window-closed-delay")).isValid());
}

void GhosttyConfigExportTest::parsesStructuredBindingSets()
{
    QJsonObject exportObject = object();
    const QJsonObject current{
        {QStringLiteral("root"),
         QJsonArray{
             binding({physicalTrigger(QStringLiteral("key_a"), 3),
                      unicodeTrigger(128578, 4), catchAllTrigger()},
                     {QStringLiteral("new_tab"), QStringLiteral("goto_tab:2")},
                     flags(false, false, false, true)),
             binding({unicodeTrigger('x')},
                     {QStringLiteral("ignore")},
                     flags(true, true)),
         }},
        {QStringLiteral("tables"),
         QJsonArray{QJsonObject{
             {QStringLiteral("name"), QStringLiteral("modeé")},
             {QStringLiteral("bindings"),
              QJsonArray{binding(
                  {unicodeTrigger('h', GhosttyKeybindCtrl)},
                  {QStringLiteral("resize_split:left,10")})}},
         }}},
    };
    exportObject.insert(QStringLiteral("keybindings"), current);

    const auto parsed = parseGhosttyConfigExportJson(json(exportObject));
    QVERIFY2(parsed.has_value(), qPrintable(errorMessage(parsed)));
    QCOMPARE(parsed->keybindings.root.size(), 2);
    const GhosttyKeybindDefinition &root = parsed->keybindings.root.constFirst();
    QCOMPARE(root.sequence.size(), 3);
    QCOMPARE(root.sequence.at(0).kind, GhosttyKeybindKeyKind::Physical);
    QCOMPARE(root.sequence.at(0).physicalName, QStringLiteral("key_a"));
    QCOMPARE(root.sequence.at(1).unicodeCodepoint, quint32(128578));
    QCOMPARE(root.sequence.at(2).kind, GhosttyKeybindKeyKind::CatchAll);
    QCOMPARE(root.actions,
             QStringList({QStringLiteral("new_tab"),
                          QStringLiteral("goto_tab:2")}));
    QVERIFY(!root.flags.consumed);
    QVERIFY(!root.flags.all);
    QVERIFY(root.flags.performable);
    QVERIFY(parsed->keybindings.root.at(1).flags.all);
    QCOMPARE(parsed->keybindings.tables.size(), 1);
    QCOMPARE(parsed->keybindings.tables.constFirst().name,
             QStringLiteral("modeé"));
    QCOMPARE(parsed->defaultKeybindings.root.size(), 1);
    QCOMPARE(parsed->defaultKeybindings.root.constFirst().actions,
             QStringList({QStringLiteral("toggle_command_palette")}));
}

void GhosttyConfigExportTest::rejectsMalformedEnvelope()
{
    auto parsed = parseGhosttyConfigExportJson(QByteArrayLiteral("{not-json"));
    QVERIFY(!parsed);
    QVERIFY(parsed.error().startsWith(
        QStringLiteral("Invalid Ghostty structured config JSON at offset ")));

    parsed = parseGhosttyConfigExportJson(QByteArrayLiteral("[]"));
    QVERIFY(!parsed);
    QCOMPARE(parsed.error(),
             QStringLiteral(
                 "Ghostty structured config JSON root must be an object"));

    QJsonObject malformed = object();
    malformed.insert(QStringLiteral("version"), 2);
    parsed = parseGhosttyConfigExportJson(json(malformed));
    QVERIFY(!parsed);
    QCOMPARE(parsed.error(), QStringLiteral(
        "Unsupported Ghostty structured config JSON schema version"));

    malformed = object();
    malformed.remove(QStringLiteral("values"));
    malformed.insert(QStringLiteral("application"), QJsonObject{});
    parsed = parseGhosttyConfigExportJson(json(malformed));
    QVERIFY(!parsed);
    QVERIFY(parsed.error().contains(QStringLiteral("unexpected field 'application'")));

    malformed = object();
    malformed.insert(QStringLiteral("future"), true);
    parsed = parseGhosttyConfigExportJson(json(malformed));
    QVERIFY(!parsed);
    QVERIFY(parsed.error().contains(QStringLiteral("unexpected field 'future'")));
}

void GhosttyConfigExportTest::rejectsInvalidValues_data()
{
    QTest::addColumn<QJsonObject>("exportObject");
    QTest::addColumn<QString>("diagnostic");

    QTest::newRow("missing-field")
        << withoutValue(object(), QStringLiteral("font-size"))
        << QStringLiteral("values is missing field 'font-size'");

    QJsonObject extra = object();
    QJsonObject extraValues = extra.value(QStringLiteral("values")).toObject();
    extraValues.insert(QStringLiteral("future"), true);
    extra.insert(QStringLiteral("values"), extraValues);
    QTest::newRow("extra-field") << extra
                                  << QStringLiteral("unexpected field 'future'");

    QTest::newRow("font-size-type")
        << withValue(object(), QStringLiteral("font-size"), true)
        << QStringLiteral("values.font-size must be a finite number");
    QTest::newRow("split-opacity-range")
        << withValue(object(), QStringLiteral("unfocused-split-opacity"), 0.1)
        << QStringLiteral("values.unfocused-split-opacity is outside");
    QTest::newRow("faint-opacity-range")
        << withValue(object(), QStringLiteral("faint-opacity"), 1.1)
        << QStringLiteral("values.faint-opacity is outside");
    QTest::newRow("canonical-color")
        << withValue(object(), QStringLiteral("foreground"),
                     QStringLiteral("#AABBCC"))
        << QStringLiteral("values.foreground must be a canonical #rrggbb");
    QTest::newRow("nullable-color")
        << withValue(object(), QStringLiteral("split-divider-color"), 7)
        << QStringLiteral("values.split-divider-color must be a string");
    QTest::newRow("required-terminal-color-null")
        << withValue(object(), QStringLiteral("search-foreground"),
                     QJsonValue::Null)
        << QStringLiteral("values.search-foreground must not be null");
    QTest::newRow("terminal-color-sentinel")
        << withValue(object(), QStringLiteral("cursor-color"),
                     QStringLiteral("window-foreground"))
        << QStringLiteral("values.cursor-color must be a canonical #rrggbb");
    QTest::newRow("cursor-blink-type")
        << withValue(object(), QStringLiteral("cursor-style-blink"), 1)
        << QStringLiteral("values.cursor-style-blink must be a boolean");
    QTest::newRow("bold-color")
        << withValue(object(), QStringLiteral("bold-color"),
                     QStringLiteral("dim"))
        << QStringLiteral("values.bold-color must be a canonical #rrggbb");
    QTest::newRow("uint-negative")
        << withValue(object(), QStringLiteral("window-width"), -1)
        << QStringLiteral("values.window-width must be an unsigned integer");
    QTest::newRow("uint-fraction")
        << withValue(object(), QStringLiteral("window-height"), 4.5)
        << QStringLiteral("values.window-height must be an unsigned integer");
    QTest::newRow("uint-overflow")
        << withValue(object(), QStringLiteral("resize-overlay-duration"),
                     4294967296.0)
        << QStringLiteral("values.resize-overlay-duration must be an unsigned integer");
    QTest::newRow("scrollback-number")
        << withValue(object(), QStringLiteral("scrollback-limit"), 50000000)
        << QStringLiteral("values.scrollback-limit must be a string");
    QTest::newRow("scrollback-leading-zero")
        << withValue(object(), QStringLiteral("scrollback-limit"),
                     QStringLiteral("01"))
        << QStringLiteral("canonical unsigned decimal string");
    QTest::newRow("scrollback-overflow")
        << withValue(object(), QStringLiteral("scrollback-limit"),
                     QStringLiteral("18446744073709551616"))
        << QStringLiteral("exceeds the uint64 range");
    QTest::newRow("delay-negative")
        << withValue(object(),
                     QStringLiteral("quit-after-last-window-closed-delay"), -1)
        << QStringLiteral("must be an unsigned integer");
    QTest::newRow("font-family-member")
        << withValue(object(), QStringLiteral("font-family"), QJsonArray{true})
        << QStringLiteral("values.font-family[0] must be a string");

    QJsonArray shortPalette = values().value(QStringLiteral("palette")).toArray();
    shortPalette.removeLast();
    QTest::newRow("palette-length")
        << withValue(object(), QStringLiteral("palette"), shortPalette)
        << QStringLiteral("must contain exactly 256 colors");
    QJsonArray badPalette = values().value(QStringLiteral("palette")).toArray();
    badPalette.replace(42, QStringLiteral("#xyzxyz"));
    QTest::newRow("palette-color")
        << withValue(object(), QStringLiteral("palette"), badPalette)
        << QStringLiteral("values.palette[42]");

    for (const QString &enumName : {
             QStringLiteral("window-new-tab-position"),
             QStringLiteral("window-show-tab-bar"),
             QStringLiteral("fullscreen"),
             QStringLiteral("cursor-style"),
             QStringLiteral("confirm-close-surface"),
             QStringLiteral("copy-on-select"),
             QStringLiteral("middle-click-action"),
             QStringLiteral("link-previews"),
             QStringLiteral("resize-overlay"),
             QStringLiteral("resize-overlay-position"),
             QStringLiteral("gtk-single-instance"),
         }) {
        const QByteArray row = QStringLiteral("enum-%1").arg(enumName).toUtf8();
        QTest::newRow(row.constData())
            << withValue(object(), enumName, QStringLiteral("unsupported"))
            << QStringLiteral("values.%1 has unsupported value").arg(enumName);
    }
}

void GhosttyConfigExportTest::rejectsInvalidValues()
{
    QFETCH(QJsonObject, exportObject);
    QFETCH(QString, diagnostic);
    const auto parsed = parseGhosttyConfigExportJson(json(exportObject));
    QVERIFY(parsed.has_value() == false);
    QVERIFY2(parsed.error().contains(diagnostic), qPrintable(parsed.error()));
}

void GhosttyConfigExportTest::rejectsInvalidBindings()
{
    const auto reject = [](QJsonObject exportObject,
                           const QString &diagnostic) {
        const auto parsed = parseGhosttyConfigExportJson(json(exportObject));
        QVERIFY(parsed.has_value() == false);
        QVERIFY2(parsed.error().contains(diagnostic), qPrintable(parsed.error()));
    };

    QJsonObject exportObject = object();
    QJsonObject bindings = keybindings();
    bindings.insert(
        QStringLiteral("tables"),
        QJsonArray{
            QJsonObject{{QStringLiteral("name"), QStringLiteral("mode")},
                        {QStringLiteral("bindings"), QJsonArray{}}},
            QJsonObject{{QStringLiteral("name"), QStringLiteral("mode")},
                        {QStringLiteral("bindings"), QJsonArray{}}},
        });
    exportObject.insert(QStringLiteral("keybindings"), bindings);
    reject(exportObject, QStringLiteral("Duplicate Ghostty keybinding table 'mode'"));

    exportObject = object();
    bindings = keybindings();
    const QJsonObject duplicate = binding({unicodeTrigger('a')},
                                          {QStringLiteral("ignore")});
    bindings.insert(QStringLiteral("root"),
                    QJsonArray{duplicate, duplicate});
    exportObject.insert(QStringLiteral("keybindings"), bindings);
    reject(exportObject, QStringLiteral("duplicate trigger sequence"));

    exportObject = object();
    bindings = keybindings();
    bindings.insert(
        QStringLiteral("root"),
        QJsonArray{binding({unicodeTrigger(0xd800)},
                           {QStringLiteral("ignore")})});
    exportObject.insert(QStringLiteral("default-keybindings"), bindings);
    reject(exportObject,
           QStringLiteral("default-keybindings.root[0].sequence[0].codepoint"));

    exportObject = object();
    bindings = keybindings();
    bindings.insert(
        QStringLiteral("root"),
        QJsonArray{binding({unicodeTrigger(0)},
                           {QStringLiteral("ignore")})});
    exportObject.insert(QStringLiteral("keybindings"), bindings);
    reject(exportObject, QStringLiteral("nonzero Unicode scalar"));

    exportObject = object();
    bindings = keybindings();
    bindings.insert(
        QStringLiteral("root"),
        QJsonArray{binding({unicodeTrigger('a'), unicodeTrigger('b')},
                           {QStringLiteral("ignore")},
                           flags(true, true))});
    exportObject.insert(QStringLiteral("keybindings"), bindings);
    reject(exportObject,
           QStringLiteral("all/global binding must contain exactly one trigger"));

    exportObject = object();
    bindings = keybindings();
    bindings.insert(
        QStringLiteral("root"),
        QJsonArray{binding({unicodeTrigger('a', 16)},
                           {QStringLiteral("ignore")})});
    exportObject.insert(QStringLiteral("keybindings"), bindings);
    reject(exportObject, QStringLiteral("mods must be an unsigned integer"));

    exportObject = object();
    bindings = keybindings();
    bindings.insert(QStringLiteral("root"),
                    QJsonArray{binding({}, {QStringLiteral("ignore")})});
    exportObject.insert(QStringLiteral("keybindings"), bindings);
    reject(exportObject, QStringLiteral("sequence must be a non-empty array"));

    exportObject = object();
    bindings = keybindings();
    bindings.insert(QStringLiteral("root"),
                    QJsonArray{binding({unicodeTrigger('a')}, {})});
    exportObject.insert(QStringLiteral("keybindings"), bindings);
    reject(exportObject, QStringLiteral("actions must be a non-empty array"));
}

QTEST_GUILESS_MAIN(GhosttyConfigExportTest)

#include "test_ghostty_config_export.moc"
