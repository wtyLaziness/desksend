// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "UpdateDialog.h"
#include "common/Settings.h"
#include "common/VersionInfo.h"
#include <QCoreApplication>
#include <QDir>
#include <QLabel>
#include <QIcon>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryFile>
#include <QUuid>
#include <QVBoxLayout>

UpdateDialog::UpdateDialog(QWidget *parent)
    : QDialog(parent), m_checker(this), m_network(new QNetworkAccessManager(this))
{
  setWindowTitle(tr("DeskSend Update"));
  setWindowIcon(QIcon(":/branding/desksend.png"));
  setMinimumWidth(440);
  auto layout = new QVBoxLayout(this);
  layout->addWidget(new QLabel(QStringLiteral("DeskSend %1").arg(kDisplayVersion), this));
  m_status = new QLabel(tr("Checking for official DeskSend releases..."), this);
  m_status->setWordWrap(true);
  layout->addWidget(m_status);
  m_progress = new QProgressBar(this);
  m_progress->hide();
  layout->addWidget(m_progress);
  auto notice = new QLabel(
      tr("Updates include keyboard/mouse sharing and file transfer components. Connections will pause briefly during installation; settings, certificates, and pairings are preserved."),
      this
  );
  notice->setWordWrap(true);
  layout->addWidget(notice);
  m_action = new QPushButton(tr("Download update"), this);
  m_action->setEnabled(false);
  layout->addWidget(m_action);
  m_close = new QPushButton(tr("Close"), this);
  layout->addWidget(m_close);
  connect(m_close, &QPushButton::clicked, this, &UpdateDialog::reject);
  connect(m_action, &QPushButton::clicked, this, [this]() { m_ready ? install() : download(); });
  connect(&m_checker, &VersionChecker::checkFailed, this, &UpdateDialog::fail);
  connect(&m_checker, &VersionChecker::checked, this, [this](bool available) {
    m_status->setText(
        available ? tr("DeskSend %1 is available (full package %2 MiB).")
                        .arg(m_checker.release().version)
                        .arg(m_checker.release().size / 1048576.0, 0, 'f', 1)
                  : tr("You are running the latest official DeskSend release.")
    );
    m_action->setEnabled(available);
  });
  m_checker.checkLatest();
}
UpdateDialog::~UpdateDialog()
{
  abortDownloadAndCleanup();
}
void UpdateDialog::setConfirmInstall(std::function<bool()> confirm)
{
  m_confirmInstall = std::move(confirm);
}
void UpdateDialog::reject()
{
  abortDownloadAndCleanup();
  QDialog::reject();
}
void UpdateDialog::abortDownloadAndCleanup()
{
  if (m_reply) {
    disconnect(m_reply, nullptr, this, nullptr);
    m_reply->abort();
    m_reply->deleteLater();
    m_reply = nullptr;
  }
  if (m_file && m_file->isOpen()) {
    m_file->cancelWriting();
  }
  m_file.reset();
  if (!m_launchedInstaller) {
    cleanupTempDirectory();
  }
}
void UpdateDialog::cleanupTempDirectory()
{
  if (!m_directory.isEmpty()) {
    QDir(m_directory).removeRecursively();
    m_directory.clear();
  }
  m_archive.clear();
}
void UpdateDialog::fail(const QString &message)
{
  m_status->setText(message);
  if (!m_ready && !m_launchedInstaller) {
    cleanupTempDirectory();
  }
  const bool canRetryDownload =
      !m_ready && m_checker.release().download.isValid() && VersionChecker::isNewer(kVersion, m_checker.release().version);
  if (canRetryDownload) {
    m_action->setText(tr("Retry download"));
    m_action->setEnabled(true);
  } else {
    m_action->setEnabled(false);
  }
  m_close->setEnabled(true);
}
void UpdateDialog::download()
{
  abortDownloadAndCleanup();
  m_received = 0;
  m_writeError.clear();
  m_hash.reset();
  m_ready = false;

  m_directory = QDir::tempPath() + "/DeskSend-update-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (!QDir().mkpath(m_directory)) {
    m_directory.clear();
    fail(tr("Unable to create temporary update directory."));
    return;
  }
  m_archive = m_directory + "/update.zip";
  m_file = std::make_unique<QSaveFile>(m_archive);
  if (!m_file->open(QIODevice::WriteOnly)) {
    const QString openErr = m_file->errorString();
    fail(tr("Unable to write update package: %1").arg(openErr));
    return;
  }
  m_action->setEnabled(false);
  m_progress->setValue(0);
  m_progress->show();
  m_status->setText(tr("Downloading update..."));
  QNetworkRequest request(m_checker.release().download);
  request.setTransferTimeout(60000);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("DeskSend/%1").arg(kVersion));
  m_reply = m_network->get(request);
  connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
    if (!m_reply || !m_writeError.isEmpty())
      return;
    const auto bytes = m_reply->readAll();
    m_received += bytes.size();
    if (m_received > m_checker.release().size || m_file->write(bytes) != bytes.size()) {
      m_writeError = tr("Unexpected update package size or disk write failure.");
      m_reply->abort();
      return;
    }
    m_hash.addData(bytes);
    m_progress->setValue(int(100.0 * m_received / m_checker.release().size));
    m_status->setText(tr("Downloaded %1 / %2 MiB")
                          .arg(m_received / 1048576.0, 0, 'f', 1)
                          .arg(m_checker.release().size / 1048576.0, 0, 'f', 1));
  });
  connect(m_reply, &QNetworkReply::finished, this, [this]() {
    auto reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || (statusCode >= 400) || !m_writeError.isEmpty()) {
      if (m_file && m_file->isOpen()) {
        m_file->cancelWriting();
      }
      m_file.reset();
      fail(m_writeError.isEmpty() ? tr("Download failed: %1").arg(reply->errorString()) : m_writeError);
      return;
    }
    if (m_received != m_checker.release().size || m_hash.result().toHex() != m_checker.release().sha256) {
      if (m_file && m_file->isOpen()) {
        m_file->cancelWriting();
      }
      m_file.reset();
      fail(tr("Update package SHA-256 verification failed. Please retry."));
      return;
    }
    if (!m_file->commit()) {
      const QString saveErr = m_file->errorString();
      m_file.reset();
      fail(tr("Failed to save update package: %1").arg(saveErr));
      return;
    }
    m_file.reset();
    m_ready = true;
    m_status->setText(tr("Update package downloaded and verified (SHA-256). Ready to install."));
    m_action->setText(tr("Install and restart"));
    m_action->setEnabled(true);
  });
}
void UpdateDialog::install()
{
#ifdef Q_OS_WIN
  if (m_confirmInstall && !m_confirmInstall()) {
    return;
  }
  if (!Settings::isPortableMode() || Settings::value(Settings::Core::ProcessMode).value<Settings::ProcessMode>() != Settings::ProcessMode::Desktop) {
    fail(tr("Built-in updates apply to the portable package. Please download the full archive from the DeskSend releases page."));
    return;
  }
  QTemporaryFile probe(QCoreApplication::applicationDirPath() + "/.desksend-write-XXXXXX");
  if (!probe.open()) {
    fail(tr("The application directory is not writable. Please extract the full package to a writable folder."));
    return;
  }
  const auto helper = m_directory + "/Apply-DeskSendUpdate.ps1";
  if (!QFile::copy(":/updates/Apply-DeskSendUpdate.ps1", helper)) {
    fail(tr("Unable to prepare updater script."));
    return;
  }
  const auto powershell = qEnvironmentVariable("SystemRoot") + "/System32/WindowsPowerShell/v1.0/powershell.exe";
  QStringList arguments{"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden", "-File", helper,
                        "-InstallDir", QCoreApplication::applicationDirPath(), "-Archive", m_archive,
                        "-ExpectedHash", QString::fromLatin1(m_checker.release().sha256),
                        "-GuiProcessId", QString::number(QCoreApplication::applicationPid())};
  if (!QProcess::startDetached(powershell, arguments, m_directory)) {
    fail(tr("Unable to start updater; you can continue using the current version."));
    return;
  }
  m_launchedInstaller = true;
  Q_EMIT installRequested();
  accept();
#else
  fail(tr("This update package is only available for Windows x64."));
#endif
}
