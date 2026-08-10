# Internationalization

ghostty-qt does not yet ship application translation catalogs. This document
records the audited state, the intended Qt localization architecture, and the
boundary for reusing translations from the pinned Ghostty checkout.

## Current state

The QML presentation is substantially marked for translation. At the time of
the audit it contains 250 `qsTr()` call sites, from which Qt's `lupdate` emits
217 messages containing 211 unique source strings. There is no corresponding
runtime or build integration yet:

- CMake does not load `Qt6LinguistTools` or call `qt_add_translations`;
- the application does not install a `QTranslator`;
- no ghostty-qt TS or QM catalogs exist;
- several user-visible C++ strings still use `QStringLiteral` and therefore
  are not extracted; and
- the private Ghostty configuration helper is deliberately built with
  `-Di18n=false` and no Ghostty MO catalogs are installed.

The first implementation should select the system UI locale at startup.
Runtime language switching requires every C++ model and cached string to be
rebuilt or notified and is a separate feature. Ghostty's `language` setting is
GTK-specific in the pinned configuration and must not silently become the Qt
frontend's locale setting.

## Application-owned catalogs

Qt Linguist is the preferred translation system for ghostty-qt. It matches the
existing `qsTr()` source, handles QML and C++ in one catalog, adds no mandatory
KDE Frameworks dependency, and supports embedding compiled QM files in the
application resource tree.

The intended build and startup sequence is:

1. find Qt's `LinguistTools` component and declare the supported locales;
2. add app-owned `translations/ghostty-qt_<locale>.ts` files with
   `qt_add_translations`;
3. embed the generated QM files below `:/i18n`, or install them in a private
   application translation directory;
4. create a `QTranslator` after `QApplication` construction, load the best
   catalog for `QLocale::system()`, and install it before constructing the QML
   engine; and
5. retain the translator for the complete application lifetime.

Qt documents the combined QML/C++ workflow in
[Localizing Applications](https://doc.qt.io/qt-6/localization.html) and the
runtime loader in
[QTranslator](https://doc.qt.io/qt-6/qtranslator.html).

Source strings should follow these rules:

- continue using `qsTr()` for QML-owned text;
- use `tr()` in `QObject` subclasses and
  `QCoreApplication::translate()` elsewhere in C++;
- translate complete sentences instead of concatenating translated fragments;
- use positional parameters so translations may reorder inserted values;
- use plural APIs for actual counts rather than selecting an English singular
  or plural in code; and
- add translator comments where a short label is ambiguous.

Dynamic models require particular attention. Toasts, desktop-notification
actions, generated command-palette rows, launch failures, configuration
diagnostics, and terminal status text must be translated before being exposed
to QML. Protocol strings, configuration keys, action identifiers, log-only
diagnostics, terminal content, and user-provided titles must remain unchanged.

## Ghostty catalog inventory

The pinned Ghostty revision is recorded in `GHOSTTY_REVISION`. Its `po/`
directory currently contains 31 locale catalogs and a POT containing 245
messages. Ghostty uses GNU gettext with the domain
`com.mitchellh.ghostty`; its build compiles catalogs to the conventional path:

```text
share/locale/<locale>/LC_MESSAGES/com.mitchellh.ghostty.mo
```

The pinned POT contains singular, context-free messages. Ghostty's exported
`ghostty_translate` function is consequently documented for singular strings
maintained by Ghostty. The main Qt application cannot call it through its
current dependency: `libghostty-vt` does not export that full-application API.
The private configuration helper does link the full Ghostty runtime.

Ghostty's localization became available for its GTK GUI in the
[1.2 release](https://ghostty.org/docs/install/release-notes/1-2-0) and its
locale coverage continues to grow. Reuse must always be based on this
repository's pinned checkout rather than an independently downloaded current
catalog, so translated strings and code cannot drift apart.

## Direct command-palette reuse

Ghostty owns the titles and descriptions of its default command-palette
entries. The pinned parser translates those defaults during configuration
initialization, before ghostty-qt's overlay serializes them to JSON. This is
the cleanest reuse path because the owning component performs the lookup and
the Qt frontend receives already localized text.

It is disabled by the current helper build. Enabling it requires all of the
following as one change:

1. build the private helper runtime with Ghostty i18n enabled;
2. compile the pinned PO files with `msgfmt`;
3. install their MO files below the shared locale directory shown above; and
4. test helper output under representative `LC_MESSAGES` or `LANGUAGE` values.

The helper sets `GHOSTTY_RESOURCES_DIR` to the installed ghostty-qt resource
root. Ghostty derives the sibling `share/locale` directory from that location,
so no locale path should be embedded at configure time. The upstream
runtime-none build does not install i18n resources automatically; CMake must
stage them explicitly or the reviewed Ghostty build overlay must add that
installation step.

Only Ghostty's built-in entries should be translated on this path. Titles and
descriptions supplied by the user through `command-palette-entry` are user
data and must remain byte-for-byte equivalent after UTF-8 decoding.

## Reusing matching frontend translations

An extraction comparison found 29 non-empty QML messages that exactly match
the pinned Ghostty POT:

- `Allow`, `Authorize Clipboard Access`, `Clear`, `Close`, `Copy`, `Deny`,
  `Ignore`, `Paste`, and `Reset`;
- `Change Title…`, `Change Tab Title…`, and
  `Leave blank to restore the default title.`;
- `New Tab`, `Close Tab`, `New Window`, `Close Window`, `Tab`, and `Window`;
- `Split Up`, `Split Down`, `Split Left`, and `Split Right`;
- `Previous Match`, `Next Match`, and `Terminal Inspector`; and
- `Configuration Errors`, `Open Configuration`, `Reload Configuration`, and
  `Reset Terminal`.

This is useful seed coverage, but a converted Ghostty QM file cannot simply be
loaded as an application translator. Qt text translations are keyed by both
source text and context. For QML, the default context is the QML filename;
Ghostty's gettext entries have no corresponding Qt context. Qt documents this
lookup behavior in
[Writing Source Code for Translation](https://doc.qt.io/qt-6/i18n-source-translation.html).
Although [`lconvert`](https://doc.qt.io/qt-6/linguist-lconvert.html) understands
both PO and TS files, format conversion does not invent the missing QML
contexts.

The preferred reuse mechanism is therefore a deterministic catalog seeder:

1. extract or update ghostty-qt's TS files normally;
2. read translations from the PO files in the pinned submodule;
3. copy only reviewed, exact message matches into each applicable Qt context;
4. skip fuzzy, empty, plural, and placeholder-incompatible translations;
5. never overwrite a non-empty app-owned translation; and
6. record the Ghostty revision and source domain in generated metadata or
   translator comments.

The match set should be an explicit allowlist. Similar strings are not safe to
reuse automatically: punctuation, ellipses, terminology such as "pane" versus
"split", and grammatical context may require different translations even
when their English text looks related.

A custom `QTranslator` that ignores the Qt context and calls gettext is not
preferred. It would silently apply one context-free Ghostty translation to
every matching app string and would make ownership and collision behavior
difficult to review.

## KI18n alternative

KI18n is a technically valid alternative. It is gettext-based and its
`i18nd()` API can intentionally reuse another installed translation domain.
The KDE documentation describes
[cross-domain translation reuse](https://develop.kde.org/docs/plasma/widget/translations-i18n/#reusing-other-translations),
and the [KI18n API](https://api.kde.org/ki18n-index.html) supports C++ and QML.

It is not the preferred initial implementation because ghostty-qt currently
uses Qt translation calls and keeps its KDE Frameworks dependency optional.
Adopting KI18n would require a mandatory KF6 I18n dependency, QML context
setup, conversion of existing calls, and a gettext workflow for all app-owned
text. That tradeoff should be revisited only if participation in KDE's
translation infrastructure becomes a project goal.

## Licensing and attribution

Ghostty and its translation catalogs are distributed under the MIT license.
Any copied or compiled catalog content must retain the Ghostty copyright and
license notice in distributed third-party notices. The seeding process should
also preserve translator attribution from the PO headers where the output
format and packaging permit it.

Keeping the MO files under Ghostty's original domain is important: they remain
an upstream-derived resource, while `ghostty-qt_<locale>` remains the domain
for this frontend's independently maintained strings.

## Validation

The implementation is complete only when tests cover both catalog systems:

- an app catalog can be loaded before QML construction and changes a QML and a
  C++ string;
- locale fallback chooses language-only catalogs where a regional catalog is
  absent;
- placeholders and plural forms pass Qt's catalog validation;
- the config helper emits localized built-in command entries with an installed
  Ghostty MO file;
- user-defined command entries remain unchanged;
- an absent or incomplete catalog falls back to the English source string;
- staged installation contains every declared QM and MO file; and
- the Ghostty translation allowlist is rechecked whenever the pinned revision
  changes.

The practical implementation order is app catalog plumbing, C++ source audit,
one small test locale, direct command-palette reuse, reviewed Ghostty seeding,
and finally additional production locales.
