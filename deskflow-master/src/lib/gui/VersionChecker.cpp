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
namespace {

QNetworkRequest makeUpdateRequest(const QUrl &url, QNetworkRequest::RedirectPolicy redirectPolicy)
{
  QNetworkRequest request{url};
  request.setTransferTimeout(20000);
  request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("DeskSend/%1").arg(kVersion));
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, redirectPolicy);
  return request;
}

QString releaseBaseUrl()
{
  QString base = kUrlDownload;
  if (base.endsWith(QLatin1String("/latest"))) {
    base.chop(6);
  } else if (!base.endsWith(QLatin1Char('/'))) {
    base.append(QLatin1Char('/'));
  }
  return base;
}

} // namespace

bool VersionChecker::shouldUseFallback(int statusCode)
{
  return statusCode == 403 || statusCode == 429;
}

bool VersionChecker::parseLatestTagUrl(const QUrl &location, QString &version)
{
  version.clear();
  if (!location.isValid())
    return false;

  const QUrl baseUrl(kUrlDownload);
  const QUrl resolved = location.isRelative() ? baseUrl.resolved(location) : location;
  if (!resolved.isValid() || resolved.scheme() != QLatin1String("https"))
    return false;
  if (resolved.host().compare(baseUrl.host(), Qt::CaseInsensitive) != 0)
    return false;
  if (resolved.port() != -1 || !resolved.userInfo().isEmpty() || resolved.hasQuery() || resolved.hasFragment())
    return false;

  const QString tagPrefix = QUrl(releaseBaseUrl()).path() + QLatin1String("tag/v");
  const QString path = resolved.path();
  if (!path.startsWith(tagPrefix))
    return false;

  const QString tagVersion = path.mid(tagPrefix.size());
  int parts = 0;
  int segStart = 0;
  for (int i = 0; i <= tagVersion.size(); ++i) {
    const bool atEnd = (i == tagVersion.size());
    if (atEnd || tagVersion.at(i) == QLatin1Char('.')) {
      if (i - segStart <= 0)
        return false;
      ++parts;
      segStart = i + 1;
      continue;
    }
    const QChar ch = tagVersion.at(i);
    if (ch < QLatin1Char('0') || ch > QLatin1Char('9'))
      return false;
  }
  if (parts != 3)
    return false;

  version = tagVersion;
  return true;
}

bool VersionChecker::parseSha256Sums(const QByteArray &text, const QString &version, QByteArray &sha256)
{
  sha256.clear();
  if (version.isEmpty())
    return false;

  const QString expectedName = QStringLiteral("DeskSend-%1-windows-x64.zip").arg(version);
  QString content = QString::fromUtf8(text);
  if (content.startsWith(QChar(0xFEFF)))
    content.remove(0, 1);

  int start = 0;
  while (start < content.size()) {
    int end = content.indexOf(QLatin1Char('\n'), start);
    if (end < 0)
      end = content.size();
    QString line = content.mid(start, end - start);
    start = end + 1;

    if (line.endsWith(QLatin1Char('\r')))
      line.chop(1);
    if (line.size() < 66)
      continue;

    bool hexOk = true;
    for (int i = 0; i < 64; ++i) {
      const QChar ch = line.at(i);
      const bool isDigit = ch >= QLatin1Char('0') && ch <= QLatin1Char('9');
      const bool isLowerHex = ch >= QLatin1Char('a') && ch <= QLatin1Char('f');
      const bool isUpperHex = ch >= QLatin1Char('A') && ch <= QLatin1Char('F');
      if (!isDigit && !isLowerHex && !isUpperHex) {
        hexOk = false;
        break;
      }
    }
    if (!hexOk)
      continue;

    int pos = 64;
    if (line.at(pos) != QLatin1Char(' ') && line.at(pos) != QLatin1Char('\t'))
      continue;
    while (pos < line.size() && (line.at(pos) == QLatin1Char(' ') || line.at(pos) == QLatin1Char('\t')))
      ++pos;
    if (pos < line.size() && line.at(pos) == QLatin1Char('*'))
      ++pos;

    if (line.mid(pos) == expectedName) {
      sha256 = line.left(64).toLower().toLatin1();
      return true;
    }
  }
  return false;
}

QUrl VersionChecker::releaseAssetUrl(const QString &version, const QString &fileName)
{
  return QUrl(QStringLiteral("%1download/v%2/%3").arg(releaseBaseUrl(), version, fileName));
}

void VersionChecker::checkLatest()
{
  if (m_checking)
    return;
  m_checking = true;
  m_release = {};
  m_pendingVersion.clear();
  m_pendingDownload.clear();
  m_pendingSha256.clear();
  m_primaryError.clear();
  startPrimaryApi();
}

void VersionChecker::startPrimaryApi()
{
  m_stage = CheckStage::PrimaryApi;
  // 固定使用 DeskSend 发布源，不读取旧版保存的上游更新地址。
  QNetworkRequest request = makeUpdateRequest(QUrl(kUrlUpdateCheck), QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setRawHeader("Accept", "application/vnd.github+json");
  if (m_hasCachedRelease && !m_etag.isEmpty()) {
    request.setRawHeader("If-None-Match", m_etag.toUtf8());
  }
  m_activeReply = m_network->get(request);
}

void VersionChecker::startFallbackTagHead()
{
  m_stage = CheckStage::FallbackTagHead;
  const QNetworkRequest request = makeUpdateRequest(QUrl(kUrlDownload), QNetworkRequest::ManualRedirectPolicy);
  m_activeReply = m_network->head(request);
}

void VersionChecker::startFallbackSumsGet()
{
  m_stage = CheckStage::FallbackSumsGet;
  const QUrl sumsUrl = releaseAssetUrl(m_pendingVersion, QStringLiteral("SHA256SUMS.txt"));
  const QNetworkRequest request = makeUpdateRequest(sumsUrl, QNetworkRequest::NoLessSafeRedirectPolicy);
  m_activeReply = m_network->get(request);
}

void VersionChecker::startFallbackAssetHead()
{
  m_stage = CheckStage::FallbackAssetHead;
  const QNetworkRequest request = makeUpdateRequest(m_pendingDownload, QNetworkRequest::NoLessSafeRedirectPolicy);
  m_activeReply = m_network->head(request);
}

void VersionChecker::replyFinished(QNetworkReply *reply)
{
  reply->deleteLater();
  if (reply != m_activeReply)
    return;
  m_activeReply = nullptr;

  const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  switch (m_stage) {
  case CheckStage::PrimaryApi:
    handlePrimaryReply(reply, statusCode);
    break;
  case CheckStage::FallbackTagHead:
    handleFallbackTagReply(reply, statusCode);
    break;
  case CheckStage::FallbackSumsGet:
    handleFallbackSumsReply(reply, statusCode);
    break;
  case CheckStage::FallbackAssetHead:
    handleFallbackAssetReply(reply, statusCode);
    break;
  case CheckStage::None:
    m_checking = false;
    break;
  }
}

void VersionChecker::handlePrimaryReply(QNetworkReply *reply, int statusCode)
{
  if (statusCode == 304 && m_hasCachedRelease) {
    finishSuccess(m_cachedRelease);
    return;
  }
  if (reply->error() != QNetworkReply::NoError) {
    const QString errorMessage = formatHttpFailure(
        statusCode, reply->rawHeader("X-RateLimit-Remaining"), reply->rawHeader("X-RateLimit-Reset"),
        reply->errorString()
    );
    // API 受限时自动回退到 releases/latest 页面重定向与 SHA256SUMS.txt，避免匿名 60 次/小时限流导致无法更新。
    if (shouldUseFallback(statusCode)) {
      m_primaryError = errorMessage;
      startFallbackTagHead();
      return;
    }
    finishFailure(errorMessage);
    return;
  }

  Release parsed;
  QString error;
  if (!parseRelease(reply->readAll(), parsed, error)) {
    finishFailure(error);
    return;
  }
  const QByteArray etag = reply->rawHeader("ETag");
  if (!etag.isEmpty()) {
    m_etag = QString::fromUtf8(etag);
    m_cachedRelease = parsed;
    m_hasCachedRelease = true;
  }
  finishSuccess(parsed);
}

void VersionChecker::handleFallbackTagReply(QNetworkReply *reply, int statusCode)
{
  if (reply->error() != QNetworkReply::NoError) {
    if (statusCode == 404) {
      finishFailure(tr("Invalid release metadata; no stable release found."));
      return;
    }
    finishFailure(
        shouldUseFallback(statusCode) && !m_primaryError.isEmpty()
            ? m_primaryError
            : tr("Unable to check for DeskSend updates: %1").arg(reply->errorString())
    );
    return;
  }
  if (statusCode < 300 || statusCode >= 400) {
    finishFailure(tr("Invalid release metadata; no stable release found."));
    return;
  }

  QUrl location = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
  if (!location.isValid()) {
    const QByteArray rawLocation = reply->rawHeader("Location").trimmed();
    if (!rawLocation.isEmpty())
      location = QUrl::fromEncoded(rawLocation);
  }
  if (!parseLatestTagUrl(location, m_pendingVersion)) {
    finishFailure(tr("Invalid release metadata; no stable release found."));
    return;
  }

  m_pendingDownload = releaseAssetUrl(
      m_pendingVersion, QStringLiteral("DeskSend-%1-windows-x64.zip").arg(m_pendingVersion)
  );
  startFallbackSumsGet();
}

void VersionChecker::handleFallbackSumsReply(QNetworkReply *reply, int statusCode)
{
  if (reply->error() != QNetworkReply::NoError) {
    finishFailure(
        statusCode == 404 ? tr("This release does not provide a complete Windows x64 update package.")
                          : tr("Unable to check for DeskSend updates: %1").arg(reply->errorString())
    );
    return;
  }

  const QByteArray payload = reply->readAll();
  if (payload.size() > 64 * 1024 || !parseSha256Sums(payload, m_pendingVersion, m_pendingSha256)) {
    finishFailure(tr("Invalid update package URL, size, or SHA-256 digest."));
    return;
  }
  startFallbackAssetHead();
}

void VersionChecker::handleFallbackAssetReply(QNetworkReply *reply, int statusCode)
{
  if (reply->error() != QNetworkReply::NoError) {
    finishFailure(
        statusCode == 404 ? tr("This release does not provide a complete Windows x64 update package.")
                          : tr("Unable to check for DeskSend updates: %1").arg(reply->errorString())
    );
    return;
  }
  if (reply->url().scheme() != QLatin1String("https")) {
    finishFailure(tr("Invalid update package URL, size, or SHA-256 digest."));
    return;
  }

  bool ok = false;
  qint64 size = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&ok);
  if (!ok || size <= 0) {
    size = reply->rawHeader("Content-Length").trimmed().toLongLong(&ok);
  }
  if (!ok || size <= 0) {
    finishFailure(tr("Invalid update package URL, size, or SHA-256 digest."));
    return;
  }

  Release resolvedRelease;
  resolvedRelease.version = m_pendingVersion;
  resolvedRelease.download = m_pendingDownload;
  resolvedRelease.sha256 = m_pendingSha256;
  resolvedRelease.size = size;
  m_etag.clear();
  m_hasCachedRelease = false;
  finishSuccess(resolvedRelease);
}

void VersionChecker::finishSuccess(const Release &release)
{
  m_checking = false;
  m_stage = CheckStage::None;
  m_release = release;
  const bool available = isNewer(kVersion, m_release.version);
  if (available)
    Q_EMIT updateFound(m_release.version);
  Q_EMIT checked(available);
}

void VersionChecker::finishFailure(const QString &error)
{
  m_checking = false;
  m_stage = CheckStage::None;
  m_release = {};
  Q_EMIT checkFailed(error);
}
