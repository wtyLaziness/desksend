// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "gui/VersionChecker.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

class VersionCheckerTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void publicChannel()
  {
    if (!qEnvironmentVariableIsSet("DESKSEND_LIVE_UPDATE_TEST"))
      QSKIP("Public release smoke test is enabled after publishing");
    VersionChecker checker;
    QSignalSpy checked(&checker, &VersionChecker::checked);
    QSignalSpy errors(&checker, &VersionChecker::checkFailed);
    checker.checkLatest();
    QTRY_VERIFY_WITH_TIMEOUT(!checked.isEmpty() || !errors.isEmpty(), 30000);
    QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
    QVERIFY(!checker.release().download.isEmpty());
    QCOMPARE(checker.release().sha256.size(), 64);
  }
  void versions()
  {
    QVERIFY(VersionChecker::isNewer("1.0.0.0", "1.0.1"));
    QVERIFY(VersionChecker::isNewer("1.9.9", "1.10.0"));
    QVERIFY(!VersionChecker::isNewer("2.0.0", "1.99.99"));
    QVERIFY(!VersionChecker::isNewer("1.0.0.0", "1.0.0"));
  }
  void metadata()
  {
    QJsonObject asset{{"name", "DeskSend-1.1.0-windows-x64.zip"}, {"state", "uploaded"}, {"size", 123456},
                      {"digest", "sha256:" + QString(64, 'a')},
                      {"browser_download_url", "https://github.com/wtyLaziness/desksend-releases/releases/download/v1.1.0/DeskSend-1.1.0-windows-x64.zip"}};
    QJsonObject release{{"tag_name", "v1.1.0"}, {"draft", false}, {"prerelease", false}, {"assets", QJsonArray{asset}}};
    VersionChecker::Release parsed;
    QString error;
    QVERIFY(VersionChecker::parseRelease(QJsonDocument(release).toJson(), parsed, error));
    QCOMPARE(parsed.version, QString("1.1.0"));
    QCOMPARE(parsed.sha256, QByteArray(64, 'a'));
    release["draft"] = true;
    QVERIFY(!VersionChecker::parseRelease(QJsonDocument(release).toJson(), parsed, error));
    QVERIFY(parsed.download.isEmpty());
    release["draft"] = false;
    release["prerelease"] = true;
    QVERIFY(!VersionChecker::parseRelease(QJsonDocument(release).toJson(), parsed, error));
    release["prerelease"] = false;
    asset["browser_download_url"] = "https://example.com/untrusted.zip";
    release["assets"] = QJsonArray{asset};
    QVERIFY(!VersionChecker::parseRelease(QJsonDocument(release).toJson(), parsed, error));
    asset["browser_download_url"] = "https://github.com/wtyLaziness/desksend-releases/releases/download/v1.1.0/DeskSend-1.1.0-windows-x64.zip";
    asset.remove("digest");
    release["assets"] = QJsonArray{asset};
    QVERIFY(!VersionChecker::parseRelease(QJsonDocument(release).toJson(), parsed, error));
    QVERIFY(!VersionChecker::parseRelease("not json", parsed, error));
  }
  void httpRateLimitFormatting()
  {
    const QString rateLimited = VersionChecker::formatHttpFailure(403, "0", "1700000000", "Forbidden");
    QVERIFY(rateLimited.contains(QStringLiteral("rate limit"), Qt::CaseInsensitive));
    QVERIFY(rateLimited.contains(':'));
    const QString tooMany = VersionChecker::formatHttpFailure(429, "", "", "Too Many Requests");
    QVERIFY(tooMany.contains(QStringLiteral("rate limit"), Qt::CaseInsensitive));
    const QString genericErr = VersionChecker::formatHttpFailure(500, "59", "", "Internal Server Error");
    QVERIFY(genericErr.contains(QStringLiteral("Internal Server Error")));
  }
  void fallbackDecisionAndAssetUrl()
  {
    QVERIFY(VersionChecker::shouldUseFallback(403));
    QVERIFY(VersionChecker::shouldUseFallback(429));
    QVERIFY(!VersionChecker::shouldUseFallback(200));
    QVERIFY(!VersionChecker::shouldUseFallback(404));
    QVERIFY(!VersionChecker::shouldUseFallback(500));

    QCOMPARE(
        VersionChecker::releaseAssetUrl(QStringLiteral("1.0.3"), QStringLiteral("SHA256SUMS.txt")),
        QUrl(QStringLiteral("https://github.com/wtyLaziness/desksend-releases/releases/download/v1.0.3/SHA256SUMS.txt"))
    );
  }
  void parseLatestTagUrl()
  {
    QString version;
    QVERIFY(VersionChecker::parseLatestTagUrl(
        QUrl(QStringLiteral("https://github.com/wtyLaziness/desksend-releases/releases/tag/v1.0.2")), version
    ));
    QCOMPARE(version, QStringLiteral("1.0.2"));

    QVERIFY(VersionChecker::parseLatestTagUrl(
        QUrl(QStringLiteral("/wtyLaziness/desksend-releases/releases/tag/v1.10.3")), version
    ));
    QCOMPARE(version, QStringLiteral("1.10.3"));

    QVERIFY(!VersionChecker::parseLatestTagUrl(
        QUrl(QStringLiteral("http://github.com/wtyLaziness/desksend-releases/releases/tag/v1.0.2")), version
    ));
    QVERIFY(!VersionChecker::parseLatestTagUrl(
        QUrl(QStringLiteral("https://evil.com/wtyLaziness/desksend-releases/releases/tag/v1.0.2")), version
    ));
    QVERIFY(!VersionChecker::parseLatestTagUrl(
        QUrl(QStringLiteral("//evil.com/wtyLaziness/desksend-releases/releases/tag/v1.0.2")), version
    ));
    QVERIFY(!VersionChecker::parseLatestTagUrl(
        QUrl(QStringLiteral("https://github.com/other/desksend-releases/releases/tag/v1.0.2")), version
    ));
    QVERIFY(!VersionChecker::parseLatestTagUrl(
        QUrl(QStringLiteral("https://github.com/wtyLaziness/desksend-releases/releases")), version
    ));
    QVERIFY(!VersionChecker::parseLatestTagUrl(
        QUrl(QStringLiteral("https://github.com/wtyLaziness/desksend-releases/releases/tag/v1.0.2-rc1")), version
    ));
    QVERIFY(!VersionChecker::parseLatestTagUrl(
        QUrl(QStringLiteral("https://github.com/wtyLaziness/desksend-releases/releases/tag/v1.0")), version
    ));
    QVERIFY(!VersionChecker::parseLatestTagUrl(
        QUrl(QStringLiteral("https://github.com:8443/wtyLaziness/desksend-releases/releases/tag/v1.0.2")), version
    ));
    QVERIFY(!VersionChecker::parseLatestTagUrl(
        QUrl(QStringLiteral("https://github.com/wtyLaziness/desksend-releases/releases/tag/v1.0.2?foo=1")), version
    ));
    QVERIFY(version.isEmpty());
  }
  void parseSha256Sums()
  {
    const QByteArray validSums =
        "58331eb9531e1c90af40ad6323397bd5ccbc0fc78906917390dd218103af04ee  DeskSend-1.0.2-source.zip\n"
        "7e3160b0f4ff17d0141a6d925af3e1f847afd041784626761a8b24af0223c700  DeskSend-1.0.2-windows-x64.zip\n";
    QByteArray sha256;
    QVERIFY(VersionChecker::parseSha256Sums(validSums, QStringLiteral("1.0.2"), sha256));
    QCOMPARE(sha256, QByteArray("7e3160b0f4ff17d0141a6d925af3e1f847afd041784626761a8b24af0223c700"));

    const QByteArray bomBinaryUpper =
        "\xEF\xBB\xBF"
        "7E3160B0F4FF17D0141A6D925AF3E1F847AFD041784626761A8B24AF0223C700 *DeskSend-1.0.2-windows-x64.zip\r\n";
    QVERIFY(VersionChecker::parseSha256Sums(bomBinaryUpper, QStringLiteral("1.0.2"), sha256));
    QCOMPARE(sha256, QByteArray("7e3160b0f4ff17d0141a6d925af3e1f847afd041784626761a8b24af0223c700"));

    QVERIFY(!VersionChecker::parseSha256Sums(validSums, QStringLiteral("1.0.3"), sha256));
    QVERIFY(sha256.isEmpty());

    QVERIFY(!VersionChecker::parseSha256Sums(
        "7e3160b0f4ff17d0141a6d925af3e1f847afd041784626761a8b24af0223c70  DeskSend-1.0.2-windows-x64.zip\n",
        QStringLiteral("1.0.2"), sha256
    ));
    QVERIFY(!VersionChecker::parseSha256Sums(
        "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz  DeskSend-1.0.2-windows-x64.zip\n",
        QStringLiteral("1.0.2"), sha256
    ));
    QVERIFY(!VersionChecker::parseSha256Sums(
        "7e3160b0f4ff17d0141a6d925af3e1f847afd041784626761a8b24af0223c700  sub/DeskSend-1.0.2-windows-x64.zip\n",
        QStringLiteral("1.0.2"), sha256
    ));
  }
};
QTEST_MAIN(VersionCheckerTests)
#include "VersionCheckerTests.moc"
