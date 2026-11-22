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
  enum class PackageKind { PortableZip, MsiInstaller };
  struct Release {
    QString version;
    QUrl download;
    QByteArray sha256;
    qint64 size = 0;
  };
  explicit VersionChecker(QObject *parent = nullptr);
  void checkLatest();
  PackageKind packageKind() const { return m_packageKind; }
  void setPackageKind(PackageKind kind);
  static PackageKind currentPackageKind();
  static QString expectedAssetName(const QString &version, PackageKind kind = PackageKind::PortableZip);
  static bool parseRelease(
      const QByteArray &json, Release &release, QString &error, PackageKind kind = PackageKind::PortableZip
  );
  static bool isNewer(const QString &current, const QString &candidate);
  static QString formatHttpFailure(
      int statusCode, const QByteArray &rateLimitRemaining, const QByteArray &rateLimitResetEpoch,
      const QString &networkError
  );
  static bool parseLatestTagUrl(const QUrl &location, QString &version);
  static bool parseSha256Sums(
      const QByteArray &text, const QString &version, QByteArray &sha256, PackageKind kind = PackageKind::PortableZip
  );
  static QUrl releaseAssetUrl(const QString &version, const QString &fileName);
  static bool shouldUseFallback(int statusCode);
  const Release &release() const { return m_release; }
Q_SIGNALS:
  void updateFound(const QString &version);
  void checked(bool available);
  void checkFailed(const QString &error);
private:
  friend class VersionCheckerTests;
  enum class CheckStage { None, PrimaryApi, FallbackTagHead, FallbackSumsGet, FallbackAssetHead };

  void startMetadataReply(QNetworkReply *reply, qsizetype limit);
  bool collectMetadata(QNetworkReply *reply);
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
  QByteArray m_metadata;
  qsizetype m_metadataLimit = 0;
  bool m_metadataTooLarge = false;
  Release m_release;
  Release m_cachedRelease;
  QString m_etag;
  CheckStage m_stage = CheckStage::None;
  QString m_pendingVersion;
  QUrl m_pendingDownload;
  QByteArray m_pendingSha256;
  QString m_primaryError;
  PackageKind m_packageKind = PackageKind::PortableZip;
  bool m_hasCachedRelease = false;
  bool m_checking = false;
};
