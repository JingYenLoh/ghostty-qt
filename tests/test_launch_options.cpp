#include "launch_options.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class LaunchOptionsTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void defaults();
    void parsesEveryOptionAndProgramArguments();
    void rejectsInvalidWorkingDirectory();
    void rejectsFileAsWorkingDirectory();
    void rejectsInvalidFontSize_data();
    void rejectsInvalidFontSize();
    void rejectsInvalidScrollbackLines_data();
    void rejectsInvalidScrollbackLines();
    void rejectsUnknownOption();
    void preservesOutputOnFailure();
};

void LaunchOptionsTest::defaults()
{
    LaunchOptions options;
    QString error;

    QVERIFY2(parseLaunchOptions({QStringLiteral("ghostty-qt")}, &options, &error),
             qPrintable(error));
    QCOMPARE(options.workingDirectory, QDir::currentPath());
    QVERIFY(options.fontFamily.isEmpty());
    QCOMPARE(options.fontSize, 12.0);
    QCOMPARE(options.scrollbackLines, 10'000);
    QVERIFY(!options.hold);
    QVERIFY(options.program.isEmpty());
    QVERIFY(error.isEmpty());
}

void LaunchOptionsTest::parsesEveryOptionAndProgramArguments()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    LaunchOptions options;
    QString error;
    const QStringList arguments{
        QStringLiteral("ghostty-qt"),
        QStringLiteral("--working-directory"),
        directory.path(),
        QStringLiteral("--font-family"),
        QStringLiteral("Iosevka Term"),
        QStringLiteral("--font-size=15.5"),
        QStringLiteral("--scrollback-lines"),
        QStringLiteral("250000"),
        QStringLiteral("--hold"),
        QStringLiteral("--"),
        QStringLiteral("/bin/sh"),
        QStringLiteral("-lc"),
        QStringLiteral("printf hello"),
    };

    QVERIFY2(parseLaunchOptions(arguments, &options, &error), qPrintable(error));
    QCOMPARE(options.workingDirectory, QDir::cleanPath(directory.path()));
    QCOMPARE(options.fontFamily, QStringLiteral("Iosevka Term"));
    QCOMPARE(options.fontSize, 15.5);
    QCOMPARE(options.scrollbackLines, 250'000);
    QVERIFY(options.hold);
    QCOMPARE(options.program,
             QStringList({QStringLiteral("/bin/sh"), QStringLiteral("-lc"),
                          QStringLiteral("printf hello")}));
}

void LaunchOptionsTest::rejectsInvalidWorkingDirectory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString missingPath = directory.filePath(QStringLiteral("missing"));

    LaunchOptions options;
    QString error;
    QVERIFY(!parseLaunchOptions({QStringLiteral("ghostty-qt"),
                                 QStringLiteral("--working-directory"), missingPath},
                                &options, &error));
    QVERIFY(error.contains(QStringLiteral("does not exist or is not a directory")));
}

void LaunchOptionsTest::rejectsFileAsWorkingDirectory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString filePath = directory.filePath(QStringLiteral("regular-file"));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    LaunchOptions options;
    QString error;
    QVERIFY(!parseLaunchOptions({QStringLiteral("ghostty-qt"),
                                 QStringLiteral("--working-directory"), filePath},
                                &options, &error));
    QVERIFY(error.contains(QStringLiteral("does not exist or is not a directory")));
}

void LaunchOptionsTest::rejectsInvalidFontSize_data()
{
    QTest::addColumn<QString>("value");

    QTest::newRow("zero") << QStringLiteral("0");
    QTest::newRow("negative") << QStringLiteral("-2");
    QTest::newRow("not-a-number") << QStringLiteral("large");
    QTest::newRow("infinity") << QStringLiteral("inf");
}

void LaunchOptionsTest::rejectsInvalidFontSize()
{
    QFETCH(QString, value);

    LaunchOptions options;
    QString error;
    QVERIFY(!parseLaunchOptions({QStringLiteral("ghostty-qt"), QStringLiteral("--font-size"),
                                 value},
                                &options, &error));
    QVERIFY(error.contains(QStringLiteral("Invalid font size")));
}

void LaunchOptionsTest::rejectsInvalidScrollbackLines_data()
{
    QTest::addColumn<QString>("value");

    QTest::newRow("negative") << QStringLiteral("-1");
    QTest::newRow("too-large") << QStringLiteral("10000001");
    QTest::newRow("fractional") << QStringLiteral("1.5");
    QTest::newRow("not-a-number") << QStringLiteral("many");
}

void LaunchOptionsTest::rejectsInvalidScrollbackLines()
{
    QFETCH(QString, value);

    LaunchOptions options;
    QString error;
    QVERIFY(!parseLaunchOptions({QStringLiteral("ghostty-qt"),
                                 QStringLiteral("--scrollback-lines"), value},
                                &options, &error));
    QVERIFY(error.contains(QStringLiteral("Invalid scrollback line count")));
}

void LaunchOptionsTest::rejectsUnknownOption()
{
    LaunchOptions options;
    QString error;
    QVERIFY(!parseLaunchOptions(
        {QStringLiteral("ghostty-qt"), QStringLiteral("--not-an-option")}, &options, &error));
    QVERIFY(!error.isEmpty());
}

void LaunchOptionsTest::preservesOutputOnFailure()
{
    LaunchOptions options;
    options.fontFamily = QStringLiteral("sentinel");
    options.fontSize = 42.0;

    QVERIFY(!parseLaunchOptions(
        {QStringLiteral("ghostty-qt"), QStringLiteral("--font-size=0")}, &options));
    QCOMPARE(options.fontFamily, QStringLiteral("sentinel"));
    QCOMPARE(options.fontSize, 42.0);
}

QTEST_APPLESS_MAIN(LaunchOptionsTest)

#include "test_launch_options.moc"
