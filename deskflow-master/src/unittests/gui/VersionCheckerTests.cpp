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
};
QTEST_MAIN(VersionCheckerTests)
#include "VersionCheckerTests.moc"
