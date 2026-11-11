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
  const Release &release() const { return m_release; }
Q_SIGNALS:
  void updateFound(const QString &version);
  void checked(bool available);
  void checkFailed(const QString &error);
private:
  void replyFinished(QNetworkReply *reply);
  QNetworkAccessManager *m_network;
  Release m_release;
  bool m_checking = false;
};
