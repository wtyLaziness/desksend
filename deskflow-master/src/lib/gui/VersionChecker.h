// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
// SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
#pragma once
#include <QObject>
#include <QUrl>
class QNetworkAccessManager;
class QNetworkReply;
class VersionChecker : public QObject
{
  Q_OBJECT
public:
  struct Release {
    QString version;
    QUrl download;
    QByteArray sha256;
    qint64 size = 0;
  };
  explicit VersionChecker(QObject *parent = nullptr);
  void checkLatest();
  static bool parseRelease(const QByteArray &json, Release &release, QString &error);
  static bool isNewer(const QString &current, const QString &candidate);
  static QString formatHttpFailure(
      int statusCode, const QByteArray &rateLimitRemaining, const QByteArray &rateLimitResetEpoch,
      const QString &networkError
  );
  static bool parseLatestTagUrl(const QUrl &location, QString &version);
  static bool parseSha256Sums(const QByteArray &text, const QString &version, QByteArray &sha256);
  static QUrl releaseAssetUrl(const QString &version, const QString &fileName);
  static bool shouldUseFallback(int statusCode);
  const Release &release() const { return m_release; }
Q_SIGNALS:
  void updateFound(const QString &version);
  void checked(bool available);
  void checkFailed(const QString &error);
private:
  enum class CheckStage { None, PrimaryApi, FallbackTagHead, FallbackSumsGet, FallbackAssetHead };

  void replyFinished(QNetworkReply *reply);
  void startPrimaryApi();
  void startFallbackTagHead();
  void startFallbackSumsGet();
  void startFallbackAssetHead();
  void handlePrimaryReply(QNetworkReply *reply, int statusCode);
  void handleFallbackTagReply(QNetworkReply *reply, int statusCode);
  void handleFallbackSumsReply(QNetworkReply *reply, int statusCode);
  void handleFallbackAssetReply(QNetworkReply *reply, int statusCode);
  void finishSuccess(const Release &release);
  void finishFailure(const QString &error);

  QNetworkAccessManager *m_network;
  QNetworkReply *m_activeReply = nullptr;
  Release m_release;
  Release m_cachedRelease;
  QString m_etag;
  CheckStage m_stage = CheckStage::None;
  QString m_pendingVersion;
  QUrl m_pendingDownload;
  QByteArray m_pendingSha256;
  QString m_primaryError;
  bool m_hasCachedRelease = false;
  bool m_checking = false;
};
