/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "IpcClient.h"

#include "common/VersionInfo.h"

#include <QDebug>
#include <QLocalSocket>
#include <QTimer>
#include <utility>

namespace deskflow::gui::ipc {

constexpr qsizetype kMaxIpcLine = 128 * 1024;

IpcClient::IpcClient(QObject *parent, const QString &socketName, const QString &typeName)
    : QObject(parent),
      m_socket{new QLocalSocket(this)},
      m_socketName(socketName), // NOSONAR - Qt memory
      m_typeName(typeName)
{
  m_socket->setReadBufferSize(kMaxIpcLine);
  m_retryTimer.setSingleShot(true);
  connect(&m_retryTimer, &QTimer::timeout, this, &IpcClient::attemptConnection);
  m_helloTimer.setSingleShot(true);
  connect(&m_helloTimer, &QTimer::timeout, this, [this] {
    disconnectFromServer();
    Q_EMIT connectionFailed();
  });
  connect(m_socket, &QLocalSocket::disconnected, this, &IpcClient::handleDisconnected);
  connect(m_socket, &QLocalSocket::errorOccurred, this, &IpcClient::handleErrorOccurred);
  connect(m_socket, &QLocalSocket::readyRead, this, &IpcClient::handleReadyRead);
}

void IpcClient::connectToServer()
{
  if (m_state == State::Connecting) {
    qWarning().noquote() << QStringLiteral("%1 ipc client already connecting to server").arg(m_typeName);
    return;
  }

  if (m_state != State::Unconnected) {
    qDebug().noquote() << QStringLiteral("%1 ipc client not in unconnected state, disconnecting").arg(m_typeName);
    disconnectFromServer();
  }

  if (m_socket->state() != QLocalSocket::UnconnectedState) {
    qWarning().noquote(
    ) << QStringLiteral("%1 ipc client socket not in unconnected state, disconnecting").arg(m_typeName);
    disconnectFromServer();
  }

  m_retryTimer.stop();
  m_readBuffer.clear();
  m_versionRejected = false;
  m_retryCount = 0;
  attemptConnection();
}

void IpcClient::attemptConnection()
{
  if (const int retryLimit = 10; m_retryCount >= retryLimit) {
    qWarning().noquote() << QStringLiteral("%1 ipc client failed to connect after %2 attempts")
                                .arg(m_typeName, QString::number(retryLimit));
    m_state = State::Unconnected;
    Q_EMIT connectionFailed();
    return;
  }

  if (m_retryCount == 0) {
    qDebug().noquote() << QStringLiteral("%1 ipc client connecting to server: %2").arg(m_typeName, m_socketName);
  } else {
    qDebug().noquote() << QStringLiteral("%1 ipc client retrying connection, attempt: %2")
                              .arg(m_typeName, QString::number(m_retryCount + 1));
  }

  m_state = State::Connecting;
  m_retryCount++;

  connect(
      m_socket, &QLocalSocket::connected, this, &IpcClient::handleConnectionSuccess,
      static_cast<Qt::ConnectionType>(Qt::SingleShotConnection | Qt::UniqueConnection)
  );

  connect(
      m_socket, &QLocalSocket::errorOccurred, this, &IpcClient::handleConnectionFailure,
      static_cast<Qt::ConnectionType>(Qt::SingleShotConnection | Qt::UniqueConnection)
  );

  m_socket->connectToServer(m_socketName);
}

void IpcClient::disconnectFromServer()
{
  disconnect(m_socket, &QLocalSocket::connected, this, &IpcClient::handleConnectionSuccess);
  disconnect(m_socket, &QLocalSocket::errorOccurred, this, &IpcClient::handleConnectionFailure);
  m_retryTimer.stop();
  m_helloTimer.stop();
  m_readBuffer.clear();
  m_versionRejected = false;
  m_stoppingLegacyCore = false;
  m_state = State::Disconnecting;
  qDebug().noquote() << QStringLiteral("%1 ipc client disconnecting from server").arg(m_typeName);
  m_socket->abort();
  m_state = State::Unconnected;
}

void IpcClient::flush(int msecs)
{
  if (m_socket->state() == QLocalSocket::ConnectedState) {
    m_socket->flush();
    if (m_socket->bytesToWrite() > 0) {
      m_socket->waitForBytesWritten(msecs);
    }
  }
}

void IpcClient::handleDisconnected()
{
  m_readBuffer.clear();
  m_helloTimer.stop();
  if (m_state == State::Connecting) {
    m_state = State::Unconnected;
    Q_EMIT connectionFailed();
    return;
  }

  qDebug().noquote() << QStringLiteral("%1 ipc client disconnected from server").arg(m_typeName);
  const auto wasConnected = m_state == State::Connected;
  m_state = State::Unconnected;

  if (wasConnected) {
    Q_EMIT connectionFailed();
  }
}

void IpcClient::handleErrorOccurred()
{
  if (m_state == State::Connecting) {
    return;
  }

  qWarning().noquote() << QStringLiteral("%1 ipc client error: %2").arg(m_typeName, m_socket->errorString());

  if (m_state == State::Connected) {
    disconnectFromServer();
    Q_EMIT connectionFailed();
  }
}

void IpcClient::handleReadyRead()
{
  QByteArray data = m_readBuffer + m_socket->readAll();
  m_readBuffer.clear();

  while (data.contains('\n')) {
    const auto index = data.indexOf('\n');
    if (index >= kMaxIpcLine) {
      disconnectFromServer();
      Q_EMIT connectionFailed();
      return;
    }
    const auto message = QString::fromUtf8(data.left(index));
    data.remove(0, index + 1);

    qDebug().noquote() << QStringLiteral("%1 ipc client message: %2").arg(m_typeName, message);
    if (message.isEmpty()) {
      qWarning().noquote() << QStringLiteral("%1 ipc client got invalid message: %2").arg(m_typeName, message);
      continue;
    }
    const auto separator = message.indexOf('=');
    const auto parts = separator < 0 ? QStringList{message}
                                    : QStringList{message.left(separator), message.mid(separator + 1)};

    if (m_state == State::Connecting && !(m_stoppingLegacyCore && parts.at(0) == QStringLiteral("bye"))) {
      handleHandshakeMessage(parts);
      if (m_state == State::Unconnected)
        return;
      continue;
    }

    if (parts.at(0) == QStringLiteral("bye")) {
      qDebug().noquote() << QStringLiteral("%1 ipc server is shutting down").arg(m_typeName);
      disconnectFromServer();
      Q_EMIT serverShutdown();
      return;
    }

    processCommand(parts.at(0), parts);
  }

  if (data.size() >= kMaxIpcLine) {
    disconnectFromServer();
    Q_EMIT connectionFailed();
    return;
  }
  m_readBuffer = std::move(data);
}

void IpcClient::handleHandshakeMessage(const QStringList &parts)
{
  if (parts.at(0) == QStringLiteral("error")) {
    const auto detail = parts.size() >= 2 ? parts.at(1) : QStringLiteral("unknown");
    qCritical().noquote() << QStringLiteral("%1 ipc server rejected connection: %2").arg(m_typeName, detail);
    disconnectFromServer();
    Q_EMIT connectionFailed();
    return;
  }

  const auto versionId = QStringLiteral("%1+%2").arg(kVersion, kVersionGitSha);

  if (parts.at(0) == QStringLiteral("versionMismatch")) {
    const auto serverVersion = parts.size() >= 2 ? parts.at(1) : QStringLiteral("unknown");
    qWarning().noquote(
    ) << QStringLiteral("%1 ipc version mismatch (client: %2, server: %3)").arg(m_typeName, versionId, serverVersion);
    m_versionRejected = parts.size() == 2 && !serverVersion.isEmpty() && serverVersion != versionId;
    Q_EMIT versionMismatch();
    if (m_stoppingLegacyCore)
      return;
    disconnectFromServer();
    Q_EMIT connectionFailed();
    return;
  }

  if (parts.at(0) != QStringLiteral("hello")) {
    return;
  }

  if (parts.size() != 2 || parts.at(1) != versionId) {
    qCritical().noquote() << QStringLiteral("%1 ipc server hello has an invalid version").arg(m_typeName);
    disconnectFromServer();
    Q_EMIT connectionFailed();
    return;
  }

  m_helloTimer.stop();
  m_state = State::Connected;
  qDebug().noquote() << QStringLiteral("%1 ipc client connected").arg(m_typeName);
  Q_EMIT connected();
}

void IpcClient::handleConnectionFailure()
{
  qWarning().noquote(
  ) << QStringLiteral("%1 ipc client failed to connect: %2").arg(m_typeName, m_socket->errorString());
  m_state = State::Disconnecting;
  m_socket->abort();
  m_readBuffer.clear();
  m_state = State::Unconnected;
  m_retryTimer.start(500);
}

void IpcClient::handleConnectionSuccess()
{
  const auto versionId = QStringLiteral("%1+%2").arg(kVersion, kVersionGitSha);
  disconnect(m_socket, &QLocalSocket::connected, this, &IpcClient::handleConnectionSuccess);
  disconnect(m_socket, &QLocalSocket::errorOccurred, this, &IpcClient::handleConnectionFailure);
  m_helloTimer.start(5000);
  m_socket->write(QStringLiteral("hello=%1\n").arg(versionId).toUtf8());
  qDebug().noquote() << QStringLiteral("%1 ipc client sent hello with version: %2").arg(m_typeName, versionId);
}

bool IpcClient::stopIncompatibleCore()
{
  if (!m_versionRejected || m_typeName != QStringLiteral("core") || m_state != State::Connecting ||
      m_socket->state() != QLocalSocket::ConnectedState)
    return false;
  m_versionRejected = false;
  m_stoppingLegacyCore = true;
  m_socket->write("stop\n");
  m_socket->flush();
  return true;
}

void IpcClient::sendMessage(const QString &message)
{
  if (m_state != State::Connected) {
    qWarning().noquote() << QStringLiteral("%1 cannot send command, ipc client not connected").arg(m_typeName);
    return;
  }

  m_socket->write(message.toUtf8() + "\n");
  qDebug().noquote() << QStringLiteral("%1 ipc client sent message: %2").arg(m_typeName, message);
}

} // namespace deskflow::gui::ipc
