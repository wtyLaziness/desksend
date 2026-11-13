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

UpdateDialog::UpdateDialog(QWidget *parent) : QDialog(parent), m_checker(this)
{
  setWindowTitle(QStringLiteral("DeskSend 更新"));
  setWindowIcon(QIcon(":/branding/desksend.png"));
  setMinimumWidth(440);
  auto layout = new QVBoxLayout(this);
  layout->addWidget(new QLabel(QStringLiteral("DeskSend %1").arg(kDisplayVersion), this));
  m_status = new QLabel(QStringLiteral("正在检查 DeskSend 正式版…"), this);
  m_status->setWordWrap(true);
  layout->addWidget(m_status);
  m_progress = new QProgressBar(this);
  m_progress->hide();
  layout->addWidget(m_progress);
  auto notice = new QLabel(QStringLiteral("更新包含键鼠共享和文件传输组件。安装时会暂时断开连接；原有配置、证书和配对会保留。"), this);
  notice->setWordWrap(true);
  layout->addWidget(notice);
  m_action = new QPushButton(QStringLiteral("下载更新"), this);
  m_action->setEnabled(false);
  layout->addWidget(m_action);
  m_close = new QPushButton(QStringLiteral("关闭"), this);
  layout->addWidget(m_close);
  connect(m_close, &QPushButton::clicked, this, &QDialog::reject);
  connect(m_action, &QPushButton::clicked, this, [this]() { m_ready ? install() : download(); });
  connect(&m_checker, &VersionChecker::checkFailed, this, &UpdateDialog::fail);
  connect(&m_checker, &VersionChecker::checked, this, [this](bool available) {
    m_status->setText(available ? QStringLiteral("发现 DeskSend %1，完整更新包 %2 MiB。").arg(m_checker.release().version).arg(m_checker.release().size / 1048576.0, 0, 'f', 1)
                              : QStringLiteral("当前已是最新的 DeskSend 正式版。"));
    m_action->setEnabled(available);
  });
  m_checker.checkLatest();
}
UpdateDialog::~UpdateDialog()
{
  if (m_reply) {
    disconnect(m_reply, nullptr, this, nullptr);
    m_reply->abort();
  }
  if (m_file && m_file->isOpen()) {
    m_file->cancelWriting();
  }
  m_file.reset();
  if (!m_launchedInstaller && !m_directory.isEmpty()) {
    QDir(m_directory).removeRecursively();
  }
}
void UpdateDialog::fail(const QString &message)
{
  m_status->setText(message);
  m_action->setEnabled(false);
  m_close->setEnabled(true);
}
void UpdateDialog::download()
{
  m_directory = QDir::tempPath() + "/DeskSend-update-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (!QDir().mkpath(m_directory)) {
    fail(QStringLiteral("无法创建更新临时目录。"));
    return;
  }
  m_archive = m_directory + "/update.zip";
  m_file = std::make_unique<QSaveFile>(m_archive);
  if (!m_file->open(QIODevice::WriteOnly)) {
    fail(QStringLiteral("无法写入更新包：%1").arg(m_file->errorString()));
    return;
  }
  m_action->setEnabled(false);
  m_progress->show();
  m_status->setText(QStringLiteral("正在下载更新…"));
  auto network = new QNetworkAccessManager(this);
  QNetworkRequest request(m_checker.release().download);
  request.setTransferTimeout(60000);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  m_reply = network->get(request);
  connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
    const auto bytes = m_reply->readAll();
    m_received += bytes.size();
    if (m_received > m_checker.release().size || m_file->write(bytes) != bytes.size()) {
      m_writeError = QStringLiteral("更新包大小异常或磁盘写入失败。");
      m_reply->abort();
      return;
    }
    m_hash.addData(bytes);
    m_progress->setValue(int(100.0 * m_received / m_checker.release().size));
    m_status->setText(QStringLiteral("已下载 %1 / %2 MiB").arg(m_received / 1048576.0, 0, 'f', 1).arg(m_checker.release().size / 1048576.0, 0, 'f', 1));
  });
  connect(m_reply, &QNetworkReply::finished, this, [this]() {
    auto reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError || !m_writeError.isEmpty()) {
      m_file->cancelWriting();
      fail(m_writeError.isEmpty() ? QStringLiteral("下载失败：%1").arg(reply->errorString()) : m_writeError);
      return;
    }
    if (m_received != m_checker.release().size || m_hash.result().toHex() != m_checker.release().sha256) {
      m_file->cancelWriting();
      fail(QStringLiteral("更新包 SHA-256 校验失败，请关闭后重试。"));
      return;
    }
    if (!m_file->commit()) {
      fail(QStringLiteral("更新包保存失败：%1").arg(m_file->errorString()));
      return;
    }
    m_ready = true;
    m_status->setText(QStringLiteral("更新包已下载并通过 SHA-256 校验，可以安装。"));
    m_action->setText(QStringLiteral("安装并重启"));
    m_action->setEnabled(true);
  });
}
void UpdateDialog::install()
{
#ifdef Q_OS_WIN
  if (!Settings::isPortableMode() || Settings::value(Settings::Core::ProcessMode).value<Settings::ProcessMode>() != Settings::ProcessMode::Desktop) {
    fail(QStringLiteral("内置更新适用于便携版。请从 DeskSend 发布页下载完整包。"));
    return;
  }
  QTemporaryFile probe(QCoreApplication::applicationDirPath() + "/.desksend-write-XXXXXX");
  if (!probe.open()) {
    fail(QStringLiteral("程序目录不可写。请将完整包解压到可写目录后运行。"));
    return;
  }
  const auto helper = m_directory + "/Apply-DeskSendUpdate.ps1";
  if (!QFile::copy(":/updates/Apply-DeskSendUpdate.ps1", helper)) {
    fail(QStringLiteral("无法准备更新程序。"));
    return;
  }
  const auto powershell = qEnvironmentVariable("SystemRoot") + "/System32/WindowsPowerShell/v1.0/powershell.exe";
  QStringList arguments{"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden", "-File", helper,
                        "-InstallDir", QCoreApplication::applicationDirPath(), "-Archive", m_archive,
                        "-ExpectedHash", QString::fromLatin1(m_checker.release().sha256),
                        "-GuiProcessId", QString::number(QCoreApplication::applicationPid())};
  if (!QProcess::startDetached(powershell, arguments, m_directory)) {
    fail(QStringLiteral("无法启动更新程序，当前版本仍可继续使用。"));
    return;
  }
  m_launchedInstaller = true;
  Q_EMIT installRequested();
  accept();
#else
  fail(QStringLiteral("此更新包仅适用于 Windows x64。"));
#endif
}
