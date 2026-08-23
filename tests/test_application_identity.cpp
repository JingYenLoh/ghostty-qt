#include "app/application_identity.h"

#include <QTest>

#include <optional>

namespace {

constexpr auto BuildApplicationId = u"io.github.JingYenLoh.ghostty_qt";

} // namespace

class ApplicationIdentityTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void missingClassUsesBuildIdentity();
    void emptyClassWarnsAndUsesBuildIdentity();
    void validClassRemainsExact();
    void validatesComponentGrammar_data();
    void validatesComponentGrammar();
    void enforcesOverallLengthBoundary();
    void preservesInvalidBytesInDiagnostic();
    void rejectsInvalidBuildIdentity();
};

void ApplicationIdentityTest::missingClassUsesBuildIdentity()
{
    const auto bytesResolution = resolveApplicationIdentity(
        std::optional<QByteArray>{}, BuildApplicationId);
    QVERIFY(bytesResolution.has_value());
    QCOMPARE(bytesResolution->applicationId,
             QStringLiteral("io.github.JingYenLoh.ghostty_qt"));
    QCOMPARE(bytesResolution->serviceId(), bytesResolution->applicationId);
    QVERIFY(!bytesResolution->diagnostic.has_value());

    const auto stringResolution = resolveApplicationIdentity(
        std::optional<QString>{}, BuildApplicationId);
    QVERIFY(stringResolution.has_value());
    QCOMPARE(stringResolution->applicationId, bytesResolution->applicationId);
    QCOMPARE(stringResolution->diagnostic, bytesResolution->diagnostic);
}

void ApplicationIdentityTest::emptyClassWarnsAndUsesBuildIdentity()
{
    const auto bytesResolution = resolveApplicationIdentity(
        std::optional<QByteArray>{QByteArray{}}, BuildApplicationId);
    QVERIFY(bytesResolution.has_value());
    QCOMPARE(bytesResolution->applicationId,
             QStringLiteral("io.github.JingYenLoh.ghostty_qt"));
    QVERIFY(bytesResolution->diagnostic.has_value());
    QVERIFY(bytesResolution->diagnostic->contains(
        QStringLiteral("invalid Ghostty class")));
    QVERIFY(bytesResolution->diagnostic->contains(QStringLiteral("\"\"")));

    const auto stringResolution = resolveApplicationIdentity(
        std::optional<QString>{QString{}}, BuildApplicationId);
    QVERIFY(stringResolution.has_value());
    QCOMPARE(stringResolution->applicationId, bytesResolution->applicationId);
    QCOMPARE(stringResolution->diagnostic, bytesResolution->diagnostic);
}

void ApplicationIdentityTest::validClassRemainsExact()
{
    const QByteArray configured =
        QByteArrayLiteral("com.example-Ghostty._Quick_Terminal2");
    const auto bytesResolution = resolveApplicationIdentity(
        std::optional<QByteArray>{configured}, BuildApplicationId);
    QVERIFY(bytesResolution.has_value());
    QCOMPARE(bytesResolution->applicationId, QString::fromLatin1(configured));
    QVERIFY(!bytesResolution->diagnostic.has_value());

    const QString configuredString =
        QStringLiteral("Com.Example_-Identity.Ghostty9");
    const auto stringResolution = resolveApplicationIdentity(
        std::optional<QString>{configuredString}, BuildApplicationId);
    QVERIFY(stringResolution.has_value());
    QCOMPARE(stringResolution->applicationId, configuredString);
    QVERIFY(!stringResolution->diagnostic.has_value());
}

void ApplicationIdentityTest::validatesComponentGrammar_data()
{
    QTest::addColumn<QByteArray>("applicationId");
    QTest::addColumn<bool>("valid");

    QTest::newRow("minimum") << QByteArrayLiteral("a.b") << true;
    QTest::newRow("hyphen-initial") << QByteArrayLiteral("-a._b") << true;
    QTest::newRow("uppercase") << QByteArrayLiteral("A.Z") << true;
    QTest::newRow("missing-separator") << QByteArrayLiteral("ghostty") << false;
    QTest::newRow("leading-separator") << QByteArrayLiteral(".a.b") << false;
    QTest::newRow("trailing-separator") << QByteArrayLiteral("a.b.") << false;
    QTest::newRow("empty-component") << QByteArrayLiteral("a..b") << false;
    QTest::newRow("digit-first-component")
        << QByteArrayLiteral("1a.b") << false;
    QTest::newRow("digit-later-component")
        << QByteArrayLiteral("a.1b") << false;
    QTest::newRow("invalid-punctuation") << QByteArrayLiteral("a/b.c") << false;
    QTest::newRow("non-ascii") << QByteArray::fromHex("612ec3a9") << false;
    QTest::newRow("embedded-null")
        << QByteArray::fromRawData("a.\0b", 4) << false;
}

void ApplicationIdentityTest::validatesComponentGrammar()
{
    QFETCH(QByteArray, applicationId);
    QFETCH(bool, valid);

    QCOMPARE(isValidApplicationId(applicationId), valid);
    const auto resolution = resolveApplicationIdentity(
        std::optional<QByteArray>{applicationId}, BuildApplicationId);
    QVERIFY(resolution.has_value());
    QCOMPARE(resolution->diagnostic.has_value(), !valid);
    QCOMPARE(resolution->applicationId,
             valid ? QString::fromLatin1(applicationId)
                   : QStringLiteral("io.github.JingYenLoh.ghostty_qt"));
}

void ApplicationIdentityTest::enforcesOverallLengthBoundary()
{
    const QByteArray maximum = QByteArrayLiteral("a.") + QByteArray(253, 'b');
    QCOMPARE(maximum.size(), 255);
    QVERIFY(isValidApplicationId(maximum));

    const QByteArray tooLong = maximum + 'b';
    QCOMPARE(tooLong.size(), 256);
    QVERIFY(!isValidApplicationId(tooLong));

    const auto maximumResolution = resolveApplicationIdentity(
        std::optional<QByteArray>{maximum}, BuildApplicationId);
    QVERIFY(maximumResolution.has_value());
    QCOMPARE(maximumResolution->applicationId, QString::fromLatin1(maximum));

    const auto tooLongResolution = resolveApplicationIdentity(
        std::optional<QByteArray>{tooLong}, BuildApplicationId);
    QVERIFY(tooLongResolution.has_value());
    QCOMPARE(tooLongResolution->applicationId,
             QStringLiteral("io.github.JingYenLoh.ghostty_qt"));
    QVERIFY(tooLongResolution->diagnostic.has_value());
}

void ApplicationIdentityTest::preservesInvalidBytesInDiagnostic()
{
    const auto resolution = resolveApplicationIdentity(
        std::optional<QByteArray>{QByteArray::fromHex("612eff0080")},
        BuildApplicationId);
    QVERIFY(resolution.has_value());
    QVERIFY(resolution->diagnostic.has_value());
    QVERIFY(resolution->diagnostic->contains(
        QStringLiteral("\"a.\\xff\\x00\\x80\"")));

    const auto unicodeResolution = resolveApplicationIdentity(
        std::optional<QString>{QStringLiteral("com.example.é")},
        BuildApplicationId);
    QVERIFY(unicodeResolution.has_value());
    QVERIFY(unicodeResolution->diagnostic.has_value());
    QVERIFY(
        unicodeResolution->diagnostic->contains(QStringLiteral("\\xc3\\xa9")));
}

void ApplicationIdentityTest::rejectsInvalidBuildIdentity()
{
    const auto resolution =
        resolveApplicationIdentity(std::optional<QByteArray>{}, u"invalid");
    QVERIFY(!resolution.has_value());
    QVERIFY(
        resolution.error().contains(QStringLiteral("build application ID")));
}

QTEST_GUILESS_MAIN(ApplicationIdentityTest)

#include "test_application_identity.moc"
