#include "ghostty_config_edit.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>

namespace {

bool writeFile(const QString &path, const QByteArray &contents)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(contents) == contents.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

} // namespace

class GhosttyConfigEditTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void selectsUsingPinnedLinuxPrecedence_data();
    void selectsUsingPinnedLinuxPrecedence();
    void createsPreferredPathAndParentWithoutTruncation();
    void opensEncodedLocalUrlAndReportsDesktopFailure();
    void rejectsInvalidPathsAndCreationFailure();
    void rejectsNulAndNonRegularCandidates();
};

void GhosttyConfigEditTest::selectsUsingPinnedLinuxPrecedence_data()
{
    QTest::addColumn<bool>("preferredExists");
    QTest::addColumn<QByteArray>("preferredContents");
    QTest::addColumn<bool>("legacyExists");
    QTest::addColumn<QByteArray>("legacyContents");
    QTest::addColumn<QString>("expectedName");

    QTest::newRow("both-non-empty")
        << true << QByteArrayLiteral("preferred")
        << true << QByteArrayLiteral("legacy")
        << QStringLiteral("config.ghostty");
    QTest::newRow("preferred-empty-legacy-non-empty")
        << true << QByteArray{}
        << true << QByteArrayLiteral("legacy")
        << QStringLiteral("config");
    QTest::newRow("both-empty")
        << true << QByteArray{}
        << true << QByteArray{}
        << QStringLiteral("config.ghostty");
    QTest::newRow("preferred-missing-legacy-empty")
        << false << QByteArray{}
        << true << QByteArray{}
        << QStringLiteral("config");
    QTest::newRow("preferred-missing-legacy-non-empty")
        << false << QByteArray{}
        << true << QByteArrayLiteral("legacy")
        << QStringLiteral("config");
}

void GhosttyConfigEditTest::selectsUsingPinnedLinuxPrecedence()
{
    QFETCH(bool, preferredExists);
    QFETCH(QByteArray, preferredContents);
    QFETCH(bool, legacyExists);
    QFETCH(QByteArray, legacyContents);
    QFETCH(QString, expectedName);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString preferred =
        QDir(temporary.path()).filePath(QStringLiteral("config.ghostty"));
    const QString legacy =
        QDir(temporary.path()).filePath(QStringLiteral("config"));
    if (preferredExists) QVERIFY(writeFile(preferred, preferredContents));
    if (legacyExists) QVERIFY(writeFile(legacy, legacyContents));

    const auto selected = prepareGhosttyConfigForEditing({preferred, legacy});
    if (!selected.has_value()) QFAIL(qPrintable(selected.error()));
    QCOMPARE(*selected,
             QDir(temporary.path()).filePath(expectedName));
    if (preferredExists) QCOMPARE(readFile(preferred), preferredContents);
    if (legacyExists) QCOMPARE(readFile(legacy), legacyContents);
}

void GhosttyConfigEditTest::createsPreferredPathAndParentWithoutTruncation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString directory = QDir(temporary.path()).filePath(
        QStringLiteral("nested/ghostty"));
    const QString preferred =
        QDir(directory).filePath(QStringLiteral("config.ghostty"));
    const QString legacy =
        QDir(directory).filePath(QStringLiteral("config"));

    const auto selected = prepareGhosttyConfigForEditing({preferred, legacy});
    if (!selected.has_value()) QFAIL(qPrintable(selected.error()));
    QCOMPARE(*selected, preferred);
    QVERIFY(QFileInfo::exists(preferred));
    QCOMPARE(QFileInfo(preferred).size(), qint64(0));

    QVERIFY(writeFile(preferred, QByteArrayLiteral("keep me")));
    const auto reopened = prepareGhosttyConfigForEditing({preferred, legacy});
    if (!reopened.has_value()) QFAIL(qPrintable(reopened.error()));
    QCOMPARE(*reopened, preferred);
    QCOMPARE(readFile(preferred), QByteArrayLiteral("keep me"));
}

void GhosttyConfigEditTest::opensEncodedLocalUrlAndReportsDesktopFailure()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString directory = QDir(temporary.path()).filePath(
        QStringLiteral("config space # 猫"));
    const QString preferred =
        QDir(directory).filePath(QStringLiteral("config.ghostty"));
    const QString legacy =
        QDir(directory).filePath(QStringLiteral("config"));

    QUrl observed;
    const auto opened = openGhosttyConfigForEditing(
        {preferred, legacy}, [&observed](const QUrl &url) {
            observed = url;
            return true;
        });
    if (!opened.has_value()) QFAIL(qPrintable(opened.error()));
    QCOMPARE(*opened, preferred);
    QVERIFY(observed.isLocalFile());
    QCOMPARE(observed.toLocalFile(), preferred);
    QVERIFY(observed.toString().contains(QStringLiteral("%23")));

    int attempts = 0;
    const auto rejected = openGhosttyConfigForEditing(
        {preferred, legacy}, [&attempts](const QUrl &) {
            ++attempts;
            return false;
        });
    QVERIFY(!rejected.has_value());
    QCOMPARE(attempts, 1);
    QVERIFY(rejected.error().contains(QStringLiteral("desktop")));
    QVERIFY(QFileInfo::exists(preferred));
}

void GhosttyConfigEditTest::rejectsInvalidPathsAndCreationFailure()
{
    const auto absent = prepareGhosttyConfigForEditing({});
    QVERIFY(!absent.has_value());
    const auto relative = prepareGhosttyConfigForEditing(
        {QStringLiteral("ghostty/config.ghostty")});
    QVERIFY(!relative.has_value());

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString blocker =
        QDir(temporary.path()).filePath(QStringLiteral("regular-file"));
    QVERIFY(writeFile(blocker, QByteArrayLiteral("not a directory")));
    const QString preferred =
        QDir(blocker).filePath(QStringLiteral("config.ghostty"));
    const QString legacy =
        QDir(blocker).filePath(QStringLiteral("config"));
    int attempts = 0;
    const auto failed = openGhosttyConfigForEditing(
        {preferred, legacy}, [&attempts](const QUrl &) {
            ++attempts;
            return true;
        });
    QVERIFY(!failed.has_value());
    QCOMPARE(attempts, 0);
    QVERIFY(failed.error().contains(QStringLiteral("directory")));
}

void GhosttyConfigEditTest::rejectsNulAndNonRegularCandidates()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    QString nulPath =
        QDir(temporary.path()).filePath(QStringLiteral("config.ghostty"));
    nulPath += QChar::Null;
    nulPath += QStringLiteral("ignored");
    const auto nul = prepareGhosttyConfigForEditing({nulPath});
    QVERIFY(!nul.has_value());
    QVERIFY2(nul.error().contains(QStringLiteral("NUL")),
             qPrintable(nul.error()));

    const auto directory = prepareGhosttyConfigForEditing({temporary.path()});
    QVERIFY(!directory.has_value());
    QVERIFY2(directory.error().contains(QStringLiteral("regular file")),
             qPrintable(directory.error()));

    const QString fifoPath =
        QDir(temporary.path()).filePath(QStringLiteral("config-fifo"));
    const QByteArray nativeFifoPath = QFile::encodeName(fifoPath);
    QCOMPARE(::mkfifo(nativeFifoPath.constData(), 0600), 0);
    const auto fifo = prepareGhosttyConfigForEditing({fifoPath});
    QVERIFY(!fifo.has_value());
    QVERIFY2(fifo.error().contains(QStringLiteral("regular file")),
             qPrintable(fifo.error()));
}

QTEST_APPLESS_MAIN(GhosttyConfigEditTest)

#include "test_ghostty_config_edit.moc"
