// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "gui/VersionChecker.h"
#include <QDialog>
#include <QCryptographicHash>
#include <QSaveFile>
#include <functional>
#include <memory>
class QLabel;
class QNetworkAccessManager;
class QProgressBar;
class QPushButton;
class QNetworkReply;
class UpdateDialog : public QDialog
{
  Q_OBJECT
public:
  explicit UpdateDialog(QWidget *parent = nullptr);
  ~UpdateDialog() override;
  void setConfirmInstall(std::function<bool()> confirm);
Q_SIGNALS:
  void installRequested();
public Q_SLOTS:
  void reject() override;
private:
  void abortDownloadAndCleanup();
  void cleanupTempDirectory();
  void download();
  void install();
  void fail(const QString &message);
  VersionChecker m_checker;
  QNetworkAccessManager *m_network;
  QLabel *m_status;
  QProgressBar *m_progress;
  QPushButton *m_action;
  QPushButton *m_close;
  QNetworkReply *m_reply = nullptr;
  std::function<bool()> m_confirmInstall;
  std::unique_ptr<QSaveFile> m_file;
  QCryptographicHash m_hash{QCryptographicHash::Sha256};
  QString m_directory;
  QString m_archive;
  QString m_writeError;
  qint64 m_received = 0;
  bool m_ready = false;
  bool m_launchedInstaller = false;
};
