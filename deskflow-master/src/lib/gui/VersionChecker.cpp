// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
// SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
#include "VersionChecker.h"
#include "common/UrlConstants.h"
#include "common/VersionInfo.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QVersionNumber>
#include <QVariant>

VersionChecker::VersionChecker(QObject *parent) : QObject(parent), m_network(new QNetworkAccessManager(this))
{
  connect(m_network, &QNetworkAccessManager::finished, this, &VersionChecker::replyFinished);
}
bool VersionChecker::isNewer(const QString &current, const QString &candidate)
{
  return QVersionNumber::compare(QVersionNumber::fromString(candidate), QVersionNumber::fromString(current)) > 0;
}
bool VersionChecker::parseRelease(const QByteArray &json, Release &release, QString &error)
{
  release = {};
  const auto document = QJsonDocument::fromJson(json);
  const auto object = document.object();
  const auto tag = object.value("tag_name").toString();
  static const QRegularExpression versionPattern("^v([0-9]+\\.[0-9]+\\.[0-9]+)$");
  const auto match = versionPattern.match(tag);
  if (!document.isObject() || !match.hasMatch() || object.value("draft").toBool(true) || object.value("prerelease").toBool(true)) {
    error = tr("Invalid release metadata; no stable release found.");
    return false;
  }
  const auto version = match.captured(1);
  const auto name = QStringLiteral("DeskSend-%1-windows-x64.zip").arg(version);
  const QUrl expected(QStringLiteral("https://github.com/wtyLaziness/desksend-releases/releases/download/%1/%2").arg(tag, name));
  for (const auto &entry : object.value("assets").toArray()) {
    const auto asset = entry.toObject();
    if (asset.value("name").toString() != name)
      continue;
    static const QRegularExpression hashPattern("^sha256:([0-9a-fA-F]{64})$");
    const auto hash = hashPattern.match(asset.value("digest").toString());
    const auto size = asset.value("size").toVariant().toLongLong();
    if (QUrl(asset.value("browser_download_url").toString()) != expected || !hash.hasMatch() || size <= 0 || asset.value("state").toString() != QStringLiteral("uploaded")) {
      error = tr("Invalid update package URL, size, or SHA-256 digest.");
      return false;
    }
    release = {version, expected, hash.captured(1).toLatin1().toLower(), size};
    return true;
  }
  error = tr("This release does not provide a complete Windows x64 update package.");
  return false;
}
QString VersionChecker::formatHttpFailure(
    int statusCode, const QByteArray &rateLimitRemaining, const QByteArray &rateLimitResetEpoch,
    const QString &networkError
)
{
  if (statusCode == 429 || (statusCode == 403 && rateLimitRemaining.trimmed() == "0")) {
    bool ok = false;
    const qint64 resetEpoch = rateLimitResetEpoch.trimmed().toLongLong(&ok);
    if (ok && resetEpoch > 0) {
      const QString resetTime = QDateTime::fromSecsSinceEpoch(resetEpoch).toLocalTime().toString(QStringLiteral("HH:mm"));
      return tr("GitHub API rate limit reached; please try again after %1.").arg(resetTime);
    }
    return tr("GitHub API rate limit reached; please try again later.");
  }
  return tr("Unable to check for DeskSend updates: %1").arg(networkError);
}
void VersionChecker::checkLatest()
{
  if (m_checking)
    return;
  m_checking = true;
  m_release = {};
  // 固定使用 DeskSend 发布源，不读取旧版保存的上游更新地址。
  QNetworkRequest request{QUrl(kUrlUpdateCheck)};
  request.setTransferTimeout(20000);
  request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("DeskSend/%1").arg(kVersion));
  request.setRawHeader("Accept", "application/vnd.github+json");
  if (m_hasCachedRelease && !m_etag.isEmpty()) {
    request.setRawHeader("If-None-Match", m_etag.toUtf8());
  }
  m_network->get(request);
}
void VersionChecker::replyFinished(QNetworkReply *reply)
{
  m_checking = false;
  reply->deleteLater();
  const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (statusCode == 304 && m_hasCachedRelease) {
    m_release = m_cachedRelease;
    const bool available = isNewer(kVersion, m_release.version);
    if (available)
      Q_EMIT updateFound(m_release.version);
    Q_EMIT checked(available);
    return;
  }
  if (reply->error() != QNetworkReply::NoError) {
    Q_EMIT checkFailed(formatHttpFailure(
        statusCode, reply->rawHeader("X-RateLimit-Remaining"), reply->rawHeader("X-RateLimit-Reset"),
        reply->errorString()
    ));
    return;
  }
  QString error;
  if (!parseRelease(reply->readAll(), m_release, error)) {
    Q_EMIT checkFailed(error);
    return;
  }
  const QByteArray etag = reply->rawHeader("ETag");
  if (!etag.isEmpty()) {
    m_etag = QString::fromUtf8(etag);
    m_cachedRelease = m_release;
    m_hasCachedRelease = true;
  }
  const bool available = isNewer(kVersion, m_release.version);
  if (available)
    Q_EMIT updateFound(m_release.version);
  Q_EMIT checked(available);
}
